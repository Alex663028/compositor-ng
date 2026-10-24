// AI生成
/*
 * shell_tools.cpp — 笔刷/修复/仿制/渐变/形状/变换等工具笔画
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {


/*
 * §6.61 整组移动的共用件：定义写在拖拽移动那一段（那里是主用法），方向键微调也用它，
 * 所以先在这里前置声明——与 panel_select_layer_id 同一做法。
 */
void collect_group_members(const ShellState& shell, comp_layer_id root,
                           std::vector<GroupMemberMove>* out);
void build_move_targets(const ShellState& shell,
                        std::vector<GroupMemberMove>* out);
bool members_off_canvas(const std::vector<GroupMemberMove>& members, int32_t dx,
                        int32_t dy, int32_t view_width, int32_t view_height);

/*
 * 连续微调的事务收口（§5.30 补）。
 *
 * nudge_flush 只做「把已开的事务结束掉」：有写入才 commit（commit 才压撤销记录），
 * 一次都没写成功就 abort，免得历史面板里出现一条什么都没改的步骤。调用点是
 * 「任何可能改变文档或另开事务的动作」——见 WM_KEYUP/WM_LBUTTONDOWN/WM_INITMENU/
 * WM_KILLFOCUS/WM_CLOSE 与 run_shell_command 入口，都是先收口再干活。
 * 返回值 = 这次是否真的压进了一条撤销记录（WM_KEYUP 用它决定要不要刷新历史栏）。
 */
bool nudge_flush(ShellState& shell) {
  if (shell.doc.nudge_txn == nullptr) return false;
  comp_txn txn = shell.doc.nudge_txn;
  const bool dirty = shell.doc.nudge_dirty;
  shell.doc.nudge_txn = nullptr;
  shell.doc.nudge_anchor = 0;
  shell.doc.nudge_dirty = false;
  if (!dirty) {
    comp_txn_abort(txn);
    return false;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"微调事务提交失败");
    return false;
  }
  return true;
}

/*
 * 取当前这段连续微调的事务：锚点相同就沿用（这是「合并成一步」的全部机关），
 * 锚点变了先收口再新开。返回 nullptr = 开不了事务，调用方应如实报错而不是
 * 退回隐式事务——那会把合并语义悄悄丢掉。
 */
comp_txn nudge_begin(ShellState& shell, comp_layer_id anchor, const char* label) {
  if (shell.doc.nudge_txn != nullptr && shell.doc.nudge_anchor == anchor) return shell.doc.nudge_txn;
  nudge_flush(shell);
  comp_txn txn = nullptr;
  if (shell.doc.document == nullptr || comp_txn_begin(shell.doc.document, label, &txn) != COMP_OK) {
    return nullptr;
  }
  shell.doc.nudge_txn = txn;
  shell.doc.nudge_anchor = anchor;
  return txn;
}

/* ------------------------------------------------------------ 渐变（S9） */

/*
 * 渐变工具（COMP_TOOL_GRADIENT）：窗口路径是「拖出两个端点 -> 一次 comp_tool_apply」。
 *
 * 为什么不在拖拽过程中反复提交：渐变是一次性铺满整层的像素算子，中途每一步都提交
 * 会在撤销栈里留下几十步。预览只画方向线（draw_canvas_overlays），松开才算一次操作。
 *
 * 色标固定为「前景色 -> 透明」：外壳只有前景色模型（S6），第二色标没有 UI 可调；
 * 与其编一个宿主不存在的背景色，不如用上游同样存在的这个预设，并如实写进状态栏。
 * 选区活跃时引擎按 coverage 混合，因此「先套选区再拉渐变」只改选区内。
 */
bool gradient_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Gradient;
}

void gradient_cancel(ShellState& shell) { shell.doc.gradient_active = false; }

void gradient_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!gradient_available(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.gradient_active = true;
  shell.doc.gradient_start = doc;
  shell.doc.gradient_current = doc;
  SetCapture(window);
  wchar_t buffer[192];
  wsprintfW(buffer, L"渐变 / %s：拖出方向与长度后松开（再按 G 切换类型；Shift 反向）",
            widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str());
  set_status(shell, buffer);
}

void gradient_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.gradient_active) return;
  shell.doc.gradient_current =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
}

void gradient_end(HWND window, ShellState& shell) {
  if (!shell.doc.gradient_active) return;
  shell.doc.gradient_active = false;
  ReleaseCapture();

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"渐变：请先选择一个图层");
    return;
  }
  const comp_ui::Point start = shell.doc.gradient_start;
  const comp_ui::Point end = shell.doc.gradient_current;
  if (!comp_ui::gradient_drag_is_usable(start.x, start.y, end.x, end.y)) {
    set_status(shell, L"渐变：拖拽太短，已放弃（方向/半径需要一个可定义的长度）");
    return;
  }

  /*
   * Shift = 反向：色标顺序整个翻转（单色标时就是「透明 -> 前景色」）。这是
   * 拖拽结束后才读的修饰键（与框选/套索的 Shift=加选同口径：提交时才决定组合）。
   */
  const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_GRADIENT;
  desc.gradient.type = shell.gradient_type;
  desc.gradient.start_x = start.x;
  desc.gradient.start_y = start.y;
  desc.gradient.end_x = end.x;
  desc.gradient.end_y = end.y;
  const uint8_t foreground[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 255};
  /*
   * P1-7：色标表来自选项栏的预设（只读表，见 ui_core::gradient_preset_stops）。
   * 预设 0 展开成「前景 -> 透明」两个色标，与 P1-7 之前的两色标结果逐字节一致。
   * 展开失败（不可能：容量 5 覆盖所有预设）时退回两色标旧路径。
   */
  comp_ui::GradientPresetStop preset_stops[5];
  const int preset_count = comp_ui::gradient_preset_stops(
      shell.gradient_preset, foreground, reverse, preset_stops, 5);
  comp_gradient_stop stops[5]{};
  if (preset_count >= 2) {
    for (int i = 0; i < preset_count; ++i) {
      stops[i].position = preset_stops[i].position;
      stops[i].midpoint = preset_stops[i].midpoint;
      for (int c = 0; c < 4; ++c) stops[i].color[c] = preset_stops[i].color[c];
    }
    desc.gradient_stops = stops;
    desc.gradient_stop_count = static_cast<size_t>(preset_count);
  } else {
    const uint8_t solid[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 255};
    const uint8_t clear[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 0};
    const uint8_t* color0 = reverse ? clear : solid;
    const uint8_t* color1 = reverse ? solid : clear;
    for (int i = 0; i < 4; ++i) {
      desc.gradient.color0[i] = color0[i];
      desc.gradient.color1[i] = color1[i];
    }
  }
  desc.gradient.opacity = 1.0f;
  desc.gradient_dither = shell.gradient_dither ? 1 : 0;

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.doc.document, id, &desc, note, sizeof(note), &note_size);
  shell.doc.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"渐变失败（%d）", static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  const wchar_t* dither_text = shell.gradient_dither ? L" + 抖动" : L"";
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"渐变 / %s / %s%s：已应用｜%s",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str(),
              widen_ascii(comp_ui::gradient_preset_name(shell.gradient_preset)).c_str(),
              dither_text, widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"渐变 / %s / %s%s%s：已应用（%d 个色标）",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str(),
              widen_ascii(comp_ui::gradient_preset_name(shell.gradient_preset)).c_str(),
              dither_text, reverse ? L" · 反向" : L"", preset_count >= 2 ? preset_count : 2);
  }
  refresh_history_status(shell, buffer);
}

/* ---------------------------------------------- Progressive Blur 渐变轴（P1） */

/*
 * Progressive Blur 的轴不走面板行：滤镜面板开着且选中该滤镜时，画布拖拽
 * 只用来定一条「起点 -> 终点」的轴（与色彩范围面板「画布点击只取样」同优先级，
 * 不交给当前绘图工具）。松手只记端点，回车才应用——面板仍然是
 * 「回车应用、Esc 关闭」，这里不产生撤销步。
 *
 * 轴端点是**文档坐标**，可以落在图层外：引擎在图层像素空间里做投影，渐变
 * 因此能横跨整幅画布（上游 `progressiveStart/End` 同为文档坐标）。
 */
bool progressive_axis_available(const ShellState& shell) {
  return shell.doc.filter_open && shell.doc.filter.id == comp_ui::FilterId::ProgressiveBlur;
}

void progressive_axis_cancel(ShellState& shell) { shell.doc.progressive_axis_dragging = false; }

void progressive_axis_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!progressive_axis_available(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.progressive_axis_start = doc;
  shell.doc.progressive_axis_current = doc;
  shell.doc.progressive_axis_dragging = true;
  SetCapture(window);
  set_status(shell,
             L"Progressive Blur：拖出渐变轴（起点 = From 端、终点 = To 端；松手出预览，回车应用）");
}

void progressive_axis_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.progressive_axis_dragging) return;
  shell.doc.progressive_axis_current =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
}

void progressive_axis_end(HWND window, ShellState& shell) {
  if (!shell.doc.progressive_axis_dragging) return;
  /* 先清状态再 ReleaseCapture：捕获丢失会同步派发 WM_CAPTURECHANGED（S15 的坑）。 */
  shell.doc.progressive_axis_dragging = false;
  ReleaseCapture();

  /* 拖拽途中 Esc / 别的面板把滤镜面板关掉了：只放掉捕获，不把轴偷偷提交。 */
  if (!progressive_axis_available(shell)) return;

  const comp_ui::Point start = shell.doc.progressive_axis_start;
  const comp_ui::Point end = shell.doc.progressive_axis_current;
  if (!comp_ui::gradient_drag_is_usable(start.x, start.y, end.x, end.y)) {
    set_status(shell, L"Progressive Blur：轴太短，已放弃（需要一条可定义的方向）");
    return;
  }
  shell.doc.progressive_axis_final = end;
  shell.doc.progressive_axis_set = true;
  wchar_t buffer[224];
  wsprintfW(buffer, L"Progressive Blur 渐变轴：(%.0f, %.0f) → (%.0f, %.0f)",
            start.x, start.y, end.x, end.y);
  /* 轴是滤镜参数的一部分：定完轴立刻按新轴出一帧预览。 */
  filter_preview_render(shell, buffer);
}

/* ------------------------------------------------------------ 形状（S10） */

/*
 * 形状工具（COMP_TOOL_SHAPE）：窗口路径是「拖出对角点 / 两端点 -> 一次 comp_tool_apply」。
 *
 * 与渐变同理，不在拖拽过程中反复提交：形状是一次性铺像素的算子，中途每一步都提交
 * 会在撤销栈里留下几十步。预览只画几何轮廓（draw_canvas_overlays），松开才算一次操作。
 *
 * 外壳能产出的几何：矩形 / 椭圆（填充前景色）与直线（前景色描边，圆头端点）；
 * 通用贝塞尔路径只经 ABI——本壳的钢笔用来提交选区，不造形状。圆角半径只对矩形
 * 有效（0 = 直角），引擎再把半径夹到短边一半。Shift = 正方/正圆、Alt = 从中心拉，
 * 与上游 ShapeTool 的修饰键一致。
 */
bool shape_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Shape;
}

void shape_cancel(ShellState& shell) { shell.doc.shape_active = false; }

void shape_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!shape_available(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.shape_active = true;
  shell.doc.shape_start = doc;
  shell.doc.shape_current = doc;
  SetCapture(window);
  wchar_t buffer[224];
  wsprintfW(buffer,
            L"形状 / %s：拖出%s后松开（Shift = 正方/正圆，Alt = 从中心；再按 U 切种类）",
            widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
            shell.shape_kind == comp_ui::kShapeLine ? L"两端点" : L"对角点");
  set_status(shell, buffer);
}

void shape_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.shape_active) return;
  shell.doc.shape_current =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
}

void shape_end(HWND window, ShellState& shell) {
  if (!shell.doc.shape_active) return;
  shell.doc.shape_active = false;
  ReleaseCapture();

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"形状：请先选择一个图层");
    return;
  }

  /* 修饰键在松开时才读：拖动中按下/松开 Shift、Alt 都能改落笔盒（与框选同口径）。 */
  const bool square = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool from_center = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const comp_ui::ShapeDragBox box = comp_ui::shape_drag_box(
      shell.doc.shape_start.x, shell.doc.shape_start.y, shell.doc.shape_current.x, shell.doc.shape_current.y,
      square, from_center);
  const int kind = shell.shape_kind;
  if (!comp_ui::shape_drag_is_usable(kind, box)) {
    set_status(shell, kind == comp_ui::kShapeLine
                          ? L"形状：拖拽太短，已放弃（直线需要两个不同端点）"
                          : L"形状：拖拽太短，已放弃（宽高都至少需要 1 px）");
    return;
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_SHAPE;
  desc.shape.kind = static_cast<comp_shape_kind>(kind);
  desc.shape.x0 = box.x0;
  desc.shape.y0 = box.y0;
  desc.shape.x1 = box.x1;
  desc.shape.y1 = box.y1;
  desc.shape.corner_radius =
      kind == comp_ui::kShapeRect
          ? comp_ui::shape_effective_corner_radius(shell.shape_corner_radius, box)
          : 0.0;
  const bool line = kind == comp_ui::kShapeLine;
  const uint8_t color[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 255};
  for (int i = 0; i < 4; ++i) {
    desc.shape.style.fill_color[i] = color[i];
    desc.shape.style.stroke_color[i] = color[i];
  }
  /* 直线只有描边（引擎会拒绝「填充直线」）：矩形/椭圆的外壳默认是纯填充，与上游一致。 */
  desc.shape.style.fill_enabled = line ? 0 : 1;
  desc.shape.style.stroke_enabled = line ? 1 : 0;
  desc.shape.style.stroke_width = comp_ui::clamp_shape_stroke_width(shell.shape_stroke_width);
  desc.shape.style.opacity = 1.0f;
  desc.shape.style.antialias = 1;

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.doc.document, id, &desc, note, sizeof(note), &note_size);
  shell.doc.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"形状失败（%d）", static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    /* 引擎的 note 会如实说明「被选区遮罩」「与图层不相交」这类非致命情况。 */
    wsprintfW(buffer, L"形状 / %s：已应用｜%s",
              widen_ascii(comp_ui::shape_kind_name(kind)).c_str(), widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"形状 / %s：已应用", widen_ascii(comp_ui::shape_kind_name(kind)).c_str());
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 画布落笔（折线） */

/*
 * Retouch 与画笔/橡皮擦共用同一条「鼠标折线 -> comp_tool_apply」通路：
 * 折线按文档像素累积，松开鼠标时一次性提交，中间不产生事务。
 */
bool stroke_in_progress(const ShellState& shell) {
  return shell.doc.stroke_kind != comp_ui::StrokeKind::None;
}

bool stroke_begin(ShellState& shell, int x, int y, const Layout& layout,
                  comp_ui::StrokeKind kind) {
  if (shell.doc.document == nullptr || kind == comp_ui::StrokeKind::None) return false;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.stroke_points.clear();
  shell.doc.stroke_pressure_sum = 0.0;
  shell.doc.stroke_pressure_samples = 0;
  /* 单击也要留下一记 dab：单点折线在 COMP_TOOL_STROKE 里就是一个圆形笔尖。 */
  shell.doc.stroke_points.push_back(static_cast<float>(doc.x));
  shell.doc.stroke_points.push_back(static_cast<float>(doc.y));
  shell.doc.stroke_kind = kind;
  return true;
}

void stroke_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!stroke_in_progress(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  const size_t count = shell.doc.stroke_points.size();
  const double last_x = count >= 2 ? shell.doc.stroke_points[count - 2] : doc.x;
  const double last_y = count >= 2 ? shell.doc.stroke_points[count - 1] : doc.y;
  if (!comp_ui::stroke_point_should_append(last_x, last_y, doc.x, doc.y)) return;
  shell.doc.stroke_points.push_back(static_cast<float>(doc.x));
  shell.doc.stroke_points.push_back(static_cast<float>(doc.y));
}

/* 失焦 / 捕获丢失 / Esc：两种笔迹共用同一个取消动作。 */
void stroke_cancel(ShellState& shell) {
  shell.doc.stroke_kind = comp_ui::StrokeKind::None;
  shell.doc.stroke_points.clear();
  shell.doc.stroke_erase = false;
}

/* ------------------------------------------------------------ Retouch 笔迹 */

/* Blur / Smudge / Liquify 共用同一条「鼠标折线 -> comp_tool_apply」通路。 */
bool retouch_stroke_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Retouch;
}

void retouch_stroke_begin(HWND window, ShellState& shell, int x, int y,
                          const Layout& layout) {
  if (!retouch_stroke_available(shell)) return;
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::Retouch)) return;
  SetCapture(window);
  wchar_t buffer[128];
  wsprintfW(buffer, L"%s 笔迹…（松开鼠标应用）",
            widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str());
  set_status(shell, buffer);
}

void retouch_stroke_end(HWND window, ShellState& shell) {
  if (shell.doc.stroke_kind != comp_ui::StrokeKind::Retouch) return;
  shell.doc.stroke_kind = comp_ui::StrokeKind::None;

  /* 同 paint_stroke_end：折线必须先搬走，ReleaseCapture 会同步触发取消。 */
  std::vector<float> points;
  points.swap(shell.doc.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"Retouch：请先选择一个图层");
    return;
  }

  comp_tool_op_desc desc{};
  switch (shell.retouch_mode) {
    case comp_ui::kRetouchBlur:
      desc.kind = COMP_TOOL_BLUR;
      desc.blur.radius = static_cast<float>(shell.blur_radius);
      break;
    case comp_ui::kRetouchSmudge:
      desc.kind = COMP_TOOL_SMUDGE;
      desc.warp.strength = static_cast<float>(shell.retouch_strength);
      break;
    case comp_ui::kRetouchLiquify:
      desc.kind = COMP_TOOL_LIQUIFY;
      desc.warp.strength = static_cast<float>(shell.retouch_strength);
      break;
    default:
      return;
  }
  desc.stroke_points = points.data();
  desc.stroke_point_count = static_cast<int32_t>(points.size() / 2);
  desc.brush.diameter = static_cast<float>(shell.brush_size);
  desc.brush.hardness = static_cast<float>(shell.brush_hardness);
  desc.brush.pressure = 1.0f; /* 鼠标无压感：按上游满笔压 */
  desc.brush.flow = 1.0f;
  desc.brush.seed = 0; /* 三个算子都不用随机数；固定值保证同参数同结果 */

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.doc.document, id, &desc, note, sizeof(note), &note_size);
  shell.doc.needs_render = true;

  const char* mode_name = comp_ui::retouch_mode_name(shell.retouch_mode);
  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"%s 失败", widen_ascii(mode_name).c_str());
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"%s：已应用 %d 个折点｜%s", widen_ascii(mode_name).c_str(),
              desc.stroke_point_count, widen_ascii(note).c_str());
  } else if (shell.retouch_mode == comp_ui::kRetouchBlur) {
    wsprintfW(buffer, L"%s：已应用 %d 个折点（半径 %d px）",
              widen_ascii(mode_name).c_str(), desc.stroke_point_count,
              static_cast<int>(shell.blur_radius + 0.5));
  } else {
    wsprintfW(buffer, L"%s：已应用 %d 个折点（强度 %d%%）",
              widen_ascii(mode_name).c_str(), desc.stroke_point_count,
              static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 色调笔迹（减淡 / 加深 / 海绵） */

/* Dodge / Burn / Sponge 共用同一条「鼠标折线 -> comp_tool_apply」通路。 */
bool tone_stroke_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Tone;
}

void tone_stroke_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!tone_stroke_available(shell)) return;
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::Tone)) return;
  SetCapture(window);
  wchar_t buffer[160];
  wsprintfW(buffer, L"%s 笔迹…（松开鼠标应用，强度 %d%%）",
            widen_ascii(comp_ui::tone_state_name(shell.tone_state)).c_str(),
            static_cast<int>(shell.tone_strength * 100.0 + 0.5));
  set_status(shell, buffer);
}

void tone_stroke_end(HWND window, ShellState& shell) {
  if (shell.doc.stroke_kind != comp_ui::StrokeKind::Tone) return;
  shell.doc.stroke_kind = comp_ui::StrokeKind::None;

  /* 同 retouch_stroke_end：折线必须先搬走，ReleaseCapture 会同步触发取消。 */
  std::vector<float> points;
  points.swap(shell.doc.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"减淡/加深/海绵：请先选择一个图层");
    return;
  }

  comp_tool_op_desc desc{};
  switch (comp_ui::tone_state_tool(shell.tone_state)) {
    case comp_ui::kToneToolDodge: desc.kind = COMP_TOOL_DODGE; break;
    case comp_ui::kToneToolBurn: desc.kind = COMP_TOOL_BURN; break;
    default: desc.kind = COMP_TOOL_SPONGE; break;
  }
  desc.tone.mode = comp_ui::tone_state_mode(shell.tone_state);
  desc.tone.strength = static_cast<float>(shell.tone_strength);
  desc.stroke_points = points.data();
  desc.stroke_point_count = static_cast<int32_t>(points.size() / 2);
  desc.brush.diameter = static_cast<float>(shell.brush_size);
  desc.brush.hardness = static_cast<float>(shell.brush_hardness);
  desc.brush.pressure = 1.0f; /* 鼠标无压感：按上游满笔压 */
  desc.brush.flow = 1.0f;
  desc.brush.seed = 0; /* 三种算子都不用随机数；固定值保证同参数同结果 */

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.doc.document, id, &desc, note, sizeof(note), &note_size);
  shell.doc.needs_render = true;

  const char* state_name = comp_ui::tone_state_name(shell.tone_state);
  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"%s 失败", widen_ascii(state_name).c_str());
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"%s：已应用 %d 个折点｜%s", widen_ascii(state_name).c_str(),
              desc.stroke_point_count, widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"%s：已应用 %d 个折点（强度 %d%%）",
              widen_ascii(state_name).c_str(), desc.stroke_point_count,
              static_cast<int>(shell.tone_strength * 100.0 + 0.5));
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 画笔 / 橡皮擦 */

/*
 * 画笔与橡皮擦是同一条 COMP_TOOL_STROKE：形状与 coverage 相同，只有合成方向
 * 由 desc.erase 翻转（预乘 destination-out）。取色一律用 shell.foreground——
 * 吸管与色板都写同一个字段，因此「吸管吸到色 -> 直接画」不需要额外状态。
 */
bool paint_stroke_available(const ShellState& shell) {
  return shell.doc.document != nullptr &&
         (shell.tool == comp_ui::ToolId::Brush || shell.tool == comp_ui::ToolId::Eraser);
}

void paint_stroke_begin(HWND window, ShellState& shell, int x, int y,
                        const Layout& layout) {
  if (!paint_stroke_available(shell)) return;
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::Paint)) return;
  shell.doc.stroke_erase = shell.tool == comp_ui::ToolId::Eraser;
  SetCapture(window);
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s：拖动画布，松开%s（直径 %d px / 硬度 %d%%）",
            shell.doc.stroke_erase ? L"橡皮擦" : L"画笔",
            shell.doc.stroke_erase ? L"擦除" : L"落色",
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(shell.brush_hardness * 100.0 + 0.5));
  set_status(shell, buffer);
}

void paint_stroke_end(HWND window, ShellState& shell) {
  if (shell.doc.stroke_kind != comp_ui::StrokeKind::Paint) return;
  const bool erasing = shell.doc.stroke_erase;
  shell.doc.stroke_kind = comp_ui::StrokeKind::None;
  shell.doc.stroke_erase = false;

  /*
   * 先把折线搬出来，再 ReleaseCapture：ReleaseCapture 会同步派发
   * WM_CAPTURECHANGED，而该处理器调用 stroke_cancel 清空 stroke_points。
   * 顺序反了就是「松开鼠标后一个点都不剩」，画笔/橡皮擦永远不落色。
   */
  std::vector<float> points;
  points.swap(shell.doc.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, erasing ? L"橡皮擦：请先选择一个图层" : L"画笔：请先选择一个图层");
    return;
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_STROKE;
  desc.brush.color[0] = shell.foreground.r;
  desc.brush.color[1] = shell.foreground.g;
  desc.brush.color[2] = shell.foreground.b; /* 抹色不透明：不透明度由 flow 表达 */
  desc.brush.diameter = static_cast<float>(shell.brush_size);
  /*
   * P1-4：压感。ABI 只有整笔一个 pressure（不是逐点数组），所以取本笔
   * WM_POINTER 采样的平均值；鼠标/不支持压感的笔一个采样都没有，平均值
   * 兜底 1.0，与 P1-4 之前的像素**逐字节相同**。压感关掉时也走 1.0。
   */
  const double pressure =
      shell.brush_pressure ? comp_ui::average_stroke_pressure(shell.doc.stroke_pressure_sum,
                                                              shell.doc.stroke_pressure_samples)
                           : 1.0;
  desc.brush.pressure = static_cast<float>(pressure);
  desc.brush.hardness = static_cast<float>(shell.brush_hardness);
  desc.brush.flow = static_cast<float>(comp_ui::clamp_brush_flow(shell.brush_flow));
  desc.brush.seed = 0; /* 圆头笔是解析式的；固定值守住「同参数同结果」 */
  desc.erase = erasing ? 1 : 0;
  desc.stroke_points = points.data();
  desc.stroke_point_count = static_cast<int32_t>(points.size() / 2);

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.doc.document, id, &desc, note, sizeof(note), &note_size);
  shell.doc.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"%s失败（错误码 %d）", erasing ? L"橡皮擦" : L"画笔",
              static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  char hex[8] = {0};
  comp_ui::color_to_hex(shell.foreground, hex);
  /*
   * P1-4：只在**非默认**时追加后缀，默认路径（流量 100%、鼠标满笔压）的状态
   * 文本与 P1-4 之前逐字相同——visual_check 的状态栏读数不因此改基线。
   */
  wchar_t suffix[96] = L"";
  const int flow_percent = static_cast<int>(desc.brush.flow * 100.0 + 0.5);
  if (flow_percent < 100) {
    wsprintfW(suffix, L" · 流量 %d%%", flow_percent);
  }
  if (pressure < 0.999) {
    wchar_t pressure_text[32];
    wsprintfW(pressure_text, L" · 笔压 %d%%", static_cast<int>(pressure * 100.0 + 0.5));
    lstrcatW(suffix, pressure_text);
  }
  if (erasing) {
    wsprintfW(buffer, L"橡皮擦：已擦除 %d 个折点（直径 %d px%s）", desc.stroke_point_count,
              static_cast<int>(shell.brush_size + 0.5), suffix);
  } else if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"画笔：已落色 %d 个折点｜%s", desc.stroke_point_count,
              widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"画笔：已落色 %d 个折点（%s · 直径 %d px%s）", desc.stroke_point_count,
              widen_ascii(hex).c_str(), static_cast<int>(shell.brush_size + 0.5), suffix);
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 仿制图章 */

/*
 * 仿制图章 = 一条 COMP_TOOL_CLONE 折线 + 宿主保存的源点/偏移。
 * Alt+点击只落源点（不改像素），普通拖动才落笔——这是引擎 ABI 的两段式：
 * 引擎自己不记「上一次的源点」，对齐完全靠宿主把 offset 带回来。
 */
bool clone_stroke_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Clone;
}

/* 换文档 / 复位：源点属于旧文档的像素网格。 */
void clone_reset_source(ShellState& shell) {
  shell.doc.clone_source_set = false;
  shell.doc.clone_source_x = 0.0;
  shell.doc.clone_source_y = 0.0;
  shell.doc.clone_has_offset = false;
  shell.doc.clone_offset_x = 0.0;
  shell.doc.clone_offset_y = 0.0;
  shell.clone_sample_all = false;
}

/*
 * 取样范围开关（Shift+K）。上游把它做成选项栏里的持久勾选；本壳没有选项栏，
 * 沿用 Shift+S 切对齐的记法。这里只翻状态：真正读它的地方是 clone_stroke_end
 * 的 desc.clone.sample_all_layers，所以切完立刻对下一笔生效。
 */
void clone_toggle_sample_all(ShellState& shell) {
  shell.clone_sample_all = !shell.clone_sample_all;
  set_status(shell,
             shell.clone_sample_all
                 ? L"仿制取样：整篇合成（Shift+K 切回当前图层）"
                 : L"仿制取样：当前图层（Shift+K 切到整篇合成）");
}

/* Alt+点击：只落源点，不改像素（同时按住 Shift = 把取样范围置为整篇合成并保持）。 */
void clone_pick_source(ShellState& shell, int x, int y, const Layout& layout) {
  if (!clone_stroke_available(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  /* layout.canvas 是整个中间区域（不是文档范围）：缩放后点击可能落在文档外，
   * 那种坐标不能当源点（引擎会逐像素跳过，等于白设）。 */
  if (doc.x < 0.0 || doc.y < 0.0 || doc.x >= static_cast<double>(shell.doc.width) ||
      doc.y >= static_cast<double>(shell.doc.height)) {
    set_status(shell, L"仿制图章：源点必须在画布内");
    return;
  }
  shell.doc.clone_source_set = true;
  shell.doc.clone_source_x = std::floor(doc.x); /* 吸附到像素：偏移也随之为整数 */
  shell.doc.clone_source_y = std::floor(doc.y);
  shell.doc.clone_has_offset = false; /* 换源点 = 重算首笔偏移 */
  /*
   * 按住 Shift = 把选项置为「整篇合成」并保持（等价于按 Shift+K 切到合并）；
   * 不带 Shift 时保留选项栏的持久态，不再把它写回「当前图层」。
   */
  if (GetKeyState(VK_SHIFT) & 0x8000) shell.clone_sample_all = true;
  wchar_t buffer[224];
  wsprintfW(buffer, L"仿制图章：源点 (%d, %d)｜%s取样｜拖动开始仿制",
            static_cast<int>(shell.doc.clone_source_x), static_cast<int>(shell.doc.clone_source_y),
            shell.clone_sample_all ? L"整篇合成" : L"当前图层");
  set_status(shell, buffer);
}

void clone_stroke_begin(HWND window, ShellState& shell, int x, int y,
                        const Layout& layout) {
  if (!clone_stroke_available(shell)) return;
  if (!shell.doc.clone_source_set) {
    set_status(shell, L"仿制图章：先 Alt+点击画布设置源点（Shift+Alt = 整篇合成，Shift+K 切取样范围）");
    return;
  }
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::Clone)) return;
  SetCapture(window);
  wchar_t buffer[256];
  wsprintfW(buffer, L"仿制图章：源 (%d, %d) · %s取样，松开鼠标落笔（直径 %d px）",
            static_cast<int>(shell.doc.clone_source_x), static_cast<int>(shell.doc.clone_source_y),
            shell.clone_sample_all ? L"整篇合成" : L"当前图层",
            static_cast<int>(shell.brush_size + 0.5));
  set_status(shell, buffer);
}

void clone_stroke_end(HWND window, ShellState& shell) {
  if (shell.doc.stroke_kind != comp_ui::StrokeKind::Clone) return;
  shell.doc.stroke_kind = comp_ui::StrokeKind::None;

  /* 同 paint_stroke_end：折线必须先搬走，ReleaseCapture 会同步触发取消。 */
  std::vector<float> points;
  points.swap(shell.doc.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) {
    set_status(shell, L"仿制图章：请先选择一个图层");
    return;
  }
  if (!shell.doc.clone_source_set) {
    set_status(shell, L"仿制图章：源点已失效，请重新 Alt+点击设置");
    return;
  }

  /*
   * 对齐：首笔不带 offset（引擎按 round(source - first_point) 现算），宿主
   * 在这一笔成功后存下同一组整数；之后每一笔都带 has_offset 复用。
   * 顺序不能反——先带 offset 再存，首笔就会用上一次会话的偏移。
   * 关掉对齐（Shift+S）时不携带也不保存 offset：每笔都由引擎现算，
   * 这正是上游 aligned = false 的语义。
   */
  const bool establish_offset = shell.clone_aligned && !shell.doc.clone_has_offset;
  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_CLONE;
  desc.clone.source_x = shell.doc.clone_source_x;
  desc.clone.source_y = shell.doc.clone_source_y;
  desc.clone.has_source = 1;
  if (shell.clone_aligned && !establish_offset) {
    desc.clone.offset_x = shell.doc.clone_offset_x;
    desc.clone.offset_y = shell.doc.clone_offset_y;
    desc.clone.has_offset = 1;
  }
  desc.clone.aligned = shell.clone_aligned ? 1 : 0;
  desc.clone.sample_all_layers = shell.clone_sample_all ? 1 : 0;
  desc.clone.opacity = static_cast<float>(comp_ui::clamp_clone_opacity(shell.clone_opacity));
  desc.stroke_points = points.data();
  desc.stroke_point_count = static_cast<int32_t>(points.size() / 2);
  desc.brush.diameter = static_cast<float>(shell.brush_size);
  desc.brush.hardness = static_cast<float>(shell.brush_hardness);
  desc.brush.pressure = 1.0f; /* 鼠标无压感：按上游满笔压 */
  desc.brush.flow = 1.0f;
  desc.brush.seed = 0; /* 仿制是解析式采样；固定值守住「同参数同结果」 */

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.doc.document, id, &desc, note, sizeof(note), &note_size);
  shell.doc.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"仿制图章失败（错误码 %d）", static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  if (establish_offset) {
    comp_ui::clone_stroke_offset(shell.doc.clone_source_x, shell.doc.clone_source_y, points[0],
                                 points[1], &shell.doc.clone_offset_x, &shell.doc.clone_offset_y);
    shell.doc.clone_has_offset = true;
  }

  const wchar_t* sample_name = shell.clone_sample_all ? L"整篇合成" : L"当前图层";
  const int opacity_percent = static_cast<int>(desc.clone.opacity * 100.0 + 0.5);
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"仿制图章：已应用 %d 个折点（源 (%d, %d) · %s · %s · 不透明度 %d%%）｜%s",
              desc.stroke_point_count, static_cast<int>(shell.doc.clone_source_x),
              static_cast<int>(shell.doc.clone_source_y), sample_name,
              shell.clone_aligned ? L"对齐" : L"不对齐", opacity_percent,
              widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"仿制图章：已应用 %d 个折点（源 (%d, %d) · %s · %s · 直径 %d px · 不透明度 %d%%）",
              desc.stroke_point_count, static_cast<int>(shell.doc.clone_source_x),
              static_cast<int>(shell.doc.clone_source_y), sample_name,
              shell.clone_aligned ? L"对齐" : L"不对齐",
              static_cast<int>(shell.brush_size + 0.5), opacity_percent);
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ Spot Healing（修补） */

/*
 * 修补工具是本仓唯一「壳侧光栅化 coverage」的落笔族：ABI 的
 * `comp_tool_heal.coverage` 要的是**图层像素空间**的整层 8 位蒙版，而引擎不提供
 * 折线参数（不像画笔 / 仿制那样在域层自己 stamp）。壳因此做三件事：
 *   1. 沿用 stroke_points（文档像素折线）；
 *   2. 松手时把折线按图层变换逆映射到图层像素空间，按笔刷直径/硬度盖圆点；
 *   3. 调一次 comp_tool_apply = 恰好一步撤销。
 */
bool heal_stroke_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Heal;
}

/*
 * 文档坐标 -> 图层像素坐标。与 engine/domain/layer.cpp 的 map_document_to_pixel
 * 同一套「归一化 -> 逆旋转 -> 翻转 -> 像素」，输出尺度只能取画布尺寸：壳自己
 * 新建的图层像素尺寸就是画布；导入层的像素尺寸 ABI 没有读口，同样按画布尺度
 * 解释（层更宽/更高时由 stride / 行数闸在写像素前拒绝，见 §5.25）。
 */
bool heal_layer_point(const comp_rect& transform, float rotation, bool flip_x, bool flip_y,
                      int width, int height, double doc_x, double doc_y, double* out_x,
                      double* out_y) {
  if (out_x == nullptr || out_y == nullptr || width <= 0 || height <= 0) return false;
  const comp_rect bounds = transform.w > 0 && transform.h > 0
                               ? transform
                               : comp_rect{transform.x, transform.y, width, height};
  if (bounds.w <= 0 || bounds.h <= 0) return false;

  const double ud = (doc_x + 0.5 - static_cast<double>(bounds.x)) /
                        static_cast<double>(bounds.w) * 2.0 - 1.0;
  const double vd = (doc_y + 0.5 - static_cast<double>(bounds.y)) /
                        static_cast<double>(bounds.h) * 2.0 - 1.0;

  double us = ud;
  double vs = vd;
  if (std::fabs(static_cast<double>(rotation)) > 1e-9) {
    constexpr double kPi = 3.14159265358979323846;
    const double rad = static_cast<double>(rotation) * kPi / 180.0;
    const double cos_t = std::cos(rad);
    const double sin_t = std::sin(rad);
    us = ud * cos_t + vd * sin_t;
    vs = -ud * sin_t + vd * cos_t;
  }
  if (flip_x) us = -us;
  if (flip_y) vs = -vs;

  *out_x = (us + 1.0) * 0.5 * static_cast<double>(width) - 0.5;
  *out_y = (vs + 1.0) * 0.5 * static_cast<double>(height) - 0.5;
  return true;
}

/*
 * 一个圆形笔尖。取 max 而不是累加：折线相邻 stamp 重叠是必然的，累加会让
 * 慢速拖动把同一块涂到 255 以外的「更满」，等于 secretly 依赖鼠标采样率。
 */
void heal_stamp_round(std::vector<uint8_t>* coverage, int width, int height, double cx,
                      double cy, double radius, double hardness) {
  if (coverage == nullptr || width <= 0 || height <= 0 || radius <= 0.0) return;
  const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
  const int x1 = std::min(width - 1, static_cast<int>(std::ceil(cx + radius)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
  const int y1 = std::min(height - 1, static_cast<int>(std::ceil(cy + radius)));
  if (x0 > x1 || y0 > y1) return;

  const double h = std::min(std::max(hardness, 0.0), 1.0);
  const double inner = radius * h; /* 硬芯半径：从这里到外沿线性降到 0 */
  const double falloff = radius - inner;
  for (int y = y0; y <= y1; ++y) {
    uint8_t* row = coverage->data() + static_cast<size_t>(y) * static_cast<size_t>(width);
    const double dy = static_cast<double>(y) - cy;
    for (int x = x0; x <= x1; ++x) {
      const double dx = static_cast<double>(x) - cx;
      const double dist = std::sqrt(dx * dx + dy * dy);
      if (dist > radius) continue;
      double amount = 1.0;
      if (falloff > 1e-9 && dist > inner) amount = (radius - dist) / falloff;
      const int value = static_cast<int>(amount * 255.0 + 0.5);
      uint8_t& cell = row[x];
      if (value > cell) cell = static_cast<uint8_t>(std::min(255, value));
    }
  }
}

/*
 * 沿折线 stamp：先在每个端点盖一记，再按 <= 半径 1/4 的步长补中间点，避免
 * 快速拖动时圆点之间留缝（与域层 stamp_polyline 的采样密度同量级）。
 */
void heal_rasterize_stroke(std::vector<uint8_t>* coverage, int width, int height,
                           const std::vector<double>& points, double radius, double hardness) {
  if (coverage == nullptr || points.size() < 2) return;
  heal_stamp_round(coverage, width, height, points[0], points[1], radius, hardness);
  for (size_t i = 2; i + 1 < points.size(); i += 2) {
    const double ax = points[i - 2];
    const double ay = points[i - 1];
    const double bx = points[i];
    const double by = points[i + 1];
    const double dx = bx - ax;
    const double dy = by - ay;
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double spacing = std::max(0.75, radius * 0.25);
    int steps = static_cast<int>(std::ceil(dist / spacing));
    if (steps < 1) steps = 1;
    if (steps > 8192) steps = 8192; /* 折点本身已按 1 px 采样；上限只防病态输入 */
    for (int s = 1; s <= steps; ++s) {
      const double t = static_cast<double>(s) / static_cast<double>(steps);
      heal_stamp_round(coverage, width, height, ax + dx * t, ay + dy * t, radius, hardness);
    }
  }
}

void heal_stroke_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!heal_stroke_available(shell)) return;
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::Heal)) return;
  SetCapture(window);
  wchar_t buffer[256];
  wsprintfW(buffer,
            L"修补：拖过要修补的区域，松开应用（%s · 直径 %d px · 不透明度 %d%%；"
            L"再按 J 切算法，Alt+[ / ] 调不透明度；有选区时只修补选区内）",
            widen_ascii(comp_ui::heal_mode_name(shell.heal_mode)).c_str(),
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(comp_ui::clamp_heal_opacity(shell.heal_opacity) * 100.0 + 0.5));
  set_status(shell, buffer);
}

void heal_stroke_end(HWND window, ShellState& shell) {
  if (shell.doc.stroke_kind != comp_ui::StrokeKind::Heal) return;
  shell.doc.stroke_kind = comp_ui::StrokeKind::None;

  /* 同 paint_stroke_end：折线必须先搬走，ReleaseCapture 会同步触发取消。 */
  std::vector<float> points;
  points.swap(shell.doc.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  if (shell.doc.document == nullptr) {
    set_status(shell, L"修补：没有文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  const LayerRow* row = selected_row(shell);
  if (id == 0) {
    set_status(shell, L"修补：请先选择一个图层");
    return;
  }
  if (row != nullptr && row->is_group != 0) {
    set_status(shell, L"修补：组图层没有像素，无法修补（先选中组里的成员图层）");
    return;
  }

  comp_rect transform{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.doc.document, id, &transform, &rotation) != COMP_OK) {
    set_status(shell, L"修补：读取图层变换失败");
    return;
  }
  int32_t flip_x = 0;
  int32_t flip_y = 0;
  if (comp_layer_get_flip(shell.doc.document, id, &flip_x, &flip_y) != COMP_OK) {
    flip_x = 0;
    flip_y = 0;
  }

  const int width = shell.doc.width;
  const int height = shell.doc.height;
  if (width <= 0 || height <= 0) {
    set_status(shell, L"修补：画布尺寸无效");
    return;
  }

  /* 折线映射到图层像素空间。映射失败（几何不可用）时整笔放弃，不写半张蒙版。 */
  std::vector<double> layer_points;
  layer_points.reserve(points.size());
  for (size_t i = 0; i + 1 < points.size(); i += 2) {
    double lx = 0.0;
    double ly = 0.0;
    if (!heal_layer_point(transform, rotation, flip_x != 0, flip_y != 0, width, height,
                          points[i], points[i + 1], &lx, &ly)) {
      set_status(shell, L"修补：图层几何不可用，未应用");
      return;
    }
    layer_points.push_back(lx);
    layer_points.push_back(ly);
  }
  if (layer_points.empty()) return;

  /* 文档像素 -> 图层像素的尺度。transform 为 0 时两轴都退化成 1（见 heal_layer_point）。 */
  const comp_rect bounds = transform.w > 0 && transform.h > 0
                               ? transform
                               : comp_rect{transform.x, transform.y, width, height};
  const double scale_x = static_cast<double>(width) / static_cast<double>(std::max(1, bounds.w));
  const double scale_y = static_cast<double>(height) / static_cast<double>(std::max(1, bounds.h));
  const double radius =
      std::max(0.5, shell.brush_size * 0.5 * (scale_x + scale_y) * 0.5);

  std::vector<uint8_t> coverage(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  heal_rasterize_stroke(&coverage, width, height, layer_points, radius, shell.brush_hardness);

  /*
   * 选区内修补：coverage 是壳侧自己光栅化的，引擎的选区闸只作用在它自己的
   * 落笔路径上，不会替我们裁这张蒙版。蒙版按文档分辨率维护，与 coverage 同一
   * 坐标系；尺寸对不上（理论上不该发生）就老实报出来，不假装裁过。
   */
  bool selection_clipped = false;
  bool selection_mask_mismatch = false;
  if (shell.doc.selection_active && !shell.doc.selection_mask.empty()) {
    if (shell.doc.selection_mask.size() == coverage.size()) {
      selection_clipped = comp_ui::intersect_coverage_with_selection(
          coverage.data(), shell.doc.selection_mask.data(), coverage.size());
    } else {
      selection_mask_mismatch = true;
    }
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_HEAL;
  desc.heal.opacity = static_cast<float>(comp_ui::clamp_heal_opacity(shell.heal_opacity));
  desc.heal.mode = shell.heal_mode;
  desc.heal.seed = 1u; /* 固定 seed：同参数同结果，PatchMatch 才不会每笔都换答案 */
  desc.heal_coverage = coverage.data();
  desc.heal_coverage_stride = static_cast<size_t>(width);
  desc.heal_coverage_rows = static_cast<size_t>(height);

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.doc.document, id, &desc, note, sizeof(note), &note_size);
  shell.doc.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    if (result == COMP_ERR_INVALID_ARGUMENT) {
      /* 行数/stride 闸：导入层的像素尺寸可以不是画布尺寸。 */
      wsprintfW(buffer, L"修补失败：图层像素尺寸大于画布，当前 coverage 装不下（错误码 %d）",
                static_cast<int>(result));
    } else if (result == COMP_ERR_STATE) {
      wsprintfW(buffer, L"修补失败：图层没有可修补的像素（错误码 %d）", static_cast<int>(result));
    } else {
      wsprintfW(buffer, L"修补失败（错误码 %d）", static_cast<int>(result));
    }
    set_status(shell, buffer);
    return;
  }

  const int stroke_count = static_cast<int>(layer_points.size() / 2);
  const wchar_t* selection_note =
      selection_clipped ? L" · 按选区裁剪"
                        : (selection_mask_mismatch ? L" · 选区未裁剪（蒙版尺寸不一致）" : L"");
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"修补工具：已应用 %d 个折点（%s · 直径 %d px）%s｜%s", stroke_count,
              widen_ascii(comp_ui::heal_mode_name(shell.heal_mode)).c_str(),
              static_cast<int>(radius * 2.0 + 0.5), selection_note, widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"修补工具：已应用 %d 个折点（%s · 直径 %d px · 不透明度 %d%%）%s",
              stroke_count, widen_ascii(comp_ui::heal_mode_name(shell.heal_mode)).c_str(),
              static_cast<int>(radius * 2.0 + 0.5),
              static_cast<int>(desc.heal.opacity * 100.0 + 0.5), selection_note);
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 吸管（只读取色） */

/*
 * 取样结果写进 shell.foreground（0..1 直通 sRGB -> 0..255）。
 * 吸管是只读工具：不进撤销栈、不改选区、不改图层。
 */
bool eyedropper_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Eyedropper;
}

void eyedropper_sample(ShellState& shell, int x, int y, const Layout& layout) {
  if (!eyedropper_available(shell)) return;
  if (!layout.canvas.contains(x, y)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  /* 画布外不取样：ABI 对越界坐标返回 INVALID_ARGUMENT，那不是「吸管坏了」。 */
  if (doc.x < 0.0 || doc.y < 0.0 || doc.x >= static_cast<double>(shell.doc.width) ||
      doc.y >= static_cast<double>(shell.doc.height)) {
    return;
  }

  const bool merged = (GetKeyState(VK_MENU) & 0x8000) != 0;
  comp_layer_id source_layer = merged ? 0 : selected_layer_id(shell);
  comp_eyedropper_source source = COMP_EYEDROPPER_LAYER;
  if (merged || source_layer == 0) {
    /* 没有选中图层时取整篇合成：画布上看到什么就吸什么，比报错更符合预期。 */
    source = COMP_EYEDROPPER_MERGED;
    source_layer = 0;
  }

  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const comp_result result = comp_eyedropper_sample(
      shell.doc.document, source_layer, doc.x, doc.y, 1, source, rgba);
  wchar_t buffer[256];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"吸管：取样失败（%s，错误码 %d）",
              source == COMP_EYEDROPPER_MERGED ? L"合并源" : L"当前图层",
              static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }

  shell.foreground = comp_ui::color_from_unit(rgba[0], rgba[1], rgba[2]);
  char hex[8] = {0};
  comp_ui::color_to_hex(shell.foreground, hex);
  const comp_ui::ColorHSV hsv = comp_ui::rgb_to_hsv(shell.foreground);
  wsprintfW(buffer, L"吸管：%s · %s · H %d° S %d%% B %d%%（不透明度 %d%%）",
            widen_ascii(hex).c_str(),
            source == COMP_EYEDROPPER_MERGED ? L"合并源" : L"当前图层",
            static_cast<int>(hsv.h + 0.5), static_cast<int>(hsv.s * 100.0 + 0.5),
            static_cast<int>(hsv.v * 100.0 + 0.5),
            static_cast<int>(rgba[3] * 100.0f + 0.5f));
  set_status(shell, buffer);
}

void eyedropper_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!eyedropper_available(shell)) return;
  shell.doc.eyedropper_drag = true;
  SetCapture(window);
  eyedropper_sample(shell, x, y, layout);
}

void eyedropper_cancel(ShellState& shell) { shell.doc.eyedropper_drag = false; }

/* ------------------------------------------------------------ 选区搬移（Move） */

/*
 * 一次拖拽 = 一步撤销：像素搬移与轮廓平移写进同一个显式事务。
 * 两个 ABI 调用内部的 ImplicitTxn 会「加入」已开启的显式事务，
 * 因此这里不需要把两步合成一个新的算子。
 */
bool move_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Move;
}

/*
 * 整层拖动（Move 工具、无选区；§6.63 起「有选区但落点在选区外」也走这里）。
 * 与选区搬移分开实现：
 *   - 选区搬移改的是**像素**（comp_selection_move_pixels），只动当前图层选区内的字节；
 *   - 整层拖动改的是**transform 的 origin**（comp_layer_set_transform），像素不动。
 * 上游 V 在没有选区时拖的是整层；有选区时按落点分流（选区内 = 搬像素、选区外 = 整层），
 * 这里对齐该语义。整层拖动**不动选区蒙版**——蚂蚁线留在原地，与上游一致。
 *
 * 拖动期间实时写 transform（所见即所得），与调整滑杆同一套事务写法：
 * 按下开显式事务，途中每个整数像素变化写一次（ImplicitTxn 加入该事务），
 * 松手 commit = 恰好一步撤销；失焦 / 捕获丢失 / Esc abort 回滚到按下前。
 */
/*
 * §6.61：把 root 子树里的成员（不含 root 自身）连同按下基准快照进 out。
 * 读不到 transform 的成员跳过——宁可少写一层，也不要把全 0 的基准写回去（那会把
 * 图层弹到左上角）。成员关系由 ui_core::layer_in_subtree 判定（沿 parent 链）。
 */
void collect_group_members(const ShellState& shell, comp_layer_id root,
                           std::vector<GroupMemberMove>* out) {
  out->clear();
  if (shell.doc.document == nullptr || shell.doc.layers.empty()) return;
  std::vector<comp_ui::LayerStackEntry> stack;
  stack.reserve(shell.doc.layers.size());
  for (const LayerRow& item : shell.doc.layers) {
    stack.push_back(comp_ui::LayerStackEntry{static_cast<int64_t>(item.id),
                                             static_cast<int64_t>(item.parent),
                                             static_cast<int64_t>(item.clipping)});
  }
  const int32_t count = static_cast<int32_t>(stack.size());
  for (const LayerRow& item : shell.doc.layers) {
    if (!comp_ui::layer_in_subtree(stack.data(), count, static_cast<int64_t>(item.id),
                                   static_cast<int64_t>(root))) {
      continue;
    }
    GroupMemberMove member{};
    member.id = item.id;
    if (comp_layer_get_transform(shell.doc.document, member.id, &member.base, &member.rotation) !=
        COMP_OK) {
      continue;
    }
    out->push_back(member);
  }
}

/*
 * §6.64 多选批量移动的目标快照：面板选中集合（已去掉「被选中祖先覆盖」的子层）×
 * 每项实际要写 transform 的成员——组贡献子树成员（组自身不写，§6.61），普通层
 * 贡献它自己。按下时快照一次，拖动/方向键只写「基准 + 总位移」，不累加。
 */
void build_move_targets(const ShellState& shell,
                        std::vector<GroupMemberMove>* out) {
  out->clear();
  const std::vector<comp_layer_id> ids = panel_selection_ids(shell, true);
  for (comp_layer_id id : ids) {
    bool is_group = false;
    for (const LayerRow& item : shell.doc.layers) {
      if (item.id != id) continue;
      is_group = item.is_group != 0;
      break;
    }
    if (is_group) {
      collect_group_members(shell, id, out);
      continue;
    }
    GroupMemberMove member{};
    member.id = id;
    if (comp_layer_get_transform(shell.doc.document, member.id, &member.base, &member.rotation) !=
        COMP_OK) {
      continue;
    }
    out->push_back(member);
  }
}

/*
 * 组整体是否已完全离开画布：**所有**成员都离开才算（与单层同一保守口径——少报
 * 「拖出画布」只是少一句提示，误报会让人以为图层丢了）。
 * 直接收成员快照而不是读 ShellState：方向键微调用的是局部清单，不经过拖动状态。
 */
bool members_off_canvas(const std::vector<GroupMemberMove>& members, int32_t dx,
                        int32_t dy, int32_t view_width, int32_t view_height) {
  if (members.empty()) return false;
  for (const GroupMemberMove& member : members) {
    if (comp_ui::layer_rect_touches_canvas(
            member.base.x + dx, member.base.y + dy, member.base.w, member.base.h,
            static_cast<double>(member.rotation), view_width, view_height)) {
      return false;
    }
  }
  return true;
}

/* 拖动路径的薄包装：读的是本次拖动的成员快照。 */
bool group_members_off_canvas(const ShellState& shell, int32_t dx, int32_t dy) {
  return members_off_canvas(shell.doc.layer_move_members, dx, dy, shell.doc.width, shell.doc.height);
}

bool same_rect(const comp_rect& a, const comp_rect& b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/*
 * 什么时候画变换框。四条硬条件缺一不可：
 *   1. Move 工具、且没有别的模态（裁剪/框选/套索）；
 *   2. 面板**单选**一个普通图层（多选的批量语义是移动，不提供整批缩放）；
 *   3. 没有像素选区（有选区时落点分流已经在 §5.36 定好，手柄不该抢命中）；
 *   4. 该层有显式变换（w/h > 0）。ABI 没有像素尺寸回读口，按像素尺寸的图层
 *      壳算不出真实边界，画一个猜的框比不画更糟——与 Alt+[ ] 拒绝同一理由。
 */
bool transform_frame_for(const ShellState& shell, comp_rect* out_rect, float* out_rotation) {
  if (shell.doc.document == nullptr) return false;
  if (shell.tool != comp_ui::ToolId::Move) return false;
  if (shell.doc.crop_active || shell.doc.marquee_active || shell.doc.lasso_active) return false;
  if (shell.doc.selection_active) return false;
  if (!shell.doc.extra_selection.empty()) return false;
  const LayerRow* row = selected_row(shell);
  if (row == nullptr || row->is_group != 0) return false;
  comp_rect transform{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.doc.document, row->id, &transform, &rotation) != COMP_OK) {
    return false;
  }
  if (transform.w <= 0 || transform.h <= 0) return false;
  if (out_rect != nullptr) *out_rect = transform;
  if (out_rotation != nullptr) *out_rotation = rotation;
  return true;
}

void layer_handle_clear(ShellState& shell) {
  shell.doc.layer_handle_active = false;
  shell.doc.layer_handle_kind = comp_ui::kTransformHandleNone;
  shell.doc.layer_handle_id = 0;
  shell.doc.layer_handle_base = comp_rect{};
  shell.doc.layer_handle_base_rotation = 0.0f;
  shell.doc.layer_handle_press = comp_ui::Point{};
  shell.doc.layer_handle_txn = nullptr;
  shell.doc.layer_handle_doc = nullptr;
  shell.doc.layer_handle_dirty = false;
  shell.doc.layer_handle_current = comp_rect{};
  shell.doc.layer_handle_current_rotation = 0.0f;
}

/*
 * 手柄按下：命中就吃掉这次点击（返回 true），不再落到整层拖动。
 * 事务名区分缩放/旋转，历史面板里两步操作不该长得一样。一次拖动一个事务，
 * 途中每帧写入都是 ImplicitTxn 加入，松手 commit = 一步撤销。
 */
bool layer_handle_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  comp_rect rect{};
  float rotation = 0.0f;
  if (!transform_frame_for(shell, &rect, &rotation)) return false;
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) return false;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  const double zoom = shell.doc.view.zoom > 0.0 ? shell.doc.view.zoom : 1.0;
  const double radius = kTransformHandleHitPx / zoom;
  const comp_ui::Rect frame = to_ui_rect(rect);
  int32_t kind = comp_ui::kTransformHandleNone;
  if (comp_ui::transform_knob_at(frame, rotation, doc, kTransformKnobOffsetPx / zoom, radius)) {
    kind = comp_ui::kTransformHandleRotate;
  } else {
    kind = comp_ui::transform_handle_at(frame, rotation, doc, radius);
  }
  if (kind == comp_ui::kTransformHandleNone) return false;

  const bool rotating = kind == comp_ui::kTransformHandleRotate;
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, rotating ? "Rotate Layer" : "Scale Layer", &txn) !=
      COMP_OK) {
    set_status(shell, L"变换：无法开始事务");
    return true; /* 已判定按在手柄上：不要退化成整层拖动 */
  }
  shell.doc.layer_handle_active = true;
  shell.doc.layer_handle_kind = kind;
  shell.doc.layer_handle_id = row->id;
  shell.doc.layer_handle_base = rect;
  shell.doc.layer_handle_base_rotation = rotation;
  shell.doc.layer_handle_press = doc;
  shell.doc.layer_handle_txn = txn;
  shell.doc.layer_handle_doc = shell.doc.document;
  shell.doc.layer_handle_dirty = false;
  shell.doc.layer_handle_current = rect;
  shell.doc.layer_handle_current_rotation = rotation;
  SetCapture(window);

  wchar_t buffer[224];
  if (rotating) {
    wsprintfW(buffer, L"旋转图层：拖动旋钮，Shift = %d° 吸附（松开算一步撤销），当前 %d°",
              comp_ui::kLayerTransformRotateStep, static_cast<int>(std::lround(rotation)));
  } else {
    wsprintfW(buffer,
              L"缩放图层：拖动手柄改变尺寸，Shift = 拖角等比（松开算一步撤销），当前 %d×%d",
              rect.w, rect.h);
  }
  set_status(shell, buffer);
  return true;
}

void layer_handle_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.layer_handle_active) return;
  /* 换文档后旧事务随旧文档失效：宁可不动，也不要写进新文档。 */
  if (shell.doc.document == nullptr || shell.doc.document != shell.doc.layer_handle_doc) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  const comp_ui::Rect base = to_ui_rect(shell.doc.layer_handle_base);

  comp_rect target = shell.doc.layer_handle_current;
  float rotation = shell.doc.layer_handle_current_rotation;
  bool uniform_scale = false;
  if (shell.doc.layer_handle_kind == comp_ui::kTransformHandleRotate) {
    const double delta = comp_ui::transform_rotate_delta(base, shell.doc.layer_handle_press, doc);
    double next = static_cast<double>(shell.doc.layer_handle_base_rotation) + delta;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
      const double step = static_cast<double>(comp_ui::kLayerTransformRotateStep);
      next = std::round(next / step) * step;
    }
    next = comp_ui::transform_normalize_rotation(next);
    if (std::fabs(next - static_cast<double>(shell.doc.layer_handle_current_rotation)) < 1e-4) {
      return;
    }
    rotation = static_cast<float>(next);
    target = shell.doc.layer_handle_base; /* 旋转只改角度，矩形不动 */
  } else {
    /* S43：Shift = 拖角等比缩放（比例锁在按下时的 base 上，见 transform_scale_rect）。 */
    uniform_scale = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const comp_ui::Rect scaled =
        comp_ui::transform_scale_rect(base, shell.doc.layer_handle_base_rotation,
                                      shell.doc.layer_handle_kind, doc,
                                      comp_ui::kTransformHandleMinSize, uniform_scale);
    target = to_abi_rect(scaled);
    if (same_rect(target, shell.doc.layer_handle_current)) return;
    rotation = shell.doc.layer_handle_base_rotation;
  }
  if (comp_layer_set_transform(shell.doc.document, shell.doc.layer_handle_id, target, rotation) !=
      COMP_OK) {
    set_status(shell, L"变换：写入图层变换失败");
    return;
  }
  shell.doc.layer_handle_dirty = true;
  shell.doc.layer_handle_current = target;
  shell.doc.layer_handle_current_rotation = rotation;
  shell.doc.needs_render = true;

  wchar_t buffer[192];
  if (shell.doc.layer_handle_kind == comp_ui::kTransformHandleRotate) {
    wsprintfW(buffer, L"旋转图层：%d°", static_cast<int>(std::lround(rotation)));
  } else {
    wsprintfW(buffer, L"缩放图层：%d×%d（位置 (%d, %d)）%s", target.w, target.h, target.x,
              target.y, uniform_scale ? L" 等比" : L"");
  }
  set_status(shell, buffer);
}

void layer_handle_end(HWND window, ShellState& shell) {
  if (!shell.doc.layer_handle_active) return;
  /* 先清状态再 ReleaseCapture：捕获变更会同步重入 move_cancel。 */
  const comp_txn txn = shell.doc.layer_handle_txn;
  const bool dirty = shell.doc.layer_handle_dirty;
  const bool rotating = shell.doc.layer_handle_kind == comp_ui::kTransformHandleRotate;
  const comp_rect current = shell.doc.layer_handle_current;
  const float rotation = shell.doc.layer_handle_current_rotation;
  layer_handle_clear(shell);
  ReleaseCapture();

  if (txn == nullptr) return;
  if (!dirty) {
    comp_txn_abort(txn);
    set_status(shell, rotating ? L"旋转：角度未变化" : L"缩放：尺寸未变化");
    return;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"变换：提交失败");
    return;
  }
  shell.doc.needs_render = true;
  wchar_t buffer[192];
  if (rotating) {
    wsprintfW(buffer, L"已旋转图层：%d°（一步撤销）", static_cast<int>(std::lround(rotation)));
  } else {
    wsprintfW(buffer, L"已缩放图层：%d×%d，位置 (%d, %d)（一步撤销）", current.w, current.h,
              current.x, current.y);
  }
  refresh_history_status(shell, buffer);
}

void layer_move_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout,
                      const wchar_t* hint = nullptr) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"移动：请先选择一个图层");
    return;
  }
  /*
   * §6.61 整组：组自己的 transform 不参与合成（filter_graph 只烘焙成员），写进去
   * 等于静默无操作。移动组 = 平移子树里每个成员，成员清单在按下时快照一次。
   * 空组照样允许按下：拖动是空操作，与「选中空组按住拖」在 PS 里的表现一致。
   *
   * §6.64 多选：选中集合里不止一个顶层图层时，整批一起搬（成员快照把所有选中项
   * 展开成实际要写的 transform），文案与事务名都换成「多个图层」。
   */
  const std::vector<comp_layer_id> batch_ids = panel_selection_ids(shell, true);
  const bool multi = batch_ids.size() > 1;
  std::vector<GroupMemberMove> batch_members;
  if (multi) build_move_targets(shell, &batch_members);
  std::vector<GroupMemberMove> group_members;
  const bool is_group = !multi && row->is_group != 0;
  if (is_group) collect_group_members(shell, row->id, &group_members);
  if (multi && batch_members.empty()) {
    /* 全部选中项都是空组：没有可写的成员，别开一个空事务让撤销栈多一步。 */
    set_status(shell, L"移动多个图层：选中的图层没有可移动的成员，无位移");
    return;
  }
  comp_rect transform{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.doc.document, row->id, &transform, &rotation) != COMP_OK) {
    set_status(shell, L"移动：读取图层变换失败");
    return;
  }
  /*
   * Alt 在按下时取样一次：途中松开 Alt 不该改变这一拖的语义（与画笔 Shift 同规矩）。
   * 事务名也据此区分——历史面板里「复制并移动」与「移动」不该长得一样。
   */
  const bool alt_copy = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const char* txn_name = "Move Layer";
  if (multi) txn_name = alt_copy ? "Duplicate & Move Layers" : "Move Layers";
  else if (is_group) txn_name = alt_copy ? "Duplicate & Move Layer Group" : "Move Layer Group";
  else if (alt_copy) txn_name = "Duplicate & Move Layer";
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, txn_name, &txn) != COMP_OK) {
    set_status(shell, L"移动：无法开始事务");
    return;
  }
  shell.doc.layer_move_active = true;
  shell.doc.layer_move_dirty = false;
  shell.doc.layer_move_doc = shell.doc.document;
  shell.doc.layer_move_txn = txn;
  shell.doc.layer_move_id = row->id;
  shell.doc.layer_move_base = transform;
  shell.doc.layer_move_rotation = rotation;
  shell.doc.layer_move_multi = multi;
  shell.doc.layer_move_ids = batch_ids;
  shell.doc.layer_move_members = multi ? std::move(batch_members) : std::move(group_members);
  shell.doc.layer_move_is_group = is_group;
  shell.doc.layer_move_start =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.layer_move_applied_dx = 0;
  shell.doc.layer_move_applied_dy = 0;
  /* Alt 语义见上：只记标志，复制推迟到首次位移。 */
  shell.doc.layer_move_alt = alt_copy;
  shell.doc.layer_move_duplicated = false;
  shell.doc.layer_move_copy_failed = false;
  SetCapture(window);

  wchar_t buffer[256];
  const size_t member_count = shell.doc.layer_move_members.size();
  if (multi) {
    if (shell.doc.layer_move_alt) {
      wsprintfW(buffer,
                L"复制并移动多个图层：拖动即复制整批再移动（%u 个成员，松开算一步撤销）",
                static_cast<unsigned>(member_count));
    } else {
      wsprintfW(buffer, L"移动多个图层：拖动移动 %u 个成员（松开算一步撤销）",
                static_cast<unsigned>(member_count));
    }
  } else if (is_group) {
    if (shell.doc.layer_move_alt) {
      wsprintfW(buffer, L"复制并移动图层组：拖动即复制整组再移动（%u 个成员，松开算一步撤销）",
                static_cast<unsigned>(member_count));
    } else {
      wsprintfW(buffer, L"移动图层组：拖动移动组内 %u 个成员（松开算一步撤销）",
                static_cast<unsigned>(member_count));
    }
  } else if (shell.doc.layer_move_alt) {
    wsprintfW(buffer, L"复制并移动图层：拖动即复制一份再移动（松开算一步撤销），当前 (%d, %d)",
              transform.x, transform.y);
  } else {
    wsprintfW(buffer, L"移动图层：拖动改变位置（松开算一步撤销），当前 (%d, %d)", transform.x,
              transform.y);
  }
  if (hint != nullptr) {
    /* 补一句「为什么这一拖动的是整层」：落点在选区外，选区自己不会跟着走。 */
    wsprintfW(buffer + lstrlenW(buffer), L" · %s", hint);
  }
  set_status(shell, buffer);
}

void layer_move_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.layer_move_active) return;
  /* 换文档后旧事务随旧文档失效：宁可不动，也不要写进新文档。 */
  if (shell.doc.document == nullptr || shell.doc.document != shell.doc.layer_move_doc) return;

  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  const int32_t dx = static_cast<int32_t>(std::lround(doc.x - shell.doc.layer_move_start.x));
  const int32_t dy = static_cast<int32_t>(std::lround(doc.y - shell.doc.layer_move_start.y));
  if (dx == shell.doc.layer_move_applied_dx && dy == shell.doc.layer_move_applied_dy) return;

  /*
   * §6.64 多选 Alt 拖动：逐个复制选中项（组会深拷贝整棵子树），再把新的副本集合
   * 当作选中项、重建成员快照——副本的成员 id 全变了，沿用旧快照会去写原图层。
   * 任何一个复制失败：本次拖动作废（不写 transform），松手 abort 连部分副本一起回滚。
   */
  if (shell.doc.layer_move_multi && shell.doc.layer_move_alt && !shell.doc.layer_move_duplicated) {
    std::vector<comp_layer_id> copies;
    bool ok = true;
    for (comp_layer_id id : shell.doc.layer_move_ids) {
      comp_layer_id copy_id = 0;
      if (comp_layer_duplicate(shell.doc.document, id, &copy_id) != COMP_OK || copy_id == 0) {
        ok = false;
        break;
      }
      copies.push_back(copy_id);
    }
    if (!ok || copies.empty()) {
      shell.doc.layer_move_alt = false;
      shell.doc.layer_move_copy_failed = true;
      return;
    }
    shell.doc.layer_move_duplicated = true;
    shell.doc.layer_move_ids = copies;
    shell.doc.extra_selection.assign(copies.begin() + 1, copies.end());
    refresh_layers(shell);
    panel_select_layer_id(shell, copies.front());
    build_move_targets(shell, &shell.doc.layer_move_members);
  }

  /*
   * Alt 拖动：第一次真正产生位移时才复制。副本落在同一个显式事务里，
   * `comp_layer_duplicate` 内部的 ImplicitTxn 会加入该事务，所以松手 commit 后
   * **一步撤销同时撤掉「多出来的图层」和「这一次位移」**（与 Ctrl+J 复制不同步）。
   * 复制失败就退回移动原图层并在状态栏说明，而不是假装复制过。
   */
  if (shell.doc.layer_move_alt && !shell.doc.layer_move_duplicated) {
    comp_layer_id copy_id = 0;
    if (comp_layer_duplicate(shell.doc.document, shell.doc.layer_move_id, &copy_id) == COMP_OK &&
        copy_id != 0) {
      shell.doc.layer_move_id = copy_id;
      shell.doc.layer_move_duplicated = true;
      /* 以副本自己的 transform 作为基准：副本若被引擎偏移过，拖动起点不会跳。 */
      comp_rect copy_transform{};
      float copy_rotation = 0.0f;
      if (comp_layer_get_transform(shell.doc.document, copy_id, &copy_transform, &copy_rotation) ==
          COMP_OK) {
        shell.doc.layer_move_base = copy_transform;
        shell.doc.layer_move_rotation = copy_rotation;
      }
      /* 面板跟到副本：接着拖、接着按方向键都是对副本生效，与原图层无关。 */
      shell.doc.extra_selection.clear();
      refresh_layers(shell);
      panel_select_layer_id(shell, copy_id);
      /*
       * §6.61 整组复制：副本是深拷贝，成员 id 全新，按下时的成员清单已全部失效。
       * 这里按副本重新快照一次——否则接着拖会去写**原组**的成员，画面上原组跟着动、
       * 副本不动，看起来像「复制了但没复制」。
       */
      shell.doc.layer_move_members.clear();
      for (const LayerRow& item : shell.doc.layers) {
        if (item.id == copy_id && item.is_group != 0) {
          collect_group_members(shell, copy_id, &shell.doc.layer_move_members);
          break;
        }
      }
    } else {
      shell.doc.layer_move_alt = false;
    }
  }

  /*
   * §6.61 整组 / §6.64 多选：逐成员写「按下基准 + 总位移」。成员为空时什么都不写：
   * 组自己的 transform 不参与合成，写进去只会让状态栏的数字动、画面不动。
   */
  if (shell.doc.layer_move_is_group || shell.doc.layer_move_multi) {
    if (shell.doc.layer_move_members.empty()) {
      set_status(shell, shell.doc.layer_move_multi ? L"移动多个图层：没有可移动的成员，无位移"
                                                : L"移动图层组：组内没有成员，无位移");
      return;
    }
    size_t moved = 0;
    for (const GroupMemberMove& member : shell.doc.layer_move_members) {
      comp_rect target = member.base;
      target.x += dx;
      target.y += dy;
      if (comp_layer_set_transform(shell.doc.document, member.id, target, member.rotation) ==
          COMP_OK) {
        ++moved;
      }
    }
    if (moved == 0) return;
    shell.doc.layer_move_applied_dx = dx;
    shell.doc.layer_move_applied_dy = dy;
    shell.doc.layer_move_dirty = true;
    shell.doc.needs_render = true;

    const wchar_t* off_canvas = group_members_off_canvas(shell, dx, dy) ? L" · 已拖出画布" : L"";
    wchar_t group_buffer[224];
    if (shell.doc.layer_move_multi) {
      if (shell.doc.layer_move_duplicated) {
        wsprintfW(group_buffer,
                  L"复制并移动多个图层：位移 (%d, %d)，%u 个成员（松开算一步撤销）%s", dx, dy,
                  static_cast<unsigned>(moved), off_canvas);
      } else {
        wsprintfW(group_buffer, L"移动多个图层：位移 (%d, %d)，%u 个成员（松开算一步撤销）%s", dx,
                  dy, static_cast<unsigned>(moved), off_canvas);
      }
    } else if (shell.doc.layer_move_duplicated) {
      wsprintfW(group_buffer, L"复制并移动图层组：位移 (%d, %d)，%u 个成员（松开算一步撤销）%s",
                dx, dy, static_cast<unsigned>(moved), off_canvas);
    } else {
      wsprintfW(group_buffer, L"移动图层组：位移 (%d, %d)，%u 个成员（松开算一步撤销）%s", dx, dy,
                static_cast<unsigned>(moved), off_canvas);
    }
    set_status(shell, group_buffer);
    return;
  }

  comp_rect target = shell.doc.layer_move_base;
  target.x += dx;
  target.y += dy;
  if (comp_layer_set_transform(shell.doc.document, shell.doc.layer_move_id, target,
                               shell.doc.layer_move_rotation) != COMP_OK) {
    return;
  }
  shell.doc.layer_move_applied_dx = dx;
  shell.doc.layer_move_applied_dy = dy;
  shell.doc.layer_move_dirty = true;
  shell.doc.needs_render = true;

  /*
   * 拖到完全离开画布时补一句：画面上会一点痕迹都不剩，不提示就像图层被拖丢了。
   * 判断是纯函数（ui_core::layer_rect_touches_canvas，AABB 保守求交），
   * 每帧重算：拖回来提示自动消失，不需要额外状态。
   */
  const wchar_t* off_canvas = comp_ui::layer_rect_touches_canvas(
                                 target.x, target.y, target.w, target.h,
                                 static_cast<double>(shell.doc.layer_move_rotation), shell.doc.width,
                                 shell.doc.height)
                                 ? L""
                                 : L" · 已拖出画布";
  wchar_t buffer[224];
  if (shell.doc.layer_move_duplicated) {
    wsprintfW(buffer, L"复制并移动图层：(%d, %d)，位移 (%d, %d)（松开算一步撤销）%s", target.x,
              target.y, dx, dy, off_canvas);
  } else {
    wsprintfW(buffer, L"移动图层：(%d, %d)，位移 (%d, %d)（松开算一步撤销）%s", target.x, target.y,
              dx, dy, off_canvas);
  }
  set_status(shell, buffer);
}

void layer_move_end(HWND window, ShellState& shell) {
  if (!shell.doc.layer_move_active) return;

  /* 与滑杆同一顺序：先清状态再 ReleaseCapture，捕获变更会重入 move_cancel。 */
  const comp_txn txn = shell.doc.layer_move_txn;
  const bool dirty = shell.doc.layer_move_dirty;
  const bool duplicated = shell.doc.layer_move_duplicated;
  const int32_t dx = shell.doc.layer_move_applied_dx;
  const int32_t dy = shell.doc.layer_move_applied_dy;
  /*
   * §6.61 组提示必须在清 state 之前算：group_members_off_canvas 读的正是成员快照。
   * 组自己的 transform 不参与合成，拿它算「是否拖出画布」只会得到错误答案。
   */
  const size_t member_count = shell.doc.layer_move_members.size();
  const bool was_group = shell.doc.layer_move_is_group;
  const bool was_multi = shell.doc.layer_move_multi;
  const bool copy_failed = shell.doc.layer_move_copy_failed;
  const bool group_off_canvas = group_members_off_canvas(shell, dx, dy);
  shell.doc.layer_move_active = false;
  shell.doc.layer_move_dirty = false;
  shell.doc.layer_move_doc = nullptr;
  shell.doc.layer_move_txn = nullptr;
  shell.doc.layer_move_alt = false;
  shell.doc.layer_move_duplicated = false;
  shell.doc.layer_move_is_group = false;
  shell.doc.layer_move_multi = false;
  shell.doc.layer_move_copy_failed = false;
  shell.doc.layer_move_ids.clear();
  shell.doc.layer_move_members.clear();
  ReleaseCapture();

  if (txn == nullptr) return;
  if (!dirty) {
    /* 只点不拖：空事务不该占一步撤销（Alt 时复制尚未发生，没有残留副本）。 */
    comp_txn_abort(txn);
    /* 万一复制成功但紧接着写 transform 失败，abort 会把副本一起回滚，
     * 面板里那个 id 已经不存在——重取一次，别留着悬空选中。 */
    if (duplicated) refresh_layers(shell);
    if (copy_failed) {
      /* 多选 Alt：部分副本已经在事务里，abort 把它们连根回滚了。 */
      refresh_layers(shell);
      set_status(shell, L"复制多个图层失败：已回滚，位置未变化");
    } else if (was_multi) {
      set_status(shell, L"移动多个图层：位置未变化");
    } else if (was_group) {
      set_status(shell, L"移动图层组：位置未变化");
    } else {
      set_status(shell, L"移动：位置未变化");
    }
    return;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, was_multi ? L"移动多个图层：提交失败"
                                : was_group ? L"移动图层组：提交失败" : L"移动图层：提交失败");
    return;
  }

  shell.doc.needs_render = true;
  wchar_t buffer[224];
  if (was_multi) {
    const wchar_t* off_canvas =
        group_off_canvas ? L" · 已拖出画布（图层仍在面板里，可拖回或 Ctrl+Z）" : L"";
    if (duplicated) {
      wsprintfW(buffer, L"已复制并移动多个图层：位移 (%d, %d)，%u 个成员（一步撤销）%s", dx, dy,
                static_cast<unsigned>(member_count), off_canvas);
    } else {
      wsprintfW(buffer, L"已移动多个图层：位移 (%d, %d)，%u 个成员（一步撤销）%s", dx, dy,
                static_cast<unsigned>(member_count), off_canvas);
    }
    refresh_history_status(shell, buffer);
    return;
  }
  if (was_group) {
    const wchar_t* off_canvas =
        group_off_canvas ? L" · 已拖出画布（组仍在面板里，可拖回或 Ctrl+Z）" : L"";
    if (duplicated) {
      wsprintfW(buffer, L"已复制并移动图层组：位移 (%d, %d)，%u 个成员（一步撤销）%s", dx, dy,
                static_cast<unsigned>(member_count), off_canvas);
    } else {
      wsprintfW(buffer, L"已移动图层组：位移 (%d, %d)，%u 个成员（一步撤销）%s", dx, dy,
                static_cast<unsigned>(member_count), off_canvas);
    }
    refresh_history_status(shell, buffer);
    return;
  }

  /* 松手时同样提示（拖拽中的提示会被这条覆盖）：base + 位移就是刚提交的矩形。 */
  const wchar_t* off_canvas =
      comp_ui::layer_rect_touches_canvas(
          shell.doc.layer_move_base.x + dx, shell.doc.layer_move_base.y + dy, shell.doc.layer_move_base.w,
          shell.doc.layer_move_base.h, static_cast<double>(shell.doc.layer_move_rotation), shell.doc.width,
          shell.doc.height)
          ? L""
          : L" · 已拖出画布（图层仍在面板里，可拖回或 Ctrl+Z）";
  if (duplicated) {
    wsprintfW(buffer, L"已复制并移动图层：位移 (%d, %d)（一步撤销）%s", dx, dy, off_canvas);
  } else {
    wsprintfW(buffer, L"已移动图层：位移 (%d, %d)（一步撤销）%s", dx, dy, off_canvas);
  }
  refresh_history_status(shell, buffer);
}

void move_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!move_available(shell)) return;
  /* 手柄优先于任何拖动语义：按在手柄上就是缩放/旋转，不做整层平移。 */
  if (layer_handle_begin(window, shell, x, y, layout)) return;
  if (!shell.doc.selection_active) {
    /* 无选区 = 拖整层（上游 V 的默认行为），不再当成错误。 */
    layer_move_begin(window, shell, x, y, layout);
    return;
  }
  /*
   * §6.63 有选区时按**落点**分流（上游 V 的语义）：
   *   - 落点在选区内 -> 搬选区像素（旧行为，蚂蚁线跟着走）；
   *   - 落点在选区外 -> 搬整层（transform），选区原地不动。
   * 蒙版读不到（`selection_mask` 空）时不猜：退回搬像素的旧行为，因为这时
   * 「in/out」没有依据，改走整层拖动才是真的猜。
   */
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  const bool mask_known = !shell.doc.selection_mask.empty() && shell.doc.width > 0 && shell.doc.height > 0;
  const bool inside =
      !mask_known || comp_ui::selection_contains_point(
                         shell.doc.selection_mask.data(), shell.doc.width, shell.doc.height,
                         static_cast<std::size_t>(shell.doc.width), doc.x, doc.y, 128);
  if (!inside) {
    layer_move_begin(window, shell, x, y, layout, L"落点在选区外：移动整层，选区不动");
    return;
  }
  shell.doc.move_active = true;
  shell.doc.move_start = doc;
  shell.doc.move_current = doc;
  shell.doc.move_duplicate = (GetKeyState(VK_MENU) & 0x8000) != 0;
  SetCapture(window);
  set_status(shell, shell.doc.move_duplicate
                        ? L"移动选区像素：Alt = 复制，松开提交"
                        : L"移动选区像素：松开提交（按住 Alt = 复制）");
}

void move_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (shell.doc.layer_handle_active) {
    layer_handle_extend(shell, x, y, layout);
    return;
  }
  if (shell.doc.layer_move_active) {
    layer_move_extend(shell, x, y, layout);
    return;
  }
  if (!shell.doc.move_active) return;
  shell.doc.move_current =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
}

void move_cancel(ShellState& shell) {
  bool rolled_back_copy = false;
  /*
   * 换文档路径也会走到这里，而那时旧文档已经 destroy：事务句柄随文档失效，
   * abort 会打到已释放对象上（UAF）。与 color_range 同一规则——只有当事务
   * 属于**当前**文档时才 abort。
   */
  if (shell.doc.layer_handle_txn != nullptr && shell.doc.layer_handle_doc == shell.doc.document) {
    comp_txn_abort(shell.doc.layer_handle_txn);
    shell.doc.needs_render = true; /* abort 把 transform 退回去，画面必须重画 */
  }
  layer_handle_clear(shell);
  if (shell.doc.layer_move_txn != nullptr && shell.doc.layer_move_doc == shell.doc.document) {
    comp_txn_abort(shell.doc.layer_move_txn);
    /*
     * Alt 拖动已经复制过（单层或 §6.64 整批）：abort 把副本一起撤了，面板里这些
     * 副本 id 已经不存在——不重取会让选中项悬空，下一次命令对着死 id 发写入。
     */
    rolled_back_copy = shell.doc.layer_move_duplicated;
  }
  shell.doc.layer_move_txn = nullptr;
  shell.doc.layer_move_doc = nullptr;
  shell.doc.layer_move_active = false;
  shell.doc.layer_move_dirty = false;
  shell.doc.layer_move_alt = false;
  shell.doc.layer_move_duplicated = false;
  shell.doc.layer_move_is_group = false;
  shell.doc.layer_move_multi = false;
  shell.doc.layer_move_copy_failed = false;
  shell.doc.layer_move_ids.clear();
  shell.doc.layer_move_members.clear();
  if (rolled_back_copy) {
    refresh_layers(shell);
    shell.doc.needs_render = true;
  }
  shell.doc.move_active = false;
}

void move_end(HWND window, ShellState& shell) {
  if (shell.doc.layer_handle_active) {
    layer_handle_end(window, shell);
    return;
  }
  if (shell.doc.layer_move_active) {
    layer_move_end(window, shell);
    return;
  }
  if (!shell.doc.move_active) return;
  shell.doc.move_active = false;
  ReleaseCapture();
  if (shell.doc.document == nullptr) return;

  const int32_t dx =
      static_cast<int32_t>(std::lround(shell.doc.move_current.x - shell.doc.move_start.x));
  const int32_t dy =
      static_cast<int32_t>(std::lround(shell.doc.move_current.y - shell.doc.move_start.y));
  if (dx == 0 && dy == 0) {
    set_status(shell, L"移动：位置未变化");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"移动：请先选择一个图层");
    return;
  }

  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document,
                     shell.doc.move_duplicate ? "Duplicate Selection Pixels"
                                          : "Move Selection Pixels",
                     &txn) != COMP_OK) {
    set_status(shell, L"移动：无法开始事务");
    return;
  }

  /* 顺序不能反：move_pixels 按当前选区取像素，先平移轮廓会搬错地方。 */
  int32_t moved_pixels = 0;
  int32_t moved_outline = 0;
  comp_result result = comp_selection_move_pixels(
      shell.doc.document, id, dx, dy, shell.doc.move_duplicate ? 1 : 0, &moved_pixels);
  if (result == COMP_OK) {
    result = comp_selection_translate(shell.doc.document, dx, dy, &moved_outline);
  }
  if (result != COMP_OK) {
    /* 失败必须整体回滚：半搬的像素比不搬更坏。 */
    comp_txn_abort(txn);
    set_status(shell, L"移动失败（16 位 / 带旋转或翻转的图层不支持像素搬移，轮廓已回滚）");
    return;
  }
  if (moved_pixels == 0 && moved_outline == 0) {
    comp_txn_abort(txn);
    set_status(shell, L"移动：选区内没有可搬移的像素");
    return;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"移动：提交失败");
    return;
  }

  refresh_selection(shell);
  shell.doc.needs_render = true;
  wchar_t buffer[224];
  wsprintfW(buffer, L"%s：位移 (%d, %d) px，%d 个像素",
            shell.doc.move_duplicate ? L"已复制选区像素" : L"已移动选区像素", dx, dy,
            moved_pixels);
  refresh_history_status(shell, buffer);
}

}  // namespace comp_shell
