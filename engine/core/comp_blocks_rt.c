// AI生成
/*
 * comp_blocks_rt.c — 非 Apple 平台的最小 BlocksRuntime。
 *
 * 背景：
 *   Compositor 的 C 像素内核（engine/imaging 目录下的 .c）沿用原 macOS 工程写法，
 *   使用 clang block 语法（^）作为“区间回调”，例如 AdjustPixels.c 的
 *   in_bands(count, ^(size_t first, size_t last){ ... })。
 *   Apple 平台的 BlocksRuntime 由 libSystem 提供；Windows / HarmonyOS 没有，
 *   因此这里补齐链接所需的最小符号集。
 *
 * M1 语义边界（必须知悉）：
 *   本实现假定 block 只在**创建它的栈帧内同步执行**，绝不逃逸、绝不跨线程持有。
 *   M1 已把内核并行改成顺序执行（原 dispatch_apply 站点顺序化），该前提成立：
 *     - _Block_copy  直接返回原指针（不做堆拷贝）；
 *     - _Block_release 空操作；
 *     - _Block_object_assign / _Block_object_dispose 空操作（__block 标量无需 RC）。
 *   若 M2 恢复并行、或让 block 逃逸出栈帧，必须换成真正的 BlocksRuntime
 *   （libBlocksRuntime / compiler-rt），否则将出现悬垂指针。
 *
 * 编译：由 scripts/build.py 以 -fblocks 编译 imaging 目录并链接本文件；
 *       MSVC cl.exe 不支持 block 语法，Windows 侧统一使用 clang-cl。
 */

#if !defined(__APPLE__)

#include <stdio.h>
#include <stdlib.h>

/* BlocksRuntime 的字段标志位（与 compiler-rt / libBlocksRuntime 一致）。 */
#define COMP_BLOCK_FIELD_IS_OBJECT 3
#define COMP_BLOCK_FIELD_IS_BLOCK  7
#define COMP_BLOCK_FIELD_IS_BYREF  8

/* block 字面量的 isa 需要指向这三个全局对象；数组大小与真实布局无关，
 * 只要求是有效地址（clang 仅取地址做类型标记）。 */
void* _NSConcreteStackBlock[32];
void* _NSConcreteGlobalBlock[32];
void* _NSConcreteMallocBlock[32];

void* _Block_copy(const void* block) { return (void*)block; }

void _Block_release(const void* block) { (void)block; }

/*
 * P2-8：本最小实现只允许「栈内同步执行、无逃逸」的 block。
 * _Block_object_assign 只会在 block 被**堆拷贝**（即逃逸）且带 __block/对象捕获时被调用；
 * 此时继续静默 no-op 会直接产出悬垂指针。宁可当场硬失败把前提破坏暴露出来，
 * 也不要让它以内存错误的形式在别处随机发作。
 *
 * 【有效性边界（N6 订正，务必知悉）：本闸在当前构建中**不可达**】
 *   - 仓内零 `_Block_copy` / `Block_copy` 调用点；
 *   - `_Block_copy`（见上方定义）是恒等实现，直接返回原指针，不做堆拷贝；
 *   - 而 `_Block_object_assign` 只由编译器生成的「块拷贝辅助函数」调用——恒等 `_Block_copy`
 *     永远不会走到那个辅助函数。
 *   因此真正的逃逸路径走的是恒等 `_Block_copy`，**既不会被本函数拦下、也仍是悬垂指针**。
 *   「本机全量用例 + 自检 + 两个 crosscheck 全绿」**不能**证明这道闸有效；
 *   该改动**不得记账为「风险已收口」**（保留价值仅在于把安全前提写成显式断言）。
 *   若将来确有 block 逃逸需求，必须换成真正的 BlocksRuntime（libBlocksRuntime / compiler-rt）。
 */
void _Block_object_assign(void* dest, const void* object, const int flags) {
  if ((flags & COMP_BLOCK_FIELD_IS_OBJECT) != 0 || (flags & COMP_BLOCK_FIELD_IS_BLOCK) != 0 ||
      (flags & COMP_BLOCK_FIELD_IS_BYREF) != 0) {
    fprintf(stderr,
            "comp_blocks_rt: block escaping with a captured object/byref is unsupported\n");
    fflush(stderr);
    abort();
  }
  (void)dest;
  (void)object;
}

void _Block_object_dispose(const void* object, const int flags) {
  (void)object;
  (void)flags;
}

#endif /* !__APPLE__ */
