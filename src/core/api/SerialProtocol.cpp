#include "core/api/SerialProtocol.h"

namespace awtrix {
namespace api {

namespace {

// Decimal only, and it refuses to overflow into nonsense: a header is attacker-adjacent input in
// the sense that it arrives from a cable, not from a peer we authenticated.
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
  frame_.clear();
  want_ = 0;
}

bool SerialProtocol::expire(int64_t nowMs, int64_t lastByteMs, int64_t idleMs) {
  const bool pending = !line_.empty() || awaitingFrame() || mode_ == Mode::Discard;
  if (!pending || nowMs - lastByteMs < idleMs) return false;
  reset();
  return true;
}

SerialEvent SerialProtocol::push(char c) {
  switch (mode_) {
    case Mode::Frame:
    case Mode::Delta: {
      frame_.push_back(static_cast<uint8_t>(c));
      if (frame_.size() < want_) return SerialEvent::None;
      const SerialEvent done = mode_ == Mode::Frame ? SerialEvent::Frame : SerialEvent::Delta;
      mode_ = Mode::Line;
      return done;
    }

    // A line that outgrew the cap is already unusable, so the rest of it is swallowed rather than
    // half-parsed. The sender learns about it from the error reply, not from silence.
    case Mode::Discard:
      if (c == '\n') mode_ = Mode::Line;
      return SerialEvent::None;

    case Mode::Line:
    default:
      break;
  }

  if (c == '\r') return SerialEvent::None;
  if (c == '\n') return finishLine();
  if (line_.size() >= maxLine_) {
    line_.clear();
    mode_ = Mode::Discard;
    // The error belongs to this oversized line, not to the successfully parsed line before it.
    // finishLine() normally clears the sequence, but an oversized line never reaches it.
    seq_ = -1;
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

  if (line.compare(at, 2, "!F") == 0 || line.compare(at, 2, "!D") == 0)
    return beginFrame(line.substr(at));

  if (line[at] == '#') {
    const std::size_t sp = line.find(' ', at);
    if (sp == std::string::npos) return fail("invalidRequest", "sequence number without a command");
    if (!parseUnsigned(line, at + 1, sp, seq_)) return fail("invalidRequest", "malformed sequence number");
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

// header is "!F<seq>:<len>" or "!D<seq>:<len>", already trimmed of leading blanks.
SerialEvent SerialProtocol::beginFrame(const std::string& header) {
  const bool delta = header[1] == 'D';
  const std::size_t colon = header.find(':', 2);
  long len = 0;
  if (colon == std::string::npos) return fail("invalidRequest", "frame header needs <seq>:<len>");
  if (!parseUnsigned(header, 2, colon, seq_)) return fail("invalidRequest", "malformed frame sequence");
  if (!parseUnsigned(header, colon + 1, header.size(), len))
    return fail("invalidRequest", "malformed frame length");

  const std::size_t want = static_cast<std::size_t>(len);
  if (want == 0 || want > maxFrame_) return fail("payloadTooLarge", "frame length out of range");
  // Truncating either record type would land half a pixel in the canvas, so the length has to
  // divide evenly rather than being rounded down here.
  const std::size_t stride = delta ? 5u : 3u;
  if (want % stride != 0) return fail("invalidRequest", "frame length is not a whole pixel count");

  frame_.clear();
  frame_.reserve(want);
  want_ = want;
  mode_ = delta ? Mode::Delta : Mode::Frame;
  return SerialEvent::None;
}

}
}
