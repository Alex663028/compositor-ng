// AI生成
/*
 * shell_internal.h — 外壳层跨翻译单元的内部声明（M3 拆分第二步）。
 *
 * main.cpp 原来是一个 1.9 万行的单文件：任何一处改动都要全量重编。这里沿用
 * ui_core.h 的做法，把「外壳内部契约」收拢到一个头文件：
 *
 *   * 所有跨文件调用的函数原型（与定义逐字同签名）；
 *   * 面板/工具内部用到的少量结构体与常量。
 *
 * 各 .cpp 只负责自己那一块实现，include 本头即可看到全部兄弟函数。壳层仍然
 * 只依赖 engine/abi/include，不包含任何 engine/ 内部头——这条约束没有松动。
 */
#ifndef COMPOSITOR_SHELL_INTERNAL_H
#define COMPOSITOR_SHELL_INTERNAL_H

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

#include "ui_core.h"
#include "shell_state.h"

namespace comp_shell {

/*
 * 色阶 = 4 通道 × 5 参（参数表顺序 `levels.ranges.N.field` 由引擎钉住，
 * tests/test_adjustment.cpp 有逐项断言）。面板按通道分页，一次只显示 5 行；
 * 其它 kind 不分页（整表显示）。这两个常量只描述「怎么分页」，不复制任何键名。
 */
constexpr int32_t kLevelsChannelCount = 4;
constexpr int32_t kLevelsFieldsPerChannel = 5;
constexpr int32_t kCurveChannels = 4;
/*
 * 同时写两种格式：
 *   1) 私有格式 `Compositor Layer Pixels`：预乘 RGBA8 + 文档落点。应用内往返
 *      逐字节相等（P0-4 的验收口径），CF_DIBV5 做不到——DIB 存的是**非预乘** BGRA；
 *   2) CF_DIBV5：给 Word / 浏览器等其它应用的直通格式。导出时反预乘，导入时
 *      再预乘，跨应用必然有一阶取整误差，这是格式本身的限制，不装作无损。
 * 读取时私有格式优先，缺失才退回 DIB——外部复制来的图仍可粘贴。
 */
constexpr uint32_t kClipboardMagic = 0x34504743u; /* "CGP4"（小端读作 43 47 50 34） */
struct ClipboardHeader {
  uint32_t magic;
  int32_t width;
  int32_t height;
  int32_t origin_x;
  int32_t origin_y;
  uint32_t premultiplied;
};
struct ClipboardImage {
  int32_t width = 0;
  int32_t height = 0;
  int32_t origin_x = 0; /* 文档坐标：左上角落点 */
  int32_t origin_y = 0;
  std::vector<uint8_t> pixels; /* 预乘 RGBA8，行优先，stride = width * 4 */
};
/* 命中半径 7 px：7×7 的方块，给 1 px 余量，缩放每帧重算不会「点边缘失手」。 */
constexpr double kSelectionHandleHitPx = 7.0;
/*
 * 随仓子集字体的固定文件名（字符集与 SHA256 记在 resources/fonts/README.md）。
 * 外壳**只**加载这一份：系统字体随机器漂移（同一份文档在两台机器上会排出不同
 * 结果），字体授权也不该由外壳去猜。加载失败就如实留在 5×7 轨道。
 */
constexpr const wchar_t* kShellFontFileName = L"NotoSansSC-Regular-Subset.ttf";
/*
 * 表是引擎级共享的只读资源（CRT4 = 10 字节头 + zlib 流，763 张 17³ 实测网格，
 * 随仓 1.78 MB），一次加载、所有文档复用。缺文件不算启动失败：滤镜菜单照常
 * 出现，打开面板时才如实报「色彩表未加载」，总比整个壳起不来强。
 */
constexpr const wchar_t* kCameraRawTableFileName = L"camera_raw_tables.bin";
/*
 * 手柄的屏幕尺寸不随缩放变化（画出来始终是 9×9 的方块、命中半径 7 px），
 * 因此命中/旋钮偏移都要先除以 zoom 换成文档像素再交给 ui_core 的纯函数。
 */
constexpr int kTransformHandlePx = 4;          /* 方块半边长（屏幕像素） */
constexpr double kTransformHandleHitPx = 7.0;  /* 命中半径（屏幕像素） */
constexpr double kTransformKnobOffsetPx = 24.0; /* 旋钮离上边中点（屏幕像素） */
/* ------- 6.67 工具图标：GDI 现画，零资源文件（S40） ------- */
/*
 * 工具栏以前画的是快捷键字母（V / B / M…），看不出工具是干什么的。但 .rc + .ico
 * 要改构建脚本，字体符号又依赖机器上装了什么，都会破坏单文件 exe 的部署前提。
 * 这里按 32×32 逻辑网格用 GDI 图元现画：坐标全是逻辑值，按按钮内框边长缩放，
 * 窗口变大变小图标跟着缩放，构建产物里不新增任何资源文件。
 * 笔宽 lround(2 * scale) 且下限 1，小缩放下线条也不会消失；虚线用 1 px PS_DOT。
 */
struct ToolIconMap {
  HDC dc;
  double ox;
  double oy;
  double scale;

  POINT at(double x, double y) const {
    return POINT{static_cast<LONG>(std::lround(ox + x * scale)),
                 static_cast<LONG>(std::lround(oy + y * scale))};
  }
  void line(double x1, double y1, double x2, double y2) const {
    const POINT a = at(x1, y1);
    const POINT b = at(x2, y2);
    MoveToEx(dc, a.x, a.y, nullptr);
    LineTo(dc, b.x, b.y);
  }
  void poly(const double* xy, int count, bool closed) const {
    POINT pts[12];
    if (count < 2 || count > 12) return;
    for (int i = 0; i < count; ++i) pts[i] = at(xy[i * 2], xy[i * 2 + 1]);
    if (closed) {
      Polygon(dc, pts, count);
    } else {
      Polyline(dc, pts, count);
    }
  }
  void circle(double cx, double cy, double r) const {
    const POINT a = at(cx - r, cy - r);
    const POINT b = at(cx + r, cy + r);
    Ellipse(dc, a.x, a.y, b.x, b.y);
  }
  void rect(double l, double t, double r, double b) const {
    const POINT a = at(l, t);
    const POINT c = at(r, b);
    Rectangle(dc, a.x, a.y, c.x, c.y);
  }
  void round_rect(double l, double t, double r, double b, double radius) const {
    const POINT a = at(l, t);
    const POINT c = at(r, b);
    const int rr = static_cast<int>(std::lround(radius * scale));
    RoundRect(dc, a.x, a.y, c.x, c.y, rr, rr);
  }
};
/*
 * --present-bench[=帧数]：DXGI 窗口直呈的端到端基准。
 *
 * 同一篇文档（1024²、背景 + 3 层，全部落在 GPU 子集内）、同一个窗口，比较两条
 * **真实上屏**路径：
 *   A. GPU：comp_render_document(COMP_SURFACE_HWND) -> comp_render_present
 *            （合成在设备上，结果拷进交换链后备缓冲，整幅不回读）
 *   B. CPU：comp_render_flatten -> 预乘 RGBA8 换 BGRA -> StretchDIBits
 *            （外壳既有上屏路径，render_canvas + present 的等价计量）
 * 另外单独计时 flatten，用来区分「合成」与「上屏」各占多少。
 *
 * -mwindows 没有控制台：结果写 present_bench.txt，同时在父进程是控制台时
 * AttachConsole 直接回显（--selftest 的写文件口径保持一致）。
 * 返回 0=成功；2=本构建/本机没有可用窗口直呈路径（如实跳过，不算失败）。
 */
constexpr int kBenchCanvas = 1024;
/*
 * ui_core.h 出于「不 include 引擎头」的约束，只能自带一份滤镜枚举。
 * 两边数值必须一一对应；这里用编译期断言把漂移挡在构建阶段。
 */
static_assert(static_cast<int>(comp_ui::FilterId::Vignette) == COMP_FILTER_VIGNETTE,
              "FilterId::Vignette must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::TonalContrast) == COMP_FILTER_TONAL_CONTRAST,
              "FilterId::TonalContrast must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::Bloom) == COMP_FILTER_BLOOM,
              "FilterId::Bloom must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::LensCorrection) == COMP_FILTER_LENS_CORRECTION,
              "FilterId::LensCorrection must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::GaussianBlur) == COMP_FILTER_GAUSSIAN_BLUR,
              "FilterId::GaussianBlur must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::AddNoise) == COMP_FILTER_ADD_NOISE,
              "FilterId::AddNoise must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::Grain) == COMP_FILTER_GRAIN,
              "FilterId::Grain must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::MotionBlur) == COMP_FILTER_MOTION_BLUR,
              "FilterId::MotionBlur must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::ProgressiveBlur) == COMP_FILTER_PROGRESSIVE_BLUR,
              "FilterId::ProgressiveBlur must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::Sharpen) == COMP_FILTER_SHARPEN,
              "FilterId::Sharpen must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::ReduceNoise) == COMP_FILTER_REDUCE_NOISE,
              "FilterId::ReduceNoise must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::HighPass) == COMP_FILTER_HIGH_PASS,
              "FilterId::HighPass must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::Minimum) == COMP_FILTER_MINIMUM,
              "FilterId::Minimum must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::Maximum) == COMP_FILTER_MAXIMUM,
              "FilterId::Maximum must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::Offset) == COMP_FILTER_OFFSET,
              "FilterId::Offset must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::Custom) == COMP_FILTER_CUSTOM,
              "FilterId::Custom must match comp_filter_kind");
/* 定义在窗口过程一节（下面才用到完整 Layout 口径），这里先声明：面板开合要
 * 按真实面板矩形夹滚动位置。 */
/* 视图 / 工具类命令的实现在窗口过程之后，这里先给出声明：
 * run_shell_command 是菜单与快捷键的唯一入口，不能被定义顺序卡住。 */

/* ===== 跨文件函数声明 ===== */
  void set_status(ShellState& shell, const wchar_t* text);
  void refresh_history_status(ShellState& shell, const wchar_t* prefix);
  void filter_preview_forget(ShellState& shell);
  uint8_t visible_of(ShellState& shell, comp_layer_id id);
  void set_visible_cache(ShellState& shell, comp_layer_id id, uint8_t value);
  void rebuild_layer_display(ShellState& shell);
  int32_t panel_row_count(const ShellState& shell);
  /*
   * 图层列表的滚动同步：夹取滚动量，并把**选中行滚进可视区**。
   *
   * 为什么放在「渲染前」而不是各个选中路径里：选中行会因点选、键盘上下、
   * 多选提升、撤销重做等很多条路径变化，每条都补一次 ensure 迟早漏一处；
   * 在绘制入口统一做一次，「选中永远可见」就是**结构上**保证的。
   *
   * 滚动量存在 shell.doc 上，主窗口与浮动窗口**共用**（同一个文档同一批层，
   * 两处各存一份反而会互相打架）。浮动窗口的可见行数与主窗口不同，
   * 打开浮窗时滚动量会被它自己的高度重新夹一次——这正是想要的。
   */
  void panel_sync_layer_scroll(ShellState& shell, const comp_ui::Rect& panel);
  std::vector<comp_ui::LayerStackEntry> layer_stack_entries(const ShellState& shell);
  const LayerRow* layer_row_at(const ShellState& shell, int32_t row);
  int32_t panel_row_of_id(const ShellState& shell, comp_layer_id id);
  bool layer_group_collapsed(const ShellState& shell, comp_layer_id id);
  void panel_toggle_group_collapsed(ShellState& shell, comp_layer_id id);
  /* P1-2 收尾：图层面板过滤（纯视图态，见 shell_state.h）。 */
  bool layer_filter_active(const ShellState& shell);
  void panel_refresh_after_filter(ShellState& shell);
  void panel_set_filter_kind(ShellState& shell, int kind);
  void panel_set_filter_color(ShellState& shell, int color);
  void panel_begin_filter_name(ShellState& shell);
  void panel_clear_filter(ShellState& shell);
  /* P1-2 收尾：图层复合（文档态，见 engine LayerComp；当前选中是视图态）。 */
  comp_layer_comp_id panel_current_layer_comp(ShellState& shell);
  void panel_layer_comp_status(ShellState& shell);
  void panel_layer_comp_select(ShellState& shell, int delta);
  void panel_layer_comp_capture_begin(ShellState& shell);
  void panel_layer_comp_rename_begin(ShellState& shell);
  void panel_layer_comp_apply(ShellState& shell);
  void panel_layer_comp_update(ShellState& shell);
  void panel_layer_comp_delete(ShellState& shell);
  /* P1-2 收尾 ⑦：智能对象 / 智能滤镜（真源在引擎，壳只提供三个动作 + 状态查询）。 */
  void panel_smart_status(ShellState& shell);
  void panel_smart_convert(ShellState& shell);
  void panel_smart_rasterize(ShellState& shell);
  void panel_smart_clear_filters(ShellState& shell);
  /*
   * P1-2 收尾 ⑨：智能滤镜栈面板。列表只读（count / _at），动作各走一条 ABI 写：
   * 空格 = _set 同参翻转 enabled，Del = _remove，回车 = 滤镜面板进「编辑该条」模式。
   * smart_panel_target 是唯一的「这一层能不能编辑滤镜栈」判定，绘制、动作、菜单同源。
   */
  comp_layer_id smart_panel_target(const ShellState& shell);
  int smart_panel_count(const ShellState& shell);
  bool smart_panel_visible(const ShellState& shell);
  void smart_panel_dismiss(ShellState& shell);
  void smart_panel_reset(ShellState& shell);
  void refresh_smart_panel(ShellState& shell);
  void open_smart_panel(ShellState& shell);
  void close_smart_panel(ShellState& shell);
  void smart_panel_status(ShellState& shell);
  void smart_panel_focus_row(ShellState& shell, int32_t row);
  void smart_panel_move(ShellState& shell, int32_t delta);
  void smart_panel_toggle_entry(ShellState& shell);
  void smart_panel_edit_entry(ShellState& shell);
  void smart_panel_remove_entry(ShellState& shell);
  /*
   * P1-2 收尾 ⑩：栈序与展示名。三条写路径各走一条引擎导出（_move / _set_name），
   * 展示名只影响面板文字与 `.comp` 的 `filters[].name`，不参与求值。
   * smart_entry_name 是「这一行显示什么」的唯一口径：自定义名优先、空名回退到
   * 滤镜类型名——绘制、状态栏、动作回报三处同源。
   */
  std::wstring smart_entry_name(comp_document document, comp_layer_id layer, size_t index,
                                const comp_filter_desc& desc);
  void smart_panel_move_entry(ShellState& shell, int32_t delta);
  void smart_panel_drop(ShellState& shell, int32_t from, int32_t to);
  void panel_begin_smart_filter_name(ShellState& shell);
  /* P1-2 收尾 ⑧：填充图层（纯色 / 渐变 / 图案，参数表由引擎驱动）。 */
  bool fill_param_query(const ShellState& shell, int32_t row, comp_fill_param* out);
  void refresh_fill(ShellState& shell);
  void open_fill_panel(ShellState& shell, uint32_t kind);
  void close_fill_panel(ShellState& shell);
  void fill_panel_focus_row(ShellState& shell, int32_t row);
  void fill_panel_move_param(ShellState& shell, int32_t delta);
  void fill_panel_step_value(ShellState& shell, int32_t direction);
  void fill_panel_begin_input(ShellState& shell, wchar_t character);
  void fill_panel_backspace(ShellState& shell);
  void fill_panel_commit_input(ShellState& shell);
  void fill_panel_reset_values(ShellState& shell);
  bool build_fill_settings_json(const ShellState& shell, std::string* out_json);
  void panel_apply_fill(ShellState& shell);
  void panel_add_fill(ShellState& shell, uint32_t kind);
  void panel_edit_fill(ShellState& shell);
  void panel_rasterize_fill(ShellState& shell);
  void panel_fill_status(ShellState& shell);
  void fill_panel_begin_drag(ShellState& shell, const comp_ui::Rect& panel, int32_t row,
                             double x);
  void fill_panel_drag_value(ShellState& shell, double x);
  void fill_panel_drag_finish(ShellState& shell, bool commit);
  void refresh_layers(ShellState& shell);
  const LayerRow* selected_row(const ShellState& shell);
  void panel_move_selection(ShellState& shell, int32_t delta);
  void panel_toggle_visible(ShellState& shell);
  void panel_adjust_opacity(ShellState& shell, float delta);
  void panel_cycle_blend(ShellState& shell, int direction);
  void panel_set_blend(ShellState& shell, int mode);
  void panel_mask_from_selection(ShellState& shell);
  void panel_mask_remove(ShellState& shell);
  void panel_mask_to_selection(ShellState& shell);
  void panel_toggle_clipping(ShellState& shell);
  void panel_toggle_isolated(ShellState& shell);
  void panel_reorder_layer(ShellState& shell, int direction);
  /* L20：图层对齐（edge 见 ui_core::LayerAlignEdge）与分布（mode 0=水平 1=垂直）。 */
  void panel_align_layers(ShellState& shell, int32_t edge);
  void panel_distribute_layers(ShellState& shell, int32_t mode);
  void panel_set_sampling(ShellState& shell, int mode);
  void refresh_histogram(ShellState& shell);
  void panel_toggle_histogram(ShellState& shell);
  const wchar_t* effect_kind_label(int kind);
  void reset_effect_values(ShellState& shell, bool use_existing);
  void refresh_effects(ShellState& shell);
  void panel_toggle_flip(ShellState& shell, bool horizontal);
  /* P1-2：图层锁 / 颜色标签。锁的写入口，也是菜单单选组的唯一事实来源。 */
  void panel_set_lock(ShellState& shell, uint32_t flags);
  void panel_set_color_label(ShellState& shell, uint32_t label);
  uint32_t panel_lock_menu_flag(uint32_t flags);
  const wchar_t* color_label_name(uint32_t label);
  bool layer_has_effect(ShellState& shell, comp_layer_id id, uint32_t kind);
  void panel_toggle_effect_multi(ShellState& shell, int clamped, const wchar_t* label);
  void panel_toggle_effect(ShellState& shell, int kind);
  void panel_clear_effects(ShellState& shell);
  bool effect_present(const ShellState& shell, int kind);
  void open_effect_panel(ShellState& shell, int kind);
  void close_effect_panel(ShellState& shell);
  void panel_effect_cycle_kind(ShellState& shell, int32_t delta);
  void panel_effect_focus_row(ShellState& shell, int32_t row);
  void panel_effect_move_param(ShellState& shell, int32_t delta);
  void panel_effect_step_value(ShellState& shell, int32_t direction);
  void panel_effect_begin_input(ShellState& shell, wchar_t character);
  void panel_effect_backspace(ShellState& shell);
  void panel_effect_commit_input(ShellState& shell);
  void panel_effect_reset_values(ShellState& shell);
  bool build_effect_settings_json(const ShellState& shell, std::string* out_json);
  void panel_apply_effect(ShellState& shell);
  void histogram_view_status(ShellState& shell);
  void panel_histogram_reset(ShellState& shell);
  void panel_histogram_zoom(ShellState& shell, double factor, double focus_bin);
  void panel_histogram_commit_drag(ShellState& shell, const comp_ui::Rect& plot);
  void panel_drop_layer(ShellState& shell, int32_t from_shown, int32_t to_shown, bool drop_into_group_item);
  void menu_set_checked_recursive(HMENU menu, UINT id, bool checked);
  void menu_radio_check_recursive(HMENU menu, UINT first, UINT last, UINT selected);
  void menu_set_enabled_recursive(HMENU menu, UINT id, bool enabled);
  void sync_menu_state(HWND window, ShellState& shell);
  void collect_group_members(const ShellState& shell, comp_layer_id root, std::vector<GroupMemberMove>* out); void build_move_targets(const ShellState& shell, std::vector<GroupMemberMove>* out); bool members_off_canvas(const std::vector<GroupMemberMove>& members, int32_t dx, int32_t dy, int32_t view_width, int32_t view_height); bool nudge_flush(ShellState& shell);
  comp_txn nudge_begin(ShellState& shell, comp_layer_id anchor, const char* label);
  void panel_move_layer(ShellState& shell, int dx, int dy);
  void panel_rotate_layer(ShellState& shell, bool forward);
  void panel_scale_layer(ShellState& shell, int direction);
  /*
   * P1-3 数值变换 / 再次变换。三条都在 shell_layers.cpp，共用一套几何口径：
   *   - layer_effective_transform：读图层「有效」矩形（未显式变换 = 原点 + 像素
   *     尺寸）；组 / 空图层（0×0）返回 false，调用方据此拒绝，不画空面板；
   *   - panel_apply_layer_transform：中心锚定写入新宽高 + 角度，并把
   *     「比例 + 角度增量」记成下一次「再次变换」要复读的一步；
   *   - panel_transform_again：把记下的那一步再套一遍。
   */
  bool layer_effective_transform(ShellState& shell, comp_layer_id id, comp_rect* out_rect,
                                 double* out_rotation);
  int panel_apply_layer_transform(ShellState& shell, comp_layer_id id, int width, int height,
                                  double angle, bool keep_rotation);
  void panel_transform_again(ShellState& shell);
  /* 记下「刚才这一步」的比例与角度增量（供再次变换复读）。无效参数直接忽略。 */
  void record_transform_step(ShellState& shell, double scale_x, double scale_y,
                             double angle_delta);
  void panel_add_layer(ShellState& shell);
  bool panel_selection_contains(const ShellState& shell, comp_layer_id id);
  std::vector<comp_layer_id> panel_selection_ids(const ShellState& shell, bool drop_covered);
  void panel_select_layer_id(ShellState& shell, comp_layer_id id);
  void panel_add_group(ShellState& shell);
  void panel_delete_layer(ShellState& shell);
  void panel_duplicate_layer(ShellState& shell);
  void refresh_layer_input_status(ShellState& shell);
  void panel_begin_rename(ShellState& shell);
  void panel_begin_opacity_set(ShellState& shell);
  void panel_cancel_layer_input(ShellState& shell);
  void panel_layer_input_char(ShellState& shell, wchar_t ch);
  void panel_layer_input_backspace(ShellState& shell);
  void panel_commit_layer_input(ShellState& shell);
  comp_layer_id selected_layer_id(const ShellState& shell);
  /* P0-6 ①：可编辑文字图层的壳侧入口（文字工具提交 / 图层 > 栅格化文字）。 */
  bool layer_is_text(ShellState& shell, comp_layer_id id);
  void panel_rasterize_text(ShellState& shell);
  void panel_merge_down(ShellState& shell);
  void panel_flatten(ShellState& shell);
  void panel_merge_visible(ShellState& shell);
  void panel_reverse_layers(ShellState& shell);
  /* P1-2 ③：链接/解除链接选中图层（组图层跳过；少于两层拒绝）。 */
  void panel_toggle_link(ShellState& shell);
  /* P1-2 ④：图层样式的拷贝/粘贴（整份替换；多选粘贴 = 一步撤销）。 */
  void panel_copy_layer_style(ShellState& shell);
  void panel_paste_layer_style(ShellState& shell);
  std::string number_text(double value);
  std::wstring widen_ascii(const char* text);
  std::wstring widen_utf8(const std::string& text);
  std::string narrow_utf8(const std::wstring& text);
  const char* adjustment_kind_name(uint32_t kind);
  bool adjust_paged_kind(uint32_t kind);
  int32_t adjust_channel_count(uint32_t kind);
  void reset_adjustment_values(ShellState& shell);
  void refresh_adjustments(ShellState& shell);
  void panel_cycle_kind(ShellState& shell, int32_t delta);
  void panel_select_adjustment(ShellState& shell, int32_t delta);
  void panel_move_param(ShellState& shell, int32_t delta);
  int32_t adjust_shown_count(const ShellState& shell);
  int32_t adjust_param_base(const ShellState& shell);
  double adjust_param_step(const comp_adjustment_param& param);
  void panel_step_value(ShellState& shell, int32_t direction);
  void panel_begin_input(ShellState& shell, wchar_t character);
  void panel_backspace(ShellState& shell);
  void panel_commit_input(ShellState& shell);
  bool build_settings_json(const ShellState& shell, std::string* out_json);
  void panel_apply_adjustment(ShellState& shell, bool add);
  void panel_remove_adjustment(ShellState& shell);
  /* ROI #8：Image > Adjustments 的破坏性落墨（ABI comp_layer_apply_adjustment）。 */
  void panel_begin_destructive(ShellState& shell, uint32_t kind);
  void panel_apply_destructive(ShellState& shell);
  void panel_exit_destructive(ShellState& shell);
  std::wstring param_status_text(const comp_ui::FilterPanel& panel);
  void open_filter_panel(ShellState& shell, comp_ui::FilterId id);
  void close_filter_panel(ShellState& shell);
  void filter_reset_values(ShellState& shell);
  void filter_focus_row(ShellState& shell, int delta);
  /* 网格面板（Custom 的 5×5 核）方向键：dx 左右挪一格、dy 上下挪一行。 */
  void filter_focus_grid(ShellState& shell, int dx, int dy);
  void filter_step_value(ShellState& shell, int direction);
  void filter_begin_input(ShellState& shell, wchar_t character);
  void filter_backspace(ShellState& shell);
  void filter_commit_input(ShellState& shell, bool with_preview);
  void build_filter_desc(const ShellState& shell, comp_filter_desc* out);
  bool filter_preview_render(ShellState& shell, const wchar_t* lead);
  void apply_filter(ShellState& shell);
  /* P1-2 收尾 ⑨：智能滤镜单条编辑——滤镜面板的「编辑该条」模式。 */
  void filter_smart_edit_clear(ShellState& shell);
  void filter_load_values(comp_ui::FilterPanel& panel, const comp_filter_desc& desc);
  void filter_load_values_from_desc(ShellState& shell, const comp_filter_desc& desc);
  void build_filter_desc_patched(const ShellState& shell, comp_filter_desc* out);
  bool open_smart_filter_editor(ShellState& shell, size_t index);
  void smart_panel_reopen_after_edit(ShellState& shell, int focus_index);
  /* 一行摘要（滤镜名 / 参数 / 开关），绘制与状态栏共用同一份口径。 */
  std::wstring smart_entry_summary(const comp_filter_desc& desc);
  std::wstring filter_kind_name(comp_filter_kind kind);
  void finish_canvas_command(ShellState& shell);
  void image_rotate_canvas(ShellState& shell, int32_t quarter_turns, const wchar_t* label);
  void image_invert_layer(ShellState& shell);
  void trim_begin(ShellState& shell);
  void trim_close(ShellState& shell);
  void build_trim_options(const ShellState& shell, comp_trim_options* out);
  void trim_apply(ShellState& shell);
  void trim_focus_row(ShellState& shell, int delta);
  void trim_step_value(ShellState& shell, int direction);
  void trim_begin_input(ShellState& shell, wchar_t character);
  void trim_commit_input(ShellState& shell);
  void refresh_selection(ShellState& shell);
  void finish_selection_command(ShellState& shell, const wchar_t* prefix);
  void select_all(ShellState& shell);
  void deselect_all(ShellState& shell);
  void invert_selection(ShellState& shell);
  UINT clipboard_layer_format();
  bool clipboard_size_ok(int64_t width, int64_t height);
  uint8_t clipboard_straight(uint8_t value, uint8_t alpha);
  uint8_t clipboard_premultiply(uint8_t value, uint8_t alpha);
  bool clipboard_put_block(UINT format, const void* data, size_t bytes);
  bool clipboard_write_image(HWND window, const ClipboardImage& image, std::wstring* error);
  bool clipboard_read_custom(HANDLE handle, ClipboardImage* out);
  bool clipboard_read_dib(HANDLE handle, ClipboardImage* out);
  bool clipboard_read_image(HWND window, ClipboardImage* out, std::wstring* error);
  bool layer_is_axis_aligned_8bit(comp_document document, comp_layer_id id, comp_rect* out_rect);
  bool read_layer_pixels(comp_document document, comp_layer_id id, ClipboardImage* out);
  void crop_clipboard_to_selection(ShellState& shell, ClipboardImage* image);
  bool build_layer_clipboard(ShellState& shell, comp_layer_id id, ClipboardImage* out, bool* out_raw, std::wstring* error);
  void edit_copy(HWND window, ShellState& shell, bool merged);
  bool clear_selected_pixels(ShellState& shell, const wchar_t* done_label);
  void edit_cut(HWND window, ShellState& shell);
  void edit_paste(HWND window, ShellState& shell, bool into_selection);
  void finish_new_selection(ShellState& shell, const char* label, const wchar_t* ok_text, const wchar_t* noop_text, const wchar_t* fail_text, const std::function<comp_result(int32_t*)>& apply, bool* applied = nullptr);
  void refresh_feather_input_status(ShellState& shell);
  void begin_feather_input(ShellState& shell);
  void commit_feather_input(ShellState& shell);
  const wchar_t* modify_op_name(comp_selection_modify_op op);
  int modify_op_min(comp_selection_modify_op op);
  int modify_op_max(comp_selection_modify_op op);
  int modify_op_clamp(comp_selection_modify_op op, int value);
  void refresh_modify_input_status(ShellState& shell);
  void begin_modify_input(ShellState& shell, comp_selection_modify_op op);
  void commit_modify_input(ShellState& shell);
  void grow_similar_selection(ShellState& shell, bool similar);
  void load_layer_pixels_selection(ShellState& shell);
  void color_range_status(ShellState& shell, const wchar_t* prefix);
  void color_range_restart_txn(ShellState& shell);
  void color_range_begin(ShellState& shell);
  void color_range_cancel(ShellState& shell);
  void color_range_commit(ShellState& shell);
  void color_range_refresh(ShellState& shell);
  void color_range_sample(ShellState& shell, int x, int y, const Layout& layout);
  void color_range_remove_swatch(ShellState& shell, int slot);
  void color_range_step_fuzziness(ShellState& shell, int direction);
  void color_range_toggle_invert(ShellState& shell);
  void color_range_clear_colors(ShellState& shell);
  void color_range_begin_input(ShellState& shell, wchar_t character);
  void color_range_commit_input(ShellState& shell);
  bool lasso_available(const ShellState& shell);
  void lasso_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void lasso_extend(ShellState& shell, int x, int y, const Layout& layout);
  void lasso_cancel(ShellState& shell);
  void lasso_end(HWND window, ShellState& shell);
  bool selection_handles_available(const ShellState& shell);
  void selection_handle_centers(const ShellState& shell, const Layout& layout, const comp_ui::Rect& doc_rect, comp_ui::Point* out);
  bool selection_handle_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void selection_handle_extend(ShellState& shell, int x, int y, const Layout& layout);
  void selection_handle_cancel(ShellState& shell);
  void selection_handle_end(HWND window, ShellState& shell);
  HCURSOR selection_handle_cursor(int index);
  bool wand_available(const ShellState& shell);
  void wand_click(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  bool pen_available(const ShellState& shell);
  double pen_close_radius(const ShellState& shell);
  void pen_refresh_status(ShellState& shell);
  void pen_cancel_drag(ShellState& shell);
  void pen_cancel(ShellState& shell);
  int path_build_cubics(const std::vector<comp_ui::PenAnchor>& anchors, std::vector<comp_selection_cubic>* cubics);
  void commit_pen_path(ShellState& shell);
  bool path_store_add(ShellState& shell, const std::vector<comp_ui::PenAnchor>& anchors);
  void path_store_clear(ShellState& shell);
  bool path_panel_visible(const ShellState& shell);
  void path_status(ShellState& shell);
  void path_select(ShellState& shell, int delta);
  void path_apply_selected(ShellState& shell);
  void path_delete_selected(ShellState& shell);
  void path_panel_toggle(ShellState& shell);
  bool history_panel_visible(const ShellState& shell);
  int history_state_count(const ShellState& shell);
  int history_current(const ShellState& shell);
  std::wstring history_state_label(const ShellState& shell, int index);
  void history_status(ShellState& shell);
  void history_jump_to(ShellState& shell, int target);
  void history_step(ShellState& shell, int delta);
  int history_brush_resolved_state(const ShellState& shell);
  void history_panel_toggle(ShellState& shell);
  /* ROI #5：存储的选区通道（PS Alpha 通道）。 */
  bool channel_panel_visible(const ShellState& shell);
  int channel_count(const ShellState& shell);
  std::wstring channel_name(const ShellState& shell, int index);
  void channel_status(ShellState& shell);
  void channel_select(ShellState& shell, int delta);
  void channel_load_selected(ShellState& shell, comp_selection_combine combine);
  void channel_delete_selected(ShellState& shell);
  void channel_panel_toggle(ShellState& shell);
  void channel_panel_close(ShellState& shell);
  void channel_store_clear(ShellState& shell);
  void channel_save_begin(ShellState& shell);
  void channel_name_char(ShellState& shell, wchar_t ch);
  void channel_name_backspace(ShellState& shell);
  void channel_name_commit(ShellState& shell);
  void channel_name_cancel(ShellState& shell);
  void pen_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void pen_drag(ShellState& shell, int x, int y, const Layout& layout);
  void pen_up(HWND window, ShellState& shell);
  bool gradient_available(const ShellState& shell);
  void gradient_cancel(ShellState& shell);
  void gradient_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void gradient_extend(ShellState& shell, int x, int y, const Layout& layout);
  void gradient_end(HWND window, ShellState& shell);
  bool progressive_axis_available(const ShellState& shell);
  void progressive_axis_cancel(ShellState& shell);
  void progressive_axis_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void progressive_axis_extend(ShellState& shell, int x, int y, const Layout& layout);
  void progressive_axis_end(HWND window, ShellState& shell);
  bool shape_available(const ShellState& shell);
  void shape_cancel(ShellState& shell);
  void shape_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void shape_extend(ShellState& shell, int x, int y, const Layout& layout);
  void shape_end(HWND window, ShellState& shell);
  bool type_available(const ShellState& shell);
  bool type_font_track(const ShellState& shell);
  comp_ui::TypeBlockMetrics type_metrics(const ShellState& shell);
  /* P0-6③：声明了段落框就用框尺寸，未声明的维度才按内容度量。 */
  comp_ui::TypeBlockMetrics type_preview_metrics(const ShellState& shell);
  void type_cancel(ShellState& shell);
  bool type_char_input(ShellState& shell, wchar_t unit);
  void type_refresh_status(ShellState& shell);
  void type_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void type_commit(ShellState& shell);
  void type_step_param(ShellState& shell, int direction, bool spacing);
  std::wstring exe_directory();
  comp_font load_shipped_font(std::wstring* out_label, comp_font_metrics* out_metrics,
                              std::string* out_path = nullptr);
  bool load_shell_font(ShellState& shell);
  bool read_whole_file(const std::wstring& path, std::vector<uint8_t>* out);
  bool load_shell_camera_raw_tables(ShellState& shell);
  bool type_panel_visible(const ShellState& shell);
  double type_panel_value(const ShellState& shell, int row);
  std::wstring type_panel_value_text(const ShellState& shell, int row);
  void type_panel_set(ShellState& shell, int row, double value);
  void type_panel_status(ShellState& shell, const wchar_t* prefix);
  void type_panel_focus(ShellState& shell, int delta);
  void type_panel_toggle_font(ShellState& shell);
  void type_panel_step(ShellState& shell, int direction);
  void type_panel_begin_input(ShellState& shell, wchar_t character);
  void type_panel_backspace(ShellState& shell);
  void type_panel_commit_input(ShellState& shell);

  /* -------------------------------------------------- 字体面板（P0-6②） */
  /*
   * 目录扫描只做「读文件 -> 读度量的族名/样式名 -> 释放」，不跨调用持有句柄：
   * comp_font 没有全局缓存，句柄留着会让每个字体几十 MB 常驻。
   * 扫描按 WM_TIMER 分片（kFontScanPerTick 个文件/片），扫描中面板照常可用。
   */
  bool font_panel_visible(const ShellState& shell);
  void font_panel_toggle(HWND window, ShellState& shell);
  void font_panel_close(ShellState& shell);
  void font_scan_start(HWND window, ShellState& shell);
  void font_scan_step(HWND window, ShellState& shell);
  void font_select_current_track(ShellState& shell, bool force);
  /* 按当前 filter 重建 font_filtered，并把选中行夹回合法范围。 */
  void font_refilter(ShellState& shell);
  int font_row_count(const ShellState& shell);
  const ShellFontEntry* font_entry_at_row(const ShellState& shell, int row);
  void font_select(ShellState& shell, int delta);
  void font_select_edge(ShellState& shell, bool end);
  void font_apply_row(ShellState& shell, int row);
  void font_filter_char(ShellState& shell, wchar_t unit);
  void font_filter_backspace(ShellState& shell);
  void font_filter_clear(ShellState& shell);
  /* keep_prefix = true 时把「第 n/m 行 + 名字」追加到当前状态栏，不覆盖上一条消息。 */
  void font_status(ShellState& shell, bool keep_prefix = false);
  bool marquee_available(const ShellState& shell);
  void marquee_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void marquee_extend(ShellState& shell, int x, int y, const Layout& layout);
  void marquee_cancel(ShellState& shell);
  void marquee_end(HWND window, ShellState& shell);
  bool crop_available(const ShellState& shell);
  void crop_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void crop_extend(ShellState& shell, int x, int y, const Layout& layout);
  void crop_cancel(ShellState& shell);
  bool crop_commit(ShellState& shell);
  bool stroke_in_progress(const ShellState& shell);
  bool stroke_begin(ShellState& shell, int x, int y, const Layout& layout, comp_ui::StrokeKind kind);
  void stroke_extend(ShellState& shell, int x, int y, const Layout& layout);
  void stroke_cancel(ShellState& shell);
  bool retouch_stroke_available(const ShellState& shell);
  void retouch_stroke_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void retouch_stroke_end(HWND window, ShellState& shell);
  bool tone_stroke_available(const ShellState& shell);
  void tone_stroke_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void tone_stroke_end(HWND window, ShellState& shell);
  bool history_brush_stroke_available(const ShellState& shell);
  void history_brush_stroke_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void history_brush_stroke_end(HWND window, ShellState& shell);
  bool paint_stroke_available(const ShellState& shell);
  void paint_stroke_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void paint_stroke_end(HWND window, ShellState& shell);
  bool clone_stroke_available(const ShellState& shell);
  void clone_reset_source(ShellState& shell);
  void clone_toggle_sample_all(ShellState& shell);
  void clone_pick_source(ShellState& shell, int x, int y, const Layout& layout);
  void clone_stroke_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void clone_stroke_end(HWND window, ShellState& shell);
  bool heal_stroke_available(const ShellState& shell);
  bool heal_layer_point(const comp_rect& transform, float rotation, bool flip_x, bool flip_y, int width, int height, double doc_x, double doc_y, double* out_x, double* out_y);
  void heal_stamp_round(std::vector<uint8_t>* coverage, int width, int height, double cx, double cy, double radius, double hardness);
  void heal_rasterize_stroke(std::vector<uint8_t>* coverage, int width, int height, const std::vector<double>& points, double radius, double hardness);
  void heal_stroke_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void heal_stroke_end(HWND window, ShellState& shell);
  bool eyedropper_available(const ShellState& shell);
  void eyedropper_sample(ShellState& shell, int x, int y, const Layout& layout);
  void eyedropper_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void eyedropper_cancel(ShellState& shell);
  bool move_available(const ShellState& shell);
  void collect_group_members(const ShellState& shell, comp_layer_id root, std::vector<GroupMemberMove>* out);
  void build_move_targets(const ShellState& shell, std::vector<GroupMemberMove>* out);
  bool members_off_canvas(const std::vector<GroupMemberMove>& members, int32_t dx, int32_t dy, int32_t view_width, int32_t view_height);
  bool group_members_off_canvas(const ShellState& shell, int32_t dx, int32_t dy);
  comp_ui::Rect to_ui_rect(const comp_rect& rect);
  comp_rect to_abi_rect(const comp_ui::Rect& rect);
  bool same_rect(const comp_rect& a, const comp_rect& b);
  bool transform_frame_for(const ShellState& shell, comp_rect* out_rect, float* out_rotation);
  void layer_handle_clear(ShellState& shell);
  bool layer_handle_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void layer_handle_extend(ShellState& shell, int x, int y, const Layout& layout);
  void layer_handle_end(HWND window, ShellState& shell);
  void layer_move_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout, const wchar_t* hint);
  void layer_move_extend(ShellState& shell, int x, int y, const Layout& layout);
  void layer_move_end(HWND window, ShellState& shell);
  void move_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void move_extend(ShellState& shell, int x, int y, const Layout& layout);
  void move_cancel(ShellState& shell);
  void move_end(HWND window, ShellState& shell);
  /*
   * ROI #3 标尺 / 参考线 / 网格 / 吸附（shell_guides.cpp）。三个 draw_* 画在
   * 画布覆盖层里（shell_paint.cpp 调用），其余是交互与开关。
   */
  void view_toggle_rulers(ShellState& shell);
  void view_toggle_guides(ShellState& shell);
  void view_toggle_grid(ShellState& shell);
  void view_toggle_snap(ShellState& shell);
  void view_clear_guides(ShellState& shell);
  bool guide_drag_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout);
  void guide_drag_extend(ShellState& shell, int x, int y, const Layout& layout);
  void guide_drag_end(HWND window, ShellState& shell, int x, int y);
  void guide_drag_cancel(ShellState& shell);
  /* 换文档 / 销毁文档前调用：只清壳侧拖动状态，不碰引擎（旧句柄可能已失效）。 */
  void guide_drag_forget(ShellState& shell);
  bool guide_hover_update(ShellState& shell, int x, int y, const Layout& layout);
  void draw_grid_lines(HDC dc, const ShellState& shell, const Layout& layout);
  void draw_guides(HDC dc, const ShellState& shell, const Layout& layout);
  void draw_rulers(HDC dc, const ShellState& shell, const Layout& layout);
  void forget_color_range(ShellState& shell);
  bool fill_document_background(ShellState& shell, int background);
  bool create_document_with(ShellState& shell, int width, int height, int dpi, int background);
  /* P0-1：位深 0=8 位 / 1=16 位；颜色模式 0=RGB / 1=灰度（取值即 ABI 枚举）。 */
  bool create_document_opts(ShellState& shell, int width, int height, int dpi, int background,
                            int pixel_format, int color_mode);

  /* ------------------------------------------------------------ P0-3 多文档 */
  /* 换文档前收口跨帧交互（事务句柄不能在后台标签里挂着）。 */
  void doc_switch_finish(ShellState& shell);
  /* 懒初始化：保证 tabs 至少一项、active_tab 合法、front 标记唯一。 */
  void tabs_ensure(ShellState& shell);
  int tab_count(ShellState& shell);
  std::wstring tab_title(const ShellState& shell, int index);
  void tab_activate(ShellState& shell, int index);
  void tab_park_active(ShellState& shell);
  void tab_commit_new(ShellState& shell);
  void tab_restore_parked(ShellState& shell);
  /* 关掉当前标签；true = 还有别的标签，false = 这是最后一个（调用方负责关窗）。 */
  bool tab_close_active(ShellState& shell);
  void tab_step(ShellState& shell, int delta);
  /* 退出流程：返回 true = 已弹未保存提示，等用户回答。 */
  bool close_flow_step(ShellState& shell);

  /* 最近打开：%APPDATA%\Compositor\recent-files.txt，纯文本一行一个路径。 */
  void recent_load(ShellState& shell);
  void recent_save(const ShellState& shell);
  void recent_push(ShellState& shell, const char* path_utf8);
  bool create_document(ShellState& shell);
  bool load_document(ShellState& shell, const char* path_utf8);
  bool save_document(ShellState& shell, const char* path_utf8);
  void undo_document(ShellState& shell);
  void redo_document(ShellState& shell);
  void list_commands(ShellState& shell);
  void open_document_dialog(HWND window, ShellState& shell);
  void save_document_dialog(HWND window, ShellState& shell);
  void save_document_quick(HWND window, ShellState& shell);
  int doc_dialog_row_value(const ShellState& shell, int row);
  void doc_dialog_set_row_value(ShellState& shell, int row, int value);
  std::wstring doc_dialog_value_text(const ShellState& shell, int row);
  void doc_dialog_close(ShellState& shell, const wchar_t* status);
  void doc_dialog_begin(ShellState& shell, comp_ui::DocDialogKind kind);
  void doc_dialog_focus_row(ShellState& shell, int delta);
  void doc_dialog_step(ShellState& shell, int direction);
  void doc_dialog_begin_input(ShellState& shell, wchar_t character);
  void doc_dialog_commit_input(ShellState& shell);
  void sync_document_buffers(ShellState& shell);
  void doc_dialog_apply(HWND window, ShellState& shell);
  void open_path_as_document(ShellState& shell, const char* path_utf8);
  void handle_drop_files(HWND window, ShellState& shell, HDROP drop);
  void open_image_dialog(HWND window, ShellState& shell);
  void import_image_dialog(HWND window, ShellState& shell);
  void export_image_dialog(HWND window, ShellState& shell);
  void run_shell_command(HWND window, ShellState& shell, UINT id);
  bool render_canvas(ShellState& shell);
  const wchar_t* backend_label(comp_backend backend);
  void draw_tool_icon(HDC dc, comp_ui::ToolId tool, const comp_ui::Rect& button, COLORREF color);
  void draw_toolbar(HDC dc, const ShellState& shell, const comp_ui::Rect& bar);
  std::wstring option_value_text(const ShellState& shell, const comp_ui::ToolOptionSpec& spec);
  bool apply_tool_option(ShellState& shell, comp_ui::ToolOptionId id, int delta);
  /* P0-10 数字直输：数值区的当前整数值 / 直接写值 / 键入态三件套。 */
  int option_value_number(const ShellState& shell, comp_ui::ToolOptionId id);
  bool set_tool_option_value(ShellState& shell, comp_ui::ToolOptionId id, int value);
  void option_input_begin(ShellState& shell, int index);
  void option_input_char(ShellState& shell, wchar_t character);
  void option_input_backspace(ShellState& shell);
  void option_input_commit(ShellState& shell);
  void option_input_cancel(ShellState& shell);
  void draw_tool_options(HDC dc, const ShellState& shell, const comp_ui::Rect& bar);
  /* P0-3：文档标签栏（只在多文档时出现，布局已保证 tabbar.h > 0）。 */
  void draw_tab_bar(HDC dc, const ShellState& shell, const Layout& layout);
  /* P0-10：浮动面板（独立顶层窗口）的浮/停回切换。 */
  void toggle_float_panel(ShellState& shell, comp_dock::PanelId id);
  /* P0-10：面板分组（tab strip）——合并/拆开/切标签。 */
  void panel_group_merge(ShellState& shell);
  void panel_group_split(ShellState& shell);
  void panel_group_select(ShellState& shell, comp_dock::DockArea side, int index);
  bool panel_group_active(const ShellState& shell);
  comp_dock::DockArea panel_group_side(const ShellState& shell);
  /* P0-10：右栏内容（按当前子面板选画）；浮动面板窗口与主窗口共用。 */
  void draw_adjust_content(HDC dc, const ShellState& shell, const comp_ui::Rect& panel);
  void draw_layer_content(HDC dc, const ShellState& shell, const comp_ui::Rect& panel);
  /*
   * 画一条可见滚动条（轨道 + 滑块）。通用：任何列表都能用。
   *
   * 此前所有可滚列表都**只能靠滚轮**——用户看不到「下面还有东西」，也就
   * 不知道能滚。图层列表先补上，其余（历史 / 通道 / 字体）复用这条。
   */
  void draw_scroll_bar(HDC dc, const comp_ui::ScrollBarGeometry& bar);
  /*
   * 调整栏**当前子面板**的可见滚动条几何（没有可滚内容时 visible=false）。
   *
   * 绘制与命中共用这一条：两处各算一次必然有一天不一致（滑块画在一处、
   * 命中算在另一处 = 点不动的滚动条）。
   */
  comp_ui::ScrollBarGeometry adjust_panel_scroll_bar(const ShellState& shell,
                                                      const comp_ui::Rect& panel);
  /* 调整栏当前子面板对应哪条滚动条（没有可滚内容时 None）。 */
  ScrollDragTarget adjust_panel_scroll_target(const ShellState& shell);
  /* 某条滚动条当前的滚动量 / 最大滚动量 / 可视行数（拖动与翻页都要用）。 */
  int adjust_panel_scroll_value(const ShellState& shell, ScrollDragTarget target);
  int adjust_panel_max_scroll(const ShellState& shell, ScrollDragTarget target,
                              const comp_ui::Rect& panel);
  int adjust_panel_visible_rows(const ShellState& shell, ScrollDragTarget target,
                                const comp_ui::Rect& panel);
  /* 写回某条滚动条的滚动量（越界夹住）。panel = 调整栏矩形（夹取要用）。 */
  void adjust_panel_set_scroll(ShellState& shell, ScrollDragTarget target, int value,
                               const comp_ui::Rect& panel);
  void draw_status_bar(HDC dc, const ShellState& shell, const comp_ui::Rect& bar);
  void draw_layer_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_channel_chip(HDC dc, const comp_ui::Rect& rect, const wchar_t* name, bool active);
  void draw_curve_editor(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_adjust_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_trim_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_doc_dialog(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_filter_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_effect_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_fill_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_color_range_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_path_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_history_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_selection_channel_panel(HDC dc, const ShellState& shell,
                                    const comp_ui::Rect& panel_rect);
  void draw_type_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  void draw_font_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect);
  bool clip_axis_aligned(const comp_ui::Rect& canvas, comp_ui::Point* a, comp_ui::Point* b);
  void draw_transform_frame(HDC dc, const ShellState& shell, const Layout& layout);
  void draw_canvas_overlays(HDC dc, const ShellState& shell, const Layout& layout);
  void draw_panel_dividers(HDC dc, const ShellState& shell, const Layout& layout);
  void draw_toggle_chevron(HDC dc, int cx, int cy, int half, bool point_left, COLORREF color);
  void draw_panel_toggle(HDC dc, const comp_ui::Rect& box, bool collapsed, bool hot, bool panel_on_left, const wchar_t* row0, const wchar_t* row1);
  void draw_panel_toggles(HDC dc, const ShellState& shell, const Layout& layout);
  /* P0-10 分组标签栏（tab strip）：一列里叠了多块面板时，列顶那条。 */
  void draw_panel_group_bars(HDC dc, const ShellState& shell, const Layout& layout);
  void update_window_title(HWND window, ShellState& shell);
  void present(HWND window, ShellState& shell);
  void set_tool(ShellState& shell, comp_ui::ToolId id);
  void set_retouch_mode(ShellState& shell, int mode);
  void set_tone_state(ShellState& shell, int state);
  void brush_step(ShellState& shell, int direction, bool hardness);
  /*
   * ROI #7 HiDPI（shell_dpi.cpp）。壳层的排版常量全部是**逻辑像素**，上屏时
   * 靠 memory DC 的 MM_ANISOTROPIC 映射统一放大，所以除了下面四条入口，其余
   * 代码不需要知道 DPI 存在：
   *   - dpi_enable_awareness：进程级 Per-Monitor V2（必须在建窗前调用）；
   *   - dpi_for_system / dpi_for_window：取 DPI（旧系统上退回 GetDeviceCaps）；
   *   - dpi_logical / dpi_device：设备像素 <-> 逻辑像素。
   */
  void dpi_enable_awareness();
  int dpi_for_system();
  int dpi_for_window(HWND window);
  int dpi_logical(int device_px, int dpi);
  int dpi_device(int logical_px, int dpi);
  int shell_dpi(const ShellState& shell);
  bool client_layout(const ShellState& shell, Layout* out);
  /*
   * 窗口边框与几何（2026-10-27）。
   *
   * 深色 UI 把客户区内容画到窗口最外沿，系统那圈边框在视觉上与内容没有分界——
   * 用户「找不到能拖的地方」。这三条把窗口缩放变成壳自己的判断，并让窗口像别的
   * 程序一样记住上次的大小与位置：
   *   - window_min_track_size：WM_GETMINMAXINFO 的客户区下限（逻辑像素）；
   *   - window_resize_hit：客户区边缘的缩放命中码（HTLEFT 等，0 = 不是边缘）；
   *   - window_geometry_*：位置 / 尺寸 / 最大化状态的存读（window.ini）。
   */
  void window_min_track_size(const ShellState& shell, int* width, int* height);
  int window_resize_hit(const ShellState& shell, int client_x, int client_y);
  /* 返回 true = 上次退出时是最大化，调用方要在首次 ShowWindow 之后再最大化。 */
  bool window_geometry_restore(HWND window, ShellState& shell);
  void window_geometry_save(HWND window, const ShellState& shell);
  void panel_resize_begin(HWND window, ShellState& shell, int target, int mouse_x);
  void panel_resize_extend(ShellState& shell, int mouse_x);
  void panel_resize_end(ShellState& shell);
  void panel_resize_cancel(ShellState& shell);
  void panel_resize_reset(ShellState& shell, int target);
  void panel_toggle_collapsed(ShellState& shell, int target);
  void view_fit(ShellState& shell);
  void view_actual(ShellState& shell);
  void view_zoom_step(ShellState& shell, double factor);
  bool zoom_tool_available(const ShellState& shell);
  void zoom_tool_click(ShellState& shell, int screen_x, int screen_y, const Layout& layout);
  HCURSOR make_zoom_cursor(bool zoom_out);
  HCURSOR zoom_cursor_handle(bool zoom_out);
  void zoom_marquee_begin(HWND window, ShellState& shell, int x, int y);
  void zoom_marquee_extend(ShellState& shell, int x, int y);
  void zoom_marquee_end(HWND window, ShellState& shell);
  void begin_pan(HWND window, ShellState& shell, int x, int y);
  void end_pan(HWND window, ShellState& shell);
  void canvas_click(ShellState& shell, int x, int y, const Layout& layout);
  void begin_color_input(ShellState& shell);
  void refresh_color_input_status(ShellState& shell);
  void commit_color_input(ShellState& shell);
  bool adjust_row_param(const ShellState& shell, int32_t row, comp_adjustment_param* out);
  bool adjust_write_live(ShellState& shell);
  void adjust_drag_value(ShellState& shell, double x);
  void adjust_begin_drag(ShellState& shell, const comp_ui::Rect& panel, int32_t row, double x);
  void adjust_drag_finish(ShellState& shell, bool commit);
  void adjust_scroll_to_focus(ShellState& shell);
  const std::string* selected_adjustment_settings(const ShellState& shell);
  bool curve_editable(const ShellState& shell);
  const wchar_t* curve_channel_name(int32_t channel);
  void refresh_curve_points(ShellState& shell);
  bool curve_write_points(ShellState& shell);
  void curve_drag_to(ShellState& shell, const comp_ui::Rect& plot, double x, double y);
  void curve_begin_drag(ShellState& shell, const comp_ui::Rect& plot, int32_t point, double x, double y);
  void curve_drag_finish(ShellState& shell, bool commit);
  void curve_add_point(ShellState& shell, const comp_ui::Rect& plot, double x, double y);
  void curve_delete_point(ShellState& shell, int32_t index);
  void curve_set_channel(ShellState& shell, int32_t channel);
  void adjust_set_channel(ShellState& shell, int32_t channel);
  void handle_key(HWND window, ShellState& shell, WPARAM key, bool ctrl, bool shift, bool alt);
  LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  HMENU build_menu();
  const char* backend_name(comp_backend backend);
  bool probe_png_exists(const char* path);
  bool write_probe_png(comp_engine engine, const char* path);
  int run_selftest(const wchar_t* out_path);
  LRESULT CALLBACK bench_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  void pump_pending_messages();
  double elapsed_ms(const LARGE_INTEGER& start, const LARGE_INTEGER& end, double frequency);
  comp_document build_present_bench_document(comp_engine engine, int32_t size);
  int run_present_bench(HINSTANCE instance, int frames);

}  // namespace comp_shell

#endif  // COMPOSITOR_SHELL_INTERNAL_H
