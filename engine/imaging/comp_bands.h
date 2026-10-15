// AI生成
/*
 * comp_bands.h — 像素内核的区间（band）调度。
 *
 * 背景：
 *   上游 macOS 工程的 C 内核用 Apple GCD（dispatch_apply + block）把一行/一带的处理
 *   切给多核（例如 AdjustPixels.c / DitherPixels.c 的 in_bands）。移植到
 *   Windows / HarmonyOS 后没有 GCD，M1 把它改成了顺序 for，本文件提供等价替代：
 *   把 block 体交给引擎核的区间调度器 comp_parallel_ranges_n 执行。
 *
 * 语义：与 GCD 版本一致——[0, count) 被切成互不重叠的区间，并集覆盖全区，全部执行完
 *   才返回（同步汇合）。各区间由调用方保证写互不相交的内存（像素内核按行/带切分天然满足）。
 *
 * block 生命周期（关键）：
 *   调度是**同步汇合**的——返回前已 join 全部工作线程，因此传入的 block 只需在调用者
 *   栈帧内有效，无需逃逸到堆，也就不需要 _Block_copy。故非 Apple 平台的
 *   engine/core/comp_blocks_rt.c 保持「栈内同步执行」的最小实现即可（见该文件说明）。
 *   将来若出现「异步持有 block」的需求，再升级为真正的 BlocksRuntime。
 *
 * 线程数：默认 1（顺序执行，与 M1 逐字节一致，保证回归确定性）；通过
 *   comp_parallel_set_band_threads(n) 显式开启并行（见 core/comp_parallel.h）。
 *
 * 用法（仅 .c 内核需要，因此本头按块语法声明）：
 *   static void in_bands(size_t count, void (^body)(size_t s, size_t e)) {
 *     comp_bands(count, body);
 *   }
 */
#ifndef COMPOSITOR_IMAGING_COMP_BANDS_H
#define COMPOSITOR_IMAGING_COMP_BANDS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把 [0, count) 切成若干区间交给 body 执行；返回时全部区间已执行完毕。 */
void comp_bands(size_t count, void (^body)(size_t start, size_t end));

#ifdef __cplusplus
}
#endif

#endif /* COMPOSITOR_IMAGING_COMP_BANDS_H */
