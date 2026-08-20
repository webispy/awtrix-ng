#include "transport/serial/SerialApiService.h"

#include <Arduino.h>

#include "core/CoreEngine.h"
#include "core/api/ApiRouter.h"
#include "system/Log.h"

namespace awtrix {

namespace {

// Replies carry the sender's sequence number back so a host can pair them up: the log shares this
// wire, and a reply can be lost with the cable. Injected rather than composed because the result
// JSON comes back from the shared router already finished.
std::string withSeq(const std::string& json, long seq) {
  if (seq < 0 || json.size() < 2 || json[0] != '{') return json;
  std::string out = "{\"seq\":";
  out += std::to_string(seq);
  out += json.size() == 2 ? "" : ",";
  out.append(json, 1, std::string::npos);
  return out;
}

}

void SerialApiService::begin(CoreEngine& engine, int totalPixels) {
  engine_ = &engine;
  total_ = totalPixels;
  // Five bytes is the delta record; a full frame needs three per pixel and is bounded again where
  // it is applied, so one ceiling covers both shapes.
  proto_.setMaxFrame(totalPixels > 0 ? static_cast<std::size_t>(totalPixels) * 5u : 0u);
  logf("serial: control channel ready (%d px)", totalPixels);
}

void SerialApiService::reply(const std::string& json, long seq) {
  const std::string line = withSeq(json, seq);
  Serial.write("<<", 2);
  Serial.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  Serial.write('\n');
}

void SerialApiService::replyError(const char* code, const char* message, long seq) {
  reply(api::errorJson(code, message), seq);
}

void SerialApiService::pump(int64_t nowMs) {
  if (!engine_) return;

  int budget = kBytesPerTick;
  int commands = 0;

  while (Serial.available() > 0) {
    if (budget <= 0 && !proto_.awaitingFrame()) break;
    if (commands >= kCommandsPerTick) break;
    --budget;
    lastByteMs_ = nowMs;

    switch (proto_.push(static_cast<char>(Serial.read()))) {
      case api::SerialEvent::Command: {
        ++commands;
        Command cmd;
        std::string immediate;
        switch (api::routeMqtt(proto_.topic(), proto_.body(), cmd, immediate)) {
          case api::RouteOutcome::Routed: {
            const DispatchResult r = engine_->execute(cmd);
            reply(api::mqttResult(r, engine_->lastDetail()), proto_.seq());
            break;
          }
          case api::RouteOutcome::Respond:
            reply(immediate, proto_.seq());
            break;
          case api::RouteOutcome::NoMatch:
          default:
            replyError("notFound", "unknown topic", proto_.seq());
            break;
        }
        break;
      }

      case api::SerialEvent::Frame:
        applyFull(nowMs);
        break;

      case api::SerialEvent::Delta:
        applyDelta(nowMs);
        break;

      case api::SerialEvent::Error:
        ++bad_;
        replyError(proto_.error(), proto_.errorMessage(), proto_.seq());
        break;

      case api::SerialEvent::None:
      default:
        break;
    }
  }

  if (proto_.expire(nowMs, lastByteMs_, kIdleMs))
    logdbg("serial: dropped an incomplete line or frame");
}

// A stream that has lapsed starts from black, so a first frame that covers only part of the panel
// cannot reveal a frozen strip of whatever app was on screen. Within a session pixels persist,
// which is what makes partial frames and deltas useful.
void SerialApiService::beginSessionIfStale(int64_t nowMs) {
  if (streaming(nowMs) && static_cast<int>(frame_.size()) == total_) return;
  frame_.assign(static_cast<std::size_t>(total_), 0u);
  lastSeq_ = -1;
  // The throughput window belongs to the session: measured from boot instead, the first report
  // divides a hundred frames by however long the device had been idle beforehand.
  statsStartMs_ = nowMs;
  frames_ = 0;
}

void SerialApiService::noteFrame(int64_t nowMs) {
  lastFrameMs_ = nowMs;
  const long seq = proto_.seq();
  // The host counts frames; a jump in its numbering is the only way we learn that the UART ring
  // overflowed or a payload was cut short, since there is no flow control on this cable.
  if (lastSeq_ >= 0 && seq >= 0 && seq != lastSeq_ + 1) ++gaps_;
  lastSeq_ = seq;

  if (++frames_ < kStatsEvery) return;
  const int64_t span = nowMs - statsStartMs_;
  const unsigned fps = span > 0 ? static_cast<unsigned>((frames_ * 1000) / span) : 0;
  std::string s = "{\"stat\":\"stream\",\"fps\":";
  s += std::to_string(fps);
  s += ",\"frames\":" + std::to_string(frames_);
  s += ",\"gaps\":" + std::to_string(gaps_);
  s += ",\"bad\":" + std::to_string(bad_) + "}";
  reply(s, -1);
  frames_ = 0;
  statsStartMs_ = nowMs;
}

void SerialApiService::applyFull(int64_t nowMs) {
  beginSessionIfStale(nowMs);
  const std::vector<uint8_t>& in = proto_.frame();
  const std::size_t pixels = in.size() / 3;
  for (std::size_t p = 0; p < pixels && p < frame_.size(); ++p)
    frame_[p] = (static_cast<uint32_t>(in[p * 3]) << 16) |
                (static_cast<uint32_t>(in[p * 3 + 1]) << 8) | in[p * 3 + 2];
  noteFrame(nowMs);
}

void SerialApiService::applyDelta(int64_t nowMs) {
  beginSessionIfStale(nowMs);
  const std::vector<uint8_t>& in = proto_.frame();
  for (std::size_t r = 0; r + 4 < in.size(); r += 5) {
    const std::size_t p = static_cast<std::size_t>(in[r]) | (static_cast<std::size_t>(in[r + 1]) << 8);
    if (p >= frame_.size()) continue;
    frame_[p] = (static_cast<uint32_t>(in[r + 2]) << 16) |
                (static_cast<uint32_t>(in[r + 3]) << 8) | in[r + 4];
  }
  noteFrame(nowMs);
}

bool SerialApiService::paint(Canvas& out, int64_t nowMs) {
  const int total = out.width() * out.height();
  if (total != total_) {
    // The panel was reconfigured under us; anything held is the wrong shape now.
    total_ = total;
    proto_.setMaxFrame(total > 0 ? static_cast<std::size_t>(total) * 5u : 0u);
    frame_.clear();
  }
  if (!streaming(nowMs)) {
    if (!frame_.empty()) frame_.clear();
    return false;
  }
  if (static_cast<int>(frame_.size()) == total)
    for (int p = 0; p < total; ++p) out.setPixel(p % out.width(), p / out.width(), frame_[p]);
  return true;
}

}
