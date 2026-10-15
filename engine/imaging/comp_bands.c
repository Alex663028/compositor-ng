// AI生成
/*
 * comp_bands.c — band 调度实现（见 comp_bands.h）。
 *
 * 核心思路：引擎核的调度器（core/comp_parallel.h）只认「函数指针 + ctx」，而内核传入的是
 * block。这里用 ctx 携带**指向 block 变量的指针**：该变量位于 comp_bands 的栈帧上，
 * 而 comp_parallel_ranges_n 返回前已 join 全部线程，故工作线程读取它时它必然有效——
 * 这正是不需要 _Block_copy 的原因。
 */
#include "imaging/comp_bands.h"

#include "core/comp_parallel.h"

/* 从 ctx 取回 block 指针并调用。ctx 实为 `void (^)(size_t, size_t)` 的地址。 */
static void comp_bands_invoke(void *raw, size_t begin, size_t end) {
  void (^body)(size_t, size_t) = *(void (^(*))(size_t, size_t))raw;
  body(begin, end);
}

void comp_bands(size_t count, void (^body)(size_t start, size_t end)) {
  if (body == NULL || count == 0) return;
  comp_parallel_ranges_n(count, comp_bands_invoke, (void *)&body, comp_parallel_band_threads());
}
