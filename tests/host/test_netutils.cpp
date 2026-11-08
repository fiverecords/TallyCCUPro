// NetUtils: LineReader, readLine(), drainInput(), deadlines, ClientBufferPrint
// (compose mode), with a scripted EthernetClient.
#include "test.h"
#include "mock_client.h"
#include "NetUtils.h"

// millis() is unsigned long (64-bit on the host) but wraps at 32 bits, as on AVR
static uint32_t since(uint32_t t0) { return (uint32_t)millis() - t0; }

TEST(linereader_line_split_across_segments) {
  MockClient c;
  c.add("SUBSCR");
  c.addLater("IBE CC");
  c.addLater("U\r\nPING\n");
  char buf[32];
  NetUtils::LineReader lr(buf, sizeof buf);
  CHECK_INT(lr.poll(c, 256), -1);  // partial line is kept
  CHECK_STR(buf, "SUBSCR");
  CHECK_INT(lr.poll(c, 256), -1);  // nothing new
  c.arrive();
  CHECK_INT(lr.poll(c, 256), -1);
  CHECK_STR(buf, "SUBSCRIBE CC");
  c.arrive();
  CHECK_INT(lr.poll(c, 256), 13);
  CHECK_STR(buf, "SUBSCRIBE CCU");  // \r dropped
  CHECK(!lr.truncated());
  CHECK_INT(lr.poll(c, 256), 4);    // next poll starts a new line
  CHECK_STR(buf, "PING");
  CHECK_INT(lr.poll(c, 256), -1);   // nothing left
  CHECK_INT(lr.length(), 0);
}

TEST(linereader_max_bytes_per_poll) {
  MockClient c;
  c.add("ABCDEFGH\n");
  char buf[32];
  NetUtils::LineReader lr(buf, sizeof buf);
  CHECK_INT(lr.poll(c, 3), -1);
  CHECK_INT(c.reads(), 3);
  CHECK_STR(buf, "ABC");
  CHECK_INT(lr.poll(c, 3), -1);
  CHECK_INT(lr.poll(c, 3), 8);
  CHECK_STR(buf, "ABCDEFGH");
}

TEST(linereader_truncates_long_line) {
  MockClient c;
  c.add("0123456789ABCDEF\nNEXT\n");
  char buf[8];
  NetUtils::LineReader lr(buf, sizeof buf);
  CHECK_INT(lr.poll(c, 256), 7);  // prefix kept, rest consumed to '\n'
  CHECK_STR(buf, "0123456");
  CHECK(lr.truncated());
  CHECK_INT(lr.poll(c, 256), 4);
  CHECK_STR(buf, "NEXT");
  CHECK(!lr.truncated());
}

TEST(linereader_empty_and_cr_only_lines) {
  MockClient c;
  c.add("\r\n\n\rA\r\r\n");
  char buf[8];
  NetUtils::LineReader lr(buf, sizeof buf);
  CHECK_INT(lr.poll(c, 256), 0);
  CHECK_INT(lr.poll(c, 256), 0);
  CHECK_INT(lr.poll(c, 256), 1);
  CHECK_STR(buf, "A");
}

TEST(linereader_content_length_ends_line) {
  MockClient c;
  c.add("a=1&b=2EXTRA");
  char buf[32];
  long remaining = 7;
  NetUtils::LineReader lr(buf, sizeof buf);
  CHECK_INT(lr.poll(c, 256, &remaining), 7);  // no '\n': the body ended
  CHECK_STR(buf, "a=1&b=2");
  CHECK_INT(remaining, 0);
  CHECK_INT(lr.poll(c, 256, &remaining), -1);  // never reads past the body
  CHECK_INT((long)c.left(), 5);
}

TEST(linereader_reset_and_tiny_buffer) {
  MockClient c;
  c.add("XYZ");
  char buf[1];
  NetUtils::LineReader lr(buf, sizeof buf);
  CHECK_INT(lr.poll(c, 256), -1);
  CHECK_STR(buf, "");
  CHECK(lr.truncated());
  lr.reset();
  CHECK(!lr.truncated());
  CHECK_INT(lr.length(), 0);
}

TEST(readline_complete_and_truncated) {
  MockClient c;
  c.add("GET /?listPresets HTTP/1.1\r\n");
  c.add("Host: t\r\n");
  char buf[16];
  bool cut = false;
  int n = NetUtils::readLine(c, buf, sizeof buf, NetUtils::deadlineIn(500), &cut);
  CHECK_INT(n, 15);
  CHECK(cut);
  CHECK_STR(buf, "GET /?listPrese");
  n = NetUtils::readLine(c, buf, sizeof buf, NetUtils::deadlineIn(500), &cut);
  CHECK_INT(n, 7);
  CHECK(!cut);
  CHECK_STR(buf, "Host: t");
}

TEST(readline_deadline_and_disconnect) {
  MockClient c;
  c.add("partial");
  char buf[16];
  bool cut = false;
  uint32_t t0 = millis();
  CHECK_INT(NetUtils::readLine(c, buf, sizeof buf, NetUtils::deadlineIn(60), &cut), -1);
  uint32_t dt = since(t0);
  CHECK(dt >= 59 && dt < 1000);  // waited for the deadline, not forever

  MockClient d;
  d.add("tail");
  d.setConnected(false);
  t0 = millis();
  CHECK_INT(NetUtils::readLine(d, buf, sizeof buf, NetUtils::deadlineIn(5000), &cut), -1);
  CHECK(since(t0) < 1000);  // a closed peer ends it at once
}

TEST(readline_remaining_already_zero) {
  MockClient c;
  c.add("data\n");
  char buf[16];
  long remaining = 0;
  CHECK_INT(NetUtils::readLine(c, buf, sizeof buf, NetUtils::deadlineIn(100), NULL, &remaining), -1);
  CHECK_INT((long)c.left(), 5);
}

TEST(drain_input_limits) {
  MockClient c;
  c.add("0123456789");
  c.addLater("abcdefghij");
  CHECK_INT(NetUtils::drainInput(c, 100, 0), 10);  // 0 ms: what has arrived
  CHECK_INT((long)c.left(), 10);
  c.arrive();
  c.setConnected(false);
  CHECK_INT(NetUtils::drainInput(c, 4, 100), 4);   // byte limit
  CHECK_INT((long)c.left(), 6);
  CHECK_INT(NetUtils::drainInput(c, 100, 100), 6); // closed peer, nothing left
  CHECK_INT(NetUtils::drainInput(c, 100, 0), 0);
}

TEST(deadline_expired_wraps) {
  // Run also with EMU_MILLIS_OFFSET close to 2^32 (tests/host/run.sh does).
  unsigned long d = NetUtils::deadlineIn(40);
  CHECK(!NetUtils::expired(d));
  CHECK(NetUtils::expired(NetUtils::deadlineIn(0)));
  CHECK(NetUtils::expired((unsigned long)((uint32_t)millis() - 1)));
  uint32_t t0 = millis();
  while (!NetUtils::expired(d) && since(t0) < 2000) delay(1);
  CHECK(NetUtils::expired(d));
  CHECK(since(t0) < 1000);
}

TEST(deadline_across_millis_wrap) {
  // Only meaningful with millis() near 2^32 (run.sh's second run)
  uint32_t toWrap = (uint32_t)0 - (uint32_t)millis();
  if (toWrap > 3000 || toWrap < 30) {
    printf("    (skipped: %lu ms to the millis() wrap)\n", (unsigned long)toWrap);
    return;
  }
  while ((uint32_t)0 - (uint32_t)millis() > 15) delay(1);
  unsigned long d = NetUtils::deadlineIn(40);
  CHECK(d < 100);  // the deadline itself has wrapped
  CHECK(!NetUtils::expired(d));
  uint32_t t0 = millis();
  while (!NetUtils::expired(d) && since(t0) < 2000) delay(1);
  CHECK((uint32_t)millis() < 1000);  // millis() wrapped meanwhile
  CHECK(since(t0) >= 39 && since(t0) < 500);
}

TEST(client_buffer_print_compose) {
  uint8_t buf[16];
  ClientBufferPrint p(buf, sizeof buf);
  p.print(F("CCU "));
  p.print(3);
  p.print(F(" gain_db 6"));
  CHECK(!p.failed());
  CHECK_INT(p.length(), 15);
  CHECK(memcmp(p.data(), "CCU 3 gain_db 6", 15) == 0);
  p.print(F("0123456789"));  // does not fit
  CHECK(p.failed());
  CHECK_INT(p.length(), 16);
  p.clear();                  // compose mode: reusable
  CHECK(!p.failed());
  CHECK_INT(p.length(), 0);
  p.print(F("PONG"));
  CHECK_INT(p.length(), 4);
}
