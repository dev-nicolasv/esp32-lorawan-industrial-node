# Hardware Integration Guide

## 1. Topología recomendada

- ESP32 como host principal (gestión de energía, adquisición y lógica de aplicación)
- LoRa-E5 como módem LoRaWAN autónomo por UART
- ADS1115 para adquisición de señal analógica de alta resolución
- Entrada de proceso 4-20 mA con resistor shunt de precisión para conversión I/V

## 2. Cableado base

## 2.1 ADS1115 (I2C)

| ADS1115 | ESP32 | Nota |
|---|---|---|
| VDD | 3V3 | Alimentación limpia, desacople local |
| GND | GND | Tierra común |
| SDA | GPIO21 | `kI2cSdaPin` |
| SCL | GPIO22 | `kI2cSclPin` |
| ADDR | GND | Dirección `0x48` |
| A0 | Nodo de shunt | Canal de lectura 4-20 mA |

## 2.2 LoRa-E5 (UART)

| LoRa-E5 | ESP32 | Nota |
|---|---|---|
| VCC | 3V3 | Confirmar corriente disponible del regulador |
| GND | GND | Tierra común |
| TX | GPIO16 (RX2) | `kLoRaRxPin` |
| RX | GPIO17 (TX2) | `kLoRaTxPin` |

## 3. Front-end 4-20 mA

Recomendación inicial:

- Shunt de precisión: `165R`, tolerancia `0.1%`, bajo coeficiente térmico
- 4 mA -> 0.66 V
- 20 mA -> 3.30 V

Esto aprovecha bien el rango de entrada con alimentación a 3.3 V.

## 4. Conversión de ingeniería

El firmware implementa:

1. `I(mA) = Vshunt / Rshunt * 1000`
2. `pressure = clamp((I - 4) / 16, 0..1) * 100 psi`

## 5. Buenas prácticas industriales

- TVS y protección ESD en entrada de lazo
- Filtro RC anti-ruido antes del ADC
- Aislamiento galvánico si el entorno lo exige
- Fuente robusta y desacople local en ESP32/LoRa-E5/ADS1115
