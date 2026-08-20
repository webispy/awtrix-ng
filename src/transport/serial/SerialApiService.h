#pragma once

#include <cstdint>
#include <string>

#include "core/api/SerialProtocol.h"

namespace awtrix {

class CoreEngine;

// The USB serial control channel: the same commands MQTT accepts, over the cable that is already
// there for flashing. It exists because the panel and the machine driving it are not always on the
// same network - see docs/reference/serial.md.
class SerialApiService {
 public:
  void begin(CoreEngine& engine);

  // Reads what the port has, executes at most kCommandsPerTick commands, and answers each one.
  void pump(int64_t nowMs);

 private:
  // A partial line older than this is a leftover from a cable that went away.
  static constexpr int64_t kIdleMs = 200;
  // Bounds the worst-case time one loop spends on the port.
  static constexpr int kBytesPerTick = 1024;
  static constexpr int kCommandsPerTick = 4;

  void reply(const std::string& json, long seq);
  void replyError(const char* code, const char* message, long seq);

  api::SerialProtocol proto_;
  CoreEngine* engine_ = nullptr;
  int64_t lastByteMs_ = 0;
};

}
