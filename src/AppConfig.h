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
// line rate: ~350 ms of traffic at any speed (4096 bytes is 356 ms at 115200; the Arduino default
// of 256 bytes covers 22 ms and is the floor). Multiply before dividing, in a wide type, so a baud
// below 115200 is not truncated to 0 and a non-multiple is not under-provisioned - the old
// `(AWTRIX_SERIAL_BAUD / 115200) * 4096` did both, and gave 0 at 57600.
inline constexpr long long kSerialRxScaled =
    static_cast<long long>(AWTRIX_SERIAL_BAUD) * 4096 / 115200;
inline constexpr int kSerialRxBufferBytes =
    static_cast<int>(kSerialRxScaled > 256 ? kSerialRxScaled : 256);

inline constexpr int kMaxPushedApps = 50;
inline constexpr int kCommandQueueDepth = 16;
inline constexpr int kMaxNotifications = 32;
}
