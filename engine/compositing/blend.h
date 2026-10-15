// AI生成
/*
 * blend.h — 平台无关混合模式（M2 «FilterGraph + 合成器» 的基准实现）。
 *
 * 背景：原 macOS 工程的混合模式散落三处（GPU 用 CoreImage 滤镜名、CPU 用
 * CGBlendMode + SeparableBlend 补偿、C 层无混合内核），不存在平台无关的合成核心。
 * 本文件按 W3C Compositing and Blending Level 1 规范统一实现全部 24 种模式，
 * 作为各渲染后端的一致性基准。
 */
#ifndef COMPOSITOR_COMPOSITING_BLEND_H
#define COMPOSITOR_COMPOSITING_BLEND_H

#include "compositor/comp_core.h"

namespace compositor {

/* 分离型混合的单通道运算；cb / cs 均为 0..1 的直通（非预乘）分量。 */
double blend_separable(comp_blend_mode mode, double cb, double cs);

/*
 * 完整混合：分离型按通道运算，非分离型（HUE / SATURATION / COLOR / LUMINOSITY）
 * 需要 RGB 三元组参与亮度/饱和度换算。
 * base / src / out 均为 0..1 直通 RGB。
 */
void blend_rgb(comp_blend_mode mode, const double base[3], const double src[3],
               double out[3]);

/* 是否为已实现的合法模式（枚举范围内）。 */
bool is_blend_supported(comp_blend_mode mode);

/*
 * .comp 清单中的规范字符串。取值逐字取自上游 LayerBlendMode 的 raw value
 * （如 "Color Burn"、"Linear Dodge (Add)"），顺序与 comp_blend_mode 数值一致。
 * 未知模式返回 nullptr。
 */
const char* blend_mode_manifest_name(comp_blend_mode mode);

/*
 * 解析清单字符串（大小写敏感的精确匹配）。name 允许非 '\0' 结尾，长度由 length 给出。
 * 成功写入 *out 并返回 true；未知名字或空指针返回 false 且不写 *out。
 */
bool blend_mode_from_manifest_name(const char* name, size_t length, comp_blend_mode* out);

}  // namespace compositor

#endif /* COMPOSITOR_COMPOSITING_BLEND_H */
