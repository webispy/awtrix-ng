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

void SerialApiService::begin(CoreEngine& engine) {
  engine_ = &engine;
  logf("serial: control channel ready");
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
    if (budget <= 0) break;
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

      case api::SerialEvent::Error:
        replyError(proto_.error(), proto_.errorMessage(), proto_.seq());
        break;

      case api::SerialEvent::None:
      default:
        break;
    }
  }

  if (proto_.expire(nowMs, lastByteMs_, kIdleMs))
    logdbg("serial: dropped an incomplete line");
}

}
