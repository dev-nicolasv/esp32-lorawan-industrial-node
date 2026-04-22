/**
 * @file main.cpp
 * @brief Industrial ESP32 telemetry node using ADS1115 (4-20 mA) and LoRa-E5 LoRaWAN modem.
 *
 * @details
 * Hardware profile:
 * - MCU: ESP32 (Arduino framework, PlatformIO)
 * - Sensor input: 4-20 mA loop converted to voltage across a precision shunt resistor
 * - ADC: ADS1115 over I2C for 16-bit conversion stability
 * - LoRaWAN modem: Seeed LoRa-E5 over UART AT interface (internal LoRaWAN stack)
 *
 * Power profile:
 * - Wakes from deep sleep every 15 minutes
 * - Performs one sensor acquisition + one uplink
 * - Returns to deep sleep immediately after TX completion event
 * - No blocking delay() calls are used; control flow is fully state-machine driven
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <esp_sleep.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#ifndef LORAE5_REGION
#define LORAE5_REGION "US915"
#endif

#ifndef LORAE5_APP_EUI
#define LORAE5_APP_EUI "0000000000000000"
#endif

#ifndef LORAE5_DEV_EUI
#define LORAE5_DEV_EUI "0000000000000000"
#endif

#ifndef LORAE5_APP_KEY
#define LORAE5_APP_KEY "00000000000000000000000000000000"
#endif

#ifndef LORAE5_UPLINK_PORT
#define LORAE5_UPLINK_PORT 10
#endif

namespace config {

/** @brief ESP32 I2C pins for ADS1115. */
constexpr uint8_t kI2cSdaPin = 21;
constexpr uint8_t kI2cSclPin = 22;

/** @brief ADS1115 address and channel configuration. */
constexpr uint8_t kAds1115Address = 0x48;
constexpr uint16_t kAds1115Mux = ADS1X15_REG_CONFIG_MUX_SINGLE_0;

/**
 * @brief 4-20 mA shunt value.
 * @note 165R maps 4-20 mA to approximately 0.66-3.30 V.
 */
constexpr float kShuntOhms = 165.0F;

/** @brief Physical conversion range from current loop to process magnitude. */
constexpr float kSensorCurrentMinMa = 4.0F;
constexpr float kSensorCurrentMaxMa = 20.0F;
constexpr float kProcessMinPsi = 0.0F;
constexpr float kProcessMaxPsi = 100.0F;

/** @brief LoRa-E5 UART pins on ESP32 (RX <- TX, TX -> RX). */
constexpr int8_t kLoRaRxPin = 16;
constexpr int8_t kLoRaTxPin = 17;

/** @brief Serial interfaces speed. */
constexpr uint32_t kDebugBaudrate = 115200;
constexpr uint32_t kLoRaBaudrate = 9600;

/** @brief Uplink periodicity: 15 minutes in deep sleep wakeup timer units. */
constexpr uint64_t kWakeupIntervalUs = 15ULL * 60ULL * 1000000ULL;

/** @brief State-machine timing and retry policy. */
constexpr uint32_t kAtTimeoutMs = 3000;
constexpr uint32_t kJoinTimeoutMs = 30000;
constexpr uint32_t kUplinkTimeoutMs = 20000;
constexpr uint32_t kAdcTimeoutMs = 200;
constexpr uint32_t kRetryBackoffMs = 2500;
constexpr uint8_t kMaxJoinAttempts = 3;
constexpr uint8_t kMaxUplinkAttempts = 3;

/** @brief LoRaWAN region/scheme and OTAA credentials provisioned to LoRa-E5. */
constexpr const char* kRegion = LORAE5_REGION;
constexpr const char* kAppEui = LORAE5_APP_EUI;
constexpr const char* kDevEui = LORAE5_DEV_EUI;
constexpr const char* kAppKey = LORAE5_APP_KEY;
constexpr uint8_t kUplinkPort = static_cast<uint8_t>(LORAE5_UPLINK_PORT);
/** @note Credentials are expected to be injected at build time in production pipelines. */

}  // namespace config

/** @brief Main firmware states for deterministic low-power execution. */
enum class NodeState : uint8_t {
  Startup,
  ModemPing,
  ModemSetMode,
  ModemSetRegion,
  ModemSetAdr,
  ModemSetPort,
  ModemSetAppEui,
  ModemSetDevEui,
  ModemSetAppKey,
  ModemJoin,
  JoinRetryWait,
  SensorStartConversion,
  SensorWaitConversion,
  SendUplink,
  UplinkRetryWait,
  EnterSleep,
  Fault,
};

/** @brief Single telemetry sample captured each wake cycle. */
struct SensorSample final {
  int16_t rawCounts {0};
  float loopCurrentMa {0.0F};
  float pressurePsi {0.0F};
};

/** @brief Lightweight parser context for one in-flight AT command. */
struct PendingAtCommand final {
  bool active {false};
  uint32_t deadlineMs {0};
  NodeState onSuccess {NodeState::Fault};
  NodeState onFailure {NodeState::Fault};
  std::array<const char*, 5> successTokens {};
  std::array<const char*, 5> failureTokens {};
  uint8_t successCount {0};
  uint8_t failureCount {0};
};

class IndustrialNode final {
 public:
  explicit IndustrialNode(HardwareSerial& modemSerial)
      : modemSerial_(modemSerial) {}

  /** @brief Configure peripherals and bootstrap the state machine. */
  void begin() {
    Serial.begin(config::kDebugBaudrate);
    Wire.begin(config::kI2cSdaPin, config::kI2cSclPin);

    modemSerial_.begin(
        config::kLoRaBaudrate,
        SERIAL_8N1,
        config::kLoRaRxPin,
        config::kLoRaTxPin);

    ads_.setGain(GAIN_ONE);
    ads_.setDataRate(RATE_ADS1115_128SPS);

    if (!ads_.begin(config::kAds1115Address, &Wire)) {
      Serial.println("[BOOT] ADS1115 init failed");
      transitionTo(NodeState::Fault);
      return;
    }

    printWakeupReason();
    transitionTo(NodeState::ModemPing);
  }

  /** @brief Non-blocking scheduler entry point, called in Arduino loop(). */
  void update() {
    consumeModemInput();
    processPendingCommandTimeout();

    const bool enteredState = (state_ != previousState_);
    if (enteredState) {
      previousState_ = state_;
      stateEntryMs_ = millis();
    }

    processState(enteredState);
  }

 private:
  static constexpr size_t kLineBufferSize = 160;
  static constexpr size_t kPayloadSize = 4;

  HardwareSerial& modemSerial_;
  Adafruit_ADS1115 ads_;

  NodeState state_ {NodeState::Startup};
  NodeState previousState_ {NodeState::Fault};

  PendingAtCommand pending_ {};

  std::array<char, kLineBufferSize> lineBuffer_ {};
  size_t lineLength_ {0};

  SensorSample sample_ {};
  std::array<uint8_t, kPayloadSize> payload_ {};

  uint8_t joinAttempts_ {0};
  uint8_t uplinkAttempts_ {0};
  bool requireJoinBeforeUplinkRetry_ {false};

  uint32_t stateEntryMs_ {0};
  uint32_t adcStartMs_ {0};

  /** @brief millis() safe timeout comparator. */
  static bool isExpired(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
  }

  /** @brief Simple substring token matching helper. */
  static bool containsToken(const char* text, const char* token) {
    return (token != nullptr) && (std::strstr(text, token) != nullptr);
  }

  /** @brief Clamp helper compatible with older standard library variants. */
  template <typename T>
  static T clampValue(T value, T low, T high) {
    if (value < low) {
      return low;
    }
    if (value > high) {
      return high;
    }
    return value;
  }

  /** @brief Move state machine to next state. */
  void transitionTo(NodeState next) {
    if (state_ == next) {
      return;
    }
    state_ = next;
  }

  /** @brief Reset AT command tracking context. */
  void clearPendingCommand() {
    pending_ = PendingAtCommand {};
  }

  /**
   * @brief Send one AT command and register expected success/failure tokens.
   * @param command AT command without trailing newline.
   * @param timeoutMs Command timeout in milliseconds.
   * @param onSuccess State transition when a success token is observed.
   * @param onFailure State transition when timeout/failure token is observed.
   */
  void startCommand(
      const char* command,
      uint32_t timeoutMs,
      NodeState onSuccess,
      NodeState onFailure,
      std::initializer_list<const char*> successTokens,
      std::initializer_list<const char*> failureTokens = {}) {
    clearPendingCommand();

    modemSerial_.print(command);
    modemSerial_.print('\n');

    pending_.active = true;
    pending_.deadlineMs = millis() + timeoutMs;
    pending_.onSuccess = onSuccess;
    pending_.onFailure = onFailure;

    uint8_t index = 0;
    for (const char* token : successTokens) {
      if (index >= pending_.successTokens.size()) {
        break;
      }
      pending_.successTokens[index++] = token;
    }
    pending_.successCount = index;

    index = 0;
    for (const char* token : failureTokens) {
      if (index >= pending_.failureTokens.size()) {
        break;
      }
      pending_.failureTokens[index++] = token;
    }
    pending_.failureCount = index;
  }

  /** @brief Handle AT success/failure transitions. */
  void resolvePendingCommand(bool success) {
    if (!pending_.active) {
      return;
    }

    const NodeState stateWhenResolved = state_;
    const NodeState nextState = success ? pending_.onSuccess : pending_.onFailure;
    clearPendingCommand();

    if (success && stateWhenResolved == NodeState::ModemJoin) {
      joinAttempts_ = 0;
    }

    if (success && stateWhenResolved == NodeState::SendUplink) {
      uplinkAttempts_ = 0;
    }

    transitionTo(nextState);
  }

  /** @brief Parse modem lines and consume AT responses/events. */
  void handleModemLine(const char* line) {
    if (line[0] == '\0') {
      return;
    }

    Serial.printf("[E5] %s\n", line);

    if (containsToken(line, "Please join network first")) {
      requireJoinBeforeUplinkRetry_ = true;
    }

    if (!pending_.active) {
      return;
    }

    for (uint8_t i = 0; i < pending_.failureCount; ++i) {
      if (containsToken(line, pending_.failureTokens[i])) {
        resolvePendingCommand(false);
        return;
      }
    }

    if (containsToken(line, "ERROR") || containsToken(line, "+ERR")) {
      resolvePendingCommand(false);
      return;
    }

    for (uint8_t i = 0; i < pending_.successCount; ++i) {
      if (containsToken(line, pending_.successTokens[i])) {
        resolvePendingCommand(true);
        return;
      }
    }
  }

  /** @brief UART input parser without dynamic memory allocations. */
  void consumeModemInput() {
    while (modemSerial_.available() > 0) {
      const char byte = static_cast<char>(modemSerial_.read());

      if (byte == '\r') {
        continue;
      }

      if (byte == '\n') {
        lineBuffer_[lineLength_] = '\0';
        handleModemLine(lineBuffer_.data());
        lineLength_ = 0;
        continue;
      }

      if (lineLength_ >= (lineBuffer_.size() - 1U)) {
        lineLength_ = 0;
      }

      lineBuffer_[lineLength_++] = byte;
    }
  }

  /** @brief Timeout watchdog for the active AT command. */
  void processPendingCommandTimeout() {
    if (!pending_.active) {
      return;
    }

    if (isExpired(millis(), pending_.deadlineMs)) {
      resolvePendingCommand(false);
    }
  }

  /** @brief Convert payload bytes to uppercase hex string for MSGHEX command. */
  template <size_t N>
  static std::array<char, (N * 2U) + 1U> toHexString(const std::array<uint8_t, N>& data) {
    static constexpr char kHexLut[] = "0123456789ABCDEF";
    std::array<char, (N * 2U) + 1U> out {};

    for (size_t i = 0; i < N; ++i) {
      out[i * 2U] = kHexLut[(data[i] >> 4U) & 0x0FU];
      out[(i * 2U) + 1U] = kHexLut[data[i] & 0x0FU];
    }

    out.back() = '\0';
    return out;
  }

  /** @brief Pack telemetry into a compact 4-byte binary payload. */
  void packPayload() {
    const float pressureClamped = clampValue(sample_.pressurePsi, config::kProcessMinPsi, config::kProcessMaxPsi);
    const float currentClamped = clampValue(sample_.loopCurrentMa, 0.0F, 65.535F);

    const uint16_t pressureCentiPsi = static_cast<uint16_t>(std::lroundf(pressureClamped * 100.0F));
    const uint16_t currentCentiMa = static_cast<uint16_t>(std::lroundf(currentClamped * 100.0F));

    payload_[0] = static_cast<uint8_t>((pressureCentiPsi >> 8U) & 0xFFU);
    payload_[1] = static_cast<uint8_t>(pressureCentiPsi & 0xFFU);
    payload_[2] = static_cast<uint8_t>((currentCentiMa >> 8U) & 0xFFU);
    payload_[3] = static_cast<uint8_t>(currentCentiMa & 0xFFU);
  }

  /** @brief Compute loop current and process variable from raw ADC conversion. */
  void processSensorSample(int16_t rawCounts) {
    sample_.rawCounts = rawCounts;

    const float voltage = ads_.computeVolts(rawCounts);
    sample_.loopCurrentMa = (voltage / config::kShuntOhms) * 1000.0F;

    const float normalized = clampValue(
        (sample_.loopCurrentMa - config::kSensorCurrentMinMa) /
            (config::kSensorCurrentMaxMa - config::kSensorCurrentMinMa),
        0.0F,
        1.0F);

    sample_.pressurePsi =
        config::kProcessMinPsi + normalized * (config::kProcessMaxPsi - config::kProcessMinPsi);

    packPayload();

    Serial.printf(
        "[SENSOR] raw=%d current=%.3f mA pressure=%.2f psi\n",
        sample_.rawCounts,
        static_cast<double>(sample_.loopCurrentMa),
        static_cast<double>(sample_.pressurePsi));
  }

  /** @brief Enter deep sleep for the configured wake interval. */
  [[noreturn]] void enterDeepSleep() {
    clearPendingCommand();

    // Best-effort command: ask LoRa-E5 to go to low-power mode before ESP32 sleeps.
    modemSerial_.print("AT+LOWPOWER\n");
    modemSerial_.flush();

    esp_sleep_enable_timer_wakeup(config::kWakeupIntervalUs);

    Serial.println("[POWER] Entering deep sleep");
    Serial.flush();

    esp_deep_sleep_start();
    while (true) {
    }
  }

  /** @brief Human-readable wakeup source for diagnostics. */
  void printWakeupReason() {
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
      case ESP_SLEEP_WAKEUP_TIMER:
        Serial.println("[BOOT] Wakeup by timer");
        break;
      case ESP_SLEEP_WAKEUP_UNDEFINED:
        Serial.println("[BOOT] Cold boot");
        break;
      default:
        Serial.printf("[BOOT] Wakeup cause=%d\n", static_cast<int>(cause));
        break;
    }
  }

  /** @brief Core deterministic state machine. */
  void processState(bool enteredState) {
    switch (state_) {
      case NodeState::Startup:
        transitionTo(NodeState::ModemPing);
        break;

      case NodeState::ModemPing:
        if (enteredState) {
          startCommand(
              "AT",
              config::kAtTimeoutMs,
              NodeState::ModemSetMode,
              NodeState::Fault,
              {"+AT: OK"});
        }
        break;

      case NodeState::ModemSetMode:
        if (enteredState) {
          startCommand(
              "AT+MODE=LWOTAA",
              config::kAtTimeoutMs,
              NodeState::ModemSetRegion,
              NodeState::Fault,
              {"+MODE: LWOTAA"});
        }
        break;

      case NodeState::ModemSetRegion:
        if (enteredState) {
          std::array<char, 40> cmd {};
          std::snprintf(cmd.data(), cmd.size(), "AT+DR=%s", config::kRegion);

          startCommand(
              cmd.data(),
              config::kAtTimeoutMs,
              NodeState::ModemSetAdr,
              NodeState::Fault,
              {"+DR:"});
        }
        break;

      case NodeState::ModemSetAdr:
        if (enteredState) {
          startCommand(
              "AT+ADR=ON",
              config::kAtTimeoutMs,
              NodeState::ModemSetPort,
              NodeState::Fault,
              {"+ADR: ON", "+ADR: OFF"});
        }
        break;

      case NodeState::ModemSetPort:
        if (enteredState) {
          std::array<char, 32> cmd {};
          std::snprintf(cmd.data(), cmd.size(), "AT+PORT=%u", config::kUplinkPort);

          startCommand(
              cmd.data(),
              config::kAtTimeoutMs,
              NodeState::ModemSetAppEui,
              NodeState::Fault,
              {"+PORT:"});
        }
        break;

      case NodeState::ModemSetAppEui:
        if (enteredState) {
          std::array<char, 64> cmd {};
          std::snprintf(cmd.data(), cmd.size(), "AT+ID=AppEui,\"%s\"", config::kAppEui);

          startCommand(
              cmd.data(),
              config::kAtTimeoutMs,
              NodeState::ModemSetDevEui,
              NodeState::Fault,
              {"+ID: AppEui"});
        }
        break;

      case NodeState::ModemSetDevEui:
        if (enteredState) {
          std::array<char, 64> cmd {};
          std::snprintf(cmd.data(), cmd.size(), "AT+ID=DevEui,\"%s\"", config::kDevEui);

          startCommand(
              cmd.data(),
              config::kAtTimeoutMs,
              NodeState::ModemSetAppKey,
              NodeState::Fault,
              {"+ID: DevEui"});
        }
        break;

      case NodeState::ModemSetAppKey:
        if (enteredState) {
          std::array<char, 80> cmd {};
          std::snprintf(cmd.data(), cmd.size(), "AT+KEY=APPKEY,\"%s\"", config::kAppKey);

          startCommand(
              cmd.data(),
              config::kAtTimeoutMs,
              NodeState::ModemJoin,
              NodeState::Fault,
              {"+KEY: APPKEY"});
        }
        break;

      case NodeState::ModemJoin:
        if (enteredState) {
          requireJoinBeforeUplinkRetry_ = false;

          if (joinAttempts_ >= config::kMaxJoinAttempts) {
            transitionTo(NodeState::EnterSleep);
            break;
          }

          ++joinAttempts_;

          startCommand(
              "AT+JOIN",
              config::kJoinTimeoutMs,
              NodeState::SensorStartConversion,
              NodeState::JoinRetryWait,
              {"+JOIN: Done", "+JOIN: Joined already", "+JOIN: Network joined"},
              {"+JOIN: Join failed", "No free channel", "LoRaWAN modem is busy"});
        }
        break;

      case NodeState::JoinRetryWait:
        if (millis() - stateEntryMs_ >= config::kRetryBackoffMs) {
          transitionTo(NodeState::ModemJoin);
        }
        break;

      case NodeState::SensorStartConversion:
        if (enteredState) {
          ads_.startADCReading(config::kAds1115Mux, false);
          adcStartMs_ = millis();
          transitionTo(NodeState::SensorWaitConversion);
        }
        break;

      case NodeState::SensorWaitConversion:
        if (ads_.conversionComplete()) {
          processSensorSample(ads_.getLastConversionResults());
          transitionTo(NodeState::SendUplink);
        } else if (millis() - adcStartMs_ >= config::kAdcTimeoutMs) {
          transitionTo(NodeState::Fault);
        }
        break;

      case NodeState::SendUplink:
        if (enteredState) {
          if (uplinkAttempts_ >= config::kMaxUplinkAttempts) {
            transitionTo(NodeState::EnterSleep);
            break;
          }

          ++uplinkAttempts_;

          const auto hexPayload = toHexString(payload_);
          std::array<char, 48> cmd {};
          std::snprintf(cmd.data(), cmd.size(), "AT+MSGHEX=\"%s\"", hexPayload.data());

          startCommand(
              cmd.data(),
              config::kUplinkTimeoutMs,
              NodeState::EnterSleep,
              NodeState::UplinkRetryWait,
              {"+MSGHEX: Done", "+MSGHEX: ACK Received"},
              {"Please join network first", "No free channel", "LoRaWAN modem is busy"});
        }
        break;

      case NodeState::UplinkRetryWait:
        if (millis() - stateEntryMs_ >= config::kRetryBackoffMs) {
          if (requireJoinBeforeUplinkRetry_) {
            transitionTo(NodeState::ModemJoin);
          } else {
            transitionTo(NodeState::SendUplink);
          }
        }
        break;

      case NodeState::EnterSleep:
        if (enteredState) {
          enterDeepSleep();
        }
        break;

      case NodeState::Fault:
        if (enteredState) {
          Serial.println("[FAULT] Unexpected condition, forcing deep sleep recovery");
          transitionTo(NodeState::EnterSleep);
        }
        break;
    }
  }
};

IndustrialNode gNode(Serial2);

void setup() {
  gNode.begin();
}

void loop() {
  gNode.update();
}
