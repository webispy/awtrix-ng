#include "core/api/SerialProtocol.h"

namespace awtrix {
namespace api {

namespace {

// Decimal only, and it refuses to overflow into nonsense: a sequence number arrives from a cable,
// not from a peer we authenticated.
bool parseUnsigned(const std::string& s, std::size_t from, std::size_t to, long& out) {
  if (from >= to) return false;
  long v = 0;
  for (std::size_t i = from; i < to; ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
    if (v > 1000000) return false;
  }
  out = v;
  return true;
}

}

SerialEvent SerialProtocol::fail(const char* code, const char* message) {
  error_ = code;
  errorMessage_ = message;
  return SerialEvent::Error;
}

void SerialProtocol::reset() {
  mode_ = Mode::Line;
  line_.clear();
}

bool SerialProtocol::expire(int64_t nowMs, int64_t lastByteMs, int64_t idleMs) {
  const bool pending = !line_.empty() || mode_ == Mode::Discard;
  if (!pending || nowMs - lastByteMs < idleMs) return false;
  reset();
  return true;
}

SerialEvent SerialProtocol::push(char c) {
  // A line that outgrew the cap is already unusable, so the rest of it is swallowed rather than
  // half-parsed. The sender learns about it from the error reply, not from silence.
  if (mode_ == Mode::Discard) {
    if (c == '\n') mode_ = Mode::Line;
    return SerialEvent::None;
  }

  if (c == '\r') return SerialEvent::None;
  if (c == '\n') return finishLine();
  if (line_.size() >= maxLine_) {
    line_.clear();
    mode_ = Mode::Discard;
    return fail("payloadTooLarge", "line exceeds the serial line limit");
  }
  line_.push_back(c);
  return SerialEvent::None;
}

SerialEvent SerialProtocol::finishLine() {
  const std::string line = line_;
  line_.clear();
  seq_ = -1;
  topic_.clear();
  body_.clear();

  std::size_t at = 0;
  while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
  if (at >= line.size()) return SerialEvent::None;  // blank line: the resync nudge

  if (line[at] == '#') {
    const std::size_t sp = line.find(' ', at);
    if (sp == std::string::npos) return fail("invalidRequest", "sequence number without a command");
    if (!parseUnsigned(line, at + 1, sp, seq_))
      return fail("invalidRequest", "malformed sequence number");
    at = sp + 1;
    while (at < line.size() && line[at] == ' ') ++at;
    if (at >= line.size()) return fail("invalidRequest", "sequence number without a command");
  }

  const std::size_t sp = line.find(' ', at);
  if (sp == std::string::npos) {
    topic_ = line.substr(at);
  } else {
    topic_ = line.substr(at, sp - at);
    body_ = line.substr(sp + 1);
  }
  return SerialEvent::Command;
}

}
}
