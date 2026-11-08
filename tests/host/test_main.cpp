// test_main.cpp - runs every TEST() linked into the binary.
//   hosttests [substring]   only the tests whose name contains substring
#include "test.h"

int g_testChecks = 0;
int g_testFailures = 0;
static TestCase* s_head = 0;
static TestCase* s_tail = 0;

void testRegister(TestCase* t) {
  t->next = 0;
  if (s_tail) s_tail->next = t; else s_head = t;
  s_tail = t;
}

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : 0;
  int run = 0, failed = 0;
  for (TestCase* t = s_head; t; t = t->next) {
    if (filter && !strstr(t->name, filter)) continue;
    int before = g_testFailures;
    printf("[ RUN  ] %s\n", t->name);
    fflush(stdout);
    t->fn();
    run++;
    if (g_testFailures != before) {
      failed++;
      printf("[ FAIL ] %s\n", t->name);
    } else {
      printf("[  OK  ] %s\n", t->name);
    }
    fflush(stdout);
  }
  printf("%d tests, %d checks, %d failed checks, %d failed tests\n", run, g_testChecks, g_testFailures, failed);
  return (failed || run == 0) ? 1 : 0;
}
