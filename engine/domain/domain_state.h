// AI生成
/*
 * domain_state.h — 可快照的文档状态。
 *
 * 撤销/重做在 M1 采用「整篇状态快照」实现：结构简单、语义绝对正确，
 * 代价是撤销栈内存占用与文档规模成正比。M2 将改为增量/分块历史（对齐原
 * DocumentHistory 的增量记录思路）。此取舍在 README 与评审材料中显式标注。
 */
#ifndef COMPOSITOR_DOMAIN_DOMAIN_STATE_H
#define COMPOSITOR_DOMAIN_DOMAIN_STATE_H

#include <cstdint>
#include <string>
#include <vector>

#include "domain/layer.h"

namespace compositor {

struct DocumentState {
  int32_t width = 0;
  int32_t height = 0;
  float dpi = 72.0f;
  std::string color_space = "sRGB";

  /* .comp 包的 documentID（大写 UUID 文本）。文档创建时生成、随文档持久保存，
   * 图层文件名由 (documentID, 图层 id) 派生，因此同一文档多次保存不会漂移。 */
  std::string document_id;

  /* 堆叠序：索引 0 为最底层，与 .comp 规范一致。 */
  std::vector<Layer> layers;
  comp_layer_id next_layer_id = 1;
  comp_layer_id active_layer = 0;
};

struct SelectionState {
  bool active = false;
  comp_rect rect{0, 0, 0, 0};
};

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_DOMAIN_STATE_H */
