#include "transport/serial/SerialApiService.h"

#include <Arduino.h>

#include "core/CoreEngine.h"
#include "core/api/ApiRouter.h"
#include "core/api/SerialStateJson.h"
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
  // The largest legal payload is an indexed frame naming every pixel: three bytes each, plus the
  // layout byte and the checksum. Nothing sensible sends that - a full frame is two bytes a pixel -
  // but the ceiling has to admit it, and each layout is measured exactly where it is applied.
  proto_.setMaxFrame(totalPixels > 0 ? static_cast<std::size_t>(totalPixels) * 3u + 3u : 0u);
  logf("serial: control channel ready (%d px)", totalPixels);
}

void SerialApiService::reply(const std::string& json, long seq) {
  // One Serial.write for the whole framed line. Split across three (marker, payload, newline), a
  // log line from another task - arduino-esp32 takes the UART lock per call, not per reply - could
  // land between them and split a reply the host reads a line at a time.
  std::string line = "<<";
  line += withSeq(json, seq);
  line += '\n';
  Serial.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
}

void SerialApiService::replyError(const char* code, const char* message, long seq) {
  reply(api::errorJson(code, message), seq);
}

void SerialApiService::emitButtonEvent(const char* button, bool pressed, int64_t atMs) {
  reply(api::serialButtonEventJson(button, pressed, atMs), -1);
}

void SerialApiService::pump(int64_t nowMs) {
  if (!engine_) return;

  int budget = kBytesPerTick;
  int commands = 0;

  while (Serial.available() > 0) {
    // Both ceilings exempt a frame in progress, for the same reason: stopping between a frame's
    // header and its last byte leaves the parser holding a fragment that the 200 ms idle timer
    // will throw away if anything delays the next pass - and a discarded frame is a lost frame,
    // which is exactly what this channel cannot report cheaply. A frame is bounded by its own
    // declared length, so finishing one is always finite work.
    if (!proto_.awaitingFrame() && (budget <= 0 || commands >= kCommandsPerTick)) break;
    --budget;
    lastByteMs_ = nowMs;

    switch (proto_.push(static_cast<char>(Serial.read()))) {
      case api::SerialEvent::Command: {
        ++commands;
        if (proto_.topic() == "qry/sensors") {
          reply(api::serialSensorsJson(engine_->state().runtime()), proto_.seq());
          break;
        }
        if (proto_.topic() == "qry/buttons") {
          reply(api::serialButtonsJson(engine_->state().runtime()), proto_.seq());
          break;
        }
        if (proto_.topic() == "qry/display") {
          reply(api::serialDisplayJson(engine_->state().settings(), engine_->state().runtime()),
                proto_.seq());
          break;
        }
        // Hand the panel back now rather than at the end of the hold window. Without it a host
        // that has stopped drawing has no way to say so, and the five seconds that keep a stream
        // alive between frames become five seconds of a picture nobody is sending any more - which
        // is what turning the console's output switch off looked like.
        if (proto_.topic() == "cmd/stream/release") {
          lastFrameMs_ = -100000;
          frame_.clear();
          lastSeq_ = -1;
          reply("{\"ok\":true}", proto_.seq());
          break;
        }
        if (proto_.topic() == "qry/capabilities") {
          reply(api::serialCapabilitiesJson(engine_->state().runtime()), proto_.seq());
          break;
        }
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
        applyFrame(nowMs);
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
  gaps_ = 0;
  // `bad_` deliberately survives. It counts malformed input, which arrives *before* a session can
  // begin - a sender with the wrong frame length never gets one started - so zeroing it here threw
  // away the count at precisely the moment it was the only clue on the wire.
}

void SerialApiService::noteFrame(int64_t nowMs) {
  lastFrameMs_ = nowMs;
  const long seq = proto_.seq();
  // The host counts frames; a jump in its numbering is the only way we learn that the UART ring
  // overflowed or a payload was cut short, since there is no flow control on this cable.
  if (seq >= 0 && !api::seqFollows(lastSeq_, seq)) {
    ++gaps_;
    // Said at once, and with both numbers in it. Folding this into the hundred-frame throughput
    // line meant a sender learned it had lost a frame up to a hundred frames later - far too late
    // to repair anything, and with no way to tell where. A driver streaming deltas has to know
    // immediately: until it resends a whole frame, everything it computes is against pixels this
    // panel does not have.
    if (nowMs - lastNoticeMs_ >= kNoticeMs) {
      lastNoticeMs_ = nowMs;
      std::string s = "{\"stat\":\"gap\",\"expected\":";
      s += std::to_string(lastSeq_ + 1);
      s += ",\"got\":" + std::to_string(seq);
      s += ",\"missed\":" + std::to_string(gaps_) + "}";
      reply(s, -1);
    }
  }
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
  gaps_ = 0;
  bad_ = 0;
  statsStartMs_ = nowMs;
}

// One frame, whichever way it spells its pixels out. What each layout means and what makes one
// legal is `api::checkFrame`, which is pure and unit-tested on the host; what is here is only what
// this end knows: whether the stream had lapsed, and who has to be told.
void SerialApiService::applyFrame(int64_t nowMs) {
  const std::vector<uint8_t>& in = proto_.frame();
  const std::size_t total = total_ > 0 ? static_cast<std::size_t>(total_) : 0u;
  const api::FrameFault fault = api::checkFrame(in.data(), in.size(), total);

  if (fault == api::FrameFault::Checksum) {
    ++bad_;
    // Deliberately not adopting the sequence number: a frame that failed its checksum is a frame
    // that did not arrive, and letting the numbering notice the hole on its own is what makes the
    // host resynchronise even if this notice is itself lost. The two share a throttle, so the same
    // frame is not reported twice.
    if (nowMs - lastNoticeMs_ >= kNoticeMs) {
      lastNoticeMs_ = nowMs;
      std::string s = "{\"stat\":\"crc\",\"seq\":";
      s += std::to_string(proto_.seq());
      s += ",\"bad\":" + std::to_string(bad_) + "}";
      reply(s, -1);
    }
    return;
  }
  if (fault != api::FrameFault::None) {
    ++bad_;
    replyError("invalidRequest", api::frameFaultMessage(fault), proto_.seq());
    return;
  }

  beginSessionIfStale(nowMs);
  api::applyFrame(in.data(), in.size(), total, frame_.data());
  noteFrame(nowMs);
}

bool SerialApiService::paint(Canvas& out, int64_t nowMs) {
  const int total = out.width() * out.height();
  if (total != total_) {
    // The panel was reconfigured under us; anything held is the wrong shape now.
    total_ = total;
    proto_.setMaxFrame(total > 0 ? static_cast<std::size_t>(total) * 3u + 3u : 0u);
    frame_.clear();
  }
  if (!streaming(nowMs)) {
    if (!frame_.empty()) frame_.clear();
    return false;
  }
  // Nothing held: say so rather than claiming the screen. Returning true here handed the panel to
  // a stream with no pixels in it, and the apps stood down for the whole five-second hold window -
  // a blank display, which is what a reconfigured panel used to show for five seconds.
  if (static_cast<int>(frame_.size()) != total) return false;
  for (int p = 0; p < total; ++p) out.setPixel(p % out.width(), p / out.width(), frame_[p]);
  return true;
}

}
