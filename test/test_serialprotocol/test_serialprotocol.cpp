#include <unity.h>

#include <string>
#include <vector>

#include "core/api/SerialProtocol.h"

using namespace awtrix;

void setUp() {}
void tearDown() {}

namespace {

int ev(api::SerialEvent e) { return static_cast<int>(e); }

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

std::string header(const char* kind, int seq, std::size_t len) {
  return std::string("!") + kind + std::to_string(seq) + ":" + std::to_string(len) + "\n";
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

void test_full_frame_payload_is_read_by_count() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::None), ev(feed(p, header("F", 3, 6))));
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

void test_delta_frame_uses_five_byte_records() {
  api::SerialProtocol p;
  p.setMaxFrame(1280);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::None), ev(feed(p, header("D", 1, 10))));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Delta), ev(feed(p, std::string(10, '\x7F'))));
  TEST_ASSERT_EQUAL_UINT32(10, p.frame().size());
}

void test_frame_lengths_that_do_not_divide_into_pixels_are_rejected() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header("F", 0, 7))));
  TEST_ASSERT_FALSE(p.awaitingFrame());
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header("D", 0, 12))));
  TEST_ASSERT_FALSE(p.awaitingFrame());
}

void test_frame_over_the_ceiling_is_rejected_without_reserving() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header("F", 0, 900))));
  TEST_ASSERT_EQUAL_STRING("payloadTooLarge", p.error());
  TEST_ASSERT_FALSE(p.awaitingFrame());

  // A zero-length frame is a malformed header, not an empty update.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header("F", 0, 0))));
}

void test_malformed_frame_headers_are_rejected() {
  api::SerialProtocol p;
  p.setMaxFrame(768);
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "!F768\n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "!F1:\n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "!Fx:768\n")));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, "!F1:76a\n")));
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
  feed(p, header("F", 5, 6));
  feed(p, std::string(3, '\x11'));
  TEST_ASSERT_TRUE(p.awaitingFrame());
  TEST_ASSERT_TRUE(p.expire(500, 100, 200));
  TEST_ASSERT_FALSE(p.awaitingFrame());

  // The three orphaned payload bytes are gone, so the next frame is whole rather than shifted.
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::None), ev(feed(p, header("F", 6, 3))));
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Frame), ev(feed(p, std::string("\x01\x02\x03", 3))));
  TEST_ASSERT_EQUAL_UINT8(0x01, p.frame()[0]);
}

void test_frames_are_refused_before_a_canvas_size_is_known() {
  api::SerialProtocol p;
  TEST_ASSERT_EQUAL_INT(ev(api::SerialEvent::Error), ev(feed(p, header("F", 0, 3))));
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
  RUN_TEST(test_full_frame_payload_is_read_by_count);
  RUN_TEST(test_delta_frame_uses_five_byte_records);
  RUN_TEST(test_frame_lengths_that_do_not_divide_into_pixels_are_rejected);
  RUN_TEST(test_frame_over_the_ceiling_is_rejected_without_reserving);
  RUN_TEST(test_malformed_frame_headers_are_rejected);
  RUN_TEST(test_idle_timeout_drops_a_partial_line);
  RUN_TEST(test_idle_timeout_drops_a_partial_frame);
  RUN_TEST(test_frames_are_refused_before_a_canvas_size_is_known);
  return UNITY_END();
}
