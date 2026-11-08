// WebServer::printJsonSafe(): JSON string contents.
#include "test.h"
#include "NetUtils.h"
#include "webserver.h"

static const char* json(const char* s, size_t len = (size_t)-1) {
  static uint8_t buf[128];
  ClientBufferPrint p(buf, sizeof buf - 1);
  if (len == (size_t)-1) WebServer::printJsonSafe(p, s);
  else WebServer::printJsonSafe(p, s, len);
  buf[p.length()] = '\0';
  return (const char*)buf;
}

TEST(json_escapes_quotes_and_backslash) {
  CHECK_STR(json("Plano general"), "Plano general");
  CHECK_STR(json("a\"b"), "a\\\"b");
  CHECK_STR(json("C:\\x"), "C:\\\\x");
  CHECK_STR(json("Escena \xc3\xb1" "and\xc3\xba"), "Escena \xc3\xb1" "and\xc3\xba");  // UTF-8 as is
  CHECK_STR(json(NULL), "");
}

TEST(json_escapes_control_chars) {
  // HTTP-10: a stored name must never break the JSON
  CHECK_STR(json("a\nb\rc\td"), "a\\nb\\rc\\td");
  CHECK_STR(json("\x01\x1f"), "\\u0001\\u001f");
  CHECK_STR(json("ab\0cd", 5), "ab\\u0000cd");
  CHECK_STR(json("abcdef", 3), "abc");
}
