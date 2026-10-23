// AI生成
/*
 * selection_channel.h — 存储的选区通道（PS 的 Alpha 通道）的文档模型。
 *
 * 通道 = 「选区快照」：保存当前选区的硬边蒙版 + 羽化半径，稍后原样载回选区。
 * 这是 PS 的 Select > Save Selection / Load Selection 那条通路，也是通道面板
 * 的数据源。真正的像素存储（StackingImage / 通道面板缩略图）不在本层。
 *
 * 为什么是「硬边蒙版 + feather」而不是「烘焙过羽化的 coverage」：
 *   - 保存走 COW 拷贝，零重采样、逐字节可逆；
 *   - 载回时恢复同一个 feather 参数，随后的反选/扩缩/再用形状叠加的口径
 *     与「选区从没离开过」完全一致；
 *   - 若存有效 coverage（已模糊），载回再羽化会二次模糊，往返不相等。
 *
 * 名字口径与图层名同一把尺子（domain/utf8_text.h）：非空、严格 UTF-8、
 * 无控制字符、≤ kMaxSelectionChannelNameBytes。名字只进面板与 `.comp`。
 *
 * 尺寸口径：mask 恒为**文档分辨率**。换画布（Image Size / Canvas Size /
 * Crop）会连同选区一起丢弃通道；旋转画布则逐通道转置（坐标系只旋转）。
 */
#ifndef COMPOSITOR_DOMAIN_SELECTION_CHANNEL_H
#define COMPOSITOR_DOMAIN_SELECTION_CHANNEL_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "domain/domain_state.h"

namespace compositor {

/* 一条文档最多存多少个通道。PS 的上限是 56 个 Alpha 通道，这里取 16：
 * 日常抠图远用不到，而每通道是一张文档尺寸的蒙版（4K 文档约 8 MiB/个），
 * 上限必须能在「荒谬输入」与「够用」之间站住。 */
constexpr size_t kMaxSelectionChannels = 16;

/* 通道名字节上限（与 ABI 的两段式读缓冲一致）。 */
constexpr size_t kMaxSelectionChannelNameBytes = 255;

/* 名字判定：非空、≤ 上限、严格 UTF-8、无控制字符。 */
bool selection_channel_name_is_valid(const std::string& name);

/*
 * 把当前选区存成一个新通道（追加到末尾）。
 *   - 没有选区（active == false）→ false，state 不变；
 *   - 基础蒙版尺寸与文档尺寸不符（内部状态损坏）→ false；
 *   - 已达 kMaxSelectionChannels → false（宿主回报 COMP_ERR_STATE）。
 * 成功时 out_index（可空）写新下标；蒙版按 COW 共享，不复制像素。
 */
bool selection_channel_save(DocumentState* state, const std::string& name, size_t* out_index);

/*
 * 把第 index 个通道载入选区。combine 取 SelectionCombine 数值：
 *   - replace：选区基础蒙版 = 通道蒙版，羽化 = 通道羽化；
 *   - add：基础蒙版取逐像素最大值（没有选区时蒙版等价于 replace，
 *     但羽化仍归 0——只有 replace 恢复通道羽化）；
 *   - subtract：基础蒙版逐像素相减（没有选区时无操作，羽化不动）；
 *   - add / subtract 都把羽化归 0（与形状叠加同口径：新几何 = 新羽化参数）。
 * 结果与当前选区逐字节相同时 changed = false（不产生空撤销步）。
 * 索引越界、通道尺寸与文档不符 → false。
 */
bool selection_channel_load(DocumentState* state, size_t index, int32_t combine, bool* changed);

/* 删除第 index 个通道。越界 → false；成功时 changed = true。 */
bool selection_channel_remove(DocumentState* state, size_t index, bool* changed);

/* 清空全部通道（换画布时用）。原本为空 ⇒ changed = false。 */
bool selection_channel_clear(DocumentState* state, bool* changed);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_SELECTION_CHANNEL_H */
