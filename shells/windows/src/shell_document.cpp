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
  /*
   * P1-3：「再次变换」复读的是**这张文档**上一步的几何变换。换文档后旧比例/
   * 角度对新图层没有意义（尺寸基准都不一定同类），一并作废，免得 Ctrl+Shift+T
   * 在刚打开的图上套一个看不见来源的缩放。
   */
  shell.doc.transform_step_valid = false;
  shell.doc.transform_step_scale_x = 1.0;
  shell.doc.transform_step_scale_y = 1.0;
  shell.doc.transform_step_angle = 0.0;
}

bool create_document_with(ShellState& shell, int width, int height, int dpi, int background) {
  return create_document_opts(shell, width, height, dpi, background, 0, 0);
}

/*
 * P0-1：带位深与颜色模式的新建。
 *
 * 走 comp_document_create_ex（新入口，comp_document_desc 布局一个字节不动）——
 * 老宿主那条 comp_document_create 仍然是「8 位 RGB」，与追加字段前完全一致。
 * 背景填充仍走 comp_layer_set_pixels，但灰阶文档的写入侧会自己转灰（引擎侧契约），
 * 所以这里不需要为「前景色填充」另写一条灰度分支。
 */
bool create_document_opts(ShellState& shell, int width, int height, int dpi, int background,
                          int pixel_format, int color_mode) {
  /* P0-3：新建 = 另起一个标签，不再覆盖当前文档（覆盖前要不要提示这个问题
   * 随之消失——旧文档一直活着，用户随时切回去）。 */
  tab_park_active(shell);
  comp_document_desc desc{};
  desc.width = comp_ui::clamp_doc_size(width);
  desc.height = comp_ui::clamp_doc_size(height);
  desc.dpi = static_cast<float>(comp_ui::clamp_doc_dpi(dpi));
  desc.color_space = "sRGB";
  comp_document_options options{};
  options.struct_size = sizeof(options);
  options.pixel_format = pixel_format != 0 ? COMP_PIXEL_RGBA16 : COMP_PIXEL_RGBA8;
  options.color_mode = color_mode != 0 ? COMP_COLOR_MODE_GRAYSCALE : COMP_COLOR_MODE_RGB;

  comp_document document = nullptr;
  const comp_result result = comp_document_create_ex(shell.engine, &desc, &options, &document);
  if (result != COMP_OK || document == nullptr) {
    set_status(shell, L"新建文档失败（当前文档未受影响）");
    /* 建失败：把刚停进标签槽的旧文档搬回前台，窗口不能空着。 */
    tab_restore_parked(shell);
    return false;
  }
  forget_color_range(shell);
  if (shell.doc.document != nullptr) comp_document_destroy(shell.doc.document);
  shell.doc.document = document;
  tab_commit_new(shell);
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
  smart_panel_reset(shell);     /* 智能滤镜栈面板绑定的是旧文档的图层 id */
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
  /* 载入成功才停旧文档：load 失败时不必动标签，用户还留在原来那份上。 */
  tab_park_active(shell);
  forget_color_range(shell);
  if (shell.doc.document != nullptr) comp_document_destroy(shell.doc.document);
  shell.doc.document = document;
  tab_commit_new(shell);

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
  smart_panel_reset(shell);
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
  recent_push(shell, path_utf8);
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

/* --------------------------------------------- P0-3：标签页 / 最近打开 */

namespace {

constexpr size_t kRecentLimit = 10;
constexpr wchar_t kRecentFileName[] = L"recent-files.txt";

/*
 * 最近列表的落盘位置：%APPDATA%\Compositor\recent-files.txt，没有 APPDATA
 * （精简环境 / 绿色包）时退到 exe 同级目录。**不写注册表**：绿色包不该在
 * 用户机器上留下要专门清理的键，一个纯文本文件坏了删掉即可。
 */
std::wstring recent_config_path() {
  wchar_t buffer[1024] = {0};
  const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, 1024);
  if (length > 0 && length < 1024) {
    std::wstring dir(buffer);
    dir += L"\\Compositor";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + kRecentFileName;
  }
  const std::wstring exe = exe_directory();
  return exe.empty() ? std::wstring() : exe + L"\\" + kRecentFileName;
}

bool write_lines(const std::wstring& path, const std::string& text) {
  if (path.empty()) return false;
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;
  const size_t written = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
  return written == text.size();
}

/* 路径去重用的键：Windows 路径大小写不敏感，\ 与 / 等价。 */
std::string recent_key(const std::string& path) {
  std::string key = path;
  for (char& c : key) {
    if (c == '/') c = '\\';
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return key;
}

}  // namespace

void recent_load(ShellState& shell) {
  shell.recent_files.clear();
  std::vector<uint8_t> data;
  if (!read_whole_file(recent_config_path(), &data) || data.empty()) return;
  std::string text(data.begin(), data.end());
  size_t start = 0;
  while (start < text.size() && shell.recent_files.size() < kRecentLimit) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) shell.recent_files.push_back(line);
    start = end + 1;
  }
  if (shell.recent_index >= static_cast<int>(shell.recent_files.size())) shell.recent_index = 0;
}

void recent_save(const ShellState& shell) {
  std::string text;
  for (const std::string& path : shell.recent_files) {
    text += path;
    text += '\n';
  }
  write_lines(recent_config_path(), text);
}

void recent_push(ShellState& shell, const char* path_utf8) {
  if (path_utf8 == nullptr || path_utf8[0] == '\0') return;
  const std::string path(path_utf8);
  const std::string key = recent_key(path);
  std::vector<std::string> next;
  next.push_back(path);
  for (const std::string& existing : shell.recent_files) {
    if (recent_key(existing) == key) continue;
    if (next.size() >= kRecentLimit) break;
    next.push_back(existing);
  }
  shell.recent_files = std::move(next);
  shell.recent_index = 0;
  recent_save(shell);
}

/* 换文档前收口所有跨帧交互：事务句柄不能在后台标签里挂着。
 * 这批函数在 create_document_with 里也各调了一次，但那是在**新文档**上；
 * 这里必须在旧文档还活着的时候先跑一遍。 */
void doc_switch_finish(ShellState& shell) {
  nudge_flush(shell);
  stroke_cancel(shell);
  move_cancel(shell);
  eyedropper_cancel(shell);
  marquee_cancel(shell);
  selection_handle_cancel(shell);
  lasso_cancel(shell);
  pen_cancel(shell);
  filter_preview_forget(shell);
  guide_drag_forget(shell);
  doc_dialog_close(shell, nullptr);
}

void tabs_ensure(ShellState& shell) {
  if (shell.tabs.empty()) {
    shell.tabs.push_back(DocTabSlot{});
    shell.active_tab = 0;
  }
  if (shell.active_tab < 0 || shell.active_tab >= static_cast<int>(shell.tabs.size())) {
    shell.active_tab = 0;
  }
  for (size_t i = 0; i < shell.tabs.size(); ++i) {
    shell.tabs[i].front = static_cast<int>(i) == shell.active_tab;
  }
}

int tab_count(ShellState& shell) {
  tabs_ensure(shell);
  return static_cast<int>(shell.tabs.size());
}

/* 标签标题：路径是唯一真源（另存为之后标签自动跟着改），脏标记向引擎现问。 */
std::wstring tab_title(const ShellState& shell, int index) {
  const std::string* path = nullptr;
  comp_document document = nullptr;
  if (index == shell.active_tab) {
    path = &shell.doc.document_path;
    document = shell.doc.document;
  } else if (index >= 0 && index < static_cast<int>(shell.tabs.size())) {
    path = &shell.tabs[index].doc.document_path;
    document = shell.tabs[index].doc.document;
  }
  std::wstring title;
  if (path != nullptr && !path->empty()) {
    const size_t slash = path->find_last_of("/\\");
    title = widen_utf8(slash == std::string::npos ? *path : path->substr(slash + 1));
  } else {
    title = document != nullptr ? L"未命名" : L"（空）";
  }
  int32_t dirty = 0;
  if (document != nullptr && comp_document_is_dirty(document, &dirty) == COMP_OK && dirty != 0) {
    title += L" *";
  }
  return title;
}

void tab_activate(ShellState& shell, int index) {
  tabs_ensure(shell);
  if (index < 0 || index >= static_cast<int>(shell.tabs.size()) || index == shell.active_tab) {
    return;
  }
  doc_switch_finish(shell);
  shell.tabs[shell.active_tab].doc = std::move(shell.doc);
  shell.tabs[shell.active_tab].front = false;
  shell.doc = std::move(shell.tabs[index].doc);
  shell.tabs[index].doc = DocumentContext{};
  shell.tabs[index].front = true;
  shell.active_tab = index;
  shell.doc.needs_render = true;
  shell.window_title_cache.clear();
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已切换文档");
}

void tab_park_active(ShellState& shell) {
  tabs_ensure(shell);
  doc_switch_finish(shell);
  if (shell.doc.document == nullptr) {
    /* 启动后第一次建文档：没有可停的旧文档，直接复用当前槽。 */
    shell.tab_parked = false;
    return;
  }
  shell.tabs[shell.active_tab].doc = std::move(shell.doc);
  shell.tabs[shell.active_tab].front = false;
  shell.doc = DocumentContext{};
  shell.tab_parked = true;
}

void tab_commit_new(ShellState& shell) {
  tabs_ensure(shell);
  if (!shell.tab_parked) {
    /* 复用当前槽：第一次建文档就是它。 */
    shell.tabs[shell.active_tab].front = true;
    shell.window_title_cache.clear();
    return;
  }
  DocTabSlot slot;
  slot.front = true;
  shell.tabs.insert(shell.tabs.begin() + shell.active_tab + 1, std::move(slot));
  shell.active_tab += 1;
  shell.tab_parked = false;
  shell.window_title_cache.clear();
}

void tab_restore_parked(ShellState& shell) {
  if (!shell.tab_parked) return;
  if (shell.doc.document != nullptr) return;
  if (shell.active_tab < 0 || shell.active_tab >= static_cast<int>(shell.tabs.size())) return;
  if (shell.tabs[shell.active_tab].doc.document == nullptr) return;
  shell.doc = std::move(shell.tabs[shell.active_tab].doc);
  shell.tabs[shell.active_tab].doc = DocumentContext{};
  shell.tabs[shell.active_tab].front = true;
  shell.tab_parked = false;
  shell.doc.needs_render = true;
  shell.window_title_cache.clear();
}

bool tab_close_active(ShellState& shell) {
  tabs_ensure(shell);
  if (shell.doc.document != nullptr) comp_document_destroy(shell.doc.document);
  shell.doc = DocumentContext{};
  shell.tabs.erase(shell.tabs.begin() + shell.active_tab);
  if (shell.tabs.empty()) {
    shell.active_tab = 0;
    shell.tab_parked = false;
    shell.window_title_cache.clear();
    return false;
  }
  if (shell.active_tab >= static_cast<int>(shell.tabs.size())) {
    shell.active_tab = static_cast<int>(shell.tabs.size()) - 1;
  }
  shell.doc = std::move(shell.tabs[shell.active_tab].doc);
  shell.tabs[shell.active_tab].doc = DocumentContext{};
  shell.tabs[shell.active_tab].front = true;
  shell.tab_parked = false;
  shell.doc.needs_render = true;
  shell.window_title_cache.clear();
  refresh_layers(shell);
  refresh_selection(shell);
  return true;
}

void tab_step(ShellState& shell, int delta) {
  tabs_ensure(shell);
  const int count = static_cast<int>(shell.tabs.size());
  if (count <= 1) {
    set_status(shell, L"只有一个文档");
    return;
  }
  int next = (shell.active_tab + delta) % count;
  if (next < 0) next += count;
  tab_activate(shell, next);
}

/*
 * 关闭流程推进（退出时逐个收口）。
 * 返回 true = 已经弹出未保存提示，等用户回答；false = 全部关完，可以关窗。
 */
bool close_flow_step(ShellState& shell) {
  for (;;) {
    tabs_ensure(shell);
    if (shell.doc.document == nullptr) {
      if (!tab_close_active(shell)) return false;
      continue;
    }
    int32_t dirty = 0;
    const bool is_dirty =
        comp_document_is_dirty(shell.doc.document, &dirty) == COMP_OK && dirty != 0;
    if (is_dirty) {
      doc_dialog_begin(shell, comp_ui::DocDialogKind::ClosePrompt);
      return true;
    }
    if (!tab_close_active(shell)) return false;
  }
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
 * 可编辑色标表（P1-7）的面板辅助：表下标与 alpha/百分比换算。
 * 选中项用 0 基存在 ShellState 里，面板第 0 行显示 1 基；表在面板开着时可能
 * 因增删变短，所以每次读都夹一次，不假设「打开时是多少现在还是多少」。
 */
int gradient_dialog_stop_index(const ShellState& shell) {
  const int count = static_cast<int>(shell.gradient_custom_stops.size());
  if (count <= 0) return -1;
  int index = shell.gradient_stop;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  return index;
}

/* alpha 0..255 <-> 不透明度百分比 0..100：四舍五入到最近整百分点。 */
int gradient_alpha_percent(uint8_t alpha) {
  return (static_cast<int>(alpha) * 100 + 127) / 255;
}

uint8_t gradient_percent_alpha(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return static_cast<uint8_t>((percent * 255 + 50) / 100);
}

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
        case 4: return shell.doc.doc_dialog_pixel_format;
        case 5: return shell.doc.doc_dialog_color_mode;
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
    case comp_ui::DocDialogKind::RecentFiles:
      return row == 0 ? shell.recent_index : 0;
    case comp_ui::DocDialogKind::TransformSelection:
      switch (row) {
        case 0: return shell.doc.doc_dialog_width;
        case 1: return shell.doc.doc_dialog_height;
        case 2: return shell.doc.doc_dialog_angle;
        default: return 0;
      }
    case comp_ui::DocDialogKind::TransformLayer:
      switch (row) {
        case 0: return shell.doc.doc_dialog_width;
        case 1: return shell.doc.doc_dialog_height;
        case 2: return shell.doc.doc_dialog_angle;
        default: return 0;
      }
    case comp_ui::DocDialogKind::GradientStops: {
      if (row < 0 || row >= comp_ui::kGradientStopsRowCount) return 0;
      const int index = gradient_dialog_stop_index(shell);
      if (index < 0) return 0;
      const comp_ui::GradientPresetStop& stop = shell.gradient_custom_stops[index];
      switch (row) {
        case 0: return index + 1;
        case 1: return comp_ui::gradient_position_permille(stop.position);
        case 2: return comp_ui::gradient_position_permille(stop.midpoint);
        case 3:
        case 4:
        case 5: return stop.color[row - 3];
        case 6: return gradient_alpha_percent(stop.color[3]);
        case 7: return shell.gradient_stop_action;
        default: return 0;
      }
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
        /* 两档都只有 0/1，仍夹一次：枚举行用 ←/→ 循环与点击前向循环，
         * 数值越界只可能来自宿主的脏值，夹在写入口比在读取处夹更省事。 */
        case 4: shell.doc.doc_dialog_pixel_format = value != 0 ? 1 : 0; break;
        case 5: shell.doc.doc_dialog_color_mode = value != 0 ? 1 : 0; break;
        default: break;
      }
      break;
    case comp_ui::DocDialogKind::RecentFiles: {
      /* 列表长度是动态的，在这里按条数取模——静态行模型的 max 只是个上界。 */
      const int count = static_cast<int>(shell.recent_files.size());
      if (count <= 0) {
        shell.recent_index = 0;
        break;
      }
      int next = value % count;
      if (next < 0) next += count;
      shell.recent_index = next;
      break;
    }
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
    case comp_ui::DocDialogKind::TransformLayer:
      /* 与选区那条共用同一组行规格（1..30000 / ±180），夹取来源只有一处。 */
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
    case comp_ui::DocDialogKind::GradientStops: {
      const int count = static_cast<int>(shell.gradient_custom_stops.size());
      if (row < 0 || row >= comp_ui::kGradientStopsRowCount || count <= 0) break;
      const int index = gradient_dialog_stop_index(shell);
      if (index < 0) break;
      comp_ui::GradientPresetStop* stops = shell.gradient_custom_stops.data();
      switch (row) {
        case 0:
          if (value < 1) value = 1;
          if (value > count) value = count;
          shell.gradient_stop = value - 1;
          break;
        case 1:
          comp_ui::gradient_set_stop_position_permille(stops, count, index, value);
          break;
        case 2:
          comp_ui::gradient_set_stop_midpoint_permille(stops, count, index, value);
          break;
        case 3:
        case 4:
        case 5:
          if (value < 0) value = 0;
          if (value > 255) value = 255;
          stops[index].color[row - 3] = static_cast<uint8_t>(value);
          break;
        case 6:
          stops[index].color[3] = gradient_percent_alpha(value);
          break;
        case 7: {
          /*
           * 增删是**瞬时动作**，不是持久档位：执行完立刻归零，面板上永远显示
           * 「→ 插入 · ← 删除」。这样同一颗键连按两次就是插两个，不会出现
           * 「操作行停在删除，回车又删了一个」。
           */
          const int action = value < 0 ? 0 : (value > 2 ? 2 : value);
          if (action == 1) {
            if (count < comp_ui::kGradientMaxStops) {
              shell.gradient_custom_stops.resize(count + 1);
              const int added = comp_ui::gradient_insert_stop(
                  shell.gradient_custom_stops.data(), count, index);
              if (added > count) {
                shell.gradient_stop = index < count - 1 ? index + 1 : count - 1;
              } else {
                shell.gradient_custom_stops.resize(count);
              }
            }
          } else if (action == 2) {
            const int removed = comp_ui::gradient_delete_stop(stops, count, index);
            if (removed > 0) shell.gradient_custom_stops.resize(removed);
          }
          shell.gradient_stop_action = 0;
          const int after = static_cast<int>(shell.gradient_custom_stops.size());
          if (shell.gradient_stop >= after) shell.gradient_stop = after - 1;
          if (shell.gradient_stop < 0) shell.gradient_stop = 0;
          break;
        }
        default:
          break;
      }
      break;
    }
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
        /* P0-1：新建面板现在有三条枚举行，必须按行分派——此前一律返回背景名，
         * 加行之后位深行会显示成「透明」，属于把数据画错的典型写法。 */
        switch (row) {
          case 3: return widen_utf8(comp_ui::doc_new_background_name(value));
          case 4: return widen_utf8(comp_ui::doc_new_bit_depth_name(value));
          case 5: return widen_utf8(comp_ui::doc_new_color_mode_name(value));
          default: return widen_utf8(comp_ui::doc_new_background_name(value));
        }
      }
      if (shell.doc.doc_dialog == comp_ui::DocDialogKind::ClosePrompt) {
        return widen_utf8(comp_ui::doc_close_action_name(value));
      }
      if (shell.doc.doc_dialog == comp_ui::DocDialogKind::GradientStops) {
        return L"→ 插入 · ← 删除";
      }
      if (shell.doc.doc_dialog == comp_ui::DocDialogKind::RecentFiles) {
        const int count = static_cast<int>(shell.recent_files.size());
        if (count == 0) return L"（列表为空）";
        const int index = shell.recent_index < 0 || shell.recent_index >= count ? 0
                                                                            : shell.recent_index;
        const std::string& path = shell.recent_files[index];
        const size_t slash = path.find_last_of("/\\");
        const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
        wsprintfW(buffer, L"%d/%d %s", index + 1, count, widen_utf8(leaf).c_str());
        return buffer;
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
    case comp_ui::DocDialogKind::TransformLayer:
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
    case comp_ui::DocDialogKind::GradientStops:
      switch (row) {
        case 0: {
          /* 选中项 + 总数一起显示，删完不会留下「第 6 个（共 4 个）」这种鬼话。 */
          const int count = static_cast<int>(shell.gradient_custom_stops.size());
          wsprintfW(buffer, L"%d / %d", value, count);
          return buffer;
        }
        case 1:
        case 2:
          /* 千分位 -> 「50.0%」；wsprintfW 不支持 %f，整数拆两位拼。 */
          wsprintfW(buffer, L"%d.%d%%", value / 10, value % 10);
          return buffer;
        case 6:
          wsprintfW(buffer, L"%d%%", value);
          return buffer;
        default:
          wsprintfW(buffer, L"%d", value);
          return buffer;
      }
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
      kind != comp_ui::DocDialogKind::ClosePrompt &&
      kind != comp_ui::DocDialogKind::RecentFiles && shell.doc.document == nullptr) {
    set_status(shell, L"文档：还没有打开任何文档");
    return;
  }
  if (kind == comp_ui::DocDialogKind::RecentFiles) {
    /* 打开面板时现读一次盘：外部删了文件、或另一个进程往同一个列表里加过，
     * 缓存一份就会点到已不存在的路径。 */
    recent_load(shell);
    shell.recent_index = 0;
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
  if (kind == comp_ui::DocDialogKind::TransformLayer) {
    /*
     * 组与空图层没有可变换的像素：layer_effective_transform 在 0×0 时回 false。
     * 在这里就拒（而不是让用户填完数才失败），也避免画出一块「改了也没用」的面板。
     */
    const comp_layer_id target = selected_layer_id(shell);
    if (target == 0) {
      set_status(shell, L"数值变换：请先在图层面板选中一个图层");
      return;
    }
    comp_rect rect{};
    double rotation = 0.0;
    if (!layer_effective_transform(shell, target, &rect, &rotation)) {
      set_status(shell, L"数值变换：组或空图层没有可变换的像素");
      return;
    }
  }
  /* 右栏一次只挂一个面板（与滤镜/效果/修边/路径/历史/色彩范围/智能滤镜栈互斥）。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  smart_panel_dismiss(shell);
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
  /* P1-2 收尾 ⑧：填充面板同样让位；拖动可能持有捕获与事务，先收口再清状态。 */
  if (shell.doc.fill_open) {
    fill_panel_drag_finish(shell, false);
    shell.doc.fill_open = false;
    shell.doc.fill_input.clear();
    shell.doc.fill_layer = 0;
  }

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
      shell.doc.doc_dialog_pixel_format = shell.new_doc_pixel_format;
      shell.doc.doc_dialog_color_mode = shell.new_doc_color_mode;
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
    case comp_ui::DocDialogKind::TransformLayer: {
      /*
       * 起点 = 图层的**有效**变换（隐式变换回落到像素尺寸），角度 = 当前旋转：
       * 什么都不填直接回车就是恒等变换（panel_apply_layer_transform 短路，不落撤销步）。
       *
       * doc_dialog_angle_base 记住打开时的取整角度，应用时用它判断「角度行有没有被
       * 动过」：没动就写回**原始 float**，否则改个宽度会把 22.5° 悄悄圆成 23°。
       */
      comp_rect rect{};
      double rotation = 0.0;
      if (layer_effective_transform(shell, selected_layer_id(shell), &rect, &rotation)) {
        shell.doc.doc_dialog_width = comp_ui::clamp_doc_size(static_cast<int>(rect.w));
        shell.doc.doc_dialog_height = comp_ui::clamp_doc_size(static_cast<int>(rect.h));
        shell.doc.doc_dialog_angle = comp_ui::doc_angle_from_rotation(rotation);
      } else {
        /* 判空已在上面做过；走到这里说明面板开着期间图层没了，给一个自洽的 1×1。 */
        shell.doc.doc_dialog_width = 1;
        shell.doc.doc_dialog_height = 1;
        shell.doc.doc_dialog_angle = 0;
      }
      shell.doc.doc_dialog_angle_base = shell.doc.doc_dialog_angle;
      break;
    }
    case comp_ui::DocDialogKind::GradientStops: {
      /*
       * 打开时把「当前档位」搬进草稿表：预设 0..2 现展开（预设 0 取前景色），
       * 自定义则沿用已存的表——只要它仍然合法。这样面板一开就有一张能直接
       * 回车的表；Esc 丢掉的是这一轮编辑，不是上一轮已应用的成果。
       */
      const uint8_t foreground[4] = {shell.foreground.r, shell.foreground.g,
                                     shell.foreground.b, 255};
      const int count = static_cast<int>(shell.gradient_custom_stops.size());
      const bool reusable =
          shell.gradient_preset == comp_ui::kGradientPresetCustom &&
          comp_ui::gradient_stops_valid(shell.gradient_custom_stops.data(), count);
      if (!reusable) {
        int source = shell.gradient_preset;
        if (source < 0 || source > 2) source = 0; /* 自定义但没有可用的表 */
        comp_ui::GradientPresetStop table[comp_ui::kGradientMaxStops];
        const int expanded = comp_ui::gradient_preset_stops(
            source, foreground, false, table, comp_ui::kGradientMaxStops);
        if (expanded >= 2) shell.gradient_custom_stops.assign(table, table + expanded);
      }
      shell.gradient_stop = 0;
      shell.gradient_stop_action = 0;
      break;
    }
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
  /*
   * 色标表的「增删」行是**动作**而不是持久值：执行后立刻归零，所以状态栏要按
   * 表长度前后变化来报，而不是照抄「增删 = → 插入 · ← 删除」。失败时也要说清
   * 是哪一种失败（满 32 / 只剩两个 / 首末固定），否则用户会反复按同一个键。
   */
  if (shell.doc.doc_dialog == comp_ui::DocDialogKind::GradientStops && row == 7) {
    const int before = static_cast<int>(shell.gradient_custom_stops.size());
    const int selected = shell.gradient_stop;
    const int choices = 3;
    const int action = ((current + direction) % choices + choices) % choices;
    doc_dialog_set_row_value(shell, row, action);
    const int after = static_cast<int>(shell.gradient_custom_stops.size());
    wchar_t buffer[192];
    if (after > before) {
      wsprintfW(buffer, L"已插入色标（共 %d 个，选中第 %d 个）", after, shell.gradient_stop + 1);
    } else if (after < before) {
      wsprintfW(buffer, L"已删除色标（共 %d 个）", after);
    } else if (action == 1) {
      wsprintfW(buffer, L"不能插入：已达 %d 个色标上限", comp_ui::kGradientMaxStops);
    } else if (before <= 2) {
      wsprintfW(buffer, L"不能删除：至少要保留两个色标");
    } else {
      wsprintfW(buffer, L"不能删除：首末色标固定在 0%% / 100%%（选中第 %d 个）", selected + 1);
    }
    set_status(shell, buffer);
    return;
  }
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
  /* 小数点只给色标表的位置/中点行：其它行是纯整数，收 '.' 会让回车落回同一个数。 */
  if (character == L'.') {
    const bool percent_row = shell.doc.doc_dialog == comp_ui::DocDialogKind::GradientStops &&
                             (shell.doc.doc_dialog_focus == 1 || shell.doc.doc_dialog_focus == 2);
    if (!percent_row || shell.doc.doc_dialog_input.empty() ||
        shell.doc.doc_dialog_input.find(L'.') != std::wstring::npos) {
      return;
    }
  }
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
  /*
   * 色标表的位置/中点行收百分数，允许一位小数（「37.5」）。千分位是内部单位，
   * 输入框里直接写 0.1% 更符合 PS 的读法；这里把「整数部分 / 十分位」拼成
   * 千分位，后面的夹取与显示仍只认一个整数，不引入浮点状态。
   */
  if (shell.doc.doc_dialog == comp_ui::DocDialogKind::GradientStops && (row == 1 || row == 2) &&
      shell.doc.doc_dialog_input.find(L'.') != std::wstring::npos) {
    long long whole = 0;
    int tenth = 0;
    bool seen_dot = false;
    for (wchar_t character : shell.doc.doc_dialog_input) {
      if (character == L'.') {
        seen_dot = true;
        continue;
      }
      if (character < L'0' || character > L'9') continue;
      if (!seen_dot) {
        whole = whole * 10 + (character - L'0');
        if (whole > 100000) whole = 100000;
      } else if (tenth == 0) {
        tenth = character - L'0';
      }
    }
    long long permille = whole * 10 + tenth;
    if (permille > spec.max) permille = spec.max;
    if (permille < spec.min) permille = spec.min;
    shell.doc.doc_dialog_input.clear();
    doc_dialog_set_row_value(shell, row, static_cast<int>(permille));
    wchar_t buffer[192];
    wsprintfW(buffer, L"%s = %s", widen_utf8(spec.label).c_str(),
              doc_dialog_value_text(shell, row).c_str());
    set_status(shell, buffer);
    return;
  }
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
    /*
     * 保存成功或「不保存」：按 close_intent 决定关到哪儿。
     *
     *   1 = 只关当前标签（Ctrl+W）：关完还有别的标签就留着窗口；
     *   0 = 退出：继续推进 —— 下一个标签若也脏，会再弹一次提示，
     *       全部收口才 DestroyWindow。多文档之后「只问前台那一份就退出」
     *       等于把后台标签的修改直接丢掉。
     */
    doc_dialog_close(shell, nullptr);
    if (shell.close_intent == 1) {
      shell.close_intent = 0;
      if (!tab_close_active(shell)) DestroyWindow(window);
      return;
    }
    /*
     * 退出：先**关掉刚答完的那一份**（选「不保存」就是丢弃这一份），再继续收口
     * 其余标签。
     *
     * 之前这里直接 close_flow_step —— 它会回头检查刚答完的那份，发现它还脏，
     * 于是又弹一次提示：按「不保存」永远关不掉窗口（视觉探针 close_prompt 场景
     * 抓到的就是这条）。答完即关，关完再问下一份。
     */
    const bool remains = tab_close_active(shell);
    if (!remains) {
      shell.close_intent = 0;
      DestroyWindow(window);
      return;
    }
    if (!close_flow_step(shell)) {
      shell.close_intent = 0;
      DestroyWindow(window);
    }
    return;
  }

  if (kind == comp_ui::DocDialogKind::RecentFiles) {
    const int count = static_cast<int>(shell.recent_files.size());
    if (count == 0) {
      doc_dialog_close(shell, L"最近列表为空（打开过 .comp 之后才会记录）");
      return;
    }
    int index = shell.recent_index;
    if (index < 0 || index >= count) index = 0;
    const std::string path = shell.recent_files[index];
    doc_dialog_close(shell, nullptr);
    if (!load_document(shell, path.c_str())) {
      /* 路径已经打不开：从列表里摘掉，免得下次又点它。 */
      for (size_t i = 0; i < shell.recent_files.size(); ++i) {
        if (shell.recent_files[i] == path) {
          shell.recent_files.erase(shell.recent_files.begin() + static_cast<long>(i));
          break;
        }
      }
      recent_save(shell);
      set_status(shell, L"打开失败：文件可能已被移动或删除（已从最近列表移除）");
    }
    return;
  }
  if (kind == comp_ui::DocDialogKind::NewDocument) {
    const int width = shell.doc.doc_dialog_width;
    const int height = shell.doc.doc_dialog_height;
    const int dpi = shell.doc.doc_dialog_dpi;
    const int background = shell.doc.doc_dialog_background;
    const int pixel_format = shell.doc.doc_dialog_pixel_format;
    const int color_mode = shell.doc.doc_dialog_color_mode;
    /* 先记设置再建：建失败也保留用户刚填的参数。 */
    shell.new_doc_width = width;
    shell.new_doc_height = height;
    shell.new_doc_dpi = dpi;
    shell.new_doc_background = background;
    shell.new_doc_pixel_format = pixel_format;
    shell.new_doc_color_mode = color_mode;
    if (!create_document_opts(shell, width, height, dpi, background, pixel_format, color_mode)) {
      doc_dialog_close(shell, L"新建文档失败（未改动当前文档）");
      return;
    }
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[192];
    wsprintfW(buffer, L"已新建 %d×%d @%d dpi（%s · %s · 背景：%s）", width, height, dpi,
              widen_utf8(comp_ui::doc_new_bit_depth_name(pixel_format)).c_str(),
              widen_utf8(comp_ui::doc_new_color_mode_name(color_mode)).c_str(),
              widen_utf8(comp_ui::doc_new_background_name(background)).c_str());
    refresh_history_status(shell, buffer);
    return;
  }

  if (shell.doc.document == nullptr) {
    doc_dialog_close(shell, L"文档：还没有打开任何文档");
    return;
  }

  /*
   * 色标表（P1-7）：只改**工具状态**，不碰文档像素，因此不开事务、不落撤销步。
   * 校验用与落笔同一份 gradient_stops_valid；不合法时保持面板打开，让用户改，
   * 而不是关掉面板再把错误丢进状态栏。
   */
  if (kind == comp_ui::DocDialogKind::GradientStops) {
    const int count = static_cast<int>(shell.gradient_custom_stops.size());
    if (!comp_ui::gradient_stops_valid(shell.gradient_custom_stops.data(), count)) {
      set_status(shell,
                 L"色标表不合法：需要 2..32 个、位置严格升序、至少一个不透明色标（面板保留）");
      return;
    }
    shell.gradient_preset = comp_ui::kGradientPresetCustom;
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[192];
    wsprintfW(buffer, L"已应用 %d 个色标（自定义）：拖拽落笔，Shift 反向", count);
    set_status(shell, buffer);
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

  if (kind == comp_ui::DocDialogKind::TransformLayer) {
    const comp_layer_id target = selected_layer_id(shell);
    if (target == 0) {
      doc_dialog_close(shell, L"数值变换：目标图层已不存在（未改动）");
      return;
    }
    /*
     * 角度行没动过就写回原始 float：用户只想改宽高时，不该因为面板按整数显示
     * 就把 22.5° 圆成 23°。判据是「显示值仍等于打开时的取整值」。
     */
    const bool keep_rotation = shell.doc.doc_dialog_angle == shell.doc.doc_dialog_angle_base;
    const int applied = panel_apply_layer_transform(
        shell, target, shell.doc.doc_dialog_width, shell.doc.doc_dialog_height,
        static_cast<double>(shell.doc.doc_dialog_angle), keep_rotation);
    if (applied < 0) {
      /* 面板留着，用户填的数不丢；引擎在事务之前就拒了，没有半成品。 */
      set_status(shell, L"数值变换：应用失败（图层未改动）");
      return;
    }
    if (applied == 0) {
      set_status(shell, L"数值变换：尺寸与角度未变（未产生撤销步）");
      return;
    }
    doc_dialog_close(shell, nullptr);
    wchar_t buffer[224];
    wsprintfW(buffer, L"数值变换：%d×%d · %d 度（撤销一步）", shell.doc.doc_dialog_width,
              shell.doc.doc_dialog_height, shell.doc.doc_dialog_angle);
    refresh_history_status(shell, buffer);
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
    smart_panel_reset(shell);
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
    /* ---- 文件 > 新建预设（P0-1 收尾）----
     * 直接调到 create_document_with，不经过对话框；背景参数仍复用对话框那套
     * 编码（0 白 / 1 透明 / 2 前景色），这里固定 0。 */
    case kCmdNewPresetHd:
      create_document_with(shell, 1920, 1080, 72, 0);
      break;
    case kCmdNewPresetA4:
      create_document_with(shell, 2480, 3508, 300, 0);
      break;
    case kCmdNewPresetWeb:
      create_document_with(shell, 1440, 900, 72, 0);
      break;
    case kCmdNewPresetPhoto:
      create_document_with(shell, 4000, 3000, 300, 0);
      break;
    case kCmdSaveAs:
      save_document_dialog(window, shell);
      break;
    /* ---- P0-3 多文档 ---- */
    case kCmdCloseDocument: {
      /* 关当前标签：脏则先问。最后一个标签关掉就等于关窗（与 PS 同口径）。 */
      int32_t dirty = 0;
      const bool is_dirty =
          shell.doc.document != nullptr &&
          comp_document_is_dirty(shell.doc.document, &dirty) == COMP_OK && dirty != 0;
      if (is_dirty) {
        shell.close_intent = 1;
        doc_dialog_begin(shell, comp_ui::DocDialogKind::ClosePrompt);
        break;
      }
      shell.close_intent = 0;
      if (!tab_close_active(shell)) DestroyWindow(window);
      break;
    }
    case kCmdNextTab:
      tab_step(shell, 1);
      break;
    case kCmdPrevTab:
      tab_step(shell, -1);
      break;
    case kCmdRecent:
      doc_dialog_begin(shell, comp_ui::DocDialogKind::RecentFiles);
      break;
    /* ---- P0-10：光标模式与面板停靠（纯视图开关，不进撤销栈） ---- */
    case kCmdCursorStandard:
      shell.cursor_mode = 0;
      set_status(shell, L"光标：标准");
      break;
    case kCmdCursorCross:
      shell.cursor_mode = 1;
      set_status(shell, L"光标：十字（画布内）");
      break;
    case kCmdCursorHidden:
      shell.cursor_mode = 2;
      set_status(shell, L"光标：隐藏（笔尖轮廓仍然可见）");
      break;
    case kCmdPanelDockRight:
      shell.dock.set_area(comp_dock::PanelId::Adjust, comp_dock::DockArea::Right);
      set_status(shell, L"调整面板已停靠右侧");
      break;
    case kCmdPanelDockLeft:
      shell.dock.set_area(comp_dock::PanelId::Adjust, comp_dock::DockArea::Left);
      set_status(shell, L"调整面板已停靠左侧");
      break;
    /* ---- P0-10：浮动面板（独立顶层窗口）---- */
    case kCmdFloatAdjust:
      toggle_float_panel(shell, comp_dock::PanelId::Adjust);
      break;
    case kCmdFloatLayers:
      toggle_float_panel(shell, comp_dock::PanelId::Layers);
      break;
    /* ---- P0-10：面板分组（tab strip）---- */
    case kCmdPanelGroupMerge:
      panel_group_merge(shell);
      break;
    case kCmdPanelGroupSplit:
      panel_group_split(shell);
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
    /* ---- P1-2 第二批：合并可见 / 反向排列 ---- */
    case kCmdLayerMergeVisible:
      panel_merge_visible(shell);
      break;
    case kCmdLayerReverse:
      panel_reverse_layers(shell);
      break;
    /* ---- P1-2 第三批：链接图层 ---- */
    case kCmdLayerLinkToggle:
      panel_toggle_link(shell);
      break;
    /* ---- P1-2 第四批：图层样式拷贝 / 粘贴（剪贴板由外壳持有） ---- */
    case kCmdLayerCopyStyle:
      panel_copy_layer_style(shell);
      break;
    case kCmdLayerPasteStyle:
      panel_paste_layer_style(shell);
      break;
    /* ---- P1-2 收尾：图层面板过滤（视图态） ---- */
    case kCmdLayerFilterName:
      panel_begin_filter_name(shell);
      break;
    case kCmdLayerFilterClear:
      panel_clear_filter(shell);
      break;
    /* ---- P1-2 收尾：图层复合（可见性 + 不透明度的具名快照） ---- */
    case kCmdLayerCompCapture:
      panel_layer_comp_capture_begin(shell);
      break;
    case kCmdLayerCompApply:
      panel_layer_comp_apply(shell);
      break;
    case kCmdLayerCompUpdate:
      panel_layer_comp_update(shell);
      break;
    case kCmdLayerCompRename:
      panel_layer_comp_rename_begin(shell);
      break;
    case kCmdLayerCompDelete:
      panel_layer_comp_delete(shell);
      break;
    case kCmdLayerCompPrev:
      panel_layer_comp_select(shell, -1);
      break;
    case kCmdLayerCompNext:
      panel_layer_comp_select(shell, 1);
      break;
    case kCmdLayerCompStatus:
      panel_layer_comp_status(shell);
      break;
    /* ---- P1-2 收尾 ⑦：智能对象 / 智能滤镜 ---- */
    case kCmdLayerSmartConvert:
      panel_smart_convert(shell);
      break;
    case kCmdLayerSmartRasterize:
      panel_smart_rasterize(shell);
      break;
    case kCmdLayerSmartClear:
      panel_smart_clear_filters(shell);
      break;
    case kCmdLayerSmartStatus:
      panel_smart_status(shell);
      break;
    /* ---- P1-2 收尾 ⑨：智能滤镜栈面板（1720 段） ---- */
    case kCmdLayerSmartPanel:
      if (shell.doc.smart_open) {
        close_smart_panel(shell);
      } else {
        open_smart_panel(shell);
      }
      break;
    case kCmdLayerSmartPanelToggle:
      smart_panel_toggle_entry(shell);
      break;
    case kCmdLayerSmartPanelEdit:
      smart_panel_edit_entry(shell);
      break;
    case kCmdLayerSmartPanelRemove:
      smart_panel_remove_entry(shell);
      break;
    /* ---- P1-2 收尾 ⑩：栈序与展示名（1724 段） ---- */
    case kCmdLayerSmartPanelRename:
      panel_begin_smart_filter_name(shell);
      break;
    case kCmdLayerSmartPanelMoveUp:
      smart_panel_move_entry(shell, -1);
      break;
    case kCmdLayerSmartPanelMoveDown:
      smart_panel_move_entry(shell, 1);
      break;
    /* ---- P1-2 收尾 ⑧：填充图层（纯色 / 渐变 / 图案） ---- */
    case kCmdLayerFillSolid:
      panel_add_fill(shell, COMP_FILL_SOLID);
      break;
    case kCmdLayerFillGradient:
      panel_add_fill(shell, COMP_FILL_GRADIENT);
      break;
    case kCmdLayerFillPattern:
      panel_add_fill(shell, COMP_FILL_PATTERN);
      break;
    case kCmdLayerFillEdit:
      panel_edit_fill(shell);
      break;
    case kCmdLayerFillRasterize:
      panel_rasterize_fill(shell);
      break;
    case kCmdLayerFillStatus:
      panel_fill_status(shell);
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
    /* ---- P1-3：数值变换 / 再次变换 ---- */
    case kCmdLayerTransformNumeric:
      doc_dialog_begin(shell, comp_ui::DocDialogKind::TransformLayer);
      break;
    case kCmdLayerTransformAgain:
      panel_transform_again(shell);
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

    /* ---- L20：对齐（6 档）与分布（2 档）。基准在 panel_align_layers 内判定 ---- */
    case kCmdLayerAlignBase:
    case kCmdLayerAlignBase + 1:
    case kCmdLayerAlignBase + 2:
    case kCmdLayerAlignBase + 3:
    case kCmdLayerAlignBase + 4:
    case kCmdLayerAlignBase + 5:
      panel_align_layers(shell, static_cast<int32_t>(id - kCmdLayerAlignBase));
      break;
    case kCmdLayerDistributeH:
      panel_distribute_layers(shell, 0);
      break;
    case kCmdLayerDistributeV:
      panel_distribute_layers(shell, 1);
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
    /* ---- P1-2：图层锁（菜单单选组）与颜色标签（基址 + 0..7） ---- */
    case kCmdLayerLockNone:
      panel_set_lock(shell, COMP_LAYER_LOCK_NONE);
      break;
    case kCmdLayerLockTransparent:
      panel_set_lock(shell, COMP_LAYER_LOCK_TRANSPARENT);
      break;
    case kCmdLayerLockPixels:
      panel_set_lock(shell, COMP_LAYER_LOCK_PIXELS);
      break;
    case kCmdLayerLockPosition:
      panel_set_lock(shell, COMP_LAYER_LOCK_POSITION);
      break;
    case kCmdLayerLockAll:
      panel_set_lock(shell, COMP_LAYER_LOCK_ALL);
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
       * 面板过滤（P1-2 收尾）：类型基址 + 0..6、颜色三段（全部 / 无标签 / 基址 +
       * 0..6）。号段在 1680..1698，与锁/标签的 1660..1672 相邻但不重叠。
       */
      if (id >= kCmdLayerFilterKindBase && id <= kCmdLayerFilterKindBase + 6) {
        panel_set_filter_kind(shell, static_cast<int>(id - kCmdLayerFilterKindBase));
        break;
      }
      if (id == kCmdLayerFilterColorAll) {
        panel_set_filter_color(shell, -1);
        break;
      }
      if (id == kCmdLayerFilterColorNone) {
        panel_set_filter_color(shell, 0);
        break;
      }
      if (id >= kCmdLayerFilterColorBase && id < kCmdLayerFilterColorBase + 7) {
        panel_set_filter_color(shell, static_cast<int>(id - kCmdLayerFilterColorBase) + 1);
        break;
      }
      /*
       * 颜色标签菜单项（P1-2）：kCmdLayerColorBase + label 0..7，0 = 无。
       * 号段在 1665..1672，已在图像调整的 64 宽宽容区间（1590..1653）之外；
       * 仍排在调整之前只是防御性顺序，别把锁/标签挪回 1610 段——那里是
       * COMP_ADJUSTMENT_* 枚举值 20..24 的号，会与自动三档等菜单项互相顶飞。
       */
      if (id >= kCmdLayerColorBase && id < kCmdLayerColorBase + COMP_LAYER_COLOR_COUNT) {
        panel_set_color_label(shell, static_cast<uint32_t>(id - kCmdLayerColorBase));
        break;
      }
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
      /* 效果菜单项（S24）：kCmdEffectBase + kind 0..9，勾选 = 已启用。 */
      if (id >= kCmdEffectBase && id < kCmdEffectBase + comp_ui::kEffectKindCount) {
        panel_toggle_effect(shell, static_cast<int>(id - kCmdEffectBase));
        break;
      }
      return;
  }
  InvalidateRect(window, nullptr, FALSE);
}

}  // namespace comp_shell
