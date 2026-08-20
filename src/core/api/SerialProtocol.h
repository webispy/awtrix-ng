#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace awtrix {
namespace api {

// Framing for the USB serial control channel. Arduino-free on purpose: the byte-level state
// machine is where the awkward cases live (a cable pulled mid-line, a sender that outruns the
// reader), so it is unit-tested on the host.
//
//   [#<seq> ]<topic>[ <body>]\n
//
// Empty lines are ignored, which is what lets a sender write a bare newline to resynchronise
// after a reconnect.
enum class SerialEvent : uint8_t {
  None,
  Command,
  Error,
};

class SerialProtocol {
 public:
  static constexpr std::size_t kDefaultMaxLine = 512;

  explicit SerialProtocol(std::size_t maxLine = kDefaultMaxLine) : maxLine_(maxLine) {}

  SerialEvent push(char c);

  // Drops a half-received line once the line has been silent for idleMs. Without this a fragment
  // left behind by an unplugged cable would splice onto the next session's first bytes. Returns
  // true when something was thrown away.
  bool expire(int64_t nowMs, int64_t lastByteMs, int64_t idleMs);

  void reset();

  const std::string& topic() const { return topic_; }
  const std::string& body() const { return body_; }
  // -1 when the sender left the sequence number off.
  long seq() const { return seq_; }
  const char* error() const { return error_; }
  const char* errorMessage() const { return errorMessage_; }

 private:
  enum class Mode : uint8_t { Line, Discard };

  SerialEvent finishLine();
  SerialEvent fail(const char* code, const char* message);

  std::size_t maxLine_;
  Mode mode_ = Mode::Line;
  std::string line_;
  std::string topic_;
  std::string body_;
  long seq_ = -1;
  const char* error_ = "";
  const char* errorMessage_ = "";
};

}
}
