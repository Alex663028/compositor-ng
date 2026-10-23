// AI生成
/*
 * selection_channel.cpp — 存储的选区通道（PS 的 Alpha 通道）的增删与载回。
 *
 * 逐条口径见 selection_channel.h。这里只强调一条实现约束：通道蒙版与选区
 * 基础蒙版共用 MaskBuffer 的 shared_ptr + COW 资产模型，保存是零拷贝共享，
 * 之后的任何修改（反选、扩缩、再次保存）都由 writable_pixels() 自己分离，
 * 不会出现「改了选区却顺手改坏通道」的别名事故。
 */
#include "domain/selection_channel.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "domain/selection.h"
#include "domain/utf8_text.h"

namespace compositor {
namespace {

/* 蒙版必须恰好是文档分辨率且有完整的字节缓冲。 */
bool mask_matches_document(const MaskBuffer& mask, const DocumentState& state) {
  if (state.width <= 0 || state.height <= 0) return false;
  if (mask.width != state.width || mask.height != state.height) return false;
  const size_t expected = static_cast<size_t>(state.width) * static_cast<size_t>(state.height);
  return mask.pixel_data().size() == expected;
}

/* 当前选区与「通道蒙版 + feather」是否逐字节一致（载回的空操作判定）。 */
bool selection_equals(const SelectionState& selection, const MaskBuffer& mask, float feather) {
  if (!selection.active) return false;
  if (selection.feather != feather) return false;
  if (selection.base.width != mask.width || selection.base.height != mask.height) return false;
  return selection.base.pixel_data() == mask.pixel_data();
}

}  // namespace

bool selection_channel_name_is_valid(const std::string& name) {
  if (name.empty()) return false;
  if (name.size() > kMaxSelectionChannelNameBytes) return false;
  if (!utf8_is_valid(name)) return false;
  return utf8_is_single_line(name);
}

bool selection_channel_save(DocumentState* state, const std::string& name, size_t* out_index) {
  if (state == nullptr) return false;
  if (!selection_channel_name_is_valid(name)) return false;
  if (!state->selection.active) return false;
  if (!mask_matches_document(state->selection.base, *state)) return false;
  if (state->channels.size() >= kMaxSelectionChannels) return false;

  SelectionChannel channel;
  channel.name = name;
  channel.feather = state->selection.feather;
  channel.mask = state->selection.base; /* COW 共享，不复制像素 */
  state->channels.push_back(std::move(channel));

  if (out_index != nullptr) *out_index = state->channels.size() - 1;
  return true;
}

bool selection_channel_load(DocumentState* state, size_t index, int32_t combine, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (state == nullptr) return false;
  if (combine != kSelectionReplace && combine != kSelectionAdd &&
      combine != kSelectionSubtract) {
    return false;
  }
  if (index >= state->channels.size()) return false;

  const SelectionChannel& channel = state->channels[index];
  if (!mask_matches_document(channel.mask, *state)) return false;

  const size_t pixel_count =
      static_cast<size_t>(state->width) * static_cast<size_t>(state->height);
  const bool same_grid = state->selection.active &&
                         state->selection.base.width == state->width &&
                         state->selection.base.height == state->height &&
                         state->selection.base.pixel_data().size() == pixel_count;

  /* subtract 没有基础选区时什么都不做（与形状叠加一致，羽化也不动）。 */
  if (combine == kSelectionSubtract && !same_grid) return true;

  if (combine == kSelectionReplace && same_grid && selection_equals(state->selection,
                                                                    channel.mask,
                                                                    channel.feather)) {
    return true; /* 等值短路：不改状态、不产生空撤销步 */
  }

  std::vector<uint8_t> next;
  switch (combine) {
    case kSelectionAdd: {
      const std::vector<uint8_t>& mask = channel.mask.pixel_data();
      next.assign(pixel_count, 0);
      for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t base_value = same_grid ? state->selection.base.pixel_data()[i] : 0;
        next[i] = std::max(base_value, mask[i]);
      }
      break;
    }
    case kSelectionSubtract: {
      const std::vector<uint8_t>& mask = channel.mask.pixel_data();
      next.assign(pixel_count, 0);
      for (size_t i = 0; i < pixel_count; ++i) {
        const double keep = 255.0 - static_cast<double>(mask[i]);
        next[i] = static_cast<uint8_t>(
            std::lround(static_cast<double>(state->selection.base.pixel_data()[i]) * keep / 255.0));
      }
      break;
    }
    default:
      break; /* replace 直接共享通道蒙版，不走拷贝路径 */
  }

  if (combine == kSelectionReplace) {
    state->selection.base = channel.mask; /* COW 共享 */
    state->selection.feather = channel.feather;
  } else {
    /* add / subtract 与形状叠加同口径：新几何 = 新羽化参数。 */
    if (same_grid && state->selection.feather == 0.0f &&
        next == state->selection.base.pixel_data()) {
      return true; /* 等值短路 */
    }
    MaskBuffer stored;
    stored.width = state->width;
    stored.height = state->height;
    stored.pixels = std::make_shared<std::vector<uint8_t>>(std::move(next));
    state->selection.base = std::move(stored);
    state->selection.feather = 0.0f;
  }

  state->selection.active = true;
  if (changed != nullptr) *changed = true;
  return true;
}

bool selection_channel_remove(DocumentState* state, size_t index, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (state == nullptr) return false;
  if (index >= state->channels.size()) return false;
  state->channels.erase(state->channels.begin() + static_cast<std::ptrdiff_t>(index));
  if (changed != nullptr) *changed = true;
  return true;
}

bool selection_channel_clear(DocumentState* state, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (state == nullptr) return false;
  if (state->channels.empty()) return true;
  state->channels.clear();
  if (changed != nullptr) *changed = true;
  return true;
}

}  // namespace compositor
