// AI生成
/*
 * shell_fonts.cpp — 字体枚举与选择器（P0-6②）
 *
 * 为什么在壳里做、且不新增 ABI：引擎侧的字体 ABI 早就齐了——
 *   comp_font_load_file 读任意 TTF/OTF/TTC（UTF-8 路径，超过 100 MB 拒收），
 *   comp_font_get_metrics 给 face_count / scalable / units_per_em / has_cjk，
 *   comp_font_name 给族名与样式名，comp_font_release 归还。
 * 缺的从来不是能力，而是「把系统里的字体列出来让用户挑」这条产品通路。
 * 字体枚举是**平台知识**（Windows 的字体目录、扩展名、per-user 安装位置），
 * 引擎不该知道这些，所以目录扫描放在壳侧；像素与排版仍然全部回引擎。
 *
 * 扫描分片：字体目录里常有上百个文件、单个几十 MB，同步扫完会冻住 UI
 * （实测 153 个文件约 1.2 s）。这里按 WM_TIMER 每片探测 kFontScanPerTick 个，
 * 扫描期间面板照常可开、可滚、可应用已探测到的条目。
 *
 * 句柄不及时归还就会常驻几十 MB：comp_font 没有全局缓存，本文件对每个面
 * 「打开 -> 读名字与度量 -> 立即 release」，只把字符串留下。
 */
#include "shell_internal.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace comp_shell {

namespace {

std::string wide_to_utf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return std::string();
  std::string out(static_cast<size_t>(size - 1), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &out[0], size, nullptr, nullptr) <= 0) {
    return std::string();
  }
  return out;
}

/* 只认可缩放的轮廓字体：位图字体（units_per_em == 0）排不了版，不进列表。 */
bool font_extension_matches(const wchar_t* name) {
  const wchar_t* dot = std::wcsrchr(name, L'.');
  if (dot == nullptr) return false;
  return _wcsicmp(dot, L".ttf") == 0 || _wcsicmp(dot, L".otf") == 0 ||
         _wcsicmp(dot, L".ttc") == 0;
}

std::wstring file_stem(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.');
  return dot == std::wstring::npos ? name : name.substr(0, dot);
}

std::wstring lower_wstring(const std::wstring& text) {
  std::wstring out = text;
  for (wchar_t& ch : out) {
    ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  }
  return out;
}

/* 去重键：Windows 路径大小写不敏感。 */
std::wstring path_key(const std::wstring& path) {
  std::wstring key = path;
  for (wchar_t& ch : key) {
    if (ch == L'/') ch = L'\\';
    ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  }
  return key;
}

void queue_directory(const std::wstring& directory, bool shipped,
                     std::vector<FontCandidate>* queue, std::vector<std::wstring>* seen) {
  if (directory.empty()) return;
  WIN32_FIND_DATAW data{};
  const std::wstring pattern = directory + L"\\*";
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) return;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    if (!font_extension_matches(data.cFileName)) continue;
    const std::wstring full = directory + L"\\" + data.cFileName;
    const std::wstring key = path_key(full);
    if (std::find(seen->begin(), seen->end(), key) != seen->end()) continue;
    seen->push_back(key);
    FontCandidate candidate;
    candidate.path_w = full;
    candidate.path_utf8 = wide_to_utf8(full);
    candidate.shipped = shipped;
    if (!candidate.path_utf8.empty()) queue->push_back(std::move(candidate));
  } while (FindNextFileW(handle, &data) != 0);
  FindClose(handle);
}

/*
 * 候选顺序：随仓字体（启动时已成功加载的那支）-> 系统字体目录 -> 当前用户
 * 安装的字体。同一份文件只探一次；目录内的顺序由 FindFirstFile 决定，
 * 结果在扫描结束时按「内置 -> 随仓 -> 含 CJK -> 名字」重排，所以不影响面板。
 */
void build_scan_queue(ShellState& shell) {
  shell.font_scan_queue.clear();
  std::vector<std::wstring> seen;
  if (!shell.type_font_path.empty()) {
    const std::wstring shipped = widen_utf8(shell.type_font_path);
    FontCandidate candidate;
    candidate.path_w = shipped;
    candidate.path_utf8 = shell.type_font_path;
    candidate.shipped = true;
    seen.push_back(path_key(shipped));
    shell.font_scan_queue.push_back(std::move(candidate));
  }
  wchar_t buffer[MAX_PATH];
  if (GetWindowsDirectoryW(buffer, MAX_PATH) > 0) {
    queue_directory(std::wstring(buffer) + L"\\Fonts", false, &shell.font_scan_queue, &seen);
  }
  const DWORD local = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  if (local > 0 && local < MAX_PATH) {
    queue_directory(std::wstring(buffer) + L"\\Microsoft\\Windows\\Fonts", false,
                    &shell.font_scan_queue, &seen);
  }
}

/* 一个面：读名字与度量，句柄由调用方释放。探测失败返回 false（不写条目）。 */
bool probe_face(comp_font font, const FontCandidate& candidate, int32_t face_index,
                ShellFontEntry* out) {
  comp_font_metrics metrics{};
  if (font == nullptr || comp_font_get_metrics(font, &metrics) != COMP_OK) return false;
  if (metrics.scalable == 0 || metrics.units_per_em == 0) return false;
  std::wstring family;
  std::wstring style;
  const char* family_name = comp_font_name(font, COMP_FONT_NAME_FAMILY);
  const char* style_name = comp_font_name(font, COMP_FONT_NAME_STYLE);
  if (family_name != nullptr && family_name[0] != '\0') family = widen_utf8(family_name);
  if (style_name != nullptr && style_name[0] != '\0') style = widen_utf8(style_name);
  if (family.empty()) family = file_stem(candidate.path_w);
  if (family.empty()) return false;
  out->label = family;
  if (!style.empty() && _wcsicmp(style.c_str(), L"Regular") != 0 &&
      _wcsicmp(style.c_str(), L"Normal") != 0) {
    out->label += L" ";
    out->label += style;
  }
  out->path = candidate.path_utf8;
  out->face_index = metrics.face_index >= 0 ? metrics.face_index : face_index;
  out->builtin = false;
  out->shipped = candidate.shipped;
  out->cjk = metrics.has_cjk != 0;
  return true;
}

/* 探测一个字体文件：.ttc 按面拆条目（最多 kFontScanMaxFaces 个面）。 */
void probe_file(ShellState& shell, const FontCandidate& candidate) {
  comp_font first = nullptr;
  if (comp_font_load_file(candidate.path_utf8.c_str(), 0, &first) != COMP_OK || first == nullptr) {
    ++shell.font_scan_skipped;
    return;
  }
  comp_font_metrics metrics{};
  const bool usable = comp_font_get_metrics(first, &metrics) == COMP_OK &&
                      metrics.scalable != 0 && metrics.units_per_em > 0;
  if (!usable) {
    comp_font_release(first);
    ++shell.font_scan_skipped;
    return;
  }
  const int32_t faces = metrics.face_count > 0 ? metrics.face_count : 1;
  const int32_t limit = std::min<int32_t>(faces, kFontScanMaxFaces);
  for (int32_t face = 0; face < limit; ++face) {
    comp_font font = first;
    if (face > 0) {
      font = nullptr;
      if (comp_font_load_file(candidate.path_utf8.c_str(), face, &font) != COMP_OK ||
          font == nullptr) {
        ++shell.font_scan_skipped;
        continue;
      }
    }
    ShellFontEntry entry;
    const bool ok = probe_face(font, candidate, face, &entry);
    if (face > 0) comp_font_release(font);
    if (ok) shell.font_entries.push_back(std::move(entry));
  }
  comp_font_release(first);
}

/* 面板顺序：内置点阵 -> 随仓字体 -> 含 CJK -> 名字（大小写不敏感）。 */
bool font_entry_before(const ShellFontEntry& left, const ShellFontEntry& right) {
  if (left.builtin != right.builtin) return left.builtin;
  if (left.shipped != right.shipped) return left.shipped;
  if (left.cjk != right.cjk) return left.cjk;
  const std::wstring a = lower_wstring(left.label);
  const std::wstring b = lower_wstring(right.label);
  if (a != b) return a < b;
  if (left.face_index != right.face_index) return left.face_index < right.face_index;
  return left.path < right.path;
}

}  // namespace

/* ------------------------------------------------------------ 扫描分片 */

/*
 * 右栏一次只挂一个面板：字体面板让位给路径/历史/色彩范围/裁剪/效果/滤镜。
 * 让位时不丢状态（font_open 与扫描进度都留着），关掉别的面板它就回来——
 * 否则「键被一边吃掉、画面画的是另一边」这种错位只会让人以为程序卡了。
 */
bool font_panel_visible(const ShellState& shell) {
  if (!shell.font_open) return false;
  return !shell.doc.path_open && !shell.doc.history_open && !shell.doc.color_range_open &&
         !shell.doc.trim_open && !shell.doc.effect_open && !shell.doc.filter_open;
}

void font_refilter(ShellState& shell) {
  const ShellFontEntry* selected = font_entry_at_row(shell, shell.font_selected);
  std::string keep_path;
  int32_t keep_face = 0;
  bool keep_builtin = false;
  bool had = false;
  if (selected != nullptr) {
    keep_path = selected->path;
    keep_face = selected->face_index;
    keep_builtin = selected->builtin;
    had = true;
  }
  shell.font_filtered.clear();
  for (size_t i = 0; i < shell.font_entries.size(); ++i) {
    if (comp_ui::font_label_matches(shell.font_entries[i].label, shell.font_filter)) {
      shell.font_filtered.push_back(static_cast<int>(i));
    }
  }
  int row = 0;
  if (had) {
    for (size_t i = 0; i < shell.font_filtered.size(); ++i) {
      const ShellFontEntry& entry = shell.font_entries[static_cast<size_t>(shell.font_filtered[i])];
      if (entry.builtin == keep_builtin && entry.face_index == keep_face &&
          entry.path == keep_path) {
        row = static_cast<int>(i);
        break;
      }
    }
  }
  const int count = static_cast<int>(shell.font_filtered.size());
  if (count <= 0) {
    shell.font_selected = 0;
    shell.font_scroll = 0;
    return;
  }
  if (row >= count) row = count - 1;
  shell.font_selected = row;
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.font_scroll =
        comp_ui::font_clamp_scroll(layout.adjust_panel, count, shell.font_scroll);
  }
}

int font_row_count(const ShellState& shell) {
  return static_cast<int>(shell.font_filtered.size());
}

const ShellFontEntry* font_entry_at_row(const ShellState& shell, int row) {
  if (row < 0 || row >= static_cast<int>(shell.font_filtered.size())) return nullptr;
  const int index = shell.font_filtered[static_cast<size_t>(row)];
  if (index < 0 || index >= static_cast<int>(shell.font_entries.size())) return nullptr;
  return &shell.font_entries[static_cast<size_t>(index)];
}

/*
 * 开启扫描（每进程一次）。内置点阵先占第 0 行：即使一个系统字体都探不出来，
 * 面板也不是空的，用户至少能明确回到 ASCII 轨道。
 * 第一片同步探测，面板打开立刻就有内容；其余片交给 WM_TIMER。
 */
void font_scan_start(HWND window, ShellState& shell) {
  shell.font_scan_started = true;
  shell.font_scan_done = false;
  shell.font_scan_cursor = 0;
  shell.font_scan_skipped = 0;
  shell.font_entries.clear();
  ShellFontEntry builtin;
  builtin.label = L"5×7 点阵（内置，仅 ASCII）";
  builtin.builtin = true;
  shell.font_entries.push_back(std::move(builtin));
  build_scan_queue(shell);
  if (!shell.font_scan_queue.empty() && window != nullptr) {
    SetTimer(window, kFontScanTimerId, kFontScanPeriodMs, nullptr);
  }
  font_scan_step(window, shell);
}

void font_scan_step(HWND window, ShellState& shell) {
  if (!shell.font_scan_started || shell.font_scan_done) {
    if (window != nullptr) KillTimer(window, kFontScanTimerId);
    return;
  }
  int budget = kFontScanPerTick;
  while (budget-- > 0 && shell.font_scan_cursor < shell.font_scan_queue.size()) {
    probe_file(shell, shell.font_scan_queue[shell.font_scan_cursor]);
    ++shell.font_scan_cursor;
  }
  if (shell.font_scan_cursor >= shell.font_scan_queue.size()) {
    shell.font_scan_done = true;
    if (window != nullptr) KillTimer(window, kFontScanTimerId);
    /*
     * 排序会打乱 font_filtered 里的下标——先按「身份」记下当前选中项，排完
     * 重建成新下标再定位。否则面板会在扫描结束时跳到一支无名字体上。
     */
    std::string keep_path;
    int32_t keep_face = 0;
    bool keep_builtin = false;
    bool had_selection = false;
    if (const ShellFontEntry* selected = font_entry_at_row(shell, shell.font_selected)) {
      keep_path = selected->path;
      keep_face = selected->face_index;
      keep_builtin = selected->builtin;
      had_selection = true;
    }
    std::stable_sort(shell.font_entries.begin(), shell.font_entries.end(), font_entry_before);
    shell.font_filtered.clear();
    for (size_t i = 0; i < shell.font_entries.size(); ++i) {
      shell.font_filtered.push_back(static_cast<int>(i));
    }
    if (had_selection) {
      for (size_t i = 0; i < shell.font_entries.size(); ++i) {
        const ShellFontEntry& entry = shell.font_entries[i];
        if (entry.builtin == keep_builtin && entry.face_index == keep_face &&
            entry.path == keep_path) {
          shell.font_selected = static_cast<int>(i);
          break;
        }
      }
    }
    /* 探测完才发现当前轨道对应的条目时，把它滚进可视区（不影响用户已选的条目）。 */
    font_select_current_track(shell, /*force=*/false);
  }
  font_refilter(shell);
  if (window != nullptr && shell.font_open) {
    InvalidateRect(window, nullptr, FALSE);
  }
}

/* 面板打开时把当前轨道对应的条目选中，用户一进来就看到「现在用的是哪支」。 */
void font_select_current_track(ShellState& shell, bool force) {
  const bool builtin = !type_font_track(shell);
  const std::string path = builtin ? std::string() : shell.type_font_path;
  const int32_t face = builtin ? 0 : shell.type_face_index;
  int row = -1;
  for (size_t i = 0; i < shell.font_filtered.size(); ++i) {
    const ShellFontEntry& entry = shell.font_entries[static_cast<size_t>(shell.font_filtered[i])];
    if (entry.builtin == builtin && entry.face_index == face && entry.path == path) {
      row = static_cast<int>(i);
      break;
    }
  }
  if (row < 0) return;
  if (!force && shell.font_selected == row) return;
  shell.font_selected = row;
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.font_scroll = comp_ui::font_ensure_visible(layout.adjust_panel, font_row_count(shell),
                                                     row, shell.font_scroll);
  }
}

/* ------------------------------------------------------------ 面板操作 */

void font_panel_close(ShellState& shell) {
  shell.font_open = false;
  /* 扫描状态保留（cursor / entries 不动），下次打开接着探；计时器先停，
   * 面板不可见时没有理由继续重绘。 */
  if (shell.window != nullptr) KillTimer(shell.window, kFontScanTimerId);
}

void font_panel_toggle(HWND window, ShellState& shell) {
  if (font_panel_visible(shell)) {
    font_panel_close(shell);
    set_status(shell, L"已关闭字体面板");
    return;
  }
  if (shell.doc.doc_dialog != comp_ui::DocDialogKind::None) {
    set_status(shell, L"字体面板：请先关闭当前对话框");
    return;
  }
  /* 右栏一次只挂一个面板：色彩范围持有显式事务，必须先 abort 再让位。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.trim_open = false;
  shell.doc.trim_input.clear();
  shell.doc.path_open = false;
  shell.doc.history_open = false;
  shell.font_open = true;
  if (!shell.font_scan_started) {
    font_scan_start(window, shell);
  } else if (!shell.font_scan_done && window != nullptr) {
    SetTimer(window, kFontScanTimerId, kFontScanPeriodMs, nullptr);
  }
  font_refilter(shell);
  font_select_current_track(shell, /*force=*/true);
  font_status(shell);
}

void font_select(ShellState& shell, int delta) {
  const int count = font_row_count(shell);
  if (count <= 0) {
    font_status(shell);
    return;
  }
  int row = shell.font_selected + delta;
  if (row < 0) row = 0;
  if (row >= count) row = count - 1;
  shell.font_selected = row;
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.font_scroll = comp_ui::font_ensure_visible(layout.adjust_panel, count, row,
                                                     shell.font_scroll);
  }
  font_status(shell);
}

void font_select_edge(ShellState& shell, bool end) {
  const int count = font_row_count(shell);
  if (count <= 0) return;
  shell.font_selected = end ? count - 1 : 0;
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.font_scroll = comp_ui::font_ensure_visible(layout.adjust_panel, count,
                                                     shell.font_selected, shell.font_scroll);
  }
  font_status(shell);
}

void font_filter_char(ShellState& shell, wchar_t unit) {
  if (unit < 0x20 || unit == 0x7F) return;
  shell.font_filter.push_back(unit);
  font_refilter(shell);
  font_status(shell);
}

void font_filter_backspace(ShellState& shell) {
  if (shell.font_filter.empty()) return;
  shell.font_filter.pop_back();
  font_refilter(shell);
  font_status(shell);
}

void font_filter_clear(ShellState& shell) {
  if (shell.font_filter.empty()) return;
  shell.font_filter.clear();
  font_refilter(shell);
  font_status(shell);
}

/* ------------------------------------------------------------ 应用字体 */

/*
 * 切换「下一次打字」的轨道。真实字体必须真的能打开才切换——拿不到句柄却
 * 把 type_use_font 置 true，会在提交时才以 COMP_ERR_IO 失败（用户已经打完字）。
 * 回到内置点阵不释放已有句柄：切回来时无需再读盘。
 */
static bool apply_track(ShellState& shell, const ShellFontEntry& entry) {
  if (entry.builtin) {
    shell.type_use_font = false;
    shell.type_face_index = 0;
    return true;
  }
  if (type_font_track(shell) && shell.type_font_path == entry.path &&
      shell.type_face_index == entry.face_index) {
    return true;
  }
  comp_font font = nullptr;
  if (comp_font_load_file(entry.path.c_str(), entry.face_index, &font) != COMP_OK ||
      font == nullptr) {
    return false;
  }
  comp_font_metrics metrics{};
  if (comp_font_get_metrics(font, &metrics) != COMP_OK || metrics.scalable == 0 ||
      metrics.units_per_em == 0) {
    comp_font_release(font);
    return false;
  }
  comp_font_release(shell.type_font);
  shell.type_font = font;
  shell.type_font_ascender = metrics.ascender;
  shell.type_font_descender = metrics.descender;
  shell.type_font_label = entry.label;
  shell.type_font_path = entry.path;
  shell.type_face_index = entry.face_index;
  shell.type_use_font = true;
  return true;
}

/*
 * 选中的是文字图层时，把字体重排一遍：get_text 拿走现有对象（文本、原点、
 * 字号、颜色、不透明度），只换 font_path / face_index，再 set_text ——
 * 一步撤销、一次重渲。非文字图层不动：字体面板改的是「下一次打字」的轨道，
 * 不该顺手给普通像素层记一条空撤销步。
 *
 * **不换算字号**：两条轨道的字号量纲不同（em / 点阵格），硬换算只会制造
 * 一个没人要的 5.2（与 type_panel_toggle_font 同一口径）。切到内置点阵而
 * 文本含 CJK 时引擎会如实拒绝（kBadText -> INVALID_ARGUMENT），这里照实报。
 */
static void apply_to_text_layer(ShellState& shell, const ShellFontEntry& entry) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.doc.document == nullptr || id == 0) return;
  if (!layer_is_text(shell, id)) return;
  size_t required = 0;
  comp_text_desc desc{};
  if (comp_layer_get_text(shell.doc.document, id, &desc, nullptr, 0, &required) != COMP_OK ||
      required == 0) {
    return;
  }
  std::vector<char> buffer(required);
  if (comp_layer_get_text(shell.doc.document, id, &desc, buffer.data(), buffer.size(),
                          &required) != COMP_OK) {
    return;
  }
  /* get_text 给的 font_path_utf8 指向引擎内部字符串；换字体时用我们自己的副本。 */
  const std::string path = entry.builtin ? std::string() : entry.path;
  desc.font_path_utf8 = path.empty() ? nullptr : path.c_str();
  desc.font_face_index = entry.builtin ? 0 : entry.face_index;
  const comp_result result = comp_layer_set_text(shell.doc.document, id, &desc);
  if (result == COMP_OK) {
    shell.doc.needs_render = true;
    refresh_layers(shell);
    wchar_t buffer_status[256];
    wsprintfW(buffer_status, L"字体已应用到文字图层（%s，一步撤销）", entry.label.c_str());
    refresh_history_status(shell, buffer_status);
    return;
  }
  const wchar_t* reason = L"文字图层未被修改";
  if (result == COMP_ERR_INVALID_ARGUMENT) {
    reason = L"文字图层未被修改（内置 5×7 点阵只排 ASCII；含中文请选真实字体）";
  } else if (result == COMP_ERR_IO) {
    reason = L"文字图层未被修改（字体文件打不开）";
  } else if (result == COMP_ERR_DECODE) {
    reason = L"文字图层未被修改（文件不是可用的字体面）";
  }
  set_status(shell, reason);
}

void font_apply_row(ShellState& shell, int row) {
  const ShellFontEntry* selected = font_entry_at_row(shell, row);
  if (selected == nullptr) {
    font_status(shell);
    return;
  }
  const ShellFontEntry entry = *selected; /* apply_track 可能让 vector 重排 */
  shell.font_selected = row;
  if (!apply_track(shell, entry)) {
    wchar_t buffer[256];
    wsprintfW(buffer, L"字体「%s」加载失败（文件不可读或不是可用的字体面）",
              entry.label.c_str());
    set_status(shell, buffer);
    return;
  }
  wchar_t prefix[256];
  wsprintfW(prefix, L"字体 = %s（%s）", entry.label.c_str(),
            entry.builtin ? L"内置 5×7 点阵，仅 ASCII，字号按格子高"
                          : L"真实字体，字号按 em 高");
  set_status(shell, prefix);
  apply_to_text_layer(shell, entry);
  font_status(shell, /*keep_prefix=*/true);
}

void font_status(ShellState& shell, bool keep_prefix) {
  const int count = font_row_count(shell);
  const ShellFontEntry* entry = font_entry_at_row(shell, shell.font_selected);
  wchar_t buffer[512];
  if (!keep_prefix) {
    if (entry == nullptr) {
      wsprintfW(buffer, L"字体：没有匹配「%s」的字体（Backspace 删字符，Esc 关闭）",
                shell.font_filter.c_str());
      set_status(shell, buffer);
      return;
    }
    wsprintfW(buffer, L"字体 %d/%d：%s%s · ↑/↓ 选行 · 回车或点击应用 · 直接键入过滤",
              shell.font_selected + 1, count, entry->label.c_str(),
              entry->cjk ? L" [CJK]" : L"");
  } else {
    wsprintfW(buffer, L"（%d/%d，%s%s）", shell.font_selected + 1, count,
              entry != nullptr ? entry->label.c_str() : L"-",
              entry != nullptr && entry->cjk ? L" [CJK]" : L"");
    shell.status += buffer;
    return;
  }
  if (!shell.font_filter.empty()) {
    wchar_t filter[192];
    wsprintfW(filter, L"｜过滤「%s」", shell.font_filter.c_str());
    lstrcatW(buffer, filter);
  }
  if (!shell.font_scan_done) {
    wchar_t progress[96];
    wsprintfW(progress, L"｜扫描中 %d/%d", static_cast<int>(shell.font_scan_cursor),
              static_cast<int>(shell.font_scan_queue.size()));
    lstrcatW(buffer, progress);
  }
  set_status(shell, buffer);
}

}  // namespace comp_shell
