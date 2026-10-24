// AI生成
/*
 * effect_eval.h — 图层效果的像素求值（对齐上游 LayerEffectsRenderer）。
 *
 * 背景（与 adjustment_eval 同源的问题）：
 *   调整图层此前只入库不求值，2026-10-16 补齐；效果（effects）此前**同样**只入库
 *   不求值——`comp_layer_set_effect` 写入成功、`engineEffects` 往返正常，但合成路径
 *   完全无视它们，且 `flatten` / `merge_down` 因「无法烘入」直接拒绝带效果的图层。
 *   于是宿主只能写不能看：挂上一个投影，图层渲染没有任何变化。
 *
 * 语义对齐上游（Compositor/Document/LayerEffects.swift:388 `render(_:mask:effects:)`）：
 *   效果画在**图层自身像素（经其蒙版裁剪后）**的周围与内部，上游的绘制次序是：
 *     1. Drop Shadow   —— 形状按 offset 平移、模糊后的覆盖率，填阴影色（画在像素下）；
 *     2. Outer Glow     —— 形状高斯柔化后**扣掉形状内部**，填发光色（画在像素下）；
 *     3. Stroke(outside)—— 形状按 size 方形扩张后**减去形状本身**的环，画在像素下；
 *     4. 图层自身像素；
 *     5. Color Overlay  —— 以 source-atop 叠色（保持像素 alpha）；
 *     6. Inner Glow     —— 形状柔化后取反、source-atop 叠色；
 *     7. Inner Shadow   —— 形状按 offset 平移、模糊后取反、source-atop 叠色；
 *     8. Stroke(inside) —— 同 3，但画在像素之上。
 *   次序不可交换：outside 效果必须垫在像素之下（否则被像素盖住），inside 效果必须
 *   压在其上（否则被像素盖住），叠色类效果用 source-atop（保持 alpha，半透明像素
 *   完全吃到颜色）。
 *
 * P1-1 追加的四种（斜面和浮雕 / 缎光 / 渐变叠加 / 图案叠加）都是 **inside 类**，
 * 全部以 source-atop 落在图层自身 alpha 之内，落笔次序在既有 over 组之后（它们只改
 * 颜色、不改 alpha，因此彼此与内发光/内阴影的先后不改变覆盖范围，只改变颜色叠加顺序）：
 *   9. Bevel & Emboss —— 形状柔化后沿光向量取前后向差，亮面/暗面分别叠色；
 *  10. Satin          —— 形状正反两个偏移副本之差的绝对值（或取反），柔化后叠色；
 *  11. Gradient Overlay —— 沿 angle 方向的线性双色斜坡，按源形状 bbox 归一化；
 *  12. Pattern Overlay —— 内置程序化图案（棋盘/网格/斜纹/圆点）按 scale/angle/offset
 *      平铺，用双色取色后 source-atop 叠色。图案没有资源文件，因此确定性可复现。
 * 四者的边界只由源形状决定，不会外扩，因此 effects_margin 对它们返回 0。
 *
 * 实现取舍（与上游的已知差异，见 README §6.5）：
 *   - 上游用 Core Image 的 CIColorMatrix + GPU 路径做模糊/重着色；本引擎复用
 *     adjustment_eval 已有的高斯模糊（同三趟滑动窗口盒式近似），语义等价、
 *     数值不逐字节等同上游（未做跨平台逐像素比对，见 AUDIT5 §7.7）。
 *   - 上游在图层**像素网格**上算效果，再整体贴到画布（`LayerEffectsRenderer.placed`
 *     用 inset 扩大 transform）；本引擎在**文档坐标**上算——效果与图层处于同一
 *     坐标系，天然对齐，避免了 inset 与 transform 的往返换算，也不需要为效果
 *     扩大画布表面。
 *
 * 参数一律「缺键回落默认 + 越界钳制」，理由同 adjustment_eval：
 *   engineEffects 是引擎自有扩展键，缺键/坏 JSON 是常态，不因此失败。
 */
#ifndef COMPOSITOR_COMPOSITING_EFFECT_EVAL_H
#define COMPOSITOR_COMPOSITING_EFFECT_EVAL_H

#include <string>

#include "domain/layer.h"

namespace compositor {

/* 效果枚举：与 .comp 的 engineEffects[].effect 一一对应。
 * 数值是 ABI 的一部分（ABI 尾部追加，不改既有取值）。 */
enum EffectKind : uint32_t {
  kEffectStroke = 0,        /* 描边：size / color / opacity / inside */
  kEffectDropShadow = 1,    /* 投影：angle / distance / blur / color / opacity */
  kEffectColorOverlay = 2,  /* 颜色叠加：color / opacity */
  kEffectInnerShadow = 3,   /* 内阴影：angle / distance / blur / color / opacity */
  kEffectOuterGlow = 4,     /* 外发光：size / color / opacity */
  kEffectInnerGlow = 5,     /* 内发光：size / color / opacity */
  /* P1-1 尾部追加：斜面和浮雕 / 缎光 / 渐变叠加。数值只增不改。 */
  kEffectBevel = 6,         /* 斜面和浮雕：angle / altitude / depth / size / soften / 双色 */
  kEffectSatin = 7,         /* 缎光：angle / distance / blur / invert / color / opacity */
  kEffectGradientOverlay = 8, /* 渐变叠加：angle / reverse / 双色 / opacity */
  /* P1-1 第二段尾部追加：图案叠加。数值只增不改。 */
  kEffectPatternOverlay = 9, /* 图案叠加：pattern / scale / angle / offset / 双色 / opacity */
  kEffectKindCount = 10,
};

struct EffectEvalResult {
  size_t applied = 0; /* 实际执行的效果个数 */
  size_t skipped = 0; /* 因 effect 未知 / 参数非法而跳过的个数 */
  bool changed = false; /* 是否真的改变了像素（默认参数下为 false） */
};

/*
 * 把一组效果**就地**应用到图层单元缓冲上（文档坐标、RGBA8 预乘）。
 *
 * buf / stride 必须描述整幅画布，坐标原点即文档原点。
 * 单个效果的失败计入 skipped，不视为整体失败；只有缓冲非法才返回 false。
 *
 * 注意：这是**纯像素**求值（不含混合模式、不透明度、剪贴）——那些由
 * blend_layer_into 在效果求值之后施加，与上游「先做效果、再当普通图层混合」
 * 的次序一致。
 */
bool apply_effects(const std::vector<Effect>& effects, uint8_t* buf, int32_t width,
                   int32_t height, size_t stride, EffectEvalResult* result,
                   std::string* error);

/*
 * 域层拒绝烘入「引擎不认识的 effect」时，错误文案必须含此字面量。
 *
 * 为什么需要一个共用标记：Document::merge_down / flatten_image 只返回
 * `bool + 自由文本`，不携带错误码；ABI 层（comp_layer_abi.cpp）要把它映射成
 * COMP_ERR_UNSUPPORTED 还是 COMP_ERR_STATE，此前只能拿错误文案做子串匹配。
 * 两边各写一份字符串就成了「靠巧合对齐」——早期版本两边各写一个 "effects"，
 * 文案一改（改成 "does not evaluate"）错误码就错报成 STATE：宿主拿到非
 * UNSUPPORTED 的错误、按「可重试」处理，于是无限重试一个永不成功的操作。
 *
 * 故把标记定义在此处（域层与 ABI 层都包含 effect_eval.h），单一来源。
 */
extern const char* const kUnevaluatedEffectMarker;

/*
 * 某 effect 是否有已落地的像素实现（供面板/文档如实汇报能力边界）。
 * 仅表示「引擎认识这个 effect 值」，不表示它在当前参数下可见
 * （例如 opacity 为 0 时任何 effect 都不可见）。 */
bool effect_kind_is_evaluated(uint32_t effect);

/*
 * 这组效果额外需要的文档空间，单位像素（对齐上游
 * `LayerEffectsRenderer.margin(for:)`）。
 *
 * outside 描边吃 size；投影吃 distance + blur*3；外发光吃 size*3；不足 1 的
 * 向上取整再加 2。inside 描边、叠色、内阴影、内发光、斜面和浮雕、缎光、渐变叠加、
 * 图案叠加都不外扩（它们只在形状内部）。
 *
 * 返回 0 表示无需外扩。图层包围盒必须按它外扩，否则 outside 效果会被 bounds 裁掉。
 */
int32_t effects_margin(const std::vector<Effect>& effects);

}  // namespace compositor

#endif /* COMPOSITOR_COMPOSITING_EFFECT_EVAL_H */
