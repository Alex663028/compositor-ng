// AI生成
/*
 * main.cpp — Windows 外壳层（M3）最小可运行骨架。
 *
 * 对应七层统一架构中的职责边界：
 *   平台外壳层    : 本文件负责窗口、消息循环、菜单/快捷键、图层面板与画布上屏（GDI blit）；
 *   交互与命令层  : 键盘/菜单映射为 C ABI 调用（新建/打开/保存、撤销/重做、图层与调整、
 *                   命令枚举），不触碰引擎内部；
 *   引擎核 C ABI  : 唯一集成边界（compositor/comp_*.h），壳不包含任何像素处理逻辑。
 *
 * 设计约束：
 *   1. 外壳不得 #include 任何 engine/ 内部头（domain/ compositing/ render/），
 *      否则七层架构的「ABI 边界」立即失效；本文件只依赖 abi/include。
 *   2. 所有合成由引擎完成；comp_engine_desc.backend = AUTO 由引擎按平台择优
 *      （Windows → D3D12），GPU 后端编入且驱动可用时真正参与合成，否则回落 CPU。
 *      实际生效的后端显示在工具条与窗口标题上，绝不静默假设。
 *   3. 画布像素为预乘 RGBA8，GDI 需要 BGRA 顺序，上屏前做一次 R/B 交换。
 *   4. 撤销语义完全由引擎承担（文档级撤销栈，comp_undo.h）；
 *      外壳只做「触发 + 展示深度」，不自己维护历史。
 *   5. 图层属性一律「查询→修改」走 ABI（含可见性：comp_layer_get_visible）；
 *      壳侧仍保留一份 visible 缓存，仅作 ABI 查询失败时的兜底。
 */
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <compositor/comp_camera_raw.h>
#include <compositor/comp_canvas.h>
#include <compositor/comp_command.h>
#include <compositor/comp_core.h>
#include <compositor/comp_crop.h>
#include <compositor/comp_engine.h>
#include <compositor/comp_eyedropper.h>
#include <compositor/comp_filter.h>
#include <compositor/comp_font.h>
#include <compositor/comp_io.h>
#include <compositor/comp_layer.h>
#include <compositor/comp_render.h>
#include <compositor/comp_selection.h>
#include <compositor/comp_undo.h>

#include "ui_core.h" /* 平台无关的视图变换 / 布局 / 命中 / 工具表 */
#include "shell_state.h" /* 状态定义（M3 拆分第一步） */
#include "shell_internal.h" /* 跨翻译单元的内部契约（M3 拆分第二步） */

namespace comp_shell {

ShellState* g_shell = nullptr;  /* 单窗口外壳，生命周期与窗口一致 */


void set_status(ShellState& shell, const wchar_t* text) {
  shell.status = text;
  if (shell.doc.document != nullptr) {
    const char* engine_error = comp_engine_last_error(shell.engine);
    if (engine_error != nullptr && engine_error[0] != '\0') {
      /* 引擎侧错误一并带上，避免壳层把失败静默掉。 */
      const std::string narrow(engine_error);
      shell.status += L" | ";
      shell.status.append(narrow.begin(), narrow.end());
    }
  }
}

/* 参数值在面板内用 ASCII 文本承载（避开 WideChar 转换噪声）。 */
std::string number_text(double value) {
  char buffer[32];
  if (std::fabs(value - std::floor(value)) < 1e-9) {
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%g", value);
  }
  return std::string(buffer);
}

/*
 * 窄字符串 -> UTF-16。名字里的 ascii 是历史遗留：壳层所有窄字面量实际上都是
 * UTF-8（选项栏标签 "大小"、枚举名 "前景→透明"、工具名 "Rectangle" 都在同一张
 * 窄表里）。早先逐字节提升把三个 UTF-8 字节显示成三个拉丁字符（"å¤§å°"），
 * 于是整条选项栏、部分枚举值、选项点击后的状态反馈全是乱码。
 *
 * 现在先按 UTF-8 严格解码（MB_ERR_INVALID_CHARS）；不是合法 UTF-8 的输入
 * （引擎回传的本地编码片段、任意二进制）退回旧的逐字节路径。ASCII 在两条
 * 路径上结果逐字符相同，所以纯 ASCII 调用的显示不变。
 */
std::wstring widen_ascii(const char* text) {
  if (text == nullptr) return std::wstring();
  const int size = static_cast<int>(std::strlen(text));
  if (size > 0) {
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, size, nullptr, 0);
    if (needed > 0) {
      std::wstring wide(static_cast<size_t>(needed), L'\0');
      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, size, wide.data(), needed) ==
          needed) {
        return wide;
      }
    }
  }
  std::wstring wide;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
  }
  return wide;
}

/*
 * UTF-8 -> UTF-16。失败返回空串：调用方（面板绘制）宁可显示 id 也不显示乱码。
 * MB_ERR_INVALID_CHARS 让坏编码显式失败，而不是被系统替换成 U+FFFD——后者会让
 * 一个本已损坏的名字在面板上看起来「正常」，掩盖真正的数据问题。
 */
std::wstring widen_utf8(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), wide.data(), needed) != needed) {
    return std::wstring();
  }
  return wide;
}

/* UTF-16 -> UTF-8（重命名提交路径）。未配对代理被系统替换为 U+FFFD，
 * 引擎侧 is_valid_utf8 随后会如实拒绝，不会把坏名字写进文档。 */
std::string narrow_utf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string narrow(static_cast<size_t>(needed), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                          needed, nullptr, nullptr) != needed) {
    return std::string();
  }
  return narrow;
}

}  // namespace comp_shell

using namespace comp_shell;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
  /*
   * ROI #7：DPI 感知必须在**任何窗口创建之前**设置（含 --present-bench 的窗口）。
   * 失败不阻断启动——最坏情况退回系统位图拉伸，功能不受影响。
   */
  dpi_enable_awareness();

  /* --selftest[=报告文件]：无窗口端到端自检（只走 C ABI），用于真机可审计验证。 */
  if (command_line != nullptr && wcsstr(command_line, L"--selftest") != nullptr) {
    const wchar_t* equals = wcschr(command_line, L'=');
    return run_selftest(equals != nullptr ? equals + 1 : L"compositor_shell_selftest.txt");
  }

  /* --present-bench[=帧数]：S16 窗口直呈基准（GPU 直呈 vs CPU flatten+GDI 上屏）。 */
  if (command_line != nullptr && wcsstr(command_line, L"--present-bench") != nullptr) {
    int frames = 20;
    const wchar_t* frame_flag = wcsstr(command_line, L"--present-bench=");
    if (frame_flag != nullptr) {
      const int parsed = _wtoi(frame_flag + wcslen(L"--present-bench="));
      if (parsed > 0 && parsed <= 1000) frames = parsed;
    }
    return run_present_bench(instance, frames);
  }

  ShellState shell;

  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-shell-windows";
  engine_desc.version = "0.1.0";
  /* AUTO：Windows 上优先 D3D12（该构建编入后端且设备可用时真正参与合成），否则回落 CPU。 */
  engine_desc.backend = COMP_BACKEND_AUTO;
  if (comp_engine_create(&engine_desc, &shell.engine) != COMP_OK) {
    MessageBoxW(nullptr, L"引擎创建失败", L"compositor", MB_ICONERROR);
    return 1;
  }

  comp_backend actual = COMP_BACKEND_AUTO;
  if (comp_engine_backend(shell.engine, &actual) == COMP_OK) {
    shell.backend = actual; /* 工具条“渲染 …”与窗口标题都显示这个真实值 */
  }
  if (shell.backend != COMP_BACKEND_CPU) {
    set_status(shell, L"引擎后端已启用硬件加速");
  }
  /* P1-5：镜头校正的 763 张实测表在启动时加载（缺文件不阻断启动，见函数注释）。 */
  load_shell_camera_raw_tables(shell);

  const wchar_t* class_name = L"CompositorShellWindow";
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  /* S24：CS_DBLCLKS —— 直方图双击复位需要 WM_LBUTTONDBLCLK（其它位置已还原成普通按下）。 */
  window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = class_name;
  if (RegisterClassExW(&window_class) == 0) {
    comp_engine_destroy(shell.engine);
    return 1;
  }

  /*
   * ROI #7：DPI 感知进程里 CreateWindowEx 的宽高是**设备像素**。默认尺寸按
   * 96 DPI 的逻辑像素给出，这里换算到当前系统 DPI，否则 200% 下首屏只有
   * 半个屏幕大小。文档默认尺寸仍用 kDefaultWidth/kDefaultHeight（逻辑值）。
   */
  const int startup_dpi = dpi_for_system();
  HWND window = CreateWindowExW(0, class_name, L"Compositor (Windows shell)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                dpi_device(kDefaultWidth, startup_dpi),
                                dpi_device(kDefaultHeight, startup_dpi), nullptr, build_menu(),
                                instance, nullptr);
  if (window == nullptr) {
    comp_engine_destroy(shell.engine);
    return 1;
  }

  g_shell = &shell;
  shell.window = window; /* 视图命令需要客户区矩形，必须早于首次绘制设置 */
  /* ROI #7：首帧之前拿到窗口所在显示器的 DPI（WM_CREATE 时 g_shell 还没挂上）。 */
  shell.dpi = dpi_for_window(window);
  /*
   * --dpi=N：诊断用的换算基准覆盖（96..480）。显示器恒为 100% 的机器（CI、
   * 远程会话）没有别的办法触发非 96 的 MM_ANISOTROPIC 映射路径，这个开关让
   * M52 的手动验证与截图复核在任何设备上都能复现 150%/200% 的排版与字号。
   * 它只改壳层自己的换算基准，不动进程 DPI 感知，也不改窗口实际像素尺寸。
   */
  if (command_line != nullptr) {
    const wchar_t* dpi_flag = wcsstr(command_line, L"--dpi=");
    if (dpi_flag != nullptr) {
      const int parsed = _wtoi(dpi_flag + wcslen(L"--dpi="));
      if (parsed >= 96 && parsed <= 480) shell.dpi = parsed;
    }
  }
  /* P0-3：最近打开列表（%APPDATA%），读失败等于空列表，不影响启动。 */
  recent_load(shell);
  create_document(shell);
  /*
   * P5-3：随仓字体在这里一次性加载（字体句柄独立于引擎，不需要文档）。
   * 失败不算错误：文字工具继续用 5×7 点阵，状态栏如实说明原因。
   */
  if (load_shell_font(shell)) {
    wchar_t font_status[192];
    wsprintfW(font_status, L"文字：已加载字体 %s（可键入中文，字号按 em 高）",
              shell.type_font_label.c_str());
    set_status(shell, font_status);
  } else {
    set_status(shell, L"文字：未找到随仓字体（resources/fonts），文字工具只用 5×7 点阵（ASCII）");
  }
  {
    /* 标题统一由 update_window_title 生成（后端 + 文件名 + 脏标记），首帧前先填一次。 */
    update_window_title(window, shell);
  }
  /* 蚂蚁线相位定时器：无选区时处理函数直接返回，不产生重绘。 */
  SetTimer(window, kAntsTimerId, kAntsTimerPeriodMs, nullptr);
  ShowWindow(window, show);
  UpdateWindow(window);

  /*
   * 命令行打开文件（compositor_shell.exe photo.png ／ *.comp）。
   * 与拖放共用 open_path_as_document，两条入口不分叉。
   * 只认第一个非开关参数，且必须是已存在的文件——沉默吞掉拼错的名字比报错更糟。
   */
  if (command_line != nullptr && command_line[0] != L'\0') {
    for (const wchar_t* cursor = command_line; *cursor != L'\0';) {
      while (*cursor == L' ' || *cursor == L'\t') ++cursor;
      if (*cursor == L'\0') break;
      const wchar_t* begin = cursor;
      while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t') ++cursor;
      if (begin[0] == L'-') continue; /* 跳过开关参数 */
      wchar_t path[MAX_PATH] = L"";
      const size_t length = static_cast<size_t>(cursor - begin);
      if (length > 0 && length < MAX_PATH) {
        wcsncpy_s(path, MAX_PATH, begin, length);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
          char utf8[MAX_PATH * 3] = {0};
          WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
          open_path_as_document(shell, utf8);
        }
      }
      break;
    }
  }

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  g_shell = nullptr;
  if (shell.doc.document != nullptr) comp_document_destroy(shell.doc.document);
  /* 字体句柄独立于引擎：引擎销毁不会带走它，必须显式释放（与 comp_font.h 一致）。 */
  if (shell.type_font != nullptr) comp_font_release(shell.type_font);
  comp_engine_destroy(shell.engine);
  return 0;
}
