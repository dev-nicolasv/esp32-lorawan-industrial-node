# Power Optimization Notes

## 1. Estrategia actual en firmware

- Arquitectura event-driven sin `delay()`
- Una sola adquisición y una sola transmisión por ciclo de wake
- Deep Sleep inmediatamente al completar uplink
- Intervalo de wake configurable por timer (`15 min` por defecto)

## 2. Puntos críticos de consumo

- Join OTAA puede ser la fase más costosa energéticamente
- TX LoRaWAN domina el consumo instantáneo
- Debug por UART en producción incrementa consumo

## 3. Recomendaciones para versión de campo

- Reducir `Serial` de depuración en build de producción
- Evaluar preservación de sesión LoRaWAN para minimizar joins
- Implementar power-gating por hardware para periféricos no usados
- Ajustar data rate, ADR y periodicidad según presupuesto energético
- Medir corriente con perfilador para cerrar autonomía real

## 4. Consideraciones del LoRa-E5

El firmware ejecuta `AT+LOWPOWER` antes de dormir el ESP32. Para ahorros máximos, complementar con diseño hardware:

- Línea de control para enable/reset del LoRa-E5
- Reguladores de baja corriente en reposo
- Diseño de caminos de fuga minimizados en PCB
