// AI生成
/*
 * comp_parallel.h — 跨平台并行原语
 *
 * 设计目标：为纯 C 像素内核与 C++ 引擎核提供统一的区间并行原语，
 * 取代原工程的 Apple GCD（dispatch_apply + block）。
 *
 * 约束：
 *   1. 只暴露 C 接口与函数指针，不引入 C++ 类型，保证 .c 内核可直接包含；
 *   2. 线程数可全局配置（0 = 按 CPU 核数自动），便于确定性回归测试；
 *   3. 调度失败时退化为串行执行，不抛异常、不返回错误（内核无需感知）；
 *   4. 回调抛出的异常在此处终止、不越过 C 边界，改由 comp_parallel_last_error()
 *      查询文本诊断（回调本身应尽量不抛，此处只是兜底）。
 */
#ifndef COMP_PARALLEL_H
#define COMP_PARALLEL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 区间回调。调用方保证 begin < end，且各 (begin, end) 区间互不重叠、
 * 并集恰好覆盖 [0, count)。ctx 由调用方提供，须保证回调内部只写入
 * 互不相交的内存区域（像素内核对“行带”写操作天然满足）。
 */
typedef void (*comp_range_fn)(void* ctx, size_t begin, size_t end);

/* 把 [0, count) 切分为至多 comp_parallel_threads() 个区间并行执行 body。 */
void comp_parallel_ranges(size_t count, comp_range_fn body, void* ctx);

/*
 * 与 comp_parallel_ranges 相同，但由调用方显式指定线程数（workers 会被裁剪到 [1, count]）。
 * 用途：固定切分做回归对比（例如 threads=1 与 threads=N 的结果必须逐字节一致）。
 */
void comp_parallel_ranges_n(size_t count, comp_range_fn body, void* ctx, size_t threads);

/* 设置全局工作线程数；0 = 自动（CPU 核数）。返回实际生效值。 */
size_t comp_parallel_set_threads(size_t thread_count);

/* 查询当前生效线程数（自动模式返回 CPU 核数，最小为 1）。 */
size_t comp_parallel_threads(void);

/*
 * 图像内核 band 调度的线程数：与上面的全局线程数相互独立，默认 1（顺序执行）。
 *
 * 之所以独立：像素内核的 band 调度直接决定逐像素结果，默认必须与 M1 的顺序实现
 * 逐字节一致，才能让回归测试保持确定性；并行是显式开启的优化项，由等价性测试守住。
 * 0 表示按 CPU 核数自动。
 */
size_t comp_parallel_set_band_threads(size_t thread_count);

/* 查询 band 调度当前生效线程数（最小为 1）。 */
size_t comp_parallel_band_threads(void);

/*
 * 最近一次调度中回调抛出异常的可读描述；无失败时返回 NULL。
 *
 * 缓冲为线程局部：返回的指针在下一次本线程调用 comp_parallel_ranges[_n] 后失效，
 * 需要长期保留请自行拷贝。多个调用线程各自持有诊断，不会互相覆盖。
 */
const char* comp_parallel_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* COMP_PARALLEL_H */
