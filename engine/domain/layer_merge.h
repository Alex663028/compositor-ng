// AI生成
/*
 * layer_merge.h — 图层合并的内容域 helper（对齐上游 LayerMerge.swift 的 ⌘E 家族）。
 *
 * 语义基准（上游 mergeLayers）：被合并的图层按「画布呈现」合成为一个像素层——
 * 混合模式、不透明度、蒙版、剪贴蒙版、变换全部烘入；结果裁剪到内容并落在
 * 锚点槽位；整次合并记为历史里的一步。
 *
 * 当前边界（与 compositing/compositor.h 的既有声明一致）：调整图层与图层效果
 * 不参与像素求值，因此携带调整/效果的合并在求值落地前一律明确拒绝
 * （ABI 层映射 COMP_ERR_UNSUPPORTED），不做「悄悄丢参数」或「假装烘入」。
 */
#ifndef COMPOSITOR_DOMAIN_LAYER_MERGE_H
#define COMPOSITOR_DOMAIN_LAYER_MERGE_H

#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "domain/layer.h"

namespace compositor {

class Document;

/*
 * 把 subset 中的图层按画布呈现合成为画布尺寸 RGBA8。
 *
 * subset 即「被合并图层」本身（组合并时含组）：组的缩放烘入结果层，结果层
 * 落到组的父级槽位。集外父引用在内部副本上剪断为根（合成器拒绝孤儿节点，
 * 见 filter_graph.cpp 的 visited 校验）；集外剪贴基底按无裁剪处理（与
 * composite_layer 的宽容一致）。像素资产经 shared_ptr 共享，本函数只读
 * 合成，不触发 copy-on-write。
 *
 * 渲染不变性（调用方保证）：被合并图层上方所有穿透组不透明度为 1 时，
 * 「子集合成 -> 结果层回填原槽位」与合并前逐字节一致（预乘 source-over
 * 结合律）；组合并时组的整棵贡献被结果层原位替代，同样不变。
 */
bool composite_merge_subset(const Document& doc, const std::vector<comp_layer_id>& subset,
                            ImageBuffer* out, std::string* error);

/*
 * 内容裁剪：按 alpha > 0 的包围盒裁剪（对齐上游 PixelFilter.trimmed）。
 * 全透明时返回 false 且不写出（调用方回退整幅，避免退化尺寸）。
 */
bool trim_to_content(const ImageBuffer& src, ImageBuffer* out, comp_rect* out_placement);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_LAYER_MERGE_H */
