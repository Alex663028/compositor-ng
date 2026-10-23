// AI生成
/*
 * shell_paint.cpp — 画布渲染、工具条与各面板绘制、上屏
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {


comp_ui::Rect to_ui_rect(const comp_rect& rect) {
  return comp_ui::Rect{rect.x, rect.y, rect.w, rect.h};
}

comp_rect to_abi_rect(const comp_ui::Rect& rect) {
  return comp_rect{rect.x, rect.y, rect.w, rect.h};
}

/*
 * 新文档的背景填充。引擎建文档时给的是**透明**背景层
 * （ensure_background_layer -> clear_transparent），所以「白底 / 前景色底」必须
 * 自己写一遍像素。写入走 comp_layer_set_pixels（与画笔同一条 ABI），因此会进
 * 历史：新建带底色的文档一开始就是脏文档——它确实还没存过盘，标题上的 * 是实话。
 */
bool fill_document_background(ShellState& shell, int background) {
  if (background == 1 || shell.doc.document == nullptr) return true; /* 透明 = 引擎缺省 */
  if (shell.doc.width <= 0 || shell.doc.height <= 0 || shell.doc.layers.empty()) return false;
  const uint8_t r = background == 2 ? shell.foreground.r : 255;
  const uint8_t g = background == 2 ? shell.foreground.g : 255;
  const uint8_t b = background == 2 ? shell.foreground.b : 255;
  /* 预乘 RGBA8，alpha = 255 时预乘与直写相同。 */
  std::vector<uint8_t> pixels(static_cast<size_t>(shell.doc.width) * shell.doc.height * 4, 255);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = r;
    pixels[i + 1] = g;
    pixels[i + 2] = b;
    pixels[i + 3] = 255;
  }
  comp_image image{};
  image.width = shell.doc.width;
  image.height = shell.doc.height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(shell.doc.width) * 4;
  image.pixels = pixels.data();
  return comp_layer_set_pixels(shell.doc.document, shell.doc.layers.front().id, &image) == COMP_OK;
}

/* 返回 false = 这一帧没画出来（无文档 / 空缓冲 / flatten 失败），framebuffer 仍是旧内容。
 * 滤镜预览要靠这个返回值决定「预览帧到底成不成立」。 */
bool render_canvas(ShellState& shell) {
  if (shell.doc.document == nullptr || shell.doc.rgba.empty()) return false;
  const size_t stride = static_cast<size_t>(shell.doc.width) * 4;
  const comp_result result =
      comp_render_flatten(shell.doc.document, shell.doc.rgba.data(), COMP_PIXEL_RGBA8, stride);
  if (result == COMP_OK) {
    /* 预乘 RGBA8 -> BGRA：GDI 的 BI_RGB 32bpp 位图按 BGRA 解释。 */
    const size_t pixel_count = static_cast<size_t>(shell.doc.width) * static_cast<size_t>(shell.doc.height);
    for (size_t i = 0; i < pixel_count; ++i) {
      const uint8_t* src = shell.doc.rgba.data() + i * 4;
      uint32_t bgra = (static_cast<uint32_t>(src[3]) << 24) |
                      (static_cast<uint32_t>(src[0]) << 16) |
                      (static_cast<uint32_t>(src[1]) << 8) |
                      static_cast<uint32_t>(src[2]);
      shell.doc.framebuffer[i] = bgra;
    }
  }
  shell.doc.needs_render = false;
  return result == COMP_OK;
}

/* 工具条：按钮几何与命中共享 ui_core.h，壳只负责把选中态画出来。 */
const wchar_t* backend_label(comp_backend backend) {
  switch (backend) {
    case COMP_BACKEND_D3D12:  return L"D3D12";
    case COMP_BACKEND_VULKAN: return L"Vulkan";
    case COMP_BACKEND_METAL:  return L"Metal";
    case COMP_BACKEND_CPU:    return L"CPU";
    default:                  return L"AUTO";
  }
}

void draw_tool_icon(HDC dc, comp_ui::ToolId tool, const comp_ui::Rect& button, COLORREF color) {
  const int side = (button.w < button.h ? button.w : button.h) - 8;
  if (side < 8) return;
  const double scale = side / 32.0;
  const ToolIconMap map{dc, static_cast<double>(button.x) + (button.w - side) * 0.5,
                        static_cast<double>(button.y) + (button.h - side) * 0.5, scale};
  const int width = static_cast<int>(std::lround(2.0 * scale));
  HPEN pen = CreatePen(PS_SOLID, width < 1 ? 1 : width, color);
  HPEN dot = CreatePen(PS_DOT, 1, color);
  HBRUSH brush = CreateSolidBrush(color);
  HGDIOBJ old_pen = SelectObject(dc, pen);
  HGDIOBJ old_brush = SelectObject(dc, brush);
  /* 空心形状先换成 NULL_BRUSH，画完再换回来；fill 只是给后面实心图元复位。 */
  const auto hollow = [&]() { SelectObject(dc, GetStockObject(NULL_BRUSH)); };
  const auto fill = [&]() { SelectObject(dc, brush); };

  switch (tool) {
    case comp_ui::ToolId::Select: {
      /* 光标箭头：外轮廓闭合，尾巴向右下，形状一眼能认。 */
      static const double arrow[] = {11, 5,  11, 26, 16, 21, 19, 29,
                                     23, 27, 20, 19, 26, 19};
      map.poly(arrow, 7, true);
      break;
    }
    case comp_ui::ToolId::Move: {
      map.line(5, 16, 27, 16);
      map.line(16, 5, 16, 27);
      map.line(5, 16, 10, 12);
      map.line(5, 16, 10, 20);
      map.line(27, 16, 22, 12);
      map.line(27, 16, 22, 20);
      map.line(16, 5, 12, 10);
      map.line(16, 5, 20, 10);
      map.line(16, 27, 12, 22);
      map.line(16, 27, 20, 22);
      break;
    }
    case comp_ui::ToolId::Marquee: {
      HGDIOBJ previous = SelectObject(dc, dot);
      hollow();
      map.rect(5, 8, 27, 24);
      fill();
      SelectObject(dc, previous);
      break;
    }
    case comp_ui::ToolId::Lasso: {
      static const double loop[] = {10, 9, 15, 5, 22, 6, 26, 11,
                                    25, 18, 19, 22, 12, 21, 8, 16, 10, 9};
      map.poly(loop, 9, false);
      map.line(12, 21, 11, 28);
      map.line(11, 28, 15, 27);
      break;
    }
    case comp_ui::ToolId::Pen: {
      /* 笔尖用空心轮廓：中间的笔缝要能透出来，实心画就成一团了。 */
      hollow();
      static const double nib[] = {21, 4, 28, 11, 14, 25, 6, 27, 7, 19};
      map.poly(nib, 5, true);
      fill();
      map.line(10, 16, 16, 22);
      break;
    }
    case comp_ui::ToolId::Wand: {
      map.line(5, 27, 18, 14);
      static const double star[] = {23, 4, 25, 8, 29, 10, 25, 12, 23, 16, 21, 12, 17, 10, 21, 8};
      map.poly(star, 8, true);
      break;
    }
    case comp_ui::ToolId::Crop: {
      map.line(10, 5, 10, 22);
      map.line(10, 22, 27, 22);
      map.line(5, 10, 22, 10);
      map.line(22, 10, 22, 27);
      break;
    }
    case comp_ui::ToolId::Eyedropper: {
      map.circle(22, 10, 4);
      map.line(19, 13, 8, 24);
      map.line(6, 22, 10, 26);
      break;
    }
    case comp_ui::ToolId::Brush: {
      map.line(23, 5, 14, 14);
      static const double ferrule[] = {14, 14, 19, 19, 16, 22, 11, 17};
      map.poly(ferrule, 4, true);
      map.line(12, 18, 5, 27);
      map.line(15, 21, 8, 29);
      break;
    }
    case comp_ui::ToolId::Eraser: {
      static const double body[] = {8, 22, 18, 7, 26, 13, 16, 28};
      map.poly(body, 4, true);
      map.line(12, 25, 22, 10);
      break;
    }
    case comp_ui::ToolId::Heal: {
      hollow();
      static const double band[] = {7, 19, 19, 7, 25, 13, 13, 25};
      map.poly(band, 4, true);
      fill();
      map.line(12, 16, 20, 16);
      map.line(16, 12, 16, 20);
      break;
    }
    case comp_ui::ToolId::Clone: {
      map.rect(11, 5, 21, 12);
      map.rect(13, 12, 19, 15);
      map.rect(8, 15, 24, 21);
      map.rect(6, 23, 26, 27);
      break;
    }
    case comp_ui::ToolId::Retouch: {
      static const double tear[] = {16, 4, 10, 20, 22, 20};
      map.poly(tear, 3, true);
      map.circle(16, 21, 6);
      break;
    }
    case comp_ui::ToolId::Tone: {
      map.circle(16, 16, 5);
      for (int i = 0; i < 8; ++i) {
        const double angle = i * 3.14159265358979323846 / 4.0;
        map.line(16.0 + std::cos(angle) * 9.0, 16.0 + std::sin(angle) * 9.0,
                 16.0 + std::cos(angle) * 13.0, 16.0 + std::sin(angle) * 13.0);
      }
      break;
    }
    case comp_ui::ToolId::Gradient: {
      /* 方框 + 对角半实心：这是「渐变」的通用字形，堆条纹在小尺寸下会看成图片图标。 */
      hollow();
      map.rect(6, 8, 26, 24);
      fill();
      static const double ramp[] = {7, 23, 25, 23, 25, 9};
      map.poly(ramp, 3, true);
      break;
    }
    case comp_ui::ToolId::Shape: {
      /* 实心方块 + 空心圆：两个都空心会看成「复制」图标。 */
      map.rect(5, 8, 18, 22);
      hollow();
      map.circle(21, 17, 7);
      fill();
      break;
    }
    case comp_ui::ToolId::Type: {
      map.line(8, 7, 24, 7);
      map.line(16, 7, 16, 26);
      map.line(12, 26, 20, 26);
      break;
    }
    case comp_ui::ToolId::Hand: {
      hollow();
      map.round_rect(9, 15, 23, 28, 6);
      map.line(12, 15, 12, 8);
      map.line(15.5, 15, 15.5, 6);
      map.line(19, 15, 19, 8);
      map.line(9, 19, 5, 15);
      fill();
      break;
    }
    case comp_ui::ToolId::Zoom: {
      hollow();
      map.circle(14, 14, 8);
      fill();
      map.line(20, 20, 27, 27);
      map.line(10, 14, 18, 14);
      map.line(14, 10, 14, 18);
      break;
    }
    case comp_ui::ToolId::HistoryBrush: {
      hollow();
      map.circle(16, 16, 10);
      fill();
      map.line(16, 16, 16, 9);
      map.line(16, 16, 21, 19);
      break;
    }
    case comp_ui::ToolId::Count:
    default:
      break;
  }

  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(brush);
  DeleteObject(dot);
  DeleteObject(pen);
}

void draw_toolbar(HDC dc, const ShellState& shell, const comp_ui::Rect& bar) {
  RECT background_rect{bar.x, bar.y, bar.right(), bar.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(44, 46, 52));
  FillRect(dc, &background_rect, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  const int count = comp_ui::tool_count();
  for (int i = 0; i < count; ++i) {
    const comp_ui::ToolInfo* info = comp_ui::tool_info_at(i);
    if (info == nullptr) continue;
    const comp_ui::Rect rect = comp_ui::tool_button_rect(bar, i);
    RECT button{rect.x, rect.y, rect.right(), rect.bottom()};
    const bool selected = info->id == shell.tool;
    HBRUSH fill = CreateSolidBrush(selected ? RGB(70, 104, 168) : RGB(58, 60, 68));
    FillRect(dc, &button, fill);
    DeleteObject(fill);
    /* 图标跟按钮文字同色：选中纯白、未选中浅灰，短键看状态栏的「工具名（V）」。 */
    draw_tool_icon(dc, info->id, rect, selected ? RGB(255, 255, 255) : RGB(198, 200, 208));
  }

  /* 右侧读数：缩放、笔刷，Retouch / Tone 工具再带上子模式与当前参数。 */
  wchar_t extra[96] = L"";
  if (shell.tool == comp_ui::ToolId::Tone) {
    wsprintfW(extra, L"   %s / 强度 %d%%",
              widen_ascii(comp_ui::tone_state_name(shell.tone_state)).c_str(),
              static_cast<int>(shell.tone_strength * 100.0 + 0.5));
  } else if (shell.tool == comp_ui::ToolId::Retouch) {
    if (shell.retouch_mode == comp_ui::kRetouchBlur) {
      wsprintfW(extra, L"   %s / 半径 %d px",
                widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str(),
                static_cast<int>(shell.blur_radius + 0.5));
    } else {
      wsprintfW(extra, L"   %s / 强度 %d%%",
                widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str(),
                static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
    }
  } else if (shell.tool == comp_ui::ToolId::HistoryBrush) {
    /* 取样源与不透明度必须常显：这支笔画出来的东西完全由它们决定。 */
    wsprintfW(extra, L"   取样 #%d / 不透明度 %d%%", history_brush_resolved_state(shell),
              static_cast<int>(shell.history_brush_opacity * 100.0 + 0.5));
  } else if (shell.tool == comp_ui::ToolId::Clone) {
    /* 源点决定仿出来的位置，必须常显；未设源点时明说，否则用户会以为工具坏了。 */
    if (shell.doc.clone_source_set) {
      wsprintfW(extra, L"   源 %d,%d%s", static_cast<int>(shell.doc.clone_source_x),
                static_cast<int>(shell.doc.clone_source_y),
                shell.clone_sample_all ? L" 合并" : L" 图层");
    } else {
      wsprintfW(extra, L"   未设源点");
    }
  }
  wchar_t info_text[192];
  wsprintfW(info_text, L"缩放 %d%%   笔刷 %d px / %d%%   渲染 %s%s",
            static_cast<int>(shell.doc.view.zoom * 100.0 + 0.5),
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(shell.brush_hardness * 100.0 + 0.5),
            backend_label(shell.backend), extra);
  RECT info_rect{bar.right() - 520, bar.y, bar.right() - 10, bar.bottom()};
  SetTextColor(dc, RGB(172, 176, 188));
  DrawTextW(dc, info_text, -1, &info_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

/* 选项栏的值文本：整数百分比与枚举名各走一条，绘制与状态反馈共用。 */
std::wstring option_value_text(const ShellState& shell, const comp_ui::ToolOptionSpec& spec) {
  wchar_t buffer[48];
  switch (spec.id) {
    case comp_ui::ToolOptionId::BrushSize:
      wsprintfW(buffer, L"%d px", static_cast<int>(shell.brush_size + 0.5));
      return buffer;
    case comp_ui::ToolOptionId::BrushHardness:
      wsprintfW(buffer, L"%d%%", static_cast<int>(shell.brush_hardness * 100.0 + 0.5));
      return buffer;
    case comp_ui::ToolOptionId::WandTolerance:
      wsprintfW(buffer, L"%d", shell.wand_tolerance);
      return buffer;
    case comp_ui::ToolOptionId::SelectionFeather:
      wsprintfW(buffer, L"%d px", shell.selection_feather);
      return buffer;
    case comp_ui::ToolOptionId::GradientType:
      return widen_ascii(comp_ui::gradient_type_name(shell.gradient_type));
    case comp_ui::ToolOptionId::ShapeKind:
      return widen_ascii(comp_ui::shape_kind_name(shell.shape_kind));
    case comp_ui::ToolOptionId::ShapeStrokeWidth:
      wsprintfW(buffer, L"%.1f px", shell.shape_stroke_width);
      return buffer;
    case comp_ui::ToolOptionId::CloneSample:
      return shell.clone_sample_all ? L"合并" : L"图层";
    case comp_ui::ToolOptionId::CloneAligned:
      return shell.clone_aligned ? L"开" : L"关";
    case comp_ui::ToolOptionId::HealMode:
      return widen_ascii(comp_ui::heal_mode_name(shell.heal_mode));
    default:
      return L"";
  }
}

/*
 * 点击选项栏：改的是**同一个** ShellState 字段，画笔下一笔就生效，不需要
 * 任何「应用」按钮。返回 false 表示这一下不是选项栏吃掉的（调用方继续派发）。
 *
 * 边界夹取全部复用 ui_core 的 clamp/step：横栏与右栏/快捷键改的是同一个值，
 * 两处各写一份夹取迟早会分叉（例如横栏能拖到 600px 而快捷键卡在 500px）。
 */
bool apply_tool_option(ShellState& shell, comp_ui::ToolOptionId id, int delta) {
  switch (id) {
    case comp_ui::ToolOptionId::BrushSize:
      shell.brush_size = comp_ui::step_brush_size(shell.brush_size, delta);
      break;
    case comp_ui::ToolOptionId::BrushHardness:
      shell.brush_hardness = comp_ui::step_brush_hardness(shell.brush_hardness, delta);
      break;
    case comp_ui::ToolOptionId::WandTolerance:
      shell.wand_tolerance = comp_ui::step_wand_tolerance(shell.wand_tolerance, delta);
      break;
    case comp_ui::ToolOptionId::SelectionFeather:
      shell.selection_feather = comp_ui::step_selection_feather(shell.selection_feather, delta);
      break;
    case comp_ui::ToolOptionId::GradientType:
      shell.gradient_type =
          comp_ui::option_cycle_choice(shell.gradient_type, comp_ui::kGradientTypeCount, delta);
      break;
    case comp_ui::ToolOptionId::ShapeKind:
      shell.shape_kind =
          comp_ui::option_cycle_choice(shell.shape_kind, comp_ui::kShellShapeKindCount, delta);
      break;
    case comp_ui::ToolOptionId::ShapeStrokeWidth:
      shell.shape_stroke_width = comp_ui::step_shape_stroke_width(shell.shape_stroke_width, delta);
      break;
    case comp_ui::ToolOptionId::CloneSample:
      shell.clone_sample_all = !shell.clone_sample_all;
      break;
    case comp_ui::ToolOptionId::CloneAligned:
      shell.clone_aligned = !shell.clone_aligned;
      break;
    case comp_ui::ToolOptionId::HealMode:
      shell.heal_mode = comp_ui::option_cycle_choice(shell.heal_mode, comp_ui::kHealModeCount, delta);
      break;
    default:
      return false;
  }
  return true;
}

/*
 * §5.49 工具选项栏：一条横栏，左边是当前工具的参数组。
 * 几何全部来自 ui_core 的 option_group_rect / option_control_rect，绘制与命中
 * 用同一份矩形——横栏只有 32px 高，差 1px 就会点错组（与面板分界线同一教训）。
 */
void draw_tool_options(HDC dc, const ShellState& shell, const comp_ui::Rect& bar) {
  RECT background_rect{bar.x, bar.y, bar.right(), bar.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(38, 40, 46));
  FillRect(dc, &background_rect, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  const int count = comp_ui::tool_option_count(shell.tool);
  if (count == 0) {
    RECT hint{bar.x + comp_ui::kOptionLeftPad, bar.y, bar.right() - 12, bar.bottom()};
    SetTextColor(dc, RGB(118, 122, 134));
    DrawTextW(dc, L"该工具没有选项栏参数", -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    return;
  }

  for (int i = 0; i < count; ++i) {
    const comp_ui::Rect group = comp_ui::option_group_rect(bar, i);
    if (group.empty()) break; /* 后面的组只会更放不下 */
    const comp_ui::ToolOptionSpec spec = comp_ui::tool_option(shell.tool, i);

    RECT label{group.x, group.y, group.x + comp_ui::kOptionLabelWidth - 6, group.bottom()};
    SetTextColor(dc, RGB(170, 174, 186));
    DrawTextW(dc, widen_ascii(spec.label).c_str(), -1, &label,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const comp_ui::Rect control = comp_ui::option_control_rect(group);
    if (control.empty()) continue;
    RECT box{control.x, control.y, control.right(), control.bottom()};
    HBRUSH fill = CreateSolidBrush(RGB(58, 60, 68));
    FillRect(dc, &box, fill);
    DeleteObject(fill);

    RECT value{control.x, control.y, control.right(), control.bottom()};
    SetTextColor(dc, RGB(228, 230, 236));
    DrawTextW(dc, option_value_text(shell, spec).c_str(), -1, &value,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (spec.kind == comp_ui::ToolOptionKind::Stepper) {
      /* Stepper 的箭头画在值两侧：命中测试按同样两条 18px 带子判断。 */
      RECT left{control.x, control.y, control.x + comp_ui::kOptionArrowWidth, control.bottom()};
      RECT right{control.right() - comp_ui::kOptionArrowWidth, control.y, control.right(),
                 control.bottom()};
      SetTextColor(dc, RGB(196, 200, 210));
      DrawTextW(dc, L"-", -1, &left, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      DrawTextW(dc, L"+", -1, &right, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }
}

/* 状态栏：工具 / 缩放 / 历史，加上壳层最近一条反馈。 */
void draw_status_bar(HDC dc, const ShellState& shell, const comp_ui::Rect& bar) {
  RECT background_rect{bar.x, bar.y, bar.right(), bar.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(26, 28, 32));
  FillRect(dc, &background_rect, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  const comp_ui::ToolInfo* info = comp_ui::tool_info(shell.tool);
  wchar_t tool_name[96];
  if (shell.tool == comp_ui::ToolId::Retouch) {
    /* R 键在同一工具内循环：状态栏必须显示当前子模式，否则用户不知道自己拿的是哪个。 */
    wsprintfW(tool_name, L"Retouch / %s",
              widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str());
  } else if (shell.tool == comp_ui::ToolId::Tone) {
    /* O 键同理：8 档共用同一个键，状态栏不写清楚就等于没切。 */
    wsprintfW(tool_name, L"%s", widen_ascii(comp_ui::tone_state_name(shell.tone_state)).c_str());
  } else {
    wsprintfW(tool_name, L"%s", info != nullptr ? widen_ascii(info->name).c_str() : L"?");
  }
  wchar_t extra[64] = L"";
  if (shell.tool == comp_ui::ToolId::Tone) {
    wsprintfW(extra, L" · 强度 %d%%", static_cast<int>(shell.tone_strength * 100.0 + 0.5));
  } else if (shell.tool == comp_ui::ToolId::Retouch) {
    if (shell.retouch_mode == comp_ui::kRetouchBlur) {
      wsprintfW(extra, L" · 半径 %d px", static_cast<int>(shell.blur_radius + 0.5));
    } else {
      wsprintfW(extra, L" · 强度 %d%%",
                static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
    }
  } else if (shell.tool == comp_ui::ToolId::HistoryBrush) {
    wsprintfW(extra, L" · 取样 #%d · 不透明度 %d%%", history_brush_resolved_state(shell),
              static_cast<int>(shell.history_brush_opacity * 100.0 + 0.5));
  } else if (shell.tool == comp_ui::ToolId::Clone && shell.doc.clone_source_set) {
    wsprintfW(extra, L" · 源 (%d, %d)%s", static_cast<int>(shell.doc.clone_source_x),
              static_cast<int>(shell.doc.clone_source_y),
              shell.clone_sample_all ? L" 合并" : L" 图层");
  }
  wchar_t line[768];
  wsprintfW(line, L"%s（%c） · 缩放 %d%% · 笔刷 %d px / %d%%%s · %s", tool_name,
            info != nullptr && info->shortcut != 0
                ? static_cast<wchar_t>(info->shortcut - 'a' + 'A')
                : L' ',
            static_cast<int>(shell.doc.view.zoom * 100.0 + 0.5),
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(shell.brush_hardness * 100.0 + 0.5), extra, shell.status.c_str());
  /* 右端留给前景色读数：色块 + 十六进制，左端长文本按省略号截断。 */
  RECT text_rect{bar.x + 12, bar.y, bar.right() - 200, bar.bottom()};
  SetTextColor(dc, RGB(220, 222, 228));
  DrawTextW(dc, line, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  if (bar.w >= 260) {
    char hex[8] = {0};
    comp_ui::color_to_hex(shell.foreground, hex);
    RECT swatch{bar.right() - 190, bar.y + 5, bar.right() - 166, bar.y + 21};
    HBRUSH foreground_brush =
        CreateSolidBrush(RGB(shell.foreground.r, shell.foreground.g, shell.foreground.b));
    FillRect(dc, &swatch, foreground_brush);
    DeleteObject(foreground_brush);
    HBRUSH swatch_border = CreateSolidBrush(RGB(96, 100, 112));
    FrameRect(dc, &swatch, swatch_border);
    DeleteObject(swatch_border);

    wchar_t foreground_text[64];
    wsprintfW(foreground_text, L"前景 %s", widen_ascii(hex).c_str());
    RECT foreground_rect{bar.right() - 158, bar.y, bar.right() - 10, bar.bottom()};
    SetTextColor(dc, RGB(220, 222, 228));
    DrawTextW(dc, foreground_text, -1, &foreground_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
}

/* 图层面板：GDI 文本绘制（外壳不读像素，只呈现引擎的属性查询结果）。 */
void draw_layer_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  RECT panel{panel_rect.x, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(24, 26, 30));
  FillRect(dc, &panel, background);
  DeleteObject(background);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(240, 240, 244));
  wchar_t title[64];
  wsprintfW(title, L"图层（%u）", static_cast<unsigned>(shell.doc.layers.size()));
  TextOutW(dc, panel_rect.x + 10, panel_rect.y + 8, title, lstrlenW(title));

  /*
   * S22：直方图打开时图层行整体下移，偏移与命中测试共用同一常量
   * （ui_core::layer_row_rect / layer_row_at 的 row_top 参数）。这里从布局算，
   * 不在壳里另写一个数字——两处各写一次必然有一天不一致。
   */
  const int row_top = comp_ui::layer_rows_top(shell.doc.histogram_open);

  const int32_t count = panel_row_count(shell);
  for (int32_t line = 0; line < count; ++line) {
    const comp_ui::Rect row_rect = comp_ui::layer_row_rect(panel_rect, line, row_top);
    const LayerRow* layer = layer_row_at(shell, line);
    if (layer == nullptr) continue;
    const LayerRow& row = *layer;
    /*
     * S44：子层缩进 + 组折叠三角。三角槽位在**所有**行上都留出（kLayerTreeMarker），
     * 同一层级的名字才会对齐；只有组行真的画三角。缩进有上限（见 ui_core），
     * 名字不会被推出栏外。
     */
    const int32_t depth = line < static_cast<int32_t>(shell.doc.layer_depth.size())
                              ? shell.doc.layer_depth[static_cast<size_t>(line)]
                              : 0;
    const int indent = comp_ui::layer_row_indent(depth);
    if (row.is_group != 0) {
      const comp_ui::Rect tri = comp_ui::layer_triangle_rect(row_rect, depth);
      const bool folded = layer_group_collapsed(shell, row.id);
      POINT points[3];
      if (folded) {
        points[0] = POINT{tri.x, tri.y};
        points[1] = POINT{tri.x, tri.bottom()};
        points[2] = POINT{tri.right(), tri.y + tri.h / 2};
      } else {
        points[0] = POINT{tri.x, tri.y};
        points[1] = POINT{tri.right(), tri.y};
        points[2] = POINT{tri.x + tri.w / 2, tri.bottom()};
      }
      HBRUSH tri_brush = CreateSolidBrush(RGB(196, 202, 214));
      HGDIOBJ old_brush = SelectObject(dc, tri_brush);
      Polygon(dc, points, 3);
      SelectObject(dc, old_brush);
      DeleteObject(tri_brush);
    }
    if (line == shell.doc.selected_layer) {
      RECT highlight{row_rect.x + 2, row_rect.y + 1, row_rect.right() - 2,
                     row_rect.bottom() - 1};
      HBRUSH selected = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, selected);
      DeleteObject(selected);
      SetTextColor(dc, RGB(255, 255, 255));
    } else {
      /*
       * P0-1 多选：主选中之外的多选行用更暗的底色区分（调整/效果面板仍只认
       * 主选中——批量命令才看多选集合）。拖到组行中间带的落点同样高亮：面板
       * 是扁平列表，没有这个提示用户看不出「拖进组」与「拖到组上方」的区别。
       */
      const bool multi_selected = panel_selection_contains(shell, row.id);
      const bool drop_into_group = shell.doc.layer_drag_active && row.is_group != 0 &&
                                   shell.doc.layer_drag_target == line;
      if (multi_selected || drop_into_group) {
        RECT highlight{row_rect.x + 2, row_rect.y + 1, row_rect.right() - 2,
                       row_rect.bottom() - 1};
        HBRUSH tint = CreateSolidBrush(drop_into_group ? RGB(48, 92, 72) : RGB(40, 46, 60));
        FillRect(dc, &highlight, tint);
        DeleteObject(tint);
      }
      SetTextColor(dc, multi_selected ? RGB(228, 232, 240) : RGB(206, 208, 216));
    }
    /* 混合模式名先转宽字符再拼：wsprintfW 的 %hs 行为不可靠（与既有 widen_ascii 用法一致）。 */
    const std::wstring blend_name = widen_ascii(comp_ui::blend_mode_name(row.blend));
    /*
     * S22 行内状态标记。三个都是「引擎已参与像素求值、面板此前看不见」的属性：
     *   M  = 带蒙版     （filter_graph 的 coverage.add(&layer->mask)）
     *   >| = 已剪贴     （composite_ops 的 clipping_factor）
     *   [] = 组且隔离   （filter_graph 按 node.isolated 定 child_backdrop）
     * S24 追加：
     *   H / V = 已水平 / 垂直翻转（flip_x / flip_y，合成时折进变换矩阵）
     *   fx    = 带效果        （effect_eval 的 6 种效果参与像素求值）
     * 不另开一列图标区：22px 行高 + 248px 栏宽已经排满，标号跟着层名走最省地方。
     */
    std::wstring marks;
    if (row.has_mask != 0) marks += L"M";
    if (row.clipping != 0) marks += L">|";
    if (row.is_group != 0) marks += row.isolated != 0 ? L"[]" : L"{}";
    if (row.flip_x != 0) marks += L"H";
    if (row.flip_y != 0) marks += L"V";
    if (row.effect_count > 0) marks += L"fx";
    /*
     * P0-1：行内显示图层名（引擎是唯一真源，refresh_layers 回读）。
     * 名称为空（旧引擎 / 查询失败）时回落到 id，面板不至于变成空行。
     * 栏宽 248px 排不下一整行，名字按宽字符截到 12 个并加省略号——截断只影响
     * 显示，写回的一律是原名。
     */
    std::wstring display_name =
        row.name.empty() ? (L"#" + std::to_wstring(static_cast<unsigned long long>(row.id)))
                         : widen_utf8(row.name);
    if (display_name.size() > 12) {
      display_name.resize(11);
      display_name += L"…";
    }
    std::wstring marks_line;
    if (!marks.empty()) {
      marks_line = L" " + marks;
    }
    wchar_t line_text[224];
    wsprintfW(line_text, L"%s %s  %s%s  %d%%", row.visible != 0 ? L"[o]" : L"[-]",
              display_name.c_str(), blend_name.c_str(), marks_line.c_str(),
              static_cast<int>(row.opacity * 100.0f + 0.5f));
    TextOutW(dc, row_rect.x + 10 + indent + comp_ui::kLayerTreeMarker, row_rect.y + 3,
             line_text, lstrlenW(line_text));
  }

  /*
   * S24 拖行落点指示线。位置由 ui_core::layer_insert_line_rect 从同一个 row_top
   * 算（与行矩形共用偏移），指示线不会与行错位——错位会让人以为层会落到相邻行。
   * 向上拖画目标行上边缘、向下拖画下边缘，见 layer_drag_insert_slot。
   */
  if (shell.doc.layer_drag_active && shell.doc.layer_drag_target >= 0 && count > 0 &&
      shell.doc.layer_drag_row >= 0) {
    const int32_t slot =
        comp_ui::layer_drag_insert_slot(shell.doc.layer_drag_row, shell.doc.layer_drag_target);
    const comp_ui::Rect line_rect =
        comp_ui::layer_insert_line_rect(panel_rect, row_top, slot, count);
    RECT line{line_rect.x, line_rect.y, line_rect.right(), line_rect.bottom()};
    HBRUSH line_brush = CreateSolidBrush(RGB(120, 200, 255));
    FillRect(dc, &line, line_brush);
    DeleteObject(line_brush);
  }

  /*
   * S22 直方图：256 桶亮度分布，像素由引擎只读入口给（预乘先反预乘再取亮度）。
   * 画成折线而不是 256 根柱子：栏宽只有 248px，每桶不到 1px，柱子会糊成一片；
   * 折线在任何宽度下都能看出形状（偏暗/偏亮/断层）。
   */
  if (shell.doc.histogram_open && !shell.doc.histogram_bins.empty()) {
    /* 几何来自 comp_ui：块贴在标题行下方，块底与第一行图层严丝合缝（见 S23 P2）。 */
    const comp_ui::Rect area = comp_ui::histogram_plot_rect(panel_rect);
    RECT frame{area.x, area.y, area.right(), area.bottom()};
    HBRUSH plot = CreateSolidBrush(RGB(14, 16, 20));
    FillRect(dc, &frame, plot);
    DeleteObject(plot);
    FrameRect(dc, &frame, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    /*
     * 可视区间由 shell.doc.histogram_view 决定（S24：拖拽框选 / 滚轮缩放 / 双击复位）。
     * 峰值只看可视区间：全幅峰值在放大后会把细节压成一条平线，正是缩放要看的
     * 那部分反而看不见了。
     */
    const comp_ui::HistogramView& view = shell.doc.histogram_view;
    double peak = 0.0;
    int32_t first_bin = static_cast<int32_t>(view.lo);
    int32_t last_bin = static_cast<int32_t>(view.hi + 0.999);
    if (first_bin < 0) first_bin = 0;
    if (last_bin > 255) last_bin = 255;
    for (int32_t i = first_bin; i <= last_bin; ++i) {
      const double value = shell.doc.histogram_bins[static_cast<size_t>(i)];
      if (value > peak) peak = value;
    }

    /* 框选预览先铺底，再画曲线，否则曲线会被色块盖掉。 */
    if (shell.doc.histogram_drag) {
      int32_t band_left = shell.doc.histogram_drag_from_x;
      int32_t band_right = shell.doc.histogram_drag_to_x;
      if (band_left > band_right) {
        const int32_t swap = band_left;
        band_left = band_right;
        band_right = swap;
      }
      if (band_left < area.x) band_left = area.x;
      if (band_right > area.right()) band_right = area.right();
      if (band_right > band_left) {
        RECT band{band_left, area.y + 1, band_right, area.bottom() - 1};
        HBRUSH band_brush = CreateSolidBrush(RGB(36, 64, 104));
        FillRect(dc, &band, band_brush);
        DeleteObject(band_brush);
        FrameRect(dc, &band, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
      }
    }

    if (peak > 0.0 && area.w > 1 && area.h > 1) {
      /*
       * 对数刻度：真实照片的直方图是「暗部一座山 + 高光一线天」，线性刻度下那条
       * 高光线永远只有 1px 高，用户看不见高光滚降——那是直方图最该看的东西。
       * 用 log1p 归一化后形状可读，且空图（全 0）不会除零。
       *
       * 每列一个采样点，x → bin 的映射与命中测试共用 histogram_x_to_bin：缩放后
       * 曲线与鼠标位置永远对得上（S22 用 i*width/255 的固定映射，一缩放就错位）。
       */
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 200, 255));
      HPEN old = static_cast<HPEN>(SelectObject(dc, pen));
      bool started = false;
      for (int column = 0; column < area.w; ++column) {
        const double bin =
            comp_ui::histogram_x_to_bin(area, view, static_cast<double>(area.x + column));
        int32_t index = static_cast<int32_t>(bin + 0.5);
        if (index < 0) index = 0;
        if (index > 255) index = 255;
        const double norm = std::log1p(shell.doc.histogram_bins[static_cast<size_t>(index)]) /
                            std::log1p(peak);
        const int py =
            area.bottom() - static_cast<int>(norm * static_cast<double>(area.h - 1) + 0.5);
        if (!started) {
          MoveToEx(dc, area.x + column, py, nullptr);
          started = true;
        } else {
          LineTo(dc, area.x + column, py);
        }
      }
      SelectObject(dc, old);
      DeleteObject(pen);
    }
    SetTextColor(dc, RGB(150, 154, 166));
    wchar_t histogram_label[96];
    if (comp_ui::histogram_view_is_full(view)) {
      wsprintfW(histogram_label, L"亮度直方图（对数刻度，双击复位）");
    } else {
      wsprintfW(histogram_label, L"直方图 %d–%d（双击复位）",
                static_cast<int>(view.lo + 0.5), static_cast<int>(view.hi + 0.5));
    }
    const comp_ui::Rect label_rect = comp_ui::histogram_label_rect(panel_rect);
    TextOutW(dc, label_rect.x, label_rect.y, histogram_label, lstrlenW(histogram_label));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hint = L"Del 删除 · Ctrl+J 复制 · F2 改名 · Ctrl+G 建组";
  TextOutW(dc, panel_rect.x + 10, panel_rect.bottom() - 46, hint, lstrlenW(hint));
  const wchar_t* hint2 = L"Ctrl 点选多选 · Shift+O 直设不透明度 · Ctrl+[ ] 步进";
  TextOutW(dc, panel_rect.x + 10, panel_rect.bottom() - 26, hint2, lstrlenW(hint2));
}

/* 通道条（色阶 / 曲线共用）：激活通道填蓝，其余深灰。 */
void draw_channel_chip(HDC dc, const comp_ui::Rect& rect, const wchar_t* name, bool active) {
  RECT box{rect.x, rect.y, rect.right(), rect.bottom()};
  HBRUSH brush = CreateSolidBrush(active ? RGB(66, 106, 176) : RGB(38, 42, 52));
  FillRect(dc, &box, brush);
  DeleteObject(brush);
  SetTextColor(dc, active ? RGB(255, 255, 255) : RGB(168, 174, 190));
  SIZE extent{};
  GetTextExtentPoint32W(dc, name, lstrlenW(name), &extent);
  TextOutW(dc, rect.x + (rect.w - extent.cx) / 2, rect.y + 1, name, lstrlenW(name));
}

/*
 * 曲线编辑器（P0-3）：通道条 + 正方形绘图区 + 折线 + 控制点。
 * 折线用 ui_core::curve_eval 采样，与引擎 Curve::apply 同一套 Hermite 公式——
 * 画出来的线和真实像素映射一致，否则用户会照着一条错的线调。
 */
void draw_curve_editor(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  for (int32_t c = 0; c < kCurveChannels; ++c) {
    draw_channel_chip(dc, comp_ui::curve_channel_chip_rect(panel_rect, c, kCurveChannels),
                      curve_channel_name(c), c == shell.doc.curve_channel);
  }
  const comp_ui::Rect plot = comp_ui::adjust_curve_plot_rect(panel_rect);
  if (plot.empty()) return;

  RECT box{plot.x, plot.y, plot.right(), plot.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(14, 16, 20));
  FillRect(dc, &box, background);
  DeleteObject(background);

  /* 四分网格 + 恒等参考线（虚线），值是 0..255 的线性刻度。 */
  HPEN grid = CreatePen(PS_SOLID, 1, RGB(42, 46, 56));
  HGDIOBJ old_pen = SelectObject(dc, grid);
  for (int i = 1; i < 4; ++i) {
    const int gx = plot.x + plot.w * i / 4;
    MoveToEx(dc, gx, plot.y, nullptr);
    LineTo(dc, gx, plot.bottom());
    const int gy = plot.y + plot.h * i / 4;
    MoveToEx(dc, plot.x, gy, nullptr);
    LineTo(dc, plot.right(), gy);
  }
  SelectObject(dc, old_pen);
  DeleteObject(grid);
  HPEN diagonal = CreatePen(PS_DOT, 1, RGB(74, 78, 90));
  old_pen = SelectObject(dc, diagonal);
  MoveToEx(dc, plot.x, plot.bottom() - 1, nullptr);
  LineTo(dc, plot.right() - 1, plot.y);
  SelectObject(dc, old_pen);
  DeleteObject(diagonal);

  /* 空通道按恒等画：用户看到的是「这条调整目前什么都不改」。 */
  static const double kIdentity[] = {0.0, 0.0, 255.0, 255.0};
  const int32_t count = static_cast<int32_t>(shell.doc.curve_xy.size() / 2);
  const double* points = count >= 2 ? shell.doc.curve_xy.data() : kIdentity;
  const int drawn = count >= 2 ? static_cast<int>(count) : 2;

  HPEN curve_pen = CreatePen(PS_SOLID, 2, RGB(120, 200, 255));
  old_pen = SelectObject(dc, curve_pen);
  const auto y_for = [&](double value) {
    return static_cast<int>(std::lround(comp_ui::curve_plot_value_to_y(plot, value)));
  };
  MoveToEx(dc, static_cast<int>(std::lround(comp_ui::curve_plot_value_to_x(plot, 0.0))),
           y_for(comp_ui::curve_eval(points, drawn, 0.0)), nullptr);
  for (int px = 1; px < plot.w; ++px) {
    const double value_x = comp_ui::curve_plot_x_to_value(plot, plot.x + px);
    LineTo(dc, plot.x + px, y_for(comp_ui::curve_eval(points, drawn, value_x)));
  }
  SelectObject(dc, old_pen);
  DeleteObject(curve_pen);

  /* 控制点：端点画空心（x 锁死），内部点实心；焦点/拖动中描黄边。 */
  for (int32_t i = 0; i < drawn; ++i) {
    const double px = comp_ui::curve_plot_value_to_x(plot, points[2 * i]);
    const double py = comp_ui::curve_plot_value_to_y(plot, points[2 * i + 1]);
    const int cx = static_cast<int>(std::lround(px));
    const int cy = static_cast<int>(std::lround(py));
    const bool endpoint = i == 0 || i == drawn - 1;
    const bool hot = i == shell.doc.curve_focus_point || i == shell.doc.curve_drag_point;
    RECT dot{cx - 3, cy - 3, cx + 4, cy + 4};
    HBRUSH fill = CreateSolidBrush(hot ? RGB(255, 226, 140)
                                       : (count >= 2 && endpoint ? RGB(150, 156, 172)
                                                                 : RGB(226, 232, 244)));
    FillRect(dc, &dot, fill);
    DeleteObject(fill);
  }

  wchar_t line[220];
  SetTextColor(dc, RGB(150, 154, 166));
  if (shell.doc.curve_focus_point >= 0 && count >= 2 &&
      shell.doc.curve_focus_point < static_cast<int32_t>(shell.doc.curve_xy.size() / 2)) {
    /* wsprintfW 只认整数宽度，%f 会把参数当 int 取、指针读飞（在 user32 里 AV）。 */
    const std::wstring point_x =
        widen_ascii(number_text(shell.doc.curve_xy[static_cast<size_t>(2 * shell.doc.curve_focus_point)])
                        .c_str());
    const std::wstring point_y =
        widen_ascii(number_text(shell.doc.curve_xy[static_cast<size_t>(2 * shell.doc.curve_focus_point + 1)])
                        .c_str());
    wsprintfW(line, L"%s · 点 %d/%d = (%s, %s)", curve_channel_name(shell.doc.curve_channel),
              shell.doc.curve_focus_point + 1, static_cast<int>(count), point_x.c_str(),
              point_y.c_str());
  } else {
    wsprintfW(line, L"%s · %d 个控制点%s", curve_channel_name(shell.doc.curve_channel),
              static_cast<int>(count), count < 2 ? L"（恒等）" : L"");
  }
  TextOutW(dc, plot.x, plot.bottom() + 4, line, lstrlenW(line));
  const wchar_t* help = L"点击加点 · 拖动改点 · Delete 删点";
  TextOutW(dc, plot.x, plot.bottom() + 20, help, lstrlenW(help));
}

/* 调整面板：控件完全由 ABI 元数据生成（外壳不复制键名表、不解析 JSON）。 */
void draw_adjust_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int height = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"调整（Adjustment）";
  TextOutW(dc, left + 10, y, title, lstrlenW(title));
  y += 22;

  const int32_t total = static_cast<int32_t>(COMP_ADJUSTMENT_KIND_COUNT);
  wsprintfW(line, L"类型 %d/%d：%s", shell.doc.adj_kind + 1, total,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(shell.doc.adj_kind))).c_str());
  SetTextColor(dc, RGB(150, 210, 255));
  TextOutW(dc, left + 10, y, line, lstrlenW(line));
  y += kAdjustRowHeight;

  /* 色阶通道条：标题行右侧，四个通道各一页（参数行几何因此不用变）。 */
  const bool paged = adjust_paged_kind(static_cast<uint32_t>(shell.doc.adj_kind));
  if (paged) {
    for (int32_t c = 0; c < kLevelsChannelCount; ++c) {
      draw_channel_chip(dc, comp_ui::adjust_channel_chip_rect(panel_rect, c, kLevelsChannelCount),
                        curve_channel_name(c), c == shell.doc.adj_channel);
    }
  }

  /* 面板显示的行数（分页视图下参数总数 != 显示行数）与「相对行 -> 参数下标」的基址。 */
  const int shown_total = adjust_shown_count(shell);
  const int base = adjust_param_base(shell);
  const int scroll = comp_ui::adjust_param_clamp_scroll(panel_rect, shown_total, shell.doc.adj_scroll);
  const int visible = comp_ui::adjust_param_visible_rows(panel_rect);
  if (shell.doc.adj_kind == COMP_ADJUSTMENT_CURVES) {
    /* 曲线没有标量参数：参数表位置让给绘图区。 */
    draw_curve_editor(dc, shell, panel_rect);
  } else if (shown_total == 0) {
    SetTextColor(dc, RGB(200, 170, 120));
    const wchar_t* note = L"（该类型无可面板编辑参数）";
    TextOutW(dc, left + 10, y, note, lstrlenW(note));
  } else {
    /*
     * P0-3：每个参数 = 上 18 px「标签 + 数值」+ 下 16 px 滑道。行几何来自
     * ui_core::adjust_param_row_rect，命中测试用的是同一个函数——绘制与命中同源。
     * 超出可视区的行不画；滚动量在这里夹一次，长表（色阶 20 行）不会画到提示区。
     */
    for (int row = scroll; row < shown_total && row < scroll + visible; ++row) {
      const int index = base + row; /* 相对行 -> 参数表绝对下标 */
      comp_adjustment_param param{};
      if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.doc.adj_kind),
                                         static_cast<size_t>(index), &param) != COMP_OK) {
        break;
      }
      const comp_ui::Rect row_rect = comp_ui::adjust_param_row_rect(panel_rect, row, scroll);
      const bool focused = index == shell.doc.adj_param;
      if (focused) {
        RECT highlight{row_rect.x + 4, row_rect.y - 1, row_rect.right() - 6,
                       row_rect.bottom() - 1};
        HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
        FillRect(dc, &highlight, brush);
        DeleteObject(brush);
      }
      const std::string value = static_cast<size_t>(index) < shell.doc.adj_values.size()
                                    ? shell.doc.adj_values[static_cast<size_t>(index)]
                                    : std::string("?");
      std::wstring shown;
      if (focused && !shell.doc.adj_input.empty()) {
        shown = L"输入中：";
        shown += shell.doc.adj_input;
      } else if (param.type == COMP_PARAM_BOOL) {
        shown = value == "1" ? L"true" : L"false";
      } else {
        shown = widen_ascii(value.c_str());
      }
      const std::wstring label = widen_ascii(param.label != nullptr ? param.label : "");
      SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
      TextOutW(dc, row_rect.x + 12, row_rect.y + 1, label.c_str(), lstrlenW(label.c_str()));
      /* 数值右对齐：滑道两端和数字对齐，拖动时眼睛不用在行里找。 */
      SIZE extent{};
      GetTextExtentPoint32W(dc, shown.c_str(), lstrlenW(shown.c_str()), &extent);
      TextOutW(dc, row_rect.right() - 12 - extent.cx, row_rect.y + 1, shown.c_str(),
               lstrlenW(shown.c_str()));
      if (param.type == COMP_PARAM_BOOL) continue;

      const comp_ui::Rect track = comp_ui::adjust_slider_track_rect(row_rect);
      const int mid = track.y + track.h / 2;
      HBRUSH groove = CreateSolidBrush(RGB(44, 48, 58));
      RECT groove_rect{track.x, mid - 2, track.right(), mid + 2};
      FillRect(dc, &groove_rect, groove);
      DeleteObject(groove);

      const double number = std::atof(value.c_str());
      const int knob_x = static_cast<int>(std::lround(comp_ui::adjust_slider_knob_x(
          track, number, param.min_value, param.max_value)));
      if (knob_x > track.x) {
        HBRUSH filled = CreateSolidBrush(RGB(78, 126, 208));
        RECT fill_rect{track.x, mid - 2, knob_x, mid + 2};
        FillRect(dc, &fill_rect, filled);
        DeleteObject(filled);
      }
      HBRUSH knob = CreateSolidBrush(focused ? RGB(255, 240, 190) : RGB(214, 220, 232));
      RECT knob_rect{knob_x - 3, track.y + 2, knob_x + 4, track.bottom() - 2};
      FillRect(dc, &knob_rect, knob);
      DeleteObject(knob);
    }
    /* 长表/分页滚动位置：只在小窗口 / 每页放不下 5 行时出现。 */
    if (comp_ui::adjust_param_max_scroll(panel_rect, shown_total) > 0) {
      const int last_row = scroll + visible < shown_total ? scroll + visible : shown_total;
      wsprintfW(line, L"%d-%d / %d", scroll + 1, last_row, shown_total);
      SetTextColor(dc, RGB(150, 154, 166));
      SIZE extent{};
      GetTextExtentPoint32W(dc, line, lstrlenW(line), &extent);
      TextOutW(dc, panel_rect.right() - 12 - extent.cx, panel_rect.y + 30, line,
               lstrlenW(line));
    }
  }

  /*
   * 底部固定区（高度 = kAdjustPanelFooter）：上「已有调整」列表、下快捷键提示。
   * 固定而不是跟着参数表流下来——参数表会滚，提示不该跟着滚出窗口。
   */
  const int footer_top = height - comp_ui::kAdjustPanelFooter;
  int footer_y = footer_top + 2;
  SetTextColor(dc, RGB(180, 184, 196));
  wsprintfW(line, L"该图层已有调整：%u", static_cast<unsigned>(shell.doc.adj_settings.size()));
  TextOutW(dc, left + 10, footer_y, line, lstrlenW(line));
  footer_y += 18;
  for (size_t i = 0; i < shell.doc.adj_settings.size() && i < 3; ++i) {
    const bool selected = static_cast<int32_t>(i) == shell.doc.selected_adjustment;
    SetTextColor(dc, selected ? RGB(255, 226, 140) : RGB(196, 198, 208));
    wsprintfW(line, L"%s %u: %s", selected ? L">" : L" ", static_cast<unsigned>(i + 1),
              widen_ascii(adjustment_kind_name(shell.doc.adj_kinds[i])).c_str());
    TextOutW(dc, left + 12, footer_y, line, lstrlenW(line));
    footer_y += 18;
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* curve_hints[] = {
      L"Ctrl+Shift+K/J 切换类型 · , / . 选调整",
      L"通道条：RGB / R / G / B（点控制点切换）",
      L"点击空白加点 · 拖动改点 · Delete 删点",
      L"Ctrl+Shift+A 新增曲线 · Ctrl+Shift+U 应用",
      L"Ctrl+Shift+D 删除选中调整 · Esc 取消拖动",
  };
  const wchar_t* slider_hints[] = {
      L"Ctrl+Shift+K/J 切换类型 · , / . 选调整",
      L"Tab 切参数 · 拖动滑杆实时预览",
      L"数字键输入 · 回车提交 · - / = 调值",
      L"色阶：顶部 RGB/R/G/B 切通道页",
      L"Ctrl+Shift+A 新增 · Ctrl+Shift+U 应用 · Ctrl+Shift+D 删除",
  };
  const wchar_t* const* hints =
      shell.doc.adj_kind == COMP_ADJUSTMENT_CURVES ? curve_hints : slider_hints;
  int hint_y = height - 96;
  for (int i = 0; i < 5; ++i) {
    TextOutW(dc, left + 10, hint_y, hints[i], lstrlenW(hints[i]));
    hint_y += 18;
  }
}

/*
 * 修边面板（P1 图像菜单）。行几何来自 ui_core.h 的 trim_row_rect——绘制与
 * 命中同源，避免「看得见点不中」。
 */
void draw_trim_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"修边（Trim）";
  TextOutW(dc, left + 10, y, title, lstrlenW(title));
  y += 22;
  SetTextColor(dc, RGB(150, 210, 255));
  const wchar_t* subtitle = L"按透明像素 / 边缘色裁掉多余边框";
  TextOutW(dc, left + 10, y, subtitle, lstrlenW(subtitle));

  for (int row = 0; row < shell.doc.trim.row_count(); ++row) {
    const comp_ui::Rect row_rect = comp_ui::trim_row_rect(panel_rect, row);
    const bool focused = row == shell.doc.trim.focused;
    if (focused) {
      RECT highlight{row_rect.x + 4, row_rect.y - 2, row_rect.right() - 6,
                     row_rect.bottom() + 2};
      HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, brush);
      DeleteObject(brush);
    }
    SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
    const std::wstring label = widen_ascii(comp_ui::trim_row_label(row));
    if (row == 0) {
      wsprintfW(line, L"%s = %s", label.c_str(),
                widen_ascii(comp_ui::trim_based_on_name(shell.doc.trim.based_on)).c_str());
    } else if (row >= 1 && row <= 4) {
      wsprintfW(line, L"%s = %s", label.c_str(),
                comp_ui::trim_edge_text(shell.doc.trim.edge[row - 1]));
    } else {
      wsprintfW(line, L"%s = %d", label.c_str(), shell.doc.trim.tolerance);
    }
    TextOutW(dc, left + 12, row_rect.y, line, lstrlenW(line));
  }

  if (!shell.doc.trim_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.doc.trim_input.c_str());
    TextOutW(dc, left + 10, bottom - 130, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"Tab / Shift+Tab 切行",
      L"- / = 调值 · 数字键输入容差",
      L"回车应用（一步撤销）· Esc 关闭",
      L"基于颜色时容差是逐通道差（0..255）",
  };
  int hint_y = bottom - 104;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 文档级对话框面板（新建 / 图像大小 / 画布大小 / 历史容量）。行几何与命中同源
 * （ui_core::doc_dialog_row_rect / doc_dialog_row_at），值文本与状态栏同源
 * （doc_dialog_value_text），所以「面板显示什么」只有一处真相。
 */
void draw_doc_dialog(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const std::wstring title = widen_utf8(comp_ui::doc_dialog_title(shell.doc.doc_dialog));
  TextOutW(dc, left + 10, y, title.c_str(), static_cast<int>(title.size()));
  y += 22;
  SetTextColor(dc, RGB(150, 210, 255));
  const std::wstring subtitle = widen_utf8(comp_ui::doc_dialog_subtitle(shell.doc.doc_dialog));
  TextOutW(dc, left + 10, y, subtitle.c_str(), static_cast<int>(subtitle.size()));

  const int row_count = comp_ui::doc_dialog_row_count(shell.doc.doc_dialog);
  for (int row = 0; row < row_count; ++row) {
    const comp_ui::Rect row_rect = comp_ui::doc_dialog_row_rect(panel_rect, row);
    const bool focused = row == shell.doc.doc_dialog_focus;
    if (focused) {
      RECT highlight{row_rect.x + 4, row_rect.y - 2, row_rect.right() - 6,
                     row_rect.bottom() + 2};
      HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, brush);
      DeleteObject(brush);
    }
    SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
    const std::wstring label =
        widen_utf8(comp_ui::doc_dialog_row(shell.doc.doc_dialog, row).label);
    /* 键入态只在聚焦行显示草稿，别的行仍显示已生效的值（-/= 改的是已生效值）。 */
    if (focused && !shell.doc.doc_dialog_input.empty()) {
      wsprintfW(line, L"%s = %s_", label.c_str(), shell.doc.doc_dialog_input.c_str());
    } else {
      wsprintfW(line, L"%s = %s", label.c_str(),
                doc_dialog_value_text(shell, row).c_str());
    }
    TextOutW(dc, left + 12, row_rect.y, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const std::wstring hint = widen_utf8(comp_ui::doc_dialog_hint(shell.doc.doc_dialog));
  int hint_y = bottom - 86;
  TextOutW(dc, left + 10, hint_y, hint.c_str(), static_cast<int>(hint.size()));
  hint_y += 18;
  /*
   * 固定键位提示分两套：关闭提示没有数值可填，照抄「数字键输入 · - / = 步进」
   * 会误导用户去按键；其它对话框的三行保持原样（像素与文字都不变）。
   */
  const bool close_prompt = shell.doc.doc_dialog == comp_ui::DocDialogKind::ClosePrompt;
  const wchar_t* keys[] = {
      close_prompt ? L"S 保存并关闭 · D 不保存" : L"Tab / Shift+Tab 切行 · 方向键切行",
      close_prompt ? L"回车执行当前动作" : L"- / = 步进 · 数字键直接输入",
      close_prompt ? L"Esc 取消（留在文档）" : L"回车应用 · Esc 取消",
  };
  for (const wchar_t* key : keys) {
    TextOutW(dc, left + 10, hint_y, key, lstrlenW(key));
    hint_y += 18;
  }
}

/*
 * 滤镜面板。行几何来自 ui_core.h 的 filter_param_row_rect——绘制与命中同源，
 * 避免「看得见点不中」。
 */
void draw_filter_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"滤镜（Filter）";
  TextOutW(dc, left + 10, y, title, lstrlenW(title));
  y += 22;

  const comp_ui::FilterSpec* spec = shell.doc.filter.spec();
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"%d/%d：%s", static_cast<int>(shell.doc.filter.id) + 1,
            comp_ui::filter_count(), spec != nullptr ? widen_ascii(spec->name).c_str() : L"?");
  TextOutW(dc, left + 10, y, line, lstrlenW(line));

  for (int row = 0; row < shell.doc.filter.param_count(); ++row) {
    const comp_ui::FilterParamSpec* parameter = shell.doc.filter.param_at(row);
    if (parameter == nullptr) break;
    const comp_ui::Rect row_rect = comp_ui::filter_param_row_rect(panel_rect, row);
    const bool focused = row == shell.doc.filter.focused;
    if (focused) {
      RECT highlight{row_rect.x + 4, row_rect.y - 2, row_rect.right() - 6,
                     row_rect.bottom() + 2};
      HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, brush);
      DeleteObject(brush);
    }
    SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
    wsprintfW(line, L"%s = %s", widen_ascii(parameter->label).c_str(),
              widen_ascii(number_text(shell.doc.filter.value(row)).c_str()).c_str());
    TextOutW(dc, left + 12, row_rect.y, line, lstrlenW(line));
  }

  if (!shell.doc.filter_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.doc.filter_input.c_str());
    TextOutW(dc, left + 10, bottom - 130, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"Tab / Shift+Tab 切参数",
      L"- / = 调值 · 数字键输入",
      L"回车应用 · Esc 关闭",
      L"Amount = 0 时滤镜为恒等（不改像素）",
  };
  int hint_y = bottom - 104;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 效果参数面板（P0-2）。行几何来自 ui_core.h 的 effect_param_row_rect——绘制与
 * 命中同源，避免「看得见点不中」。参数名/量程展示全部来自 ABI 参数表。
 */
void draw_effect_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"效果参数（Effect）";
  TextOutW(dc, left + 10, y, title, lstrlenW(title));
  y += 22;

  const int kind = comp_ui::clamp_effect_kind(shell.doc.effect_kind);
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"%d/%d：%s · %s", kind + 1, comp_ui::kEffectKindCount,
            effect_kind_label(kind),
            effect_present(shell, kind) ? L"本层已启用" : L"本层未启用（回车添加）");
  TextOutW(dc, left + 10, y, line, lstrlenW(line));

  const uint32_t effect = static_cast<uint32_t>(kind);
  const int param_count = static_cast<int>(shell.doc.effect_values.size());
  if (param_count == 0) {
    SetTextColor(dc, RGB(200, 170, 120));
    const wchar_t* note = L"（该效果没有可面板编辑的参数）";
    TextOutW(dc, left + 10, panel_rect.y + comp_ui::kEffectRowsTop, note, lstrlenW(note));
  }
  int rows_bottom = panel_rect.y + comp_ui::kEffectRowsTop;
  for (int row = 0; row < param_count; ++row) {
    comp_effect_param param{};
    if (comp_layer_effect_param_at(effect, static_cast<size_t>(row), &param) != COMP_OK) break;
    const comp_ui::Rect row_rect = comp_ui::effect_param_row_rect(panel_rect, row);
    const bool focused = row == shell.doc.effect_param;
    if (focused) {
      RECT highlight{row_rect.x + 4, row_rect.y - 2, row_rect.right() - 6, row_rect.bottom() + 2};
      HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, brush);
      DeleteObject(brush);
    }
    SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
    const std::string value = shell.doc.effect_values[static_cast<size_t>(row)];
    if (param.type == COMP_PARAM_BOOL) {
      wsprintfW(line, L"%s = %s", widen_ascii(param.label != nullptr ? param.label : "").c_str(),
                value == "1" ? L"true" : L"false");
    } else {
      wsprintfW(line, L"%s = %s", widen_ascii(param.label != nullptr ? param.label : "").c_str(),
                widen_ascii(value.c_str()).c_str());
    }
    TextOutW(dc, left + 12, row_rect.y, line, lstrlenW(line));
    rows_bottom = row_rect.bottom();
  }

  /* 参数来源：新增 = 引擎缺省；已有 = 在该层既有 settings 上合并（未知键保留）。 */
  int note_y = rows_bottom + 8;
  SetTextColor(dc, RGB(180, 184, 196));
  const wchar_t* source = effect_present(shell, kind)
                              ? L"在既有 settings 上合并（未知键原样保留）"
                              : L"新增效果（未给出的键走引擎缺省）";
  TextOutW(dc, left + 10, note_y, source, lstrlenW(source));
  note_y += 18;
  wsprintfW(line, L"该图层已有有效果：%u", static_cast<unsigned>(shell.doc.effect_kinds.size()));
  TextOutW(dc, left + 10, note_y, line, lstrlenW(line));

  if (!shell.doc.effect_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.doc.effect_input.c_str());
    TextOutW(dc, left + 10, bottom - 136, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L", / . 切换效果类型",
      L"Tab / Shift+Tab 切参数",
      L"- / = 调值 · 数字键输入",
      L"回车应用（按 kind 替换/新增）",
      L"Backspace 重置为默认值",
      L"Esc 关闭面板",
  };
  int hint_y = bottom - 120;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 色彩范围面板（P0-4 第二批）。排版与命中都走 ui_core.h 的
 * color_range_swatch_rect / color_range_row_rect——尤其是色块，
 * 画在哪里就必须点在哪里（点掉一个取错的颜色是本面板唯一的纠正入口）。
 */
void draw_color_range_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[288];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"色彩范围（Color Range）";
  TextOutW(dc, left + 10, y, title, lstrlenW(title));

  const int include_count = static_cast<int>(shell.doc.color_range_include.size() / 3);
  const int exclude_count = static_cast<int>(shell.doc.color_range_exclude.size() / 3);
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"取色 %d · 排除 %d（画布点选：Shift 加色 · Alt 减色）", include_count,
            exclude_count);
  TextOutW(dc, left + 10, panel_rect.y + 30, line, lstrlenW(line));

  /* 色块：首屏放得下的才画（超出宽度的颜色仍生效，只是这里不显示/不可点）。 */
  const int capacity = comp_ui::color_range_swatch_capacity(panel_rect);
  const int shown_include = include_count < capacity ? include_count : capacity;
  const int shown_exclude = exclude_count < capacity ? exclude_count : capacity;
  if (capacity > 0) {
    HBRUSH border = CreateSolidBrush(RGB(110, 114, 126));
    for (int i = 0; i < shown_include; ++i) {
      const comp_ui::Rect rect = comp_ui::color_range_swatch_rect(panel_rect, false, i);
      RECT swatch{rect.x, rect.y, rect.right(), rect.bottom()};
      const uint8_t* color = shell.doc.color_range_include.data() + static_cast<size_t>(i) * 3;
      HBRUSH brush = CreateSolidBrush(RGB(color[0], color[1], color[2]));
      FillRect(dc, &swatch, brush);
      DeleteObject(brush);
      FrameRect(dc, &swatch, border);
    }
    for (int i = 0; i < shown_exclude; ++i) {
      const comp_ui::Rect rect = comp_ui::color_range_swatch_rect(panel_rect, true, i);
      RECT swatch{rect.x, rect.y, rect.right(), rect.bottom()};
      const uint8_t* color = shell.doc.color_range_exclude.data() + static_cast<size_t>(i) * 3;
      HBRUSH brush = CreateSolidBrush(RGB(color[0], color[1], color[2]));
      FillRect(dc, &swatch, brush);
      DeleteObject(brush);
      FrameRect(dc, &swatch, border);
      /* 排除色画一道斜杠，避免与取色行同色时看不出来。 */
      HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 100, 100));
      HGDIOBJ old_pen = SelectObject(dc, pen);
      MoveToEx(dc, rect.x + 3, rect.y + 3, nullptr);
      LineTo(dc, rect.right() - 3, rect.bottom() - 3);
      SelectObject(dc, old_pen);
      DeleteObject(pen);
    }
    DeleteObject(border);
    if (include_count == 0) {
      SetTextColor(dc, RGB(170, 150, 110));
      const wchar_t* empty = L"还没有取过色：点画布任意位置开始";
      TextOutW(dc, left + 12, panel_rect.y + comp_ui::kColorRangeIncludeTop + 2, empty,
               lstrlenW(empty));
    }
  }

  const int fuzziness_y = panel_rect.y + comp_ui::kColorRangeRowsTop;
  SetTextColor(dc, RGB(206, 208, 216));
  wsprintfW(line, L"容差 Fuzziness = %d  (%d..%d)", shell.doc.color_range_fuzziness,
            comp_ui::kMinColorRangeFuzziness, comp_ui::kMaxColorRangeFuzziness);
  TextOutW(dc, left + 12, fuzziness_y, line, lstrlenW(line));
  wsprintfW(line, L"反相 Invert = %s  (I 切换)", shell.doc.color_range_invert ? L"开" : L"关");
  TextOutW(dc, left + 12, fuzziness_y + comp_ui::kColorRangeRowHeight, line, lstrlenW(line));

  SetTextColor(dc, RGB(180, 184, 196));
  const wchar_t* note = shell.doc.color_range_txn != nullptr
                            ? L"预览中：选区随取样/容差实时变化，回车才落一步撤销"
                            : L"（没有预览事务：改动会直接落盘）";
  TextOutW(dc, left + 12, fuzziness_y + comp_ui::kColorRangeRowHeight * 2 + 4, note,
           lstrlenW(note));

  if (!shell.doc.color_range_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.doc.color_range_input.c_str());
    TextOutW(dc, left + 10, bottom - 124, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"画布点选 = 取色（替换）",
      L"Shift+点 = 加色 · Alt+点 = 排除",
      L"点色块 = 移除该取样色",
      L"- / = 调容差 · 数字键输入",
      L"I 切换反相 · Backspace 清空取样",
      L"回车应用（一步撤销）· Esc 取消",
  };
  int hint_y = bottom - 120;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 路径面板（P0-4 可选）。行几何走 ui_core.h 的 path_row_rect / path_row_at：
 * 画在哪一行，双击就命中哪一行——这是面板唯一的手势入口。
 */
void draw_path_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"路径（Paths）";
  TextOutW(dc, left + 10, panel_rect.y + 8, title, lstrlenW(title));

  const int count = static_cast<int>(shell.doc.paths.size());
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"已存 %d / %d 条（钢笔闭合提交后自动入列）", count,
            comp_ui::kPathMaxEntries);
  TextOutW(dc, left + 10, panel_rect.y + 30, line, lstrlenW(line));

  const int scroll = comp_ui::path_clamp_scroll(panel_rect, count, shell.doc.path_scroll);
  const int visible = comp_ui::path_visible_rows(panel_rect);
  if (count == 0) {
    SetTextColor(dc, RGB(170, 150, 110));
    const wchar_t* empty = L"还没有路径：用钢笔（P）画一条闭合路径";
    TextOutW(dc, left + 12, panel_rect.y + comp_ui::kPathRowsTop, empty, lstrlenW(empty));
  }
  for (int row = 0; row < visible; ++row) {
    const int index = scroll + row;
    if (index >= count) break;
    const comp_ui::Rect rect = comp_ui::path_row_rect(panel_rect, row);
    const bool selected = index == shell.doc.path_selected;
    if (selected) {
      RECT band{rect.x, rect.y, rect.right(), rect.bottom()};
      HBRUSH highlight = CreateSolidBrush(RGB(48, 62, 86));
      FillRect(dc, &band, highlight);
      DeleteObject(highlight);
    }
    const StoredPath& path = shell.doc.paths[static_cast<size_t>(index)];
    SetTextColor(dc, selected ? RGB(250, 250, 255) : RGB(196, 200, 210));
    wsprintfW(line, L"%d. %s  ·  %d 个锚点", index + 1, path.name.c_str(),
              static_cast<int>(path.anchors.size()));
    TextOutW(dc, left + 12, rect.y + 3, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"↑ / ↓ 选择 · 回车转成选区",
      L"双击行转成选区（一步撤销）",
      L"Del 删除该条（选区不动）· Esc 关闭",
  };
  int hint_y = bottom - 8 - 3 * 18 + 4;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 历史记录面板（P1）。行几何走 ui_core.h 的 history_row_rect / history_row_at，
 * 与命中同源。当前状态（= 引擎 current_index）高亮；它之后的行是「可重做」的
 * 未来状态，用暗色区分。标签直接来自 ABI，不在这里改写成中文。
 */
void draw_history_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[288];
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"历史（History）";
  TextOutW(dc, left + 10, panel_rect.y + 8, title, lstrlenW(title));

  const int count = history_state_count(shell);
  const int current = history_current(shell);
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"%d 个状态 · 当前第 %d 个（点行 / ↑↓ 回跳）", count, current + 1);
  TextOutW(dc, left + 10, panel_rect.y + 30, line, lstrlenW(line));

  const int scroll = comp_ui::history_clamp_scroll(panel_rect, count, shell.doc.history_scroll);
  const int visible = comp_ui::history_visible_rows(panel_rect);
  for (int row = 0; row < visible; ++row) {
    const int index = scroll + row;
    if (index >= count) break;
    const comp_ui::Rect rect = comp_ui::history_row_rect(panel_rect, row);
    const bool selected = index == current;
    /* 显式取样源（Shift+点行）单独标出来：这支笔画什么完全取决于它。 */
    const bool sampled = shell.doc.history_brush_state >= 0 && index == shell.doc.history_brush_state;
    if (selected) {
      RECT band{rect.x, rect.y, rect.right(), rect.bottom()};
      HBRUSH highlight = CreateSolidBrush(RGB(48, 62, 86));
      FillRect(dc, &band, highlight);
      DeleteObject(highlight);
    }
    if (sampled) {
      SetTextColor(dc, RGB(255, 206, 112));
    } else if (selected) {
      SetTextColor(dc, RGB(250, 250, 255));
    } else if (index > current) {
      /* 当前状态之后 = 重做侧：暗色，一眼看出「还没发生」。 */
      SetTextColor(dc, RGB(130, 136, 148));
    } else {
      SetTextColor(dc, RGB(196, 200, 210));
    }
    const std::wstring label = history_state_label(shell, index);
    if (sampled) {
      wsprintfW(line, L"%d. %s   ← 画笔取样", index, label.c_str());
    } else {
      wsprintfW(line, L"%d. %s", index, label.c_str());
    }
    TextOutW(dc, left + 12, rect.y + 3, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"点行 / ↑ / ↓ 回跳（不新增撤销步）",
      L"Shift+点行 = 只指定历史记录画笔的取样源（不回跳）",
      L"Ctrl+Z / Ctrl+Y 仍按步撤销 / 重做",
      L"Escape 关闭面板",
  };
  const int hint_count = static_cast<int>(sizeof(hints) / sizeof(hints[0]));
  int hint_y = bottom - 8 - hint_count * 18 + 4;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 文字属性面板。行几何来自 ui_core.h 的 type_param_row_rect——绘制与命中同源，
 * 避免「看得见点不中」。面板改的是「下一段文字」的参数；键入态仍照常显示，
 * 因为键入中的文字也读同一组值（状态栏数字与预览框会立刻跟着变）。
 */
void draw_type_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"文字（Type）";
  TextOutW(dc, left + 10, y, title, lstrlenW(title));
  y += 22;

  SetTextColor(dc, RGB(150, 210, 255));
  const wchar_t* sub = shell.doc.type_active ? L"键入中（回车提交 · Esc 取消）" : L"属性写入下一段文字";
  TextOutW(dc, left + 10, y, sub, lstrlenW(sub));

  for (int row = 0; row < comp_ui::kTypePanelRowCount; ++row) {
    const comp_ui::Rect row_rect = comp_ui::type_param_row_rect(panel_rect, row);
    const bool focused = row == shell.type_panel_focused;
    if (focused) {
      RECT highlight{row_rect.x + 4, row_rect.y - 2, row_rect.right() - 6,
                     row_rect.bottom() + 2};
      HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, brush);
      DeleteObject(brush);
    }
    SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
    wsprintfW(line, L"%s = %s", widen_ascii(comp_ui::type_panel_row_label(row)).c_str(),
              type_panel_value_text(shell, row).c_str());
    TextOutW(dc, left + 12, row_rect.y, line, lstrlenW(line));
  }

  /* 文本框度量：点阵轨道与引擎 build_layout 同公式，字体轨道是 em 估计值。 */
  const comp_ui::TypeBlockMetrics metrics = type_metrics(shell);
  SetTextColor(dc, RGB(170, 174, 186));
  y = comp_ui::type_param_row_rect(panel_rect, comp_ui::kTypePanelRowCount - 1).bottom() + 8;
  if (type_font_track(shell)) {
    wsprintfW(line, L"文本框 ≈%d×%d px｜%d 行｜em 高（宽度是估计值）",
              static_cast<int>(std::lround(metrics.width)),
              static_cast<int>(std::lround(metrics.height)), metrics.line_count);
  } else {
    wsprintfW(line, L"文本框 %d×%d px｜%d 行｜步距 %d px",
              static_cast<int>(std::lround(metrics.width)),
              static_cast<int>(std::lround(metrics.height)), metrics.line_count,
              static_cast<int>(std::lround(metrics.advance)));
  }
  TextOutW(dc, left + 10, y, line, lstrlenW(line));

  if (!shell.doc.type_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.doc.type_input.c_str());
    TextOutW(dc, left + 10, bottom - 130, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"Tab / Shift+Tab 切参数",
      L"- / = 调值 · 数字键输入",
      L"Alt+[ / ] 调行距（键入中也可用）",
      L"点画布落插入点 · 回车提交 · Esc 取消",
  };
  int hint_y = bottom - 104;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 字体面板（P0-6②）。行几何走 ui_core.h 的 font_row_rect / font_row_at：
 * 画在哪一行，点中就应用哪一行。扫描可能是分片的，顶部实时给进度；「当前
 * 轨道」那一行用 ▲ 标出——用户不需要凭记忆回想上一支字体叫什么。
 */
void draw_font_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[320];
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"字体（Fonts）";
  TextOutW(dc, left + 10, panel_rect.y + 8, title, lstrlenW(title));

  const int count = font_row_count(shell);
  SetTextColor(dc, RGB(150, 210, 255));
  if (!shell.font_scan_done) {
    wsprintfW(line, L"%d 支可用（含内置）· 扫描中 %d/%d", count,
              static_cast<int>(shell.font_scan_cursor),
              static_cast<int>(shell.font_scan_queue.size()));
  } else if (shell.font_scan_skipped > 0) {
    wsprintfW(line, L"%d 支可用（含内置）· %d 个文件不可用", count, shell.font_scan_skipped);
  } else {
    wsprintfW(line, L"%d 支可用（含内置 5×7 点阵）", count);
  }
  TextOutW(dc, left + 10, panel_rect.y + 30, line, lstrlenW(line));

  /* 过滤行：空的时候给用法，非空时把词本身显示出来（Backspace 可删）。 */
  SetTextColor(dc, shell.font_filter.empty() ? RGB(140, 144, 156) : RGB(255, 236, 150));
  if (shell.font_filter.empty()) {
    const wchar_t* filter_hint = L"过滤：直接键入字母或中文";
    TextOutW(dc, left + 10, panel_rect.y + 54, filter_hint, lstrlenW(filter_hint));
  } else {
    wsprintfW(line, L"过滤：%s｜Backspace 删字符", shell.font_filter.c_str());
    TextOutW(dc, left + 10, panel_rect.y + 54, line, lstrlenW(line));
  }

  const int scroll = comp_ui::font_clamp_scroll(panel_rect, count, shell.font_scroll);
  const int visible = comp_ui::font_visible_rows(panel_rect);
  if (count == 0) {
    SetTextColor(dc, RGB(170, 150, 110));
    const wchar_t* empty = L"没有匹配的字体（Backspace 删过滤词）";
    TextOutW(dc, left + 12, panel_rect.y + comp_ui::kFontRowsTop, empty, lstrlenW(empty));
  }
  for (int row = 0; row < visible; ++row) {
    const int index = scroll + row;
    if (index >= count) break;
    const ShellFontEntry* entry = font_entry_at_row(shell, index);
    if (entry == nullptr) break;
    const comp_ui::Rect rect = comp_ui::font_row_rect(panel_rect, row);
    if (index == shell.font_selected) {
      RECT band{rect.x, rect.y, rect.right(), rect.bottom()};
      HBRUSH highlight = CreateSolidBrush(RGB(48, 62, 86));
      FillRect(dc, &band, highlight);
      DeleteObject(highlight);
    }
    const bool current =
        entry->builtin
            ? !type_font_track(shell)
            : (type_font_track(shell) && entry->path == shell.type_font_path &&
               entry->face_index == shell.type_face_index);
    SetTextColor(dc, index == shell.font_selected ? RGB(250, 250, 255) : RGB(196, 200, 210));
    wsprintfW(line, L"%s%s%s", current ? L"▲ " : L"   ", entry->label.c_str(),
              entry->cjk ? L"  [CJK]" : L"");
    RECT text_box{rect.x + 8, rect.y, rect.right() - 6, rect.bottom()};
    DrawTextW(dc, line, -1, &text_box,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"↑ / ↓ 选择 · Enter 或点击应用",
      L"键入字母或中文过滤 · Backspace 删字符",
      L"字号不换算（em 高 ↔ 点阵格高）",
      L"Esc 先清过滤词，再按一次关闭",
  };
  int hint_y = bottom - 8 - 4 * 18 + 4;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 画布叠加层（选区蚂蚁线 / 框选矩形 / 笔迹预览 / 笔尖轮廓）。
 *
 * 只画在画布矩形内：先把轴对齐线段裁到画布，再生成虚线段，避免 32 倍缩放
 * 时一条边生成上万个屏幕虚线段。几何与分段规则全部来自 ui_core.h，这里只做
 * 「文档坐标 -> 屏幕坐标 -> GDI 调用」的搬运。
 */
bool clip_axis_aligned(const comp_ui::Rect& canvas, comp_ui::Point* a, comp_ui::Point* b) {
  const double tolerance = 1e-6;
  if (std::fabs(a->y - b->y) <= tolerance) { /* 水平线 */
    const double y = a->y;
    if (y < static_cast<double>(canvas.y) || y >= static_cast<double>(canvas.bottom())) {
      return false;
    }
    double x0 = std::min(a->x, b->x);
    double x1 = std::max(a->x, b->x);
    x0 = std::max(x0, static_cast<double>(canvas.x));
    x1 = std::min(x1, static_cast<double>(canvas.right()));
    if (x1 <= x0) return false;
    *a = comp_ui::Point{x0, y};
    *b = comp_ui::Point{x1, y};
    return true;
  }
  if (std::fabs(a->x - b->x) <= tolerance) { /* 垂直线 */
    const double x = a->x;
    if (x < static_cast<double>(canvas.x) || x >= static_cast<double>(canvas.right())) {
      return false;
    }
    double y0 = std::min(a->y, b->y);
    double y1 = std::max(a->y, b->y);
    y0 = std::max(y0, static_cast<double>(canvas.y));
    y1 = std::min(y1, static_cast<double>(canvas.bottom()));
    if (y1 <= y0) return false;
    *a = comp_ui::Point{x, y0};
    *b = comp_ui::Point{x, y1};
    return true;
  }
  return true; /* 非轴对齐线段（当前不产生）原样返回，不做错误裁剪 */
}

/*
 * §6.66 变换框：蓝色实线框 + 8 个白底蓝边方块 + 上边外侧的旋转旋钮（带引线）。
 * 手柄大小固定为屏幕像素，缩放画布时不会跟着变粗变细；位置由 ui_core 的
 * 纯几何算出（与命中、缩放用的是同一组函数，画在哪就点在哪）。只在
 * transform_frame_for 认定可用时绘制。
 */
void draw_transform_frame(HDC dc, const ShellState& shell, const Layout& layout) {
  comp_rect rect{};
  float rotation = 0.0f;
  if (!transform_frame_for(shell, &rect, &rotation)) return;
  const comp_ui::Rect frame = to_ui_rect(rect);
  const double zoom = shell.doc.view.zoom > 0.0 ? shell.doc.view.zoom : 1.0;

  const int corners[4] = {0, 2, 4, 6}; /* 左上、右上、右下、左下 */
  POINT quad[5];
  for (int i = 0; i < 4; ++i) {
    const comp_ui::Point doc = comp_ui::transform_handle_point(frame, rotation, corners[i]);
    const comp_ui::Point screen =
        shell.doc.view.doc_to_screen(doc.x, doc.y, layout.canvas, shell.doc.width, shell.doc.height);
    quad[i].x = static_cast<LONG>(std::lround(screen.x));
    quad[i].y = static_cast<LONG>(std::lround(screen.y));
  }
  quad[4] = quad[0];

  HPEN frame_pen = CreatePen(PS_SOLID, 1, RGB(0, 120, 215));
  HBRUSH handle_brush = CreateSolidBrush(RGB(255, 255, 255));
  HGDIOBJ old_pen = SelectObject(dc, frame_pen);
  HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Polyline(dc, quad, 5);

  SelectObject(dc, handle_brush);
  for (int handle = 0; handle < comp_ui::kTransformHandleCount; ++handle) {
    const comp_ui::Point doc = comp_ui::transform_handle_point(frame, rotation, handle);
    const comp_ui::Point screen =
        shell.doc.view.doc_to_screen(doc.x, doc.y, layout.canvas, shell.doc.width, shell.doc.height);
    const int hx = static_cast<int>(std::lround(screen.x));
    const int hy = static_cast<int>(std::lround(screen.y));
    Rectangle(dc, hx - kTransformHandlePx, hy - kTransformHandlePx,
              hx + kTransformHandlePx + 1, hy + kTransformHandlePx + 1);
  }

  /* 旋钮：从上边中点引一条线，圆点与方块同色，拖它转角度。 */
  const comp_ui::Point top_doc = comp_ui::transform_handle_point(frame, rotation, 1);
  const comp_ui::Point knob_doc =
      comp_ui::transform_rotate_knob_point(frame, rotation, kTransformKnobOffsetPx / zoom);
  const comp_ui::Point top_screen = shell.doc.view.doc_to_screen(top_doc.x, top_doc.y, layout.canvas,
                                                             shell.doc.width, shell.doc.height);
  const comp_ui::Point knob_screen = shell.doc.view.doc_to_screen(knob_doc.x, knob_doc.y,
                                                              layout.canvas, shell.doc.width,
                                                              shell.doc.height);
  SelectObject(dc, GetStockObject(NULL_BRUSH));
  MoveToEx(dc, static_cast<int>(std::lround(top_screen.x)),
           static_cast<int>(std::lround(top_screen.y)), nullptr);
  LineTo(dc, static_cast<int>(std::lround(knob_screen.x)),
         static_cast<int>(std::lround(knob_screen.y)));
  SelectObject(dc, handle_brush);
  const int knob_x = static_cast<int>(std::lround(knob_screen.x));
  const int knob_y = static_cast<int>(std::lround(knob_screen.y));
  Ellipse(dc, knob_x - 5, knob_y - 5, knob_x + 6, knob_y + 6);

  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(frame_pen);
  DeleteObject(handle_brush);
}

void draw_canvas_overlays(HDC dc, const ShellState& shell, const Layout& layout) {
  if (layout.canvas.empty()) return;
  const int saved = SaveDC(dc);
  IntersectClipRect(dc, layout.canvas.x, layout.canvas.y, layout.canvas.right(),
                    layout.canvas.bottom());
  SetBkMode(dc, TRANSPARENT);

  /*
   * 移动预览：拖动期间蚂蚁线整体跟着鼠标走，像素与轮廓在松开时一次性提交。
   * 偏移取整到文档像素，和 move_end 提交的 dx/dy 完全一致，预览不会「差一像素」。
   */
  int ants_dx = 0;
  int ants_dy = 0;
  if (shell.doc.move_active) {
    ants_dx = static_cast<int>(std::lround(shell.doc.move_current.x - shell.doc.move_start.x));
    ants_dy = static_cast<int>(std::lround(shell.doc.move_current.y - shell.doc.move_start.y));
  }

  /* 选区蚂蚁线：白色底线 + 黑色虚线段；相位随时间前进 = 「蚂蚁在爬」。 */
  if (shell.doc.selection_active && !shell.doc.selection_segments.empty()) {
    HPEN base_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dash_pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, base_pen);
    for (const auto& segment : shell.doc.selection_segments) {
      comp_ui::Point a = shell.doc.view.doc_to_screen(segment.x0 + ants_dx, segment.y0 + ants_dy,
                                                 layout.canvas, shell.doc.width, shell.doc.height);
      comp_ui::Point b = shell.doc.view.doc_to_screen(segment.x1 + ants_dx, segment.y1 + ants_dy,
                                                 layout.canvas, shell.doc.width, shell.doc.height);
      if (!clip_axis_aligned(layout.canvas, &a, &b)) continue;
      MoveToEx(dc, static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)),
               nullptr);
      LineTo(dc, static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)));
    }

    SelectObject(dc, dash_pen);
    comp_ui::DashSpan spans[1024];
    for (const auto& segment : shell.doc.selection_segments) {
      comp_ui::Point a = shell.doc.view.doc_to_screen(segment.x0 + ants_dx, segment.y0 + ants_dy,
                                                 layout.canvas, shell.doc.width, shell.doc.height);
      comp_ui::Point b = shell.doc.view.doc_to_screen(segment.x1 + ants_dx, segment.y1 + ants_dy,
                                                 layout.canvas, shell.doc.width, shell.doc.height);
      if (!clip_axis_aligned(layout.canvas, &a, &b)) continue;
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const double length = std::sqrt(dx * dx + dy * dy);
      if (!(length > 0.0)) continue;
      const int count = comp_ui::dash_spans(length, kAntsDashPx, kAntsGapPx,
                                            shell.doc.selection_phase, spans, 1024);
      const int drawn = std::min(count, 1024);
      for (int i = 0; i < drawn; ++i) {
        const double t0 = spans[i].begin / length;
        const double t1 = spans[i].end / length;
        MoveToEx(dc, static_cast<int>(std::lround(a.x + dx * t0)),
                 static_cast<int>(std::lround(a.y + dy * t0)), nullptr);
        LineTo(dc, static_cast<int>(std::lround(a.x + dx * t1)),
               static_cast<int>(std::lround(a.y + dy * t1)));
      }
    }
    SelectObject(dc, old_pen);
    DeleteObject(base_pen);
    DeleteObject(dash_pen);
  }

  /*
   * §5.44 选区手柄：8 个把手画在**基础蒙版**的紧包围盒上（与蚂蚁线的口径不同：
   * 蚂蚁线画的是含羽化的有效 coverage）。拖动中把手跟着**预览**落点走，并把
   * 落点画成双描边矩形——用户看到的就是松手后选区会变成的框。
   * 命中与绘制共用 selection_handle_centers，两处坐标不可能分叉。
   */
  if (shell.doc.selection_handle_drag >= 0) {
    const comp_ui::Rect& preview = shell.doc.selection_handle_current;
    const comp_ui::Point a = shell.doc.view.doc_to_screen(preview.x, preview.y, layout.canvas,
                                                     shell.doc.width, shell.doc.height);
    const comp_ui::Point b = shell.doc.view.doc_to_screen(preview.right(), preview.bottom(),
                                                     layout.canvas, shell.doc.width, shell.doc.height);
    const int left = static_cast<int>(std::lround(std::min(a.x, b.x)));
    const int top = static_cast<int>(std::lround(std::min(a.y, b.y)));
    const int right = static_cast<int>(std::lround(std::max(a.x, b.x)));
    const int bottom = static_cast<int>(std::lround(std::max(a.y, b.y)));
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, left, top, right, bottom);
    SelectObject(dc, dotted);
    Rectangle(dc, left, top, right, bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }
  if (selection_handles_available(shell)) {
    comp_ui::Point centers[comp_ui::kSelectionHandleCount];
    selection_handle_centers(shell, layout,
                             shell.doc.selection_handle_drag >= 0 ? shell.doc.selection_handle_current
                                                              : shell.doc.selection_base,
                             centers);
    HBRUSH fill = CreateSolidBrush(RGB(255, 255, 255));
    HPEN frame = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ old_brush = SelectObject(dc, fill);
    HGDIOBJ old_pen = SelectObject(dc, frame);
    for (int i = 0; i < comp_ui::kSelectionHandleCount; ++i) {
      const int cx = static_cast<int>(std::lround(centers[i].x));
      const int cy = static_cast<int>(std::lround(centers[i].y));
      /* 7×7 方块（3 px 半径 + 1 px 黑框）：够大能点中，又不盖住 1 px 的蚂蚁线。 */
      Rectangle(dc, cx - 3, cy - 3, cx + 4, cy + 4);
    }
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(frame);
    DeleteObject(fill);
  }

  /* 框选预览：文档坐标两点 -> 屏幕矩形；白实线 + 黑虚线（M 键切椭圆）。 */
  if (shell.doc.marquee_active) {
    const comp_ui::Point a = shell.doc.view.doc_to_screen(shell.doc.marquee_start.x,
                                                     shell.doc.marquee_start.y, layout.canvas,
                                                     shell.doc.width, shell.doc.height);
    const comp_ui::Point b = shell.doc.view.doc_to_screen(shell.doc.marquee_current.x,
                                                     shell.doc.marquee_current.y, layout.canvas,
                                                     shell.doc.width, shell.doc.height);
    const int left = static_cast<int>(std::lround(std::min(a.x, b.x)));
    const int top = static_cast<int>(std::lround(std::min(a.y, b.y)));
    const int right = static_cast<int>(std::lround(std::max(a.x, b.x)));
    const int bottom = static_cast<int>(std::lround(std::max(a.y, b.y)));
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (shell.marquee_shape == comp_ui::kMarqueeEllipse) {
      Ellipse(dc, left, top, right, bottom);
      SelectObject(dc, dotted);
      Ellipse(dc, left, top, right, bottom);
    } else {
      Rectangle(dc, left, top, right, bottom);
      SelectObject(dc, dotted);
      Rectangle(dc, left, top, right, bottom);
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  /*
   * 缩放拖框预览：屏幕坐标矩形，白实线 + 黑虚线（松手后该区域铺满画布）。
   * 与选区预览同一套双描边，区别只是不经过文档坐标——拖框期间视图不变。
   */
  if (shell.doc.zoom_marquee_active) {
    const int left = std::min(shell.doc.zoom_marquee_start.x, shell.doc.zoom_marquee_current.x);
    const int top = std::min(shell.doc.zoom_marquee_start.y, shell.doc.zoom_marquee_current.y);
    const int right = std::max(shell.doc.zoom_marquee_start.x, shell.doc.zoom_marquee_current.x);
    const int bottom = std::max(shell.doc.zoom_marquee_start.y, shell.doc.zoom_marquee_current.y);
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, left, top, right, bottom);
    SelectObject(dc, dotted);
    Rectangle(dc, left, top, right, bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  /*
   * 裁剪预览（S22）：框外三段遮罩 + 白实线框。
   * 遮罩分上/下/左/右四条带而不是「框外全糊」——中间保留一块完整可见区域，
   * 用户才能看清自己框住了什么（PS 的同款做法，全屏暗反而看不出边界）。
   */
  if (shell.doc.crop_active && shell.doc.crop_rect.width > 0 && shell.doc.crop_rect.height > 0) {
    const comp_ui::Point a = shell.doc.view.doc_to_screen(shell.doc.crop_rect.x, shell.doc.crop_rect.y,
                                                     layout.canvas, shell.doc.width, shell.doc.height);
    const comp_ui::Point b =
        shell.doc.view.doc_to_screen(shell.doc.crop_rect.x + shell.doc.crop_rect.width,
                                 shell.doc.crop_rect.y + shell.doc.crop_rect.height, layout.canvas,
                                 shell.doc.width, shell.doc.height);
    const int left = static_cast<int>(std::lround(std::min(a.x, b.x)));
    const int top = static_cast<int>(std::lround(std::min(a.y, b.y)));
    const int right = static_cast<int>(std::lround(std::max(a.x, b.x)));
    const int bottom = static_cast<int>(std::lround(std::max(a.y, b.y)));
    const int canvas_left = layout.canvas.x;
    const int canvas_top = layout.canvas.y;
    const int canvas_right = layout.canvas.right();
    const int canvas_bottom = layout.canvas.bottom();
    const bool visible = right >= canvas_left && bottom >= canvas_top &&
                         left <= canvas_right && top <= canvas_bottom;
    if (visible) {
      /* 裁剪框允许超出画布（Crop 支持向负方向扩），遮罩夹到画布矩形内。 */
      const int over_left = std::max(left, canvas_left);
      const int over_top = std::max(top, canvas_top);
      const int over_right = std::min(right, canvas_right);
      const int over_bottom = std::min(bottom, canvas_bottom);
      HBRUSH shade = CreateSolidBrush(RGB(96, 96, 96));
      HGDIOBJ old_brush = SelectObject(dc, shade);
      if (over_top > canvas_top) {
        Rectangle(dc, canvas_left, canvas_top, canvas_right, over_top);
      }
      if (over_bottom < canvas_bottom) {
        Rectangle(dc, canvas_left, over_bottom, canvas_right, canvas_bottom);
      }
      if (over_left > canvas_left) {
        Rectangle(dc, canvas_left, over_top, over_left, over_bottom);
      }
      if (over_right < canvas_right) {
        Rectangle(dc, over_right, over_top, canvas_right, over_bottom);
      }
      SelectObject(dc, old_brush);
      DeleteObject(shade);
    }
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ frame_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, left, top, right, bottom);
    SelectObject(dc, frame_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
  }

  /*
   * 渐变预览（S9）：方向线 + 两个端点方块；径向额外画半径圆。只是几何提示，
   * 不预演颜色渐变——真正的像素在松开时由引擎一次性写出。
   */
  if (shell.doc.gradient_active) {
    const comp_ui::Point a =
        shell.doc.view.doc_to_screen(shell.doc.gradient_start.x, shell.doc.gradient_start.y,
                                 layout.canvas, shell.doc.width, shell.doc.height);
    const comp_ui::Point b =
        shell.doc.view.doc_to_screen(shell.doc.gradient_current.x, shell.doc.gradient_current.y,
                                 layout.canvas, shell.doc.width, shell.doc.height);
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dashed = CreatePen(PS_DASH, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int ax = static_cast<int>(std::lround(a.x));
    const int ay = static_cast<int>(std::lround(a.y));
    const int bx = static_cast<int>(std::lround(b.x));
    const int by = static_cast<int>(std::lround(b.y));
    MoveToEx(dc, ax, ay, nullptr);
    LineTo(dc, bx, by);
    if (shell.gradient_type == comp_ui::kGradientRadial) {
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const int radius = static_cast<int>(std::lround(std::sqrt(dx * dx + dy * dy)));
      SelectObject(dc, dashed);
      Ellipse(dc, ax - radius, ay - radius, ax + radius, ay + radius);
      SelectObject(dc, solid);
    }
    /* 起点 = 空心方框（色标 0），终点 = 实心小方块（色标 1）。 */
    Rectangle(dc, ax - 3, ay - 3, ax + 4, ay + 4);
    HGDIOBJ old_fill = SelectObject(dc, GetStockObject(BLACK_BRUSH));
    Rectangle(dc, bx - 2, by - 2, bx + 3, by + 3);
    SelectObject(dc, old_fill);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dashed);
  }

  /*
   * Progressive Blur 的渐变轴（P1 滤镜收官）：拖拽中画实线，已提交的轴画虚线
   * 常驻；起点（From 端）空心方框、终点（To 端）实心方块，径向档额外画半径圆。
   * 只画几何——真正的模糊在回车时由引擎一次性写出（与渐变/形状预览同口径）。
   */
  if (progressive_axis_available(shell)) {
    comp_ui::Point from{};
    comp_ui::Point to{};
    bool show = false;
    if (shell.doc.progressive_axis_dragging) {
      from = shell.doc.progressive_axis_start;
      to = shell.doc.progressive_axis_current;
      show = true;
    } else if (shell.doc.progressive_axis_set) {
      from = shell.doc.progressive_axis_start;
      to = shell.doc.progressive_axis_final;
      show = true;
    }
    if (show) {
      const comp_ui::Point a = shell.doc.view.doc_to_screen(from.x, from.y, layout.canvas, shell.doc.width,
                                                       shell.doc.height);
      const comp_ui::Point b = shell.doc.view.doc_to_screen(to.x, to.y, layout.canvas, shell.doc.width,
                                                       shell.doc.height);
      const int ax = static_cast<int>(std::lround(a.x));
      const int ay = static_cast<int>(std::lround(a.y));
      const int bx = static_cast<int>(std::lround(b.x));
      const int by = static_cast<int>(std::lround(b.y));
      HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
      HPEN dashed = CreatePen(PS_DASH, 1, RGB(0, 0, 0));
      HGDIOBJ old_pen = SelectObject(dc, shell.doc.progressive_axis_dragging ? solid : dashed);
      HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      MoveToEx(dc, ax, ay, nullptr);
      LineTo(dc, bx, by);
      if (shell.doc.filter.value(2) >= 0.5) { /* Radial 0/1 行 */
        const int radius = static_cast<int>(std::lround(std::hypot(b.x - a.x, b.y - a.y)));
        Ellipse(dc, ax - radius, ay - radius, ax + radius, ay + radius);
      }
      Rectangle(dc, ax - 3, ay - 3, ax + 4, ay + 4);
      HGDIOBJ old_fill = SelectObject(dc, GetStockObject(BLACK_BRUSH));
      Rectangle(dc, bx - 2, by - 2, bx + 3, by + 3);
      SelectObject(dc, old_fill);
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(solid);
      DeleteObject(dashed);
    }
  }

  /*
   * 形状预览（S10）：矩形/椭圆画白实线 + 黑虚线双描边（圆角矩形用 RoundRect），
   * 直线画一条双线。只是几何提示，不预演颜色——真正的像素在松开时由引擎一次性写出
   * （与渐变预览同口径）。修饰键（Shift = 正方/正圆、Alt = 从中心）在预览里就生效，
   * 所见即落笔盒。
   */
  if (shell.doc.shape_active) {
    const bool square = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool from_center = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const comp_ui::ShapeDragBox box = comp_ui::shape_drag_box(
        shell.doc.shape_start.x, shell.doc.shape_start.y, shell.doc.shape_current.x, shell.doc.shape_current.y,
        square, from_center);
    const comp_ui::Point a = shell.doc.view.doc_to_screen(box.x0, box.y0, layout.canvas,
                                                      shell.doc.width, shell.doc.height);
    const comp_ui::Point b = shell.doc.view.doc_to_screen(box.x1, box.y1, layout.canvas,
                                                      shell.doc.width, shell.doc.height);
    const int left = static_cast<int>(std::lround(std::min(a.x, b.x)));
    const int top = static_cast<int>(std::lround(std::min(a.y, b.y)));
    const int right = static_cast<int>(std::lround(std::max(a.x, b.x)));
    const int bottom = static_cast<int>(std::lround(std::max(a.y, b.y)));

    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (shell.shape_kind == comp_ui::kShapeLine) {
      MoveToEx(dc, left, top, nullptr);
      LineTo(dc, right, bottom);
      SelectObject(dc, dotted);
      MoveToEx(dc, left, top, nullptr);
      LineTo(dc, right, bottom);
    } else if (shell.shape_kind == comp_ui::kShapeEllipse) {
      Ellipse(dc, left, top, right, bottom);
      SelectObject(dc, dotted);
      Ellipse(dc, left, top, right, bottom);
    } else {
      /* 圆角半径按同样的屏幕缩放换算，预览与落笔盒一致。 */
      const double zoom = shell.doc.view.zoom;
      const int radius = static_cast<int>(
          std::lround(comp_ui::shape_effective_corner_radius(shell.shape_corner_radius, box) *
                      (zoom > 0.0 ? zoom : 0.0)));
      if (radius > 0) {
        RoundRect(dc, left, top, right, bottom, radius * 2, radius * 2);
        SelectObject(dc, dotted);
        RoundRect(dc, left, top, right, bottom, radius * 2, radius * 2);
      } else {
        Rectangle(dc, left, top, right, bottom);
        SelectObject(dc, dotted);
        Rectangle(dc, left, top, right, bottom);
      }
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  /*
   * 文字预览（S11/P5）：插入点光标 + 未来文本框（白实线 + 黑虚线双描边）。
   * 只给位置与尺寸，不预演字形：点阵轨道没有可借的 GDI 字体，字体轨道也不该
   * 用 GDI 字体冒充引擎结果（同一段文字会排出两种字形）。像素在回车时由引擎
   * 一次写出（与渐变/形状预览同口径）。点阵轨道的度量与内核 build_layout 同公式；
   * 字体轨道是 em 估计值，框线只是提示，宽度在面板/状态栏都标了「≈」。
   */
  if (shell.doc.type_active) {
    const comp_ui::TypeBlockMetrics metrics = type_metrics(shell);
    const comp_ui::Point origin = shell.doc.view.doc_to_screen(
        shell.doc.type_origin.x, shell.doc.type_origin.y, layout.canvas, shell.doc.width, shell.doc.height);
    const comp_ui::Point corner = shell.doc.view.doc_to_screen(
        shell.doc.type_origin.x + metrics.width, shell.doc.type_origin.y + metrics.height,
        layout.canvas, shell.doc.width, shell.doc.height);
    const int left = static_cast<int>(std::lround(std::min(origin.x, corner.x)));
    const int top = static_cast<int>(std::lround(std::min(origin.y, corner.y)));
    const int right = static_cast<int>(std::lround(std::max(origin.x, corner.x)));
    const int bottom = static_cast<int>(std::lround(std::max(origin.y, corner.y)));

    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (right - left >= 1 && bottom - top >= 1) {
      Rectangle(dc, left, top, right, bottom);
      SelectObject(dc, dotted);
      Rectangle(dc, left, top, right, bottom);
      SelectObject(dc, solid);
    }
    /* 插入点光标：从原点向下画一个字号高（文档字号 × 缩放，至少 1 px）。 */
    const int caret = static_cast<int>(
        std::lround(comp_ui::type_caret_height(shell.doc.type_font_size, shell.doc.view.zoom)));
    MoveToEx(dc, left, top, nullptr);
    LineTo(dc, left, top + caret);
    SelectObject(dc, dotted);
    MoveToEx(dc, left + 1, top, nullptr);
    LineTo(dc, left + 1, top + caret);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  /* 套索预览：沿已采样的折线画一条高对比度细线（闭合边在松开时由引擎生成）。 */  if (shell.doc.lasso_active && shell.doc.lasso_points.size() >= 4) {
    HPEN trail = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(dc, trail);
    bool started = false;
    for (size_t i = 0; i + 1 < shell.doc.lasso_points.size(); i += 2) {
      const comp_ui::Point screen =
          shell.doc.view.doc_to_screen(shell.doc.lasso_points[i], shell.doc.lasso_points[i + 1],
                                   layout.canvas, shell.doc.width, shell.doc.height);
      const int px = static_cast<int>(std::lround(screen.x));
      const int py = static_cast<int>(std::lround(screen.y));
      if (!started) {
        MoveToEx(dc, px, py, nullptr);
        started = true;
      } else {
        LineTo(dc, px, py);
      }
    }
    SelectObject(dc, old_pen);
    DeleteObject(trail);
  }

  /*
   * 钢笔预览（S8）：逐段采样折线 + 锚点方块 + 控制柄。闭合段用虚线提示
   * 「提交时会闭合成选区」——引擎 apply_path 对子路径按闭合解释。
   * 逐段取几何，避免给 2048 个锚点的整条路径开大缓冲。
   */
  if (shell.tool == comp_ui::ToolId::Pen && !shell.doc.pen_anchors.empty()) {
    const int anchor_count = static_cast<int>(shell.doc.pen_anchors.size());
    const int segment_count = comp_ui::pen_segment_count(anchor_count, /*closed=*/false);
    HPEN path_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN handle_pen = CreatePen(PS_DOT, 1, RGB(80, 200, 255));
    HGDIOBJ old_pen = SelectObject(dc, path_pen);

    comp_ui::Point samples[comp_ui::kPenPreviewSamples + 1];
    for (int i = 0; i < segment_count; ++i) {
      const comp_ui::PenSegment segment =
          comp_ui::pen_segment_at(shell.doc.pen_anchors.data(), anchor_count, i);
      samples[0] = segment.start;
      for (int sample = 1; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        samples[sample] = comp_ui::pen_cubic_at(
            segment, static_cast<double>(sample) / static_cast<double>(comp_ui::kPenPreviewSamples));
      }
      bool started = false;
      for (int sample = 0; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        const comp_ui::Point screen =
            shell.doc.view.doc_to_screen(samples[sample].x, samples[sample].y, layout.canvas,
                                     shell.doc.width, shell.doc.height);
        const int px = static_cast<int>(std::lround(screen.x));
        const int py = static_cast<int>(std::lround(screen.y));
        if (!started) {
          MoveToEx(dc, px, py, nullptr);
          started = true;
        } else {
          LineTo(dc, px, py);
        }
      }
    }

    /* 未闭合时用虚线画出「会补上的闭合边」：2 个锚点起就有意义。 */
    if (anchor_count >= 2) {
      SelectObject(dc, handle_pen);
      const comp_ui::PenSegment closing =
          comp_ui::pen_segment_at(shell.doc.pen_anchors.data(), anchor_count, anchor_count - 1);
      bool started = false;
      for (int sample = 0; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        const comp_ui::Point point =
            sample == 0 ? closing.start
                        : comp_ui::pen_cubic_at(
                              closing, static_cast<double>(sample) /
                                           static_cast<double>(comp_ui::kPenPreviewSamples));
        const comp_ui::Point screen = shell.doc.view.doc_to_screen(
            point.x, point.y, layout.canvas, shell.doc.width, shell.doc.height);
        const int px = static_cast<int>(std::lround(screen.x));
        const int py = static_cast<int>(std::lround(screen.y));
        if (!started) {
          MoveToEx(dc, px, py, nullptr);
          started = true;
        } else {
          LineTo(dc, px, py);
        }
      }
    }

    /* 锚点与控制柄：锚点是 7×7 白框黑心，控制柄用 1px 虚线连到端点。 */
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SelectObject(dc, path_pen);
    for (int i = 0; i < anchor_count; ++i) {
      const comp_ui::PenAnchor& anchor = shell.doc.pen_anchors[static_cast<size_t>(i)];
      const comp_ui::Point center = shell.doc.view.doc_to_screen(
          anchor.position.x, anchor.position.y, layout.canvas, shell.doc.width, shell.doc.height);
      const int cx = static_cast<int>(std::lround(center.x));
      const int cy = static_cast<int>(std::lround(center.y));
      Rectangle(dc, cx - 3, cy - 3, cx + 4, cy + 4);
      SelectObject(dc, handle_pen);
      if (anchor.handle_out.x != 0.0 || anchor.handle_out.y != 0.0) {
        const comp_ui::Point handle = shell.doc.view.doc_to_screen(
            anchor.position.x + anchor.handle_out.x, anchor.position.y + anchor.handle_out.y,
            layout.canvas, shell.doc.width, shell.doc.height);
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, static_cast<int>(std::lround(handle.x)),
               static_cast<int>(std::lround(handle.y)));
      }
      if (anchor.handle_in.x != 0.0 || anchor.handle_in.y != 0.0) {
        const comp_ui::Point handle = shell.doc.view.doc_to_screen(
            anchor.position.x + anchor.handle_in.x, anchor.position.y + anchor.handle_in.y,
            layout.canvas, shell.doc.width, shell.doc.height);
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, static_cast<int>(std::lround(handle.x)),
               static_cast<int>(std::lround(handle.y)));
      }
      SelectObject(dc, path_pen);
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(path_pen);
    DeleteObject(handle_pen);
  }

  /*
   * 仿制图章的源点十字：源点是文档坐标，缩放/平移后必须跟着画布走，所以每帧
   * 由 doc_to_screen 现算，不缓存屏幕坐标。取像素中心（+0.5）让十字压在像素上。
   * 只在选中该工具、且已设源点时画——别的工具下画一个没有意义的准星只会误导。
   */
  if (shell.tool == comp_ui::ToolId::Clone && shell.doc.clone_source_set) {
    const comp_ui::Point cross = shell.doc.view.doc_to_screen(
        shell.doc.clone_source_x + 0.5, shell.doc.clone_source_y + 0.5, layout.canvas, shell.doc.width,
        shell.doc.height);
    const int cx = static_cast<int>(std::lround(cross.x));
    const int cy = static_cast<int>(std::lround(cross.y));
    HPEN source_pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HPEN source_inner = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(dc, source_pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 6, cy - 6, cx + 6, cy + 6);
    MoveToEx(dc, cx - 10, cy, nullptr);
    LineTo(dc, cx + 10, cy);
    MoveToEx(dc, cx, cy - 10, nullptr);
    LineTo(dc, cx, cy + 10);
    SelectObject(dc, source_inner);
    Ellipse(dc, cx - 6, cy - 6, cx + 6, cy + 6);
    MoveToEx(dc, cx - 10, cy, nullptr);
    LineTo(dc, cx + 10, cy);
    MoveToEx(dc, cx, cy - 10, nullptr);
    LineTo(dc, cx, cy + 10);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(source_pen);
    DeleteObject(source_inner);
  }

  /* 进行中的笔迹折线（文档像素 -> 屏幕）：落色用前景色，重绘用高对比度青。 */
  if (stroke_in_progress(shell) && shell.doc.stroke_points.size() >= 4) {
    const COLORREF trail_color =
        shell.doc.stroke_kind == comp_ui::StrokeKind::Paint && !shell.doc.stroke_erase
            ? RGB(shell.foreground.r, shell.foreground.g, shell.foreground.b)
            : RGB(80, 200, 255);
    HPEN trail = CreatePen(PS_SOLID, 3, trail_color);
    HGDIOBJ old_pen = SelectObject(dc, trail);
    bool started = false;
    for (size_t i = 0; i + 1 < shell.doc.stroke_points.size(); i += 2) {
      const comp_ui::Point screen = shell.doc.view.doc_to_screen(
          shell.doc.stroke_points[i], shell.doc.stroke_points[i + 1], layout.canvas, shell.doc.width,
          shell.doc.height);
      const int px = static_cast<int>(std::lround(screen.x));
      const int py = static_cast<int>(std::lround(screen.y));
      if (!started) {
        MoveToEx(dc, px, py, nullptr);
        started = true;
      } else {
        LineTo(dc, px, py);
      }
    }
    SelectObject(dc, old_pen);
    DeleteObject(trail);
  }

  /* 笔尖轮廓：吃笔刷直径的工具，鼠标在画布上时随时可见。 */
  if (shell.cursor_valid && comp_ui::tool_has_brush_tip(shell.tool) &&
      layout.canvas.contains(shell.cursor_x, shell.cursor_y)) {
    const double radius = comp_ui::brush_tip_radius(shell.brush_size, shell.doc.view.zoom);
    const int left = static_cast<int>(std::lround(shell.cursor_x - radius));
    const int top = static_cast<int>(std::lround(shell.cursor_y - radius));
    const int right = static_cast<int>(std::lround(shell.cursor_x + radius));
    const int bottom = static_cast<int>(std::lround(shell.cursor_y + radius));
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, left, top, right, bottom);
    SelectObject(dc, dotted);
    Ellipse(dc, left, top, right, bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  /* 变换手柄画在最后：它压在所有预览之上（可点时必须可见）。 */
  draw_transform_frame(dc, shell, layout);

  RestoreDC(dc, saved);
}

/*
 * §6.60 面板分界线。几何全在 ui_core::Layout（panel_splitter_at 命中用的就是同一
 * 块矩形），这里只画：常态 1px 暗线标出边界；悬停/拖动时整条 5px 带提亮——否则
 * 用户根本看不出来这里能拖。
 */
void draw_panel_dividers(HDC dc, const ShellState& shell, const Layout& layout) {
  if (layout.layer_splitter.w <= 0 && layout.adjust_splitter.w <= 0) return;

  const bool left_hot = shell.panel_resize_target == 1 || shell.panel_splitter_hover == 1;
  const bool right_hot = shell.panel_resize_target == 2 || shell.panel_splitter_hover == 2;

  if (layout.layer_splitter.w > 0) {
    if (left_hot) {
      RECT band{layout.layer_splitter.x, layout.layer_splitter.y,
                layout.layer_splitter.right(), layout.layer_splitter.bottom()};
      HBRUSH hot = CreateSolidBrush(RGB(46, 54, 68));
      FillRect(dc, &band, hot);
      DeleteObject(hot);
    }
    RECT line{layout.layer_splitter.right() - 1, layout.layer_splitter.y,
              layout.layer_splitter.right(), layout.layer_splitter.bottom()};
    HBRUSH brush = CreateSolidBrush(left_hot ? RGB(86, 130, 200) : RGB(16, 18, 22));
    FillRect(dc, &line, brush);
    DeleteObject(brush);
  }
  if (layout.adjust_splitter.w > 0) {
    if (right_hot) {
      RECT band{layout.adjust_splitter.x, layout.adjust_splitter.y,
                layout.adjust_splitter.right(), layout.adjust_splitter.bottom()};
      HBRUSH hot = CreateSolidBrush(RGB(46, 54, 68));
      FillRect(dc, &band, hot);
      DeleteObject(hot);
    }
    RECT line{layout.adjust_splitter.x, layout.adjust_splitter.y,
              layout.adjust_splitter.x + 1, layout.adjust_splitter.bottom()};
    HBRUSH brush = CreateSolidBrush(right_hot ? RGB(86, 130, 200) : RGB(16, 18, 22));
    FillRect(dc, &line, brush);
    DeleteObject(brush);
  }
}

/*
 * §5.43 收起/展开按钮。几何全部来自 Layout（panel_toggle_at 命中用的是同一块
 * 矩形），这里只画两种形态：
 *   - 展开态：标题行右端 18×18 小方块，三角指向「收进去」的一侧；
 *   - 收起态：整条 24px 竖条，竖排两字 + 指向「展开出来」的三角。
 * 收起条整条可点，因此悬停高亮也铺整条：点哪儿亮哪儿，不必去瞄 18px 的小方块。
 */
void draw_toggle_chevron(HDC dc, int cx, int cy, int half, bool point_left, COLORREF color) {
  POINT points[3];
  if (point_left) {
    points[0] = POINT{cx - half, cy};
    points[1] = POINT{cx + half, cy - half};
    points[2] = POINT{cx + half, cy + half};
  } else {
    points[0] = POINT{cx + half, cy};
    points[1] = POINT{cx - half, cy - half};
    points[2] = POINT{cx - half, cy + half};
  }
  HBRUSH brush = CreateSolidBrush(color);
  HGDIOBJ old = SelectObject(dc, brush);
  Polygon(dc, points, 3);
  SelectObject(dc, old);
  DeleteObject(brush);
}

void draw_panel_toggle(HDC dc, const comp_ui::Rect& box, bool collapsed, bool hot,
                       bool panel_on_left, const wchar_t* row0, const wchar_t* row1) {
  if (box.w <= 0 || box.h <= 0) return;
  RECT rect{box.x, box.y, box.right(), box.bottom()};
  HBRUSH fill = CreateSolidBrush(collapsed ? (hot ? RGB(44, 50, 62) : RGB(30, 33, 40))
                                          : (hot ? RGB(52, 62, 80) : RGB(38, 42, 52)));
  FillRect(dc, &rect, fill);
  DeleteObject(fill);

  SetBkMode(dc, TRANSPARENT);
  if (!collapsed) {
    FrameRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    /* 展开态的三角朝外：左栏收起 = 往左推，右栏收起 = 往右推。 */
    draw_toggle_chevron(dc, box.x + box.w / 2, box.y + box.h / 2, 4, panel_on_left,
                        hot ? RGB(255, 255, 255) : RGB(190, 196, 210));
    return;
  }

  /* 收起条与画布交界描一条暗线：两边都接近黑色时，不描就看不出边界。 */
  RECT edge{panel_on_left ? box.right() - 1 : box.x, box.y,
            panel_on_left ? box.right() : box.x + 1, box.bottom()};
  HBRUSH line = CreateSolidBrush(RGB(16, 18, 22));
  FillRect(dc, &edge, line);
  DeleteObject(line);

  SetTextColor(dc, hot ? RGB(232, 238, 250) : RGB(168, 174, 188));
  SIZE extent{};
  GetTextExtentPoint32W(dc, row0, lstrlenW(row0), &extent);
  TextOutW(dc, box.x + (box.w - extent.cx) / 2, box.y + 8, row0, lstrlenW(row0));
  GetTextExtentPoint32W(dc, row1, lstrlenW(row1), &extent);
  TextOutW(dc, box.x + (box.w - extent.cx) / 2, box.y + 26, row1, lstrlenW(row1));

  /* 三角朝画布：左栏展开 = 往右拉出来，右栏展开 = 往左拉出来。 */
  int cy = box.y + 52;
  if (cy > box.bottom() - 10) cy = box.bottom() - 10;
  if (cy > box.y + 44) {
    draw_toggle_chevron(dc, box.x + box.w / 2, cy, 4, !panel_on_left,
                        hot ? RGB(232, 238, 250) : RGB(150, 158, 176));
  }
}

void draw_panel_toggles(HDC dc, const ShellState& shell, const Layout& layout) {
  if (layout.layer_toggle.w > 0) {
    draw_panel_toggle(dc, layout.layer_toggle, layout.layer_collapsed,
                      shell.panel_toggle_hover == 1, true, L"图", L"层");
  }
  if (layout.adjust_toggle.w > 0) {
    draw_panel_toggle(dc, layout.adjust_toggle, layout.adjust_collapsed,
                      shell.panel_toggle_hover == 2, false, L"调", L"整");
  }
}

/*
 * 标题栏：后端 + 文件名 + 脏标记（*）。present 每帧调用，但文本没变就不发
 * SetWindowTextW——缓存字符串的目的只是省消息，不是第二份真相：
 * 文件名来自 shell.doc.document_path，脏标记每帧向引擎 comp_document_is_dirty 现问。
 */
void update_window_title(HWND window, ShellState& shell) {
  std::wstring title = L"Compositor (Windows shell) - ";
  title += backend_label(shell.backend);
  if (!shell.doc.document_path.empty()) {
    const std::string& path = shell.doc.document_path;
    const size_t slash = path.find_last_of("/\\");
    title += L" - ";
    title += widen_utf8(slash == std::string::npos ? path : path.substr(slash + 1));
  } else if (shell.doc.document != nullptr) {
    title += L" - 未命名";
  }
  int32_t dirty = 0;
  if (shell.doc.document != nullptr &&
      comp_document_is_dirty(shell.doc.document, &dirty) == COMP_OK && dirty != 0) {
    title += L" *";
  }
  if (title == shell.window_title_cache) return;
  shell.window_title_cache = title;
  SetWindowTextW(window, title.c_str());
}

void present(HWND window, ShellState& shell) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(window, &paint);
  if (shell.doc.needs_render) {
    render_canvas(shell);
    /*
     * 从文档重画这一帧 = 滤镜预览帧被顶掉了：pending 只描述 framebuffer 里那一帧，
     * 不是文档状态。放在这个唯一的上屏入口，撤销/换工具/任何 needs_render 都能自洽。
     */
    shell.doc.filter_preview_pending = false;
  }
  update_window_title(window, shell);

  RECT client{};
  GetClientRect(window, &client);
  const int client_width = client.right - client.left;
  const int client_height = client.bottom - client.top;
  const Layout layout = comp_ui::compute_layout(client_width, client_height);

  /* 新文档 / 打开文档后按窗口适配一次；之后缩放与平移完全由用户操作决定。 */
  if (shell.doc.needs_fit) {
    if (shell.doc.document != nullptr && !layout.canvas.empty()) {
      shell.doc.view.fit(shell.doc.width, shell.doc.height, layout.canvas);
    }
    shell.doc.needs_fit = false;
  }

  HDC memory_dc = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, client_width, client_height);
  HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);

  RECT fill{0, 0, client_width, client_height};
  HBRUSH background = CreateSolidBrush(RGB(32, 32, 36));
  FillRect(memory_dc, &fill, background);
  DeleteObject(background);

  /*
   * 画布上屏：视图变换给出文档 (0,0)/(w,h) 的屏幕位置，与画布矩形求交后
   * 只把可见部分交给 GDI。不能直接传整幅目标矩形——32x 放大时它会达到
   * 数万像素，既慢又可能触发 GDI 坐标上限。
   */
  if (!shell.doc.framebuffer.empty() && shell.doc.width > 0 && shell.doc.height > 0 &&
      !layout.canvas.empty()) {
    const comp_ui::Point top_left = shell.doc.view.doc_to_screen(
        0.0, 0.0, layout.canvas, shell.doc.width, shell.doc.height);
    const comp_ui::Point bottom_right = shell.doc.view.doc_to_screen(
        shell.doc.width, shell.doc.height, layout.canvas, shell.doc.width, shell.doc.height);
    const double clip_left = std::max<double>(top_left.x, layout.canvas.x);
    const double clip_top = std::max<double>(top_left.y, layout.canvas.y);
    const double clip_right = std::min<double>(bottom_right.x, layout.canvas.right());
    const double clip_bottom = std::min<double>(bottom_right.y, layout.canvas.bottom());

    if (clip_right > clip_left && clip_bottom > clip_top) {
      const int dst_x = static_cast<int>(std::floor(clip_left));
      const int dst_y = static_cast<int>(std::floor(clip_top));
      const int dst_w = std::max(1, static_cast<int>(std::ceil(clip_right)) - dst_x);
      const int dst_h = std::max(1, static_cast<int>(std::ceil(clip_bottom)) - dst_y);

      int src_x = static_cast<int>(std::floor((clip_left - top_left.x) / shell.doc.view.zoom));
      int src_y = static_cast<int>(std::floor((clip_top - top_left.y) / shell.doc.view.zoom));
      int src_w = static_cast<int>(std::ceil(static_cast<double>(dst_w) / shell.doc.view.zoom)) + 1;
      int src_h = static_cast<int>(std::ceil(static_cast<double>(dst_h) / shell.doc.view.zoom)) + 1;
      if (src_x < 0) src_x = 0;
      if (src_y < 0) src_y = 0;
      if (src_x > shell.doc.width) src_x = shell.doc.width;
      if (src_y > shell.doc.height) src_y = shell.doc.height;
      if (src_x + src_w > shell.doc.width) src_w = shell.doc.width - src_x;
      if (src_y + src_h > shell.doc.height) src_h = shell.doc.height - src_y;

      if (src_w > 0 && src_h > 0) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = shell.doc.width;
        info.bmiHeader.biHeight = -shell.doc.height; /* 负值 = 自顶向下，与引擎缓冲一致 */
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        const int saved = SaveDC(memory_dc);
        IntersectClipRect(memory_dc, layout.canvas.x, layout.canvas.y, layout.canvas.right(),
                          layout.canvas.bottom());
        SetStretchBltMode(memory_dc, HALFTONE);
        SetBrushOrgEx(memory_dc, 0, 0, nullptr);
        StretchDIBits(memory_dc, dst_x, dst_y, dst_w, dst_h, src_x, src_y, src_w, src_h,
                      shell.doc.framebuffer.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        RestoreDC(memory_dc, saved);
      }
    }
  }

  draw_canvas_overlays(memory_dc, shell, layout);

  draw_toolbar(memory_dc, shell, layout.toolbar);
  /* §5.49：选项栏高度为 0 时整块跳过（矮窗口），画布已经回收了这条高度。 */
  if (layout.options.h > 0) {
    draw_tool_options(memory_dc, shell, layout.options);
  }
  /*
   * §5.43：收起态只剩 24px 竖条，面板内容一列都排不下——整块不画，交给
   * draw_panel_toggles 画竖条。两者互斥，竖条上不会漏出半截图层行。
   */
  if (layout.layer_panel.w > 0 && !layout.layer_collapsed) {
    draw_layer_panel(memory_dc, shell, layout.layer_panel);
  }
  if (layout.adjust_panel.w > 0 && !layout.adjust_collapsed) {
    if (shell.doc.doc_dialog != comp_ui::DocDialogKind::None) {
      draw_doc_dialog(memory_dc, shell, layout.adjust_panel);
    } else if (path_panel_visible(shell)) {
      draw_path_panel(memory_dc, shell, layout.adjust_panel);
    } else if (history_panel_visible(shell)) {
      draw_history_panel(memory_dc, shell, layout.adjust_panel);
    } else if (font_panel_visible(shell)) {
      draw_font_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.doc.color_range_open) {
      draw_color_range_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.doc.trim_open) {
      draw_trim_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.doc.effect_open) {
      draw_effect_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.doc.filter_open) {
      draw_filter_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.tool == comp_ui::ToolId::Type) {
      draw_type_panel(memory_dc, shell, layout.adjust_panel);
    } else {
      draw_adjust_panel(memory_dc, shell, layout.adjust_panel);
    }
  }
  /* 分界线最后画：盖在面板底色之上，1px 线才不会被面板自己的填充吃掉。 */
  draw_panel_toggles(memory_dc, shell, layout);
  draw_panel_dividers(memory_dc, shell, layout);
  draw_status_bar(memory_dc, shell, layout.status);

  BitBlt(dc, 0, 0, client_width, client_height, memory_dc, 0, 0, SRCCOPY);
  SelectObject(memory_dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory_dc);
  EndPaint(window, &paint);
}

}  // namespace comp_shell
