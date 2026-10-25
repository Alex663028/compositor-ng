// AI生成
/*
 * shell_layers.cpp — 图层 / 调整 / 曲线 / 效果 / 直方图面板逻辑
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {


/*
 * 撤销/重做后刷新状态栏：深度、内存占用、是否截断统统直接问引擎，
 * 壳内不记账（P1-3：撤销栈预算信号必须透到用户可见处）。
 * 注意：wsprintfW 不支持 %f，内存按 KiB 用整数输出。
 */
void refresh_history_status(ShellState& shell, const wchar_t* prefix) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  size_t undo_depth = 0;
  size_t redo_depth = 0;
  comp_document_history_stats(shell.doc.document, &undo_depth, &redo_depth);

  size_t history_bytes = 0;
  if (comp_document_history_bytes(shell.doc.document, &history_bytes) != COMP_OK) history_bytes = 0;
  int32_t truncated = 0;
  if (comp_document_history_truncated(shell.doc.document, &truncated) != COMP_OK) truncated = 0;
  const unsigned kib = static_cast<unsigned>(history_bytes / 1024ull);

  wchar_t buffer[192];
  wsprintfW(buffer, L"%s（撤销 %u / 重做 %u · 历史 %u KiB%s）", prefix,
            static_cast<unsigned>(undo_depth), static_cast<unsigned>(redo_depth), kib,
            truncated != 0 ? L"，已截断" : L"");
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 图层面板 */

uint8_t visible_of(ShellState& shell, comp_layer_id id) {
  for (const auto& entry : shell.doc.visibility) {
    if (entry.first == id) return entry.second;
  }
  shell.doc.visibility.emplace_back(id, static_cast<uint8_t>(1));
  return 1;
}

void set_visible_cache(ShellState& shell, comp_layer_id id, uint8_t value) {
  for (auto& entry : shell.doc.visibility) {
    if (entry.first == id) {
      entry.second = value;
      return;
    }
  }
  shell.doc.visibility.emplace_back(id, value);
}

/*
 * P1-2 收尾：图层面板过滤（视图态，见 shell_state.h 的字段说明）。
 *
 * 判定只用 LayerRow 的只读快照，不调引擎、不开事务。三个维度是「与」关系：
 * 类型、颜色、名称同时满足才显示。名称比较按 UTF-16 折叠大小写后做子串匹配，
 * 中文不受影响（只折叠 ASCII / 拉丁）。
 *
 * 祖先保留：命中行的父组即使自己没命中也要显示，否则组消失后子行会以
 * 「凭空出现的缩进」出现，层级看起来像坏了。祖先行按命中处理（可选中、可操作）。
 */
bool layer_filter_active(const ShellState& shell) {
  return shell.layer_filter_kind != 0 || shell.layer_filter_color >= 0 ||
         !shell.layer_filter_name.empty();
}

namespace {

wchar_t fold_case(wchar_t ch) {
  if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch - L'A' + L'a');
  return ch;
}

bool name_contains(const std::wstring& haystack, const std::wstring& needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  const size_t last = haystack.size() - needle.size();
  for (size_t start = 0; start <= last; ++start) {
    size_t i = 0;
    while (i < needle.size() && fold_case(haystack[start + i]) == fold_case(needle[i])) ++i;
    if (i == needle.size()) return true;
  }
  return false;
}

bool row_matches_filter(const ShellState& shell, const LayerRow& row) {
  switch (shell.layer_filter_kind) {
    case 0:
      break;
    case 1: /* 像素层：不是组、不是文字、不是调整层、不是填充层、不是智能对象 */
      if (row.is_group != 0 || row.is_text != 0 || row.adjustment_count > 0 ||
          row.fill_kind != 0 || row.is_smart != 0) {
        return false;
      }
      break;
    case 2:
      if (row.is_text == 0) return false;
      break;
    case 3:
      if (row.is_group == 0) return false;
      break;
    case 4:
      if (row.adjustment_count <= 0) return false;
      break;
    case 5:
      if (row.fill_kind == 0) return false;
      break;
    case 6:
      if (row.is_smart == 0) return false;
      break;
    default:
      break;
  }
  if (shell.layer_filter_color >= 0 &&
      static_cast<int>(row.color_label) != shell.layer_filter_color) {
    return false;
  }
  if (!shell.layer_filter_name.empty() &&
      !name_contains(widen_utf8(row.name), shell.layer_filter_name)) {
    return false;
  }
  return true;
}

}  // namespace

/*
 * 过滤后的显示序：命中行 + 其全部祖先。返回的是 `shell.doc.layers` 的下标
 * （升序，与数组序一致）。无过滤时返回全量。
 */
std::vector<int32_t> filtered_layer_indices(const ShellState& shell) {
  const int32_t count = static_cast<int32_t>(shell.doc.layers.size());
  std::vector<int32_t> indices;
  indices.reserve(static_cast<size_t>(count));
  if (!layer_filter_active(shell)) {
    for (int32_t i = 0; i < count; ++i) indices.push_back(i);
    return indices;
  }
  std::vector<uint8_t> keep(static_cast<size_t>(count), 0);
  for (int32_t i = 0; i < count; ++i) {
    const LayerRow& row = shell.doc.layers[static_cast<size_t>(i)];
    if (!row_matches_filter(shell, row)) continue;
    /* 自身 + 沿 parent 链上溯（含防御性上限：脏数据里的环不能把这里挂死）。 */
    int32_t cursor = i;
    for (int32_t guard = 0; cursor >= 0 && guard <= count; ++guard) {
      if (keep[static_cast<size_t>(cursor)] != 0) break;
      keep[static_cast<size_t>(cursor)] = 1;
      const comp_layer_id parent = shell.doc.layers[static_cast<size_t>(cursor)].parent;
      if (parent == 0) break;
      cursor = -1;
      for (int32_t j = 0; j < count; ++j) {
        if (shell.doc.layers[static_cast<size_t>(j)].id == parent) {
          cursor = j;
          break;
        }
      }
    }
  }
  for (int32_t i = 0; i < count; ++i) {
    if (keep[static_cast<size_t>(i)] != 0) indices.push_back(i);
  }
  return indices;
}

/*
 * S44：重建树形显示序（组行在上、子层缩进在下、折叠的组不展开）。
 *
 * layers 变了（换文档 / 撤销 / 增删层）或折叠状态变了都要调。折叠集合先按「仍是组」
 * 收敛：撤销或删除之后 id 可能已经不在了，留着它只会让以后新建的同 id 组莫名其妙
 * 处于折叠态。
 */
void rebuild_layer_display(ShellState& shell) {
  if (!shell.doc.collapsed_groups.empty()) {
    std::vector<comp_layer_id> alive;
    alive.reserve(shell.doc.collapsed_groups.size());
    for (comp_layer_id id : shell.doc.collapsed_groups) {
      for (const LayerRow& row : shell.doc.layers) {
        if (row.id == id && row.is_group != 0) {
          alive.push_back(id);
          break;
        }
      }
    }
    shell.doc.collapsed_groups.swap(alive);
  }

  /* P1-2 收尾：先算可见子集，再把它当成一个紧凑数组交给树形排布。 */
  const std::vector<int32_t> indices = filtered_layer_indices(shell);
  const int32_t visible = static_cast<int32_t>(indices.size());
  std::vector<comp_ui::LayerStackEntry> entries(static_cast<size_t>(visible));
  std::vector<uint8_t> collapsed(static_cast<size_t>(visible), 0);
  for (int32_t i = 0; i < visible; ++i) {
    const LayerRow& row = shell.doc.layers[static_cast<size_t>(indices[static_cast<size_t>(i)])];
    entries[static_cast<size_t>(i)] = comp_ui::LayerStackEntry{
        static_cast<int64_t>(row.id), static_cast<int64_t>(row.parent),
        static_cast<int64_t>(row.clipping)};
    for (comp_layer_id id : shell.doc.collapsed_groups) {
      if (id == row.id) {
        collapsed[static_cast<size_t>(i)] = 1;
        break;
      }
    }
  }
  shell.doc.layer_display.assign(static_cast<size_t>(visible), 0);
  shell.doc.layer_depth.assign(static_cast<size_t>(visible), 0);
  const int32_t rows = comp_ui::build_layer_display(
      entries.data(), visible, collapsed.data(), shell.doc.layer_display.data(),
      shell.doc.layer_depth.data(), visible);
  shell.doc.layer_display.resize(static_cast<size_t>(rows));
  shell.doc.layer_depth.resize(static_cast<size_t>(rows));
  /* display 里存的是紧凑下标；投影回 layers 的真实下标。 */
  for (int32_t& index : shell.doc.layer_display) {
    const int32_t compact = index;
    index = (compact >= 0 && compact < visible) ? indices[static_cast<size_t>(compact)] : -1;
  }
}

/* 面板显示行数（折叠之后可能少于图层数）。 */
int32_t panel_row_count(const ShellState& shell) {
  return static_cast<int32_t>(shell.doc.layer_display.size());
}

/* 把面板镜像投影成 ui_core 的纯数据（数组序、自下而上），供纯函数规划用。 */
std::vector<comp_ui::LayerStackEntry> layer_stack_entries(const ShellState& shell) {
  std::vector<comp_ui::LayerStackEntry> entries(shell.doc.layers.size());
  for (size_t i = 0; i < shell.doc.layers.size(); ++i) {
    const LayerRow& row = shell.doc.layers[i];
    entries[i] = comp_ui::LayerStackEntry{static_cast<int64_t>(row.id),
                                          static_cast<int64_t>(row.parent),
                                          static_cast<int64_t>(row.clipping)};
  }
  return entries;
}

/* 显示行 -> 图层；越界 / 被折叠返回 nullptr。所有面板读路径都必须走这里。 */
const LayerRow* layer_row_at(const ShellState& shell, int32_t row) {
  if (row < 0 || row >= panel_row_count(shell)) return nullptr;
  const int32_t index = shell.doc.layer_display[static_cast<size_t>(row)];
  if (index < 0 || index >= static_cast<int32_t>(shell.doc.layers.size())) return nullptr;
  return &shell.doc.layers[static_cast<size_t>(index)];
}

/* 图层 id -> 显示行；不存在或被折叠返回 -1。 */
int32_t panel_row_of_id(const ShellState& shell, comp_layer_id id) {
  const int32_t rows = panel_row_count(shell);
  for (int32_t row = 0; row < rows; ++row) {
    const LayerRow* layer = layer_row_at(shell, row);
    if (layer != nullptr && layer->id == id) return row;
  }
  return -1;
}

bool layer_group_collapsed(const ShellState& shell, comp_layer_id id) {
  for (comp_layer_id collapsed : shell.doc.collapsed_groups) {
    if (collapsed == id) return true;
  }
  return false;
}

/*
 * 组折叠开关（S44）：只改壳侧显示状态，不写文档、不进撤销——上游的折叠三角
 * 同样是视图态。折叠后选中的子层会被藏起来，选中态落到组行上，避免「看不见却
 * 还在选中」导致下一次快捷键作用到不可见的层。
 */
void panel_toggle_group_collapsed(ShellState& shell, comp_layer_id id) {
  const LayerRow* selected_before = layer_row_at(shell, shell.doc.selected_layer);
  const comp_layer_id selected_id = selected_before != nullptr ? selected_before->id : 0;

  bool removed = false;
  for (size_t i = 0; i < shell.doc.collapsed_groups.size(); ++i) {
    if (shell.doc.collapsed_groups[i] == id) {
      shell.doc.collapsed_groups.erase(shell.doc.collapsed_groups.begin() +
                                   static_cast<std::ptrdiff_t>(i));
      removed = true;
      break;
    }
  }
  if (!removed) shell.doc.collapsed_groups.push_back(id);

  rebuild_layer_display(shell);
  const int32_t rows = panel_row_count(shell);
  if (shell.doc.selected_layer >= rows) shell.doc.selected_layer = rows > 0 ? rows - 1 : 0;
  if (shell.doc.selected_layer < 0) shell.doc.selected_layer = 0;
  if (selected_id != 0 && panel_row_of_id(shell, selected_id) < 0) {
    const int32_t group_row = panel_row_of_id(shell, id);
    if (group_row >= 0) shell.doc.selected_layer = group_row;
  }
  /*
   * 多选集合同样只保留看得见的层：批量命令按可见行枚举 id，留着被折叠的子层
   * 只会让「已选中 N 个图层」的计数与真正被执行的数量对不上。
   */
  if (!shell.doc.extra_selection.empty()) {
    std::vector<comp_layer_id> visible;
    visible.reserve(shell.doc.extra_selection.size());
    for (comp_layer_id extra : shell.doc.extra_selection) {
      if (panel_row_of_id(shell, extra) >= 0) visible.push_back(extra);
    }
    shell.doc.extra_selection.swap(visible);
  }
}

/* ------------------------------------------------ 面板过滤（P1-2 收尾） */

/*
 * 过滤条件变化后的统一收尾：重建显示序，把选中行夹回范围，并让多选集合只
 * 保留仍可见的层——否则「已选中 N 个」的计数会包含面板上根本看不见的行。
 */
void panel_refresh_after_filter(ShellState& shell) {
  const LayerRow* selected_before = selected_row(shell);
  const comp_layer_id selected_id = selected_before != nullptr ? selected_before->id : 0;

  rebuild_layer_display(shell);
  const int32_t rows = panel_row_count(shell);
  if (shell.doc.selected_layer < 0) shell.doc.selected_layer = 0;
  if (rows <= 0) {
    shell.doc.selected_layer = 0;
  } else if (shell.doc.selected_layer >= rows) {
    shell.doc.selected_layer = rows - 1;
  }
  /* 原选中层若被过滤掉，落到离它最近的可见行（面板非空时至少能继续操作）。 */
  if (selected_id != 0 && panel_row_of_id(shell, selected_id) < 0 && rows > 0) {
    if (shell.doc.selected_layer >= rows) shell.doc.selected_layer = rows - 1;
  }
  if (!shell.doc.extra_selection.empty()) {
    std::vector<comp_layer_id> visible;
    visible.reserve(shell.doc.extra_selection.size());
    for (comp_layer_id extra : shell.doc.extra_selection) {
      if (panel_row_of_id(shell, extra) >= 0) visible.push_back(extra);
    }
    shell.doc.extra_selection.swap(visible);
  }

  wchar_t buffer[224];
  if (!layer_filter_active(shell)) {
    wsprintfW(buffer, L"过滤已清除（%d 行）", static_cast<int>(rows));
  } else {
    const wchar_t* kind = L"全部";
    switch (shell.layer_filter_kind) {
      case 1: kind = L"像素"; break;
      case 2: kind = L"文字"; break;
      case 3: kind = L"组"; break;
      case 4: kind = L"调整"; break;
      case 5: kind = L"填充"; break;
      case 6: kind = L"智能对象"; break;
      default: break;
    }
    wsprintfW(buffer, L"过滤：类型=%s 颜色=%d 名称=%s（%d 行）", kind,
              shell.layer_filter_color, shell.layer_filter_name.c_str(),
              static_cast<int>(rows));
  }
  set_status(shell, buffer);
}

void panel_set_filter_kind(ShellState& shell, int kind) {
  if (kind < 0 || kind > 6) return;
  shell.layer_filter_kind = kind;
  panel_refresh_after_filter(shell);
}

void panel_set_filter_color(ShellState& shell, int color) {
  if (color < -1 || color > 7) return;
  shell.layer_filter_color = color;
  panel_refresh_after_filter(shell);
}

void panel_begin_filter_name(ShellState& shell) {
  shell.doc.layer_input = LayerInput::FilterName;
  shell.doc.layer_input_id = 0;
  shell.doc.layer_input_text = shell.layer_filter_name;
  refresh_layer_input_status(shell);
}

void panel_clear_filter(ShellState& shell) {
  shell.layer_filter_kind = 0;
  shell.layer_filter_color = -1;
  shell.layer_filter_name.clear();
  panel_refresh_after_filter(shell);
}

/* ------------------------------------------------ 图层复合（P1-2 收尾 ⑥） */

/*
 * 图层复合 = 「每个现存图层可见不可见 + 不透明度多少」的一份具名快照。
 *
 * 与 PS 一致的口径：只记可见性与不透明度，不记位置/几何（那是变换的事）；
 * 捕获之后才建的层不受复合管辖，应用时已被删除层按 id 找不到就跳过。
 *
 * 与 PS 的差异：复合表存在**文档里**（进撤销栈、随 .comp 落盘），壳只额外记
 * 一个「当前复合」的视图态 id（ShellState::layer_comp_current）。撤销/重开文档
 * 后该 id 可能失效，因此每次读取都用引擎现状校验一次，失效就退回第一个复合，
 * 不靠壳侧缓存与引擎对齐。
 */

namespace {

/* 复合是否还存在：按 id_at 线性枚举，不依赖引擎内部的查找接口。 */
bool layer_comp_exists(comp_document document, comp_layer_comp_id comp) {
  if (document == nullptr || comp == 0) return false;
  size_t count = 0;
  if (comp_layer_comp_count(document, &count) != COMP_OK) return false;
  for (size_t i = 0; i < count; ++i) {
    comp_layer_comp_id id = 0;
    if (comp_layer_comp_id_at(document, i, &id) != COMP_OK) return false;
    if (id == comp) return true;
  }
  return false;
}

/* 复合名（两段式读取，UTF-8 -> UTF-16）。失败回空串，状态栏据此降级显示。 */
std::wstring layer_comp_display_name(comp_document document, comp_layer_comp_id comp) {
  size_t required = 0;
  if (comp_layer_comp_name(document, comp, nullptr, 0, &required) != COMP_OK || required == 0) {
    return std::wstring();
  }
  std::vector<char> buffer(required, '\0');
  if (comp_layer_comp_name(document, comp, buffer.data(), buffer.size(), &required) != COMP_OK) {
    return std::wstring();
  }
  return widen_utf8(std::string(buffer.data()));
}

/*
 * 复合是否与文档当前状态一致（口径与引擎的 update 幂等判定相同：逐条比可见性
 * 与不透明度，已删除的层跳过）。不用撤销深度去猜——栈满被截断后深度不涨，
 * 那样会把「其实记了一步」误报成「未记入撤销」。
 */
bool layer_comp_matches_current(comp_document document, comp_layer_comp_id comp) {
  size_t count = 0;
  if (comp_layer_comp_entry_count(document, comp, &count) != COMP_OK) return false;
  for (size_t i = 0; i < count; ++i) {
    comp_layer_id layer = 0;
    uint8_t visible = 0;
    float opacity = 0.0f;
    if (comp_layer_comp_entry(document, comp, i, &layer, &visible, &opacity) != COMP_OK) {
      return false;
    }
    int32_t current_visible = 0;
    float current_opacity = 0.0f;
    if (comp_layer_get_visible(document, layer, &current_visible) != COMP_OK) continue;
    if (comp_layer_get_opacity(document, layer, &current_opacity) != COMP_OK) continue;
    if ((current_visible != 0 ? 1 : 0) != (visible != 0 ? 1 : 0)) return false;
    if (current_opacity != opacity) return false;
  }
  return true;
}

}  // namespace

comp_layer_comp_id panel_current_layer_comp(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    shell.layer_comp_current = 0;
    return 0;
  }
  if (layer_comp_exists(shell.doc.document, shell.layer_comp_current)) {
    return shell.layer_comp_current;
  }
  comp_layer_comp_id first = 0;
  if (comp_layer_comp_id_at(shell.doc.document, 0, &first) != COMP_OK) first = 0;
  shell.layer_comp_current = first;
  return first;
}

/* 状态栏报「当前复合」的位置与条目数（条目数 = 捕获时的图层数）。 */
void panel_layer_comp_status(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  size_t count = 0;
  if (comp_layer_comp_count(shell.doc.document, &count) != COMP_OK) count = 0;
  if (count == 0) {
    set_status(shell, L"还没有图层复合（图层 > 图层复合 > 捕获为新复合）");
    return;
  }
  const comp_layer_comp_id current = panel_current_layer_comp(shell);
  size_t index = 0;
  for (size_t i = 0; i < count; ++i) {
    comp_layer_comp_id id = 0;
    if (comp_layer_comp_id_at(shell.doc.document, i, &id) == COMP_OK && id == current) {
      index = i;
      break;
    }
  }
  size_t entries = 0;
  if (comp_layer_comp_entry_count(shell.doc.document, current, &entries) != COMP_OK) entries = 0;
  const std::wstring name = layer_comp_display_name(shell.doc.document, current);
  wchar_t buffer[224];
  wsprintfW(buffer, L"图层复合 %u/%u：%s（%u 个图层）", static_cast<unsigned>(index + 1),
            static_cast<unsigned>(count), name.c_str(), static_cast<unsigned>(entries));
  set_status(shell, buffer);
}

/* 上一个 / 下一个复合（循环）。只改视图态 id，不碰文档、不落撤销步。 */
void panel_layer_comp_select(ShellState& shell, int delta) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  size_t count = 0;
  if (comp_layer_comp_count(shell.doc.document, &count) != COMP_OK) count = 0;
  if (count == 0) {
    shell.layer_comp_current = 0;
    set_status(shell, L"还没有图层复合（图层 > 图层复合 > 捕获为新复合）");
    return;
  }
  std::vector<comp_layer_comp_id> ids(count, 0);
  for (size_t i = 0; i < count; ++i) {
    if (comp_layer_comp_id_at(shell.doc.document, i, &ids[i]) != COMP_OK) {
      set_status(shell, L"读取图层复合列表失败");
      return;
    }
  }
  const comp_layer_comp_id current = panel_current_layer_comp(shell);
  size_t index = 0;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == current) {
      index = i;
      break;
    }
  }
  const long long total = static_cast<long long>(ids.size());
  long long next = static_cast<long long>(index) + static_cast<long long>(delta);
  next %= total;
  if (next < 0) next += total;
  shell.layer_comp_current = ids[static_cast<size_t>(next)];
  panel_layer_comp_status(shell);
}

/*
 * 捕获/改名共用一条文本输入通道：layer_input_id == 0 表示捕获新复合，
 * 非 0 表示改这个复合的名字。默认名「复合 N」按当前数量 +1 起，重名引擎允许
 * （与上游一致），名字只用来给人看。
 */
void panel_layer_comp_capture_begin(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  size_t count = 0;
  if (comp_layer_comp_count(shell.doc.document, &count) != COMP_OK) count = 0;
  wchar_t buffer[64];
  wsprintfW(buffer, L"复合 %d", static_cast<int>(count + 1));
  shell.doc.layer_input = LayerInput::CompName;
  shell.doc.layer_input_id = 0;
  shell.doc.layer_input_text = buffer;
  refresh_layer_input_status(shell);
}

void panel_layer_comp_rename_begin(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_comp_id comp = panel_current_layer_comp(shell);
  if (comp == 0) {
    set_status(shell, L"还没有图层复合");
    return;
  }
  shell.doc.layer_input = LayerInput::CompName;
  shell.doc.layer_input_id = comp;
  shell.doc.layer_input_text = layer_comp_display_name(shell.doc.document, comp);
  refresh_layer_input_status(shell);
}

/*
 * 应用复合。幂等由引擎判定：全表都已是快照状态 -> changed == 0，不落撤销步，
 * 壳也就别推进缓存、别置 needs_render（否则每按一次都像改了什么）。
 */
void panel_layer_comp_apply(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_comp_id comp = panel_current_layer_comp(shell);
  if (comp == 0) {
    set_status(shell, L"还没有图层复合");
    return;
  }
  int32_t changed = 0;
  if (comp_layer_comp_apply(shell.doc.document, comp, &changed) != COMP_OK) {
    set_status(shell, L"应用图层复合失败");
    return;
  }
  const std::wstring name = layer_comp_display_name(shell.doc.document, comp);
  refresh_layers(shell);
  wchar_t buffer[192];
  if (changed == 0) {
    wsprintfW(buffer, L"复合「%s」已是当前状态（0 处改动）", name.c_str());
    set_status(shell, buffer);
    return;
  }
  shell.doc.needs_render = true;
  wsprintfW(buffer, L"已应用复合「%s」（%d 个图层被改动）", name.c_str(),
            static_cast<int>(changed));
  refresh_history_status(shell, buffer);
}

/* 用当前状态覆盖复合。内容未变时引擎不落步，状态栏照实说「无变化」。 */
void panel_layer_comp_update(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_comp_id comp = panel_current_layer_comp(shell);
  if (comp == 0) {
    set_status(shell, L"还没有图层复合");
    return;
  }
  comp_document document = shell.doc.document;
  const bool unchanged = layer_comp_matches_current(document, comp);
  if (comp_layer_comp_update(document, comp) != COMP_OK) {
    set_status(shell, L"更新图层复合失败");
    return;
  }
  const std::wstring name = layer_comp_display_name(document, comp);
  wchar_t buffer[192];
  if (unchanged) {
    wsprintfW(buffer, L"复合「%s」内容未变，未记入撤销", name.c_str());
    set_status(shell, buffer);
    return;
  }
  wsprintfW(buffer, L"已用当前状态更新复合「%s」", name.c_str());
  refresh_history_status(shell, buffer);
}

/*
 * 删除当前复合。引擎对不存在的 id 返回 NOT_FOUND（删除故意不幂等），所以先从
 * 视图态里退一个仍然存在的复合，避免删完还指着孤儿 id。删除不影响像素。
 */
void panel_layer_comp_delete(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_comp_id comp = panel_current_layer_comp(shell);
  if (comp == 0) {
    set_status(shell, L"还没有图层复合");
    return;
  }
  const std::wstring name = layer_comp_display_name(shell.doc.document, comp);
  if (comp_layer_comp_delete(shell.doc.document, comp) != COMP_OK) {
    set_status(shell, L"删除图层复合失败");
    return;
  }
  shell.layer_comp_current = 0;
  panel_current_layer_comp(shell);
  wchar_t buffer[192];
  wsprintfW(buffer, L"已删除复合「%s」", name.c_str());
  refresh_history_status(shell, buffer);
}

}

/* 两段式枚举图层 id，并逐个查询属性（只读，不引入隐式事务）。 */
void refresh_layers(ShellState& shell) {
  shell.doc.layers.clear();
  if (shell.doc.document == nullptr) {
    shell.doc.extra_selection.clear();
    shell.doc.layer_display.clear();
    shell.doc.layer_depth.clear();
    return;
  }

  size_t count = 0;
  if (comp_layer_count(shell.doc.document, &count) != COMP_OK || count == 0) {
    shell.doc.selected_layer = 0;
    shell.doc.extra_selection.clear();
    shell.doc.layer_display.clear();
    shell.doc.layer_depth.clear();
    return;
  }
  std::vector<comp_layer_id> ids(count, 0);
  size_t written = 0;
  if (comp_layer_list(shell.doc.document, ids.data(), ids.size(), &written) != COMP_OK) {
    /* 列表读失败：显示序必须一起清掉，否则行数还停在上一份文档。 */
    shell.doc.layer_display.clear();
    shell.doc.layer_depth.clear();
    return;
  }
  if (written < ids.size()) ids.resize(written);

  for (comp_layer_id id : ids) {
    LayerRow row;
    row.id = id;
    /*
     * P0-1：名字走两段式读取（先问长度再取字节）。名字是 UTF-8，可能长于一行的
     * 显示宽度，绘制侧再截断；这里不截断——截断后写回去就不是原名了。
     * 查询失败（旧引擎/已删除）留空串，面板回落到 id 显示。
     */
    size_t name_required = 0;
    if (comp_layer_get_name(shell.doc.document, id, nullptr, 0, &name_required) == COMP_OK &&
        name_required > 1u && name_required <= kLayerNameCapacity) {
      std::vector<char> name_bytes(name_required, '\0');
      size_t name_written = 0;
      if (comp_layer_get_name(shell.doc.document, id, name_bytes.data(), name_bytes.size(),
                              &name_written) == COMP_OK) {
        row.name.assign(name_bytes.data(), name_bytes.size() - 1u);
      }
    }
    float opacity = 1.0f;
    if (comp_layer_get_opacity(shell.doc.document, id, &opacity) == COMP_OK) {
      row.opacity = opacity;
    }
    comp_blend_mode blend = static_cast<comp_blend_mode>(0);
    if (comp_layer_get_blend(shell.doc.document, id, &blend) == COMP_OK) {
      row.blend = static_cast<int32_t>(blend);
    }
    int32_t visible = 0;
    if (comp_layer_get_visible(shell.doc.document, id, &visible) == COMP_OK) {
      row.visible = visible != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
      set_visible_cache(shell, id, row.visible);
    } else {
      /* 兜底：仅在 ABI 查询失败时沿用壳侧缓存。 */
      row.visible = visible_of(shell, id);
    }
    /*
     * S22：把「引擎已参与像素求值、壳却看不到」的三个结构属性接进面板。全部走
     * ABI 回读、只用于绘制与置灰——写路径一律回到引擎，所以撤销 / 合并 /
     * `.comp` 读回都不会让面板与文档漂移（与 S21 的 transform 回读口同一理由）。
     */
    int32_t has_mask = 0;
    int32_t mask_w = 0;
    int32_t mask_h = 0;
    if (comp_layer_get_mask(shell.doc.document, id, &has_mask, &mask_w, &mask_h) == COMP_OK) {
      row.has_mask = has_mask != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    }
    int32_t info_is_group = 0;
    comp_layer_id info_parent = 0;
    comp_layer_id info_clipping = 0;
    if (comp_layer_get_group_info(shell.doc.document, id, &info_is_group, &info_parent,
                                  &info_clipping) == COMP_OK) {
      row.is_group = info_is_group != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
      row.clipping = info_clipping;
      row.parent = info_parent;
      /*
       * 组隔离开关只对组有意义（ABI 注释：写入非组图层不报错但属性不参与合成）。
       * 面板据此置灰：非组行不显示、也不允许切换隔离——否则用户对一个普通图层
       * 反复按快捷键只看到状态栏说「已忽略」，那是把引擎语义泄漏成 UI 异常。
       */
      int32_t isolated = 0;
      if (row.is_group != 0 &&
          comp_layer_get_isolated(shell.doc.document, id, &isolated) == COMP_OK) {
        row.isolated = isolated != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
      }
    }
    comp_sampling sampling = COMP_SAMPLING_HIGH_QUALITY;
    if (comp_layer_get_sampling(shell.doc.document, id, &sampling) == COMP_OK) {
      row.sampling = static_cast<int32_t>(sampling);
    }
    /*
     * S24：镜像与效果。镜像走本层新增的 getter（set_flip 早已存在但壳调不到，
     * 于是「翻转」这个 PS 基础操作在本壳里不存在）；效果数走 effect_count，
     * 用于菜单勾选与行内 fx 标记。两者都只读、都只用于展示。
     */
    int32_t flip_x = 0;
    int32_t flip_y = 0;
    if (comp_layer_get_flip(shell.doc.document, id, &flip_x, &flip_y) == COMP_OK) {
      row.flip_x = flip_x != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
      row.flip_y = flip_y != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    }
    size_t effect_count = 0;
    if (comp_layer_effect_count(shell.doc.document, id, &effect_count) == COMP_OK) {
      /* 单层效果数量极少（<256），int32_t 足够；超限按 255 封顶只影响标记显示。 */
      row.effect_count = effect_count > 255u ? 255 : static_cast<int32_t>(effect_count);
    }
    /*
     * P1-2：锁与颜色标签也走回读。这两项是引擎侧拒绝写入口的依据，面板必须显示
     * 同一份事实——尤其是撤销（改锁本身可撤销）之后，缓存不留旧值。
     */
    uint32_t lock_flags = 0;
    if (comp_layer_get_lock(shell.doc.document, id, &lock_flags) == COMP_OK) {
      row.lock_flags = lock_flags & COMP_LAYER_LOCK_MASK;
    }
    uint32_t color_label = 0;
    if (comp_layer_get_color_label(shell.doc.document, id, &color_label) == COMP_OK) {
      row.color_label = color_label < COMP_LAYER_COLOR_COUNT ? color_label : 0u;
    }
    uint32_t link_group = 0;
    if (comp_layer_get_link(shell.doc.document, id, &link_group) == COMP_OK) {
      row.link_group = link_group;
    }
    /*
     * P1-2 收尾：过滤用的类型快照（只读）。文字层用 get_text 的空查询判定
     * （非文字层返回 COMP_ERR_STATE），调整层数走 adjustment_count；
     * 智能对象与填充层分别走 is_smart / is_fill（见下）。
     */
    size_t text_required = 0;
    row.is_text = comp_layer_get_text(shell.doc.document, id, nullptr, nullptr, 0,
                                      &text_required) == COMP_OK
                      ? 1
                      : 0;
    size_t adjustment_count = 0;
    if (comp_layer_adjustment_count(shell.doc.document, id, &adjustment_count) == COMP_OK) {
      row.adjustment_count =
          adjustment_count > 255u ? 255 : static_cast<int32_t>(adjustment_count);
    }
    shell.doc.layers.push_back(row);
  }

  /* S44：显示序跟着 layers 重建，再按显示行数夹取选中行（折叠后行数会变少）。 */
  rebuild_layer_display(shell);
  const int32_t rows = panel_row_count(shell);
  if (shell.doc.selected_layer < 0) shell.doc.selected_layer = 0;
  if (rows <= 0) {
    shell.doc.selected_layer = 0;
  } else if (shell.doc.selected_layer >= rows) {
    shell.doc.selected_layer = rows - 1;
  }
  /*
   * 多选集合随图层事实收敛：被删掉的图层（删除/合并/撤销/拼合）必须从额外选中里
   * 摘掉，否则下一次批量命令会对着已失效 id 发写入，状态栏报错而用户不知道是谁。
   */
  if (!shell.doc.extra_selection.empty()) {
    std::vector<comp_layer_id> alive;
    alive.reserve(shell.doc.extra_selection.size());
    for (comp_layer_id id : shell.doc.extra_selection) {
      for (const LayerRow& row : shell.doc.layers) {
        if (row.id == id) {
          alive.push_back(id);
          break;
        }
      }
    }
    shell.doc.extra_selection.swap(alive);
  }
  /* 选中图层变化后，调整面板必须跟着换数据源。 */
  refresh_adjustments(shell);
  /* S24：效果列表与调整列表同一数据源（选中图层），同一刷新汇合点。 */
  refresh_effects(shell);
  /*
   * S22：直方图随后同步。挂在这里而不是每个写入口各调一次，是因为
   * refresh_layers 已经是全部变更路径（滤镜 / 工具 / 撤销 / 合并 / 打开文档）
   * 的共同汇合点；漏挂一条路径的症状是「面板上的图不动了」，而那种滞后很难
   * 追。取不到的图层（组 / 16 位）由 refresh_histogram 自己关面板并说明原因。
   */
  refresh_histogram(shell);
}

/* 面板显示行 -> 图层：S44 起行号经树形 display 换算，不再假设「行 = 逆序下标」。 */
const LayerRow* selected_row(const ShellState& shell) {
  return layer_row_at(shell, shell.doc.selected_layer);
}

void panel_move_selection(ShellState& shell, int32_t delta) {
  const int32_t count = panel_row_count(shell);
  if (count == 0) {
    set_status(shell, L"无图层");
    return;
  }
  int32_t next = shell.doc.selected_layer + delta;
  if (next < 0) next = 0;
  if (next >= count) next = count - 1;
  shell.doc.selected_layer = next;
  const LayerRow* row = selected_row(shell);
  if (row != nullptr) {
    wchar_t buffer[128];
    wsprintfW(buffer, L"已选中图层 #%I64u（混合#%d %d%%）",
              static_cast<unsigned long long>(row->id), row->blend,
              static_cast<int>(row->opacity * 100.0f + 0.5f));
    set_status(shell, buffer);
  }
}

void panel_toggle_visible(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const comp_layer_id id = row->id;
  const uint8_t next = row->visible != 0 ? static_cast<uint8_t>(0) : static_cast<uint8_t>(1);
  if (comp_layer_set_visible(shell.doc.document, id, next) != COMP_OK) {
    set_status(shell, L"切换可见性失败");
    return;
  }
  set_visible_cache(shell, id, next);
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, next != 0 ? L"已显示图层" : L"已隐藏图层");
}

void panel_adjust_opacity(ShellState& shell, float delta) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  float next = row->opacity + delta;
  if (next < 0.0f) next = 0.0f;
  if (next > 1.0f) next = 1.0f;
  const comp_layer_id id = row->id;
  if (comp_layer_set_opacity(shell.doc.document, id, next) != COMP_OK) {
    set_status(shell, L"设置不透明度失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  wchar_t buffer[96];
  wsprintfW(buffer, L"不透明度 %d%%", static_cast<int>(next * 100.0f + 0.5f));
  refresh_history_status(shell, buffer);
}

/*
 * 混合模式步进（S21）：按上游 LayerBlendMode.allCases 的声明序循环并回绕，
 * 一步一次撤销（对齐上游 cycleBlendMode / setLayerBlendMode）。
 * 走 menu order 的档位由 ui_core 提供，壳内不复制枚举表。
 */
void panel_cycle_blend(ShellState& shell, int direction) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const int next = comp_ui::step_blend_mode(row->blend, direction);
  if (next == row->blend) return; /* 单一档位（不应发生）*/
  if (comp_layer_set_blend(shell.doc.document, row->id, static_cast<comp_blend_mode>(next)) != COMP_OK) {
    set_status(shell, L"设置混合模式失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  const std::wstring name = widen_ascii(comp_ui::blend_mode_name(next));
  const std::wstring group = widen_ascii(comp_ui::blend_group_name(comp_ui::blend_group_of(next)));
  wchar_t buffer[224];
  wsprintfW(buffer, L"混合模式：%s（%s 组，%d/%d）", name.c_str(), group.c_str(), next + 1,
            comp_ui::kBlendCount);
  refresh_history_status(shell, buffer);
}

/*
 * 混合模式直达（S21）：菜单选哪一项就落到那一档，与步进走同一条 ABI 路径。
 * 幂等：已是该档位时仍走一次写入（与上游 setLayerBlendMode 同语义，历史记一步）。
 */
void panel_set_blend(ShellState& shell, int mode) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const int next = comp_ui::clamp_blend_mode(mode);
  if (comp_layer_set_blend(shell.doc.document, row->id, static_cast<comp_blend_mode>(next)) != COMP_OK) {
    set_status(shell, L"设置混合模式失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  const std::wstring name = widen_ascii(comp_ui::blend_mode_name(next));
  const std::wstring group = widen_ascii(comp_ui::blend_group_name(comp_ui::blend_group_of(next)));
  wchar_t buffer[224];
  wsprintfW(buffer, L"混合模式：%s（%s 组，%d/%d）", name.c_str(), group.c_str(), next + 1,
            comp_ui::kBlendCount);
  refresh_history_status(shell, buffer);
}

/*
 * 选区 -> 图层蒙版（S22，Ctrl+Shift+M / 菜单「从选区建立蒙版」）。
 *
 * 接的是引擎**早已实现且已参与合成**的能力：filter_graph.cpp 的
 * coverage.add(&layer->mask) 真的把蒙版合成进画面，`.comp` 侧 maskFile /
 * maskEnabled 也早已落盘。缺的从来只是壳没开口子。
 *
 * 两处刻意的口径：
 *  1. 无选区时如实拒绝（COMP_ERR_STATE 由引擎给），不静默画一张全 255 的空操作
 *     蒙版——那会让用户以为「取消了选区限制」而实际上层已被完全遮住；
 *  2. 蒙版取的是**当前有效选区 coverage**（含羽化），因此「羽化 8px 后建蒙版」
 *     得到的正是边缘渐变的蒙版，与蚂蚁线看见的一致，不必再走一层近似。
 */
void panel_mask_from_selection(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (!shell.doc.selection_active || shell.doc.selection_mask.empty()) {
    set_status(shell, L"没有活动选区：先用框选/套索/魔棒划定范围");
    return;
  }
  /*
   * 选区 coverage 是文档分辨率、行距 = 文档宽；这就是 comp_mask 的原样形态
   * （refresh_selection 正是用同一组字段从引擎取回来的），直接转手即可，
   * 不在壳内重排或复制一份。
   */
  comp_mask mask{};
  mask.width = shell.doc.width;
  mask.height = shell.doc.height;
  mask.stride = static_cast<size_t>(shell.doc.width);
  mask.pixels = shell.doc.selection_mask.data();
  if (comp_layer_set_mask(shell.doc.document, row->id, &mask) != COMP_OK) {
    set_status(shell, L"建立图层蒙版失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, L"已从选区建立图层蒙版");
}

/*
 * 清除图层蒙版（S22）。无蒙版时如实说明而不是假成功——面板上一排图标里
 * 「有没有蒙版」必须是可核对的，假成功会让用户以为操作生效过。
 */
void panel_mask_remove(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (row->has_mask == 0) {
    set_status(shell, L"该图层没有蒙版");
    return;
  }
  if (comp_layer_set_mask(shell.doc.document, row->id, nullptr) != COMP_OK) {
    set_status(shell, L"清除蒙版失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, L"已清除图层蒙版");
}

/*
 * 图层蒙版 -> 选区（S22，上游 Load Mask as Selection 的 read 侧）。
 * 走 comp_selection_load_layer(COMP_SELECTION_LOAD_MASK)：内核按图层 transform
 * 把蒙版映射回文档坐标，壳不自己重采样——否则缩放/旋转过的图层会得到错位选区。
 */
void panel_mask_to_selection(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (row->has_mask == 0) {
    set_status(shell, L"该图层没有蒙版");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_load_layer(shell.doc.document, row->id, COMP_SELECTION_LOAD_MASK,
                               COMP_SELECTION_REPLACE, 0, &changed) != COMP_OK) {
    set_status(shell, L"载入蒙版为选区失败");
    return;
  }
  finish_selection_command(shell, L"已把图层蒙版载入为选区");
  shell.doc.needs_render = true;
}

/*
 * 剪贴蒙版开关（S22，Ctrl+Alt+G，上游 Create Clipping Mask 的对应键位）。
 *
 * 语义取上游默认档：**剪贴到下方紧邻的那一层**（base = 栈中下一层），因为上游
 * 的 ⌘G 就是「与下面一个图层互相剪贴」，不是让用户从一堆 id 里挑。再次按下取消。
 * 已处于剪贴态时先取消再决定是否重新挂——否则连按两次会在「取消/改挂」之间
 * 跳变，用户无法预测。
 *
 * 引擎已参与像素求值（composite_ops.cpp 的 clipping_factor 真的按来源图层算
 * 覆盖率），壳此前只是没调 setter。
 */
void panel_toggle_clipping(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (shell.doc.layers.size() < 2) {
    set_status(shell, L"至少需要两个图层才能剪贴");
    return;
  }
  const comp_layer_id self = row->id;
  /* 取消：已在剪贴态就解绑，一步撤销。 */
  if (row->clipping != 0) {
    if (comp_layer_set_clipping_source(shell.doc.document, self, 0) != COMP_OK) {
      set_status(shell, L"取消剪贴蒙版失败");
      return;
    }
    refresh_layers(shell);
    shell.doc.needs_render = true;
    refresh_history_status(shell, L"已取消剪贴蒙版");
    return;
  }
  /*
   * 挂靠：基底取「下方紧邻的同父图层」——这正是引擎成栈的条件
   * （filter_graph.cpp： siblings[i+1+clip_run]->clipping_source == layer->id，
   * 即基底在前、成员紧跟其后；数组序自下而上，所以下方 = 下标更小）。
   *
   * S22 这里取的是 self_index + 1（**上方**一层），并且守卫拒绝的是顶层、提示却
   * 写「最底层无法剪贴」。取反之后剪贴栈永远不成形：FilterGraph 路径画面不变，
   * 而 compositor.cpp 的单层视图只看 clipping_source != 0 就按基底求值，于是同一
   * 份文档在不同视图下画得不一样。换算现在全部交给 comp_ui::plan_clipping_base_index，
   * 由 test_ui_core.cpp 表驱动穷举（同父相邻、跨组、已有剪贴栈、边界）。
   */
  std::vector<comp_ui::LayerStackEntry> stack;
  stack.reserve(shell.doc.layers.size());
  for (const LayerRow& item : shell.doc.layers) {
    stack.push_back(comp_ui::LayerStackEntry{static_cast<int64_t>(item.id),
                                             static_cast<int64_t>(item.parent),
                                             static_cast<int64_t>(item.clipping)});
  }
  /* S44：行号先经 display 换成数组下标——树形面板里两者不再是 count-1-row。 */
  const comp_ui::ClippingPlan plan = comp_ui::plan_clipping_base_index(
      stack.data(), static_cast<int32_t>(stack.size()),
      comp_ui::layer_index_at_row(shell.doc.layer_display.data(), panel_row_count(shell),
                                  shell.doc.selected_layer));
  if (!plan.ok()) {
    switch (plan.target) {
      case comp_ui::ClippingTarget::kNoSiblingBelow:
        set_status(shell, L"下方没有同组图层可剪贴");
        break;
      case comp_ui::ClippingTarget::kBrokenStack:
        set_status(shell, L"下方图层的剪贴关系异常，无法并入");
        break;
      default:
        set_status(shell, L"无图层");
        break;
    }
    return;
  }
  const comp_layer_id base =
      shell.doc.layers[static_cast<size_t>(plan.base_index)].id;
  if (comp_layer_set_clipping_source(shell.doc.document, self, base) != COMP_OK) {
    set_status(shell, L"建立剪贴蒙版失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  wchar_t buffer[128];
  if (plan.joins_existing_stack) {
    wsprintfW(buffer, L"已并入剪贴栈，基底 #%I64u",
              static_cast<unsigned long long>(base));
  } else {
    wsprintfW(buffer, L"已剪贴到下方图层 #%I64u",
              static_cast<unsigned long long>(base));
  }
  refresh_history_status(shell, buffer);
}

/*
 * 组隔离开关（S22）。引擎侧语义见 ABI：隔离 = 子树先自合成一张透明缓冲再作为
 * 整体参与混合；穿透（默认）= 子层直接叠加到组外背景。两者都是**已参与像素
 * 求值**的真实差异（filter_graph.cpp 按 node.isolated 决定 child_backdrop）。
 *
 * 只对组图层有意义：非组行不显示也不响应——把引擎的「忽略」语义暴露成 UI
 * 异常（按了没反应）比置灰更糟。
 */
void panel_toggle_isolated(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (row->is_group == 0) {
    set_status(shell, L"只有图层组才能切换穿透/隔离");
    return;
  }
  const int32_t next = row->isolated != 0 ? 0 : 1;
  if (comp_layer_set_isolated(shell.doc.document, row->id, next) != COMP_OK) {
    set_status(shell, L"切换组隔离失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, next != 0 ? L"图层组：已隔离" : L"图层组：已穿透");
}

/*
 * 图层重排（S22）：按面板显示方向把选中图层上移 / 下移一层（或置顶 / 置底）。
 *
 * 换算全部交给 comp_ui::plan_layer_reorder 纯函数——S22 首版把这段逻辑内联在这里，
 * 而本层没有任何测试，于是「上移 / 下移两个分支整对写反」+「选中态按相反的假设
 * 跟随」两个错误互相抵消着进了主干：不报错、自检也过，只有真去排序的人才发现
 * 层往反方向跑。抽成纯函数后可以表驱动穷举（见 test_ui_core.cpp 的 S23 段）。
 *
 * 引擎语义（engine/domain/document.cpp `reorder_layer`）：before 是「插到哪一层
 * **之前**」，数组序即堆叠序、**尾 = 顶层**，before=0 = 同级末尾（顶层）。
 * 面板行序相反（行 0 = 顶层），纯函数负责这个翻转。
 *
 * 这是 T2 点名的缺口之一：comp_layer_reorder 在壳侧调用数为 0，用户只能
 * 进菜单一条一条读说明，完全没有拖不动、也按不动的排序入口。
 */
void panel_reorder_layer(ShellState& shell, int direction) {
  if (shell.doc.layers.empty()) {
    set_status(shell, L"无图层");
    return;
  }
  const int32_t count = static_cast<int32_t>(shell.doc.layers.size());
  const int32_t index = comp_ui::layer_index_at_row(
      shell.doc.layer_display.data(), panel_row_count(shell), shell.doc.selected_layer);
  const LayerRow* row = layer_row_at(shell, shell.doc.selected_layer);
  if (index < 0 || row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const comp_layer_id self = row->id;

  const std::vector<comp_ui::LayerStackEntry> entries = layer_stack_entries(shell);
  const comp_ui::ReorderPlan plan =
      comp_ui::plan_layer_reorder(entries.data(), count, index, direction);

  if (!plan.ok) {
    /* 已在端头：如实提示，不静默无操作（否则用户以为菜单坏了）。 */
    set_status(shell, plan.at_top ? L"已经是顶层" : L"已经是底层");
    return;
  }

  /*
   * before_index < 0 表示引擎约定的 before = 0（同级末尾 = 顶层）；否则锚点是
   * 那个数组下标对应的图层 id。锚点由纯函数保证与被移动层同级——跨父锚点会被
   * Document::reorder_layer 拒绝（返回 false）而不是静默错位。
   */
  comp_layer_id before = 0;
  if (plan.before_index >= 0) {
    before = shell.doc.layers[static_cast<size_t>(plan.before_index)].id;
  }

  if (comp_layer_reorder(shell.doc.document, self, before) != COMP_OK) {
    set_status(shell, L"调整图层顺序失败（跨组重排需与目标同组）");
    return;
  }
  refresh_layers(shell);
  /*
   * 选中态跟着该图层走：重排后数组与显示序都变了，selected_layer 是**面板行号**，
   * 按 id 重选。树形面板里行号还会因展开的子树而整体平移，所以不能沿用旧行号 +
   * 固定偏移——那类错误不报任何异常，只会让后续快捷键作用到别层。
   */
  const int32_t new_row = panel_row_of_id(shell, self);
  if (new_row >= 0) shell.doc.selected_layer = new_row;
  shell.doc.needs_render = true;
  if (direction == -2) {
    refresh_history_status(shell, L"图层置顶");
  } else if (direction < 0) {
    refresh_history_status(shell, L"图层上移一层");
  } else if (direction > 0) {
    refresh_history_status(shell, L"图层下移一层");
  } else {
    refresh_history_status(shell, L"图层置底");
  }
}

/*
 * 采样模式（S22）：comp_layer_set_sampling / get_sampling 是对上已有的 ABI
 * （0 High quality / 1 Nearest / 2 Smooth），且引擎真的按它决定缩放手感
 * （resample.cpp 的 sampling_from_abi）。壳此前一次都没调过。
 *
 * 与混合模式同样按「菜单直达 + 档位表」接：档位表在 ui_core，壳不复制命名。
 */
void panel_set_sampling(ShellState& shell, int mode) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const int next = comp_ui::clamp_sampling_mode(mode);
  if (comp_layer_set_sampling(shell.doc.document, row->id, static_cast<comp_sampling>(next)) !=
      COMP_OK) {
    set_status(shell, L"设置采样模式失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  const std::wstring name = widen_ascii(comp_ui::sampling_mode_name(next));
  wchar_t buffer[96];
  wsprintfW(buffer, L"采样模式：%s", name.c_str());
  refresh_history_status(shell, buffer);
}

/*
 * 直方图面板开关（S22）。
 *
 * 接通 comp_layer_histogram——上游 Levels 面板左半边就是这张图，而本壳的调整面板
 * 只有一排数字控件，用户看不到「这张图现在偏暗还是偏亮」，只能凭感觉拖参数。
 *
 * 三条刻意的口径：
 *  1. **像素不自己扫**：全部由引擎的只读入口给（预乘先反预乘再取亮度、按 alpha
 *     加权）。自己扫一遍必然与内核的 levels 口径漂移，面板曲线会系统性偏向暗部。
 *  2. 取不到就如实说原因，不画一张空图糊弄：组图层、空像素图层、16 位图层
 *     由引擎分别给 COMP_ERR_STATE / COMP_ERR_UNSUPPORTED。
 *  3. 面板开着时，任何改动图层 / 撤销 / 合并 / 打开文档的路径都会刷新它——
 *     见 refresh_histogram()，在各写入口后统一调用。
 */
void refresh_histogram(ShellState& shell) {
  shell.doc.histogram_bins.clear();
  shell.doc.histogram_layer = 0;
  if (!shell.doc.histogram_open) return;
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    shell.doc.histogram_open = false;
    return;
  }
  std::vector<double> bins(256, 0.0);
  const comp_result result =
      comp_layer_histogram(shell.doc.document, row->id, nullptr, 0, bins.data());
  if (result != COMP_OK) {
    /*
     * 取不到就关掉面板并说明原因：留着空面板比关掉更糟——用户会以为「这张图
     * 没有直方图」，而真实原因是这一层取不到（组/16 位/无像素）。
     */
    shell.doc.histogram_open = false;
    set_status(shell, L"直方图不可用：组图层、空图层或 16 位图层");
    return;
  }
  shell.doc.histogram_bins = std::move(bins);
  shell.doc.histogram_layer = row->id;
}

void panel_toggle_histogram(ShellState& shell) {
  shell.doc.histogram_open = !shell.doc.histogram_open;
  refresh_histogram(shell);
  shell.doc.needs_render = true;
  if (shell.doc.histogram_open && !shell.doc.histogram_bins.empty()) {
    refresh_history_status(shell, L"直方图：已显示");
  } else if (!shell.doc.histogram_open) {
    set_status(shell, L"直方图：已隐藏");
  }
}

/* ==================================================== S24：镜像 / 效果 / 区间 */

/*
 * 效果档位的中文名（S24）。
 *
 * 只保留**名称**：默认参数摘要原先也抄在这里，P0-2 起改由 ABI 参数表提供
 * （comp_layer_effect_param_at 的 label/default_value），壳不再自持任何一份
 * 数值——两份默认值迟早会漂移，而漂移的症状是「面板说 90°，落笔按 0° 算」。
 */
const wchar_t* effect_kind_label(int kind) {
  switch (comp_ui::clamp_effect_kind(kind)) {
    case 0: return L"描边";
    case 1: return L"投影";
    case 2: return L"颜色叠加";
    case 3: return L"内阴影";
    case 4: return L"外发光";
    case 5: return L"内发光";
    case 6: return L"斜面和浮雕";
    case 7: return L"缎光";
    case 8: return L"渐变叠加";
    case 9: return L"图案叠加";
    default: return L"描边";
  }
}

/*
 * 重建参数编辑缓冲（P0-2）。
 *
 * 与 reset_adjustment_values 同构：底色来自 ABI 参数表的 default_value；当该层
 * 已经有这个 kind 时，再按 settings 逐项回填（解析由引擎完成，壳不解析 JSON）。
 * use_existing = false 用于面板的「重置为默认」——只改缓冲，回车才落盘。
 */
void reset_effect_values(ShellState& shell, bool use_existing = true) {
  shell.doc.effect_values.clear();
  shell.doc.effect_param = 0;
  shell.doc.effect_input.clear();

  const uint32_t kind = static_cast<uint32_t>(comp_ui::clamp_effect_kind(shell.doc.effect_kind));
  shell.doc.effect_base_json = "{}";
  if (use_existing) {
    for (size_t i = 0; i < shell.doc.effect_kinds.size(); ++i) {
      if (shell.doc.effect_kinds[i] != kind) continue;
      if (i < shell.doc.effect_settings.size()) shell.doc.effect_base_json = shell.doc.effect_settings[i];
      break;
    }
  }

  size_t param_count = 0;
  if (comp_layer_effect_param_count(kind, &param_count) != COMP_OK) param_count = 0;
  for (size_t i = 0; i < param_count; ++i) {
    comp_effect_param param{};
    if (comp_layer_effect_param_at(kind, i, &param) != COMP_OK) {
      shell.doc.effect_values.emplace_back();
      continue;
    }
    bool filled = false;
    std::string text;
    if (use_existing) {
      if (param.type == COMP_PARAM_BOOL) {
        int32_t boolean = 0;
        if (comp_layer_effect_read_value(shell.doc.effect_base_json.c_str(), param.json_path, nullptr,
                                         &boolean) == COMP_OK) {
          text = boolean != 0 ? "1" : "0";
          filled = true;
        }
      } else {
        double number = 0.0;
        if (comp_layer_effect_read_value(shell.doc.effect_base_json.c_str(), param.json_path, &number,
                                         nullptr) == COMP_OK) {
          /*
           * .comp 里可能带着超出当前量程的旧值（引擎求值时 `ranged` 会夹取）。面板也夹：
           * 否则面板写 1000、引擎按 500 算，用户看到的「当前值」就是假的。
           */
          text = number_text(comp_ui::clamp_effect_param(number, param.min_value, param.max_value));
          filled = true;
        }
      }
    }
    if (!filled) {
      text = param.type == COMP_PARAM_BOOL ? (param.default_value != 0.0 ? "1" : "0")
                                           : number_text(param.default_value);
    }
    shell.doc.effect_values.push_back(text);
  }
}

/*
 * 效果回读（S24）：与 refresh_adjustments 同构，走 count + at 两段式。
 *
 * P0-2：settings 也一并回读，作为参数面板的合并底——面板只覆盖用户改过的键，
 * 上游 .comp 里新增的未知键必须原样带回去（effect_build_settings 的深合并契约）。
 * 键名/量程/缺省值仍然一个都不在壳里：全部由 ABI 参数表转出。
 */
void refresh_effects(ShellState& shell) {
  shell.doc.effect_kinds.clear();
  shell.doc.effect_settings.clear();
  const comp_layer_id id = shell.doc.document != nullptr ? selected_layer_id(shell) : 0;
  if (id == 0) {
    reset_effect_values(shell);
    return;
  }
  size_t count = 0;
  if (comp_layer_effect_count(shell.doc.document, id, &count) != COMP_OK) {
    reset_effect_values(shell);
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    uint32_t kind = 0;
    size_t need = 0;
    if (comp_layer_effect_at(shell.doc.document, id, i, &kind, nullptr, 0, &need) != COMP_OK) break;
    /* P0-2：settings 一并回读，作为参数面板「合并底」（未知键必须原样带回去）。 */
    std::vector<char> text(need + 1, '\0');
    if (comp_layer_effect_at(shell.doc.document, id, i, &kind, text.data(), text.size(), &need) !=
        COMP_OK) {
      break;
    }
    shell.doc.effect_kinds.push_back(kind);
    shell.doc.effect_settings.emplace_back(text.data(), need);
  }
  reset_effect_values(shell);
}

/*
 * 图层镜像（S24）。
 *
 * 引擎的 Layer::flip_x / flip_y 一直在合成里生效（layer_transform 把符号折进
 * 变换矩阵），但 ABI 只有 set 没有 get——壳读不到现值就只能「盲目取反」，而撤销、
 * 合并、.comp 读回都会改变文档状态，盲目取反会把「已翻转」翻回「未翻转」。
 * 因此 S24 先补 getter（comp_layer_get_flip），这里每次先读再写。
 *
 * 水平 / 垂直是**两个独立位**（PS 的「水平翻转」「垂直翻转」同款），不是三态循环：
 * 两次水平翻转必须回到原样，而不是变成垂直。
 */
void panel_toggle_flip(ShellState& shell, bool horizontal) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const comp_layer_id id = row->id;
  int32_t flip_x = 0;
  int32_t flip_y = 0;
  if (comp_layer_get_flip(shell.doc.document, id, &flip_x, &flip_y) != COMP_OK) {
    set_status(shell, L"读取镜像状态失败");
    return;
  }
  const int32_t next_x = horizontal ? (flip_x != 0 ? 0 : 1) : flip_x;
  const int32_t next_y = horizontal ? flip_y : (flip_y != 0 ? 0 : 1);
  if (comp_layer_set_flip(shell.doc.document, id, next_x, next_y) != COMP_OK) {
    set_status(shell, L"镜像失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  if (horizontal) {
    refresh_history_status(shell, next_x != 0 ? L"已水平翻转图层" : L"已取消水平翻转");
  } else {
    refresh_history_status(shell, next_y != 0 ? L"已垂直翻转图层" : L"已取消垂直翻转");
  }
}

/* ====================================== P1-2：图层锁 / 颜色标签 */

/*
 * 锁的中文名（P1-2）。位组合在引擎里是合法的，但本壳的菜单是**单选**：
 * 一项 = 一份完整 flags。位组合只可能来自别的 ABI 调用方或 .comp 文件，
 * 面板按「最先命中的位」显示（见 panel_lock_menu_flag），任何一次点选都会
 * 把它归一成单选值——这正是用户能理解的口径。
 */
const wchar_t* lock_label(uint32_t flags) {
  switch (flags) {
    case COMP_LAYER_LOCK_TRANSPARENT: return L"透明像素";
    case COMP_LAYER_LOCK_PIXELS: return L"图像像素";
    case COMP_LAYER_LOCK_POSITION: return L"位置";
    case COMP_LAYER_LOCK_ALL: return L"全部";
    default: return L"无";
  }
}

/*
 * 面板 / 菜单上该标成哪一个 radio：单选组只能勾一项。顺序刻意与 PS 菜单一致
 * （透明 / 图像 / 位置 / 全部），位组合时先命中的先报——例如
 * TRANSPARENT|POSITION 显示「透明像素」，点它就落到纯 TRANSPARENT。
 */
uint32_t panel_lock_menu_flag(uint32_t flags) {
  if ((flags & COMP_LAYER_LOCK_TRANSPARENT) != 0) return COMP_LAYER_LOCK_TRANSPARENT;
  if ((flags & COMP_LAYER_LOCK_PIXELS) != 0) return COMP_LAYER_LOCK_PIXELS;
  if ((flags & COMP_LAYER_LOCK_POSITION) != 0) return COMP_LAYER_LOCK_POSITION;
  if ((flags & COMP_LAYER_LOCK_ALL) != 0) return COMP_LAYER_LOCK_ALL;
  return COMP_LAYER_LOCK_NONE;
}

const wchar_t* color_label_name(uint32_t label) {
  switch (label) {
    case COMP_LAYER_COLOR_RED: return L"红";
    case COMP_LAYER_COLOR_ORANGE: return L"橙";
    case COMP_LAYER_COLOR_YELLOW: return L"黄";
    case COMP_LAYER_COLOR_GREEN: return L"绿";
    case COMP_LAYER_COLOR_BLUE: return L"蓝";
    case COMP_LAYER_COLOR_VIOLET: return L"紫";
    case COMP_LAYER_COLOR_GRAY: return L"灰";
    default: return L"无";
  }
}

/*
 * 图层锁（P1-2）。
 *
 * 引擎的锁位一直在拒绝写入口（P1-2 前半段接的是 ABI 闸），壳此前既读不到也
 * 设不了，于是「锁定图层」这个 PS 基础操作在本壳里不存在。这里做成 PS 同款
 * 的**点选即切**：菜单是单选组，点已勾选的那一项 = 解锁；点别的 = 换成那一项。
 * 不点「无」也能解锁，避免「想解锁却要先找无」这条反直觉路径。
 *
 * 锁不参与像素求值，所以不置 needs_render——只有面板标记与菜单勾选变化。
 */
void panel_set_lock(ShellState& shell, uint32_t flags) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const comp_layer_id id = row->id;
  uint32_t current = 0;
  if (comp_layer_get_lock(shell.doc.document, id, &current) != COMP_OK) {
    set_status(shell, L"读取图层锁失败");
    return;
  }
  uint32_t next = flags & COMP_LAYER_LOCK_MASK;
  /* 单选组语义：再点一次已生效的锁 = 解锁。 */
  if (next != COMP_LAYER_LOCK_NONE && panel_lock_menu_flag(current) == next) {
    next = COMP_LAYER_LOCK_NONE;
  }
  if (comp_layer_set_lock(shell.doc.document, id, next) != COMP_OK) {
    set_status(shell, L"设置图层锁失败");
    return;
  }
  refresh_layers(shell);
  wchar_t buffer[128];
  if (next == COMP_LAYER_LOCK_NONE) {
    wsprintfW(buffer, L"已解锁图层：%s", lock_label(panel_lock_menu_flag(current)));
  } else {
    wsprintfW(buffer, L"图层已锁定：%s", lock_label(next));
  }
  refresh_history_status(shell, buffer);
}

/*
 * 颜色标签（P1-2）。纯元数据：只影响面板配色与行内色块，不参与合成、
 * 不推进缓存——所以同样不置 needs_render。label 越界由 ABI 拒绝，这里先夹一次。
 */
void panel_set_color_label(ShellState& shell, uint32_t label) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (label >= COMP_LAYER_COLOR_COUNT) label = COMP_LAYER_COLOR_NONE;
  if (comp_layer_set_color_label(shell.doc.document, row->id, label) != COMP_OK) {
    set_status(shell, L"设置颜色标签失败");
    return;
  }
  refresh_layers(shell);
  wchar_t buffer[96];
  wsprintfW(buffer, L"颜色标签：%s", color_label_name(label));
  refresh_history_status(shell, buffer);
}

/*
 * 链接图层（P1-2 第三批）。
 *
 * PS 的「链接图层」是勾选式：选中的多个层已经同组 -> 解除；否则并成一组。
 * 这里沿用同一点选口径，但**只作用在选中的像素/文字层上**：
 *   - 组图层跳过（引擎明确不支持，且组的几何由子树合成，没有可联动的矩形）；
 *   - 有效选中层少于 2 个 -> 直接拒绝并提示（不偷偷对主选中层做单层操作）；
 *   - 判定用引擎回读的 link_group，不信任面板缓存（撤销后缓存会滞后一帧）。
 *
 * 链接是纯关系元数据，但会进撤销栈，所以用 refresh_history_status 刷新
 * 「撤销」标题——与设置锁/色标同一条路径，不置 needs_render。
 */
void panel_toggle_link(ShellState& shell) {
  std::vector<comp_layer_id> batch = panel_selection_ids(shell, false);
  std::vector<comp_layer_id> targets;
  targets.reserve(batch.size());
  for (comp_layer_id id : batch) {
    const LayerRow* row = nullptr;
    for (const LayerRow& candidate : shell.doc.layers) {
      if (candidate.id == id) {
        row = &candidate;
        break;
      }
    }
    if (row == nullptr || row->is_group != 0) continue;
    targets.push_back(id);
  }
  if (targets.size() < 2) {
    set_status(shell, L"链接图层需要至少选中两个图层（不含图层组）");
    return;
  }

  uint32_t shared = 0;
  bool all_linked = true;
  for (comp_layer_id id : targets) {
    uint32_t group = 0;
    if (comp_layer_get_link(shell.doc.document, id, &group) != COMP_OK) {
      set_status(shell, L"读取图层链接失败");
      return;
    }
    if (group == 0) {
      all_linked = false;
    } else if (shared == 0) {
      shared = group;
    } else if (shared != group) {
      all_linked = false;
    }
  }

  wchar_t buffer[128];
  if (all_linked) {
    int32_t changed = 0;
    if (comp_layer_unlink(shell.doc.document, targets.data(), targets.size(), &changed) != COMP_OK) {
      set_status(shell, L"解除图层链接失败");
      return;
    }
    refresh_layers(shell);
    wsprintfW(buffer, L"已解除 %d 个图层的链接", static_cast<int>(targets.size()));
    refresh_history_status(shell, buffer);
    return;
  }

  uint32_t group = 0;
  int32_t changed = 0;
  if (comp_layer_link(shell.doc.document, targets.data(), targets.size(), &group, &changed) !=
      COMP_OK) {
    set_status(shell, L"链接图层失败");
    return;
  }
  refresh_layers(shell);
  if (changed == 0) {
    wsprintfW(buffer, L"已在同一链接组：%u", group);
  } else {
    wsprintfW(buffer, L"已链接 %d 个图层（组 %u）", static_cast<int>(targets.size()), group);
  }
  refresh_history_status(shell, buffer);
}

/*
 * 图层样式的拷贝 / 粘贴（P1-2 第四批）。
 *
 * 「图层样式」在本引擎里就是层的效果栈，所以这一对命令不新增文档语义：
 *   - 引擎负责把效果栈导出成与 `.comp` engineEffects 同形的报文（kind 顺序、
 *     settings 原文都不变形，未知键也带着），外壳只当剪贴板；
 *   - 粘贴是**整份替换**而不是合并：目标层原有样式全被剪贴板取代，`[]` 即清空；
 *   - 多选粘贴包在一个事务里 = 一步撤销（与批量切效果同口径）。
 *
 * 拒绝面照抄引擎：组图层两侧都不支持（组的样式从不参与求值），最高版本才
 * 认识的 kind 导出时直接拒绝，不产出粘不回来的剪贴板。
 */
void panel_copy_layer_style(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"无图层");
    return;
  }
  size_t need = 0;
  comp_result result = comp_layer_export_style(shell.doc.document, id, nullptr, 0, &need);
  if (result == COMP_ERR_UNSUPPORTED) {
    set_status(shell, L"该图层不能拷贝样式（图层组没有求值的样式）");
    return;
  }
  if (result != COMP_OK) {
    set_status(shell, L"导出图层样式失败");
    return;
  }
  std::vector<char> buffer(need + 1, '\0');
  result = comp_layer_export_style(shell.doc.document, id, buffer.data(), buffer.size(), &need);
  if (result != COMP_OK) {
    set_status(shell, L"导出图层样式失败");
    return;
  }
  shell.layer_style_clipboard.assign(buffer.data(), need);
  refresh_layers(shell);
  wchar_t message[160];
  wsprintfW(message, L"已拷贝图层样式（%u 字节）", static_cast<unsigned>(need));
  set_status(shell, message);
}

void panel_paste_layer_style(ShellState& shell) {
  if (shell.layer_style_clipboard.empty()) {
    set_status(shell, L"剪贴板里没有图层样式（先拷贝一次）");
    return;
  }
  const std::vector<comp_layer_id> batch = panel_selection_ids(shell, true);
  if (batch.empty()) {
    set_status(shell, L"无图层");
    return;
  }

  /* 单层粘贴走引擎的隐式事务；多选必须显式包一层，才能一步撤销整批。 */
  comp_txn txn = nullptr;
  if (batch.size() > 1) {
    if (comp_txn_begin(shell.doc.document, "Paste Layer Style", &txn) != COMP_OK) {
      set_status(shell, L"粘贴图层样式：无法开始事务");
      return;
    }
  }
  size_t changed = 0;
  size_t same = 0;
  size_t groups = 0;
  size_t failed = 0;
  for (comp_layer_id id : batch) {
    int32_t at = 0;
    const comp_result result = comp_layer_import_style(
        shell.doc.document, id, shell.layer_style_clipboard.c_str(), &at);
    if (result == COMP_OK) {
      if (at != 0) {
        ++changed;
      } else {
        ++same; /* 已经是这份样式：引擎不落撤销步，这里也不计入改动。 */
      }
    } else if (result == COMP_ERR_UNSUPPORTED) {
      ++groups;
    } else {
      ++failed;
    }
  }

  if (txn != nullptr) {
    if (changed == 0) {
      comp_txn_abort(txn);
      wchar_t empty_message[224];
      wsprintfW(empty_message, L"粘贴图层样式：选中的图层没有改动%s",
                groups > 0 ? L"（组图层不参与样式）" : L"");
      set_status(shell, empty_message);
      return;
    }
    if (comp_txn_commit(txn) != COMP_OK) {
      set_status(shell, L"粘贴图层样式：提交失败");
      return;
    }
  }

  refresh_effects(shell);
  refresh_layers(shell);
  if (changed > 0) shell.doc.needs_render = true;
  wchar_t buffer[320];
  if (changed == 0) {
    wsprintfW(buffer, L"粘贴图层样式：没有改动%s",
              groups > 0 ? L"（组图层不参与样式）" : L"（样式已相同）");
    set_status(shell, buffer);
    return;
  }
  wsprintfW(buffer, L"已为 %u 个图层粘贴样式", static_cast<unsigned>(changed));
  if (same > 0) {
    wsprintfW(buffer + lstrlenW(buffer), L"（%u 个已相同）", static_cast<unsigned>(same));
  }
  if (groups > 0) {
    wsprintfW(buffer + lstrlenW(buffer), L" · 跳过 %u 个组图层", static_cast<unsigned>(groups));
  }
  if (failed > 0) {
    wsprintfW(buffer + lstrlenW(buffer), L" · %u 个失败", static_cast<unsigned>(failed));
  }
  refresh_history_status(shell, buffer);
}

/*
 * 效果开关（S24）。
 *
 * 引擎侧 9 种效果（描边/投影/颜色叠加/内阴影/外发光/内发光/斜面和浮雕/缎光/
 * 渐变叠加）早已参与像素求值，
 * 但 ABI 只有 set_effect / remove_effect，壳无法知道「这一层现在有哪些效果」。
 * 加了 effect_count / effect_at 之后，这里可以做**幂等切换**：勾选 = 已启用。
 *
 * 已有同 kind 的效果时**不覆盖**：用户可能是 .comp 读回来的、带自定义参数的效果，
 * 盖上 "{}" 会把他的参数静默抹掉（这是最典型的数据丢失）。要去掉就用「移除」。
 */

/* 单层的效果清单查询（count + at 两段式，只问 kind）。批量切换时要逐层问自己，
 * 不能用壳里缓存的 shell.doc.effect_kinds——那是主选中层的副本。 */
bool layer_has_effect(ShellState& shell, comp_layer_id id, uint32_t kind) {
  size_t count = 0;
  if (comp_layer_effect_count(shell.doc.document, id, &count) != COMP_OK) return false;
  for (size_t i = 0; i < count; ++i) {
    uint32_t at = 0;
    size_t need = 0;
    if (comp_layer_effect_at(shell.doc.document, id, i, &at, nullptr, 0, &need) != COMP_OK) break;
    if (at == kind) return true;
  }
  return false;
}

/*
 * §6.65 多选批量切换效果：主选中层决定「加还是减」（菜单勾选就是看主选中层），
 * 整个选中集合执行同一个动作，一个事务 = 一步撤销。
 *
 * 组图层必须跳过，不能「顺着搬」：`filter_graph` 的 kGroup 分支只合成子树，
 * 从不读组自己的 effects，写进去是静默无操作（与组 transform 同一类假象）。
 */
void panel_toggle_effect_multi(ShellState& shell, int clamped, const wchar_t* label) {
  const std::vector<comp_layer_id> batch = panel_selection_ids(shell, true);
  const comp_layer_id primary = selected_layer_id(shell);
  if (primary == 0 || batch.size() < 2) return;
  const bool removing = layer_has_effect(shell, primary, static_cast<uint32_t>(clamped));
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, removing ? "Remove Effect From Layers" : "Add Effect To Layers",
                     &txn) != COMP_OK) {
    set_status(shell, L"切换效果：无法开始事务");
    return;
  }
  size_t changed = 0;
  size_t skipped = 0;
  size_t groups = 0;
  size_t failed = 0;
  for (comp_layer_id id : batch) {
    const LayerRow* row = nullptr;
    for (const LayerRow& item : shell.doc.layers) {
      if (item.id == id) {
        row = &item;
        break;
      }
    }
    if (row == nullptr) {
      ++failed;
      continue;
    }
    if (row->is_group != 0) {
      ++groups;
      continue;
    }
    const bool present = layer_has_effect(shell, id, static_cast<uint32_t>(clamped));
    if (present == removing) {
      const comp_result result =
          removing ? comp_layer_remove_effect(shell.doc.document, id, static_cast<uint32_t>(clamped))
                   : comp_layer_set_effect(shell.doc.document, id, static_cast<uint32_t>(clamped), "{}");
      if (result == COMP_OK) {
        ++changed;
      } else {
        ++failed;
      }
    } else {
      ++skipped; /* 本来就在目标状态：不写、也不占撤销步。 */
    }
  }
  if (changed == 0) {
    comp_txn_abort(txn);
    wchar_t empty_buffer[224];
    wsprintfW(empty_buffer, L"切换效果：选中的图层没有改动%s", groups > 0 ? L"（组图层不参与效果）" : L"");
    set_status(shell, empty_buffer);
    return;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"切换效果：提交失败");
    return;
  }
  refresh_effects(shell);
  refresh_layers(shell);
  shell.doc.needs_render = true;
  wchar_t buffer[320];
  wsprintfW(buffer, removing ? L"已为 %u 个图层移除效果：%s" : L"已为 %u 个图层添加效果：%s",
            static_cast<unsigned>(changed), label);
  if (skipped > 0) {
    wsprintfW(buffer + lstrlenW(buffer), L"（%u 个已在目标状态）", static_cast<unsigned>(skipped));
  }
  if (groups > 0) {
    /* 说清楚为什么跳过：组的效果不会被求值，写进去只会让人以为生效了。 */
    wsprintfW(buffer + lstrlenW(buffer), L" · 跳过 %u 个组图层（引擎不按组求值效果）",
              static_cast<unsigned>(groups));
  }
  if (failed > 0) {
    wsprintfW(buffer + lstrlenW(buffer), L" · %u 个失败", static_cast<unsigned>(failed));
  }
  refresh_history_status(shell, buffer);
}

void panel_toggle_effect(ShellState& shell, int kind) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const int clamped = comp_ui::clamp_effect_kind(kind);
  if (!shell.doc.extra_selection.empty()) {
    panel_toggle_effect_multi(shell, clamped, effect_kind_label(clamped));
    return;
  }
  if (row->is_group != 0) {
    /*
     * §6.65 组图层不能加效果：`filter_graph` 的 kGroup 分支只合成子树，从不读组
     * 自己的 effects——写进去画面一个像素都不会变。如实拒绝，不制造「已添加」的假象。
     */
    set_status(shell, L"效果：组图层不参与效果求值（请选中组内成员）");
    return;
  }
  const comp_layer_id id = row->id;
  const bool present = layer_has_effect(shell, id, static_cast<uint32_t>(clamped));
  wchar_t buffer[256];
  if (present) {
    if (comp_layer_remove_effect(shell.doc.document, id, static_cast<uint32_t>(clamped)) != COMP_OK) {
      set_status(shell, L"移除效果失败");
      return;
    }
    wsprintfW(buffer, L"已移除效果：%s", effect_kind_label(clamped));
  } else {
    /*
     * "{}" = 全部走引擎默认参数（键缺失即默认，见 effect_eval.cpp 的 read_*）。
     * 壳内不拼一份默认 JSON：两份默认值必然有一天不一致，且引擎改默认值时壳不会跟着改。
     *
     * P0-2：添加后直接打开参数面板——面板里的值就是 ABI 参数表的缺省值，用户
     * 点完「投影」若看不出变化（黑底黑阴影），至少能立刻看到距离/角度在哪调。
     */
    if (comp_layer_set_effect(shell.doc.document, id, static_cast<uint32_t>(clamped), "{}") != COMP_OK) {
      set_status(shell, L"添加效果失败");
      return;
    }
    shell.doc.effect_open = true;
    shell.doc.effect_kind = clamped;
    wsprintfW(buffer, L"已添加效果：%s（参数面板已打开：Tab 选参数 · -/= 调值 · 回车应用）",
              effect_kind_label(clamped));
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, buffer);
}

/*
 * 清除选中图层的全部效果（S24）。ABI 没有批量清除，只能逐个 remove_effect，
 * 因此会产生 N 条撤销记录——如实说明，不假装是一步（引擎侧要一步得加新 ABI，
 * 那是 S25 的事，不能在壳里绕）。
 */
void panel_clear_effects(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (shell.doc.effect_kinds.empty()) {
    set_status(shell, L"该图层没有效果");
    return;
  }
  const comp_layer_id id = row->id;
  const std::vector<uint32_t> kinds = shell.doc.effect_kinds;
  int removed = 0;
  for (uint32_t kind : kinds) {
    if (comp_layer_remove_effect(shell.doc.document, id, kind) == COMP_OK) ++removed;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  wchar_t buffer[128];
  wsprintfW(buffer, L"已清除 %d 个效果（每个效果一步撤销）", removed);
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------- 效果参数面板（P0-2） */

/*
 * 面板的全部「知识」只有：当前 kind、当前参数序号、参数表给的 label/量程/缺省值。
 * 键名、量程、缺省值一个都不在壳里——这就是 P0-2 拆掉 effect_kind_defaults() 的
 * 目的。写路径只有 comp_layer_set_effect 一条：它本来就按 kind 幂等替换，
 * 「添加」和「修改参数」因此走同一个动作，不需要两套代码。
 */

bool effect_present(const ShellState& shell, int kind) {
  return comp_ui::effect_list_contains(
      shell.doc.effect_kinds.empty() ? nullptr : shell.doc.effect_kinds.data(),
      static_cast<int32_t>(shell.doc.effect_kinds.size()), kind);
}

void open_effect_panel(ShellState& shell, int kind) {
  if (selected_row(shell) == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (shell.doc.color_range_open) color_range_cancel(shell);
  /* 右栏一次只挂一个面板：滤镜、路径、历史与效果共用同一块矩形，同时打开会互相遮住。 */
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell); /* 滤镜预览帧不能留在新面板底下 */
  shell.doc.path_open = false;
  shell.doc.history_open = false;
  channel_panel_close(shell);
  shell.doc.effect_open = true;
  shell.doc.effect_kind = comp_ui::clamp_effect_kind(kind);
  reset_effect_values(shell);
  wchar_t buffer[224];
  wsprintfW(buffer, L"效果参数：%s（%s）Tab 选参数 · -/= 调值 · 回车应用 · Esc 关闭",
            effect_kind_label(shell.doc.effect_kind),
            effect_present(shell, shell.doc.effect_kind) ? L"本层已启用" : L"本层未启用，回车添加");
  set_status(shell, buffer);
}

void close_effect_panel(ShellState& shell) {
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  set_status(shell, L"已关闭效果参数面板");
}

/* `,` / `.`：在 10 个 kind 之间循环（不必先启用就能预览参数表；回车即添加）。 */
void panel_effect_cycle_kind(ShellState& shell, int32_t delta) {
  if (selected_row(shell) == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const int32_t total = comp_ui::kEffectKindCount;
  int32_t kind = shell.doc.effect_kind + delta;
  if (kind < 0) kind = total - 1;
  if (kind >= total) kind = 0;
  shell.doc.effect_kind = kind;
  shell.doc.effect_open = true;
  reset_effect_values(shell);
  wchar_t buffer[200];
  wsprintfW(buffer, L"效果 %d/%d：%s（%s）", kind + 1, total, effect_kind_label(kind),
            effect_present(shell, kind) ? L"本层已启用" : L"本层未启用，回车添加");
  set_status(shell, buffer);
}

/* 聚焦某一行参数；越界夹取（与面板行几何共用同一个序号空间）。 */
void panel_effect_focus_row(ShellState& shell, int32_t row) {
  const int32_t count = static_cast<int32_t>(shell.doc.effect_values.size());
  if (count == 0) {
    set_status(shell, L"该效果没有可编辑参数");
    return;
  }
  if (row < 0) row = 0;
  if (row >= count) row = count - 1;
  shell.doc.effect_param = row;
  shell.doc.effect_input.clear();
  comp_effect_param param{};
  if (comp_layer_effect_param_at(static_cast<uint32_t>(shell.doc.effect_kind),
                                 static_cast<size_t>(row), &param) != COMP_OK) {
    return;
  }
  wchar_t buffer[220];
  wsprintfW(buffer, L"参数 %d/%d：%s = %s", row + 1, count,
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.doc.effect_values[static_cast<size_t>(row)].c_str()).c_str());
  set_status(shell, buffer);
}

void panel_effect_move_param(ShellState& shell, int32_t delta) {
  panel_effect_focus_row(shell, shell.doc.effect_param + delta);
}

/*
 * 步长由量程决定（ui_core::step_effect_param）：0..1 的归一量走 0.05，
 * 像素/角度走 1。夹取也在 ui_core，ABI 只做最后一道拒绝。
 */
void panel_effect_step_value(ShellState& shell, int32_t direction) {
  const size_t index = static_cast<size_t>(shell.doc.effect_param);
  if (index >= shell.doc.effect_values.size()) {
    set_status(shell, L"该效果没有可编辑参数");
    return;
  }
  comp_effect_param param{};
  if (comp_layer_effect_param_at(static_cast<uint32_t>(shell.doc.effect_kind), index, &param) !=
      COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    shell.doc.effect_values[index] = shell.doc.effect_values[index] == "1" ? "0" : "1";
  } else {
    const double current = std::atof(shell.doc.effect_values[index].c_str());
    shell.doc.effect_values[index] = number_text(comp_ui::step_effect_param(
        current, direction, param.min_value, param.max_value));
  }
  shell.doc.effect_input.clear();
  wchar_t buffer[240];
  wsprintfW(buffer, L"%s = %s （范围 [%s, %s]）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.doc.effect_values[index].c_str()).c_str(),
            widen_ascii(number_text(param.min_value).c_str()).c_str(),
            widen_ascii(number_text(param.max_value).c_str()).c_str());
  set_status(shell, buffer);
}

/* 数字键开始输入：布尔参数不进入文本态，直接切换（与调整面板同规矩）。 */
void panel_effect_begin_input(ShellState& shell, wchar_t character) {
  const size_t index = static_cast<size_t>(shell.doc.effect_param);
  if (index >= shell.doc.effect_values.size()) {
    set_status(shell, L"该效果没有可编辑参数");
    return;
  }
  comp_effect_param param{};
  if (comp_layer_effect_param_at(static_cast<uint32_t>(shell.doc.effect_kind), index, &param) !=
      COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    panel_effect_step_value(shell, 1);
    return;
  }
  shell.doc.effect_input.assign(1, character);
  set_status(shell, L"输入中：回车提交并应用，Esc 取消");
}

void panel_effect_backspace(ShellState& shell) {
  if (!shell.doc.effect_input.empty()) shell.doc.effect_input.pop_back();
}

void panel_effect_commit_input(ShellState& shell) {
  if (shell.doc.effect_input.empty()) {
    set_status(shell, L"未在输入状态（数字键开始输入）");
    return;
  }
  const size_t index = static_cast<size_t>(shell.doc.effect_param);
  if (index >= shell.doc.effect_values.size()) {
    shell.doc.effect_input.clear();
    return;
  }
  comp_effect_param param{};
  if (comp_layer_effect_param_at(static_cast<uint32_t>(shell.doc.effect_kind), index, &param) !=
      COMP_OK) {
    shell.doc.effect_input.clear();
    return;
  }
  const std::string text(shell.doc.effect_input.begin(), shell.doc.effect_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  /* 面板夹取（与滤镜同口径）；不夹就等着 ABI 拒绝，状态栏只能报「参数不合法」。 */
  shell.doc.effect_values[index] =
      number_text(comp_ui::clamp_effect_param(value, param.min_value, param.max_value));
  shell.doc.effect_input.clear();
  wchar_t buffer[192];
  wsprintfW(buffer, L"已设置 %s = %s", widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.doc.effect_values[index].c_str()).c_str());
  set_status(shell, buffer);
}

/*
 * Backspace（非输入态）= 缓冲回引擎缺省；仍然要回车才落盘。
 *
 * 注意保留 effect_base_json：`reset_effect_values(..., false)` 会把它清成 "{}"，
 * 那样回车时就用「纯缺省」重建整串，把本层 settings 里的未知键（上游新增字段）
 * 一起抹掉。重置的是**参数值**，不是这一层的 settings。
 */
void panel_effect_reset_values(ShellState& shell) {
  const std::string base = shell.doc.effect_base_json;
  reset_effect_values(shell, false);
  shell.doc.effect_base_json = base;
  set_status(shell, L"效果参数已重置为默认值（回车应用，Esc 关闭）");
}

/*
 * settings JSON 由引擎组装并在既有 settings 上合并（壳内不拼 JSON、不解析 JSON）。
 * 返回的字符串里未知键原样保留——这是 .comp 前向兼容的关键。
 */
bool build_effect_settings_json(const ShellState& shell, std::string* out_json) {
  std::vector<const char*> values;
  values.reserve(shell.doc.effect_values.size());
  for (const std::string& text : shell.doc.effect_values) values.push_back(text.c_str());

  const uint32_t kind = static_cast<uint32_t>(comp_ui::clamp_effect_kind(shell.doc.effect_kind));
  const char* const* data = values.empty() ? nullptr : values.data();
  size_t need = 0;
  if (comp_layer_effect_build_settings(kind, data, values.size(),
                                       shell.doc.effect_base_json.c_str(), nullptr, 0, &need) !=
      COMP_OK) {
    return false;
  }
  std::string json(need + 1, '\0');
  if (comp_layer_effect_build_settings(kind, data, values.size(),
                                       shell.doc.effect_base_json.c_str(), &json[0], json.size(),
                                       &need) != COMP_OK) {
    return false;
  }
  json.resize(need);
  if (out_json != nullptr) *out_json = json;
  return true;
}

/*
 * 回车 = 落盘。set_effect 按 kind 幂等替换，因此「添加」与「改参数」是同一个动作；
 * 每次调用产生一步隐式撤销（与菜单里的效果开关同口径）。
 */
void panel_apply_effect(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  std::string json;
  if (!build_effect_settings_json(shell, &json)) {
    set_status(shell, L"应用被拒：参数不合法");
    return;
  }
  const uint32_t kind = static_cast<uint32_t>(comp_ui::clamp_effect_kind(shell.doc.effect_kind));
  const comp_result result = comp_layer_set_effect(shell.doc.document, id, kind, json.c_str());
  shell.doc.needs_render = true;

  /* refresh_layers 会把编辑缓冲重建（并清焦点），这里把焦点恢复到同一行。 */
  const int32_t focused = shell.doc.effect_param;
  refresh_layers(shell);
  if (!shell.doc.effect_values.empty()) {
    if (focused >= 0 && focused < static_cast<int32_t>(shell.doc.effect_values.size())) {
      shell.doc.effect_param = focused;
    }
  }

  wchar_t buffer[200];
  wsprintfW(buffer, L"%s：%s", result == COMP_OK ? L"已应用效果" : L"应用效果失败",
            effect_kind_label(static_cast<int>(kind)));
  refresh_history_status(shell, buffer);
}

/* 直方图当前区间 → 状态栏文案（夹取后的实际值，不写「理想值」）。 */
void histogram_view_status(ShellState& shell) {
  if (comp_ui::histogram_view_is_full(shell.doc.histogram_view)) {
    set_status(shell, L"直方图区间：全幅 0–255（双击复位）");
    return;
  }
  wchar_t buffer[128];
  wsprintfW(buffer, L"直方图区间：亮度 %d–%d（双击复位）",
            static_cast<int>(shell.doc.histogram_view.lo + 0.5),
            static_cast<int>(shell.doc.histogram_view.hi + 0.5));
  set_status(shell, buffer);
}

void panel_histogram_reset(ShellState& shell) {
  if (!shell.doc.histogram_open) {
    set_status(shell, L"直方图未打开（F6 打开）");
    return;
  }
  shell.doc.histogram_view = comp_ui::HistogramView{};
  shell.doc.needs_render = true;
  histogram_view_status(shell);
}

void panel_histogram_zoom(ShellState& shell, double factor, double focus_bin) {
  if (!shell.doc.histogram_open) return;
  shell.doc.histogram_view = comp_ui::zoom_histogram_view(shell.doc.histogram_view, focus_bin, factor);
  shell.doc.needs_render = true;
  histogram_view_status(shell);
}

/*
 * 框选松手：把拖出的横向区间变成新的可视窗口。
 * 位移小于 4px 视为单击（不放大）——否则「点一下图」会突然缩放到 8 个 bin 的
 * 极限，看起来像图坏了。夹取由 ui_core 统一做，这里不重算。
 */
void panel_histogram_commit_drag(ShellState& shell, const comp_ui::Rect& plot) {
  const int32_t delta = shell.doc.histogram_drag_to_x - shell.doc.histogram_drag_from_x;
  if (delta >= -3 && delta <= 3) return;
  shell.doc.histogram_view = comp_ui::histogram_window_from_drag(
      plot, shell.doc.histogram_view, static_cast<double>(shell.doc.histogram_drag_from_x),
      static_cast<double>(shell.doc.histogram_drag_to_x));
  shell.doc.needs_render = true;
  histogram_view_status(shell);
}

/*
 * 拖行重排落点（S24 + P0-1）：从面板行 from_shown 拖到 to_shown。
 *
 * 规划（行→数组下标换算、同父校验、锚点选择）全在 comp_ui::plan_layer_drag；
 * 这里只做「把纯函数的结果翻译成一次 comp_layer_reorder」。
 *
 * P0-1 起多了两条跨父级路径（面板仍是扁平列表，只能在落点几何上区分）：
 *   - 落点是组行的中间带（drop_into_group_item=true）  -> 拖进该组（末尾）；
 *   - 其它跨父级落点且目标行在根级                 -> 移出到根级，放到目标行下方。
 * 其余跨父级（目标在组内、拖到自己子树里）仍然明确拒绝：静默改父级会让图层
 * 「消失」在别的组里。所有路径都只发一次 comp_layer_move，一步撤销。
 */
void panel_drop_layer(ShellState& shell, int32_t from_shown, int32_t to_shown,
                      bool drop_into_group_item) {
  const int32_t count = static_cast<int32_t>(shell.doc.layers.size());
  if (count <= 0) return;
  const LayerRow* source = layer_row_at(shell, from_shown);
  const LayerRow* target_row = layer_row_at(shell, to_shown);
  if (source == nullptr || target_row == nullptr) return;
  const comp_layer_id self = source->id;
  if (drop_into_group_item && to_shown != from_shown) {
    const LayerRow& target = *target_row;
    if (target.is_group != 0 && target.id != self) {
      if (comp_layer_move(shell.doc.document, self, target.id, 0) != COMP_OK) {
        /* 常见原因：把自己/自己的祖先拖进自己的子树。引擎里已拒绝，原样透出。 */
        set_status(shell, L"移入组失败（不能把组拖进自己的子树）");
        return;
      }
      refresh_layers(shell);
      panel_select_layer_id(shell, self);
      shell.doc.needs_render = true;
      wchar_t buffer[160];
      wsprintfW(buffer, L"已把图层 #%I64u 移入组 #%I64u",
                static_cast<unsigned long long>(self),
                static_cast<unsigned long long>(target.id));
      refresh_history_status(shell, buffer);
      return;
    }
  }
  const std::vector<comp_ui::LayerStackEntry> entries = layer_stack_entries(shell);
  const comp_ui::DragPlan plan = comp_ui::plan_layer_drag(
      entries.data(), count, shell.doc.layer_display.data(), panel_row_count(shell), from_shown,
      to_shown);
  switch (plan.target) {
    case comp_ui::DragTarget::kSameRow:
      set_status(shell, L"拖回原位，图层顺序未变");
      return;
    case comp_ui::DragTarget::kCrossParent: {
      /* 跨父级且不是「拖进组」：只有目标行在根级时解释成「移出组」。 */
      const LayerRow& target = *target_row;
      const LayerRow& dragged = *source;
      if (target.parent == 0 && dragged.parent != 0) {
        if (comp_layer_move(shell.doc.document, self, 0, target.id) != COMP_OK) {
          set_status(shell, L"移出组失败（未修改任何数据）");
          return;
        }
        refresh_layers(shell);
        panel_select_layer_id(shell, self);
        shell.doc.needs_render = true;
        wchar_t buffer[160];
        wsprintfW(buffer, L"已把图层 #%I64u 移出组到根级",
                  static_cast<unsigned long long>(self));
        refresh_history_status(shell, buffer);
        return;
      }
      set_status(shell, L"跨组拖拽需落在组行中间（拖进组）或根级行（拖出组），未修改任何数据");
      return;
    }
    case comp_ui::DragTarget::kInvalid:
      return;
    case comp_ui::DragTarget::kOk:
      break;
  }
  comp_layer_id before = 0;
  if (plan.before_index >= 0) {
    before = shell.doc.layers[static_cast<size_t>(plan.before_index)].id;
  }
  if (comp_layer_reorder(shell.doc.document, self, before) != COMP_OK) {
    set_status(shell, L"拖行重排失败（跨组需与目标同组）");
    return;
  }
  refresh_layers(shell);
  /*
   * 选中态跟着被拖动的那一层走：按 id 重选。树形面板里落点行号会因展开的子树
   * 整体平移（next_row 只在扁平文档里精确），沿用行号会选中别的层。
   */
  panel_select_layer_id(shell, self);
  shell.doc.needs_render = true;
  const int32_t landed = panel_row_of_id(shell, self);
  wchar_t buffer[128];
  wsprintfW(buffer, L"拖行重排：第 %d 行 → 第 %d 行", from_shown + 1,
            landed >= 0 ? landed + 1 : to_shown + 1);
  refresh_history_status(shell, buffer);
}

/*
 * 图层平移（S21）：对选中图层做整层几何位移——与 Move 工具的选区搬移
 * （comp_selection_move_pixels，只动当前图层的选区像素）是两件事，勿混。
 *
 * 前置：必须先 comp_layer_get_transform 读到现值再步进，不能凭壳内缓存——
 * 撤销、合并、.comp 读回都会改变文档里的 transform，缓存必然漂移（这正是
 * 补 comp_layer_get_transform 的原因）。
 */
void panel_move_layer(ShellState& shell, int dx, int dy) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  /*
   * §6.64 多选：选中集合里不止一个顶层图层时，整批一起微调（整段连续微调 = 一个
   * 事务 = 一步撤销）。主选中层仍决定「当前图层」，但位移是所有选中项共享的。
   */
  if (!shell.doc.extra_selection.empty()) {
    std::vector<GroupMemberMove> members;
    build_move_targets(shell, &members);
    if (members.empty()) {
      set_status(shell, L"图层位置：选中的图层没有可移动的成员，无位移");
      return;
    }
    comp_txn txn = nudge_begin(shell, row->id, "Nudge Layers");
    if (txn == nullptr) {
      set_status(shell, L"平移图层：无法开始事务");
      return;
    }
    size_t moved = 0;
    for (const GroupMemberMove& member : members) {
      comp_rect target = member.base;
      target.x += dx;
      target.y += dy;
      if (comp_layer_set_transform(shell.doc.document, member.id, target, member.rotation) ==
          COMP_OK) {
        ++moved;
        shell.doc.nudge_dirty = true;
      }
    }
    if (moved == 0) {
      set_status(shell, L"平移图层失败");
      return;
    }
    shell.doc.needs_render = true;
    const wchar_t* off_canvas =
        members_off_canvas(members, dx, dy, shell.doc.width, shell.doc.height) ? L" · 已拖出画布" : L"";
    wchar_t buffer[192];
    wsprintfW(buffer, L"图层：位移 (%d, %d)，%u 个成员（方向键微调）%s", dx, dy,
              static_cast<unsigned>(moved), off_canvas);
    refresh_history_status(shell, buffer);
    return;
  }
  if (row->is_group != 0) {
    /*
     * §6.61 组图层：组自身的 transform 不参与合成（filter_graph 只烘焙成员），
     * 写进去是静默无操作。微调组 = 平移子树里每个成员；整段连续微调共用一个
     * 事务（松键收口），于是「一步撤销」回到按键之前——与拖动松手同一口径。
     */
    std::vector<GroupMemberMove> members;
    collect_group_members(shell, row->id, &members);
    if (members.empty()) {
      /* 空组没有可写的成员。如实说明，而不是写一个无效果的 transform。 */
      set_status(shell, L"图层位置：组内没有成员，无位移");
      return;
    }
    comp_txn txn = nudge_begin(shell, row->id, "Nudge Layer Group");
    if (txn == nullptr) {
      set_status(shell, L"平移图层组：无法开始事务");
      return;
    }
    size_t moved = 0;
    for (const GroupMemberMove& member : members) {
      comp_rect target = member.base;
      target.x += dx;
      target.y += dy;
      if (comp_layer_set_transform(shell.doc.document, member.id, target, member.rotation) ==
          COMP_OK) {
        ++moved;
        shell.doc.nudge_dirty = true;
      }
    }
    if (moved == 0) {
      set_status(shell, L"平移图层组失败");
      return;
    }
    shell.doc.needs_render = true;
    const wchar_t* off_canvas =
        members_off_canvas(members, dx, dy, shell.doc.width, shell.doc.height) ? L" · 已拖出画布" : L"";
    wchar_t buffer[192];
    wsprintfW(buffer, L"图层组：位移 (%d, %d)，%u 个成员（方向键微调）%s", dx, dy,
              static_cast<unsigned>(moved), off_canvas);
    refresh_history_status(shell, buffer);
    return;
  }
  /* 单层也要显式事务：跨按键合并成一步的全部依据，见 nudge_begin 的注释。 */
  if (nudge_begin(shell, row->id, "Nudge Layer") == nullptr) {
    set_status(shell, L"平移图层：无法开始事务");
    return;
  }
  comp_rect transform{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.doc.document, row->id, &transform, &rotation) != COMP_OK) {
    set_status(shell, L"读取图层变换失败");
    return;
  }
  transform.x += dx;
  transform.y += dy;
  if (comp_layer_set_transform(shell.doc.document, row->id, transform, rotation) != COMP_OK) {
    set_status(shell, L"平移图层失败");
    return;
  }
  shell.doc.nudge_dirty = true;
  shell.doc.needs_render = true;
  /* 微调也能把图层提出画布：与拖动同一句提示，否则照样像丢了图层。 */
  const wchar_t* off_canvas = comp_ui::layer_rect_touches_canvas(
                                 transform.x, transform.y, transform.w, transform.h,
                                 static_cast<double>(rotation), shell.doc.width, shell.doc.height)
                                 ? L""
                                 : L" · 已拖出画布";
  wchar_t buffer[192];
  wsprintfW(buffer, L"图层位置：(%d, %d)（方向键微调，松键合并为一步）%s", transform.x, transform.y,
            off_canvas);
  refresh_history_status(shell, buffer);
}

/*
 * 图层旋转（S21）：在既有角度上累加——必须先读现值，理由同 panel_move_layer。
 * 步进量与夹取口径全在 ui_core（layer_transform_rotate_step），壳不重算。
 */
/* P1-3 的步进记录在这两个命令里也要用；实现在本节末尾（面板变换段）。 */
static double rotation_delta_degrees(double from, double to);

void panel_rotate_layer(ShellState& shell, bool forward) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  comp_rect transform{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.doc.document, row->id, &transform, &rotation) != COMP_OK) {
    set_status(shell, L"读取图层变换失败");
    return;
  }
  /*
   * ui_core 的步进函数只做「+step」；反向是取负方向（上游 Ctrl+左右是加减同一
   * 档位，旋转不因方向不同而换粒度）。
   */
  double next = comp_ui::layer_transform_rotate_step(static_cast<double>(rotation), false);
  if (!forward) next = static_cast<double>(rotation) - comp_ui::kLayerTransformRotateStep;
  /* 多圈累加后归一化到 (-360, 360]，保持与上游 radians 的 truncatingRemainder 一致。 */
  while (next > 360.0) next -= 360.0;
  while (next <= -360.0) next += 360.0;
  if (comp_layer_set_transform(shell.doc.document, row->id, transform,
                               static_cast<float>(next)) != COMP_OK) {
    set_status(shell, L"旋转图层失败");
    return;
  }
  shell.doc.needs_render = true;
  wchar_t buffer[160];
  wsprintfW(buffer, L"图层旋转：%d°（Ctrl+左右）",
            static_cast<int>(next + 0.5));
  refresh_history_status(shell, buffer);
  /* P1-3：记下这一步的实际角度增量（可能不是 ±15——旋转命令按档位累加）。 */
  record_transform_step(shell, 1.0, 1.0, rotation_delta_degrees(rotation, next));
}

/*
 * 图层缩放（S21）：等比例、中心锚定，增量与夹取在 ui_core（scale_rect）。
 * 同样必须先读现值——缩放算的是「新尺寸 = 旧尺寸 × 系数」。
 */
void panel_scale_layer(ShellState& shell, int direction) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  comp_rect transform{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.doc.document, row->id, &transform, &rotation) != COMP_OK) {
    set_status(shell, L"读取图层变换失败");
    return;
  }
  if (transform.w <= 0 || transform.h <= 0) {
    /* w/h<=0 = 按像素尺寸（未显式给过变换）：此时没有可缩放的基准，如实拒绝。 */
    set_status(shell, L"该图层还没有显式变换（按尺寸缩放需先给定变换）");
    return;
  }
  int32_t x = transform.x, y = transform.y, w = transform.w, h = transform.h;
  comp_ui::layer_transform_scale_rect(&x, &y, &w, &h, direction >= 0 ? 1.0 + comp_ui::kLayerTransformScaleStep
                                                                    : 1.0 - comp_ui::kLayerTransformScaleStep);
  if (w == transform.w && h == transform.h) {
    set_status(shell, L"缩放已到上下限");
    return;
  }
  const comp_rect scaled{x, y, w, h};
  if (comp_layer_set_transform(shell.doc.document, row->id, scaled, rotation) != COMP_OK) {
    set_status(shell, L"缩放图层失败");
    return;
  }
  shell.doc.needs_render = true;
  wchar_t buffer[192];
  wsprintfW(buffer, L"图层尺寸：%d×%d（Alt+[ ]，中心锚定）", w, h);
  refresh_history_status(shell, buffer);
  record_transform_step(shell, static_cast<double>(w) / transform.w,
                        static_cast<double>(h) / transform.h, 0.0);
}

/* -------------------------------------------------------- P1-3 数值变换 */

/*
 * 角度差归一到 (-180, 180]：再次变换复读的是「这一步转了多少」，不是绝对角度。
 * 例：图层当前 350°（与 -10° 等价），目标 5° -> 复读 +15°，而不是 +355°。
 */
static double rotation_delta_degrees(double from, double to) {
  double delta = to - from;
  while (delta > 180.0) delta -= 360.0;
  while (delta <= -180.0) delta += 360.0;
  return delta;
}

void record_transform_step(ShellState& shell, double scale_x, double scale_y,
                           double angle_delta) {
  if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || !std::isfinite(angle_delta)) return;
  if (scale_x <= 0.0 || scale_y <= 0.0) return;
  const double delta = rotation_delta_degrees(0.0, angle_delta);
  /* 纯恒等的一步不值得复读：拖了又拖回原位的松手不该顶掉上一次真变换。 */
  if (std::fabs(scale_x - 1.0) < 1e-9 && std::fabs(scale_y - 1.0) < 1e-9 &&
      std::fabs(delta) < 1e-9) {
    return;
  }
  shell.doc.transform_step_valid = true;
  shell.doc.transform_step_scale_x = scale_x;
  shell.doc.transform_step_scale_y = scale_y;
  shell.doc.transform_step_angle = delta;
}

bool layer_effective_transform(ShellState& shell, comp_layer_id id, comp_rect* out_rect,
                               double* out_rotation) {
  if (out_rect == nullptr || id == 0 || shell.doc.document == nullptr) return false;
  comp_rect rect{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.doc.document, id, &rect, &rotation) != COMP_OK) return false;
  if (rect.w <= 0 || rect.h <= 0) {
    /*
     * 没显式给过变换：有效包围盒 = 原点 + 图层像素尺寸（与引擎 effective_bounds
     * 同口径）。组与真正的空层在查询模式下报 0×0——那是「没有可变换的像素」，
     * 不是「尺寸为零的变换」；交回调用方拒绝，绝不写一个 0×0 的 transform 进去。
     */
    int32_t width = 0;
    int32_t height = 0;
    comp_pixel_format format = COMP_PIXEL_RGBA8;
    if (comp_layer_get_pixels(shell.doc.document, id, nullptr, 0, &width, &height, &format) !=
        COMP_OK) {
      return false;
    }
    if (width <= 0 || height <= 0) return false;
    rect.x = 0;
    rect.y = 0;
    rect.w = width;
    rect.h = height;
  }
  *out_rect = rect;
  if (out_rotation != nullptr) *out_rotation = static_cast<double>(rotation);
  return true;
}

int panel_apply_layer_transform(ShellState& shell, comp_layer_id id, int width, int height,
                                double angle, bool keep_rotation) {
  if (shell.doc.document == nullptr || id == 0) return -1;
  comp_rect current{};
  double current_rotation = 0.0;
  if (!layer_effective_transform(shell, id, &current, &current_rotation)) return -1;
  width = comp_ui::clamp_doc_size(width);
  height = comp_ui::clamp_doc_size(height);
  const double next_rotation = keep_rotation ? current_rotation : angle;
  if (!std::isfinite(next_rotation)) return -1;
  /*
   * 恒等（宽高与角度都没变）不写、不落撤销步：面板留着让用户接着改，与变换
   * 选区的恒等短路由同一约定。未显式变换的图层「按原尺寸 + 0°」回车也走这里，
   * 不会平白把隐式变换物化成一条 transform。
   */
  if (width == current.w && height == current.h &&
      std::fabs(next_rotation - current_rotation) < 1e-6) {
    return 0;
  }
  /* 中心锚定：新矩形以旧矩形的中心为中心，和面板副标题的承诺一致。 */
  const double center_x = static_cast<double>(current.x) + current.w * 0.5;
  const double center_y = static_cast<double>(current.y) + current.h * 0.5;
  const comp_rect next{static_cast<int32_t>(std::lround(center_x - width * 0.5)),
                       static_cast<int32_t>(std::lround(center_y - height * 0.5)), width, height};
  if (comp_layer_set_transform(shell.doc.document, id, next, static_cast<float>(next_rotation)) !=
      COMP_OK) {
    return -1;
  }
  record_transform_step(shell, static_cast<double>(width) / static_cast<double>(current.w),
                        static_cast<double>(height) / static_cast<double>(current.h),
                        rotation_delta_degrees(current_rotation, next_rotation));
  shell.doc.needs_render = true;
  return 1;
}

/*
 * 再次变换：把上一次（旋转 / 缩放 / 数值变换 / 手柄拖动）的比例与角度增量
 * 原样再套一遍。每一次执行都会覆盖「上一次」，所以连按 Ctrl+Shift+T 是
 * 等比/等角累加，和 PS 一致。移动刻意不记录：它走 nudge 事务，与矩形不是
 * 同一个量（见 shell_state.h 的字段注释）。
 */
void panel_transform_again(ShellState& shell) {
  if (!shell.doc.transform_step_valid) {
    set_status(shell, L"再次变换：还没有可复读的变换（先旋转 / 缩放 / 数值变换一次）");
    return;
  }
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  comp_rect current{};
  double rotation = 0.0;
  if (!layer_effective_transform(shell, row->id, &current, &rotation)) {
    set_status(shell, L"再次变换：该图层没有可变换的像素（组或空图层）");
    return;
  }
  const int width = comp_ui::clamp_doc_size(static_cast<int>(
      std::lround(static_cast<double>(current.w) * shell.doc.transform_step_scale_x)));
  const int height = comp_ui::clamp_doc_size(static_cast<int>(
      std::lround(static_cast<double>(current.h) * shell.doc.transform_step_scale_y)));
  const int angle = comp_ui::doc_angle_from_rotation(rotation + shell.doc.transform_step_angle);
  const int applied = panel_apply_layer_transform(shell, row->id, width, height, angle, false);
  if (applied < 0) {
    set_status(shell, L"再次变换：写入图层变换失败");
    return;
  }
  if (applied == 0) {
    set_status(shell, L"再次变换：尺寸已到 1..30000 边界且角度未变（未产生撤销步）");
    return;
  }
  wchar_t buffer[192];
  wsprintfW(buffer, L"再次变换：%d×%d · %d°（Ctrl+Shift+T）", width, height, angle);
  refresh_history_status(shell, buffer);
}

void panel_add_layer(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  comp_layer_desc desc{};
  desc.name = nullptr; /* 交给引擎自动命名 */
  desc.blend = static_cast<comp_blend_mode>(0); /* 0 = Normal（冻结表） */
  desc.opacity = 1.0f;
  desc.is_group = 0;
  desc.parent = 0;
  desc.sampling = static_cast<comp_sampling>(0);
  comp_layer_id id = 0;
  if (comp_layer_add(shell.doc.document, &desc, &id) != COMP_OK) {
    set_status(shell, L"新建图层失败");
    return;
  }
  set_visible_cache(shell, id, static_cast<uint8_t>(1));
  refresh_layers(shell);
  shell.doc.needs_render = true;
  wchar_t buffer[96];
  wsprintfW(buffer, L"已新建图层 #%I64u", static_cast<unsigned long long>(id));
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------- 图层多选 / 批量操作（P0-1） */

/*
 * 多选集合 = 主选中 + extra_selection。
 *
 * 主选中仍是唯一的「当前图层」（调整面板、效果面板、绘图目标都只认它）；
 * extra_selection 只影响批量命令（删除/复制/不透明度）。这样上游 PS 的
 * 「一级选中 + 多选扩展」语义在壳里不打架：单击永远收敛成一个。
 */
bool panel_selection_contains(const ShellState& shell, comp_layer_id id) {
  if (id == 0) return false;
  const LayerRow* row = selected_row(shell);
  if (row != nullptr && row->id == id) return true;
  for (comp_layer_id extra : shell.doc.extra_selection) {
    if (extra == id) return true;
  }
  return false;
}

/*
 * 批量命令的 id 列表（面板显示序：从上到下；S44 起树形序，子层紧跟组行）。
 *
 * 已经「被选中祖先覆盖」的图层要从列表里摘掉：删/复制一个组时整棵子树
 * 已经跟着走了，再对子层发一次写入只会失败——而且是那种「第一层成功、
 * 第二层报 not found」的半成品失败。
 */
std::vector<comp_layer_id> panel_selection_ids(const ShellState& shell, bool drop_covered) {
  std::vector<comp_layer_id> ids;
  const int32_t rows = panel_row_count(shell);
  for (int32_t shown = 0; shown < rows; ++shown) {
    const LayerRow* layer = layer_row_at(shell, shown);
    if (layer == nullptr) continue;
    const LayerRow& row = *layer;
    if (!panel_selection_contains(shell, row.id)) continue;
    if (drop_covered) {
      bool covered = false;
      comp_layer_id parent = row.parent;
      int guard = 0;
      while (parent != 0 && guard++ < 4096) {
        if (panel_selection_contains(shell, parent)) {
          covered = true;
          break;
        }
        comp_layer_id next_parent = 0;
        bool found = false;
        for (const LayerRow& other : shell.doc.layers) {
          if (other.id == parent) {
            next_parent = other.parent;
            found = true;
            break;
          }
        }
        if (!found) break;
        parent = next_parent;
      }
      if (covered) continue;
    }
    ids.push_back(row.id);
  }
  return ids;
}

/* 按 id 选中（selected_layer 是面板显示行号，S44 起经树形 display 反查）。 */
void panel_select_layer_id(ShellState& shell, comp_layer_id id) {
  const int32_t row = panel_row_of_id(shell, id);
  if (row >= 0) shell.doc.selected_layer = row;
}

/*
 * 新建组（Ctrl+G，对齐上游 Group Layers）。
 *
 * 有选区时把选中图层搬进新组（上游语义：Cmd+G 直接建组并收入选区，
 * 而不是建一个空组让用户再拖）。整批写入走一个显式事务，一步撤销。
 * 新组落在根级栈顶；组内顺序按原数组序（自下而上）搬入，堆叠关系不变。
 */
void panel_add_group(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const std::vector<comp_layer_id> moving = panel_selection_ids(shell, true);
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, "Group Layers", &txn) != COMP_OK) {
    set_status(shell, L"新建组：无法开始事务");
    return;
  }
  comp_layer_desc desc{};
  desc.name = nullptr; /* 引擎自动命名 */
  desc.blend = static_cast<comp_blend_mode>(0);
  desc.opacity = 1.0f;
  desc.is_group = 1;
  desc.parent = 0;
  desc.sampling = static_cast<comp_sampling>(0);
  comp_layer_id group_id = 0;
  if (comp_layer_add(shell.doc.document, &desc, &group_id) != COMP_OK) {
    comp_txn_abort(txn);
    set_status(shell, L"新建组失败");
    return;
  }
  set_visible_cache(shell, group_id, static_cast<uint8_t>(1));
  /*
   * **从下往上搬**（moving 是面板显示序，即从上到下，所以反向遍历）。
   * comp_layer_move(id, group, 0) 的语义是「移到组内末尾 = 数组末尾 = 组顶」，
   * 每搬一层它都落在组顶。按显示序搬的话，最后搬的最底层会跑到组顶，整组堆叠序
   * 被静默反转（Ctrl+G 两层后画面上下颠倒）。从下往上搬，最后落下的是原顶层。
   */
  for (auto it = moving.rbegin(); it != moving.rend(); ++it) {
    if (comp_layer_move(shell.doc.document, *it, group_id, 0) != COMP_OK) {
      comp_txn_abort(txn);
      set_status(shell, L"新建组失败（无法把图层移入组，已整体回滚）");
      return;
    }
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"新建组：提交失败");
    return;
  }
  shell.doc.extra_selection.clear();
  refresh_layers(shell);
  panel_select_layer_id(shell, group_id);
  shell.doc.needs_render = true;
  wchar_t buffer[160];
  wsprintfW(buffer, L"已新建组 #%I64u（收入 %d 个图层）",
            static_cast<unsigned long long>(group_id),
            static_cast<int>(moving.size()));
  refresh_history_status(shell, buffer);
}

/* 删除选中图层（Del）：多选时一个显式事务 = 一步撤销。 */
void panel_delete_layer(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const std::vector<comp_layer_id> ids = panel_selection_ids(shell, true);
  if (ids.empty()) {
    set_status(shell, L"无图层可删除");
    return;
  }
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, "Delete Layers", &txn) != COMP_OK) {
    set_status(shell, L"删除图层：无法开始事务");
    return;
  }
  for (comp_layer_id id : ids) {
    if (comp_layer_remove(shell.doc.document, id) != COMP_OK) {
      comp_txn_abort(txn);
      set_status(shell, L"删除图层失败（已整体回滚）");
      return;
    }
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"删除图层：提交失败");
    return;
  }
  shell.doc.extra_selection.clear();
  refresh_layers(shell);
  shell.doc.needs_render = true;
  wchar_t buffer[128];
  wsprintfW(buffer, L"已删除 %d 个图层", static_cast<int>(ids.size()));
  refresh_history_status(shell, buffer);
}

/* 复制图层（Ctrl+J，对齐上游 Duplicate Layer）：多选 = 一次事务一步撤销。 */
void panel_duplicate_layer(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const std::vector<comp_layer_id> ids = panel_selection_ids(shell, true);
  if (ids.empty()) {
    set_status(shell, L"无图层可复制");
    return;
  }
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, "Duplicate Layers", &txn) != COMP_OK) {
    set_status(shell, L"复制图层：无法开始事务");
    return;
  }
  comp_layer_id first_copy = 0;
  for (comp_layer_id id : ids) {
    comp_layer_id copy_id = 0;
    if (comp_layer_duplicate(shell.doc.document, id, &copy_id) != COMP_OK) {
      comp_txn_abort(txn);
      set_status(shell, L"复制图层失败（已整体回滚）");
      return;
    }
    if (first_copy == 0) first_copy = copy_id;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"复制图层：提交失败");
    return;
  }
  shell.doc.extra_selection.clear();
  refresh_layers(shell);
  if (first_copy != 0) panel_select_layer_id(shell, first_copy);
  shell.doc.needs_render = true;
  wchar_t buffer[128];
  wsprintfW(buffer, L"已复制 %d 个图层", static_cast<int>(ids.size()));
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------- 图层名 / 不透明度键入 */

void refresh_layer_input_status(ShellState& shell) {
  wchar_t buffer[240];
  if (shell.doc.layer_input == LayerInput::Rename) {
    wsprintfW(buffer, L"重命名图层 #%I64u：%s（回车确认 / Esc 取消）",
              static_cast<unsigned long long>(shell.doc.layer_input_id),
              shell.doc.layer_input_text.c_str());
  } else if (shell.doc.layer_input == LayerInput::SmartFilterName) {
    wsprintfW(buffer, L"智能滤镜第 %d 条：%s（回车确认 / Esc 取消，空 = 用类型名）",
              static_cast<int>(shell.doc.layer_input_slot + 1),
              shell.doc.layer_input_text.c_str());
  } else if (shell.doc.layer_input == LayerInput::FilterName) {
    wsprintfW(buffer, L"按名称过滤图层：%s（回车应用 / Esc 取消，空 = 不过滤）",
              shell.doc.layer_input_text.c_str());
  } else if (shell.doc.layer_input == LayerInput::CompName) {
    if (shell.doc.layer_input_id != 0) {
      wsprintfW(buffer, L"重命名图层复合 #%I64u：%s（回车确认 / Esc 取消）",
                static_cast<unsigned long long>(shell.doc.layer_input_id),
                shell.doc.layer_input_text.c_str());
    } else {
      wsprintfW(buffer, L"新建图层复合：%s（回车捕获当前可见性 + 不透明度 / Esc 取消）",
                shell.doc.layer_input_text.c_str());
    }
  } else {
    wsprintfW(buffer, L"图层不透明度：%s%%（回车应用 / Esc 取消，0..100）",
              shell.doc.layer_input_text.c_str());
  }
  set_status(shell, buffer);
}

/* 重命名（F2 / 双击行）：预填当前名字，UTF-8 → UTF-16 后交给 WM_CHAR 追加。 */
void panel_begin_rename(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr || shell.doc.document == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  shell.doc.layer_input = LayerInput::Rename;
  shell.doc.layer_input_id = row->id;
  shell.doc.layer_input_text = widen_utf8(row->name);
  refresh_layer_input_status(shell);
}

/* 直接键入不透明度（对齐上游双击不透明度数值）：预填当前百分比。 */
void panel_begin_opacity_set(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr || shell.doc.document == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  int percent = static_cast<int>(row->opacity * 100.0f + 0.5f);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  wchar_t buffer[16];
  wsprintfW(buffer, L"%d", percent);
  shell.doc.layer_input = LayerInput::Opacity;
  shell.doc.layer_input_id = row->id;
  shell.doc.layer_input_text = buffer;
  refresh_layer_input_status(shell);
}

void panel_cancel_layer_input(ShellState& shell) {
  shell.doc.layer_input = LayerInput::None;
  shell.doc.layer_input_id = 0;
  shell.doc.layer_input_text.clear();
}

/*
 * 键入一个字符。名字接受任意可打印字符（含 CJK —— WM_CHAR/WM_IME_CHAR 送来的
 * 就是 UTF-16，不用在这里做编码转换）；不透明度只接受数字与一个小数点。
 * 名字长度上限按 UTF-16 码元粗夹到 kLayerNameCapacity（UTF-8 字节超限由
 * comp_layer_set_name 精确拒绝并给出原因）。
 */
void panel_layer_input_char(ShellState& shell, wchar_t ch) {
  if (shell.doc.layer_input == LayerInput::None) return;
  if (ch < 0x20 || ch == 0x7F) return; /* 控制字符（回车/退格/Esc 由 WM_KEYDOWN 处理） */
  if (shell.doc.layer_input == LayerInput::Opacity) {
    if ((ch < L'0' || ch > L'9') && ch != L'.') return;
    if (ch == L'.' && shell.doc.layer_input_text.find(L'.') != std::wstring::npos) return;
    if (shell.doc.layer_input_text.size() >= 6) return;
  } else if (shell.doc.layer_input_text.size() >= kLayerNameCapacity) {
    return;
  }
  shell.doc.layer_input_text.push_back(ch);
  refresh_layer_input_status(shell);
}

void panel_layer_input_backspace(ShellState& shell) {
  if (shell.doc.layer_input == LayerInput::None) return;
  if (shell.doc.layer_input_text.empty()) return;
  shell.doc.layer_input_text.pop_back();
  refresh_layer_input_status(shell);
}

/* 提交键入：改名走 comp_layer_set_name（严格 UTF-8），不透明度走批量设置。 */
void panel_commit_layer_input(ShellState& shell) {
  if (shell.doc.layer_input == LayerInput::None) return;
  /* 过滤词不属于文档：没有文档也能改（清空过滤后状态栏照常回报行数）。 */
  if (shell.doc.layer_input == LayerInput::FilterName) {
    shell.layer_filter_name = shell.doc.layer_input_text;
    panel_cancel_layer_input(shell);
    panel_refresh_after_filter(shell);
    return;
  }
  if (shell.doc.document == nullptr) {
    panel_cancel_layer_input(shell);
    set_status(shell, L"无文档");
    return;
  }
  const LayerInput mode = shell.doc.layer_input;
  const comp_layer_id id = shell.doc.layer_input_id;
  const std::wstring text = shell.doc.layer_input_text;
      /* 无名复合在状态栏与菜单里都没法指认；拒绝并保持输入态。 */
      set_status(shell, L"图层复合名不能为空");
      refresh_layer_input_status(shell);
      return;
    }
    wchar_t buffer[192];
    if (id == 0) {
      comp_layer_comp_id created = 0;
      if (comp_layer_comp_capture(shell.doc.document, name.c_str(), &created) != COMP_OK) {
        /* 引擎 last_error 已由 set_status 附上（空名/坏编码/超长/达到上限）。 */
        set_status(shell, L"捕获图层复合失败");
        refresh_layer_input_status(shell);
        return;
      }
      panel_cancel_layer_input(shell);
      shell.layer_comp_current = created;
      wsprintfW(buffer, L"已捕获图层复合「%s」", text.c_str());
      refresh_history_status(shell, buffer);
      return;
    }
    if (comp_layer_comp_rename(shell.doc.document, id, name.c_str()) != COMP_OK) {
      set_status(shell, L"重命名图层复合失败");
      refresh_layer_input_status(shell);
      return;
    }
    panel_cancel_layer_input(shell);
    wsprintfW(buffer, L"已重命名图层复合「%s」", text.c_str());
    refresh_history_status(shell, buffer);
    return;
  }
  if (mode == LayerInput::Rename) {
    const std::string name = narrow_utf8(text);
    if (name.empty()) {
      /* 空名会让图层在没有标识的情况下落盘；拒绝并保持输入态让用户改。 */
      set_status(shell, L"图层名不能为空");
      refresh_layer_input_status(shell);
      return;
    }
    if (comp_layer_set_name(shell.doc.document, id, name.c_str()) != COMP_OK) {
      /* 引擎 last_error 已由 set_status 附上（空名/坏编码/超长/未知 id）。 */
      set_status(shell, L"重命名失败");
      refresh_layer_input_status(shell);
      return;
    }
    panel_cancel_layer_input(shell);
    refresh_layers(shell);
    wchar_t buffer[160];
    wsprintfW(buffer, L"已重命名图层 #%I64u", static_cast<unsigned long long>(id));
    refresh_history_status(shell, buffer);
    return;
  }

  /* 不透明度：百分比输入，夹到 0..100；作用于整个多选集合，一步撤销。 */
  const std::wstring digits = [&text]() {
    std::wstring trimmed = text;
    while (!trimmed.empty() && trimmed.back() == L'%') trimmed.pop_back();
    return trimmed;
  }();
  wchar_t* end = nullptr;
  const double parsed = wcstod(digits.c_str(), &end);
  if (digits.empty() || end == digits.c_str() || *end != L'\0' || !(parsed >= 0.0)) {
    set_status(shell, L"不透明度：请输入 0..100 的数值");
    refresh_layer_input_status(shell);
    return;
  }
  double percent = parsed;
  if (percent > 100.0) percent = 100.0;
  const float opacity = static_cast<float>(percent / 100.0);
  const std::vector<comp_layer_id> ids = panel_selection_ids(shell, false);
  if (ids.empty()) {
    panel_cancel_layer_input(shell);
    set_status(shell, L"无图层");
    return;
  }
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, "Set Layer Opacity", &txn) != COMP_OK) {
    set_status(shell, L"设置不透明度：无法开始事务");
    refresh_layer_input_status(shell);
    return;
  }
  for (comp_layer_id target : ids) {
    if (comp_layer_set_opacity(shell.doc.document, target, opacity) != COMP_OK) {
      comp_txn_abort(txn);
      set_status(shell, L"设置不透明度失败（已整体回滚）");
      refresh_layer_input_status(shell);
      return;
    }
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"设置不透明度：提交失败");
    refresh_layer_input_status(shell);
    return;
  }
  panel_cancel_layer_input(shell);
  refresh_layers(shell);
  shell.doc.needs_render = true;
  wchar_t buffer[128];
  wsprintfW(buffer, L"不透明度 %d%%（%d 个图层）",
            static_cast<int>(percent + 0.5), static_cast<int>(ids.size()));
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 调整面板 */

comp_layer_id selected_layer_id(const ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  return row != nullptr ? row->id : 0;
}

/*
 * 向下合并（Ctrl+E，对齐上游 ⌘E）：把选中图层与下方图层按「画布呈现」
 * 合并为一个像素层。失败原因（无可合并 / 上方穿透组不透明度非 100% /
 * 携带调整或效果）由 set_status 附带的引擎 last_error 如实透出。
 */
void panel_merge_down(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"无图层");
    return;
  }
  if (comp_layer_merge_down(shell.doc.document, id) != COMP_OK) {
    set_status(shell, L"合并失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, L"已向下合并");
}

/* 拼合图像（对齐上游 Flatten Image）：整篇文档合成为单一像素层。 */
void panel_flatten(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  if (comp_document_flatten(shell.doc.document) != COMP_OK) {
    set_status(shell, L"拼合失败");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, L"已拼合图像");
}

/*
 * 合并可见（对齐上游 Merge Visible）：所有「实际参与合成」的图层烘成一个像素层，
 * 隐藏图层 / 隐藏组原样保留。拒绝原因（可见层剪贴到隐藏基底 / 引擎不认识的
 * effect）由 set_status 附带的引擎 last_error 如实透出；参与层不足两层时引擎
 * 返回 changed=0，这里明确提示而不是假装成功。
 */
void panel_merge_visible(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  comp_layer_id merged = 0;
  int32_t changed = 0;
  if (comp_layer_merge_visible(shell.doc.document, &merged, &changed) != COMP_OK) {
    set_status(shell, L"合并可见失败（未改动）");
    return;
  }
  if (changed == 0) {
    set_status(shell, L"合并可见：实际参与合成的图层不足两层（未产生撤销步）");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, L"已合并可见图层");
}

/*
 * 反向排列（对齐上游 Arrange > Reverse）：反转同级顺序。选中组时反转组内子层，
 * 选中普通层时反转它的同级，未选中时反转根级——与上游「Reverse 作用于当前层
 * 所在层级」一致。少于两个兄弟是空操作，引擎返回 changed=0。
 */
void panel_reverse_layers(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  comp_layer_id parent = 0;
  const comp_layer_id id = selected_layer_id(shell);
  if (id != 0) {
    int32_t is_group = 0;
    comp_layer_id group_parent = 0;
    comp_layer_id clipping = 0;
    if (comp_layer_get_group_info(shell.doc.document, id, &is_group, &group_parent, &clipping) ==
        COMP_OK) {
      parent = is_group != 0 ? id : group_parent;
    }
  }
  int32_t changed = 0;
  if (comp_layer_reverse(shell.doc.document, parent, &changed) != COMP_OK) {
    set_status(shell, L"反向排列失败（未改动）");
    return;
  }
  if (changed == 0) {
    set_status(shell, L"反向排列：同级图层不足两层（未产生撤销步）");
    return;
  }
  refresh_layers(shell);
  shell.doc.needs_render = true;
  refresh_history_status(shell, L"已反向排列");
}

/* 名称经 ABI 取自引擎键名表，外壳不复制任何键名。 */
const char* adjustment_kind_name(uint32_t kind) {
  const char* name = nullptr;
  if (comp_layer_adjustment_kind_name(kind, &name) != COMP_OK || name == nullptr) return "(unknown)";
  return name;
}

bool adjust_paged_kind(uint32_t kind) { return kind == COMP_ADJUSTMENT_LEVELS; }
int32_t adjust_channel_count(uint32_t kind) {
  return adjust_paged_kind(kind) ? kLevelsChannelCount : 1;
}

/* 重建编辑值：以缺省值为底；若选中了既有调整，则按 settings 回填（引擎侧解析 JSON）。 */
void reset_adjustment_values(ShellState& shell) {
  shell.doc.adj_values.clear();
  shell.doc.adj_param = 0;
  shell.doc.adj_input.clear();

  size_t param_count = 0;
  if (comp_layer_adjustment_param_count(static_cast<uint32_t>(shell.doc.adj_kind), &param_count) != COMP_OK) {
    param_count = 0;
  }
  const std::string* existing = nullptr;
  if (shell.doc.selected_adjustment >= 0 &&
      shell.doc.selected_adjustment < static_cast<int32_t>(shell.doc.adj_settings.size())) {
    existing = &shell.doc.adj_settings[static_cast<size_t>(shell.doc.selected_adjustment)];
  }

  for (size_t i = 0; i < param_count; ++i) {
    comp_adjustment_param param{};
    if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.doc.adj_kind), i, &param) != COMP_OK) {
      shell.doc.adj_values.emplace_back();
      continue;
    }
    bool filled = false;
    std::string text;
    if (existing != nullptr) {
      if (param.type == COMP_PARAM_BOOL) {
        int32_t boolean = 0;
        if (comp_layer_adjustment_read_value(existing->c_str(), param.json_path, nullptr, &boolean) ==
            COMP_OK) {
          text = boolean != 0 ? "1" : "0";
          filled = true;
        }
      } else {
        double number = 0.0;
        if (comp_layer_adjustment_read_value(existing->c_str(), param.json_path, &number, nullptr) ==
            COMP_OK) {
          text = number_text(number);
          filled = true;
        }
      }
    }
    if (!filled) {
      text = param.type == COMP_PARAM_BOOL ? (param.default_value != 0.0 ? "1" : "0")
                                           : number_text(param.default_value);
    }
    shell.doc.adj_values.push_back(text);
  }

  /*
   * 分页视图的入口状态：通道号只夹取、不重置——拖动结束会走 refresh -> 这里，
   * 若在这里把通道清 0，用户拖 B 通道的滑杆松手就会被弹回 RGB。焦点落在当前
   * 通道首行，避免高亮停在不显示的行上。
   */
  const int32_t channels = adjust_channel_count(static_cast<uint32_t>(shell.doc.adj_kind));
  if (shell.doc.adj_channel < 0) shell.doc.adj_channel = 0;
  if (shell.doc.adj_channel >= channels) shell.doc.adj_channel = channels - 1;
  shell.doc.adj_param =
      adjust_paged_kind(static_cast<uint32_t>(shell.doc.adj_kind))
          ? shell.doc.adj_channel * kLevelsFieldsPerChannel
          : 0;
  if (shell.doc.adj_param >= static_cast<int32_t>(shell.doc.adj_values.size())) shell.doc.adj_param = 0;
}

/* 读取选中图层的调整列表（只读，不开事务），并把 kind 与编辑值同步过来。 */
void refresh_adjustments(ShellState& shell) {
  shell.doc.adj_kinds.clear();
  shell.doc.adj_settings.clear();

  const comp_layer_id id = shell.doc.document != nullptr ? selected_layer_id(shell) : 0;
  if (id == 0) {
    shell.doc.selected_adjustment = -1;
    reset_adjustment_values(shell);
    refresh_curve_points(shell);
    return;
  }
  size_t count = 0;
  if (comp_layer_adjustment_count(shell.doc.document, id, &count) != COMP_OK) count = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t kind = 0;
    size_t need = 0;
    if (comp_layer_adjustment_at(shell.doc.document, id, i, &kind, nullptr, 0, &need) != COMP_OK) break;
    std::vector<char> text(need + 1, '\0');
    if (comp_layer_adjustment_at(shell.doc.document, id, i, &kind, text.data(), text.size(), &need) !=
        COMP_OK) {
      break;
    }
    shell.doc.adj_kinds.push_back(kind);
    shell.doc.adj_settings.emplace_back(text.data(), need);
  }
  if (shell.doc.selected_adjustment >= static_cast<int32_t>(shell.doc.adj_settings.size())) {
    shell.doc.selected_adjustment = static_cast<int32_t>(shell.doc.adj_settings.size()) - 1;
  }
  if (shell.doc.selected_adjustment >= 0) {
    shell.doc.adj_kind = static_cast<int32_t>(shell.doc.adj_kinds[static_cast<size_t>(shell.doc.selected_adjustment)]);
  }
  reset_adjustment_values(shell);
  refresh_curve_points(shell);
}

void panel_cycle_kind(ShellState& shell, int32_t delta) {
  /* 换类型 = 回到调整层面板：破坏性面板里按这个键会退出落墨态（kind 可能不再可破坏）。 */
  if (shell.doc.adj_destructive) panel_exit_destructive(shell);
  const int32_t total = static_cast<int32_t>(COMP_ADJUSTMENT_KIND_COUNT);
  int32_t kind = shell.doc.adj_kind + delta;
  if (kind < 0) kind = total - 1;
  if (kind >= total) kind = 0;
  shell.doc.adj_kind = kind;
  shell.doc.selected_adjustment = -1; /* 换类型即进入「新增」态 */
  reset_adjustment_values(shell);
  refresh_curve_points(shell);
  wchar_t buffer[160];
  wsprintfW(buffer, L"调整类型 %d/%d：%s", shell.doc.adj_kind + 1, total,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(kind))).c_str());
  set_status(shell, buffer);
}

void panel_select_adjustment(ShellState& shell, int32_t delta) {
  /* 选的永远是「已有调整层」；在破坏性面板里选调整即退出落墨态。 */
  if (shell.doc.adj_destructive) panel_exit_destructive(shell);
  const int32_t count = static_cast<int32_t>(shell.doc.adj_settings.size());
  if (count == 0) {
    set_status(shell, L"该图层没有调整（Ctrl+Shift+A 新增）");
    return;
  }
  int32_t next = shell.doc.selected_adjustment + delta;
  if (next < 0) next = 0;
  if (next >= count) next = count - 1;
  shell.doc.selected_adjustment = next;
  shell.doc.adj_kind = static_cast<int32_t>(shell.doc.adj_kinds[static_cast<size_t>(next)]);
  reset_adjustment_values(shell);
  refresh_curve_points(shell);
  wchar_t buffer[160];
  wsprintfW(buffer, L"已选中调整 #%d：%s", next + 1,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(shell.doc.adj_kind))).c_str());
  set_status(shell, buffer);
}

void panel_move_param(ShellState& shell, int32_t delta) {
  const int32_t count = static_cast<int32_t>(shell.doc.adj_values.size());
  if (count == 0) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  /* 分页视图（色阶）里 Tab 只在当前通道的 5 行内绕，不会串到别的通道。 */
  const bool paged = adjust_paged_kind(static_cast<uint32_t>(shell.doc.adj_kind));
  const int32_t first = paged ? shell.doc.adj_channel * kLevelsFieldsPerChannel : 0;
  int32_t last = paged ? first + kLevelsFieldsPerChannel - 1 : count - 1;
  if (last >= count) last = count - 1;
  if (last < first) last = first;
  int32_t next = shell.doc.adj_param + delta;
  if (next < first) next = first;
  if (next > last) next = last;
  shell.doc.adj_param = next;
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.doc.adj_kind), static_cast<size_t>(next),
                                     &param) != COMP_OK) {
    return;
  }
  wchar_t buffer[200];
  wsprintfW(buffer, L"参数 %d/%d：%s = %s", next + 1, count,
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.doc.adj_values[static_cast<size_t>(next)].c_str()).c_str());
  set_status(shell, buffer);
  shell.doc.adj_input.clear();
  /* 色阶 20 行、小窗口只显示 12 行，切参数后必须把焦点行滚进可视区。 */
  adjust_scroll_to_focus(shell);
}

/* 面板当前显示的行数，以及「相对行 -> 参数表下标」的基址。分页视图下两者不再
 * 相等：色阶 20 项按通道切成 4 页，每页 5 行。绘制/命中/滚动全用这两个函数。 */
int32_t adjust_shown_count(const ShellState& shell) {
  const int32_t total = static_cast<int32_t>(shell.doc.adj_values.size());
  if (!adjust_paged_kind(static_cast<uint32_t>(shell.doc.adj_kind))) return total;
  const int32_t base = shell.doc.adj_channel * kLevelsFieldsPerChannel;
  if (base >= total) return 0;
  const int32_t rest = total - base;
  return rest < kLevelsFieldsPerChannel ? rest : kLevelsFieldsPerChannel;
}

int32_t adjust_param_base(const ShellState& shell) {
  return adjust_paged_kind(static_cast<uint32_t>(shell.doc.adj_kind))
             ? shell.doc.adj_channel * kLevelsFieldsPerChannel
             : 0;
}

/* 键盘/滑杆共用的步长：宽量程一律 1（色阶 0..255、±100 的明暗这类「整数量」），
 * 窄量程取百分之一（gamma、曝光这类必须留小数的）。 */
double adjust_param_step(const comp_adjustment_param& param) {
  const double span = param.max_value - param.min_value;
  if (!(span > 0.0)) return 0.0;
  return span >= 50.0 ? 1.0 : span / 100.0;
}

void panel_step_value(ShellState& shell, int32_t direction) {
  const size_t index = static_cast<size_t>(shell.doc.adj_param);
  if (index >= shell.doc.adj_values.size()) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.doc.adj_kind), index, &param) != COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    shell.doc.adj_values[index] = shell.doc.adj_values[index] == "1" ? "0" : "1";
  } else {
    const double value = std::atof(shell.doc.adj_values[index].c_str()) +
                         static_cast<double>(direction) * adjust_param_step(param);
    /* 与滑杆同一套归整：键盘调出来的值不许是 40.000000000000004。 */
    shell.doc.adj_values[index] =
        number_text(comp_ui::adjust_snap_value(value, param.min_value, param.max_value));
  }
  shell.doc.adj_input.clear();
  wchar_t buffer[220];
  wsprintfW(buffer, L"%s = %s （范围 [%s, %s]）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.doc.adj_values[index].c_str()).c_str(),
            widen_ascii(number_text(param.min_value).c_str()).c_str(),
            widen_ascii(number_text(param.max_value).c_str()).c_str());
  set_status(shell, buffer);
}

/* 数字键开始输入：布尔参数不进入文本态，直接切换。 */
void panel_begin_input(ShellState& shell, wchar_t character) {
  const size_t index = static_cast<size_t>(shell.doc.adj_param);
  if (index >= shell.doc.adj_values.size()) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.doc.adj_kind), index, &param) != COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    panel_step_value(shell, 1);
    return;
  }
  shell.doc.adj_input.assign(1, character);
  set_status(shell, L"输入中：回车提交，Esc 取消");
}

void panel_backspace(ShellState& shell) {
  if (!shell.doc.adj_input.empty()) shell.doc.adj_input.pop_back();
}

void panel_commit_input(ShellState& shell) {
  if (shell.doc.adj_input.empty()) {
    set_status(shell, L"未在输入状态（数字键开始输入）");
    return;
  }
  const size_t index = static_cast<size_t>(shell.doc.adj_param);
  if (index >= shell.doc.adj_values.size()) {
    shell.doc.adj_input.clear();
    return;
  }
  const std::string text(shell.doc.adj_input.begin(), shell.doc.adj_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  shell.doc.adj_values[index] = text;
  shell.doc.adj_input.clear();
  wchar_t buffer[160];
  wsprintfW(buffer, L"已提交 %s", widen_ascii(text.c_str()).c_str());
  set_status(shell, buffer);
}

/* settings JSON 由引擎组装（壳内不拼 JSON、不解析 JSON）。 */
bool build_settings_json(const ShellState& shell, std::string* out_json) {
  /* 破坏性 Curves 没有参数表（adj_values 为空），settings 由曲线编辑器的寄存串给出。 */
  if (shell.doc.adj_destructive && shell.doc.adj_kind == COMP_ADJUSTMENT_CURVES) {
    if (out_json != nullptr) {
      *out_json = shell.doc.destructive_curve_settings.empty() ? std::string("{}")
                                                               : shell.doc.destructive_curve_settings;
    }
    return true;
  }
  std::vector<const char*> values;
  values.reserve(shell.doc.adj_values.size());
  for (const std::string& text : shell.doc.adj_values) values.push_back(text.c_str());

  const uint32_t kind = static_cast<uint32_t>(shell.doc.adj_kind);
  const char* const* data = values.empty() ? nullptr : values.data();
  size_t need = 0;
  if (comp_layer_adjustment_build_settings(kind, data, values.size(), nullptr, 0, &need) != COMP_OK) {
    return false;
  }
  std::string json(need + 1, '\0');
  if (comp_layer_adjustment_build_settings(kind, data, values.size(), &json[0], json.size(), &need) !=
      COMP_OK) {
    return false;
  }
  json.resize(need);
  if (out_json != nullptr) *out_json = json;
  return true;
}

void panel_apply_adjustment(ShellState& shell, bool add) {
  if (shell.doc.adj_destructive) {
    /* 破坏性面板没有「新增调整」这个动作：同样的参数键落的是像素。 */
    if (add) {
      set_status(shell, L"破坏性调整：Ctrl+Shift+U 直接作用于像素，Esc 退出");
      return;
    }
    panel_apply_destructive(shell);
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  if (!add && shell.doc.selected_adjustment < 0) {
    set_status(shell, L"未选中已有调整（, / . 选择；Ctrl+Shift+A 新增）");
    return;
  }
  std::string json;
  if (!build_settings_json(shell, &json)) {
    set_status(shell, add ? L"新增被拒：参数不合法" : L"应用被拒：参数不合法");
    return;
  }

  const uint32_t kind = static_cast<uint32_t>(shell.doc.adj_kind);
  comp_result result = COMP_OK;
  if (add) {
    result = comp_layer_add_adjustment(shell.doc.document, id, kind, json.c_str());
  } else {
    result = comp_layer_update_adjustment_at(
        shell.doc.document, id, static_cast<size_t>(shell.doc.selected_adjustment), kind, json.c_str());
  }

  shell.doc.needs_render = true;
  if (result != COMP_OK && !add) shell.doc.selected_adjustment = -1;
  refresh_adjustments(shell);
  if (add && !shell.doc.adj_settings.empty()) {
    /* 新增成功后选中最后一条，便于继续微调。 */
    shell.doc.selected_adjustment = static_cast<int32_t>(shell.doc.adj_settings.size()) - 1;
    shell.doc.adj_kind = static_cast<int32_t>(shell.doc.adj_kinds[static_cast<size_t>(shell.doc.selected_adjustment)]);
    reset_adjustment_values(shell);
  } else if (add) {
    reset_adjustment_values(shell);
  }
  if (add) refresh_curve_points(shell);

  wchar_t buffer[180];
  wsprintfW(buffer, L"%s：%s",
            result == COMP_OK ? (add ? L"已新增调整" : L"已应用调整")
                              : (add ? L"新增调整失败" : L"应用调整失败"),
            widen_ascii(adjustment_kind_name(kind)).c_str());
  refresh_history_status(shell, buffer);
}

/* ---------------------------------------- 破坏性调整（ROI #8，图像 > 调整） */

/*
 * 与调整层面板共用同一份参数行（键名 / 量程 / 缺省值都来自 ABI 参数表），
 * 区别只在落点：这里没有「选中的调整」，Ctrl+Shift+U 调
 * comp_layer_apply_adjustment 一次性写进像素，不产生 engineAdjustments 条目。
 * 拖动滑杆时 selected_adjustment 恒为 -1，所以只是编辑缓冲预览，不会边拖边落墨。
 */
void panel_begin_destructive(ShellState& shell, uint32_t kind) {
  if (shell.doc.document == nullptr || selected_layer_id(shell) == 0) {
    set_status(shell, L"图像调整：请先选择一个图层");
    return;
  }
  shell.doc.adj_kind = static_cast<int32_t>(kind);
  shell.doc.adj_channel = 0;
  shell.doc.selected_adjustment = -1;
  reset_adjustment_values(shell);
  /* Curves 没有参数表，点列走这条寄存串（其余 kind 不用它，进态时一律重置）。 */
  shell.doc.destructive_curve_settings = "{\"curves\":{}}";
  /*
   * 没有可调参数的类型（去色、三种自动调整）点菜单即落墨：停在面板里没有滑杆可动，
   * 只会多按一次 Ctrl+Shift+U。曲线同样 0 项，但要在面板里拉控制点，不能提前落墨。
   */
  size_t param_count = 0;
  if (comp_layer_adjustment_param_count(kind, &param_count) != COMP_OK) param_count = 0;
  if (param_count == 0 && kind != COMP_ADJUSTMENT_CURVES) {
    shell.doc.adj_destructive = false; /* 一次性落墨，不进入面板态 */
    panel_apply_destructive(shell);
    return;
  }
  shell.doc.adj_destructive = true;
  refresh_curve_points(shell);
  wchar_t buffer[200];
  wsprintfW(buffer, L"破坏性调整：%s — Ctrl+Shift+U 应用，Esc 退出",
            widen_ascii(adjustment_kind_name(kind)).c_str());
  set_status(shell, buffer);
}

void panel_exit_destructive(ShellState& shell) {
  if (!shell.doc.adj_destructive) return;
  shell.doc.adj_destructive = false;
  shell.doc.destructive_curve_settings.clear();
  refresh_adjustments(shell); /* 回到调整层面板：kind/值以选中图层的调整列表为准 */
  set_status(shell, L"已退出破坏性调整（未应用的参数已丢弃）");
}

/* 把当前参数行一次性落到像素上；失败时文档不变（引擎在开事务前就拒了）。 */
void panel_apply_destructive(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"图像调整：请先选择一个图层");
    return;
  }
  std::string json;
  if (!build_settings_json(shell, &json)) {
    set_status(shell, L"应用被拒：参数不合法");
    return;
  }
  const uint32_t kind = static_cast<uint32_t>(shell.doc.adj_kind);
  int32_t changed = 0;
  const comp_result result =
      comp_layer_apply_adjustment(shell.doc.document, id, kind, json.c_str(), &changed);
  shell.doc.needs_render = true;
  if (result == COMP_ERR_UNSUPPORTED) {
    set_status(shell, L"图像调整：HDR（RGBA16F）图层不受支持（未改动）");
    return;
  }
  if (result == COMP_ERR_STATE) {
    set_status(shell, L"图像调整：组图层或没有像素的图层（未改动）");
    return;
  }
  if (result != COMP_OK) {
    set_status(shell, L"图像调整失败（未改动）");
    return;
  }
  wchar_t buffer[200];
  if (changed == 0) {
    /* 显式空选区 / 恒等参数：引擎不落墨也不进历史。 */
    wsprintfW(buffer, L"%s：结果与像素相同，未进历史",
              widen_ascii(adjustment_kind_name(kind)).c_str());
    set_status(shell, buffer);
    return;
  }
  wsprintfW(buffer, L"已应用破坏性调整：%s", widen_ascii(adjustment_kind_name(kind)).c_str());
  refresh_history_status(shell, buffer);
}

void panel_remove_adjustment(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  if (shell.doc.selected_adjustment < 0) {
    set_status(shell, L"未选中已有调整（, / . 选择）");
    return;
  }
  const comp_result result = comp_layer_remove_adjustment(
      shell.doc.document, id, static_cast<size_t>(shell.doc.selected_adjustment));
  shell.doc.needs_render = true;
  refresh_adjustments(shell);

  wchar_t buffer[120];
  wsprintfW(buffer, result == COMP_OK ? L"已删除调整" : L"删除调整失败");
  refresh_history_status(shell, buffer);
}

std::wstring param_status_text(const comp_ui::FilterPanel& panel) {
  const comp_ui::FilterParamSpec* spec = panel.param_at(panel.focused);
  if (spec == nullptr) return std::wstring();
  return widen_ascii(spec->label) + L" = " +
         widen_ascii(number_text(panel.value(panel.focused)).c_str());
}

/* ------------------------------------------------------ 调整滑杆（P0-3） */

/*
 * 一步撤销的关键在事务边界：按下开显式事务，拖动途中每次写回都落在它里面，
 * 松手才 commit。不开事务时引擎的 ImplicitTxn 会把每次写都自提交——拖一次
 * 滑杆能产生几十步撤销，正是 P0-3 要修的东西。
 * 新增态（selected_adjustment < 0）没有可写的对象：拖动只改编辑缓冲，仍由
 * Ctrl+Shift+U 落盘，这样「先调参数、满意了再加」不会被写进历史。
 */
bool adjust_row_param(const ShellState& shell, int32_t row, comp_adjustment_param* out) {
  if (row < 0 || out == nullptr) return false;
  return comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.doc.adj_kind),
                                        static_cast<size_t>(row), out) == COMP_OK;
}

/* 编辑缓冲 -> 选中的那条调整。只在拖动持有事务时写；失败保持静默（拖动中不刷屏）。 */
bool adjust_write_live(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0 || shell.doc.selected_adjustment < 0) return false;
  if (shell.doc.adj_drag_txn == nullptr) return false;
  std::string json;
  if (!build_settings_json(shell, &json)) return false;
  if (comp_layer_update_adjustment_at(shell.doc.document, id,
                                      static_cast<size_t>(shell.doc.selected_adjustment),
                                      static_cast<uint32_t>(shell.doc.adj_kind),
                                      json.c_str()) != COMP_OK) {
    return false;
  }
  shell.doc.needs_render = true;
  return true;
}

/* 把指针 x 换算成值写进编辑缓冲；值没变就不写文档（少一次快照，也少一次重绘）。 */
void adjust_drag_value(ShellState& shell, double x) {
  if (shell.doc.adj_drag_param < 0) return;
  comp_adjustment_param param{};
  if (!adjust_row_param(shell, shell.doc.adj_drag_param, &param)) return;
  if (param.type == COMP_PARAM_BOOL) return;
  const size_t index = static_cast<size_t>(shell.doc.adj_drag_param);
  if (index >= shell.doc.adj_values.size()) return;

  const double raw =
      comp_ui::adjust_slider_value(shell.doc.adj_drag_track, x, param.min_value, param.max_value);
  const double value = comp_ui::adjust_snap_value(raw, param.min_value, param.max_value);
  const std::string text = number_text(value);
  if (shell.doc.adj_values[index] == text) return;
  shell.doc.adj_values[index] = text;
  shell.doc.adj_input.clear();
  if (adjust_write_live(shell)) shell.doc.adj_drag_dirty = true;
  shell.doc.needs_render = true;

  wchar_t buffer[220];
  wsprintfW(buffer, L"%s = %s（%s）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(text.c_str()).c_str(),
            shell.doc.adj_drag_txn != nullptr ? L"实时预览，松手算一步撤销"
                                          : L"预览值，Ctrl+Shift+U 应用");
  set_status(shell, buffer);
}

void adjust_begin_drag(ShellState& shell, const comp_ui::Rect& panel, int32_t row, double x) {
  comp_adjustment_param param{};
  if (!adjust_row_param(shell, row, &param)) return;
  if (param.type == COMP_PARAM_BOOL) return; /* 布尔没有滑道，点击即切换 */

  if (shell.doc.selected_adjustment >= 0) {
    if (shell.doc.document == nullptr) return;
    if (comp_txn_begin(shell.doc.document, "Adjust Parameter", &shell.doc.adj_drag_txn) != COMP_OK) {
      shell.doc.adj_drag_txn = nullptr;
      set_status(shell, L"调整滑杆：无法开启事务（未改动）");
      return;
    }
  } else {
    shell.doc.adj_drag_txn = nullptr;
  }
  shell.doc.adj_param = row;
  shell.doc.adj_input.clear();
  shell.doc.adj_drag_param = row;
  shell.doc.adj_drag_dirty = false;
  /* 几何在按下瞬间冻结：拖动途中面板重排也不会让滑道在指针下跑掉。
   * 行几何用「相对行」（分页视图里绝对下标要减掉通道基址）。 */
  shell.doc.adj_drag_track = comp_ui::adjust_slider_track_rect(comp_ui::adjust_param_row_rect(
      panel, row - adjust_param_base(shell), shell.doc.adj_scroll));
  adjust_drag_value(shell, x);
}

/* commit=false 用于 Esc / 失焦：事务 abort 会回滚到按下前的快照。 */
void adjust_drag_finish(ShellState& shell, bool commit) {
  if (shell.doc.adj_drag_param < 0) return;
  const int32_t focused = shell.doc.adj_param;
  comp_adjustment_param param{};
  const bool has_param = adjust_row_param(shell, focused, &param);
  const comp_txn txn = shell.doc.adj_drag_txn;
  const bool dirty = shell.doc.adj_drag_dirty;
  /* 先清状态再 ReleaseCapture：捕获变更会重入 WM_CAPTURECHANGED。 */
  shell.doc.adj_drag_param = -1;
  shell.doc.adj_drag_txn = nullptr;
  shell.doc.adj_drag_dirty = false;
  ReleaseCapture();

  bool committed = false;
  if (txn != nullptr) {
    if (commit && dirty) {
      committed = comp_txn_commit(txn) == COMP_OK;
    } else {
      /* 没有实际写入的空事务不该占一步撤销：直接回滚。 */
      comp_txn_abort(txn);
    }
  }
  shell.doc.needs_render = true;
  refresh_adjustments(shell);
  if (focused >= 0 && focused < static_cast<int32_t>(shell.doc.adj_values.size())) {
    shell.doc.adj_param = focused;
  }

  wchar_t buffer[220];
  if (committed) {
    wsprintfW(buffer, L"已应用 %s = %s（一步撤销）",
              widen_ascii(has_param && param.label != nullptr ? param.label : "").c_str(),
              widen_ascii(shell.doc.adj_param < static_cast<int32_t>(shell.doc.adj_values.size())
                              ? shell.doc.adj_values[static_cast<size_t>(shell.doc.adj_param)].c_str()
                              : "")
                  .c_str());
    refresh_history_status(shell, buffer);
  } else if (txn != nullptr && !dirty) {
    wsprintfW(buffer, L"调整未改动（未产生撤销步）");
    refresh_history_status(shell, buffer);
  } else if (txn != nullptr) {
    refresh_history_status(shell, L"已回滚调整拖动");
  } else {
    set_status(shell, L"调整预览值：Ctrl+Shift+U 应用（新增模式不写历史）");
  }
}

/* Tab 切参数后把焦点行滚进可视区（小窗口 / 长参数表时必需）。 */
void adjust_scroll_to_focus(ShellState& shell) {
  Layout layout{};
  if (!client_layout(shell, &layout) || layout.adjust_panel.w <= 0) return;
  shell.doc.adj_scroll = comp_ui::adjust_param_ensure_visible(
      layout.adjust_panel, adjust_shown_count(shell), shell.doc.adj_param - adjust_param_base(shell),
      shell.doc.adj_scroll);
}

/* ------------------------------------------------ 曲线编辑器（P0-3） */

/*
 * 控制点的真相在引擎里（settings JSON）。外壳只缓存一份「当前通道的点列」用于
 * 绘制/命中；任何写入都走 comp_layer_curves_set_points 拼 JSON，再由
 * comp_layer_update_adjustment_at 落盘 —— 撤销步由后者产生，曲线接口本身是
 * 纯函数（见 comp_layer.h 的契约）。拖动期间用显式事务把多次写入折成一步。
 */
const std::string* selected_adjustment_settings(const ShellState& shell) {
  /* 破坏性 Curves 没有可写的调整对象，点列寄存态就是它的「settings」。 */
  if (shell.doc.adj_destructive) {
    return shell.doc.adj_kind == COMP_ADJUSTMENT_CURVES ? &shell.doc.destructive_curve_settings
                                                        : nullptr;
  }
  if (shell.doc.selected_adjustment < 0 ||
      shell.doc.selected_adjustment >= static_cast<int32_t>(shell.doc.adj_settings.size())) {
    return nullptr;
  }
  return &shell.doc.adj_settings[static_cast<size_t>(shell.doc.selected_adjustment)];
}

/* 曲线编辑只在「已选中的 Curves 调整」上可用：新增态没有可写对象。 */
bool curve_editable(const ShellState& shell) {
  return shell.doc.adj_kind == COMP_ADJUSTMENT_CURVES && selected_adjustment_settings(shell) != nullptr;
}

const wchar_t* curve_channel_name(int32_t channel) {
  switch (channel) {
    case 1: return L"R";
    case 2: return L"G";
    case 3: return L"B";
    default: return L"RGB";
  }
}

/* 从 settings 重读当前通道的点列；空通道（`{"curves":{}}`）留在空态，由编辑器
 * 在第一次加点时补上恒等端点。 */
void refresh_curve_points(ShellState& shell) {
  shell.doc.curve_xy.clear();
  shell.doc.curve_focus_point = -1;
  const std::string* settings = selected_adjustment_settings(shell);
  if (settings == nullptr || shell.doc.adj_kind != COMP_ADJUSTMENT_CURVES) return;
  const uint32_t channel = static_cast<uint32_t>(shell.doc.curve_channel);
  size_t count = 0;
  if (comp_layer_curves_point_count(settings->c_str(), channel, &count) != COMP_OK) return;
  if (count > 32) count = 32;
  for (size_t i = 0; i < count; ++i) {
    double x = 0.0;
    double y = 0.0;
    if (comp_layer_curves_point_at(settings->c_str(), channel, i, &x, &y) != COMP_OK) break;
    shell.doc.curve_xy.push_back(x);
    shell.doc.curve_xy.push_back(y);
  }
}

/* 点列 -> settings -> 文档。两遍调用（先问容量再写），与 ABI 契约一致。 */
bool curve_write_points(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  const std::string* settings = selected_adjustment_settings(shell);
  if (shell.doc.document == nullptr || id == 0 || settings == nullptr) return false;
  const size_t count = shell.doc.curve_xy.size() / 2;
  if (count < 2) return false;
  const uint32_t channel = static_cast<uint32_t>(shell.doc.curve_channel);
  size_t need = 0;
  if (comp_layer_curves_set_points(settings->c_str(), channel, shell.doc.curve_xy.data(), count, nullptr,
                                   0, &need) != COMP_OK) {
    return false;
  }
  std::string json(need + 1, '\0');
  if (comp_layer_curves_set_points(settings->c_str(), channel, shell.doc.curve_xy.data(), count,
                                   &json[0], json.size(), &need) != COMP_OK) {
    return false;
  }
  json.resize(need);
  if (shell.doc.adj_destructive) {
    /* 破坏性态：只更新寄存串（纯函数，不开事务、不写文档），像素等 Ctrl+Shift+U。 */
    shell.doc.destructive_curve_settings = json;
    shell.doc.needs_render = true;
    return true;
  }
  if (comp_layer_update_adjustment_at(shell.doc.document, id,
                                      static_cast<size_t>(shell.doc.selected_adjustment),
                                      COMP_ADJUSTMENT_CURVES, json.c_str()) != COMP_OK) {
    return false;
  }
  shell.doc.needs_render = true;
  return true;
}

void curve_drag_to(ShellState& shell, const comp_ui::Rect& plot, double x, double y) {
  const int32_t index = shell.doc.curve_drag_point;
  if (index < 0) return;
  const int32_t count = static_cast<int32_t>(shell.doc.curve_xy.size() / 2);
  if (index >= count) return;

  double value_x = std::floor(comp_ui::curve_plot_x_to_value(plot, x) + 0.5);
  double value_y = std::floor(comp_ui::curve_plot_y_to_value(plot, y) + 0.5);
  if (index == 0) {
    value_x = 0.0; /* 端点锁 x：曲线定义域必须覆盖 0..255 */
  } else if (index == count - 1) {
    value_x = 255.0;
  } else {
    /* 内部点保持严格递增（比较用整数 x，所以最小间隔就是 1）。 */
    const double lo = shell.doc.curve_xy[static_cast<size_t>(2 * (index - 1))] + 1.0;
    const double hi = shell.doc.curve_xy[static_cast<size_t>(2 * (index + 1))] - 1.0;
    if (value_x < lo) value_x = lo;
    if (value_x > hi) value_x = hi;
  }
  if (value_y < 0.0) value_y = 0.0;
  if (value_y > 255.0) value_y = 255.0;
  const size_t slot = static_cast<size_t>(2 * index);
  if (shell.doc.curve_xy[slot] == value_x && shell.doc.curve_xy[slot + 1] == value_y) return;
  shell.doc.curve_xy[slot] = value_x;
  shell.doc.curve_xy[slot + 1] = value_y;

  if (curve_write_points(shell)) shell.doc.curve_drag_dirty = true;
  shell.doc.needs_render = true;
  wchar_t buffer[200];
  /* 同上：wsprintfW 不支持 %f，先转文本再 %s。 */
  const std::wstring text_x = widen_ascii(number_text(value_x).c_str());
  const std::wstring text_y = widen_ascii(number_text(value_y).c_str());
  wsprintfW(buffer, L"曲线 %s 点 %d/%d = (%s, %s) %s", curve_channel_name(shell.doc.curve_channel),
            index + 1, count, text_x.c_str(), text_y.c_str(),
            shell.doc.curve_drag_txn != nullptr ? L"（实时预览，松手算一步撤销）" : L"（未写入）");
  set_status(shell, buffer);
}

/* 按住控制点开事务 + 冻结绘图区几何（与滑杆同一套理由）。 */
void curve_begin_drag(ShellState& shell, const comp_ui::Rect& plot, int32_t point, double x,
                      double y) {
  if (!curve_editable(shell)) {
    set_status(shell, L"曲线：先 Ctrl+Shift+A 新增一条 Curves（恒等），再从空白处加点");
    return;
  }
  if (shell.doc.document == nullptr) return;
  if (shell.doc.adj_destructive) {
    /* 破坏性态没有文档可写：整段拖动只改寄存串，不需要事务，也不占撤销步。 */
    shell.doc.curve_drag_txn = nullptr;
    shell.doc.curve_drag_point = point;
    shell.doc.curve_focus_point = point;
    shell.doc.curve_drag_dirty = false;
    shell.doc.curve_drag_plot = plot;
    curve_drag_to(shell, plot, x, y);
    return;
  }
  if (comp_txn_begin(shell.doc.document, "Adjust Curve", &shell.doc.curve_drag_txn) != COMP_OK) {
    shell.doc.curve_drag_txn = nullptr;
    set_status(shell, L"曲线：无法开启事务（未改动）");
    return;
  }
  shell.doc.curve_drag_point = point;
  shell.doc.curve_focus_point = point;
  shell.doc.curve_drag_dirty = false;
  shell.doc.curve_drag_plot = plot;
  curve_drag_to(shell, plot, x, y);
}

void curve_drag_finish(ShellState& shell, bool commit) {
  if (shell.doc.curve_drag_point < 0) return;
  const int32_t focused = shell.doc.curve_focus_point;
  const comp_txn txn = shell.doc.curve_drag_txn;
  const bool dirty = shell.doc.curve_drag_dirty;
  shell.doc.curve_drag_point = -1;
  shell.doc.curve_drag_txn = nullptr;
  shell.doc.curve_drag_dirty = false;
  ReleaseCapture();

  if (shell.doc.adj_destructive) {
    /* 破坏性态：寄存串已是最新，不碰撤销栈；也不能 refresh_adjustments（那会把面板
     * 打回调整层模式），点列从寄存串重读即可。 */
    shell.doc.needs_render = true;
    refresh_curve_points(shell);
    shell.doc.curve_focus_point = focused;
    set_status(shell, L"曲线参数已改（Ctrl+Shift+U 落墨，Esc 退出）");
    return;
  }

  bool committed = false;
  if (txn != nullptr) {
    if (commit && dirty) {
      committed = comp_txn_commit(txn) == COMP_OK;
    } else {
      comp_txn_abort(txn);
    }
  }
  shell.doc.needs_render = true;
  refresh_adjustments(shell); /* 重读 settings，点列以引擎为准 */
  shell.doc.curve_focus_point = focused;

  wchar_t buffer[200];
  if (committed) {
    wsprintfW(buffer, L"已应用曲线 %s（一步撤销）", curve_channel_name(shell.doc.curve_channel));
    refresh_history_status(shell, buffer);
  } else if (txn != nullptr && !dirty) {
    refresh_history_status(shell, L"曲线未改动（未产生撤销步）");
  } else if (txn != nullptr) {
    refresh_history_status(shell, L"已回滚曲线拖动");
  } else {
    set_status(shell, L"曲线未写入（无事务）");
  }
}

/* 空白处点击 = 插入一个控制点，并立刻进入拖动（一次按下就能定位）。 */
void curve_add_point(ShellState& shell, const comp_ui::Rect& plot, double x, double y) {
  if (!curve_editable(shell)) {
    set_status(shell, L"曲线：先 Ctrl+Shift+A 新增一条 Curves（恒等），再从空白处加点");
    return;
  }
  if (shell.doc.curve_xy.empty()) {
    shell.doc.curve_xy = {0.0, 0.0, 255.0, 255.0}; /* 空通道从恒等基线开始 */
  }
  const int32_t count = static_cast<int32_t>(shell.doc.curve_xy.size() / 2);
  if (count >= 32) {
    set_status(shell, L"曲线控制点已满（上限 32）");
    return;
  }
  const double value_x = comp_ui::curve_insert_x(
      shell.doc.curve_xy.data(), static_cast<int>(count), comp_ui::curve_plot_x_to_value(plot, x));
  if (std::isnan(value_x)) {
    set_status(shell, L"这里插不进新点（与相邻点的 x 距离不足 1）");
    return;
  }
  double value_y = std::floor(comp_ui::curve_plot_y_to_value(plot, y) + 0.5);
  if (value_y < 0.0) value_y = 0.0;
  if (value_y > 255.0) value_y = 255.0;
  int32_t index = 0;
  while (index < count && shell.doc.curve_xy[static_cast<size_t>(2 * index)] < value_x) ++index;
  shell.doc.curve_xy.insert(shell.doc.curve_xy.begin() + 2 * index, {value_x, value_y});
  curve_begin_drag(shell, plot, index, x, y);
}

/* 删点：端点（x=0 / x=255）锁死，删掉它们曲线都没定义域了。 */
void curve_delete_point(ShellState& shell, int32_t index) {
  if (!curve_editable(shell)) return;
  const int32_t count = static_cast<int32_t>(shell.doc.curve_xy.size() / 2);
  if (index < 0 || index >= count) {
    set_status(shell, L"曲线：没有可删除的控制点（先在曲线上点一下选中）");
    return;
  }
  if (index == 0 || index == count - 1) {
    set_status(shell, L"曲线端点不能删除（x = 0 / x = 255 固定）");
    return;
  }
  shell.doc.curve_xy.erase(shell.doc.curve_xy.begin() + 2 * index,
                       shell.doc.curve_xy.begin() + 2 * index + 2);
  shell.doc.curve_focus_point = -1;
  const bool ok = curve_write_points(shell); /* 隐式事务 = 一步撤销 */
  if (shell.doc.adj_destructive) {
    shell.doc.needs_render = true;
    set_status(shell, ok ? L"已删除曲线控制点（Ctrl+Shift+U 落墨）" : L"删除曲线控制点失败");
    return;
  }
  refresh_adjustments(shell);
  refresh_history_status(shell, ok ? L"已删除曲线控制点（一步撤销）" : L"删除曲线控制点失败");
}

void curve_set_channel(ShellState& shell, int32_t channel) {
  if (channel < 0 || channel >= kCurveChannels) return;
  shell.doc.curve_channel = channel;
  refresh_curve_points(shell);
  wchar_t buffer[120];
  wsprintfW(buffer, L"曲线通道：%s", curve_channel_name(channel));
  set_status(shell, buffer);
}

/* 色阶：切通道页。焦点落到该通道首行，输入缓冲清掉（不在别的通道上提交数字）。 */
void adjust_set_channel(ShellState& shell, int32_t channel) {
  if (channel < 0 || channel >= kLevelsChannelCount) return;
  shell.doc.adj_channel = channel;
  shell.doc.adj_param = adjust_param_base(shell);
  shell.doc.adj_input.clear();
  shell.doc.adj_scroll = 0;
  wchar_t buffer[120];
  wsprintfW(buffer, L"色阶通道：%s", curve_channel_name(channel));
  set_status(shell, buffer);
}

}  // namespace comp_shell
