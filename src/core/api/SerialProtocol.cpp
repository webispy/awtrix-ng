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

// RGB565 back to the canvas's RGB888. The high bits are replicated into the low ones rather than
// zero-filled, so full stays full and the two ends round-trip: 0x1f becomes 0xff, not 0xf8.
uint32_t expand565(uint16_t c) {
  const uint32_t r = (c >> 11) & 0x1fu;
  const uint32_t g = (c >> 5) & 0x3fu;
  const uint32_t b = c & 0x1fu;
  return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

uint16_t le16(const uint8_t* at) {
  return static_cast<uint16_t>(at[0]) | static_cast<uint16_t>(static_cast<uint16_t>(at[1]) << 8);
}

std::size_t bitsSet(const uint8_t* at, std::size_t bytes) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < bytes; ++i)
    for (uint8_t b = at[i]; b; b &= static_cast<uint8_t>(b - 1)) ++n;
  return n;
}

std::size_t maskBytesFor(std::size_t total) { return (total + 7) / 8; }

}

uint16_t crc16(const uint8_t* data, std::size_t length) {
  uint16_t crc = 0xffff;
  for (std::size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

const char* frameFaultMessage(FrameFault fault) {
  switch (fault) {
    case FrameFault::Checksum:
      return "frame failed its checksum";
    case FrameFault::Length:
      return "frame length does not match its layout";
    case FrameFault::Layout:
      return "unknown frame layout";
    case FrameFault::None:
    default:
      return "";
  }
}

FrameFault checkFrame(const uint8_t* payload, std::size_t length, std::size_t total) {
  if (length < kMinFramePayload) return FrameFault::Length;
  const std::size_t body = length - 2;
  if (crc16(payload, body) != le16(payload + body)) return FrameFault::Checksum;

  const std::size_t data = body - 1;
  const uint8_t* at = payload + 1;
  switch (static_cast<FrameLayout>(payload[0])) {
    case FrameLayout::Full:
      return data == total * 2 ? FrameFault::None : FrameFault::Length;
    case FrameLayout::Index:
      return data % 3 == 0 ? FrameFault::None : FrameFault::Length;
    case FrameLayout::Mask: {
      const std::size_t mask = maskBytesFor(total);
      if (data < mask) return FrameFault::Length;
      return data == mask + bitsSet(at, mask) * 2 ? FrameFault::None : FrameFault::Length;
    }
    default:
      return FrameFault::Layout;
  }
}

void applyFrame(const uint8_t* payload, std::size_t length, std::size_t total, uint32_t* canvas) {
  const std::size_t data = length - 3;
  const uint8_t* at = payload + 1;
  switch (static_cast<FrameLayout>(payload[0])) {
    case FrameLayout::Full:
      for (std::size_t p = 0; p < total; ++p) canvas[p] = expand565(le16(at + p * 2));
      return;
    case FrameLayout::Index:
      for (std::size_t r = 0; r + 2 < data; r += 3) {
        const std::size_t p = at[r];
        if (p < total) canvas[p] = expand565(le16(at + r + 1));
      }
      return;
    case FrameLayout::Mask:
    default: {
      const std::size_t mask = maskBytesFor(total);
      const uint8_t* colour = at + mask;
      for (std::size_t p = 0; p < total; ++p) {
        if ((at[p / 8] & (1u << (p % 8))) == 0) continue;
        canvas[p] = expand565(le16(colour));
        colour += 2;
      }
      return;
    }
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
    case Mode::Frame: {
      frame_.push_back(static_cast<uint8_t>(c));
      if (frame_.size() < want_) return SerialEvent::None;
      mode_ = Mode::Line;
      return SerialEvent::Frame;
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

  if (line.compare(at, 2, "!P") == 0) return beginFrame(line.substr(at));

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

// header is "!P<seq>:<len>", already trimmed of leading blanks.
SerialEvent SerialProtocol::beginFrame(const std::string& header) {
  const std::size_t colon = header.find(':', 2);
  long len = 0;
  if (colon == std::string::npos) return fail("invalidRequest", "frame header needs <seq>:<len>");
  if (!parseUnsigned(header, 2, colon, seq_)) return fail("invalidRequest", "malformed frame sequence");
  if (!parseUnsigned(header, colon + 1, header.size(), len))
    return fail("invalidRequest", "malformed frame length");

  const std::size_t want = static_cast<std::size_t>(len);
  // A payload too short to hold a layout byte and a checksum cannot be checked at all, so it is
  // refused here rather than reaching a reader that would have to guess. Whether the rest of the
  // length makes sense depends on the layout and on the canvas, and is checked where both are
  // known.
  if (want < kMinFramePayload || want > maxFrame_)
    return fail("payloadTooLarge", "frame length out of range");

  frame_.clear();
  frame_.reserve(want);
  want_ = want;
  mode_ = Mode::Frame;
  return SerialEvent::None;
}

}
}
