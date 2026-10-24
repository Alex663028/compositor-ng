// AI生成
/*
 * shell_document.cpp — 文档生命周期、文件对话框、导入导出与外壳命令
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {


/* ------------------------------------------------------------ 图像菜单（转画布 / 修边 / 反相） */

/*
 * 画布级写操作（转画布 / 修边）的共同尾部：换尺寸后 view 的偏移与新坐标
 * 系不再对应，必须重新适配；图层、选区与撤销深度都要重新从引擎回读
 * （旋转会转置选区蒙版，修边会清掉选区——壳侧缓存一律作废，不自己推）。
 */
void finish_canvas_command(ShellState& shell) {
  comp_size size{};
  if (comp_document_size(shell.doc.document, &size) == COMP_OK) {
    shell.doc.width = size.width;
    shell.doc.height = size.height;
  }
  shell.doc.needs_fit = true;
  shell.doc.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell);
}

/*
 * 图像 > 图像旋转。quarter_turns：1 = 顺时针 90°，-1 = 逆时针，2 = 180°。
 * 引擎不重采样：只换画布尺寸与图层落位，像素字节不动（见 comp_canvas.h）。
 */
void image_rotate_canvas(ShellState& shell, int32_t quarter_turns, const wchar_t* label) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"图像：还没有文档");
    return;
  }
  /* 修边预设按旧画布算出来的边，旋转后已无意义：先收掉面板，别让它留在那儿。 */
  if (shell.doc.trim_open) {
    shell.doc.trim_open = false;
    shell.doc.trim_input.clear();
  }
  int32_t changed = 0;
  const comp_result result =
      comp_document_rotate_canvas(shell.doc.document, quarter_turns, &changed);
  if (result != COMP_OK) {
    set_status(shell, L"画布旋转失败：参数或图层位置不合法（未改动）");
    return;
  }
  if (changed == 0) {
    /* 空文档 + 180°（或正方形画布的 90°）：状态逐字节相同，引擎不进历史。 */
    set_status(shell, L"画布未变化（空文档且旋转后尺寸不变，未产生撤销步）");
    return;
  }
  finish_canvas_command(shell);
  wchar_t buffer[192];
  wsprintfW(buffer, L"已旋转画布：%s", label);
  refresh_history_status(shell, buffer);
}

/* 图像 > 反相：破坏性一次性落墨，只碰目标图层（与调整层的 Invert 并存）。 */
void image_invert_layer(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"图像：还没有文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"反相：请先选择一个图层");
    return;
  }
  int32_t changed = 0;
  const comp_result result = comp_layer_invert(shell.doc.document, id, &changed);
  if (result == COMP_ERR_UNSUPPORTED) {
    set_status(shell, L"反相：HDR（RGBA16F）图层不受支持（未改动）");
    return;
  }
  if (result == COMP_ERR_STATE) {
    set_status(shell, L"反相：组图层或没有像素的图层（未改动）");
    return;
  }
  if (result != COMP_OK) {
    set_status(shell, L"反相失败（未改动）");
    return;
  }
  shell.doc.needs_render = true;
  if (changed == 0) {
    /* 显式空选区：引擎什么都不改，不产生撤销步。 */
    set_status(shell, L"反相：选区为空，未改动");
    return;
  }
  refresh_history_status(shell, L"已反相图层");
}

std::wstring exe_directory() {
  wchar_t path[MAX_PATH] = L"";
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return std::wstring();
  std::wstring full(path, length);
  const size_t slash = full.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::wstring();
  return full.substr(0, slash);
}

/*
 * 候选顺序：exe 同级 resources/、exe 上一级（开发态 build/ 与仓根并列）、
 * 当前工作目录。命中第一个可用字体就停；三处都没有返回 nullptr。
 *
 * 可用性检查不只看「能打开」：位图字体（units_per_em = 0）、没有 CJK 字形的
 * 字体都不能承担文字工具，宁可继续用点阵，也不要让用户在面板上选到一个
 * 一打字就满屏缺字的字体。自检与窗口启动共用这一条加载路径。
 */
comp_font load_shipped_font(std::wstring* out_label, comp_font_metrics* out_metrics,
                            std::string* out_path) {
  std::vector<std::wstring> candidates;
  const std::wstring exe_dir = exe_directory();
  if (!exe_dir.empty()) {
    candidates.push_back(exe_dir + L"\\resources\\fonts\\" + kShellFontFileName);
    candidates.push_back(exe_dir + L"\\..\\resources\\fonts\\" + kShellFontFileName);
  }
  candidates.push_back(std::wstring(L"resources\\fonts\\") + kShellFontFileName);

  for (const std::wstring& wide : candidates) {
    char utf8[MAX_PATH * 3] = {0};
    if (WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr) <=
        0) {
      continue;
    }
    comp_font font = nullptr;
    if (comp_font_load_file(utf8, 0, &font) != COMP_OK || font == nullptr) continue;
    comp_font_metrics metrics{};
    const bool usable = comp_font_get_metrics(font, &metrics) == COMP_OK &&
                        metrics.scalable != 0 && metrics.units_per_em > 0 &&
                        metrics.has_cjk != 0;
    if (!usable) {
      comp_font_release(font);
      continue;
    }
    if (out_metrics != nullptr) *out_metrics = metrics;
    /* 路径要留下：可编辑文字图层把字体文件路径写进 .comp，重开时才能原样重现。 */
    if (out_path != nullptr) *out_path = std::string(utf8);
    if (out_label != nullptr) {
      const char* family = comp_font_name(font, COMP_FONT_NAME_FAMILY);
      *out_label = (family != nullptr && family[0] != '\0') ? widen_utf8(std::string(family))
                                                            : std::wstring(L"字体轨道");
    }
    return font;
  }
  return nullptr;
}

bool load_shell_font(ShellState& shell) {
  comp_font_metrics metrics{};
  std::wstring label;
  std::string path;
  comp_font font = load_shipped_font(&label, &metrics, &path);
  if (font == nullptr) return false;
  shell.type_font = font;
  shell.type_font_ascender = metrics.ascender;
  shell.type_font_descender = metrics.descender;
  shell.type_font_label = label;
  shell.type_font_path = path;
  /* 默认就走字体轨道：P0-5 的验收是「中文能打、能排、能提交」，多一步切换只是障碍。 */
  shell.type_use_font = true;
  return true;
}

bool read_whole_file(const std::wstring& path, std::vector<uint8_t>* out) {
  if (out == nullptr) return false;
  out->clear();
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;
  bool ok = false;
  if (std::fseek(file, 0, SEEK_END) == 0) {
    const long size = std::ftell(file);
    if (size > 0 && std::fseek(file, 0, SEEK_SET) == 0) {
      out->resize(static_cast<size_t>(size));
      ok = std::fread(out->data(), 1, out->size(), file) == out->size();
    }
  }
  std::fclose(file);
  if (!ok) out->clear();
  return ok;
}

bool load_shell_camera_raw_tables(ShellState& shell) {
  shell.camera_raw_tables_loaded = false;
  if (shell.engine == nullptr) return false;
  /* 表是引擎级共享资源：同一进程里只解码一次，重复调用不重读 1.78 MB。 */
  int32_t already = 0;
  if (comp_engine_camera_raw_tables_loaded(shell.engine, &already) == COMP_OK && already != 0) {
    shell.camera_raw_tables_loaded = true;
    return true;
  }
  std::vector<std::wstring> candidates;
  const std::wstring exe_dir = exe_directory();
  if (!exe_dir.empty()) {
    candidates.push_back(exe_dir + L"\\resources\\" + kCameraRawTableFileName);
    candidates.push_back(exe_dir + L"\\..\\resources\\" + kCameraRawTableFileName);
  }
  candidates.push_back(std::wstring(L"resources\\") + kCameraRawTableFileName);

  for (const std::wstring& wide : candidates) {
    std::vector<uint8_t> data;
    if (!read_whole_file(wide, &data)) continue;
    /* 坏表不覆盖引擎里已有的表（ABI 契约）：继续试下一个候选。 */
    if (comp_engine_load_camera_raw_tables(shell.engine, data.data(), data.size()) != COMP_OK) {
      continue;
    }
    shell.camera_raw_tables_loaded = true;
    return true;
  }
  return false;
}

/* ------------------------------------------------------------ 文件与命令 */

/*
 * 文档要被替换/销毁：色彩范围的预览事务属于旧文档，句柄必须直接丢掉。
 * 这里**不能**调 color_range_cancel——它会 abort 事务，而旧文档马上就要被
 * destroy，abort 会打到已经释放的对象上（UAF）。关面板只是顺带效果。
 */
void forget_color_range(ShellState& shell) {
  shell.doc.color_range_txn = nullptr;
  shell.doc.color_range_open = false;
  shell.doc.color_range_include.clear();
  shell.doc.color_range_exclude.clear();
  shell.doc.color_range_input.clear();
  /* 路径面板的锚点同样是旧文档坐标：换文档时一起丢掉（关面板只是顺带）。 */
  path_store_clear(shell);
  /* ROI #5：通道是旧文档的状态，引擎会随文档销毁；壳侧面板连同未提交的命名
   * 一起复位（命名缓冲留着会把旧名字带给新文档）。 */
  channel_store_clear(shell);
  /*
   * 历史面板没有壳侧数据（列表直接来自引擎），换文档后可以继续开着显示新文档的
   * 历史；只把滚动归零，否则可能停在新文档根本到不了的行上。
   */
  shell.doc.history_scroll = 0;
  /*
   * ROI #3：参考线拖动持有旧文档的事务与捕获，必须在旧文档被销毁**之前**收尾
   * （abort 打到已释放对象上就是 UAF，同 color_range 那条注释）。视图开关是
   * 按文档存的，新文档一律回到默认：标尺/网格默认关（它们会盖住画面），
   * 参考线可见与吸附默认开（对齐开关只影响交互，不影响像素）。
   */
  guide_drag_forget(shell);
  shell.doc.show_rulers = false;
  shell.doc.show_guides = true;
  shell.doc.show_grid = false;
  shell.doc.snap_enabled = true;
}

bool create_document_with(ShellState& shell, int width, int height, int dpi, int background) {
  comp_document_desc desc{};
  desc.width = comp_ui::clamp_doc_size(width);
  desc.height = comp_ui::clamp_doc_size(height);
  desc.dpi = static_cast<float>(comp_ui::clamp_doc_dpi(dpi));
  desc.color_space = "sRGB";

  comp_document document = nullptr;
  const comp_result result = comp_document_create(shell.engine, &desc, &document);
  if (result != COMP_OK || document == nullptr) {
    set_status(shell, L"新建文档失败");
    return false;
  }
  forget_color_range(shell);
  if (shell.doc.document != nullptr) comp_document_destroy(shell.doc.document);
  shell.doc.document = document;
  /* 新文档没有磁盘来源；dpi 记下来给标题栏用（引擎不提供回读口）。 */
  shell.doc.document_path.clear();
  shell.doc.document_dpi = static_cast<int>(desc.dpi);

  comp_size size{};
  if (comp_document_size(shell.doc.document, &size) == COMP_OK) {
    shell.doc.width = size.width;
    shell.doc.height = size.height;
  }
  const size_t pixel_count = static_cast<size_t>(shell.doc.width) * static_cast<size_t>(shell.doc.height);
  shell.doc.rgba.assign(pixel_count * 4, 0);
  shell.doc.framebuffer.assign(pixel_count, 0);
  shell.doc.needs_render = true;
  shell.doc.needs_fit = true;
  shell.doc.visibility.clear();
  /* 新文档没有「之前的滤镜面板/未完成笔迹」可言，一并复位，避免跨文档误应用。 */
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell); /* 旧文档的预览帧绝不能留在新文档的画布上 */
  shell.doc.filter.select(comp_ui::FilterId::Vignette);
  /* Progressive Blur 的渐变轴端点是旧文档坐标，换文档后一并作废。 */
  shell.doc.progressive_axis_set = false;
  shell.doc.progressive_axis_dragging = false;
  /* 仿制图章的源点同理：它属于旧文档的像素网格。 */
  clone_reset_source(shell);
  stroke_cancel(shell);
  move_cancel(shell);
  eyedropper_cancel(shell);
  marquee_cancel(shell);
  /* §5.44 换文档时手柄拖动的预览属于旧文档坐标，一并作废。 */
  selection_handle_cancel(shell);
  lasso_cancel(shell);
  pen_cancel(shell);
  refresh_layers(shell);
  refresh_selection(shell);
  if (!fill_document_background(shell, background)) {
    set_status(shell, L"新建文档成功，但背景填充失败（画布保持透明）");
    return true;
  }
  shell.doc.needs_render = true;
  refresh_history_status(shell, L"已新建文档");
  return true;
}

/* 旧入口（Ctrl+N / 启动 / 新建菜单）：行为与历史完全一致，透明背景 + 默认尺寸。 */
bool create_document(ShellState& shell) {
  return create_document_with(shell, kDefaultWidth, kDefaultHeight, 72, 1);
}

bool load_document(ShellState& shell, const char* path_utf8) {
  comp_document document = nullptr;
  if (comp_document_load(shell.engine, path_utf8, &document) != COMP_OK || document == nullptr) {
    set_status(shell, L"打开 .comp 失败");
    return false;
  }
  forget_color_range(shell);
  if (shell.doc.document != nullptr) comp_document_destroy(shell.doc.document);
  shell.doc.document = document;

  comp_size size{};
  if (comp_document_size(shell.doc.document, &size) == COMP_OK) {
    shell.doc.width = size.width;
    shell.doc.height = size.height;
  }
  const size_t pixel_count = static_cast<size_t>(shell.doc.width) * static_cast<size_t>(shell.doc.height);
  shell.doc.rgba.assign(pixel_count * 4, 0);
  shell.doc.framebuffer.assign(pixel_count, 0);
  shell.doc.needs_render = true;
  shell.doc.needs_fit = true;
  shell.doc.visibility.clear();
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell);
  shell.doc.filter.select(comp_ui::FilterId::Vignette);
  shell.doc.progressive_axis_set = false;
  shell.doc.progressive_axis_dragging = false;
  clone_reset_source(shell);
  stroke_cancel(shell);
  move_cancel(shell);
  eyedropper_cancel(shell);
  marquee_cancel(shell);
  /* §5.44 换文档时手柄拖动的预览属于旧文档坐标，一并作废。 */
  selection_handle_cancel(shell);
  lasso_cancel(shell);
  pen_cancel(shell);
  refresh_layers(shell);
  refresh_selection(shell);
  /* 标题栏的文件名只在 .comp 上有意义：从位图打开的是「未命名文档」。 */
  shell.doc.document_path.assign(path_utf8);
  shell.doc.document_dpi = 0; /* 读盘没有 dpi 回读口：标题里省略，不瞎猜 72 */
  refresh_history_status(shell, L"已打开 .comp");
  return true;
}

bool save_document(ShellState& shell, const char* path_utf8) {
  if (shell.doc.document == nullptr) return false;
  const bool ok = comp_document_save(shell.doc.document, path_utf8) == COMP_OK;
  if (ok) shell.doc.document_path.assign(path_utf8);
  set_status(shell, ok ? L"已保存 .comp" : L"保存 .comp 失败");
  return ok;
}

/* ------------------------------------------------------------ 撤销 / 重做 */

void undo_document(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t can_undo = 0;
  if (comp_document_can_undo(shell.doc.document, &can_undo) != COMP_OK || can_undo == 0) {
    set_status(shell, L"已到最早状态，无可撤销");
    return;
  }
  const comp_result result = comp_document_undo(shell.doc.document);
  if (result != COMP_OK) {
    set_status(shell, L"撤销失败");
    return;
  }
  shell.doc.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell); /* 选区操作也在撤销栈里：撤销可能改变蚂蚁线 */
  refresh_history_status(shell, L"已撤销");
}

void redo_document(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t can_redo = 0;
  if (comp_document_can_redo(shell.doc.document, &can_redo) != COMP_OK || can_redo == 0) {
    set_status(shell, L"已到最新状态，无可重做");
    return;
  }
  const comp_result result = comp_document_redo(shell.doc.document);
  if (result != COMP_OK) {
    set_status(shell, L"重做失败");
    return;
  }
  shell.doc.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell); /* 同撤销：重做也可能恢复/撤掉选区 */
  refresh_history_status(shell, L"已重做");
}

/* 命令层冒烟：两段式枚举，只报数量，不猜测命令 id。 */
void list_commands(ShellState& shell) {
  if (shell.doc.document == nullptr) return;
  size_t count = 0;
  /* count-only 模式不要求文档；带 desc 的枚举才需要按文档状态求值 enabled。 */
  if (comp_command_list(shell.doc.document, nullptr, 0, &count) != COMP_OK) {
    set_status(shell, L"命令枚举失败");
    return;
  }
  wchar_t buffer[96];
  wsprintfW(buffer, L"引擎注册命令 %u 条", static_cast<unsigned>(count));
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 文件对话框 */

void open_document_dialog(HWND window, ShellState& shell) {
  wchar_t buffer[MAX_PATH] = L"";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter = L"Compositor 项目 (*.comp)\0*.comp\0所有文件\0*.*\0";
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&dialog)) return;
  char utf8[MAX_PATH * 3] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  load_document(shell, utf8);
}

void save_document_dialog(HWND window, ShellState& shell) {
  if (shell.doc.document == nullptr) return;
  wchar_t buffer[MAX_PATH] = L"";
  /* 已保存过的文档把当前路径填进去：另存为/首次保存都少敲一遍目录。 */
  if (!shell.doc.document_path.empty()) {
    MultiByteToWideChar(CP_UTF8, 0, shell.doc.document_path.c_str(), -1, buffer, MAX_PATH);
    buffer[MAX_PATH - 1] = L'\0';
  }
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter = L"Compositor 项目 (*.comp)\0*.comp\0";
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrDefExt = L"comp";
  if (!GetSaveFileNameW(&dialog)) return;
  char utf8[MAX_PATH * 3] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  save_document(shell, utf8);
}

/*
 * Ctrl+S：有磁盘来源就直接写回（与上游 Save 一致），没有才弹另存为。
 * 这样「保存」不打断手头工作，而「另存为」仍是一个显式入口。
 */
void save_document_quick(HWND window, ShellState& shell) {
  if (shell.doc.document == nullptr) return;
  if (!shell.doc.document_path.empty()) {
    save_document(shell, shell.doc.document_path.c_str());
    return;
  }
  save_document_dialog(window, shell);
}

/* ------------------------------------------------- 文档级对话框（P0 接线） */

/*
 * 行 -> 值 / 值 -> 行：唯一映射点。行序必须与 ui_core::doc_dialog_row 完全相同
 * （单元测试 ui_core_doc_dialog_rows 钉住行数与行序）。所有写入都经
 * doc_dialog_set_row_value，夹取一次、联动一次，绘制与状态栏读的是同一份值。
 */
int doc_dialog_row_value(const ShellState& shell, int row) {
  switch (shell.doc.doc_dialog) {
    case comp_ui::DocDialogKind::NewDocument:
      switch (row) {
        case 0: return shell.doc.doc_dialog_width;
        case 1: return shell.doc.doc_dialog_height;
        case 2: return shell.doc.doc_dialog_dpi;
        case 3: return shell.doc.doc_dialog_background;
        default: return 0;
      }
    case comp_ui::DocDialogKind::ImageSize:
      switch (row) {
        case 0: return shell.doc.doc_dialog_width;
        case 1: return shell.doc.doc_dialog_height;
        case 2: return shell.doc.doc_dialog_lock_aspect;
        /* 百分比不另存一份：始终由「当前宽 / 基准宽」现算，来回切换不会漂移。 */
        case 3:
          return comp_ui::doc_percent_from_size(shell.doc.doc_dialog_base_width,
                                                shell.doc.doc_dialog_width);
        default: return 0;
      }
    case comp_ui::DocDialogKind::CanvasSize:
      switch (row) {
        case 0: return shell.doc.doc_dialog_width;
        case 1: return shell.doc.doc_dialog_height;
        case 2: return shell.doc.doc_dialog_anchor;
        default: return 0;
      }
    case comp_ui::DocDialogKind::HistoryLimit:
      switch (row) {
        case 0: return shell.doc.doc_dialog_steps;
        case 1: return shell.doc.doc_dialog_budget_mb;
        default: return 0;
      }
    case comp_ui::DocDialogKind::CameraRaw:
      if (row < 0 || row >= comp_ui::kCameraRawRowCount) return 0;
      return shell.doc.camera_raw_values[row];
    case comp_ui::DocDialogKind::ClosePrompt:
      if (row != 0) return 0;
      return shell.doc.doc_dialog_close_action;
    case comp_ui::DocDialogKind::TransformSelection:
      switch (row) {
        case 0: return shell.doc.doc_dialog_width;
        case 1: return shell.doc.doc_dialog_height;
        case 2: return shell.doc.doc_dialog_angle;
        default: return 0;
      }
    default:
      return 0;
  }
}

void doc_dialog_set_row_value(ShellState& shell, int row, int value) {
  switch (shell.doc.doc_dialog) {
    case comp_ui::DocDialogKind::NewDocument:
      switch (row) {
        case 0: shell.doc.doc_dialog_width = comp_ui::clamp_doc_size(value); break;
        case 1: shell.doc.doc_dialog_height = comp_ui::clamp_doc_size(value); break;
        case 2: shell.doc.doc_dialog_dpi = comp_ui::clamp_doc_dpi(value); break;
        case 3: shell.doc.doc_dialog_background = value < 0 ? 0 : (value > 2 ? 2 : value); break;
        default: break;
      }
      break;
    case comp_ui::DocDialogKind::ImageSize:
      switch (row) {
        case 0:
          shell.doc.doc_dialog_width = comp_ui::clamp_doc_size(value);
          if (shell.doc.doc_dialog_lock_aspect != 0) {
            shell.doc.doc_dialog_height = comp_ui::doc_locked_height(
                shell.doc.doc_dialog_base_width, shell.doc.doc_dialog_base_height,
                shell.doc.doc_dialog_width);
          }
          break;
        case 1:
          shell.doc.doc_dialog_height = comp_ui::clamp_doc_size(value);
          if (shell.doc.doc_dialog_lock_aspect != 0) {
            shell.doc.doc_dialog_width = comp_ui::doc_locked_width(
                shell.doc.doc_dialog_base_width, shell.doc.doc_dialog_base_height,
                shell.doc.doc_dialog_height);
          }
          break;
        case 2:
          shell.doc.doc_dialog_lock_aspect = value != 0 ? 1 : 0;
          /* 刚打开约束就立刻对齐到原始比例，不等到用户再动一次尺寸。 */
          if (shell.doc.doc_dialog_lock_aspect != 0) {
            shell.doc.doc_dialog_height = comp_ui::doc_locked_height(
                shell.doc.doc_dialog_base_width, shell.doc.doc_dialog_base_height,
                shell.doc.doc_dialog_width);
          }
          break;
        case 3: {
          int percent = value;
          if (percent < 1) percent = 1;
          if (percent > 10000) percent = 10000;
          shell.doc.doc_dialog_width = comp_ui::doc_size_from_percent(
              shell.doc.doc_dialog_base_width, percent);
          shell.doc.doc_dialog_height = comp_ui::doc_size_from_percent(
              shell.doc.doc_dialog_base_height, percent);
          if (shell.doc.doc_dialog_lock_aspect != 0) {
            shell.doc.doc_dialog_height = comp_ui::doc_locked_height(
                shell.doc.doc_dialog_base_width, shell.doc.doc_dialog_base_height,
                shell.doc.doc_dialog_width);
          }
          break;
        }
        default:
          break;
      }
      break;
    case comp_ui::DocDialogKind::CanvasSize:
      switch (row) {
        case 0: shell.doc.doc_dialog_width = comp_ui::clamp_doc_size(value); break;
        case 1: shell.doc.doc_dialog_height = comp_ui::clamp_doc_size(value); break;
        case 2:
          if (value < 0) value = 0;
          if (value > 8) value = 8;
          shell.doc.doc_dialog_anchor = value;
          break;
        default: break;
      }
      break;
    case comp_ui::DocDialogKind::HistoryLimit:
      switch (row) {
        case 0: shell.doc.doc_dialog_steps = comp_ui::clamp_history_steps(value); break;
        case 1: shell.doc.doc_dialog_budget_mb = comp_ui::clamp_history_budget_mb(value); break;
        default: break;
      }
      break;
    case comp_ui::DocDialogKind::CameraRaw:
      if (row < 0 || row >= comp_ui::kCameraRawRowCount) break;
      /* 夹取区间取自行规格本身：量程改了这里不用跟着改，引擎侧不会收到越界值。 */
      {
        const comp_ui::DocRowSpec spec = comp_ui::doc_dialog_row(shell.doc.doc_dialog, row);
        int clamped = value;
        if (clamped < spec.min) clamped = spec.min;
        if (clamped > spec.max) clamped = spec.max;
        shell.doc.camera_raw_values[row] = clamped;
      }
      break;
    case comp_ui::DocDialogKind::ClosePrompt:
      if (row != 0) break;
      if (value < comp_ui::kCloseActionSave) value = comp_ui::kCloseActionSave;
      if (value > comp_ui::kCloseActionCancel) value = comp_ui::kCloseActionCancel;
      shell.doc.doc_dialog_close_action = value;
      break;
    case comp_ui::DocDialogKind::TransformSelection:
      /* 夹取区间同样取自行规格（宽/高 = 文档尺寸量程，角度 = ±180）。
       * 宽/高允许到 1，退化到 0 由「收成空选区」分支处理，所以这里不额外抬。 */
      {
        const comp_ui::DocRowSpec spec = comp_ui::doc_dialog_row(shell.doc.doc_dialog, row);
        int clamped = value;
        if (clamped < spec.min) clamped = spec.min;
        if (clamped > spec.max) clamped = spec.max;
        switch (row) {
          case 0: shell.doc.doc_dialog_width = clamped; break;
          case 1: shell.doc.doc_dialog_height = clamped; break;
          case 2: shell.doc.doc_dialog_angle = clamped; break;
          default: break;
        }
      }
      break;
    default:
      break;
  }
}

/* 行值的显示文本：绘制与状态栏共用，避免「面板 72 dpi、状态栏 72 px」两套说法。 */
std::wstring doc_dialog_value_text(const ShellState& shell, int row) {
  const comp_ui::DocRowSpec spec = comp_ui::doc_dialog_row(shell.doc.doc_dialog, row);
  const int value = doc_dialog_row_value(shell, row);
  wchar_t buffer[64];
  switch (spec.kind) {
    case comp_ui::DocRowKind::Toggle:
      return value != 0 ? L"开" : L"关";
    case comp_ui::DocRowKind::Choice:
      if (shell.doc.doc_dialog == comp_ui::DocDialogKind::NewDocument) {
        return widen_utf8(comp_ui::doc_new_background_name(value));
      }
      if (shell.doc.doc_dialog == comp_ui::DocDialogKind::ClosePrompt) {
        return widen_utf8(comp_ui::doc_close_action_name(value));
      }
      return widen_utf8(comp_ui::doc_anchor_name(value));
    case comp_ui::DocRowKind::Number:
      break;
  }
  switch (shell.doc.doc_dialog) {
    case comp_ui::DocDialogKind::NewDocument:
      if (row == 2) {
        wsprintfW(buffer, L"%d dpi", value);
        return buffer;
      }
      wsprintfW(buffer, L"%d px", value);
      return buffer;
    case comp_ui::DocDialogKind::ImageSize:
      if (row == 3) {
        wsprintfW(buffer, L"%d %%", value);
        return buffer;
      }
      wsprintfW(buffer, L"%d px", value);
      return buffer;
    case comp_ui::DocDialogKind::CanvasSize:
      wsprintfW(buffer, L"%d px", value);
      return buffer;
    case comp_ui::DocDialogKind::TransformSelection:
      if (row == 2) {
        wsprintfW(buffer, L"%d 度", value);
      } else {
        wsprintfW(buffer, L"%d px", value);
      }
      return buffer;
    case comp_ui::DocDialogKind::HistoryLimit:
      if (row == 0) {
        wsprintfW(buffer, L"%d 步", value);
      } else {
        wsprintfW(buffer, L"%d MiB", value);
      }
      return buffer;
    case comp_ui::DocDialogKind::CameraRaw: {
      /*
       * wsprintfW（user32）**不支持浮点格式**——P0-3 的曲线面板曾因此在 %f 上崩。
       * 曝光按整数拆成「元/角分」两段拼，既避开 %f，也保证显示与写进 JSON 的
       * 数值同源（都是同一个 0.01 EV 整数）。
       */
      if (comp_ui::camera_raw_row_is_exposure(row)) {
        const int magnitude = value < 0 ? -value : value;
        wsprintfW(buffer, L"%s%d.%02d EV", value < 0 ? L"-" : L"+", magnitude / 100,
                  magnitude % 100);
      } else {
        wsprintfW(buffer, value > 0 ? L"+%d" : L"%d", value);
      }
      return buffer;
    }
    default:
      return L"";
  }
}

void doc_dialog_close(ShellState& shell, const wchar_t* status) {
  shell.doc.doc_dialog = comp_ui::DocDialogKind::None;
  shell.doc.doc_dialog_input.clear();
  if (status != nullptr) set_status(shell, status);
}

void doc_dialog_begin(ShellState& shell, comp_ui::DocDialogKind kind) {
  if (kind == comp_ui::DocDialogKind::None) return;
  /* 新建不依赖当前文档；关闭提示的成立条件由 WM_CLOSE 那边先查（脏标记仍读引擎）。 */
  if (kind != comp_ui::DocDialogKind::NewDocument &&
      kind != comp_ui::DocDialogKind::ClosePrompt && shell.doc.document == nullptr) {
    set_status(shell, L"文档：还没有打开任何文档");
    return;
  }
  /*
   * Camera Raw 是**图层级**的破坏性操作，入口条件比文档级对话框多两条：
   * 必须有目标图层，且引擎手里得有实测表（否则 comp_camera_raw_apply 只会回
   * COMP_ERR_STATE）。两者都在打开前查清，用户不会白填一轮参数才被拒。
   */
  if (kind == comp_ui::DocDialogKind::CameraRaw) {
    if (selected_layer_id(shell) == 0) {
      set_status(shell, L"Camera Raw：请先在图层面板选中一个图层");
      return;
    }
    if (!shell.camera_raw_tables_loaded) {
      set_status(shell, L"Camera Raw：色彩表未加载（缺 resources/camera_raw_tables.bin）");
      return;
    }
  }
  if (kind == comp_ui::DocDialogKind::TransformSelection) {
    /*
     * 变换选区要有一张真的能变换的蒙版：selection_active 对**显式空选区**也为真，
     * 所以这里看基础蒙版的紧包围盒——没有可变换的几何就直接回退，不开空面板。
     */
    if (shell.doc.selection_base.w <= 0 || shell.doc.selection_base.h <= 0) {
      set_status(shell, L"变换选区：请先建立一个选区");
      return;
    }
  }
  /* 右栏一次只挂一个面板（与滤镜/效果/修边/路径/历史/色彩范围互斥）。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.path_open = false;
  shell.doc.history_open = false;
  channel_panel_close(shell);
  shell.doc.trim_open = false;
  shell.doc.trim_input.clear();

  shell.doc.doc_dialog = kind;
  shell.doc.doc_dialog_focus = 0;
  shell.doc.doc_dialog_input.clear();
  shell.doc.doc_dialog_lock_aspect = 1;
  shell.doc.doc_dialog_anchor = 4;
  switch (kind) {
    case comp_ui::DocDialogKind::NewDocument:
      /* 记住上次的新建设置（与上游 Remember Last Used Settings 同效）。 */
      shell.doc.doc_dialog_width = shell.new_doc_width;
      shell.doc.doc_dialog_height = shell.new_doc_height;
      shell.doc.doc_dialog_dpi = shell.new_doc_dpi;
      shell.doc.doc_dialog_background = shell.new_doc_background;
      shell.doc.doc_dialog_base_width = shell.new_doc_width;
      shell.doc.doc_dialog_base_height = shell.new_doc_height;
      break;
    case comp_ui::DocDialogKind::ImageSize:
      shell.doc.doc_dialog_base_width = shell.doc.width;
      shell.doc.doc_dialog_base_height = shell.doc.height;
      shell.doc.doc_dialog_width = shell.doc.width;
      shell.doc.doc_dialog_height = shell.doc.height;
      break;
    case comp_ui::DocDialogKind::CanvasSize:
      shell.doc.doc_dialog_base_width = shell.doc.width;
      shell.doc.doc_dialog_base_height = shell.doc.height;
      shell.doc.doc_dialog_width = shell.doc.width;
      shell.doc.doc_dialog_height = shell.doc.height;
      break;
    case comp_ui::DocDialogKind::HistoryLimit:
      /* ABI 没有 limit/budget 的 getter：0 = 引擎默认，回显如实写 0。 */
      shell.doc.doc_dialog_steps = 0;
      shell.doc.doc_dialog_budget_mb = 0;
      break;
    case comp_ui::DocDialogKind::CameraRaw:
      /*
       * ABI 同样没有「读回某图层当前 Camera Raw 参数」的 getter 级入口（设置是
       * 破坏性写进像素的，不留在图层上）。面板因此是**草稿**：每次打开归零，
       * 副标题已如实写明「对选中图层整层写入」。
       */
      for (int row = 0; row < comp_ui::kCameraRawRowCount; ++row) {
        shell.doc.camera_raw_values[row] = 0;
      }
      break;
    case comp_ui::DocDialogKind::ClosePrompt:
      /* 缺省落在「保存」：回车是安全动作，误触不会丢像素。 */
      shell.doc.doc_dialog_close_action = comp_ui::kCloseActionSave;
      break;
    case comp_ui::DocDialogKind::TransformSelection:
      /*
       * 起点 = 当前基础蒙版的紧包围盒、角度 0：不填任何数直接回车就是**恒等变换**
       * （引擎 masks_equal 短路由，changed = 0，不落撤销步）。中心点不另存：
       * 应用时现取包围盒中心，避免面板开着时选区被别的命令改动后中心漂移。
       */
      shell.doc.doc_dialog_width = comp_ui::clamp_doc_size(shell.doc.selection_base.w);
      shell.doc.doc_dialog_height = comp_ui::clamp_doc_size(shell.doc.selection_base.h);
      shell.doc.doc_dialog_angle = 0;
      break;
    default:
      break;
  }
  wchar_t buffer[192];
  if (kind == comp_ui::DocDialogKind::ClosePrompt) {
    /* 这个面板没有数值可填：状态栏直接给三个动作的键，不照抄通用提示。 */
    set_status(shell, L"关闭文档：S 保存并关闭 · D 不保存 · Esc 取消");
    return;
  }
  wsprintfW(buffer, L"%s：Tab 切行 · 回车应用 · Esc 取消",
            widen_utf8(comp_ui::doc_dialog_title(kind)).c_str());
  set_status(shell, buffer);
}

void doc_dialog_focus_row(ShellState& shell, int delta) {
  if (shell.doc.doc_dialog == comp_ui::DocDialogKind::None) return;
  shell.doc.doc_dialog_input.clear();
  const int count = comp_ui::doc_dialog_row_count(shell.doc.doc_dialog);
  if (count <= 0) return;
  shell.doc.doc_dialog_focus =
      ((shell.doc.doc_dialog_focus + delta) % count + count) % count;
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s %d/%d：%s", widen_utf8(comp_ui::doc_dialog_title(shell.doc.doc_dialog)).c_str(),
            shell.doc.doc_dialog_focus + 1, count,
            widen_utf8(comp_ui::doc_dialog_row(shell.doc.doc_dialog, shell.doc.doc_dialog_focus).label)
                .c_str());
  set_status(shell, buffer);
}

void doc_dialog_step(ShellState& shell, int direction) {
  if (shell.doc.doc_dialog == comp_ui::DocDialogKind::None) return;
  const int row = shell.doc.doc_dialog_focus;
  const comp_ui::DocRowSpec spec = comp_ui::doc_dialog_row(shell.doc.doc_dialog, row);
  const int current = doc_dialog_row_value(shell, row);
  int next = current;
  if (spec.kind == comp_ui::DocRowKind::Number) {
    const long long raw = static_cast<long long>(current) +
                          static_cast<long long>(spec.step) * direction;
    next = raw < spec.min ? spec.min : (raw > spec.max ? spec.max : static_cast<int>(raw));
  } else if (spec.kind == comp_ui::DocRowKind::Toggle) {
    next = current != 0 ? 0 : 1;
  } else {
    const int choices = spec.choice_count > 0 ? spec.choice_count : 1;
    next = ((current + direction) % choices + choices) % choices;
  }
  doc_dialog_set_row_value(shell, row, next);
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s = %s", widen_utf8(spec.label).c_str(),
            doc_dialog_value_text(shell, row).c_str());
  set_status(shell, buffer);
}

void doc_dialog_begin_input(ShellState& shell, wchar_t character) {
  if (shell.doc.doc_dialog == comp_ui::DocDialogKind::None) return;
  const comp_ui::DocRowSpec spec =
      comp_ui::doc_dialog_row(shell.doc.doc_dialog, shell.doc.doc_dialog_focus);
  if (spec.kind != comp_ui::DocRowKind::Number) return;
  /*
   * 负号（Camera Raw 的大部分行）：只许出现在**负值区间**的行的**行首**。
   * 其它位置直接忽略——若把 '-' 当噪声跳过，"-5" 会静默变成 +5，那是最坏的失败方式。
   */
  if (character == L'-' && (spec.min >= 0 || !shell.doc.doc_dialog_input.empty())) return;
  /* 30000 / 16384 / 10000 都是五位；六位只可能是误按，留着没意义。 */
  if (shell.doc.doc_dialog_input.size() >= 5) return;
  shell.doc.doc_dialog_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.doc.doc_dialog_input.c_str());
  set_status(shell, buffer);
}

void doc_dialog_commit_input(ShellState& shell) {
  if (shell.doc.doc_dialog_input.empty()) return;
  const int row = shell.doc.doc_dialog_focus;
  const comp_ui::DocRowSpec spec = comp_ui::doc_dialog_row(shell.doc.doc_dialog, row);
  bool negative = false;
  size_t digits_from = 0;
  if (shell.doc.doc_dialog_input[0] == L'-') {
    negative = true;
    digits_from = 1;
  }
  int value = 0;
  for (size_t index = digits_from; index < shell.doc.doc_dialog_input.size(); ++index) {
    const wchar_t character = shell.doc.doc_dialog_input[index];
    if (character >= L'0' && character <= L'9') value = value * 10 + (character - L'0');
  }
  if (negative) value = -value;
  shell.doc.doc_dialog_input.clear();
  if (spec.kind != comp_ui::DocRowKind::Number) return;
  /* 键入值按行的合法区间夹取（0 在历史两项里是合法值「引擎默认」）。 */
  if (value < spec.min) value = spec.min;
  if (value > spec.max) value = spec.max;
  doc_dialog_set_row_value(shell, row, value);
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s = %s", widen_utf8(spec.label).c_str(),
            doc_dialog_value_text(shell, row).c_str());
  set_status(shell, buffer);
}

/*
 * 尺寸变化后的缓冲同步：rgba/framebuffer 是**按旧尺寸**分配的，render_canvas 直接
 * 按 shell.doc.width/height 写进去。这里只在长度变化时重分配（等于旧尺寸的旋转/修边
 * 不受影响），不引入额外的每帧开销。
 */
void sync_document_buffers(ShellState& shell) {
  comp_size size{};
  if (shell.doc.document != nullptr && comp_document_size(shell.doc.document, &size) == COMP_OK) {
    shell.doc.width = size.width;
    shell.doc.height = size.height;
  }
  const size_t pixel_count =
      static_cast<size_t>(shell.doc.width) * static_cast<size_t>(shell.doc.height);
  if (shell.doc.rgba.size() != pixel_count * 4) shell.doc.rgba.assign(pixel_count * 4, 0);
  if (shell.doc.framebuffer.size() != pixel_count) shell.doc.framebuffer.assign(pixel_count, 0);
}

void doc_dialog_apply(HWND window, ShellState& shell) {
  const comp_ui::DocDialogKind kind = shell.doc.doc_dialog;
  if (kind == comp_ui::DocDialogKind::None) return;
  /* 键入态下直接回车 = 先落值再应用，少按一次键（与滤镜面板同一约定）。 */
  if (!shell.doc.doc_dialog_input.empty()) doc_dialog_commit_input(shell);

  /*
   * 关闭提示（P0-3）：动作作用在**窗口**上而不是文档内容上，所以排在所有
   * 文档级分支之前。三条路径的共同底线是「没保存成功就不关窗」——宁可让用户
   * 再选一次，也不能出现「以为存了其实丢了」。
   */
  if (kind == comp_ui::DocDialogKind::ClosePrompt) {
    const int action = shell.doc.doc_dialog_close_action;
    if (action == comp_ui::kCloseActionCancel) {
      doc_dialog_close(shell, L"已取消关闭（文档未改动）");
      return;
    }
    if (action == comp_ui::kCloseActionSave) {
      if (!shell.doc.document_path.empty()) {
        if (!save_document(shell, shell.doc.document_path.c_str())) {
          doc_dialog_close(shell, L"保存失败，窗口未关闭");
          return;
        }
      } else {
        /*
         * 没有磁盘来源 = 走另存为。用户取消系统对话框时脏标记仍在，据此判断
         * 「真的存下去了」而不是只看函数有没有被调用过。
         */
        save_document_dialog(window, shell);
        int32_t dirty = 0;
        if (shell.doc.document == nullptr || shell.doc.document_path.empty() ||
            comp_document_is_dirty(shell.doc.document, &dirty) != COMP_OK || dirty != 0) {
          doc_dialog_close(shell, L"未保存（另存为已取消），窗口未关闭");
          return;
        }
      }
    }
    /* 保存成功或「不保存」：关窗即退出（WM_DESTROY 里 PostQuitMessage）。 */
    doc_dialog_close(shell, nullptr);
    DestroyWindow(window);
    return;
  }

  if (kind == comp_ui::DocDialogKind::NewDocument) {
    const int width = shell.doc.doc_dialog_width;
    const int height = shell.doc.doc_dialog_height;
    const int dpi = shell.doc.doc_dialog_dpi;
    const int background = shell.doc.doc_dialog_background;
    /* 先记设置再建：建失败也保留用户刚填的参数。 */
    shell.new_doc_width = width;
    shell.new_doc_height = height;
    shell.new_doc_dpi = dpi;
    shell.new_doc_background = background;
    if (!create_document_with(shell, width, height, dpi, background)) {
      doc_dialog_close(shell, L"新建文档失败（未改动当前文档）");
      return;
    }
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[192];
    wsprintfW(buffer, L"已新建 %d×%d @%d dpi（背景：%s）", width, height, dpi,
              widen_utf8(comp_ui::doc_new_background_name(background)).c_str());
    refresh_history_status(shell, buffer);
    return;
  }

  if (shell.doc.document == nullptr) {
    doc_dialog_close(shell, L"文档：还没有打开任何文档");
    return;
  }

  if (kind == comp_ui::DocDialogKind::ImageSize) {
    const int width = shell.doc.doc_dialog_width;
    const int height = shell.doc.doc_dialog_height;
    if (width == shell.doc.width && height == shell.doc.height) {
      set_status(shell, L"图像大小：尺寸未变（未产生撤销步）");
      return;
    }
    comp_size size{};
    size.width = width;
    size.height = height;
    const comp_result result = comp_document_resize_image(shell.doc.document, size);
    if (result != COMP_OK) {
      set_status(shell, L"图像大小：重采样失败（文档未改动）");
      return;
    }
    sync_document_buffers(shell);
    finish_canvas_command(shell);
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[192];
    wsprintfW(buffer, L"已重采样为 %d×%d（选区已丢弃）", width, height);
    refresh_history_status(shell, buffer);
    return;
  }

  if (kind == comp_ui::DocDialogKind::CanvasSize) {
    const int width = shell.doc.doc_dialog_width;
    const int height = shell.doc.doc_dialog_height;
    const int delta_width = width - shell.doc.width;
    const int delta_height = height - shell.doc.height;
    if (delta_width == 0 && delta_height == 0) {
      set_status(shell, L"画布大小：尺寸未变（未产生撤销步）");
      return;
    }
    comp_txn txn = nullptr;
    if (comp_txn_begin(shell.doc.document, "Canvas Size", &txn) != COMP_OK || txn == nullptr) {
      set_status(shell, L"画布大小：无法开启事务（未改动）");
      return;
    }
    const int dx = comp_ui::doc_anchor_dx(shell.doc.doc_dialog_anchor, delta_width);
    const int dy = comp_ui::doc_anchor_dy(shell.doc.doc_dialog_anchor, delta_height);
    bool ok = true;
    if (dx != 0 || dy != 0) {
      /*
       * transform 是每层**文档坐标**下的绝对落位（引擎的转画布也是逐层改
       * transform，不区分根/子层），所以按锚点平移必须遍历**引擎里的全部图层**。
       * 这里刻意不用 shell.doc.layers：那是面板的可见行（折叠组里的子层不在其中，
       * 按它挪会只挪组不挪子层，整组就散了）。comp_layer_list 给的是堆叠序全量。
       */
      size_t count = 0;
      ok = comp_layer_list(shell.doc.document, nullptr, 0, &count) == COMP_OK;
      std::vector<comp_layer_id> ids(count);
      if (ok && count > 0) {
        ok = comp_layer_list(shell.doc.document, ids.data(), count, &count) == COMP_OK;
        ids.resize(count);
      }
      for (size_t index = 0; ok && index < ids.size(); ++index) {
        comp_rect transform{};
        float rotation = 0.0f;
        if (comp_layer_get_transform(shell.doc.document, ids[index], &transform, &rotation) !=
            COMP_OK) {
          ok = false;
          break;
        }
        transform.x += dx;
        transform.y += dy;
        if (comp_layer_set_transform(shell.doc.document, ids[index], transform, rotation) !=
            COMP_OK) {
          ok = false;
          break;
        }
      }
    }
    if (ok) {
      comp_size size{};
      size.width = width;
      size.height = height;
      ok = comp_document_set_size(shell.doc.document, size) == COMP_OK;
    }
    if (!ok) {
      comp_txn_abort(txn);
      set_status(shell, L"画布大小：应用失败（已回滚，文档未改动）");
      return;
    }
    if (comp_txn_commit(txn) != COMP_OK) {
      set_status(shell, L"画布大小：提交失败（文档未改动）");
      return;
    }
    sync_document_buffers(shell);
    finish_canvas_command(shell);
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[224];
    wsprintfW(buffer, L"已改画布为 %d×%d（锚点 %s）", width, height,
              widen_utf8(comp_ui::doc_anchor_name(shell.doc.doc_dialog_anchor)).c_str());
    refresh_history_status(shell, buffer);
    return;
  }

  if (kind == comp_ui::DocDialogKind::HistoryLimit) {
    const int steps = shell.doc.doc_dialog_steps;
    const size_t budget_bytes =
        static_cast<size_t>(shell.doc.doc_dialog_budget_mb) * 1024ull * 1024ull;
    const comp_result limit_result =
        comp_document_set_history_limit(shell.doc.document, static_cast<size_t>(steps));
    const comp_result budget_result =
        comp_document_set_history_budget(shell.doc.document, budget_bytes);
    if (limit_result != COMP_OK || budget_result != COMP_OK) {
      set_status(shell, L"历史容量：设置失败（引擎拒绝，可能已部分生效）");
      return;
    }
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[192];
    wsprintfW(buffer, L"历史容量：%d 步 / %d MiB", steps, shell.doc.doc_dialog_budget_mb);
    refresh_history_status(shell, buffer);
    return;
  }

  if (kind == comp_ui::DocDialogKind::CameraRaw) {
    const comp_layer_id target = selected_layer_id(shell);
    if (target == 0) {
      doc_dialog_close(shell, L"Camera Raw：目标图层已不存在");
      return;
    }
    /*
     * 只把**非零**行写进 JSON：0 就是上游缺省值，省掉它们让「设置等于默认」
     * 干干净净地变成 "{}"——引擎据此回 changed=0、不开事务（见 comp_camera_raw.h
     * 的恒等契约）。键名与缺省判定都在 ui_core.h 里单测过，引擎对未知键是硬拒绝。
     */
    const std::string settings = comp_ui::camera_raw_settings_json(
        shell.doc.camera_raw_values, comp_ui::kCameraRawRowCount);
    int32_t changed = 0;
    char note[256] = {0};
    size_t note_size = 0;
    const comp_result result = comp_camera_raw_apply(
        shell.doc.document, target, settings.c_str(), 1.0, /*seed=*/0u, &changed, note, sizeof(note),
        &note_size);
    if (result != COMP_OK) {
      /* 面板留着，用户改的参数不丢；last_error 由 set_status 附带。 */
      set_status(shell, L"Camera Raw：应用失败（图层未改动）");
      return;
    }
    if (changed == 0) {
      set_status(shell, settings == "{}" ? L"Camera Raw：设置等于默认值，像素未改动（未产生撤销步）"
                                         : L"Camera Raw：像素未改动（未产生撤销步）");
      return;
    }
    shell.doc.needs_render = true;
    refresh_layers(shell);
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[384];
    if (note[0] != '\0') {
      note[sizeof(note) - 1] = '\0';
      wsprintfW(buffer, L"Camera Raw：已应用到选中图层（%s）",
                widen_utf8(note).c_str());
    } else {
      wsprintfW(buffer, L"Camera Raw：已应用到选中图层");
    }
    refresh_history_status(shell, buffer);
    return;
  }

  if (kind == comp_ui::DocDialogKind::TransformSelection) {
    /*
     * 中心点现算而不是打开面板时缓存：面板开着期间选区可能被撤销/重做换掉
     * （undo 会刷新 selection_base），缓存中心会把它转到一个不存在的位置上。
     */
    const comp_ui::Rect base = shell.doc.selection_base;
    if (base.w <= 0 || base.h <= 0) {
      doc_dialog_close(shell, L"变换选区：选区已不存在（未改动）");
      return;
    }
    comp_selection_transform_desc transform{};
    transform.center_x = static_cast<double>(base.x) + base.w * 0.5;
    transform.center_y = static_cast<double>(base.y) + base.h * 0.5;
    transform.width = shell.doc.doc_dialog_width;
    transform.height = shell.doc.doc_dialog_height;
    transform.angle_degrees = shell.doc.doc_dialog_angle;
    int32_t changed = 0;
    const comp_result result = comp_selection_transform(shell.doc.document, transform, &changed);
    if (result != COMP_OK) {
      /* 面板留着，用户填的数不丢；引擎在事务之前就拒了，没有半成品。 */
      set_status(shell, L"变换选区：应用失败（文档未改动）");
      return;
    }
    if (changed == 0) {
      /* 恒等（角度 0 且尺寸等于当前包围盒）：引擎没动蒙版，也就不该落撤销步。 */
      set_status(shell, L"变换选区：选区未变化（未产生撤销步）");
      return;
    }
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[224];
    wsprintfW(buffer, L"变换选区：%d×%d · %d 度（撤销一步）", shell.doc.doc_dialog_width,
              shell.doc.doc_dialog_height, shell.doc.doc_dialog_angle);
    finish_selection_command(shell, buffer);
    return;
  }
}

/*
 * 一个打开入口，两条去向。用户拖进来的东西可能是一张图，也可能是一个 .comp，
 * 按扩展名分派比让用户先想「该点哪个菜单」更接近 PS 的行为（PS 也是看文件类型）。
 */
void open_path_as_document(ShellState& shell, const char* path_utf8) {
  const comp_image_format format = comp_io_format_from_path(path_utf8);
  if (format != COMP_FORMAT_UNKNOWN) {
    comp_document document = nullptr;
    /*
     * PSD 是分层格式，走「整篇文档」入口（M1）：组、蒙版、剪贴、不透明度、
     * 混合模式全部还原成图层树。其余格式仍走 comp_io_import（单图层）。
     *
     * 分层读失败**不回落**到拍平：回落等于在用户不知情的情况下把分层工程换成了
     * 一张图，比打不开更糟。失败就报错并把引擎给出的具名原因带出来。
     */
    const comp_result opened = format == COMP_FORMAT_PSD
                                   ? comp_io_import_document(shell.engine, path_utf8, &document)
                                   : comp_io_import(shell.engine, path_utf8, &document);
    if (opened != COMP_OK || document == nullptr) {
      /*
       * 这里不能用 set_status：它只在已有文档时附加 last_error，而打开失败恰恰
       * 常常发生在「还没有文档」的时候，那样用户就只看得到一句泛泛的失败。
       */
      shell.status = format == COMP_FORMAT_PSD ? L"打开 PSD 失败" : L"打开位图失败";
      const char* engine_error = comp_engine_last_error(shell.engine);
      if (engine_error != nullptr && engine_error[0] != '\0') {
        const std::string narrow(engine_error);
        shell.status += L" | ";
        shell.status.append(narrow.begin(), narrow.end());
      } else {
        shell.status += L"：格式不支持或文件损坏";
      }
      return;
    }
    forget_color_range(shell);
    if (shell.doc.document != nullptr) comp_document_destroy(shell.doc.document);
    shell.doc.document = document;
    comp_size size{};
    if (comp_document_size(shell.doc.document, &size) == COMP_OK) {
      shell.doc.width = size.width;
      shell.doc.height = size.height;
    }
    const size_t pixel_count =
        static_cast<size_t>(shell.doc.width) * static_cast<size_t>(shell.doc.height);
    shell.doc.rgba.assign(pixel_count * 4, 0);
    shell.doc.framebuffer.assign(pixel_count, 0);
    shell.doc.needs_render = true;
    shell.doc.needs_fit = true;
    shell.doc.visibility.clear();
    shell.doc.filter_open = false;
    shell.doc.filter_input.clear();
    filter_preview_forget(shell);
    clone_reset_source(shell);
    stroke_cancel(shell);
    move_cancel(shell);
    eyedropper_cancel(shell);
    marquee_cancel(shell);
    lasso_cancel(shell);
    pen_cancel(shell);
    refresh_layers(shell);
    refresh_selection(shell);
    /* 分层读成功要说清楚「带图层」，否则用户会以为又是一张拍平图。 */
    refresh_history_status(shell,
                           format == COMP_FORMAT_PSD ? L"已打开 PSD（分层）" : L"已打开位图");
    return;
  }
  load_document(shell, path_utf8);
}

/*
 * 拖放导入。Windows 的 WM_DROPFILES 用 GMEM 句柄把路径交给窗口，必须
 * DragQueryFileW + DragFinish 成对，否则句柄泄漏。
 *
 * 语义对齐 PS：**空文档**时拖入 = 打开该文件；**已有内容**时拖入 = 置为新图层。
 * 这条分派比「永远新建文档」更符合工作流（在已有画布上叠一张素材是最常见操作）。
 */
void handle_drop_files(HWND window, ShellState& shell, HDROP drop) {
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT index = 0; index < count; ++index) {
    wchar_t path[MAX_PATH] = L"";
    if (DragQueryFileW(drop, index, path, MAX_PATH) == 0) continue;
    char utf8[MAX_PATH * 3] = {0};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (utf8[0] == '\0') continue;

    if (shell.doc.document == nullptr) {
      open_path_as_document(shell, utf8);
    } else if (comp_io_format_from_path(utf8) != COMP_FORMAT_UNKNOWN) {
      comp_layer_id id = 0;
      if (comp_io_import_layer(shell.doc.document, utf8, &id) == COMP_OK && id != 0) {
        shell.doc.needs_render = true;
        refresh_layers(shell);
        refresh_selection(shell);
        refresh_history_status(shell, L"已置入位图图层");
      } else {
        set_status(shell, L"置入位图失败");
      }
    } else {
      open_path_as_document(shell, utf8);
    }
  }
  DragFinish(drop);
  (void)window;
}

/* ------------------------------------------------------- 位图导入 / 导出 */

/*
 * 位图对话框与 .comp 对话框分开：用户心智里「打开图片」和「打开工程」不是一件事，
 * 混在一个过滤器里会让人以为 .comp 和 png 同级。格式由 comp_io_format_from_path
 * 从扩展名推断，本构建不支持的（HEIF/WebP/TIFF/PSB/SVG）在引擎侧以具名原因拒绝，
 * 这里不重复判断、不猜格式。
 */
void open_image_dialog(HWND window, ShellState& shell) {
  wchar_t buffer[MAX_PATH] = L"";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter =
      L"位图 (*.png;*.jpg;*.jpeg;*.exr;*.psd;*.hdr)\0*.png;*.jpg;*.jpeg;*.exr;*.psd;*.hdr\0"
      L"PNG (*.png)\0*.png\0JPEG (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
      L"EXR (*.exr)\0*.exr\0PSD (*.psd)\0*.psd\0Radiance HDR (*.hdr)\0*.hdr\0"
      L"所有文件\0*.*\0";
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
  if (!GetOpenFileNameW(&dialog)) return;
  char utf8[MAX_PATH * 3] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  open_path_as_document(shell, utf8);
}

/* 把图片作为新图层放进当前文档；失败时文档保持原样（引擎侧契约）。 */
void import_image_dialog(HWND window, ShellState& shell) {
  if (shell.doc.document == nullptr) return;
  wchar_t buffer[MAX_PATH] = L"";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter =
      L"位图 (*.png;*.jpg;*.jpeg;*.exr;*.psd;*.hdr)\0*.png;*.jpg;*.jpeg;*.exr;*.psd;*.hdr\0"
      L"所有文件\0*.*\0";
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
  if (!GetOpenFileNameW(&dialog)) return;
  char utf8[MAX_PATH * 3] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  comp_layer_id id = 0;
  if (comp_io_import_layer(shell.doc.document, utf8, &id) != COMP_OK || id == 0) {
    set_status(shell, L"置入图片失败");
    return;
  }
  shell.doc.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已置入图片图层");
}

/* 另存为位图。flatten 只对 PSD 有意义，其余格式引擎侧已按合成表面导出。 */
void export_image_dialog(HWND window, ShellState& shell) {
  if (shell.doc.document == nullptr) return;
  wchar_t buffer[MAX_PATH] = L"";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter =
      L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg\0EXR (*.exr)\0*.exr\0"
      L"PSD (*.psd)\0*.psd\0所有文件\0*.*\0";
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrDefExt = L"png";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (!GetSaveFileNameW(&dialog)) return;
  char utf8[MAX_PATH * 3] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  const comp_image_format format = comp_io_format_from_path(utf8);
  if (format == COMP_FORMAT_UNKNOWN) {
    set_status(shell, L"无法识别的导出格式");
    return;
  }
  /*
   * PSD 走「整篇文档」导出（M1）：图层树直接落盘，不拍平。其余格式仍是一次
   * 合成、一张图。分层导出拒绝的文档（文字层、HDR、未求值的图层效果…）由引擎
   * 给出具名原因，set_status 会把它接在状态栏后面——不做「悄悄拍平」。
   */
  const comp_result written =
      format == COMP_FORMAT_PSD
          ? comp_io_export_document(shell.doc.document, utf8, format, nullptr)
          : comp_io_export(shell.doc.document, utf8, format, nullptr);
  if (written != COMP_OK) {
    set_status(shell, L"导出失败：该格式不可用");
    return;
  }
  set_status(shell, format == COMP_FORMAT_PSD ? L"已导出 PSD（分层）" : L"已导出图片");
}

void run_shell_command(HWND window, ShellState& shell, UINT id) {
  /*
   * 任何命令之前先给连续微调收口：撤销/重做/新建/打开都要求「没有活动事务」，
   * 而微调事务是跨按键保持的（见 nudge_begin）。这里也是菜单路径的分步点——
   * 每点一次菜单项都是一条独立撤销记录。
   */
  nudge_flush(shell);
  switch (id) {
    case kCmdNew:
      create_document(shell);
      break;
    case kCmdOpen:
      open_document_dialog(window, shell);
      break;
    case kCmdSave:
      /* Ctrl+S / 文件>保存：有路径就静默覆盖，没有才弹对话框（见 save_document_quick）。 */
      save_document_quick(window, shell);
      break;
    /* ---- 文件 > 新建文档… / 另存为… ---- */
    case kCmdNewDocumentDialog:
      doc_dialog_begin(shell, comp_ui::DocDialogKind::NewDocument);
      break;
    case kCmdSaveAs:
      save_document_dialog(window, shell);
      break;
    /* ---- 图像 > 图像大小… / 画布大小…（对应上游 Image Size / Canvas Size） ---- */
    case kCmdImageSize:
      doc_dialog_begin(shell, comp_ui::DocDialogKind::ImageSize);
      break;
    case kCmdCanvasSize:
      doc_dialog_begin(shell, comp_ui::DocDialogKind::CanvasSize);
      break;
    /* ---- 编辑 > 历史记录容量… ---- */
    case kCmdHistorySettings:
      doc_dialog_begin(shell, comp_ui::DocDialogKind::HistoryLimit);
      break;
    /* ---- 滤镜 > Camera Raw 滤镜… ---- */
    case kCmdCameraRaw:
      doc_dialog_begin(shell, comp_ui::DocDialogKind::CameraRaw);
      break;
    case kCmdOpenImage:
      open_image_dialog(window, shell);
      break;
    case kCmdImportImage:
      import_image_dialog(window, shell);
      break;
    case kCmdExportImage:
      export_image_dialog(window, shell);
      break;
    case kCmdUndo:
      undo_document(shell);
      break;
    case kCmdRedo:
      redo_document(shell);
      break;
    case kCmdListCommands:
      list_commands(shell);
      break;
    /* ---- P0-4：剪贴板 ---- */
    case kCmdEditCopy:
      edit_copy(window, shell, false);
      break;
    case kCmdEditCopyMerged:
      edit_copy(window, shell, true);
      break;
    case kCmdEditCut:
      edit_cut(window, shell);
      break;
    case kCmdEditPaste:
      edit_paste(window, shell, false);
      break;
    case kCmdEditPasteInto:
      edit_paste(window, shell, true);
      break;
    case kCmdEditClear:
      clear_selected_pixels(shell, L"已清除图层像素");
      break;
    case kCmdSelectAll:
      select_all(shell);
      break;
    case kCmdSelectNone:
      deselect_all(shell);
      break;
    case kCmdSelectInvert:
      invert_selection(shell);
      break;
    /* ---- P0-4：扩展 / 收缩 / 按图层像素载入选区 ---- */
    case kCmdSelectExpand:
      begin_modify_input(shell, COMP_SELECTION_EXPAND);
      break;
    case kCmdSelectContract:
      begin_modify_input(shell, COMP_SELECTION_CONTRACT);
      break;
    /* ---- P0-5：修改补全（边界 / 平滑），同样走键入态 ---- */
    case kCmdSelectBorder:
      begin_modify_input(shell, COMP_SELECTION_BORDER);
      break;
    case kCmdSelectSmooth:
      begin_modify_input(shell, COMP_SELECTION_SMOOTH);
      break;
    /* ---- P0-5 收尾：扩大选取 / 选取相似（容差 = 魔棒容差） ---- */
    case kCmdSelectGrow:
      grow_similar_selection(shell, false);
      break;
    case kCmdSelectSimilar:
      grow_similar_selection(shell, true);
      break;
    /* ---- P0-5 收尾：变换选区（宽 / 高 / 角度数值输入） ---- */
    case kCmdSelectTransform:
      doc_dialog_begin(shell, comp_ui::DocDialogKind::TransformSelection);
      break;
    case kCmdSelectLoadPixels:
      load_layer_pixels_selection(shell);
      break;
    /* ---- P0-4 第二批：色彩范围 ---- */
    case kCmdSelectColorRange:
      color_range_begin(shell);
      break;
    /* ---- P0-4 可选：路径面板 ---- */
    case kCmdSelectPaths:
      path_panel_toggle(shell);
      break;
    /* ---- ROI #5：存储选区通道（PS Alpha 通道） ---- */
    case kCmdSelectChannels:
      channel_panel_toggle(shell);
      break;
    case kCmdSelectSaveChannel:
      channel_save_begin(shell);
      break;
    /* ---- P1：历史记录面板 ---- */
    case kCmdHistoryPanel:
      history_panel_toggle(shell);
      break;
    /* ---- P1：图像菜单（转画布 / 修边 / 反相） ---- */
    case kCmdImageRotateCW:
      image_rotate_canvas(shell, 1, L"顺时针 90°");
      break;
    case kCmdImageRotateCCW:
      image_rotate_canvas(shell, -1, L"逆时针 90°");
      break;
    case kCmdImageRotate180:
      image_rotate_canvas(shell, 2, L"180°");
      break;
    case kCmdImageTrim:
      trim_begin(shell);
      break;
    case kCmdImageInvert:
      image_invert_layer(shell);
      break;
    case kCmdLayerAdd:
      panel_add_layer(shell);
      break;
    /* ---- P0-1：图层面板补全（删除 / 复制 / 改名 / 建组 / 直设不透明度） ---- */
    case kCmdLayerDelete:
      panel_delete_layer(shell);
      break;
    case kCmdLayerDuplicate:
      panel_duplicate_layer(shell);
      break;
    case kCmdLayerRename:
      panel_begin_rename(shell);
      break;
    case kCmdLayerNewGroup:
      panel_add_group(shell);
      break;
    case kCmdLayerOpacitySet:
      panel_begin_opacity_set(shell);
      break;
    case kCmdLayerMergeDown:
      panel_merge_down(shell);
      break;
    case kCmdLayerFlatten:
      panel_flatten(shell);
      break;
    /* ---- P0-6 ①：文字图层栅格化（之后才是普通像素层） ---- */
    case kCmdLayerRasterizeText:
      panel_rasterize_text(shell);
      break;
    case kCmdLayerFonts:
      font_panel_toggle(window, shell);
      break;
    case kCmdViewFit:
      view_fit(shell);
      break;
    case kCmdViewActual:
      view_actual(shell);
      break;
    case kCmdViewZoomIn:
      view_zoom_step(shell, 1.2);
      break;
    case kCmdViewZoomOut:
      view_zoom_step(shell, 1.0 / 1.2);
      break;
    /* ---- ROI #3 标尺 / 参考线 / 网格 / 对齐（都是视图开关，不进文档像素） ---- */
    case kCmdViewRulers:
      view_toggle_rulers(shell);
      break;
    case kCmdViewGuides:
      view_toggle_guides(shell);
      break;
    case kCmdViewGrid:
      view_toggle_grid(shell);
      break;
    case kCmdViewSnap:
      view_toggle_snap(shell);
      break;
    case kCmdViewClearGuides:
      view_clear_guides(shell);
      break;
    case kCmdLayerSelectUp:
      panel_move_selection(shell, 1); /* 面板自上而下：上移一行 = 索引 +1 */
      break;
    case kCmdLayerSelectDown:
      panel_move_selection(shell, -1);
      break;
    case kCmdLayerToggleVisible:
      panel_toggle_visible(shell);
      break;
    case kCmdLayerOpacityDown:
      panel_adjust_opacity(shell, -0.1f);
      break;
    case kCmdLayerOpacityUp:
      panel_adjust_opacity(shell, 0.1f);
      break;
    case kCmdLayerBlendPrev:
      panel_cycle_blend(shell, -1);
      break;
    case kCmdLayerBlendNext:
      panel_cycle_blend(shell, 1);
      break;
    case kCmdLayerMoveLeft:
      panel_move_layer(shell, -comp_ui::layer_transform_move_step(false), 0);
      break;
    case kCmdLayerMoveRight:
      panel_move_layer(shell, comp_ui::layer_transform_move_step(false), 0);
      break;
    case kCmdLayerMoveUp:
      panel_move_layer(shell, 0, -comp_ui::layer_transform_move_step(false));
      break;
    case kCmdLayerMoveDown:
      panel_move_layer(shell, 0, comp_ui::layer_transform_move_step(false));
      break;
    case kCmdLayerRotateLeft:
      panel_rotate_layer(shell, false);
      break;
    case kCmdLayerRotateRight:
      panel_rotate_layer(shell, true);
      break;
    case kCmdLayerScaleDown:
      panel_scale_layer(shell, -1);
      break;
    case kCmdLayerScaleUp:
      panel_scale_layer(shell, 1);
      break;
    /* ---- S22：引擎早已实现、壳此前未接线的像素级图层能力 ---- */
    case kCmdLayerMaskFromSelection:
      panel_mask_from_selection(shell);
      break;
    case kCmdLayerMaskRemove:
      panel_mask_remove(shell);
      break;
    case kCmdLayerMaskToSelection:
      panel_mask_to_selection(shell);
      break;
    case kCmdLayerToggleClipping:
      panel_toggle_clipping(shell);
      break;
    case kCmdLayerToggleIsolated:
      panel_toggle_isolated(shell);
      break;
    case kCmdLayerBringForward:
      panel_reorder_layer(shell, -1);
      break;
    case kCmdLayerSendBackward:
      panel_reorder_layer(shell, 1);
      break;
    case kCmdLayerBringToFront:
      panel_reorder_layer(shell, -2);
      break;
    case kCmdLayerSendToBack:
      panel_reorder_layer(shell, 0);
      break;
    case kCmdHistogramToggle:
      panel_toggle_histogram(shell);
      break;
    /* ---- §5.43 面板整块收起（停靠最小版）：菜单 / F7 / F8 / 点标题行按钮同一入口 ---- */
    case kCmdPanelLayers:
      panel_toggle_collapsed(shell, 1);
      break;
    case kCmdPanelAdjust:
      panel_toggle_collapsed(shell, 2);
      break;
    /* ---- S24：镜像、效果开关、直方图区间复位 ---- */
    case kCmdLayerFlipHorizontal:
      panel_toggle_flip(shell, true);
      break;
    case kCmdLayerFlipVertical:
      panel_toggle_flip(shell, false);
      break;
    case kCmdEffectClear:
      panel_clear_effects(shell);
      break;
    /* P0-2：编辑参数。没有效果时从第一档（描边）开始，回车即添加。 */
    case kCmdEffectEdit:
      open_effect_panel(shell, shell.doc.effect_kinds.empty()
                                   ? 0
                                   : static_cast<int>(shell.doc.effect_kinds.front()));
      break;
    case kCmdHistogramReset:
      panel_histogram_reset(shell);
      break;
    case kCmdAdjKindPrev:
      panel_cycle_kind(shell, -1);
      break;
    case kCmdAdjKindNext:
      panel_cycle_kind(shell, 1);
      break;
    case kCmdAdjRowPrev:
      panel_select_adjustment(shell, -1);
      break;
    case kCmdAdjRowNext:
      panel_select_adjustment(shell, 1);
      break;
    case kCmdAdjParamPrev:
      panel_move_param(shell, -1);
      break;
    case kCmdAdjParamNext:
      panel_move_param(shell, 1);
      break;
    case kCmdAdjValueDown:
      panel_step_value(shell, -1);
      break;
    case kCmdAdjValueUp:
      panel_step_value(shell, 1);
      break;
    case kCmdAdjAdd:
      panel_apply_adjustment(shell, true);
      break;
    case kCmdAdjApply:
      panel_apply_adjustment(shell, false);
      break;
    case kCmdAdjRemove:
      panel_remove_adjustment(shell);
      break;
    case kCmdFilterApply:
      apply_filter(shell);
      break;
    case kCmdFilterReset:
      filter_reset_values(shell);
      break;
    case kCmdFilterClose:
      close_filter_panel(shell);
      break;
    case kCmdExit:
      PostMessageW(window, WM_CLOSE, 0, 0);
      return;
    default:
      /*
       * ROI #8 图像 > 调整：kCmdImageAdjustBase + kind。菜单只给 kImageAdjustKinds
       * 里的 kind 发号（表在 shell_state.h，与菜单构建共用），其余号一律忽略。
       */
      if (id >= kCmdImageAdjustBase && id < kCmdImageAdjustBase + 64) {
        const uint32_t kind = id - kCmdImageAdjustBase;
        for (size_t i = 0; i < kImageAdjustKindCount; ++i) {
          if (kImageAdjustKinds[i] == kind) {
            panel_begin_destructive(shell, kind);
            break;
          }
        }
        break;
      }
      /* 滤镜菜单项：kCmdFilterBase + 滤镜序号。 */
      if (id >= kCmdFilterBase && id < kCmdFilterBase + comp_ui::filter_count()) {
        open_filter_panel(shell, static_cast<comp_ui::FilterId>(id - kCmdFilterBase));
        break;
      }
      /* 混合模式菜单项（S21）：kCmdBlendBase + 档位。 */
      if (id >= kCmdBlendBase && id < kCmdBlendBase + comp_ui::kBlendCount) {
        panel_set_blend(shell, static_cast<int>(id - kCmdBlendBase));
        break;
      }
      /* 采样模式菜单项（S22）：kCmdSamplingBase + 档位 0..2。 */
      if (id >= kCmdSamplingBase && id < kCmdSamplingBase + comp_ui::kSamplingCount) {
        panel_set_sampling(shell, static_cast<int>(id - kCmdSamplingBase));
        break;
      }
      /* 效果菜单项（S24）：kCmdEffectBase + kind 0..5，勾选 = 已启用。 */
      if (id >= kCmdEffectBase && id < kCmdEffectBase + comp_ui::kEffectKindCount) {
        panel_toggle_effect(shell, static_cast<int>(id - kCmdEffectBase));
        break;
      }
      return;
  }
  InvalidateRect(window, nullptr, FALSE);
}

}  // namespace comp_shell
