#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/api/SerialProtocol.h"
#include "core/render/Canvas.h"

namespace awtrix {

class CoreEngine;

// The USB serial control channel: the same commands MQTT accepts, plus raw frame streaming, over
// the cable that is already there for flashing. It exists because the panel and the machine
// driving it are not always on the same network - see docs/reference/serial.md.
//
// Input is drained in pump() and painted in paint(), deliberately split: commands have to keep
// working while something else owns the screen (a powered-off display is exactly when you need to
// send the command that powers it back on), whereas frames only matter when the panel is free.
class SerialApiService {
 public:
  void begin(CoreEngine& engine, int totalPixels);

  // Reads what the port has, executes at most kCommandsPerTick commands, and answers each one.
  void pump(int64_t nowMs);

  // Paints the most recent streamed frame and reports whether the stream owns the display. Mirrors
  // ArtnetService: a hold window rather than a mode, so a sender that stops just goes quiet.
  bool paint(Canvas& out, int64_t nowMs);

  // Emits a debounced physical-button edge independently of command replies.
  void emitButtonEvent(const char* button, bool pressed, int64_t atMs);

  bool streaming(int64_t nowMs) const { return nowMs - lastFrameMs_ < kHoldMs; }

 private:
  // Long enough that a sender at 10 fps never falls out of it, short enough that the apps come
  // back promptly once the host stops. Same value Art-Net uses.
  static constexpr int64_t kHoldMs = 5000;
  // A partial line or frame older than this is a leftover from a cable that went away.
  static constexpr int64_t kIdleMs = 200;
  // Bounds the worst-case time one loop spends on the port. Frame payloads are exempt so a frame
  // is never spread over more ticks than the wire needs; correctness does not rest on it, since a
  // frame is only applied once it is whole.
  static constexpr int kBytesPerTick = 1024;
  static constexpr int kCommandsPerTick = 4;
  // Frames are not acknowledged one by one - the round trip would cost more than the frame - so
  // throughput is reported in batches instead.
  static constexpr uint32_t kStatsEvery = 100;

  void reply(const std::string& json, long seq);
  void replyError(const char* code, const char* message, long seq);
  void applyFull(int64_t nowMs);
  void applyDelta(int64_t nowMs);
  void beginSessionIfStale(int64_t nowMs);
  void noteFrame(int64_t nowMs);

  api::SerialProtocol proto_;
  CoreEngine* engine_ = nullptr;
  int total_ = 0;
  std::vector<uint32_t> frame_;
  int64_t lastFrameMs_ = -100000;
  int64_t lastByteMs_ = 0;
  int64_t statsStartMs_ = 0;
  uint32_t frames_ = 0;
  uint32_t gaps_ = 0;
  uint32_t bad_ = 0;
  long lastSeq_ = -1;
};

}
