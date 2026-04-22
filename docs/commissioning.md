# Commissioning (LoRaWAN OTAA)

## 1. Antes de compilar

Configura en `platformio.ini`:

- `LORAE5_REGION` (ejemplo: `US915`, `AU915`, `EU868`)
- `LORAE5_APP_EUI`
- `LORAE5_DEV_EUI`
- `LORAE5_APP_KEY`
- `LORAE5_UPLINK_PORT`

## 2. Alta del dispositivo en el LNS

En tu servidor LoRaWAN (TTN/ChirpStack/etc):

- Registrar `DevEUI`
- Registrar `AppEUI` / `JoinEUI`
- Registrar `AppKey`
- Verificar plan regional compatible con `LORAE5_REGION`

## 3. Verificación en monitor serie

Comandos/etapas esperadas al arrancar:

- `AT`
- `AT+MODE=LWOTAA`
- `AT+DR=<REGION>`
- `AT+ID=AppEui,...`
- `AT+ID=DevEui,...`
- `AT+KEY=APPKEY,...`
- `AT+JOIN`
- `AT+MSGHEX="..."`

Respuesta de transmisión esperada:

- `+MSGHEX: Done`

Después de ese evento, el nodo entra a Deep Sleep.

## 4. Diagnóstico rápido

- Si aparece `Please join network first`: revisar credenciales OTAA y cobertura.
- Si aparece `No free channel`: revisar duty-cycle/canales/sub-band según región.
- Si no hay join: validar antena, gateway y parámetros regionales.

## 5. Validación de payload en backend

Payload recibido (4 bytes):

- `pressure_psi = uint16_be(bytes[0..1]) / 100.0`
- `loop_current_ma = uint16_be(bytes[2..3]) / 100.0`
