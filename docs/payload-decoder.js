/**
 * Payload decoder for The Things Stack / ChirpStack JavaScript format.
 * Payload layout:
 *   bytes[0..1] = pressure in centi-psi (uint16, big-endian)
 *   bytes[2..3] = loop current in centi-mA (uint16, big-endian)
 */
function decodeUplink(input) {
  const bytes = input.bytes || [];

  if (bytes.length !== 4) {
    return {
      errors: ["Invalid payload length. Expected exactly 4 bytes."]
    };
  }

  const pressureCentiPsi = (bytes[0] << 8) | bytes[1];
  const currentCentiMa = (bytes[2] << 8) | bytes[3];

  return {
    data: {
      pressure_psi: pressureCentiPsi / 100,
      loop_current_ma: currentCentiMa / 100
    }
  };
}
