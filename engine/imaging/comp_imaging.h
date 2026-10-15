// AI生成
/*
 * comp_imaging.h — 像素内核统一入口（C / C++ 均可用）。
 *
 * 背景：
 *   engine/imaging 下的 10 组像素内核是自原 macOS 工程移植的**纯 C** 代码，
 *   其原始头文件（LensPixels.h 等）没有 extern "C" 保护。若 C++ 源文件直接
 *   #include 它们，函数会被按 C++ 规则做名字修饰（例如
 *   `lens_distort(unsigned char const*, ...)`），与 .c 侧编译出的 C 符号
 *   `lens_distort` 不匹配，链接期报 undefined symbol。
 *
 * 用法：
 *   - C++ 源文件统一 #include "imaging/comp_imaging.h"，由本头负责 extern "C" 包裹；
 *   - C 源文件包含本头亦安全（#ifdef __cplusplus 使包裹失效）。
 *
 * 依赖：内核使用 clang block 语法，编译期需 -fblocks；非 Apple 平台的
 * BlocksRuntime 由 engine/core/comp_blocks_rt.c 提供（见该文件说明）。
 */

#ifndef COMPOSITOR_IMAGING_COMP_IMAGING_H
#define COMPOSITOR_IMAGING_COMP_IMAGING_H

#ifdef __cplusplus
extern "C" {
#endif

#include "imaging/AdjustPixels.h"
#include "imaging/BrushPixels.h"
#include "imaging/DeepPixels.h"
#include "imaging/DitherPixels.h"
#include "imaging/HealPixels.h"
#include "imaging/LensPixels.h"
#include "imaging/LevelsPixels.h"
#include "imaging/NoisePixels.h"
#include "imaging/PatchPixels.h"
#include "imaging/WandPixels.h"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_IMAGING_COMP_IMAGING_H */
