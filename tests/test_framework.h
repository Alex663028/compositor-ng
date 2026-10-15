// AI生成
/*
 * test_framework.h — 零依赖的极简测试框架。
 *
 * 不用 gtest：M1 的验证目标是「引擎可编译、可测」，引入第三方测试框架会
 * 给三端构建增加额外依赖。此处只依赖 C++ 标准库。
 */
#ifndef COMPOSITOR_TESTS_TEST_FRAMEWORK_H
#define COMPOSITOR_TESTS_TEST_FRAMEWORK_H

#include <cstdio>
#include <string>
#include <vector>

namespace comptest {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();

/* 全局失败计数（跨测试用例累加）。 */
extern int failures;

void fail(const char* file, int line, const std::string& message);

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

}  // namespace comptest

#define COMP_TEST(name)                                     \
  static void name();                                       \
  static ::comptest::Registrar comptest_registrar_##name(#name, name); \
  static void name()

#define COMP_CHECK(cond)                                                        \
  do {                                                                          \
    if (!(cond)) {                                                              \
      ::comptest::fail(__FILE__, __LINE__, std::string("expect: ") + #cond);    \
    }                                                                           \
  } while (0)

#define COMP_CHECK_MSG(cond, label)                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      ::comptest::fail(__FILE__, __LINE__, std::string(label) + " | " + #cond); \
    }                                                                           \
  } while (0)

#endif /* COMPOSITOR_TESTS_TEST_FRAMEWORK_H */
