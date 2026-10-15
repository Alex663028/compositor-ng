// AI生成
/*
 * test_parallel.cpp — 区间并行调度与像素内核并行的等价性验证。
 *
 * M2 的第一条硬约束是「并行不得改变结果」：
 *   1) 调度器必须做到区间互不重叠、并集恰好覆盖 [0, count)；
 *   2) 像素内核在 band 线程数 = 1 与 = N 时必须输出逐字节一致；
 *   3) band 线程数默认为 1——这是回归测试保持确定性的前提。
 */
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/comp_parallel.h"
#include "imaging/comp_imaging.h"
#include "test_framework.h"

namespace {

struct Coverage {
  std::vector<std::atomic<int>> hits;
  explicit Coverage(size_t n) : hits(n) {
    for (size_t i = 0; i < n; ++i) hits[i].store(0, std::memory_order_relaxed);
  }
};

void cover_range(void* ctx, size_t begin, size_t end) {
  Coverage* cover = static_cast<Coverage*>(ctx);
  for (size_t i = begin; i < end; ++i) {
    cover->hits[i].fetch_add(1, std::memory_order_relaxed);
  }
}

/* 每个下标恰被访问一次（既无重叠也无遗漏）。 */
bool exactly_once(Coverage& cover) {
  for (size_t i = 0; i < cover.hits.size(); ++i) {
    if (cover.hits[i].load(std::memory_order_relaxed) != 1) return false;
  }
  return true;
}

std::vector<uint16_t> pattern16(size_t count) {
  std::vector<uint16_t> values(count);
  uint32_t state = 0x12345678u;
  for (size_t i = 0; i < count; ++i) {
    state = state * 1664525u + 1013904223u;
    values[i] = static_cast<uint16_t>((state >> 13) & 0xFFFFu);
  }
  return values;
}

/* 回调抛异常：契约要求异常不得越过 C 边界，改由 comp_parallel_last_error() 查询。 */
void throwing_range(void* ctx, size_t begin, size_t end) {
  (void)begin;
  (void)end;
  static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
  throw std::runtime_error("range callback failed");
}

}  // namespace

COMP_TEST(parallel_ranges_covers_exactly_once) {
  const size_t counts[] = {1, 2, 7, 64, 999, 1024};
  const size_t threads[] = {1, 2, 3, 8, 64};
  for (size_t count : counts) {
    for (size_t workers : threads) {
      Coverage cover(count);
      comp_parallel_ranges_n(count, cover_range, &cover, workers);
      COMP_CHECK_MSG(exactly_once(cover), "each index must be covered exactly once");
    }
  }
}

COMP_TEST(parallel_ranges_degenerate_inputs_are_noops) {
  Coverage cover(4);
  comp_parallel_ranges_n(0, cover_range, &cover, 4);
  comp_parallel_ranges_n(4, nullptr, &cover, 4);
  for (size_t i = 0; i < cover.hits.size(); ++i) {
    COMP_CHECK(cover.hits[i].load(std::memory_order_relaxed) == 0);
  }
}

COMP_TEST(parallel_callback_exception_is_contained) {
  std::atomic<int> calls{0};

  /*
   * 单线程与多线程两条路径都必须兜住异常：一旦异常越过 C 边界，
   * std::thread 的栈展开会让测试进程直接 terminate，而不是报断言失败。
   */
  comp_parallel_ranges_n(64, throwing_range, &calls, 1);
  COMP_CHECK(calls.load() == 1);
  const char* diagnostic = comp_parallel_last_error();
  COMP_CHECK_MSG(diagnostic != nullptr, "contained exception must be queryable");
  COMP_CHECK_MSG(diagnostic != nullptr &&
                     std::string(diagnostic).find("range callback failed") != std::string::npos,
                 "diagnostic carries the callback's message");

  /* 一个区间失败不影响其余区间继续执行；重复多轮以覆盖多线程并发写入诊断的竞态。 */
  for (int repeat = 0; repeat < 8; ++repeat) {
    calls.store(0);
    comp_parallel_ranges_n(64, throwing_range, &calls, 8);
    COMP_CHECK_MSG(calls.load() == 8, "every range still runs after a throw");
  }
  COMP_CHECK_MSG(comp_parallel_last_error() != nullptr, "diagnostic survives the parallel path");

  /* 成功调度清空上一条诊断，避免把历史失败当成本次结果。 */
  Coverage cover(16);
  comp_parallel_ranges_n(16, cover_range, &cover, 4);
  COMP_CHECK(exactly_once(cover));
  COMP_CHECK_MSG(comp_parallel_last_error() == nullptr, "clean dispatch clears the diagnostic");
}

COMP_TEST(parallel_band_threads_defaults_to_sequential) {
  /* 默认 1：像素内核默认走顺序路径，回归测试才具备确定性。 */
  COMP_CHECK(comp_parallel_band_threads() == 1);
  COMP_CHECK(comp_parallel_set_band_threads(1) == 1);
}

COMP_TEST(parallel_kernel_quantize16_matches_sequential) {
  const size_t width = 64;
  const size_t height = 64;
  const size_t stride = width * 4;
  const std::vector<uint16_t> wide = pattern16(width * height * 4);

  comp_parallel_set_band_threads(1);
  std::vector<uint8_t> sequential(width * height * 4, 0);
  dither_quantize16(wide.data(), sequential.data(), width, height, stride);

  comp_parallel_set_band_threads(4);
  std::vector<uint8_t> parallel(width * height * 4, 0);
  dither_quantize16(wide.data(), parallel.data(), width, height, stride);

  comp_parallel_set_band_threads(1);
  COMP_CHECK_MSG(std::memcmp(sequential.data(), parallel.data(), sequential.size()) == 0,
                 "dither_quantize16 must be identical for threads=1 and threads=4");
}

COMP_TEST(parallel_kernel_glow_matches_sequential) {
  const size_t width = 64;
  const size_t height = 64;
  const size_t stride = width * 4;

  std::vector<uint8_t> base(width * height * 4);
  for (size_t i = 0; i < base.size(); ++i) {
    base[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
  }
  /* glow 与 rgba 同为 4 字节/像素、按 stride 索引的缓冲（内核用 `glow + y*stride`）。 */
  std::vector<uint8_t> glow(width * height * 4);
  for (size_t i = 0; i < glow.size(); ++i) {
    glow[i] = static_cast<uint8_t>((i * 17) & 0xFF);
  }

  comp_parallel_set_band_threads(1);
  std::vector<uint8_t> sequential = base;
  dither_glow(sequential.data(), glow.data(), width, height, stride, 0.75f);

  comp_parallel_set_band_threads(4);
  std::vector<uint8_t> parallel = base;
  dither_glow(parallel.data(), glow.data(), width, height, stride, 0.75f);

  comp_parallel_set_band_threads(1);
  COMP_CHECK_MSG(std::memcmp(sequential.data(), parallel.data(), sequential.size()) == 0,
                 "dither_glow must be identical for threads=1 and threads=4");
}
