#pragma once

#ifndef AWTRIX_NG_VERSION
#define AWTRIX_NG_VERSION "0.0.0-dev"
#endif

// UART0's speed, for the boot log and the serial control channel alike. 115200 is the documented
// default and what `pio device monitor` assumes; raise it only for a board whose USB bridge is
// known to sustain more, with -D AWTRIX_SERIAL_BAUD=230400. Note that this is unrelated to the
// flashing speed: esptool syncs at 115200 regardless.
#ifndef AWTRIX_SERIAL_BAUD
#define AWTRIX_SERIAL_BAUD 115200
#endif

namespace awtrix {
// The RX ring has to cover whatever arrives while the loop is busy elsewhere, so it scales with the
// line rate: ~350 ms of traffic at any speed. The Arduino default of 256 bytes covers 22 ms.
inline constexpr int kSerialRxBufferBytes = (AWTRIX_SERIAL_BAUD / 115200) * 4096;

inline constexpr int kMaxPushedApps = 50;
inline constexpr int kCommandQueueDepth = 16;
inline constexpr int kMaxNotifications = 32;
}
