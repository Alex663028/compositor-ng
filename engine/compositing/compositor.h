// AI生成
/*
 * compositor.h — 平台无关合成器（CPU 基准实现）。
 *
 * 职责：把文档的图层树按堆叠序合成为一张 RGBA8 画布。
 *
 * 求值路径（M2）：
 *   - 合成已升级为有向图求值（compositing/filter_graph.h）：先由图层树建图，再按拓扑序合成，
 *     图层树的嵌套与同级顺序完整参与求值；
 *   - 支持：可见性、不透明度、Normal 等全部 24 种混合模式、蒙版、翻转、剪贴蒙版近似；
 *   - 图层组默认「穿透（pass-through）」：子层直接叠加到组下方内容上（混合穿透），组的不透明度
 *     乘进每个子层；标记为隔离的组则先自合成一张缓冲，再作为整体与下层混合（详见 filter_graph.h）；
 *   - 调整图层与图层效果仅入库（参数模型与 ABI 已就位），像素求值归后续的滤镜节点；
 *   - 旋转/透视等仿射变形归 M2 的变形模块（当前支持平移与翻转）。
 */
#ifndef COMPOSITOR_COMPOSITING_COMPOSITOR_H
#define COMPOSITOR_COMPOSITING_COMPOSITOR_H

#include <string>

#include "compositor/comp_core.h"
#include "domain/document.h"
#include "domain/layer.h"

namespace compositor {

struct CompositeOptions {
  comp_rect region{0, 0, 0, 0};     /* w/h <= 0 表示整幅画布 */
  bool isolate_layer = false;       /* true 时仅合成 target_layer */
  comp_layer_id target_layer = 0;
};

/*
 * 合成表面的像素格式（C5）：文档里任一层是 half（COMP_PIXEL_RGBA16F）时，
 * 表面按 RGBA16F 求值，否则 RGBA8。
 *
 * 口径与上游一致——有 HDR 图层就在 half 表面上合成，没有就整条路仍是 8 位；
 * RGBA16（整数 16 位）文档维持既有的 8 位表面（那是 C5 之前记录在案的行为，
 * 不在本轮改口径，避免把 8 位验收底线牵动到 16 位整数路径）。
 */
comp_pixel_format composite_surface_format(const Document& doc);

/* 合成整篇文档；out 会被重置为画布尺寸、composite_surface_format 指定的深度。 */
bool composite_document(const Document& doc, ImageBuffer* out, std::string* error);

/*
 * 仅合成单个图层（含其剪贴基底），用于图层缩略图。
 * 内部按图层深度求值（half 图层在 half 表面上合成），输出始终是 RGBA8 的
 * 显示副本——缩略图的调用契约不变，高光只在这最后一步按白显示截断。
 */
bool composite_layer(const Document& doc, comp_layer_id id, ImageBuffer* out,
                     std::string* error);

/* 等比降采样到不超过 max_edge 的尺寸（双线性简化实现，M2 换 Lanczos）。 */
bool downsample_rgba8(const ImageBuffer& src, int32_t max_edge, ImageBuffer* out);

}  // namespace compositor

#endif /* COMPOSITOR_COMPOSITING_COMPOSITOR_H */
