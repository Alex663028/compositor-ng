// AI生成
/*
 * guide.cpp — 参考线模型的保存与校验。语义说明见 domain/guide.h。
 *
 * 这里刻意不碰外壳：标尺、吸附、显示开关都不在引擎里；引擎只保证
 * 「存得住、读得回、非法值进不来」。
 */
#include "domain/guide.h"

#include <cmath>

namespace compositor {

bool guide_is_valid(uint32_t axis, double position) {
  if (!guide_axis_valid(axis)) return false;
  if (!std::isfinite(position)) return false;
  return std::fabs(position) <= kMaxGuidePosition;
}

bool guide_add(DocumentState* state, uint32_t axis, double position, size_t* out_index) {
  if (state == nullptr) return false;
  if (!guide_is_valid(axis, position)) return false;
  if (state->guides.size() >= kMaxGuides) return false;
  Guide guide;
  guide.axis = axis;
  guide.position = position;
  state->guides.push_back(guide);
  if (out_index != nullptr) *out_index = state->guides.size() - 1;
  return true;
}

bool guide_move(DocumentState* state, size_t index, double position, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (state == nullptr) return false;
  if (index >= state->guides.size()) return false;
  Guide& guide = state->guides[index];
  if (!guide_is_valid(guide.axis, position)) return false;
  if (guide.position == position) return true;
  guide.position = position;
  if (changed != nullptr) *changed = true;
  return true;
}

bool guide_remove(DocumentState* state, size_t index, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (state == nullptr) return false;
  if (index >= state->guides.size()) return false;
  state->guides.erase(state->guides.begin() + static_cast<std::ptrdiff_t>(index));
  if (changed != nullptr) *changed = true;
  return true;
}

bool guide_clear(DocumentState* state, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (state == nullptr) return false;
  if (state->guides.empty()) return true;
  state->guides.clear();
  if (changed != nullptr) *changed = true;
  return true;
}

bool guide_snap_to(double position, const double* candidates, size_t count, double tolerance,
                   double* out_position) {
  if (out_position == nullptr) return false;
  if (candidates == nullptr || count == 0) return false;
  if (!std::isfinite(position) || !std::isfinite(tolerance)) return false;
  if (tolerance < 0.0) return false;

  bool found = false;
  double best_distance = tolerance;
  double best = position;
  for (size_t i = 0; i < count; ++i) {
    const double candidate = candidates[i];
    if (!std::isfinite(candidate)) continue;
    const double distance = std::fabs(candidate - position);
    /* 严格小于：等距时保留先出现的候选（调用方按固定顺序拼装候选表）。
     * 首个命中允许 distance == tolerance（tolerance = 0 时正好等于候选也应吸附）。 */
    if (distance < best_distance || (!found && distance <= best_distance)) {
      best_distance = distance;
      best = candidate;
      found = true;
    }
  }
  if (found) *out_position = best;
  return found;
}

}  // namespace compositor
