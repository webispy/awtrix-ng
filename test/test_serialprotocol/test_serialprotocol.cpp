#include <unity.h>

#include <string>
#include <vector>

#include "core/api/SerialProtocol.h"

using namespace awtrix;

void setUp() {}
void tearDown() {}

namespace {

int ev(api::SerialEvent e) { return static_cast<int>(e); }
int ev2(api::FrameFault f) { return static_cast<int>(f); }

// Feeds a string one byte at a time and returns the last event that was not None, so a test reads
// as "what did this input produce" rather than as a loop.
api::SerialEvent feed(api::SerialProtocol& p, const std::string& in) {
  api::SerialEvent last = api::SerialEvent::None;
  for (char c : in) {
    const api::SerialEvent e = p.push(c);
    if (e != api::SerialEvent::None) last = e;
  }
  return last;
}

std::string header(int seq, std::size_t len) {
  return "!P" + std::to_string(seq) + ":" + std::to_string(len) + "\n";
}

// A payload with its checksum on the end, which is how every legal frame arrives.
std::string framed(std::vector<uint8_t> body) {
  const uint16_t crc = api::crc16(body.data(), body.size());
  body.push_back(static_cast<uint8_t>(crc & 0xff));
  body.push_back(static_cast<uint8_t>(crc >> 8));
  return std::string(body.begin(), body.end());
}

void test_command_line_splits_topic_and_body() {
  api::SerialProtocol p;
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command),
                        ev(feed(p, "cmd/notify {\"text\":\"hi\"}\n")));
  TEST_ASSERT_EQUAL_STRING("cmd/notify", p.topic().c_str());
  TEST_ASSERT_EQUAL_STRING("{\"text\":\"hi\"}", p.body().c_str());
  TEST_ASSERT_EQUAL_INT(-1, static_cast<int>(p.seq()));
}

void test_command_without_a_body_is_a_bare_topic() {
  api::SerialProtocol p;
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command), ev(feed(p, "cmd/apps/pushed/mac\n")));
  TEST_ASSERT_EQUAL_STRING("cmd/apps/pushed/mac", p.topic().c_str());
  TEST_ASSERT_EQUAL_STRING("", p.body().c_str());
}

void test_sequence_prefix_is_parsed_and_optional() {
  api::SerialProtocol p;
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command), ev(feed(p, "#7 cmd/notify {}\n")));
  TEST_ASSERT_EQUAL_INT(7, static_cast<int>(p.seq()));
  TEST_ASSERT_EQUAL_STRING("cmd/notify", p.topic().c_str());
  TEST_ASSERT_EQUAL_STRING("{}", p.body().c_str());

  // A sequence number carried by one line must not leak into the next.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command), ev(feed(p, "cmd/apps/next\n")));
  TEST_ASSERT_EQUAL_INT(-1, static_cast<int>(p.seq()));
}

void test_sequence_without_a_command_is_rejected() {
  api::SerialProtocol p;
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "#7\n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "#7 \n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "#x1 cmd/notify\n")));
}

void test_crlf_and_blank_lines_are_tolerated() {
  api::SerialProtocol p;
  // The blank line is the resync nudge a host writes right after connecting.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::None), ev(feed(p, "\n\n   \n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command), ev(feed(p, "cmd/apps/next\r\n")));
  TEST_ASSERT_EQUAL_STRING("cmd/apps/next", p.topic().c_str());
}

void test_byte_at_a_time_and_batched_arrival_agree() {
  const std::string wire = "cmd/notify {\"text\":\"a\"}\ncmd/apps/next\n";

  api::SerialProtocol one;
  std::vector<std::string> split;
  for (char c : wire)
    if (one.push(c) == api::SerialEvent::Command) split.push_back(one.topic());

  api::SerialProtocol batch;
  std::vector<std::string> whole;
  for (char c : wire)
    if (batch.push(c) == api::SerialEvent::Command) whole.push_back(batch.topic());

  TEST_ASSERT_EQUAL_UINT32(2, split.size());
  TEST_ASSERT_EQUAL_STRING("cmd/notify", split[0].c_str());
  TEST_ASSERT_EQUAL_STRING("cmd/apps/next", split[1].c_str());
  TEST_ASSERT_EQUAL_UINT32(split.size(), whole.size());
}

void test_oversize_line_errors_once_and_resynchronises() {
  api::SerialProtocol p(16);
  int errors = 0;
  for (char c : std::string(64, 'x'))
    if (p.push(c) == api::SerialEvent::Error) ++errors;
  TEST_ASSERT_EQUAL_INT(1, errors);
  TEST_ASSERT_EQUAL_STRING("payloadTooLarge", p.error());

  // The rest of the doomed line is swallowed, and the line after it parses normally.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::None), ev(feed(p, "more junk\n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command), ev(feed(p, "cmd/x\n")));
  TEST_ASSERT_EQUAL_STRING("cmd/x", p.topic().c_str());
}

void test_oversize_line_does_not_borrow_the_previous_sequence() {
  api::SerialProtocol p(16);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command), ev(feed(p, "#7 cmd/x\n")));
  TEST_ASSERT_EQUAL_INT(7, static_cast<int>(p.seq()));

  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, std::string(17, 'x'))));
  TEST_ASSERT_EQUAL_INT(-1, static_cast<int>(p.seq()));
}

void test_frame_payload_is_read_by_count() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::None), ev(feed(p, header(3, 6))));
  TEST_ASSERT_TRUE(p.awaitingFrame());
  TEST_ASSERT_EQUAL_UINT32(6, p.framePending());

  // Newline and carriage return are ordinary pixel data here, which is the whole reason the
  // payload is length-prefixed instead of delimited.
  const std::string payload("\xFF\x00\x0A\x0D\x20\x01", 6);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Frame), ev(feed(p, payload)));
  TEST_ASSERT_FALSE(p.awaitingFrame());
  TEST_ASSERT_EQUAL_UINT32(6, p.frame().size());
  TEST_ASSERT_EQUAL_UINT8(0x0A, p.frame()[2]);
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(p.seq()));

  // And the parser is back in line mode straight after the payload.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command), ev(feed(p, "cmd/apps/next\n")));
}

// Both ends compute this independently over every frame, so it is pinned against the published
// check value for CRC-16/CCITT-FALSE rather than against whatever this implementation happens to
// produce.
void test_checksum_matches_the_published_check_value() {
  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_UINT16(0x29b1, api::crc16(check, sizeof(check)));
  TEST_ASSERT_EQUAL_UINT16(0xffff, api::crc16(nullptr, 0));

  // Leading zeroes are part of the payload, not padding: a checksum that skipped them would pass a
  // frame whose first pixels were lost.
  const uint8_t zeroes[] = {0, 0, 0, 1};
  TEST_ASSERT_NOT_EQUAL(api::crc16(zeroes, sizeof(zeroes)), api::crc16(zeroes + 3, 1));
}

// The payload's own length is the parser's business only as far as "could this be a frame at all".
// What each layout means is checkFrame's, and is tested against a canvas below.
void test_frame_too_short_to_carry_a_layout_and_a_checksum_is_rejected() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header(0, 2))));
  TEST_ASSERT_FALSE(p.awaitingFrame());
  TEST_ASSERT_EQUAL_STRING("payloadTooLarge", p.error());

  // Three bytes is the hold frame: a layout, no pixels, and the checksum.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::None), ev(feed(p, header(0, 3))));
  TEST_ASSERT_TRUE(p.awaitingFrame());
}

void test_a_corrupt_frame_is_refused_rather_than_painted() {
  uint32_t canvas[4] = {0, 0, 0, 0};
  std::string frame = framed({0x00, 0xff, 0xff, 0x1f, 0x00, 0xe0, 0x07, 0x00, 0xf8});
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(frame.data());
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::None), ev2(api::checkFrame(bytes, frame.size(), 4)));

  // One bit of one colour, which is what a cable flips and what a length check cannot see.
  frame[3] = static_cast<char>(frame[3] ^ 0x01);
  bytes = reinterpret_cast<const uint8_t*>(frame.data());
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::Checksum),
                        ev2(api::checkFrame(bytes, frame.size(), 4)));
  TEST_ASSERT_EQUAL_UINT32(0, canvas[0]);
}

// Five-six-five in, eight-eight-eight out, with the high bits replicated: white has to survive the
// round trip or every full-brightness pixel on the panel is a shade off.
void test_layouts_paint_the_pixels_they_name() {
  uint32_t canvas[4] = {1, 2, 3, 4};
  const std::string full = framed({0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xf8, 0xe0, 0x07});
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(full.data());
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::None), ev2(api::checkFrame(bytes, full.size(), 4)));
  api::applyFrame(bytes, full.size(), 4, canvas);
  TEST_ASSERT_EQUAL_HEX32(0xffffff, canvas[0]);
  TEST_ASSERT_EQUAL_HEX32(0x000000, canvas[1]);
  TEST_ASSERT_EQUAL_HEX32(0xff0000, canvas[2]);
  TEST_ASSERT_EQUAL_HEX32(0x00ff00, canvas[3]);

  // An index and a colour: pixel 1 only, and everything else left where it was.
  const std::string indexed = framed({0x01, 0x01, 0x1f, 0x00});
  bytes = reinterpret_cast<const uint8_t*>(indexed.data());
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::None), ev2(api::checkFrame(bytes, indexed.size(), 4)));
  api::applyFrame(bytes, indexed.size(), 4, canvas);
  TEST_ASSERT_EQUAL_HEX32(0x0000ff, canvas[1]);
  TEST_ASSERT_EQUAL_HEX32(0xffffff, canvas[0]);

  // A bitmap of pixels 0 and 3, then their two colours in index order.
  const std::string masked = framed({0x02, 0x09, 0x00, 0xf8, 0x1f, 0x00});
  bytes = reinterpret_cast<const uint8_t*>(masked.data());
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::None), ev2(api::checkFrame(bytes, masked.size(), 4)));
  api::applyFrame(bytes, masked.size(), 4, canvas);
  TEST_ASSERT_EQUAL_HEX32(0xff0000, canvas[0]);
  TEST_ASSERT_EQUAL_HEX32(0x0000ff, canvas[3]);
  TEST_ASSERT_EQUAL_HEX32(0x0000ff, canvas[1]);

  // A hold frame: no pixels at all, which is what a still panel sends to keep the display.
  const std::string hold = framed({0x01});
  bytes = reinterpret_cast<const uint8_t*>(hold.data());
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::None), ev2(api::checkFrame(bytes, hold.size(), 4)));
  api::applyFrame(bytes, hold.size(), 4, canvas);
  TEST_ASSERT_EQUAL_HEX32(0xff0000, canvas[0]);
}

void test_lengths_that_no_layout_can_explain_are_refused() {
  auto fault = [](std::vector<uint8_t> body, std::size_t total) {
    const std::string f = framed(std::move(body));
    return api::checkFrame(reinterpret_cast<const uint8_t*>(f.data()), f.size(), total);
  };
  // A full frame is two bytes a pixel, exactly - one short would paint half a canvas.
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::Length), ev2(fault({0x00, 0xff, 0xff}, 4)));
  // Indexed records are three bytes each.
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::Length), ev2(fault({0x01, 0x01, 0x1f}, 4)));
  // A mask promising two colours and carrying one.
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::Length), ev2(fault({0x02, 0x09, 0x00, 0xf8}, 4)));
  // And a bitmap the canvas is too big for.
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::Length), ev2(fault({0x02}, 4)));
  TEST_ASSERT_EQUAL_INT(ev2(api::FrameFault::Layout), ev2(fault({0x07}, 4)));
}

void test_frame_over_the_ceiling_is_rejected_without_reserving() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header(0, 900))));
  TEST_ASSERT_EQUAL_STRING("payloadTooLarge", p.error());
  TEST_ASSERT_FALSE(p.awaitingFrame());

  // A zero-length frame is a malformed header, not an empty update.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header(0, 0))));
}

void test_malformed_frame_headers_are_rejected() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "!P768\n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "!P1:\n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "!Px:768\n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "!P1:76a\n")));
  TEST_ASSERT_FALSE(p.awaitingFrame());
}

// The cable-pulled-mid-command case: the fragment must not splice onto whatever arrives next.
void test_idle_timeout_drops_a_partial_line() {
  api::SerialProtocol p;
  feed(p, "cmd/noti");
  TEST_ASSERT_FALSE(p.expire(1000, 950, 200));   // still fresh
  TEST_ASSERT_TRUE(p.expire(1200, 950, 200));    // silent long enough
  TEST_ASSERT_FALSE(p.expire(2000, 950, 200));   // nothing left to drop

  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Command), ev(feed(p, "cmd/apps/next\n")));
  TEST_ASSERT_EQUAL_STRING("cmd/apps/next", p.topic().c_str());
}

void test_idle_timeout_drops_a_partial_frame() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  feed(p, header(5, 6));
  feed(p, std::string(3, '\x11'));
  TEST_ASSERT_TRUE(p.awaitingFrame());
  TEST_ASSERT_TRUE(p.expire(500, 100, 200));
  TEST_ASSERT_FALSE(p.awaitingFrame());

  // The three orphaned payload bytes are gone, so the next frame is whole rather than shifted.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::None), ev(feed(p, header(6, 3))));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Frame), ev(feed(p, std::string("\x01\x02\x03", 3))));
  TEST_ASSERT_EQUAL_UINT8(0x01, p.frame()[0]);
}

// The frame counter's contract, which neither end can verify alone. A receiver that compared
// against a hardcoded wrap point scored one phantom loss per cycle against any host that wrapped
// elsewhere - so what is pinned here is that *any* wrap point works.
void test_sequence_numbering_follows_or_wraps() {
  TEST_ASSERT_TRUE(api::seqFollows(41, 42));            // the ordinary case
  TEST_ASSERT_FALSE(api::seqFollows(41, 43));           // one frame lost
  TEST_ASSERT_FALSE(api::seqFollows(41, 141));          // a hundred lost
  TEST_ASSERT_TRUE(api::seqFollows(-1, 7));             // nothing counted yet is never a gap
  TEST_ASSERT_TRUE(api::seqFollows(41, 41));            // a repeat is not a gap either

  // Every wrap point a host might pick, including the recommended one, and one that is not.
  TEST_ASSERT_TRUE(api::seqFollows(api::kSeqWrap - 1, 0));
  TEST_ASSERT_TRUE(api::seqFollows(65535, 0));
  TEST_ASSERT_TRUE(api::seqFollows(255, 0));
  TEST_ASSERT_TRUE(api::seqFollows(999999, 1));
}

void test_frames_are_refused_before_a_canvas_size_is_known() {
  api::SerialProtocol p;
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header(0, 3))));
  TEST_ASSERT_FALSE(p.awaitingFrame());
}

}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_command_line_splits_topic_and_body);
  RUN_TEST(test_command_without_a_body_is_a_bare_topic);
  RUN_TEST(test_sequence_prefix_is_parsed_and_optional);
  RUN_TEST(test_sequence_without_a_command_is_rejected);
  RUN_TEST(test_crlf_and_blank_lines_are_tolerated);
  RUN_TEST(test_byte_at_a_time_and_batched_arrival_agree);
  RUN_TEST(test_oversize_line_errors_once_and_resynchronises);
  RUN_TEST(test_oversize_line_does_not_borrow_the_previous_sequence);
  RUN_TEST(test_frame_payload_is_read_by_count);
  RUN_TEST(test_checksum_matches_the_published_check_value);
  RUN_TEST(test_frame_too_short_to_carry_a_layout_and_a_checksum_is_rejected);
  RUN_TEST(test_a_corrupt_frame_is_refused_rather_than_painted);
  RUN_TEST(test_layouts_paint_the_pixels_they_name);
  RUN_TEST(test_lengths_that_no_layout_can_explain_are_refused);
  RUN_TEST(test_frame_over_the_ceiling_is_rejected_without_reserving);
  RUN_TEST(test_malformed_frame_headers_are_rejected);
  RUN_TEST(test_idle_timeout_drops_a_partial_line);
  RUN_TEST(test_idle_timeout_drops_a_partial_frame);
  RUN_TEST(test_frames_are_refused_before_a_canvas_size_is_known);
  RUN_TEST(test_sequence_numbering_follows_or_wraps);
  return UNITY_END();
}
