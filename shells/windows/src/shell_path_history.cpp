// AI生成
/*
 * shell_path_history.cpp — 钢笔路径与历史面板
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {


/* ------------------------------------------------------------ 钢笔（S8） */

bool pen_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Pen;
}

/* 闭合命中半径：屏幕上约 6 px，换算成文档半径，缩放变化不影响手感。 */
double pen_close_radius(const ShellState& shell) {
  const double zoom = shell.doc.view.zoom > 1e-6 ? shell.doc.view.zoom : 1.0;
  return 6.0 / zoom;
}

void pen_refresh_status(ShellState& shell) {
  wchar_t buffer[256];
  wsprintfW(buffer,
            L"钢笔：%d 个锚点（点击继续 / 拖动拉控制柄 / 点首锚点闭合 / Enter 完成 / Esc 取消）",
            static_cast<int>(shell.doc.pen_anchors.size()));
  set_status(shell, buffer);
}

void pen_cancel_drag(ShellState& shell) { shell.doc.pen_dragging = false; }

void pen_cancel(ShellState& shell) {
  const bool had_path = !shell.doc.pen_anchors.empty();
  shell.doc.pen_anchors.clear();
  shell.doc.pen_dragging = false;
  if (had_path) set_status(shell, L"钢笔：已取消当前路径");
}

/*
 * 锚点 -> 贝塞尔段（闭合）。钢笔提交与路径面板「转选区」共用同一条装配口径，
 * 免得两条入口对控制柄的加减、闭合末段处理走偏。
 * 返回可用段数；0 = 没有段（调用方报失败，不改选区）。
 */
int path_build_cubics(const std::vector<comp_ui::PenAnchor>& anchors,
                      std::vector<comp_selection_cubic>* cubics) {
  if (cubics == nullptr) return 0;
  const int anchor_count = static_cast<int>(anchors.size());
  const int segment_count = comp_ui::pen_segment_count(anchor_count, /*closed=*/true);
  if (segment_count <= 0) return 0;
  std::vector<comp_ui::PenSegment> segments(static_cast<size_t>(segment_count));
  const int built = comp_ui::pen_build_segments(anchors.data(), anchor_count,
                                                /*closed=*/true, segments.data(),
                                                segment_count);
  if (built <= 0) return 0;
  const int path_count = std::min(built, segment_count);
  cubics->assign(static_cast<size_t>(path_count), comp_selection_cubic{});
  for (int i = 0; i < path_count; ++i) {
    const comp_ui::PenSegment& source = segments[static_cast<size_t>(i)];
    comp_selection_cubic& target = (*cubics)[static_cast<size_t>(i)];
    target.start[0] = static_cast<float>(source.start.x);
    target.start[1] = static_cast<float>(source.start.y);
    target.control1[0] = static_cast<float>(source.control1.x);
    target.control1[1] = static_cast<float>(source.control1.y);
    target.control2[0] = static_cast<float>(source.control2.x);
    target.control2[1] = static_cast<float>(source.control2.y);
    target.end[0] = static_cast<float>(source.end.x);
    target.end[1] = static_cast<float>(source.end.y);
    /* start 只在第一段（new_subpath = 1）有意义，其余段接着上一段的 end。 */
    target.new_subpath = i == 0 ? 1 : 0;
  }
  return path_count;
}

/*
 * 提交路径：把锚点 + 控制柄装配成 comp_selection_cubic 段，走
 * comp_selection_apply_path（与套索/框选共用 finish_new_selection 的事务与
 * 羽化口径）。钢笔选区总是闭合子路径——这是选区语义，不是图形路径语义。
 */
void commit_pen_path(ShellState& shell) {
  if (shell.doc.document == nullptr) return;
  const int anchor_count = static_cast<int>(shell.doc.pen_anchors.size());
  if (anchor_count < 2) {
    set_status(shell, L"钢笔：至少需要 2 个锚点才能闭合路径");
    return;
  }

  std::vector<comp_selection_cubic> cubics;
  const int path_count = path_build_cubics(shell.doc.pen_anchors, &cubics);
  if (path_count <= 0) {
    set_status(shell, L"钢笔失败：路径没有段");
    return;
  }

  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }
  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"钢笔：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"钢笔：已减选"
                                                                : L"钢笔：已应用闭合选区";

  /* 路径面板要留一份副本：提交后 pen_anchors 就清空了。 */
  const std::vector<comp_ui::PenAnchor> saved_path = shell.doc.pen_anchors;
  /* 先清状态再提交：路径已经变成选区参数，失败也不该留下半截预览。 */
  shell.doc.pen_anchors.clear();
  shell.doc.pen_dragging = false;
  ReleaseCapture();
  bool applied = false;
  finish_new_selection(
      shell, "Pen Selection", ok_text, L"钢笔：选区未变化", L"钢笔失败：路径无效",
      [&](int32_t* changed) {
        return comp_selection_apply_path(shell.doc.document, cubics.data(), path_count, combine,
                                         /*antialias=*/1, changed);
      },
      &applied);
  /*
   * 入列不产生撤销步——它只动壳侧列表，不碰文档。选区没变（changed == 0）时
   * finish_new_selection 会回滚事务、applied 保持 false；那种情况下路径没被提交，
   * 但锚点本身有效，仍然值得留在面板里重新用一次。
   */
  const bool evicted = path_store_add(shell, saved_path);
  if (evicted) {
    wchar_t buffer[192];
    wsprintfW(buffer, L"｜路径已入列 %s（到 %d 条上限，最旧一条被挤掉）",
              shell.doc.paths.back().name.c_str(), comp_ui::kPathMaxEntries);
    shell.status += buffer;
  } else if (applied) {
    wchar_t buffer[256];
    wsprintfW(buffer, L"%s；路径已入列 %s（双击路径面板可再落一次）", ok_text,
              shell.doc.paths.back().name.c_str());
    set_status(shell, buffer);
  }
}

/*
 * 面板挂什么：已提交过的钢笔路径（锚点）。引擎没有路径实体，所以「保存路径」
 * 就是壳侧留一份锚点副本，用途只有一个——再落一次选区。因此这里没有重命名/
 * 复制/导出，只有选中、删除、转选区三件事。
 */
bool path_store_add(ShellState& shell, const std::vector<comp_ui::PenAnchor>& anchors) {
  if (anchors.size() < 2) return false;
  bool evicted = false;
  if (static_cast<int>(shell.doc.paths.size()) >= comp_ui::kPathMaxEntries) {
    shell.doc.paths.erase(shell.doc.paths.begin());
    evicted = true;
  }
  StoredPath entry;
  ++shell.doc.path_serial;
  wchar_t name[32];
  wsprintfW(name, L"路径 %d", shell.doc.path_serial);
  entry.name = name;
  entry.anchors = anchors;
  shell.doc.paths.push_back(std::move(entry));
  shell.doc.path_selected = static_cast<int>(shell.doc.paths.size()) - 1;
  return evicted;
}

/* 文档被替换/新建：锚点是旧文档坐标，留着只会把选区落到错误的位置。 */
void path_store_clear(ShellState& shell) {
  shell.doc.path_open = false;
  shell.doc.paths.clear();
  shell.doc.path_selected = 0;
  shell.doc.path_scroll = 0;
  shell.doc.path_serial = 0;
}

/* 文字工具（T）独占右栏显示它自己的属性面板：此时路径面板只是「挂着不显示」。 */
bool path_panel_visible(const ShellState& shell) {
  return shell.doc.path_open && shell.tool != comp_ui::ToolId::Type;
}

void path_status(ShellState& shell) {
  if (shell.doc.paths.empty()) {
    set_status(shell, L"路径：还没有保存的路径（钢笔闭合提交后自动入列）");
    return;
  }
  const int count = static_cast<int>(shell.doc.paths.size());
  int index = shell.doc.path_selected;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  const StoredPath& path = shell.doc.paths[static_cast<size_t>(index)];
  wchar_t buffer[224];
  wsprintfW(buffer, L"路径 %d/%d：%s（%d 个锚点）· 回车转选区 · Del 删除", index + 1, count,
            path.name.c_str(), static_cast<int>(path.anchors.size()));
  set_status(shell, buffer);
}

void path_select(ShellState& shell, int delta) {
  const int count = static_cast<int>(shell.doc.paths.size());
  if (count <= 0) {
    path_status(shell);
    return;
  }
  int index = shell.doc.path_selected + delta;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  shell.doc.path_selected = index;
  /* 键盘选行要把高亮滚进可视区，否则↑/↓ 走到看不见的行就没反馈了。 */
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.doc.path_scroll = comp_ui::path_ensure_visible(layout.adjust_panel, count, index,
                                                     shell.doc.path_scroll);
  }
  path_status(shell);
}

/* 重新落成选区：与钢笔提交同一条装配口径，同样一步撤销（套索/形状同款）。 */
void path_apply_selected(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"路径：还没有文档");
    return;
  }
  const int count = static_cast<int>(shell.doc.paths.size());
  if (count <= 0 || shell.doc.path_selected < 0 || shell.doc.path_selected >= count) {
    set_status(shell, L"路径面板：先选中一条路径");
    return;
  }
  const StoredPath& path = shell.doc.paths[static_cast<size_t>(shell.doc.path_selected)];
  std::vector<comp_selection_cubic> cubics;
  const int path_count = path_build_cubics(path.anchors, &cubics);
  if (path_count <= 0) {
    set_status(shell, L"路径：该条没有可用的段（未改动）");
    return;
  }
  wchar_t ok_text[192];
  wsprintfW(ok_text, L"路径「%s」已转成选区（一步撤销）", path.name.c_str());
  finish_new_selection(
      shell, "Path Selection", ok_text, L"路径：这次没有圈到选区（未改动）",
      L"路径：转选区失败（未改动）", [&](int32_t* changed) {
        return comp_selection_apply_path(shell.doc.document, cubics.data(), path_count,
                                         COMP_SELECTION_REPLACE, /*antialias=*/1, changed);
      });
}

/* 删一条路径只动壳侧列表：文档没变，所以不产生撤销步（状态栏里说清楚）。 */
void path_delete_selected(ShellState& shell) {
  const int count = static_cast<int>(shell.doc.paths.size());
  if (count <= 0) {
    set_status(shell, L"路径面板：没有可删除的路径");
    return;
  }
  int index = shell.doc.path_selected;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  const std::wstring name = shell.doc.paths[static_cast<size_t>(index)].name;
  shell.doc.paths.erase(shell.doc.paths.begin() + index);
  if (shell.doc.path_selected >= static_cast<int>(shell.doc.paths.size())) {
    shell.doc.path_selected = static_cast<int>(shell.doc.paths.size()) - 1;
  }
  if (shell.doc.paths.empty()) {
    shell.doc.path_selected = 0;
    shell.doc.path_scroll = 0;
  } else {
    Layout layout{};
    if (client_layout(shell, &layout)) {
      shell.doc.path_scroll = comp_ui::path_ensure_visible(
          layout.adjust_panel, static_cast<int>(shell.doc.paths.size()), shell.doc.path_selected,
          shell.doc.path_scroll);
    }
  }
  wchar_t buffer[192];
  if (shell.doc.paths.empty()) {
    wsprintfW(buffer, L"已删除路径「%s」（列表清空，选区不动）", name.c_str());
  } else {
    wsprintfW(buffer, L"已删除路径「%s」（剩 %d 条，选区不动）", name.c_str(),
              static_cast<int>(shell.doc.paths.size()));
  }
  set_status(shell, buffer);
}

void path_panel_toggle(ShellState& shell) {
  if (shell.doc.path_open) {
    shell.doc.path_open = false;
    set_status(shell, L"已关闭路径面板");
    return;
  }
  if (shell.doc.document == nullptr) {
    set_status(shell, L"路径面板：还没有文档");
    return;
  }
  /* 右栏一次只挂一个面板：色彩范围持有显式事务，必须先 abort 再让位。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  smart_panel_dismiss(shell);
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.path_open = true;
  shell.doc.history_open = false;
  channel_panel_close(shell);
  const int count = static_cast<int>(shell.doc.paths.size());
  if (shell.doc.path_selected >= count) shell.doc.path_selected = count - 1;
  if (shell.doc.path_selected < 0) shell.doc.path_selected = 0;
  if (!shell.doc.paths.empty()) {
    Layout layout{};
    if (client_layout(shell, &layout)) {
      shell.doc.path_scroll = comp_ui::path_ensure_visible(layout.adjust_panel, count,
                                                       shell.doc.path_selected, shell.doc.path_scroll);
    }
  }
  if (!path_panel_visible(shell)) {
    set_status(shell, L"路径面板已打开（T 文字工具占着右栏，切走后可见）");
    return;
  }
  set_status(shell, L"路径面板：↑/↓ 选行 · 回车（或双击行）转成选区 · Del 删除 · Esc 关闭");
}

/* ------------------------------------------------------ 历史记录面板（P1） */

/*
 * 面板列的是「状态」而不是「步」：index 0 = 最初状态（新建/打开），index i =
 * 第 i 步之后的状态。高亮行不单独记账——它就是引擎的 current_index，所以撤销/
 * 重做/新操作之后不会出现「高亮停在不存在的状态上」。列表本身来自
 * comp_document_history_step_count / step_label（ABI 尾部追加，零结构体改动）。
 */
bool history_panel_visible(const ShellState& shell) {
  return shell.doc.history_open && shell.tool != comp_ui::ToolId::Type;
}

int history_state_count(const ShellState& shell) {
  if (shell.doc.document == nullptr) return 1;
  size_t steps = 0;
  if (comp_document_history_step_count(shell.doc.document, &steps) != COMP_OK) return 1;
  return static_cast<int>(steps) + 1;
}

int history_current(const ShellState& shell) {
  if (shell.doc.document == nullptr) return 0;
  size_t undo = 0;
  size_t redo = 0;
  if (comp_document_history_stats(shell.doc.document, &undo, &redo) != COMP_OK) return 0;
  return static_cast<int>(undo);
}

/* 第 index 个状态的标题；标签是 UTF-8（写入方给的），面板只负责展示。 */
std::wstring history_state_label(const ShellState& shell, int index) {
  if (index <= 0) return L"最初状态（新建 / 打开）";
  char utf8[192];
  size_t required = 0;
  if (shell.doc.document == nullptr ||
      comp_document_history_step_label(shell.doc.document, static_cast<size_t>(index), nullptr, 0,
                                       &required) != COMP_OK ||
      required == 0 || required > sizeof(utf8) ||
      comp_document_history_step_label(shell.doc.document, static_cast<size_t>(index), utf8,
                                       sizeof(utf8), &required) != COMP_OK) {
    return L"（无法读取标签）";
  }
  return widen_utf8(utf8);
}

void history_status(ShellState& shell) {
  const int count = history_state_count(shell);
  const int current = history_current(shell);
  wchar_t buffer[288];
  wsprintfW(buffer, L"历史 第 %d / %d 个状态：%s · ↑/↓ 或点行回跳 · Esc 关闭",
            current + 1, count, history_state_label(shell, current).c_str());
  set_status(shell, buffer);
}

/*
 * 回跳：一次 ABI 调用把当前下标移到目标（内部连续 undo/redo），不新增历史记录。
 * 回跳等于换整篇文档状态（图层、选区、调整都可能变），所以刷新口径与
 * undo_document/redo_document 完全一致，只是把两次刷新合成一次。
 */
void history_jump_to(ShellState& shell, int target) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"历史记录：还没有文档");
    return;
  }
  const int count = history_state_count(shell);
  if (target < 0) target = 0;
  if (target >= count) target = count - 1;
  const int current = history_current(shell);
  if (target == current) {
    set_status(shell, L"已在该状态（未改动，不产生撤销步）");
    return;
  }
  if (comp_document_history_jump(shell.doc.document, static_cast<size_t>(target)) != COMP_OK) {
    set_status(shell, L"回跳失败");
    return;
  }
  shell.doc.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell);
  const int after = history_current(shell);
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.doc.history_scroll = comp_ui::history_ensure_visible(layout.adjust_panel, count, after,
                                                           shell.doc.history_scroll);
  }
  wchar_t prefix[224];
  wsprintfW(prefix, L"已回跳到第 %d/%d 个状态：%s", after + 1, count,
            history_state_label(shell, after).c_str());
  refresh_history_status(shell, prefix);
}

void history_step(ShellState& shell, int delta) {
  const int count = history_state_count(shell);
  int target = history_current(shell) + delta;
  if (target < 0) target = 0;
  if (target >= count) target = count - 1;
  history_jump_to(shell, target);
}

/*
 * 历史记录画笔的取样下标（面板下标，不是撤销步数）。
 * -1 = 自动 = 当前状态的上一格：「往回刷一步」是最常用的用法，也正好是「刚
 * 画错一笔，拿历史画笔抹掉」的直觉。显式下标（Shift+点行）夹到 [0, count-1]，
 * 面板行数变少（回跳后重做侧消失不可能，但换文档会）也不会越界。
 */
int history_brush_resolved_state(const ShellState& shell) {
  const int count = history_state_count(shell);
  if (count <= 0) return 0;
  if (shell.doc.history_brush_state < 0) {
    const int target = history_current(shell) - 1;
    return target < 0 ? 0 : target;
  }
  return std::min(shell.doc.history_brush_state, count - 1);
}

void history_panel_toggle(ShellState& shell) {
  if (shell.doc.history_open) {
    shell.doc.history_open = false;
    set_status(shell, L"已关闭历史记录面板");
    return;
  }
  if (shell.doc.document == nullptr) {
    set_status(shell, L"历史记录：还没有文档");
    return;
  }
  /* 右栏一次只挂一个面板：色彩范围持有显式事务，必须先 abort 再让位。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  smart_panel_dismiss(shell);
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.path_open = false;
  shell.doc.history_open = true;
  channel_panel_close(shell);
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.doc.history_scroll = comp_ui::history_ensure_visible(
        layout.adjust_panel, history_state_count(shell), history_current(shell),
        shell.doc.history_scroll);
  }
  if (!history_panel_visible(shell)) {
    set_status(shell, L"历史记录面板已打开（T 文字工具占着右栏，切走后可见）");
    return;
  }
  history_status(shell);
}

void pen_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!pen_available(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  const int anchor_count = static_cast<int>(shell.doc.pen_anchors.size());
  /* 点第一个锚点 = 闭合（首锚点至少要有 2 个锚点才有闭合路径可言）。 */
  if (comp_ui::pen_hits_first_anchor(shell.doc.pen_anchors.data(), anchor_count, doc,
                                     pen_close_radius(shell))) {
    commit_pen_path(shell);
    return;
  }
  const size_t index = static_cast<size_t>(anchor_count);
  shell.doc.pen_anchors.resize(index + 1u);
  int next = anchor_count;
  if (!comp_ui::pen_append_anchor(shell.doc.pen_anchors.data(), &next, doc)) {
    shell.doc.pen_anchors.resize(index);
    wchar_t buffer[160];
    wsprintfW(buffer, L"钢笔：锚点数已达上限 %d", comp_ui::kPenMaxAnchors);
    set_status(shell, buffer);
    return;
  }
  shell.doc.pen_anchors.resize(static_cast<size_t>(next));
  /* 按住不放就进入「拖控制柄」状态；只点不拖时松开即得到直角锚点。 */
  shell.doc.pen_dragging = true;
  SetCapture(window);
  pen_refresh_status(shell);
}

void pen_drag(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.pen_dragging || shell.doc.pen_anchors.empty()) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  comp_ui::pen_drag_handle(shell.doc.pen_anchors.data(),
                           static_cast<int>(shell.doc.pen_anchors.size()), doc);
}

void pen_up(HWND window, ShellState& shell) {
  if (!shell.doc.pen_dragging) return;
  shell.doc.pen_dragging = false;
  ReleaseCapture();
  pen_refresh_status(shell);
}

/* ------------------------------------------------------------ 历史记录画笔（P1） */

/*
 * 与 Tone / Retouch 共用同一条「鼠标折线 -> comp_tool_apply」通路：折线在文档
 * 坐标累积，松开鼠标时一次性提交（= 恰好一个撤销步）。取样状态只在提交时读
 * 一次——笔画途中点开历史面板、回跳都不会改变这一笔的来源。
 */
bool history_brush_stroke_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::HistoryBrush;
}

void history_brush_stroke_begin(HWND window, ShellState& shell, int x, int y,
                                const Layout& layout) {
  if (!history_brush_stroke_available(shell)) return;
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::HistoryBrush)) return;
  SetCapture(window);
  const int sample = history_brush_resolved_state(shell);
  wchar_t buffer[320];
  wsprintfW(buffer, L"历史记录画笔：取样第 %d 个状态（%s），松开鼠标刷回｜不透明度 %d%%", sample,
            history_state_label(shell, sample).c_str(),
            static_cast<int>(shell.history_brush_opacity * 100.0 + 0.5));
  set_status(shell, buffer);
}

void history_brush_stroke_end(HWND window, ShellState& shell) {
  if (shell.doc.stroke_kind != comp_ui::StrokeKind::HistoryBrush) return;
  shell.doc.stroke_kind = comp_ui::StrokeKind::None;

  /* 同 tone_stroke_end：折线必须先搬走，ReleaseCapture 会同步触发取消。 */
  std::vector<float> points;
  points.swap(shell.doc.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"历史记录画笔：请先选择一个图层");
    return;
  }

  const int sample = history_brush_resolved_state(shell);
  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_HISTORY_BRUSH;
  desc.history.state = sample;
  desc.history.opacity =
      static_cast<float>(comp_ui::clamp_history_brush_opacity(shell.history_brush_opacity));
  desc.stroke_points = points.data();
  desc.stroke_point_count = static_cast<int32_t>(points.size() / 2);
  desc.brush.diameter = static_cast<float>(shell.brush_size);
  desc.brush.hardness = static_cast<float>(shell.brush_hardness);
  desc.brush.pressure = 1.0f; /* 鼠标无压感：按上游满笔压 */
  desc.brush.flow = 1.0f;
  desc.brush.seed = 0; /* 不用随机数；固定值保证同参数同结果 */

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.doc.document, id, &desc, note, sizeof(note), &note_size);
  shell.doc.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    /*
     * 最常见的拒绝：取样状态里还没有当前图层（图层是后加的），引擎给
     * COMP_ERR_STATE。把「取样下标」写进状态栏，用户才知道该换一格。
     */
    wsprintfW(buffer, L"历史记录画笔失败：取样第 %d 个状态（该状态下可能没有此图层）",
              sample);
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"历史记录画笔：取样第 %d 个状态，已应用 %d 个折点｜%s", sample,
              desc.stroke_point_count, widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"历史记录画笔：取样第 %d 个状态，已应用 %d 个折点（不透明度 %d%%）",
              sample, desc.stroke_point_count,
              static_cast<int>(desc.history.opacity * 100.0f + 0.5f));
  }
  refresh_history_status(shell, buffer);
}

}  // namespace comp_shell
