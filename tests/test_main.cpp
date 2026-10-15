// AI生成
#include "test_framework.h"

namespace comptest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

int failures = 0;

void fail(const char* file, int line, const std::string& message) {
  ++failures;
  std::printf("       failure: %s  (%s:%d)\n", message.c_str(), file, line);
}

}  // namespace comptest

int main() {
  /* 关闭 stdout 缓冲：用例若崩溃（访问违例）也要能看到崩在哪个用例。 */
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  int failed_tests = 0;
  const std::vector<comptest::TestCase>& cases = comptest::registry();

  std::printf("compositor-ng engine tests: %zu cases\n\n", cases.size());

  for (const comptest::TestCase& test : cases) {
    const int before = comptest::failures;
    std::printf("[ RUN  ] %s\n", test.name);
    test.fn();
    if (comptest::failures == before) {
      std::printf("[  OK  ] %s\n", test.name);
    } else {
      std::printf("[ FAIL ] %s\n", test.name);
      ++failed_tests;
    }
  }

  std::printf("\nsummary: %zu cases, %d failed, %d check failures\n", cases.size(),
              failed_tests, comptest::failures);
  return failed_tests == 0 ? 0 : 1;
}
