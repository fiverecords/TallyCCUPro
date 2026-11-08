// test.h - minimal unit test helpers for the host tests (no STL: the
// Arduino shim's min/max macros clash with the standard C++ headers).
#ifndef HOST_TEST_H
#define HOST_TEST_H

#include <stdio.h>
#include <string.h>
#include <math.h>

struct TestCase {
  const char* name;
  void (*fn)();
  TestCase* next;
};

void testRegister(TestCase* t);
extern int g_testChecks;
extern int g_testFailures;

struct TestRegistrar {
  explicit TestRegistrar(TestCase* t) { testRegister(t); }
};

#define TEST(name)                                         \
  static void test_##name();                               \
  static TestCase testcase_##name = {#name, test_##name, 0}; \
  static TestRegistrar testreg_##name(&testcase_##name);   \
  static void test_##name()

#define TEST_FAIL_AT(fmt, ...)                                              \
  do {                                                                      \
    g_testFailures++;                                                       \
    printf("    FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);   \
  } while (0)

#define CHECK(cond)                                  \
  do {                                               \
    g_testChecks++;                                  \
    if (!(cond)) TEST_FAIL_AT("%s", #cond);          \
  } while (0)

#define CHECK_INT(actual, expected)                                          \
  do {                                                                       \
    g_testChecks++;                                                          \
    long long a_ = (long long)(actual), e_ = (long long)(expected);          \
    if (a_ != e_) TEST_FAIL_AT("%s == %lld, expected %lld", #actual, a_, e_); \
  } while (0)

#define CHECK_NEAR(actual, expected, eps)                                         \
  do {                                                                            \
    g_testChecks++;                                                               \
    double a_ = (double)(actual), e_ = (double)(expected);                        \
    if (!(fabs(a_ - e_) <= (eps))) TEST_FAIL_AT("%s == %.9g, expected %.9g", #actual, a_, e_); \
  } while (0)

#define CHECK_STR(actual, expected)                                               \
  do {                                                                            \
    g_testChecks++;                                                               \
    const char *a_ = (actual), *e_ = (expected);                                  \
    if (!a_ || strcmp(a_, e_) != 0) TEST_FAIL_AT("%s == \"%s\", expected \"%s\"", #actual, a_ ? a_ : "(null)", e_); \
  } while (0)

#endif  // HOST_TEST_H
