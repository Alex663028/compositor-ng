// AI生成
#include "test_framework.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

int main(int argc, char** argv) {
  /* 关闭 stdout 缓冲：用例若崩溃（访问违例）也要能看到崩在哪个用例。 */
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  /*
   * 命令行（均可省略，无参数时行为与全量串行完全一致）：
   *   compositor_tests --list        只枚举用例名（供脚本 / 人工查）
   *   compositor_tests <substring>   只跑名字含子串的用例（单用例/单文件复跑）
   * 781 个用例全量约分钟级；调试一个用例不该付全量的代价。summary 行的
   * cases 计数相应只统计实际运行的用例——run_regression.py 解析的是同一行，
   * 全量口径不受影响。
   */
  const char* filter = nullptr;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--list") == 0) {
      list_only = true;
    } else if (argv[i][0] != '-') {
      filter = argv[i];
    }
  }

  int failed_tests = 0;
  const std::vector<comptest::TestCase>& cases = comptest::registry();

  if (list_only) {
    for (const comptest::TestCase& test : cases) {
      std::printf("%s\n", test.name);
    }
    return 0;
  }

  std::printf("compositor-ng engine tests: %zu cases\n\n", cases.size());
  if (filter != nullptr) {
    std::printf("filter: %s\n\n", filter);
  }

  size_t ran = 0;
  for (const comptest::TestCase& test : cases) {
    if (filter != nullptr && std::strstr(test.name, filter) == nullptr) continue;
    ++ran;
    const int before = comptest::failures;
    const auto started = std::chrono::steady_clock::now();
    std::printf("[ RUN  ] %s\n", test.name);
    test.fn();
    /* 单用例耗时：观测哪个用例拖慢全量回归（此前「781 例到底跑多久」无数据）。 */
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    if (comptest::failures == before) {
      std::printf("[  OK  ] %s (%.1f ms)\n", test.name, elapsed_ms);
    } else {
      std::printf("[ FAIL ] %s (%.1f ms)\n", test.name, elapsed_ms);
      ++failed_tests;
    }
  }

  std::printf("\nsummary: %zu cases, %d failed, %d check failures\n", ran, failed_tests,
              comptest::failures);
  return failed_tests == 0 ? 0 : 1;
}
