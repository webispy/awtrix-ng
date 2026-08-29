#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace awtrix {
namespace api {

// Framing for the USB serial control channel. Arduino-free on purpose: the byte-level state
// machine is where the awkward cases live (a cable pulled mid-line, a binary payload that
// contains 0x0A, a sender that outruns the reader), so it is unit-tested on the host.
//
// Two shapes share the wire:
//
//   [#<seq> ]<topic>[ <body>]\n      a command line, routed through api::routeMqtt
//   !P<seq>:<len>\n<len raw bytes>   a frame: [layout][data...][crc16 LE]
//
// The frame header is ASCII so it parses as an ordinary line; only its payload is binary, read by
// count rather than by delimiter. Empty lines are ignored, which is what lets a sender write a
// bare newline to resynchronise after a reconnect.
//
// The payload's first byte says how its pixels are spelled out - every pixel, an index per pixel,
// or a bitmap of which ones moved - and its colours are RGB565 throughout. What each layout means
// is SerialApiService's business, because only that end knows how big the canvas is; this parser
// takes the payload by count and checks nothing about it but its length and its checksum. See
// docs/reference/serial.md for the format and the measurements behind it.
// The frame counter both ends keep, and the one number in this protocol that neither end can check
// on its own. A host numbers its frames and wraps somewhere; this end notices a number it did not
// expect and reports it, because a cable with no flow control has no other way to say "I never got
// that one".
//
// Wrapping is detected by the number going *down*, not by comparing against a constant. That was
// how this worked and it was a trap: the wrap point lived as a literal in the receiver while the
// docs called it a suggestion, so a host that wrapped anywhere else scored one phantom lost frame
// per cycle - silently, in a counter nobody could tie back to a cause. Any wrap point now works,
// and the only case missed is a real loss that happens to straddle one.
//
// This value is what the tools and docs recommend, not a rule the parser enforces: it keeps the
// number four digits wide on the wire, which is worth a byte or two a frame at these rates.
inline constexpr long kSeqWrap = 10000;

// Whether no frame is missing between `previous` and `seq`.
//
// The question is only ever "was something lost", so the three ways of not having lost anything are
// all true here: nothing counted yet (`previous` below zero), the next number, or a number that did
// not advance - a wrap, or a sender that repeated itself. A repeat is a sender bug rather than a
// lost frame, and answering "lost" to it would have a driver resend a whole frame to repair pixels
// that were never wrong.
inline constexpr bool seqFollows(long previous, long seq) {
  return previous < 0 || seq <= previous || seq == previous + 1;
}

enum class SerialEvent : uint8_t {
  None,
  Command,
  Frame,
  Error,
};

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, no reflection, no final xor. Checks as 0x29b1 over
// "123456789". Two bytes on every frame, and the only thing on this cable that can tell a frame
// that arrived wrong from one that arrived right: a sequence number sees a frame that never came,
// and nothing else here sees one whose bytes changed on the way. Bitwise rather than table-driven,
// because 512 bytes is 4096 iterations and this core has them to spare.
uint16_t crc16(const uint8_t* data, std::size_t length);

// The smallest legal payload: a layout byte and its checksum, which is the frame a host sends when
// nothing changed but the panel is still its.
inline constexpr std::size_t kMinFramePayload = 3;

// How a payload spells out which pixels it carries. The host sends whichever is smallest for the
// frame in hand, so all three arrive in ordinary use: indices for a still panel, a bitmap for an
// animation, everything for a scene change or a keyframe.
enum class FrameLayout : uint8_t {
  Full = 0,   // every pixel, row-major, RGB565 little-endian
  Index = 1,  // 3-byte records: pixel index, then its colour
  Mask = 2,   // a bitmap of which pixels changed, LSB first, then their colours in index order
};

enum class FrameFault : uint8_t { None, Checksum, Length, Layout };

// Why a frame was refused, in the words the sender is answered with.
const char* frameFaultMessage(FrameFault fault);

// Whether a payload is a frame this canvas can apply: checksum first, then its length against what
// its layout claims. Checked before anything is believed, because a payload whose bytes changed on
// the way can otherwise describe a perfectly plausible frame - a length that divides, an index in
// range - and be painted.
FrameFault checkFrame(const uint8_t* payload, std::size_t length, std::size_t total);

// Paints a payload that `checkFrame` has already passed onto `canvas`, which holds `total` pixels
// as 0xRRGGBB. Every index in the payload is known to be inside the canvas by then, which is why
// this does not check any of them again.
void applyFrame(const uint8_t* payload, std::size_t length, std::size_t total, uint32_t* canvas);

class SerialProtocol {
 public:
  // Command lines are short - a notification payload is tens of bytes - but a frame of pixel art
  // sent as draw commands is not, and the HTTP side allows 8 KB for exactly that. 2 KB covers a
  // full-panel bitmap and leaves the RX ring, which is twice this, room to absorb a stalled loop.
  static constexpr std::size_t kDefaultMaxLine = 2048;

  explicit SerialProtocol(std::size_t maxLine = kDefaultMaxLine) : maxLine_(maxLine) {}

  // Payload ceiling for !P. Zero rejects every frame, which is what a caller that has no canvas
  // yet wants.
  void setMaxFrame(std::size_t bytes) { maxFrame_ = bytes; }

  SerialEvent push(char c);

  // Drops a half-received line or frame once the line has been silent for idleMs. Without this a
  // fragment left behind by an unplugged cable would splice onto the next session's first bytes.
  // Returns true when something was thrown away.
  bool expire(int64_t nowMs, int64_t lastByteMs, int64_t idleMs);

  void reset();

  bool awaitingFrame() const { return mode_ == Mode::Frame; }
  // Bytes still outstanding for the frame being received; zero outside frame mode.
  std::size_t framePending() const { return awaitingFrame() ? want_ - frame_.size() : 0; }

  const std::string& topic() const { return topic_; }
  const std::string& body() const { return body_; }
  // -1 when the sender left the sequence number off.
  long seq() const { return seq_; }
  const std::vector<uint8_t>& frame() const { return frame_; }
  const char* error() const { return error_; }
  const char* errorMessage() const { return errorMessage_; }

 private:
  enum class Mode : uint8_t { Line, Discard, Frame };

  SerialEvent finishLine();
  SerialEvent beginFrame(const std::string& header);
  SerialEvent fail(const char* code, const char* message);

  std::size_t maxLine_;
  std::size_t maxFrame_ = 0;
  Mode mode_ = Mode::Line;
  std::string line_;
  std::string topic_;
  std::string body_;
  long seq_ = -1;
  std::size_t want_ = 0;
  std::vector<uint8_t> frame_;
  const char* error_ = "";
  const char* errorMessage_ = "";
};

}
}
