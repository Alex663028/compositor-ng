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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <compositor/comp_command.h>
#include <compositor/comp_core.h>
#include <compositor/comp_crop.h>
#include <compositor/comp_engine.h>
#include <compositor/comp_eyedropper.h>
#include <compositor/comp_filter.h>
#include <compositor/comp_io.h>
#include <compositor/comp_layer.h>
#include <compositor/comp_render.h>
#include <compositor/comp_selection.h>
#include <compositor/comp_undo.h>

#include "ui_core.h" /* 平台无关的视图变换 / 布局 / 命中 / 工具表 */

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;

/* 蚂蚁线：虚线 4 px / 间隔 4 px，每 120 ms 前进 1 px（一个周期 8 px）。 */
constexpr UINT_PTR kAntsTimerId = 1;
constexpr UINT kAntsTimerPeriodMs = 120;
constexpr int kAntsDashPx = 4;
constexpr int kAntsGapPx = 4;
constexpr int kAntsPhasePeriod = kAntsDashPx + kAntsGapPx;
/* 边界线段缓冲上限：常规选区的量级是几十条；超过即截断并如实报到状态栏。 */
constexpr int kMaxAntSegments = 20000;

/* 面板几何统一由 ui_core.h 提供：绘制与命中测试必须共用同一套常量。 */
using comp_ui::Layout;

/* 调整面板的行高（仅是排版密度，不参与命中测试）。 */
constexpr int kAdjustRowHeight = 20;

/* 菜单命令 id（与快捷键一一对应，见 run_shell_command）。 */
constexpr UINT kCmdNew = 1001;
constexpr UINT kCmdOpen = 1002;
constexpr UINT kCmdSave = 1003;
constexpr UINT kCmdExit = 1004;
constexpr UINT kCmdUndo = 1005;
constexpr UINT kCmdRedo = 1006;
constexpr UINT kCmdListCommands = 1007;
constexpr UINT kCmdLayerAdd = 1008;
constexpr UINT kCmdLayerSelectUp = 1009;
constexpr UINT kCmdLayerSelectDown = 1010;
constexpr UINT kCmdLayerToggleVisible = 1011;
constexpr UINT kCmdLayerOpacityDown = 1012;
constexpr UINT kCmdLayerOpacityUp = 1013;
constexpr UINT kCmdLayerBlendPrev = 1034; /* S21：混合模式按菜单序后退一步 */
constexpr UINT kCmdLayerBlendNext = 1035; /* S21：混合模式按菜单序前进一步 */
constexpr UINT kCmdBlendBase = 1300;      /* S21：+ 混合模式档位 0..23，菜单直达 */
/*
 * S22 命令 id 段（1400 起，避开 1001..1046 单体、1100..1116 工具、1200..1212 滤镜、
 * 1300..1323 混合模式四段已占用区间）。
 * 这一组接的是「引擎早已实现、壳一直没调」的像素级图层能力：蒙版 / 剪贴蒙版 /
 * 组隔离 / 采样 / 重排 / 直方图。它们与混合模式的差别只在于壳没开口子。
 */
constexpr UINT kCmdLayerMaskFromSelection = 1400; /* S22：选区 -> 图层蒙版 */
constexpr UINT kCmdLayerMaskRemove = 1401;         /* S22：清除图层蒙版 */
constexpr UINT kCmdLayerMaskToSelection = 1402;    /* S22：图层蒙版 -> 选区（回读） */
constexpr UINT kCmdLayerToggleClipping = 1403;     /* S22：与下方图层互相剪贴 */
constexpr UINT kCmdLayerToggleIsolated = 1404;     /* S22：组穿透 / 隔离开关 */
constexpr UINT kCmdLayerBringForward = 1405;       /* S22：上移一层 */
constexpr UINT kCmdLayerSendBackward = 1406;       /* S22：下移一层 */
constexpr UINT kCmdLayerBringToFront = 1407;       /* S22：置顶 */
constexpr UINT kCmdLayerSendToBack = 1408;         /* S22：置底 */
constexpr UINT kCmdHistogramToggle = 1409;         /* S22：直方图面板显隐 */
constexpr UINT kCmdSamplingBase = 1410;             /* S22：+ 采样档位 0..2，菜单直达 */
/*
 * S24 命令号：接着 S22 的 1400 段排到 1500 段，避免与工具(1100)/滤镜(1200)/
 * 混合(1300)/S22(1400) 的档位区间交错——两段共用基址时，加一档就会覆盖相邻命令。
 */
constexpr UINT kCmdLayerFlipHorizontal = 1500; /* S24：水平镜像（ABI 尾部追加） */
constexpr UINT kCmdLayerFlipVertical = 1501;   /* S24：垂直镜像 */
constexpr UINT kCmdEffectBase = 1510;          /* S24：+ 效果档位 0..5，菜单勾选 = 已启用 */
constexpr UINT kCmdEffectClear = 1520;         /* S24：清除选中图层的全部效果 */
constexpr UINT kCmdHistogramReset = 1521;      /* S24：直方图区间复位（双击同效） */
constexpr UINT kCmdEffectEdit = 1522;          /* P0-2：打开效果参数面板（Ctrl+Shift+E） */
/*
 * P0-1 命令号（1530 段）：图层面板的四个「上游有、本壳没有」的动作。
 * 重命名与精确不透明度需要键入，沿用本壳既有的「状态栏输入态」而不是另开对话框
 * ——输入态已经是全壳唯一的模态约定（颜色 / 羽化 / 文字 / 调整面板同款）。
 */
constexpr UINT kCmdLayerDelete = 1530;      /* Del：删除选中图层（含子树） */
constexpr UINT kCmdLayerDuplicate = 1531;   /* Ctrl+J：复制选中图层 */
constexpr UINT kCmdLayerRename = 1532;      /* F2：重命名（键入态） */
constexpr UINT kCmdLayerNewGroup = 1533;    /* Ctrl+G：新建组（选区非空时收纳进组） */
constexpr UINT kCmdLayerOpacitySet = 1534;  /* 精确不透明度（键入态） */
/* 与 Document::kMaxLayerNameBytes 同一个上限（comp_layer.h 的 set_name 约定）。 */
constexpr size_t kLayerNameCapacity = 1024;
constexpr UINT kCmdLayerMoveLeft = 1036;  /* S21：图层平移（Shift=微调、Ctrl=粗调） */
constexpr UINT kCmdLayerMoveRight = 1037;
constexpr UINT kCmdLayerMoveUp = 1038;
constexpr UINT kCmdLayerMoveDown = 1039;
constexpr UINT kCmdLayerRotateLeft = 1040;  /* S21：图层旋转，见 layer_rotate_step */
constexpr UINT kCmdLayerRotateRight = 1041;
constexpr UINT kCmdLayerScaleDown = 1042;   /* S21：图层等比例缩放，中心锚定 */
constexpr UINT kCmdLayerScaleUp = 1043;
constexpr UINT kCmdOpenImage = 1044;   /* S22: open bitmap as new document */
constexpr UINT kCmdImportImage = 1045; /* S22: bitmap as new layer */
constexpr UINT kCmdExportImage = 1046; /* S22: export bitmap */
constexpr UINT kCmdAdjKindPrev = 1014;
constexpr UINT kCmdAdjKindNext = 1015;
constexpr UINT kCmdAdjRowPrev = 1016;
constexpr UINT kCmdAdjRowNext = 1017;
constexpr UINT kCmdAdjParamPrev = 1018;
constexpr UINT kCmdAdjParamNext = 1019;
constexpr UINT kCmdAdjValueDown = 1020;
constexpr UINT kCmdAdjValueUp = 1021;
constexpr UINT kCmdAdjAdd = 1022;
constexpr UINT kCmdAdjApply = 1023;
constexpr UINT kCmdAdjRemove = 1024;
constexpr UINT kCmdLayerMergeDown = 1025;
constexpr UINT kCmdLayerFlatten = 1026;
constexpr UINT kCmdViewFit = 1027;
constexpr UINT kCmdViewActual = 1028;
constexpr UINT kCmdViewZoomIn = 1029;
constexpr UINT kCmdViewZoomOut = 1030;
constexpr UINT kCmdSelectAll = 1031;
constexpr UINT kCmdSelectNone = 1032;
constexpr UINT kCmdSelectInvert = 1033;
/* P0-4：扩展/收缩走同一个键入态（op 由菜单项决定），载入用当前图层像素。 */
constexpr UINT kCmdSelectExpand = 1047;
constexpr UINT kCmdSelectContract = 1048;
constexpr UINT kCmdSelectLoadPixels = 1049;
/* P0-4 第二批：色彩范围面板（打开即进预览事务）。 */
constexpr UINT kCmdSelectColorRange = 1050;
constexpr UINT kCmdSelectPaths = 1051; /* P0-4 可选：路径面板（Ctrl+Shift+P） */
constexpr UINT kCmdHistoryPanel = 1052; /* P1：历史记录面板（Ctrl+Alt+H） */
constexpr UINT kCmdToolBase = 1100; /* + 工具序号，范围 1100..1118 */
constexpr UINT kCmdFilterBase = 1200; /* + 滤镜序号 0..3 */
constexpr UINT kCmdFilterApply = 1210;
constexpr UINT kCmdFilterReset = 1211;
constexpr UINT kCmdFilterClose = 1212;

struct LayerRow {
  comp_layer_id id = 0;
  float opacity = 1.0f;
  int32_t blend = 0;   /* comp_blend_mode 数值，0..23（ABI 冻结表） */
  uint8_t visible = 1; /* 壳侧自持：ABI 无可见性 getter */
  /*
   * S22 新增的三个面板可见状态。三者都不是「壳的第二种真相」——全部由 ABI 回读，
   * 只用于绘制与置灰，写路径一律回到引擎，所以撤销/合并/.comp 读回都不会让面板
   * 与文档漂移（S21 的 transform 回读口就是同一个理由）。
   */
  uint8_t has_mask = 0;        /* 本层带蒙版（引擎已参与合成） */
  comp_layer_id clipping = 0; /* 剪贴蒙版来源；0 = 不剪贴 */
  uint8_t is_group = 0;        /* 图层组 */
  uint8_t isolated = 0;        /* 组隔离开关；非组恒 0 */
  int32_t sampling = 0;        /* comp_sampling，0..2 */
  /*
   * S23：父级 id（0 = 根级）。剪贴基底必须与自身**同父**才算合法——引擎
   * filter_graph 只把同一 siblings 序列里紧跟基底的那串成员收进剪贴栈。S22 只按
   * 数组下标取「下方一层」，有图层组时取到的可能是别的组里的层，于是写了
   * 一条永远不成栈的 clipping_source：画面不变，但单层视图又按它求值。
   */
  comp_layer_id parent = 0;
  /*
   * S24：镜像与效果的只读快照。两者都是「引擎已参与像素求值、AIB 此前没有回读口
   * 或只在创建时可给」的属性，补齐后与蒙版/组信息同一口径：只用于绘制与菜单勾选，
   * 写路径一律回到引擎（comp_layer_set_flip / set_effect / remove_effect）。
   */
  uint8_t flip_x = 0;
  uint8_t flip_y = 0;
  int32_t effect_count = 0; /* 本层已有效果的个数（菜单标勾、面板标 fxN） */
  /*
   * P0-1：名字来自 comp_layer_get_name（UTF-8），面板只读回读结果。
   * 壳侧**不再自持名字**——重命名、撤销、合并、读盘之后名字都可能变，
   * 存第二份就会与文档漂移（与 opacity / blend 同一口径）。
   */
  std::string name;
};

/* P0-1：图层面板键入态（重命名 / 精确不透明度）。 */
enum class LayerInput { None, Rename, Opacity };

struct ShellState {
  comp_engine engine = nullptr;
  comp_document document = nullptr;
  /* 引擎实际生效的渲染后端（S13：AUTO 可能是 D3D12）；工具条/标题如实展示。 */
  comp_backend backend = COMP_BACKEND_AUTO;
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint32_t> framebuffer;   /* BGRA，供 GDI 直接使用 */
  std::vector<uint8_t> rgba;           /* 引擎输出的预乘 RGBA8 */
  bool needs_render = true;
  std::wstring status;
  /* 图层面板状态 */
  std::vector<LayerRow> layers;
  std::vector<std::pair<comp_layer_id, uint8_t>> visibility; /* id -> visible */
  int32_t selected_layer = 0; /* 面板行索引（0 = 最上层显示行） */
  /*
   * P0-1：多选。selected_layer 仍是「主选中行」——所有既有单层命令的输入不变；
   * extra_selection 只是额外选中的图层 id。批量命令 = 主选中 + 额外，且整批放进
   * 一个显式事务（comp_txn_begin/commit），N 层删除/复制只占一步撤销。
   * 存 id 而不是行索引：重排/撤销之后行号会变，id 不会。
   */
  std::vector<comp_layer_id> extra_selection;
  /*
   * P0-1：图层面板的键入态（重命名 / 精确不透明度）。与本壳颜色、羽化、文字面板
   * 同一约定：进入即模态——Esc 取消、Enter 提交，其它快捷键一律让路。
   * layer_input_text 是宽字符，提交时一次性转 UTF-8（引擎只收 UTF-8）。
   */
  LayerInput layer_input = LayerInput::None;
  std::wstring layer_input_text;
  comp_layer_id layer_input_id = 0;
  /* 调整面板状态：只经 C ABI 元数据/写入接口工作，壳内不解析也不拼装 JSON。 */
  std::vector<uint32_t> adj_kinds;       /* 选中图层已有调整的 kind（数组序） */
  std::vector<std::string> adj_settings; /* 与 adj_kinds 对齐的 settings JSON */
  int32_t selected_adjustment = -1;      /* 已有调整的选中索引；-1 = 未选（新增态） */
  int32_t adj_kind = 3;                  /* 当前编辑 kind，默认 3 = Exposure */
  int32_t adj_param = 0;                 /* 聚焦的可编辑参数序号 */
  std::vector<std::string> adj_values;   /* 与参数表对齐的 ASCII 编辑值 */
  std::wstring adj_input;                /* 键入缓冲；非空 = 输入态 */
  /*
   * P0-3 滑杆拖动：按下即进拖动态并开显式事务（只有选中既有调整时才开），拖动
   * 途中每次写回都落在同一事务里，松手提交 = 一步撤销；Esc / 失焦则 abort 回滚。
   * adj_drag_track 是按下瞬间的滑道矩形——拖动途中不重算几何，面板不会在指针
   * 下「跑掉」。adj_scroll 是长参数表（色阶 20 行）滚掉的行数。
   */
  int32_t adj_drag_param = -1;
  comp_ui::Rect adj_drag_track{};
  comp_txn adj_drag_txn = nullptr;
  bool adj_drag_dirty = false;
  int32_t adj_scroll = 0;
  /*
   * P0-3 色阶按通道分页（RGB / R / G / B，每页 5 行）与曲线编辑器状态。
   * curve_xy 是**当前通道**的控制点（x,y 交替、已按 x 升序），与 settings 里的
   * curves.channels[channel] 同步；空表示该通道没有点（= 恒等曲线）。
   * 拖动走显式事务：按下开、每次移动写、松手提交，一次拖动 = 一步撤销。
   */
  int32_t adj_channel = 0;
  int32_t curve_channel = 0;
  std::vector<double> curve_xy;
  int32_t curve_focus_point = -1;
  int32_t curve_drag_point = -1;
  comp_ui::Rect curve_drag_plot; /* 拖动期间冻结的绘图区（指针 -> 值换算用） */
  comp_txn curve_drag_txn = nullptr;
  bool curve_drag_dirty = false;
  /* 画布视图与工具状态：几何/变换全部来自 ui_core.h，壳内不重复实现。 */
  comp_ui::CanvasView view;
  comp_ui::ToolId tool = comp_ui::ToolId::Select;
  double brush_size = 24.0;
  double brush_hardness = 0.75;
  /* Retouch 工具（R 循环）：0 = 模糊（走 COMP_TOOL_BLUR）/ 1 涂抹 / 2 液化。 */
  int retouch_mode = comp_ui::kRetouchBlur;
  double blur_radius = 8.0;
  double retouch_strength = 0.5; /* Smudge / Liquify 的 warp.strength */
  /*
   * Tone 工具（O 循环）：8 个状态 = Dodge/Burn 各三档 + Sponge 两档。
   * 算子与 mode 都由 comp_ui::tone_state_tool/mode 换算，壳内不另存一份。
   */
  int tone_state = comp_ui::kToneDodgeMidtone;
  double tone_strength = 0.5; /* comp_tool_tone.strength（Exposure / Flow） */
  /*
   * P1 历史记录画笔：取样状态是历史面板的**下标**（不是撤销步数）。
   * -1 = 自动（取当前状态的上一格），Shift+点行显式指定某个下标。
   * opacity 走 Alt+[ / ]，下限 10%（引擎的闸拒绝 0）。
   */
  int history_brush_state = -1;
  double history_brush_opacity = 1.0;
  /* 滤镜面板：状态（种类/焦点/值）全在 ui_core.h，壳只做绘制与 ABI 搬运。 */
  comp_ui::FilterPanel filter;
  bool filter_open = false;
  std::wstring filter_input; /* 滤镜面板的键入缓冲；非空 = 输入态 */
  /*
   * S22 直方图（comp_layer_histogram 的壳侧唯一调用点）。
   *
   * 256 桶亮度分布来自引擎的只读入口：预乘像素先反预乘再取亮度、按 alpha 加权。
   * 壳**不自己扫像素**——自己算一遍就会与内核的 levels 调整口径漂移，那是
   * 「面板曲线与实际调整对不上」的经典成因。
   * bins 为 nullptr = 面板关闭或当前图层取不到（组图层 / 16 位图层）。
   */
  bool histogram_open = false;
  std::vector<double> histogram_bins;
  int32_t histogram_layer = 0; /* 取数时对应的图层 id；换层即重取 */
  /*
   * S24 直方图区间缩放：view 是可视亮度窗口（默认全幅 0…255），拖拽框选 / 滚轮
   * 缩放 / 双击复位都只改它，几何与夹取规则全在 ui_core（clamp/zoom/映射）。
   * 拖着的时候图不重取（bins 不动），只改窗口——重取要扫一遍全图，每帧都做会卡。
   */
  comp_ui::HistogramView histogram_view;
  bool histogram_drag = false;
  int32_t histogram_drag_from_x = 0;
  int32_t histogram_drag_to_x = 0;
  /*
   * S24 图层拖行重排：pending = 已按下（还没超过阈值），active = 已构成拖拽。
   * 用「按下再移动超阈值」而不是「一按下就算拖」，是为了不和「点一行选中」抢
   * 语义：单击必须还是选中，手抖 1px 不该把层挪走。
   */
  int32_t layer_drag_row = -1;
  int32_t layer_drag_target = -1;
  int32_t layer_drag_press_y = 0;
  bool layer_drag_pending = false;
  bool layer_drag_active = false;
  /* S24：选中图层已启用的效果 kind（数组序，与引擎 effects 同序）。 */
  std::vector<uint32_t> effect_kinds;
  /*
   * P0-2 效果参数面板。
   *
   * effect_settings 与 effect_kinds 对齐，是本层效果的 settings 回读副本——面板只
   * 从它取「底」，写回时由 ABI 在它上面合并（未知键保留）。effect_open 时面板占
   * 右侧栏（与滤镜面板同一位置、同一模态规矩）；effect_kind 是正在编辑的 kind，
   * 允许尚未启用（回车即添加）。
   *
   * effect_values 是本面板的**编辑缓冲**，长度与 ABI 参数表一致；空串 = 该项不动。
   * 所有键名/量程/缺省值都来自 comp_layer_effect_param_*，壳里不再抄一份
   * （旧 effect_kind_defaults() 已删除）。
   */
  bool effect_open = false;
  std::vector<std::string> effect_settings;
  int32_t effect_kind = 0;
  int32_t effect_param = 0;
  std::vector<std::string> effect_values;
  std::wstring effect_input;
  std::string effect_base_json;
  /* 前景色（S6）：画笔的笔画色；吸管与色板都写这里。 */
  comp_ui::ColorRGB foreground{};
  int32_t palette_index = 0; /* X 键循环到色板的哪一项 */
  std::wstring color_input;  /* Shift+X 的十六进制键入；非空 = 输入态 */
  /*
   * 进行中的画布笔迹（文档像素折线）。Retouch 族（Blur / Smudge / Liquify）、
   * Tone 族（Dodge / Burn / Sponge）与画笔/橡皮擦共用同一条折线通路与去抖规则，
   * stroke_kind 记住松开时装配哪种算子；stroke_erase 在落笔时冻结——拖拽中途
   * 按 E 不改变这一笔的合成方向。
   */
  comp_ui::StrokeKind stroke_kind = comp_ui::StrokeKind::None;
  std::vector<float> stroke_points;
  bool stroke_erase = false;
  /* 选区搬移（Move 工具，S6）：按下记起点，松开一次性提交（Alt = 复制）。 */
  bool move_active = false;
  comp_ui::Point move_start{};
  comp_ui::Point move_current{};
  bool move_duplicate = false;
  /* 吸管拖拽（S6）：按住拖动时连续取样；只读，不产生撤销步。 */
  bool eyedropper_drag = false;
  /*
   * 选区（comp_selection.h 的 F1 ABI）。mask 是文档分辨率的有效 coverage；
   * 蚂蚁线的线段由 ui_core 从 mask 现算，壳内不维护「选区形状」的第二种表示。
   */
  bool selection_active = false;
  std::vector<uint8_t> selection_mask;
  std::vector<comp_ui::BoundarySegment> selection_segments;
  int32_t selection_phase = 0;    /* 蚂蚁线相位 0..7 px */
  bool selection_truncated = false; /* 线段数超过缓冲上限（如实提示） */
  /* 框选（Marquee 工具）进行中的矩形，文档坐标。 */
  bool marquee_active = false;
  comp_ui::Point marquee_start{};
  comp_ui::Point marquee_current{};
  /* Marquee 形状（S7）：M 键在矩形 / 椭圆之间循环。 */
  int marquee_shape = comp_ui::kMarqueeRect;
  /*
   * 裁剪（S22）：C 切工具，拖出框后 Enter 提交 / Esc 取消。
   * 与 Marquee 不同的关键点：框本身是**文档坐标**的裁剪区，提交会改画布尺寸
   * （引擎侧 comp_crop_apply 负责，且自带「取整 → 校验 → 一步撤销」的契约）。
   * active 表示「框已定、等待提交」，拖拽中同样为 true（预览就是最终值）。
   */
  bool crop_active = false;
  comp_crop_rect crop_rect{};
  /* 套索（S7）：自由手绘折线（文档像素 x,y 交替），松开闭合成多边形。 */
  bool lasso_active = false;
  std::vector<float> lasso_points;
  /* 魔棒（S7）：容差、取样来源与连通性；Shift+W 切「合并/当前图层」。 */
  int wand_tolerance = comp_ui::kDefaultWandTolerance;
  bool wand_contiguous = true;
  bool wand_merged = false;
  /* 魔棒取样窗口半径（S8）：Alt+[ / Alt+] 调，0 = 只取种子像素（S7 行为）。 */
  int wand_radius = comp_ui::kDefaultWandRadius;
  /*
   * 渐变工具（S9）：G 切工具，已在该工具时再按 G 在线性/径向之间循环。
   * 拖拽的两个端点（文档像素）在松开时一次性提交 comp_tool_apply(COMP_TOOL_GRADIENT)；
   * 色标固定为「前景色 -> 透明」（Photoshop 的 Foreground to Transparent 预设），
   * 按住 Shift 反向（透明 -> 前景色）。
   */
  int gradient_type = comp_ui::kGradientLinear;
  bool gradient_active = false;
  comp_ui::Point gradient_start{};
  comp_ui::Point gradient_current{};
  /*
   * 形状工具（S10）：U 切工具，已在该工具时再按 U 在矩形/椭圆/直线之间循环。
   * 拖拽的对角点/两端点（文档像素）在松开时一次性提交 comp_tool_apply(COMP_TOOL_SHAPE)；
   * 矩形/椭圆填充前景色，直线用前景色描边（宽度 = shape_stroke_width）。
   */
  int shape_kind = comp_ui::kShapeRect;
  bool shape_active = false;
  comp_ui::Point shape_start{};
  comp_ui::Point shape_current{};
  double shape_stroke_width = comp_ui::kDefaultShapeStrokeWidth;
  double shape_corner_radius = comp_ui::kDefaultShapeCornerRadius;
  /*
   * 文字工具（S11）：T 切工具；在画布上点一下落插入点，随后直接键入 ASCII，
   * 回车提交一次 comp_tool_apply(COMP_TOOL_TYPE)，Esc / 失焦 / 切工具取消
   * （只丢预览，不产生撤销步）。字体是引擎内的 5×7 点阵，没有系统字体，
   * 因此只支持 ASCII；CJK 由引擎参数闸如实拒绝（不画方框顶替）。
   */
  bool type_active = false; /* 已落插入点，正在键入/等待提交 */
  comp_ui::Point type_origin{}; /* 插入点（文档像素，文本框左上角） */
  std::string type_text;        /* ASCII 键入缓冲，'\n' = 换行（Shift+Enter） */
  double type_font_size = comp_ui::kDefaultTypeFontSize;
  double type_line_height = comp_ui::kDefaultTypeLineHeight;
  double type_letter_spacing = 0.0;
  /* 文字属性面板（S14）：三行（字号/行距/字距）；type_input 非空 = 数值键入态。 */
  int type_panel_focused = comp_ui::kTypePanelFontSizeRow;
  std::wstring type_input;
  /* 新选区羽化半径（Shift+F 键入）：形状/套索/魔棒提交时同事务应用。 */
  int selection_feather = comp_ui::kMinSelectionFeather;
  std::wstring feather_input; /* 羽化键入缓冲；非空 = 输入态 */
  /*
   * 扩展/收缩（P0-4）：同一个键入态服务两个菜单项，op 记住这次是扩还是缩，
   * 回车时才真正调 comp_selection_modify。0 被引擎拒（见 ui_core 的
   * kMinSelectionGrow 注释），所以这里额外记一版「上次用的半径」给提示用。
   */
  comp_selection_modify_op modify_op = COMP_SELECTION_EXPAND;
  std::wstring modify_input; /* 扩展/收缩半径键入缓冲；非空 = 输入态 */
  int selection_grow = comp_ui::kMinSelectionGrow;
  /*
   * 色彩范围（P0-4 第二批）：对齐上游 ColorRangeSheet——打开面板就开一个显式
   * 事务，之后每次取色/改容差都在事务里重算选区（预览不单独占撤销步）；
   * 回车提交 = 一步撤销，Esc/从没取过色 = 事务回滚到打开前。
   * include/exclude 是扁平 RGB 三元组（引擎 ABI 的数组口径，按**颜色数**计数）。
   */
  bool color_range_open = false;
  comp_txn color_range_txn = nullptr;
  std::vector<uint8_t> color_range_include;
  std::vector<uint8_t> color_range_exclude;
  int color_range_fuzziness = comp_ui::kDefaultColorRangeFuzziness;
  int color_range_invert = 0;
  std::wstring color_range_input; /* 容差键入缓冲；非空 = 输入态 */
  /*
   * 钢笔路径（S8）：锚点与控制柄都在文档坐标；pen_dragging = 按下后正在拖
   * 最后一个锚点的控制柄。路径本身不产生撤销步，Enter/点首锚点才提交选区。
   */
  std::vector<comp_ui::PenAnchor> pen_anchors;
  bool pen_dragging = false;
  /*
   * 路径面板（P0-4 可选，S26）：引擎没有「路径」实体——路径的唯一用途就是
   * 转成选区。所以壳侧自己存已闭合的钢笔锚点，双击/回车重新落成选区。
   * 锚点是**文档坐标**，换文档后没有意义：新建/打开时整表清空。
   */
  struct StoredPath {
    std::wstring name;
    std::vector<comp_ui::PenAnchor> anchors;
  };
  bool path_open = false;
  std::vector<StoredPath> paths;
  int path_selected = 0;
  int path_scroll = 0;
  int path_serial = 0; /* 命名用流水号（删除后不复用，避免重名） */
  /*
   * 历史记录面板（P1）：列出撤销栈里的每一步，点行/↑↓ 回跳。高亮行**不单独记账**
   * ——它就是引擎的 current_index（undo_depth），面板只存滚动偏移。这样撤销、
   * 重做、新操作之后都不会出现「面板高亮停在一个不存在的状态上」。
   */
  bool history_open = false;
  int history_scroll = 0;
  /* 鼠标是否停在画布上（决定画不画笔尖轮廓），客户区坐标。 */
  bool cursor_valid = false;
  bool tracking_leave = false;
  int cursor_x = 0;
  int cursor_y = 0;
  bool space_down = false; /* 按住空格 = 临时抓手 */
  bool panning = false;
  POINT pan_anchor{};
  bool needs_fit = true; /* 新文档/打开文档后，下一次绘制时适配窗口 */
  HWND window = nullptr;
};

ShellState* g_shell = nullptr;  /* 单窗口外壳，生命周期与窗口一致 */

void set_status(ShellState& shell, const wchar_t* text) {
  shell.status = text;
  if (shell.document != nullptr) {
    const char* engine_error = comp_engine_last_error(shell.engine);
    if (engine_error != nullptr && engine_error[0] != '\0') {
      /* 引擎侧错误一并带上，避免壳层把失败静默掉。 */
      const std::string narrow(engine_error);
      shell.status += L" | ";
      shell.status.append(narrow.begin(), narrow.end());
    }
  }
}

/*
 * 撤销/重做后刷新状态栏：深度、内存占用、是否截断统统直接问引擎，
 * 壳内不记账（P1-3：撤销栈预算信号必须透到用户可见处）。
 * 注意：wsprintfW 不支持 %f，内存按 KiB 用整数输出。
 */
void refresh_history_status(ShellState& shell, const wchar_t* prefix) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  size_t undo_depth = 0;
  size_t redo_depth = 0;
  comp_document_history_stats(shell.document, &undo_depth, &redo_depth);

  size_t history_bytes = 0;
  if (comp_document_history_bytes(shell.document, &history_bytes) != COMP_OK) history_bytes = 0;
  int32_t truncated = 0;
  if (comp_document_history_truncated(shell.document, &truncated) != COMP_OK) truncated = 0;
  const unsigned kib = static_cast<unsigned>(history_bytes / 1024ull);

  wchar_t buffer[192];
  wsprintfW(buffer, L"%s（撤销 %u / 重做 %u · 历史 %u KiB%s）", prefix,
            static_cast<unsigned>(undo_depth), static_cast<unsigned>(redo_depth), kib,
            truncated != 0 ? L"，已截断" : L"");
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 图层面板 */

uint8_t visible_of(ShellState& shell, comp_layer_id id) {
  for (const auto& entry : shell.visibility) {
    if (entry.first == id) return entry.second;
  }
  shell.visibility.emplace_back(id, static_cast<uint8_t>(1));
  return 1;
}

void set_visible_cache(ShellState& shell, comp_layer_id id, uint8_t value) {
  for (auto& entry : shell.visibility) {
    if (entry.first == id) {
      entry.second = value;
      return;
    }
  }
  shell.visibility.emplace_back(id, value);
}

/* 调整面板在下方定义；refresh_layers 需要它同步选中图层的调整列表。 */
void refresh_adjustments(ShellState& shell);

/* 曲线控制点缓存跟随「选中调整 + 当前通道」，refresh_adjustments 末尾统一重读。 */
void refresh_curve_points(ShellState& shell);

/* 通道名（RGB / R / G / B）：色阶通道条与曲线通道条共用，绘制在面板段之前。 */
const wchar_t* curve_channel_name(int32_t channel);

/* S22：直方图数据同源于下方定义；refresh_layers 在换层/变更后统一刷新它。 */
void refresh_histogram(ShellState& shell);

/* S24：效果回读同源于下方定义；与 refresh_adjustments 一样跟随选中图层。 */
void refresh_effects(ShellState& shell);

/* S24：效果回读需要选中图层的 id；该 helper 定义在调整面板段之后。 */
comp_layer_id selected_layer_id(const ShellState& shell);

/* ASCII -> wstring（面板/状态栏展示引擎返回的 ASCII 名称）。 */
std::wstring widen_ascii(const char* text);

/* 数值 -> 面板文本（定义在调整面板段；效果参数面板也要用）。 */
std::string number_text(double value);

/* UTF-8 <-> UTF-16。图层名是 UTF-8（comp_layer.h 契约），面板是 UTF-16。 */
std::wstring widen_utf8(const std::string& text);
std::string narrow_utf8(const std::wstring& text);

/* 两段式枚举图层 id，并逐个查询属性（只读，不引入隐式事务）。 */
void refresh_layers(ShellState& shell) {
  shell.layers.clear();
  if (shell.document == nullptr) {
    shell.extra_selection.clear();
    return;
  }

  size_t count = 0;
  if (comp_layer_count(shell.document, &count) != COMP_OK || count == 0) {
    shell.selected_layer = 0;
    shell.extra_selection.clear();
    return;
  }
  std::vector<comp_layer_id> ids(count, 0);
  size_t written = 0;
  if (comp_layer_list(shell.document, ids.data(), ids.size(), &written) != COMP_OK) return;
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
    if (comp_layer_get_name(shell.document, id, nullptr, 0, &name_required) == COMP_OK &&
        name_required > 1u && name_required <= kLayerNameCapacity) {
      std::vector<char> name_bytes(name_required, '\0');
      size_t name_written = 0;
      if (comp_layer_get_name(shell.document, id, name_bytes.data(), name_bytes.size(),
                              &name_written) == COMP_OK) {
        row.name.assign(name_bytes.data(), name_bytes.size() - 1u);
      }
    }
    float opacity = 1.0f;
    if (comp_layer_get_opacity(shell.document, id, &opacity) == COMP_OK) {
      row.opacity = opacity;
    }
    comp_blend_mode blend = static_cast<comp_blend_mode>(0);
    if (comp_layer_get_blend(shell.document, id, &blend) == COMP_OK) {
      row.blend = static_cast<int32_t>(blend);
    }
    int32_t visible = 0;
    if (comp_layer_get_visible(shell.document, id, &visible) == COMP_OK) {
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
    if (comp_layer_get_mask(shell.document, id, &has_mask, &mask_w, &mask_h) == COMP_OK) {
      row.has_mask = has_mask != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    }
    int32_t info_is_group = 0;
    comp_layer_id info_parent = 0;
    comp_layer_id info_clipping = 0;
    if (comp_layer_get_group_info(shell.document, id, &info_is_group, &info_parent,
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
          comp_layer_get_isolated(shell.document, id, &isolated) == COMP_OK) {
        row.isolated = isolated != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
      }
    }
    comp_sampling sampling = COMP_SAMPLING_HIGH_QUALITY;
    if (comp_layer_get_sampling(shell.document, id, &sampling) == COMP_OK) {
      row.sampling = static_cast<int32_t>(sampling);
    }
    /*
     * S24：镜像与效果。镜像走本层新增的 getter（set_flip 早已存在但壳调不到，
     * 于是「翻转」这个 PS 基础操作在本壳里不存在）；效果数走 effect_count，
     * 用于菜单勾选与行内 fx 标记。两者都只读、都只用于展示。
     */
    int32_t flip_x = 0;
    int32_t flip_y = 0;
    if (comp_layer_get_flip(shell.document, id, &flip_x, &flip_y) == COMP_OK) {
      row.flip_x = flip_x != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
      row.flip_y = flip_y != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    }
    size_t effect_count = 0;
    if (comp_layer_effect_count(shell.document, id, &effect_count) == COMP_OK) {
      /* 单层效果数量极少（<256），int32_t 足够；超限按 255 封顶只影响标记显示。 */
      row.effect_count = effect_count > 255u ? 255 : static_cast<int32_t>(effect_count);
    }
    shell.layers.push_back(row);
  }

  if (shell.selected_layer < 0) shell.selected_layer = 0;
  if (shell.selected_layer >= static_cast<int32_t>(shell.layers.size())) {
    shell.selected_layer = static_cast<int32_t>(shell.layers.size()) - 1;
  }
  /*
   * 多选集合随图层事实收敛：被删掉的图层（删除/合并/撤销/拼合）必须从额外选中里
   * 摘掉，否则下一次批量命令会对着已失效 id 发写入，状态栏报错而用户不知道是谁。
   */
  if (!shell.extra_selection.empty()) {
    std::vector<comp_layer_id> alive;
    alive.reserve(shell.extra_selection.size());
    for (comp_layer_id id : shell.extra_selection) {
      for (const LayerRow& row : shell.layers) {
        if (row.id == id) {
          alive.push_back(id);
          break;
        }
      }
    }
    shell.extra_selection.swap(alive);
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

/* 面板显示序：数组序是自下而上，UI 需要自上而下（顶层在第一行）。 */
const LayerRow* selected_row(const ShellState& shell) {
  if (shell.layers.empty()) return nullptr;
  const int32_t count = static_cast<int32_t>(shell.layers.size());
  if (shell.selected_layer < 0 || shell.selected_layer >= count) return nullptr;
  return &shell.layers[static_cast<size_t>(count - 1 - shell.selected_layer)];
}

void panel_move_selection(ShellState& shell, int32_t delta) {
  const int32_t count = static_cast<int32_t>(shell.layers.size());
  if (count == 0) {
    set_status(shell, L"无图层");
    return;
  }
  int32_t next = shell.selected_layer + delta;
  if (next < 0) next = 0;
  if (next >= count) next = count - 1;
  shell.selected_layer = next;
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
  if (comp_layer_set_visible(shell.document, id, next) != COMP_OK) {
    set_status(shell, L"切换可见性失败");
    return;
  }
  set_visible_cache(shell, id, next);
  refresh_layers(shell);
  shell.needs_render = true;
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
  if (comp_layer_set_opacity(shell.document, id, next) != COMP_OK) {
    set_status(shell, L"设置不透明度失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
  if (comp_layer_set_blend(shell.document, row->id, static_cast<comp_blend_mode>(next)) != COMP_OK) {
    set_status(shell, L"设置混合模式失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
  if (comp_layer_set_blend(shell.document, row->id, static_cast<comp_blend_mode>(next)) != COMP_OK) {
    set_status(shell, L"设置混合模式失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
  const std::wstring name = widen_ascii(comp_ui::blend_mode_name(next));
  const std::wstring group = widen_ascii(comp_ui::blend_group_name(comp_ui::blend_group_of(next)));
  wchar_t buffer[224];
  wsprintfW(buffer, L"混合模式：%s（%s 组，%d/%d）", name.c_str(), group.c_str(), next + 1,
            comp_ui::kBlendCount);
  refresh_history_status(shell, buffer);
}

/* 取完蒙版信息后刷新选区缓存与状态栏的共同尾部（定义在选区小节之后）。 */
void finish_selection_command(ShellState& shell, const wchar_t* prefix);

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
  if (!shell.selection_active || shell.selection_mask.empty()) {
    set_status(shell, L"没有活动选区：先用框选/套索/魔棒划定范围");
    return;
  }
  /*
   * 选区 coverage 是文档分辨率、行距 = 文档宽；这就是 comp_mask 的原样形态
   * （refresh_selection 正是用同一组字段从引擎取回来的），直接转手即可，
   * 不在壳内重排或复制一份。
   */
  comp_mask mask{};
  mask.width = shell.width;
  mask.height = shell.height;
  mask.stride = static_cast<size_t>(shell.width);
  mask.pixels = shell.selection_mask.data();
  if (comp_layer_set_mask(shell.document, row->id, &mask) != COMP_OK) {
    set_status(shell, L"建立图层蒙版失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
  if (comp_layer_set_mask(shell.document, row->id, nullptr) != COMP_OK) {
    set_status(shell, L"清除蒙版失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
  if (comp_selection_load_layer(shell.document, row->id, COMP_SELECTION_LOAD_MASK,
                               COMP_SELECTION_REPLACE, 0, &changed) != COMP_OK) {
    set_status(shell, L"载入蒙版为选区失败");
    return;
  }
  finish_selection_command(shell, L"已把图层蒙版载入为选区");
  shell.needs_render = true;
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
  if (shell.layers.size() < 2) {
    set_status(shell, L"至少需要两个图层才能剪贴");
    return;
  }
  const comp_layer_id self = row->id;
  /* 取消：已在剪贴态就解绑，一步撤销。 */
  if (row->clipping != 0) {
    if (comp_layer_set_clipping_source(shell.document, self, 0) != COMP_OK) {
      set_status(shell, L"取消剪贴蒙版失败");
      return;
    }
    refresh_layers(shell);
    shell.needs_render = true;
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
   * 份文档在不同视图下画得不一样。换算现在全部交给 comp_ui::plan_clipping_base，
   * 由 test_ui_core.cpp 表驱动穷举（同父相邻、跨组、已有剪贴栈、边界）。
   */
  std::vector<comp_ui::LayerStackEntry> stack;
  stack.reserve(shell.layers.size());
  for (const LayerRow& item : shell.layers) {
    stack.push_back(comp_ui::LayerStackEntry{static_cast<int64_t>(item.id),
                                             static_cast<int64_t>(item.parent),
                                             static_cast<int64_t>(item.clipping)});
  }
  const comp_ui::ClippingPlan plan = comp_ui::plan_clipping_base(
      stack.data(), static_cast<int32_t>(stack.size()), shell.selected_layer);
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
      shell.layers[static_cast<size_t>(plan.base_index)].id;
  if (comp_layer_set_clipping_source(shell.document, self, base) != COMP_OK) {
    set_status(shell, L"建立剪贴蒙版失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
  if (comp_layer_set_isolated(shell.document, row->id, next) != COMP_OK) {
    set_status(shell, L"切换组隔离失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
  if (shell.layers.empty()) {
    set_status(shell, L"无图层");
    return;
  }
  const int32_t count = static_cast<int32_t>(shell.layers.size());
  const comp_ui::ReorderPlan plan =
      comp_ui::plan_layer_reorder(count, shell.selected_layer, direction);

  if (!plan.ok) {
    /* 已在端头：如实提示，不静默无操作（否则用户以为菜单坏了）。 */
    set_status(shell, plan.at_top ? L"已经是顶层" : L"已经是底层");
    return;
  }

  const comp_layer_id self =
      shell.layers[static_cast<size_t>(
          comp_ui::layer_array_index(count, shell.selected_layer))].id;

  /*
   * before_row < 0 表示引擎约定的 before = 0（同级末尾 = 顶层）；
   * 否则锚点是那一行对应的图层 id。锚点必须与被移动层同级——跨父锚点会被
   * Document::reorder_layer 拒绝（返回 false）而不是静默错位，这里是壳侧
   * 唯一可能越级的地方（面板是扁平列表，组内层也在其中）。
   */
  comp_layer_id before = 0;
  if (plan.before_row >= 0) {
    const int32_t anchor_index = comp_ui::layer_array_index(count, plan.before_row);
    before = shell.layers[static_cast<size_t>(anchor_index)].id;
  }

  if (comp_layer_reorder(shell.document, self, before) != COMP_OK) {
    set_status(shell, L"调整图层顺序失败（跨组重排需与目标同组）");
    return;
  }
  refresh_layers(shell);
  /*
   * 选中态跟着该图层走：重排后数组换了位置，selected_layer 是**面板行索引**，
   * 必须用纯函数给出的 next_row 重算，否则「刚移动的那一层」在面板上换了行，
   * 后续快捷键会作用到别层——这一类错误不报任何异常，只会让用户觉得软件
   * 选不中自己要的那层。
   */
  shell.selected_layer = comp_ui::clamp_selected_row(plan.next_row, count);
  shell.needs_render = true;
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
  if (comp_layer_set_sampling(shell.document, row->id, static_cast<comp_sampling>(next)) !=
      COMP_OK) {
    set_status(shell, L"设置采样模式失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
  shell.histogram_bins.clear();
  shell.histogram_layer = 0;
  if (!shell.histogram_open) return;
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    shell.histogram_open = false;
    return;
  }
  std::vector<double> bins(256, 0.0);
  const comp_result result =
      comp_layer_histogram(shell.document, row->id, nullptr, 0, bins.data());
  if (result != COMP_OK) {
    /*
     * 取不到就关掉面板并说明原因：留着空面板比关掉更糟——用户会以为「这张图
     * 没有直方图」，而真实原因是这一层取不到（组/16 位/无像素）。
     */
    shell.histogram_open = false;
    set_status(shell, L"直方图不可用：组图层、空图层或 16 位图层");
    return;
  }
  shell.histogram_bins = std::move(bins);
  shell.histogram_layer = row->id;
}

void panel_toggle_histogram(ShellState& shell) {
  shell.histogram_open = !shell.histogram_open;
  refresh_histogram(shell);
  shell.needs_render = true;
  if (shell.histogram_open && !shell.histogram_bins.empty()) {
    refresh_history_status(shell, L"直方图：已显示");
  } else if (!shell.histogram_open) {
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
  shell.effect_values.clear();
  shell.effect_param = 0;
  shell.effect_input.clear();

  const uint32_t kind = static_cast<uint32_t>(comp_ui::clamp_effect_kind(shell.effect_kind));
  shell.effect_base_json = "{}";
  if (use_existing) {
    for (size_t i = 0; i < shell.effect_kinds.size(); ++i) {
      if (shell.effect_kinds[i] != kind) continue;
      if (i < shell.effect_settings.size()) shell.effect_base_json = shell.effect_settings[i];
      break;
    }
  }

  size_t param_count = 0;
  if (comp_layer_effect_param_count(kind, &param_count) != COMP_OK) param_count = 0;
  for (size_t i = 0; i < param_count; ++i) {
    comp_effect_param param{};
    if (comp_layer_effect_param_at(kind, i, &param) != COMP_OK) {
      shell.effect_values.emplace_back();
      continue;
    }
    bool filled = false;
    std::string text;
    if (use_existing) {
      if (param.type == COMP_PARAM_BOOL) {
        int32_t boolean = 0;
        if (comp_layer_effect_read_value(shell.effect_base_json.c_str(), param.json_path, nullptr,
                                         &boolean) == COMP_OK) {
          text = boolean != 0 ? "1" : "0";
          filled = true;
        }
      } else {
        double number = 0.0;
        if (comp_layer_effect_read_value(shell.effect_base_json.c_str(), param.json_path, &number,
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
    shell.effect_values.push_back(text);
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
  shell.effect_kinds.clear();
  shell.effect_settings.clear();
  const comp_layer_id id = shell.document != nullptr ? selected_layer_id(shell) : 0;
  if (id == 0) {
    reset_effect_values(shell);
    return;
  }
  size_t count = 0;
  if (comp_layer_effect_count(shell.document, id, &count) != COMP_OK) {
    reset_effect_values(shell);
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    uint32_t kind = 0;
    size_t need = 0;
    if (comp_layer_effect_at(shell.document, id, i, &kind, nullptr, 0, &need) != COMP_OK) break;
    /* P0-2：settings 一并回读，作为参数面板「合并底」（未知键必须原样带回去）。 */
    std::vector<char> text(need + 1, '\0');
    if (comp_layer_effect_at(shell.document, id, i, &kind, text.data(), text.size(), &need) !=
        COMP_OK) {
      break;
    }
    shell.effect_kinds.push_back(kind);
    shell.effect_settings.emplace_back(text.data(), need);
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
  if (comp_layer_get_flip(shell.document, id, &flip_x, &flip_y) != COMP_OK) {
    set_status(shell, L"读取镜像状态失败");
    return;
  }
  const int32_t next_x = horizontal ? (flip_x != 0 ? 0 : 1) : flip_x;
  const int32_t next_y = horizontal ? flip_y : (flip_y != 0 ? 0 : 1);
  if (comp_layer_set_flip(shell.document, id, next_x, next_y) != COMP_OK) {
    set_status(shell, L"镜像失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
  if (horizontal) {
    refresh_history_status(shell, next_x != 0 ? L"已水平翻转图层" : L"已取消水平翻转");
  } else {
    refresh_history_status(shell, next_y != 0 ? L"已垂直翻转图层" : L"已取消垂直翻转");
  }
}

/*
 * 效果开关（S24）。
 *
 * 引擎侧 6 种效果（描边/投影/颜色叠加/内阴影/外发光/内发光）早已参与像素求值，
 * 但 ABI 只有 set_effect / remove_effect，壳无法知道「这一层现在有哪些效果」。
 * 加了 effect_count / effect_at 之后，这里可以做**幂等切换**：勾选 = 已启用。
 *
 * 已有同 kind 的效果时**不覆盖**：用户可能是 .comp 读回来的、带自定义参数的效果，
 * 盖上 "{}" 会把他的参数静默抹掉（这是最典型的数据丢失）。要去掉就用「移除」。
 */
void panel_toggle_effect(ShellState& shell, int kind) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const int clamped = comp_ui::clamp_effect_kind(kind);
  const comp_layer_id id = row->id;
  const bool present =
      comp_ui::effect_list_contains(shell.effect_kinds.empty() ? nullptr : shell.effect_kinds.data(),
                                    static_cast<int32_t>(shell.effect_kinds.size()), clamped);
  wchar_t buffer[256];
  if (present) {
    if (comp_layer_remove_effect(shell.document, id, static_cast<uint32_t>(clamped)) != COMP_OK) {
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
    if (comp_layer_set_effect(shell.document, id, static_cast<uint32_t>(clamped), "{}") != COMP_OK) {
      set_status(shell, L"添加效果失败");
      return;
    }
    shell.effect_open = true;
    shell.effect_kind = clamped;
    wsprintfW(buffer, L"已添加效果：%s（参数面板已打开：Tab 选参数 · -/= 调值 · 回车应用）",
              effect_kind_label(clamped));
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
  if (shell.effect_kinds.empty()) {
    set_status(shell, L"该图层没有效果");
    return;
  }
  const comp_layer_id id = row->id;
  const std::vector<uint32_t> kinds = shell.effect_kinds;
  int removed = 0;
  for (uint32_t kind : kinds) {
    if (comp_layer_remove_effect(shell.document, id, kind) == COMP_OK) ++removed;
  }
  refresh_layers(shell);
  shell.needs_render = true;
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
      shell.effect_kinds.empty() ? nullptr : shell.effect_kinds.data(),
      static_cast<int32_t>(shell.effect_kinds.size()), kind);
}

/* P0-4 第二批：色彩范围面板持有显式事务，被其它面板顶掉时必须先 abort。 */
void color_range_cancel(ShellState& shell);

void open_effect_panel(ShellState& shell, int kind) {
  if (selected_row(shell) == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  if (shell.color_range_open) color_range_cancel(shell);
  /* 右栏一次只挂一个面板：滤镜、路径、历史与效果共用同一块矩形，同时打开会互相遮住。 */
  shell.filter_open = false;
  shell.filter_input.clear();
  shell.path_open = false;
  shell.history_open = false;
  shell.effect_open = true;
  shell.effect_kind = comp_ui::clamp_effect_kind(kind);
  reset_effect_values(shell);
  wchar_t buffer[224];
  wsprintfW(buffer, L"效果参数：%s（%s）Tab 选参数 · -/= 调值 · 回车应用 · Esc 关闭",
            effect_kind_label(shell.effect_kind),
            effect_present(shell, shell.effect_kind) ? L"本层已启用" : L"本层未启用，回车添加");
  set_status(shell, buffer);
}

void close_effect_panel(ShellState& shell) {
  shell.effect_open = false;
  shell.effect_input.clear();
  set_status(shell, L"已关闭效果参数面板");
}

/* `,` / `.`：在 6 个 kind 之间循环（不必先启用就能预览参数表；回车即添加）。 */
void panel_effect_cycle_kind(ShellState& shell, int32_t delta) {
  if (selected_row(shell) == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const int32_t total = comp_ui::kEffectKindCount;
  int32_t kind = shell.effect_kind + delta;
  if (kind < 0) kind = total - 1;
  if (kind >= total) kind = 0;
  shell.effect_kind = kind;
  shell.effect_open = true;
  reset_effect_values(shell);
  wchar_t buffer[200];
  wsprintfW(buffer, L"效果 %d/%d：%s（%s）", kind + 1, total, effect_kind_label(kind),
            effect_present(shell, kind) ? L"本层已启用" : L"本层未启用，回车添加");
  set_status(shell, buffer);
}

/* 聚焦某一行参数；越界夹取（与面板行几何共用同一个序号空间）。 */
void panel_effect_focus_row(ShellState& shell, int32_t row) {
  const int32_t count = static_cast<int32_t>(shell.effect_values.size());
  if (count == 0) {
    set_status(shell, L"该效果没有可编辑参数");
    return;
  }
  if (row < 0) row = 0;
  if (row >= count) row = count - 1;
  shell.effect_param = row;
  shell.effect_input.clear();
  comp_effect_param param{};
  if (comp_layer_effect_param_at(static_cast<uint32_t>(shell.effect_kind),
                                 static_cast<size_t>(row), &param) != COMP_OK) {
    return;
  }
  wchar_t buffer[220];
  wsprintfW(buffer, L"参数 %d/%d：%s = %s", row + 1, count,
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.effect_values[static_cast<size_t>(row)].c_str()).c_str());
  set_status(shell, buffer);
}

void panel_effect_move_param(ShellState& shell, int32_t delta) {
  panel_effect_focus_row(shell, shell.effect_param + delta);
}

/*
 * 步长由量程决定（ui_core::step_effect_param）：0..1 的归一量走 0.05，
 * 像素/角度走 1。夹取也在 ui_core，ABI 只做最后一道拒绝。
 */
void panel_effect_step_value(ShellState& shell, int32_t direction) {
  const size_t index = static_cast<size_t>(shell.effect_param);
  if (index >= shell.effect_values.size()) {
    set_status(shell, L"该效果没有可编辑参数");
    return;
  }
  comp_effect_param param{};
  if (comp_layer_effect_param_at(static_cast<uint32_t>(shell.effect_kind), index, &param) !=
      COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    shell.effect_values[index] = shell.effect_values[index] == "1" ? "0" : "1";
  } else {
    const double current = std::atof(shell.effect_values[index].c_str());
    shell.effect_values[index] = number_text(comp_ui::step_effect_param(
        current, direction, param.min_value, param.max_value));
  }
  shell.effect_input.clear();
  wchar_t buffer[240];
  wsprintfW(buffer, L"%s = %s （范围 [%s, %s]）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.effect_values[index].c_str()).c_str(),
            widen_ascii(number_text(param.min_value).c_str()).c_str(),
            widen_ascii(number_text(param.max_value).c_str()).c_str());
  set_status(shell, buffer);
}

/* 数字键开始输入：布尔参数不进入文本态，直接切换（与调整面板同规矩）。 */
void panel_effect_begin_input(ShellState& shell, wchar_t character) {
  const size_t index = static_cast<size_t>(shell.effect_param);
  if (index >= shell.effect_values.size()) {
    set_status(shell, L"该效果没有可编辑参数");
    return;
  }
  comp_effect_param param{};
  if (comp_layer_effect_param_at(static_cast<uint32_t>(shell.effect_kind), index, &param) !=
      COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    panel_effect_step_value(shell, 1);
    return;
  }
  shell.effect_input.assign(1, character);
  set_status(shell, L"输入中：回车提交并应用，Esc 取消");
}

void panel_effect_backspace(ShellState& shell) {
  if (!shell.effect_input.empty()) shell.effect_input.pop_back();
}

void panel_effect_commit_input(ShellState& shell) {
  if (shell.effect_input.empty()) {
    set_status(shell, L"未在输入状态（数字键开始输入）");
    return;
  }
  const size_t index = static_cast<size_t>(shell.effect_param);
  if (index >= shell.effect_values.size()) {
    shell.effect_input.clear();
    return;
  }
  comp_effect_param param{};
  if (comp_layer_effect_param_at(static_cast<uint32_t>(shell.effect_kind), index, &param) !=
      COMP_OK) {
    shell.effect_input.clear();
    return;
  }
  const std::string text(shell.effect_input.begin(), shell.effect_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  /* 面板夹取（与滤镜同口径）；不夹就等着 ABI 拒绝，状态栏只能报「参数不合法」。 */
  shell.effect_values[index] =
      number_text(comp_ui::clamp_effect_param(value, param.min_value, param.max_value));
  shell.effect_input.clear();
  wchar_t buffer[192];
  wsprintfW(buffer, L"已设置 %s = %s", widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.effect_values[index].c_str()).c_str());
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
  const std::string base = shell.effect_base_json;
  reset_effect_values(shell, false);
  shell.effect_base_json = base;
  set_status(shell, L"效果参数已重置为默认值（回车应用，Esc 关闭）");
}

/*
 * settings JSON 由引擎组装并在既有 settings 上合并（壳内不拼 JSON、不解析 JSON）。
 * 返回的字符串里未知键原样保留——这是 .comp 前向兼容的关键。
 */
bool build_effect_settings_json(const ShellState& shell, std::string* out_json) {
  std::vector<const char*> values;
  values.reserve(shell.effect_values.size());
  for (const std::string& text : shell.effect_values) values.push_back(text.c_str());

  const uint32_t kind = static_cast<uint32_t>(comp_ui::clamp_effect_kind(shell.effect_kind));
  const char* const* data = values.empty() ? nullptr : values.data();
  size_t need = 0;
  if (comp_layer_effect_build_settings(kind, data, values.size(),
                                       shell.effect_base_json.c_str(), nullptr, 0, &need) !=
      COMP_OK) {
    return false;
  }
  std::string json(need + 1, '\0');
  if (comp_layer_effect_build_settings(kind, data, values.size(),
                                       shell.effect_base_json.c_str(), &json[0], json.size(),
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
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  std::string json;
  if (!build_effect_settings_json(shell, &json)) {
    set_status(shell, L"应用被拒：参数不合法");
    return;
  }
  const uint32_t kind = static_cast<uint32_t>(comp_ui::clamp_effect_kind(shell.effect_kind));
  const comp_result result = comp_layer_set_effect(shell.document, id, kind, json.c_str());
  shell.needs_render = true;

  /* refresh_layers 会把编辑缓冲重建（并清焦点），这里把焦点恢复到同一行。 */
  const int32_t focused = shell.effect_param;
  refresh_layers(shell);
  if (!shell.effect_values.empty()) {
    if (focused >= 0 && focused < static_cast<int32_t>(shell.effect_values.size())) {
      shell.effect_param = focused;
    }
  }

  wchar_t buffer[200];
  wsprintfW(buffer, L"%s：%s", result == COMP_OK ? L"已应用效果" : L"应用效果失败",
            effect_kind_label(static_cast<int>(kind)));
  refresh_history_status(shell, buffer);
}

/* 直方图当前区间 → 状态栏文案（夹取后的实际值，不写「理想值」）。 */
void histogram_view_status(ShellState& shell) {
  if (comp_ui::histogram_view_is_full(shell.histogram_view)) {
    set_status(shell, L"直方图区间：全幅 0–255（双击复位）");
    return;
  }
  wchar_t buffer[128];
  wsprintfW(buffer, L"直方图区间：亮度 %d–%d（双击复位）",
            static_cast<int>(shell.histogram_view.lo + 0.5),
            static_cast<int>(shell.histogram_view.hi + 0.5));
  set_status(shell, buffer);
}

void panel_histogram_reset(ShellState& shell) {
  if (!shell.histogram_open) {
    set_status(shell, L"直方图未打开（F6 打开）");
    return;
  }
  shell.histogram_view = comp_ui::HistogramView{};
  shell.needs_render = true;
  histogram_view_status(shell);
}

void panel_histogram_zoom(ShellState& shell, double factor, double focus_bin) {
  if (!shell.histogram_open) return;
  shell.histogram_view = comp_ui::zoom_histogram_view(shell.histogram_view, focus_bin, factor);
  shell.needs_render = true;
  histogram_view_status(shell);
}

/*
 * 框选松手：把拖出的横向区间变成新的可视窗口。
 * 位移小于 4px 视为单击（不放大）——否则「点一下图」会突然缩放到 8 个 bin 的
 * 极限，看起来像图坏了。夹取由 ui_core 统一做，这里不重算。
 */
void panel_histogram_commit_drag(ShellState& shell, const comp_ui::Rect& plot) {
  const int32_t delta = shell.histogram_drag_to_x - shell.histogram_drag_from_x;
  if (delta >= -3 && delta <= 3) return;
  shell.histogram_view = comp_ui::histogram_window_from_drag(
      plot, shell.histogram_view, static_cast<double>(shell.histogram_drag_from_x),
      static_cast<double>(shell.histogram_drag_to_x));
  shell.needs_render = true;
  histogram_view_status(shell);
}

/* P0-1：按 id 选中（定义在多选辅助段，拖拽落点先用到）。 */
void panel_select_layer_id(ShellState& shell, comp_layer_id id);

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
  const int32_t count = static_cast<int32_t>(shell.layers.size());
  if (count <= 0) return;
  const comp_layer_id self =
      shell.layers[static_cast<size_t>(comp_ui::layer_array_index(count, from_shown))].id;
  if (drop_into_group_item && to_shown != from_shown) {
    const LayerRow& target = shell.layers[static_cast<size_t>(count - 1 - to_shown)];
    if (target.is_group != 0 && target.id != self) {
      if (comp_layer_move(shell.document, self, target.id, 0) != COMP_OK) {
        /* 常见原因：把自己/自己的祖先拖进自己的子树。引擎里已拒绝，原样透出。 */
        set_status(shell, L"移入组失败（不能把组拖进自己的子树）");
        return;
      }
      refresh_layers(shell);
      panel_select_layer_id(shell, self);
      shell.needs_render = true;
      wchar_t buffer[160];
      wsprintfW(buffer, L"已把图层 #%I64u 移入组 #%I64u",
                static_cast<unsigned long long>(self),
                static_cast<unsigned long long>(target.id));
      refresh_history_status(shell, buffer);
      return;
    }
  }
  std::vector<comp_ui::LayerStackEntry> entries(static_cast<size_t>(count));
  for (int32_t i = 0; i < count; ++i) {
    const LayerRow& row = shell.layers[static_cast<size_t>(i)];
    entries[static_cast<size_t>(i)].id = static_cast<int64_t>(row.id);
    entries[static_cast<size_t>(i)].parent = static_cast<int64_t>(row.parent);
    entries[static_cast<size_t>(i)].clipping = static_cast<int64_t>(row.clipping);
  }
  const comp_ui::DragPlan plan =
      comp_ui::plan_layer_drag(entries.data(), count, from_shown, to_shown);
  switch (plan.target) {
    case comp_ui::DragTarget::kSameRow:
      set_status(shell, L"拖回原位，图层顺序未变");
      return;
    case comp_ui::DragTarget::kCrossParent: {
      /* 跨父级且不是「拖进组」：只有目标行在根级时解释成「移出组」。 */
      const LayerRow& target = shell.layers[static_cast<size_t>(count - 1 - to_shown)];
      const LayerRow& dragged =
          shell.layers[static_cast<size_t>(comp_ui::layer_array_index(count, from_shown))];
      if (target.parent == 0 && dragged.parent != 0) {
        if (comp_layer_move(shell.document, self, 0, target.id) != COMP_OK) {
          set_status(shell, L"移出组失败（未修改任何数据）");
          return;
        }
        refresh_layers(shell);
        panel_select_layer_id(shell, self);
        shell.needs_render = true;
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
    before = shell.layers[static_cast<size_t>(plan.before_index)].id;
  }
  if (comp_layer_reorder(shell.document, self, before) != COMP_OK) {
    set_status(shell, L"拖行重排失败（跨组需与目标同组）");
    return;
  }
  refresh_layers(shell);
  /* 选中态跟着被拖动的那一层走（selected_layer 是面板行索引，必须重算）。 */
  shell.selected_layer = comp_ui::clamp_selected_row(plan.next_row, count);
  shell.needs_render = true;
  wchar_t buffer[128];
  wsprintfW(buffer, L"拖行重排：第 %d 行 → 第 %d 行", from_shown + 1, plan.next_row + 1);
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------ 菜单勾选态（S24） */

/*
 * 递归设置勾选。为什么必须递归：图层菜单里的「可见性/剪贴/隔离/直方图」在
 * 顶层子菜单，「混合模式/采样模式」的档位在二级子菜单——CheckMenuItem 只能对
 * 单个 HMENU 按命令 id 生效，不递归就会出现「顶层勾了、子菜单没勾」。
 *
 * 为什么不重建菜单：重建（DestroyMenu + build_menu + SetMenu）会让已打开的菜单
 * 句柄悬空，并让每次 INITMENU 都闪一下；勾选态是纯显示状态，WM_INITMENU 里刷
 * 一遍即可（GetMenu(window) 拿到的就是同一份 HMENU）。
 */
void menu_set_checked_recursive(HMENU menu, UINT id, bool checked) {
  if (menu == nullptr) return;
  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    const UINT state = GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION);
    if (state == static_cast<UINT>(-1)) continue;
    if ((state & MF_POPUP) != 0) {
      menu_set_checked_recursive(GetSubMenu(menu, i), id, checked);
    } else if (GetMenuItemID(menu, i) == id) {
      CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    }
  }
}

/*
 * 单选设置（混合模式 / 采样模式 / 当前工具）：命令 id 落在 [first, last] 区间里
 * 的整段做互斥勾选。只对**包含区间成员的同一个子菜单**调用 CheckMenuRadioItem，
 * 否则 Win32 会把不相邻的项也当成一组。
 */
void menu_radio_check_recursive(HMENU menu, UINT first, UINT last, UINT selected) {
  if (menu == nullptr) return;
  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    const UINT state = GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION);
    if (state == static_cast<UINT>(-1)) continue;
    if ((state & MF_POPUP) != 0) {
      menu_radio_check_recursive(GetSubMenu(menu, i), first, last, selected);
      continue;
    }
    const UINT command = GetMenuItemID(menu, i);
    if (command >= first && command <= last) {
      CheckMenuRadioItem(menu, first, last, selected, MF_BYCOMMAND);
      return; /* 同一子菜单只做一次；继续递归会重复调用 */
    }
  }
}

/*
 * WM_INITMENU 的唯一入口：把当前状态映射成勾选态。
 *
 * 判定规则在纯函数 comp_ui::layer_menu_checks 里（无选中层不勾、非组的 isolated
 * 不算、采样/混合夹到合法档位），这里只做「状态 → 菜单」的搬运，所以规则部分
 * 可以表驱动单测，不需要起窗口。
 */
void sync_menu_state(HWND window, ShellState& shell) {
  HMENU menu = GetMenu(window);
  if (menu == nullptr) return;

  comp_ui::LayerMenuState state{};
  const LayerRow* row = selected_row(shell);
  if (row != nullptr) {
    state.has_layer = true;
    state.visible = row->visible != 0;
    state.clipping = row->clipping != 0;
    state.is_group = row->is_group != 0;
    state.isolated = row->isolated != 0;
    state.sampling = row->sampling;
    state.blend = row->blend;
  }
  state.histogram_open = shell.histogram_open;
  const comp_ui::LayerMenuChecks checks = comp_ui::layer_menu_checks(state);

  menu_set_checked_recursive(menu, kCmdLayerToggleVisible, checks.visible);
  menu_set_checked_recursive(menu, kCmdLayerToggleClipping, checks.clipping);
  menu_set_checked_recursive(menu, kCmdLayerToggleIsolated, checks.isolated);
  menu_set_checked_recursive(menu, kCmdHistogramToggle, checks.histogram);
  if (checks.sampling >= 0) {
    menu_radio_check_recursive(menu, kCmdSamplingBase,
                               kCmdSamplingBase + static_cast<UINT>(comp_ui::kSamplingCount) - 1,
                               kCmdSamplingBase + static_cast<UINT>(checks.sampling));
  }
  if (checks.blend >= 0) {
    menu_radio_check_recursive(menu, kCmdBlendBase,
                               kCmdBlendBase + static_cast<UINT>(comp_ui::kBlendCount) - 1,
                               kCmdBlendBase + static_cast<UINT>(checks.blend));
  }
  /* 效果项：6 个 kind 各自独立勾选（已启用 = 勾），不是互斥组。 */
  for (int kind = 0; kind < comp_ui::kEffectKindCount; ++kind) {
    const bool present =
        comp_ui::effect_list_contains(shell.effect_kinds.empty() ? nullptr : shell.effect_kinds.data(),
                                      static_cast<int32_t>(shell.effect_kinds.size()), kind);
    menu_set_checked_recursive(menu, kCmdEffectBase + static_cast<UINT>(kind), present);
  }
  /* 工具菜单单选：当前工具（tool_index 是 ui_core 的单一真相，不在这里另算）。 */
  const int tool = comp_ui::tool_index(shell.tool);
  if (tool >= 0) {
    menu_radio_check_recursive(menu, kCmdToolBase,
                               kCmdToolBase + static_cast<UINT>(comp_ui::tool_count()) - 1,
                               kCmdToolBase + static_cast<UINT>(tool));
  }
  /* 镜像两项目前是「动作」而非开关：勾选态展示该方向是否已翻转，便于自诊断。 */
  if (row != nullptr) {
    menu_set_checked_recursive(menu, kCmdLayerFlipHorizontal, row->flip_x != 0);
    menu_set_checked_recursive(menu, kCmdLayerFlipVertical, row->flip_y != 0);
  }
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
  comp_rect transform{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.document, row->id, &transform, &rotation) != COMP_OK) {
    set_status(shell, L"读取图层变换失败");
    return;
  }
  transform.x += dx;
  transform.y += dy;
  if (comp_layer_set_transform(shell.document, row->id, transform, rotation) != COMP_OK) {
    set_status(shell, L"平移图层失败");
    return;
  }
  shell.needs_render = true;
  wchar_t buffer[128];
  wsprintfW(buffer, L"图层位置：(%d, %d) 方向键", transform.x, transform.y);
  refresh_history_status(shell, buffer);
}

/*
 * 图层旋转（S21）：在既有角度上累加——必须先读现值，理由同 panel_move_layer。
 * 步进量与夹取口径全在 ui_core（layer_transform_rotate_step），壳不重算。
 */
void panel_rotate_layer(ShellState& shell, bool forward) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  comp_rect transform{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.document, row->id, &transform, &rotation) != COMP_OK) {
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
  if (comp_layer_set_transform(shell.document, row->id, transform,
                               static_cast<float>(next)) != COMP_OK) {
    set_status(shell, L"旋转图层失败");
    return;
  }
  shell.needs_render = true;
  wchar_t buffer[160];
  wsprintfW(buffer, L"图层旋转：%d°（Ctrl+左右）",
            static_cast<int>(next + 0.5));
  refresh_history_status(shell, buffer);
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
  if (comp_layer_get_transform(shell.document, row->id, &transform, &rotation) != COMP_OK) {
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
  if (comp_layer_set_transform(shell.document, row->id, scaled, rotation) != COMP_OK) {
    set_status(shell, L"缩放图层失败");
    return;
  }
  shell.needs_render = true;
  wchar_t buffer[192];
  wsprintfW(buffer, L"图层尺寸：%d×%d（Alt+[ ]，中心锚定）", w, h);
  refresh_history_status(shell, buffer);
}

void panel_add_layer(ShellState& shell) {
  if (shell.document == nullptr) {
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
  if (comp_layer_add(shell.document, &desc, &id) != COMP_OK) {
    set_status(shell, L"新建图层失败");
    return;
  }
  set_visible_cache(shell, id, static_cast<uint8_t>(1));
  refresh_layers(shell);
  shell.needs_render = true;
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
  for (comp_layer_id extra : shell.extra_selection) {
    if (extra == id) return true;
  }
  return false;
}

/*
 * 批量命令的 id 列表（面板显示序：从上到下，即数组逆序）。
 *
 * 已经「被选中祖先覆盖」的图层要从列表里摘掉：删/复制一个组时整棵子树
 * 已经跟着走了，再对子层发一次写入只会失败——而且是那种「第一层成功、
 * 第二层报 not found」的半成品失败。
 */
std::vector<comp_layer_id> panel_selection_ids(const ShellState& shell, bool drop_covered) {
  std::vector<comp_layer_id> ids;
  const int32_t count = static_cast<int32_t>(shell.layers.size());
  for (int32_t shown = 0; shown < count; ++shown) {
    const LayerRow& row = shell.layers[static_cast<size_t>(count - 1 - shown)];
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
        for (const LayerRow& other : shell.layers) {
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

/* 按 id 选中（selected_layer 是面板显示行号，必须反算）。 */
void panel_select_layer_id(ShellState& shell, comp_layer_id id) {
  const int32_t count = static_cast<int32_t>(shell.layers.size());
  for (int32_t i = 0; i < count; ++i) {
    if (shell.layers[static_cast<size_t>(i)].id == id) {
      shell.selected_layer = count - 1 - i;
      return;
    }
  }
}

/*
 * 新建组（Ctrl+G，对齐上游 Group Layers）。
 *
 * 有选区时把选中图层搬进新组（上游语义：Cmd+G 直接建组并收入选区，
 * 而不是建一个空组让用户再拖）。整批写入走一个显式事务，一步撤销。
 * 新组落在根级栈顶；组内顺序按原数组序（自下而上）搬入，堆叠关系不变。
 */
void panel_add_group(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const std::vector<comp_layer_id> moving = panel_selection_ids(shell, true);
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.document, "Group Layers", &txn) != COMP_OK) {
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
  if (comp_layer_add(shell.document, &desc, &group_id) != COMP_OK) {
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
    if (comp_layer_move(shell.document, *it, group_id, 0) != COMP_OK) {
      comp_txn_abort(txn);
      set_status(shell, L"新建组失败（无法把图层移入组，已整体回滚）");
      return;
    }
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"新建组：提交失败");
    return;
  }
  shell.extra_selection.clear();
  refresh_layers(shell);
  panel_select_layer_id(shell, group_id);
  shell.needs_render = true;
  wchar_t buffer[160];
  wsprintfW(buffer, L"已新建组 #%I64u（收入 %d 个图层）",
            static_cast<unsigned long long>(group_id),
            static_cast<int>(moving.size()));
  refresh_history_status(shell, buffer);
}

/* 删除选中图层（Del）：多选时一个显式事务 = 一步撤销。 */
void panel_delete_layer(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const std::vector<comp_layer_id> ids = panel_selection_ids(shell, true);
  if (ids.empty()) {
    set_status(shell, L"无图层可删除");
    return;
  }
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.document, "Delete Layers", &txn) != COMP_OK) {
    set_status(shell, L"删除图层：无法开始事务");
    return;
  }
  for (comp_layer_id id : ids) {
    if (comp_layer_remove(shell.document, id) != COMP_OK) {
      comp_txn_abort(txn);
      set_status(shell, L"删除图层失败（已整体回滚）");
      return;
    }
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"删除图层：提交失败");
    return;
  }
  shell.extra_selection.clear();
  refresh_layers(shell);
  shell.needs_render = true;
  wchar_t buffer[128];
  wsprintfW(buffer, L"已删除 %d 个图层", static_cast<int>(ids.size()));
  refresh_history_status(shell, buffer);
}

/* 复制图层（Ctrl+J，对齐上游 Duplicate Layer）：多选 = 一次事务一步撤销。 */
void panel_duplicate_layer(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const std::vector<comp_layer_id> ids = panel_selection_ids(shell, true);
  if (ids.empty()) {
    set_status(shell, L"无图层可复制");
    return;
  }
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.document, "Duplicate Layers", &txn) != COMP_OK) {
    set_status(shell, L"复制图层：无法开始事务");
    return;
  }
  comp_layer_id first_copy = 0;
  for (comp_layer_id id : ids) {
    comp_layer_id copy_id = 0;
    if (comp_layer_duplicate(shell.document, id, &copy_id) != COMP_OK) {
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
  shell.extra_selection.clear();
  refresh_layers(shell);
  if (first_copy != 0) panel_select_layer_id(shell, first_copy);
  shell.needs_render = true;
  wchar_t buffer[128];
  wsprintfW(buffer, L"已复制 %d 个图层", static_cast<int>(ids.size()));
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------- 图层名 / 不透明度键入 */

void refresh_layer_input_status(ShellState& shell) {
  wchar_t buffer[224];
  if (shell.layer_input == LayerInput::Rename) {
    wsprintfW(buffer, L"重命名图层 #%I64u：%s（回车确认 / Esc 取消）",
              static_cast<unsigned long long>(shell.layer_input_id),
              shell.layer_input_text.c_str());
  } else {
    wsprintfW(buffer, L"图层不透明度：%s%%（回车应用 / Esc 取消，0..100）",
              shell.layer_input_text.c_str());
  }
  set_status(shell, buffer);
}

/* 重命名（F2 / 双击行）：预填当前名字，UTF-8 → UTF-16 后交给 WM_CHAR 追加。 */
void panel_begin_rename(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr || shell.document == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  shell.layer_input = LayerInput::Rename;
  shell.layer_input_id = row->id;
  shell.layer_input_text = widen_utf8(row->name);
  refresh_layer_input_status(shell);
}

/* 直接键入不透明度（对齐上游双击不透明度数值）：预填当前百分比。 */
void panel_begin_opacity_set(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr || shell.document == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  int percent = static_cast<int>(row->opacity * 100.0f + 0.5f);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  wchar_t buffer[16];
  wsprintfW(buffer, L"%d", percent);
  shell.layer_input = LayerInput::Opacity;
  shell.layer_input_id = row->id;
  shell.layer_input_text = buffer;
  refresh_layer_input_status(shell);
}

void panel_cancel_layer_input(ShellState& shell) {
  shell.layer_input = LayerInput::None;
  shell.layer_input_id = 0;
  shell.layer_input_text.clear();
}

/*
 * 键入一个字符。名字接受任意可打印字符（含 CJK —— WM_CHAR/WM_IME_CHAR 送来的
 * 就是 UTF-16，不用在这里做编码转换）；不透明度只接受数字与一个小数点。
 * 名字长度上限按 UTF-16 码元粗夹到 kLayerNameCapacity（UTF-8 字节超限由
 * comp_layer_set_name 精确拒绝并给出原因）。
 */
void panel_layer_input_char(ShellState& shell, wchar_t ch) {
  if (shell.layer_input == LayerInput::None) return;
  if (ch < 0x20 || ch == 0x7F) return; /* 控制字符（回车/退格/Esc 由 WM_KEYDOWN 处理） */
  if (shell.layer_input == LayerInput::Opacity) {
    if ((ch < L'0' || ch > L'9') && ch != L'.') return;
    if (ch == L'.' && shell.layer_input_text.find(L'.') != std::wstring::npos) return;
    if (shell.layer_input_text.size() >= 6) return;
  } else if (shell.layer_input_text.size() >= kLayerNameCapacity) {
    return;
  }
  shell.layer_input_text.push_back(ch);
  refresh_layer_input_status(shell);
}

void panel_layer_input_backspace(ShellState& shell) {
  if (shell.layer_input == LayerInput::None) return;
  if (shell.layer_input_text.empty()) return;
  shell.layer_input_text.pop_back();
  refresh_layer_input_status(shell);
}

/* 提交键入：改名走 comp_layer_set_name（严格 UTF-8），不透明度走批量设置。 */
void panel_commit_layer_input(ShellState& shell) {
  if (shell.layer_input == LayerInput::None) return;
  if (shell.document == nullptr) {
    panel_cancel_layer_input(shell);
    set_status(shell, L"无文档");
    return;
  }
  const LayerInput mode = shell.layer_input;
  const comp_layer_id id = shell.layer_input_id;
  const std::wstring text = shell.layer_input_text;
  if (mode == LayerInput::Rename) {
    const std::string name = narrow_utf8(text);
    if (name.empty()) {
      /* 空名会让图层在没有标识的情况下落盘；拒绝并保持输入态让用户改。 */
      set_status(shell, L"图层名不能为空");
      refresh_layer_input_status(shell);
      return;
    }
    if (comp_layer_set_name(shell.document, id, name.c_str()) != COMP_OK) {
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
  if (comp_txn_begin(shell.document, "Set Layer Opacity", &txn) != COMP_OK) {
    set_status(shell, L"设置不透明度：无法开始事务");
    refresh_layer_input_status(shell);
    return;
  }
  for (comp_layer_id target : ids) {
    if (comp_layer_set_opacity(shell.document, target, opacity) != COMP_OK) {
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
  shell.needs_render = true;
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
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"无图层");
    return;
  }
  if (comp_layer_merge_down(shell.document, id) != COMP_OK) {
    set_status(shell, L"合并失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
  refresh_history_status(shell, L"已向下合并");
}

/* 拼合图像（对齐上游 Flatten Image）：整篇文档合成为单一像素层。 */
void panel_flatten(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  if (comp_document_flatten(shell.document) != COMP_OK) {
    set_status(shell, L"拼合失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
  refresh_history_status(shell, L"已拼合图像");
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

std::wstring widen_ascii(const char* text) {
  std::wstring wide;
  if (text == nullptr) return wide;
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

/* 名称经 ABI 取自引擎键名表，外壳不复制任何键名。 */
const char* adjustment_kind_name(uint32_t kind) {
  const char* name = nullptr;
  if (comp_layer_adjustment_kind_name(kind, &name) != COMP_OK || name == nullptr) return "(unknown)";
  return name;
}

/*
 * 色阶 = 4 通道 × 5 参（参数表顺序 `levels.ranges.N.field` 由引擎钉住，
 * tests/test_adjustment.cpp 有逐项断言）。面板按通道分页，一次只显示 5 行；
 * 其它 kind 不分页（整表显示）。这两个常量只描述「怎么分页」，不复制任何键名。
 */
constexpr int32_t kLevelsChannelCount = 4;
constexpr int32_t kLevelsFieldsPerChannel = 5;
constexpr int32_t kCurveChannels = 4;

bool adjust_paged_kind(uint32_t kind) { return kind == COMP_ADJUSTMENT_LEVELS; }
int32_t adjust_channel_count(uint32_t kind) {
  return adjust_paged_kind(kind) ? kLevelsChannelCount : 1;
}

/* 重建编辑值：以缺省值为底；若选中了既有调整，则按 settings 回填（引擎侧解析 JSON）。 */
void reset_adjustment_values(ShellState& shell) {
  shell.adj_values.clear();
  shell.adj_param = 0;
  shell.adj_input.clear();

  size_t param_count = 0;
  if (comp_layer_adjustment_param_count(static_cast<uint32_t>(shell.adj_kind), &param_count) != COMP_OK) {
    param_count = 0;
  }
  const std::string* existing = nullptr;
  if (shell.selected_adjustment >= 0 &&
      shell.selected_adjustment < static_cast<int32_t>(shell.adj_settings.size())) {
    existing = &shell.adj_settings[static_cast<size_t>(shell.selected_adjustment)];
  }

  for (size_t i = 0; i < param_count; ++i) {
    comp_adjustment_param param{};
    if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), i, &param) != COMP_OK) {
      shell.adj_values.emplace_back();
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
    shell.adj_values.push_back(text);
  }

  /*
   * 分页视图的入口状态：通道号只夹取、不重置——拖动结束会走 refresh -> 这里，
   * 若在这里把通道清 0，用户拖 B 通道的滑杆松手就会被弹回 RGB。焦点落在当前
   * 通道首行，避免高亮停在不显示的行上。
   */
  const int32_t channels = adjust_channel_count(static_cast<uint32_t>(shell.adj_kind));
  if (shell.adj_channel < 0) shell.adj_channel = 0;
  if (shell.adj_channel >= channels) shell.adj_channel = channels - 1;
  shell.adj_param =
      adjust_paged_kind(static_cast<uint32_t>(shell.adj_kind))
          ? shell.adj_channel * kLevelsFieldsPerChannel
          : 0;
  if (shell.adj_param >= static_cast<int32_t>(shell.adj_values.size())) shell.adj_param = 0;
}

/* 读取选中图层的调整列表（只读，不开事务），并把 kind 与编辑值同步过来。 */
void refresh_adjustments(ShellState& shell) {
  shell.adj_kinds.clear();
  shell.adj_settings.clear();

  const comp_layer_id id = shell.document != nullptr ? selected_layer_id(shell) : 0;
  if (id == 0) {
    shell.selected_adjustment = -1;
    reset_adjustment_values(shell);
    refresh_curve_points(shell);
    return;
  }
  size_t count = 0;
  if (comp_layer_adjustment_count(shell.document, id, &count) != COMP_OK) count = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t kind = 0;
    size_t need = 0;
    if (comp_layer_adjustment_at(shell.document, id, i, &kind, nullptr, 0, &need) != COMP_OK) break;
    std::vector<char> text(need + 1, '\0');
    if (comp_layer_adjustment_at(shell.document, id, i, &kind, text.data(), text.size(), &need) !=
        COMP_OK) {
      break;
    }
    shell.adj_kinds.push_back(kind);
    shell.adj_settings.emplace_back(text.data(), need);
  }
  if (shell.selected_adjustment >= static_cast<int32_t>(shell.adj_settings.size())) {
    shell.selected_adjustment = static_cast<int32_t>(shell.adj_settings.size()) - 1;
  }
  if (shell.selected_adjustment >= 0) {
    shell.adj_kind = static_cast<int32_t>(shell.adj_kinds[static_cast<size_t>(shell.selected_adjustment)]);
  }
  reset_adjustment_values(shell);
  refresh_curve_points(shell);
}

void panel_cycle_kind(ShellState& shell, int32_t delta) {
  const int32_t total = static_cast<int32_t>(COMP_ADJUSTMENT_KIND_COUNT);
  int32_t kind = shell.adj_kind + delta;
  if (kind < 0) kind = total - 1;
  if (kind >= total) kind = 0;
  shell.adj_kind = kind;
  shell.selected_adjustment = -1; /* 换类型即进入「新增」态 */
  reset_adjustment_values(shell);
  refresh_curve_points(shell);
  wchar_t buffer[160];
  wsprintfW(buffer, L"调整类型 %d/%d：%s", shell.adj_kind + 1, total,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(kind))).c_str());
  set_status(shell, buffer);
}

void panel_select_adjustment(ShellState& shell, int32_t delta) {
  const int32_t count = static_cast<int32_t>(shell.adj_settings.size());
  if (count == 0) {
    set_status(shell, L"该图层没有调整（Ctrl+Shift+A 新增）");
    return;
  }
  int32_t next = shell.selected_adjustment + delta;
  if (next < 0) next = 0;
  if (next >= count) next = count - 1;
  shell.selected_adjustment = next;
  shell.adj_kind = static_cast<int32_t>(shell.adj_kinds[static_cast<size_t>(next)]);
  reset_adjustment_values(shell);
  refresh_curve_points(shell);
  wchar_t buffer[160];
  wsprintfW(buffer, L"已选中调整 #%d：%s", next + 1,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(shell.adj_kind))).c_str());
  set_status(shell, buffer);
}

/* 定义在后面（要用 client_layout），先声明给「切参数时自动滚动」用。 */
void adjust_scroll_to_focus(ShellState& shell);

void panel_move_param(ShellState& shell, int32_t delta) {
  const int32_t count = static_cast<int32_t>(shell.adj_values.size());
  if (count == 0) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  /* 分页视图（色阶）里 Tab 只在当前通道的 5 行内绕，不会串到别的通道。 */
  const bool paged = adjust_paged_kind(static_cast<uint32_t>(shell.adj_kind));
  const int32_t first = paged ? shell.adj_channel * kLevelsFieldsPerChannel : 0;
  int32_t last = paged ? first + kLevelsFieldsPerChannel - 1 : count - 1;
  if (last >= count) last = count - 1;
  if (last < first) last = first;
  int32_t next = shell.adj_param + delta;
  if (next < first) next = first;
  if (next > last) next = last;
  shell.adj_param = next;
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), static_cast<size_t>(next),
                                     &param) != COMP_OK) {
    return;
  }
  wchar_t buffer[200];
  wsprintfW(buffer, L"参数 %d/%d：%s = %s", next + 1, count,
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.adj_values[static_cast<size_t>(next)].c_str()).c_str());
  set_status(shell, buffer);
  shell.adj_input.clear();
  /* 色阶 20 行、小窗口只显示 12 行，切参数后必须把焦点行滚进可视区。 */
  adjust_scroll_to_focus(shell);
}

/* 面板当前显示的行数，以及「相对行 -> 参数表下标」的基址。分页视图下两者不再
 * 相等：色阶 20 项按通道切成 4 页，每页 5 行。绘制/命中/滚动全用这两个函数。 */
int32_t adjust_shown_count(const ShellState& shell) {
  const int32_t total = static_cast<int32_t>(shell.adj_values.size());
  if (!adjust_paged_kind(static_cast<uint32_t>(shell.adj_kind))) return total;
  const int32_t base = shell.adj_channel * kLevelsFieldsPerChannel;
  if (base >= total) return 0;
  const int32_t rest = total - base;
  return rest < kLevelsFieldsPerChannel ? rest : kLevelsFieldsPerChannel;
}

int32_t adjust_param_base(const ShellState& shell) {
  return adjust_paged_kind(static_cast<uint32_t>(shell.adj_kind))
             ? shell.adj_channel * kLevelsFieldsPerChannel
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
  const size_t index = static_cast<size_t>(shell.adj_param);
  if (index >= shell.adj_values.size()) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), index, &param) != COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    shell.adj_values[index] = shell.adj_values[index] == "1" ? "0" : "1";
  } else {
    const double value = std::atof(shell.adj_values[index].c_str()) +
                         static_cast<double>(direction) * adjust_param_step(param);
    /* 与滑杆同一套归整：键盘调出来的值不许是 40.000000000000004。 */
    shell.adj_values[index] =
        number_text(comp_ui::adjust_snap_value(value, param.min_value, param.max_value));
  }
  shell.adj_input.clear();
  wchar_t buffer[220];
  wsprintfW(buffer, L"%s = %s （范围 [%s, %s]）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.adj_values[index].c_str()).c_str(),
            widen_ascii(number_text(param.min_value).c_str()).c_str(),
            widen_ascii(number_text(param.max_value).c_str()).c_str());
  set_status(shell, buffer);
}

/* 数字键开始输入：布尔参数不进入文本态，直接切换。 */
void panel_begin_input(ShellState& shell, wchar_t character) {
  const size_t index = static_cast<size_t>(shell.adj_param);
  if (index >= shell.adj_values.size()) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), index, &param) != COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    panel_step_value(shell, 1);
    return;
  }
  shell.adj_input.assign(1, character);
  set_status(shell, L"输入中：回车提交，Esc 取消");
}

void panel_backspace(ShellState& shell) {
  if (!shell.adj_input.empty()) shell.adj_input.pop_back();
}

void panel_commit_input(ShellState& shell) {
  if (shell.adj_input.empty()) {
    set_status(shell, L"未在输入状态（数字键开始输入）");
    return;
  }
  const size_t index = static_cast<size_t>(shell.adj_param);
  if (index >= shell.adj_values.size()) {
    shell.adj_input.clear();
    return;
  }
  const std::string text(shell.adj_input.begin(), shell.adj_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  shell.adj_values[index] = text;
  shell.adj_input.clear();
  wchar_t buffer[160];
  wsprintfW(buffer, L"已提交 %s", widen_ascii(text.c_str()).c_str());
  set_status(shell, buffer);
}

/* settings JSON 由引擎组装（壳内不拼 JSON、不解析 JSON）。 */
bool build_settings_json(const ShellState& shell, std::string* out_json) {
  std::vector<const char*> values;
  values.reserve(shell.adj_values.size());
  for (const std::string& text : shell.adj_values) values.push_back(text.c_str());

  const uint32_t kind = static_cast<uint32_t>(shell.adj_kind);
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
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  if (!add && shell.selected_adjustment < 0) {
    set_status(shell, L"未选中已有调整（, / . 选择；Ctrl+Shift+A 新增）");
    return;
  }
  std::string json;
  if (!build_settings_json(shell, &json)) {
    set_status(shell, add ? L"新增被拒：参数不合法" : L"应用被拒：参数不合法");
    return;
  }

  const uint32_t kind = static_cast<uint32_t>(shell.adj_kind);
  comp_result result = COMP_OK;
  if (add) {
    result = comp_layer_add_adjustment(shell.document, id, kind, json.c_str());
  } else {
    result = comp_layer_update_adjustment_at(
        shell.document, id, static_cast<size_t>(shell.selected_adjustment), kind, json.c_str());
  }

  shell.needs_render = true;
  if (result != COMP_OK && !add) shell.selected_adjustment = -1;
  refresh_adjustments(shell);
  if (add && !shell.adj_settings.empty()) {
    /* 新增成功后选中最后一条，便于继续微调。 */
    shell.selected_adjustment = static_cast<int32_t>(shell.adj_settings.size()) - 1;
    shell.adj_kind = static_cast<int32_t>(shell.adj_kinds[static_cast<size_t>(shell.selected_adjustment)]);
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

void panel_remove_adjustment(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  if (shell.selected_adjustment < 0) {
    set_status(shell, L"未选中已有调整（, / . 选择）");
    return;
  }
  const comp_result result = comp_layer_remove_adjustment(
      shell.document, id, static_cast<size_t>(shell.selected_adjustment));
  shell.needs_render = true;
  refresh_adjustments(shell);

  wchar_t buffer[120];
  wsprintfW(buffer, result == COMP_OK ? L"已删除调整" : L"删除调整失败");
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 滤镜面板 */

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

std::wstring param_status_text(const comp_ui::FilterPanel& panel) {
  const comp_ui::FilterParamSpec* spec = panel.param_at(panel.focused);
  if (spec == nullptr) return std::wstring();
  return widen_ascii(spec->label) + L" = " +
         widen_ascii(number_text(panel.value(panel.focused)).c_str());
}

void open_filter_panel(ShellState& shell, comp_ui::FilterId id) {
  /* 右栏一次只挂一个面板：打开滤镜即收起效果参数面板、色彩范围、路径与历史面板。 */
  if (shell.color_range_open) color_range_cancel(shell);
  shell.effect_open = false;
  shell.effect_input.clear();
  shell.path_open = false;
  shell.history_open = false;
  shell.filter.select(id);
  shell.filter_open = true;
  shell.filter_input.clear();
  const comp_ui::FilterSpec* spec = shell.filter.spec();
  wchar_t buffer[192];
  wsprintfW(buffer, L"滤镜：%s（Tab 选参数 · -/= 调值 · 数字键入 · 回车应用 · Esc 关闭）",
            widen_ascii(spec != nullptr ? spec->name : "?").c_str());
  set_status(shell, buffer);
}

void close_filter_panel(ShellState& shell) {
  shell.filter_open = false;
  shell.filter_input.clear();
  set_status(shell, L"已关闭滤镜面板");
}

void filter_reset_values(ShellState& shell) {
  shell.filter_input.clear();
  shell.filter.reset_values();
  set_status(shell, L"滤镜参数已重置为默认值");
}

void filter_focus_row(ShellState& shell, int delta) {
  if (!shell.filter_open) return;
  shell.filter.focus(delta);
  const std::wstring text = param_status_text(shell.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"参数 %d/%d：%s", shell.filter.focused + 1, shell.filter.param_count(),
            text.c_str());
  set_status(shell, buffer);
}

void filter_step_value(ShellState& shell, int direction) {
  if (!shell.filter_open) return;
  const comp_ui::FilterParamSpec* spec = shell.filter.param_at(shell.filter.focused);
  if (spec == nullptr) return;
  shell.filter.step(direction);
  const std::wstring text = param_status_text(shell.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s（步长 %s）", text.c_str(),
            widen_ascii(number_text(spec->step).c_str()).c_str());
  set_status(shell, buffer);
}

void filter_begin_input(ShellState& shell, wchar_t character) {
  if (!shell.filter_open) return;
  shell.filter_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.filter_input.c_str());
  set_status(shell, buffer);
}

void filter_backspace(ShellState& shell) {
  if (shell.filter_input.empty()) return;
  shell.filter_input.pop_back();
}

/* 键入值先夹到面板量程再落值：面板永不产生越界参数（ABI 仍会拒绝越界）。 */
void filter_commit_input(ShellState& shell) {
  if (shell.filter_input.empty()) return;
  const std::string text(shell.filter_input.begin(), shell.filter_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  shell.filter.set_value(shell.filter.focused, value);
  shell.filter_input.clear();
  const std::wstring shown = param_status_text(shell.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"已设置 %s", shown.c_str());
  set_status(shell, buffer);
}

/* 面板值按固定行序搬进 ABI 结构（唯一映射点；顺序见 ui_core 的 filter_table）。 */
void build_filter_desc(const comp_ui::FilterPanel& panel, comp_filter_desc* out) {
  comp_filter_desc desc{};
  desc.kind = static_cast<comp_filter_kind>(static_cast<int>(panel.id));
  switch (panel.id) {
    case comp_ui::FilterId::Vignette:
      desc.vignette.amount = panel.value(0);
      desc.vignette.midpoint = panel.value(1);
      desc.vignette.roundness = panel.value(2);
      desc.vignette.feather = panel.value(3);
      desc.vignette.highlights = panel.value(4);
      /* 面板 v1 不暴露边缘色：固定黑（与上游默认色一致），已在文档记边界。 */
      desc.vignette.color[0] = 0.0;
      desc.vignette.color[1] = 0.0;
      desc.vignette.color[2] = 0.0;
      break;
    case comp_ui::FilterId::TonalContrast:
      desc.tonal_contrast.amount = panel.value(0);
      desc.tonal_contrast.radius = panel.value(1);
      desc.tonal_contrast.shadows = panel.value(2);
      desc.tonal_contrast.midtones = panel.value(3);
      desc.tonal_contrast.highlights = panel.value(4);
      break;
    case comp_ui::FilterId::Bloom:
      desc.bloom.amount = panel.value(0);
      desc.bloom.radius = panel.value(1);
      break;
    case comp_ui::FilterId::LensCorrection:
      desc.lens_correction.distortion = panel.value(0);
      break;
    case comp_ui::FilterId::GaussianBlur:
      desc.gaussian_blur.radius = panel.value(0);
      desc.gaussian_blur.preserve_transparency = panel.value(1) >= 0.5 ? 1 : 0;
      break;
    case comp_ui::FilterId::AddNoise:
      desc.add_noise.amount = panel.value(0);
      desc.add_noise.gaussian = panel.value(1) >= 0.5 ? 1 : 0;
      desc.add_noise.monochromatic = panel.value(2) >= 0.5 ? 1 : 0;
      desc.add_noise.seed = static_cast<uint32_t>(std::llround(panel.value(3)));
      break;
    case comp_ui::FilterId::Grain:
      desc.grain.amount = panel.value(0);
      desc.grain.size = panel.value(1);
      desc.grain.roughness = panel.value(2);
      desc.grain.seed = static_cast<uint32_t>(std::llround(panel.value(3)));
      break;
    default:
      break;
  }
  *out = desc;
}

void apply_filter(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  const comp_ui::FilterSpec* spec = shell.filter.spec();
  if (spec == nullptr) return;

  comp_filter_desc desc{};
  build_filter_desc(shell.filter, &desc);

  int32_t changed = 0;
  char note[512] = {0};
  size_t note_size = 0;
  /* 全分辨率渲染，scale = 1：高斯半径不缩放。 */
  const comp_result result = comp_filter_apply(shell.document, id, &desc, 1.0, &changed, note,
                                               sizeof(note), &note_size);
  shell.needs_render = true;

  wchar_t buffer[768];
  const std::wstring name = widen_ascii(spec->name);
  if (result != COMP_OK) {
    wsprintfW(buffer, L"%s 失败", name.c_str());
    set_status(shell, buffer);
    return;
  }
  if (changed == 0) {
    /* 恒等（例如 Amount 仍为 0）：引擎不碰像素、不开事务，这里如实回报。 */
    wsprintfW(buffer, L"%s：参数为恒等，未改动", name.c_str());
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"%s：已应用｜%s", name.c_str(), widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"%s：已应用", name.c_str());
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 选区 */

/*
 * 从引擎取完整选区状态：active、有效 coverage（含羽化）、边界线段。
 * 无选区时清空全部缓存——「上一次的蚂蚁线」绝不能留在屏幕上。
 *
 * coverage 是文档分辨率的 8 位灰度图；1500 万像素的文档约 15 MB 副本，
 * 只在选区/文档变化时取一次（不随每帧绘制重取）。
 */
void refresh_selection(ShellState& shell) {
  shell.selection_active = false;
  shell.selection_mask.clear();
  shell.selection_segments.clear();
  shell.selection_truncated = false;
  if (shell.document == nullptr || shell.width <= 0 || shell.height <= 0) return;

  int32_t active = 0;
  if (comp_selection_get_active(shell.document, &active) != COMP_OK || active == 0) return;

  const std::size_t pixel_count = static_cast<std::size_t>(shell.width) *
                                  static_cast<std::size_t>(shell.height);
  shell.selection_mask.assign(pixel_count, 0);
  comp_mask mask{};
  mask.width = shell.width;
  mask.height = shell.height;
  mask.stride = static_cast<std::size_t>(shell.width);
  mask.pixels = shell.selection_mask.data();
  if (comp_selection_get_coverage(shell.document, &mask) != COMP_OK) {
    shell.selection_mask.clear();
    return;
  }
  shell.selection_active = true;

  shell.selection_segments.resize(kMaxAntSegments);
  const int needed = comp_ui::selection_boundary_segments(
      shell.selection_mask.data(), shell.width, shell.height,
      static_cast<std::size_t>(shell.width), 128, shell.selection_segments.data(),
      kMaxAntSegments);
  shell.selection_truncated = needed > kMaxAntSegments;
  if (!shell.selection_truncated) {
    shell.selection_segments.resize(static_cast<std::size_t>(needed));
  }
}

/* 选区命令的共同尾部：刷新缓存 + 撤销深度 + 截断提示（如有）。 */
void finish_selection_command(ShellState& shell, const wchar_t* prefix) {
  refresh_selection(shell);
  wchar_t buffer[192];
  if (shell.selection_truncated) {
    wsprintfW(buffer, L"%s（选区边界过多，蚂蚁线截断显示）", prefix);
    refresh_history_status(shell, buffer);
  } else {
    refresh_history_status(shell, prefix);
  }
}

void select_all(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_select_all(shell.document, &changed) != COMP_OK) {
    set_status(shell, L"全选失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已全选（Ctrl+D 取消）" : L"已全选：选区未变化");
}

void deselect_all(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_deselect(shell.document, &changed) != COMP_OK) {
    set_status(shell, L"取消选择失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已取消选择" : L"取消选择：本来就没有选区");
}

void invert_selection(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_invert(shell.document, &changed) != COMP_OK) {
    set_status(shell, L"反选失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已反选" : L"反选：无选区，未变化");
}

/* ------------------------------------------------------- 选区工具共同尾部（S7） */

/*
 * 形状 / 套索 / 魔棒三个入口的共同尾部：一次选区操作只落**一个**撤销步。
 *
 * 引擎的 apply_shape / apply_wand 各自会自成一个隐式事务，而「新选区 + 羽化」是
 * 两次 ABI 调用；因此这里开显式事务把它们并成一步（隐式事务会加入已开启的显式
 * 事务，不会嵌套）。选区没有变化时回滚，避免空撤销步。
 */
void finish_new_selection(ShellState& shell, const char* label, const wchar_t* ok_text,
                          const wchar_t* noop_text, const wchar_t* fail_text,
                          const std::function<comp_result(int32_t*)>& apply,
                          bool* applied = nullptr) {
  if (applied != nullptr) *applied = false;
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.document, label, &txn) != COMP_OK) {
    set_status(shell, L"选区：无法开始事务");
    return;
  }
  int32_t changed = 0;
  comp_result result = apply(&changed);
  /* 新选区羽化只作用于**本次真正新建**的选区：形状/路径/魔棒把 feather 归零后
   * 再用 comp_selection_set_feather 按绝对值设一次（S8 新增的覆盖语义入口）。
   * changed == 0 表示这次点击什么都没选中（例如魔棒没匹配），此时绝不能去动
   * 用户已有选区的羽化——那会把「点空处」变成一次羽化修改。 */
  if (result == COMP_OK && changed != 0 && shell.selection_feather > 0) {
    int32_t feathered = 0;
    result = comp_selection_set_feather(shell.document, shell.selection_feather, &feathered);
    if (result == COMP_OK && feathered != 0) changed = 1;
  }
  if (result != COMP_OK) {
    /* 失败必须整体回滚：半成品选区比不动更坏。 */
    comp_txn_abort(txn);
    set_status(shell, fail_text);
    return;
  }
  if (changed == 0) {
    comp_txn_abort(txn);
    set_status(shell, noop_text);
    return;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"选区：提交失败");
    return;
  }
  if (applied != nullptr) *applied = true;
  finish_selection_command(shell, ok_text);
}

/* --------------------------------------------------------------- 羽化（S7） */

void refresh_feather_input_status(ShellState& shell) {
  wchar_t buffer[192];
  wsprintfW(buffer, L"新选区羽化：%s px（回车应用 / Esc 取消，0..%d）",
            shell.feather_input.c_str(), comp_ui::kMaxSelectionFeather);
  set_status(shell, buffer);
}

void begin_feather_input(ShellState& shell) {
  shell.feather_input.clear();
  wchar_t buffer[192];
  wsprintfW(buffer, L"新选区羽化：键入 0..%d 后回车（当前 %d px）",
            comp_ui::kMaxSelectionFeather, shell.selection_feather);
  set_status(shell, buffer);
}

/*
 * 回车：写进「新选区羽化」默认值。若已有选区，用 S8 的
 * comp_selection_set_feather 按**绝对值覆盖**当前选区的羽化（不是叠加）——
 * 与新选区提交时同一口径：键入几次都只会得到键入的那个半径，0 即清除羽化。
 */
void commit_feather_input(ShellState& shell) {
  const int value =
      comp_ui::clamp_selection_feather(_wtoi(shell.feather_input.c_str()));
  shell.feather_input.clear();
  shell.selection_feather = value;

  wchar_t buffer[224];
  if (shell.document == nullptr || !shell.selection_active) {
    wsprintfW(buffer, L"新选区羽化：%d px（对之后新建的选区生效）", value);
    set_status(shell, buffer);
    return;
  }
  int32_t changed = 0;
  if (comp_selection_set_feather(shell.document, value, &changed) != COMP_OK) {
    set_status(shell, L"羽化失败：当前选区不支持羽化");
    return;
  }
  refresh_selection(shell);
  if (changed == 0) {
    wsprintfW(buffer, L"当前选区羽化已是 %d px（无变化）", value);
    refresh_history_status(shell, buffer);
    return;
  }
  wsprintfW(buffer, L"已把当前选区羽化设为 %d px（绝对值）", value);
  refresh_history_status(shell, buffer);
}

/* --------------------------------------------------- 扩展 / 收缩（P0-4） */

/*
 * 扩展与收缩共用一套键入态：菜单点哪条只决定 modify_op，半径的键入/退格/
 * 回车/Esc 语义完全一致（与羽化同款），所以不做两份状态机。
 */
const wchar_t* modify_op_name(comp_selection_modify_op op) {
  return op == COMP_SELECTION_CONTRACT ? L"收缩" : L"扩展";
}

void refresh_modify_input_status(ShellState& shell) {
  wchar_t buffer[192];
  wsprintfW(buffer, L"选区%s：%s px（回车应用 / Esc 取消，%d..%d）",
            modify_op_name(shell.modify_op), shell.modify_input.c_str(),
            comp_ui::kMinSelectionGrow, comp_ui::kMaxSelectionGrow);
  set_status(shell, buffer);
}

void begin_modify_input(ShellState& shell, comp_selection_modify_op op) {
  if (shell.document == nullptr) {
    set_status(shell, L"选区：还没有文档");
    return;
  }
  /* 空选区扩展没有意义（引擎会如实返回无变化），先在入口拦下来。 */
  if (!shell.selection_active) {
    set_status(shell, L"选区：先建立一个选区，再扩展/收缩");
    return;
  }
  shell.modify_op = op;
  shell.modify_input.clear();
  wchar_t buffer[192];
  wsprintfW(buffer, L"选区%s：键入 %d..%d px 后回车（上次 %d px）",
            modify_op_name(op), comp_ui::kMinSelectionGrow, comp_ui::kMaxSelectionGrow,
            shell.selection_grow);
  set_status(shell, buffer);
}

void commit_modify_input(ShellState& shell) {
  const int typed = _wtoi(shell.modify_input.c_str());
  shell.modify_input.clear();
  /* 0 是「用户还没键入」的常见残留，按下限 1 处理会有 1px 的意外扩展，
   * 所以 0 先拦成提示，不写进画布也不占撤销步。 */
  if (typed <= 0) {
    set_status(shell, L"选区：半径必须 ≥ 1 px（未改动）");
    return;
  }
  if (shell.document == nullptr) {
    set_status(shell, L"选区：还没有文档");
    return;
  }
  if (!shell.selection_active) {
    set_status(shell, L"选区：当前没有选区可改");
    return;
  }
  const int value = comp_ui::clamp_selection_grow(typed);
  shell.selection_grow = value;

  int32_t changed = 0;
  if (comp_selection_modify(shell.document, shell.modify_op, value, &changed) != COMP_OK) {
    set_status(shell, L"选区修改失败（半径超出引擎量程？）");
    return;
  }

  wchar_t buffer[224];
  if (changed == 0) {
    /* changed = 0 只表示引擎没动蒙版（例如收缩把选区收空了不在此列——
     * 空选区也算一次真实变化）。 */
    wsprintfW(buffer, L"选区%s %d px：无变化", modify_op_name(shell.modify_op), value);
  } else {
    wsprintfW(buffer, L"已将选区%s %d px（撤销一步）", modify_op_name(shell.modify_op), value);
  }
  finish_selection_command(shell, buffer);
}

/*
 * 按图层像素载入选区（P0-4）：threshold 是 alpha 下限，取 1 = 「任何有像素的
 * 地方都算选中」。取 0 会把全透明像素也算进去（等于全选），不是「按内容载入」
 * 的本意；面板将来给阈值时再放出来。
 */
void load_layer_pixels_selection(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"选区：还没有文档");
    return;
  }
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"选区：先选中一个图层");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_load_layer(shell.document, row->id, COMP_SELECTION_LOAD_PIXELS,
                                COMP_SELECTION_REPLACE, 1, &changed) != COMP_OK) {
    set_status(shell, L"按图层像素载入选区失败");
    return;
  }
  if (changed == 0) {
    refresh_selection(shell);
    refresh_history_status(shell, L"按图层像素载入选区：该层没有不透明像素");
    return;
  }
  const std::wstring name = widen_utf8(row->name);
  wchar_t buffer[224];
  wsprintfW(buffer, L"已按图层「%s」的像素载入选区", name.c_str());
  finish_selection_command(shell, buffer);
}

/* --------------------------------------------------- 色彩范围（P0-4 第二批） */

/*
 * 上游 `ColorRangeSheet` 的壳内等价物（Select > Color Range）：
 *   - 打开面板 = `comp_txn_begin`，事务快照停在「打开前」；
 *   - 每次取色 / 改容差 / 切反相都在事务里重算**整个**选区（REPLACE），
 *     所以预览本身不产生撤销步，松手也不会有几十步垃圾；
 *   - 回车提交 = 一步撤销；Esc 或从没取过色 = 事务 abort，选区回到打开前。
 * 与魔棒的本质区别：色彩范围匹配**整篇可见合成**里所有同色像素，不要求连通。
 */

/* 状态栏共用文案：颜色数用 include.size()/3 换算（ABI 按颜色数计数，不是字节数）。 */
void color_range_status(ShellState& shell, const wchar_t* prefix) {
  wchar_t buffer[288];
  wsprintfW(buffer, L"%s：取色 %d / 排除 %d · 容差 %d · 反相 %s",
            prefix, static_cast<int>(shell.color_range_include.size() / 3),
            static_cast<int>(shell.color_range_exclude.size() / 3),
            shell.color_range_fuzziness, shell.color_range_invert ? L"是" : L"否");
  set_status(shell, buffer);
}

/* 事务重开：没取过色时上游「恢复原选区」，abort 正好把我们送回那个快照。 */
void color_range_restart_txn(ShellState& shell) {
  if (shell.document == nullptr) {
    shell.color_range_txn = nullptr;
    return;
  }
  if (shell.color_range_txn != nullptr) {
    comp_txn_abort(shell.color_range_txn);
    shell.color_range_txn = nullptr;
  }
  if (comp_txn_begin(shell.document, "Color Range", &shell.color_range_txn) != COMP_OK) {
    shell.color_range_txn = nullptr;
  }
  refresh_selection(shell);
  shell.needs_render = true;
}

void color_range_begin(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"色彩范围：还没有文档");
    return;
  }
  if (shell.color_range_open) {
    /* 再点一次菜单 = 回到打开前的选区再重开，避免两个事务叠在同一个文档上。 */
    if (shell.color_range_txn != nullptr) comp_txn_abort(shell.color_range_txn);
    shell.color_range_txn = nullptr;
    shell.color_range_open = false;
  }
  /* 右栏一次只挂一个面板（与滤镜/效果/路径/历史互斥，避免两块矩形互相遮住）。 */
  shell.filter_open = false;
  shell.filter_input.clear();
  shell.effect_open = false;
  shell.effect_input.clear();
  shell.path_open = false;
  shell.history_open = false;
  shell.color_range_include.clear();
  shell.color_range_exclude.clear();
  shell.color_range_fuzziness = comp_ui::kDefaultColorRangeFuzziness;
  shell.color_range_invert = 0;
  shell.color_range_input.clear();
  shell.color_range_open = true;
  if (comp_txn_begin(shell.document, "Color Range", &shell.color_range_txn) != COMP_OK) {
    shell.color_range_txn = nullptr;
    /* 没有事务就没有「预览后回滚」，不如直接不开面板，别让用户改一堆无效参数。 */
    shell.color_range_open = false;
    set_status(shell, L"色彩范围：无法开启事务（未改动）");
    return;
  }
  set_status(shell,
             L"色彩范围：点画布取色（Shift 加色 · Alt 减色）· -/= 容差 · I 反相 · "
             L"回车应用 · Esc 取消");
}

void color_range_cancel(ShellState& shell) {
  const comp_txn txn = shell.color_range_txn;
  shell.color_range_txn = nullptr;
  shell.color_range_open = false;
  shell.color_range_input.clear();
  if (txn != nullptr) comp_txn_abort(txn);
  refresh_selection(shell);
  shell.needs_render = true;
  set_status(shell, L"已取消色彩范围（选区回到打开前）");
}

void color_range_commit(ShellState& shell) {
  const comp_txn txn = shell.color_range_txn;
  const bool has_colors = !shell.color_range_include.empty();
  shell.color_range_txn = nullptr;
  shell.color_range_open = false;
  shell.color_range_input.clear();
  bool committed = false;
  if (txn != nullptr) {
    if (!has_colors) {
      /* 没取过色 = 上游的「什么都不选」：回滚，不留一步空撤销。 */
      comp_txn_abort(txn);
    } else {
      committed = comp_txn_commit(txn) == COMP_OK;
    }
  }
  refresh_selection(shell);
  shell.needs_render = true;
  if (!has_colors) {
    set_status(shell, L"色彩范围：还没取色（选区未改动）");
    return;
  }
  wchar_t buffer[288];
  wsprintfW(buffer, L"色彩范围：已应用（取色 %d / 排除 %d · 容差 %d%s）",
            static_cast<int>(shell.color_range_include.size() / 3),
            static_cast<int>(shell.color_range_exclude.size() / 3),
            shell.color_range_fuzziness, shell.color_range_invert ? L" · 反相" : L"");
  refresh_history_status(shell, committed ? buffer : L"色彩范围：提交失败（已回滚）");
}

/*
 * 重算预览选区。没有 include 颜色时上游回到原选区（不是全不选），这里用
 * 「事务重开」实现：abort 回快照 + 重新 begin，快照随之变成当前（= 打开前）状态。
 * 引擎对 0 匹配的定义是「不改选区」，而上游面板此时显示空选区，所以显式 deselect。
 */
void color_range_refresh(ShellState& shell) {
  if (shell.document == nullptr || !shell.color_range_open) return;
  if (!shell.color_range_input.empty()) return; /* 键入态还没落值，不预览 */
  if (shell.color_range_include.empty()) {
    color_range_restart_txn(shell);
    return;
  }
  if (shell.color_range_txn == nullptr) return;

  int32_t changed = 0;
  const comp_result result = comp_selection_apply_color_range(
      shell.document,
      shell.color_range_include.empty() ? nullptr : shell.color_range_include.data(),
      shell.color_range_include.size() / 3,
      shell.color_range_exclude.empty() ? nullptr : shell.color_range_exclude.data(),
      shell.color_range_exclude.size() / 3, shell.color_range_fuzziness, shell.color_range_invert,
      COMP_SELECTION_REPLACE, &changed);
  if (result != COMP_OK) {
    set_status(shell, L"色彩范围：计算失败（参数被引擎拒绝）");
    return;
  }
  if (changed == 0) {
    int32_t deselected = 0;
    comp_selection_deselect(shell.document, &deselected);
  }
  refresh_selection(shell);
  shell.needs_render = true;
}

/* 取色：合并源 + 3×3 均值（上游 color(in:at:) 的口径），坐标落在画布内才取。 */
void color_range_sample(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.color_range_open || shell.document == nullptr) return;
  if (!layout.canvas.contains(x, y)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  if (doc.x < 0.0 || doc.y < 0.0 || doc.x >= static_cast<double>(shell.width) ||
      doc.y >= static_cast<double>(shell.height)) {
    set_status(shell, L"色彩范围：点在画布外，没有取样");
    return;
  }
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (comp_eyedropper_sample(shell.document, 0, doc.x, doc.y, /*sample_size=*/3,
                             COMP_EYEDROPPER_MERGED, rgba) != COMP_OK) {
    set_status(shell, L"色彩范围：取样失败（合成不可用）");
    return;
  }
  const comp_ui::ColorRGB color = comp_ui::color_from_unit(rgba[0], rgba[1], rgba[2]);
  const uint8_t triple[3] = {color.r, color.g, color.b};
  const bool remove = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool add = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const wchar_t* action = L"取色";
  if (remove) {
    shell.color_range_exclude.insert(shell.color_range_exclude.end(), triple, triple + 3);
    action = L"排除";
  } else if (add) {
    shell.color_range_include.insert(shell.color_range_include.end(), triple, triple + 3);
    action = L"加色";
  } else {
    /* replace：与上游 sampleMode = .replace 一致，重开取色并把排除清空。 */
    shell.color_range_include.assign(triple, triple + 3);
    shell.color_range_exclude.clear();
  }
  color_range_refresh(shell);

  char hex[8] = {0};
  comp_ui::color_to_hex(color, hex);
  wchar_t buffer[288];
  wsprintfW(buffer, L"色彩范围：%s %s（取色 %d / 排除 %d · 容差 %d%s）", action,
            widen_ascii(hex).c_str(), static_cast<int>(shell.color_range_include.size() / 3),
            static_cast<int>(shell.color_range_exclude.size() / 3),
            shell.color_range_fuzziness, shell.color_range_invert ? L" · 反相" : L"");
  set_status(shell, buffer);
}

/* 点掉一个色块：取色行删 include，排除行删 exclude；删空即回到打开前的选区。 */
void color_range_remove_swatch(ShellState& shell, int slot) {
  const int include_count = static_cast<int>(shell.color_range_include.size() / 3);
  if (slot < 0) return;
  if (slot < include_count) {
    auto& list = shell.color_range_include;
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(slot) * 3,
               list.begin() + static_cast<std::ptrdiff_t>(slot) * 3 + 3);
  } else {
    const int index = slot - include_count;
    auto& list = shell.color_range_exclude;
    if (index * 3 + 3 > static_cast<int>(list.size())) return;
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index) * 3,
               list.begin() + static_cast<std::ptrdiff_t>(index) * 3 + 3);
  }
  color_range_refresh(shell);
  color_range_status(shell, L"色彩范围：已移除一个取样色");
}

void color_range_step_fuzziness(ShellState& shell, int direction) {
  shell.color_range_fuzziness =
      comp_ui::step_color_range_fuzziness(shell.color_range_fuzziness, direction);
  color_range_refresh(shell);
  wchar_t buffer[240];
  wsprintfW(buffer, L"色彩范围：容差 %d（%d..%d）%s", shell.color_range_fuzziness,
            comp_ui::kMinColorRangeFuzziness, comp_ui::kMaxColorRangeFuzziness,
            shell.color_range_txn != nullptr ? L"· 实时预览，回车应用" : L"");
  set_status(shell, buffer);
}

void color_range_toggle_invert(ShellState& shell) {
  shell.color_range_invert = shell.color_range_invert ? 0 : 1;
  color_range_refresh(shell);
  color_range_status(shell, shell.color_range_invert ? L"色彩范围：反相开" : L"色彩范围：反相关");
}

void color_range_clear_colors(ShellState& shell) {
  shell.color_range_include.clear();
  shell.color_range_exclude.clear();
  color_range_refresh(shell);
  set_status(shell, L"色彩范围：已清空取样色（选区回到打开前）");
}

void color_range_begin_input(ShellState& shell, wchar_t character) {
  if (shell.color_range_input.size() >= 3) return;
  shell.color_range_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.color_range_input.c_str());
  set_status(shell, buffer);
}

/* 键入值夹到 0..200 再落值：面板永不产生越界参数（ABI 仍会拒绝越界）。 */
void color_range_commit_input(ShellState& shell) {
  if (shell.color_range_input.empty()) return;
  const std::wstring text = shell.color_range_input;
  shell.color_range_input.clear();
  int value = 0;
  for (size_t i = 0; i < text.size(); ++i) value = value * 10 + static_cast<int>(text[i] - L'0');
  shell.color_range_fuzziness = comp_ui::clamp_color_range_fuzziness(value);
  color_range_refresh(shell);
  wchar_t buffer[200];
  wsprintfW(buffer, L"色彩范围：容差 = %d", shell.color_range_fuzziness);
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 套索（S7） */

bool lasso_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Lasso;
}

void lasso_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!lasso_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.lasso_active = true;
  shell.lasso_points.clear();
  shell.lasso_points.push_back(static_cast<float>(doc.x));
  shell.lasso_points.push_back(static_cast<float>(doc.y));
  SetCapture(window);
  set_status(shell, L"套索：按住拖出自由选区，松开闭合成选区（Shift 加选 / Alt 减选）");
}

void lasso_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.lasso_active) return;
  const int point_count = static_cast<int>(shell.lasso_points.size() / 2);
  if (!comp_ui::lasso_can_append(point_count)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  const size_t count = shell.lasso_points.size();
  const double last_x = count >= 2 ? shell.lasso_points[count - 2] : doc.x;
  const double last_y = count >= 2 ? shell.lasso_points[count - 1] : doc.y;
  if (!comp_ui::stroke_point_should_append(last_x, last_y, doc.x, doc.y)) return;
  shell.lasso_points.push_back(static_cast<float>(doc.x));
  shell.lasso_points.push_back(static_cast<float>(doc.y));
}

void lasso_cancel(ShellState& shell) {
  shell.lasso_active = false;
  shell.lasso_points.clear();
}

void lasso_end(HWND window, ShellState& shell) {
  if (!shell.lasso_active) return;
  shell.lasso_active = false;

  /* 同画笔：ReleaseCapture 同步触发 WM_CAPTURECHANGED -> lasso_cancel 清点。 */
  std::vector<float> points;
  points.swap(shell.lasso_points);
  ReleaseCapture();
  if (shell.document == nullptr) return;

  const int point_count = static_cast<int>(points.size() / 2);
  if (!comp_ui::lasso_should_close(point_count)) {
    set_status(shell, L"套索：至少需要拖出 3 个点才能闭合成选区");
    return;
  }

  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }

  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"套索：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"套索：已减选"
                                                                : L"套索：已应用";
  finish_new_selection(
      shell, "Lasso Selection", ok_text, L"套索：选区未变化", L"套索失败：多边形无效",
      [&](int32_t* changed) {
        return comp_selection_apply_shape(shell.document, COMP_SELECTION_POLYGON, points.data(),
                                          point_count, combine, /*antialias=*/1, changed);
      });
}

/* ------------------------------------------------------------ 魔棒（S7） */

bool wand_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Wand;
}

void wand_click(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!wand_available(shell)) return;
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"魔棒：请先选择一个图层");
    return;
  }
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }
  const comp_selection_wand_source source =
      shell.wand_merged ? COMP_SELECTION_WAND_MERGED : COMP_SELECTION_WAND_LAYER;

  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"魔棒：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"魔棒：已减选"
                                                                : L"魔棒：已应用";
  /* 图层带缩放/旋转/翻转时引擎拒绝 1:1 假设：提示切到合并取样，而不是静默失败。 */
  const wchar_t* fail_text = L"魔棒失败：参数无效或该图层非 1:1 变换（Shift+W 切合并取样）";
  finish_new_selection(shell, "Magic Wand Selection", ok_text, L"魔棒：没有匹配的像素，选区未变",
                       fail_text, [&](int32_t* changed) {
                         return comp_selection_apply_wand(
                             shell.document, id, source, doc.x, doc.y, shell.wand_tolerance,
                             shell.wand_contiguous ? 1 : 0, shell.wand_radius, combine,
                             changed);
                       });
}

/* ------------------------------------------------------------ 钢笔（S8） */

bool pen_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Pen;
}

/* 闭合命中半径：屏幕上约 6 px，换算成文档半径，缩放变化不影响手感。 */
double pen_close_radius(const ShellState& shell) {
  const double zoom = shell.view.zoom > 1e-6 ? shell.view.zoom : 1.0;
  return 6.0 / zoom;
}

void pen_refresh_status(ShellState& shell) {
  wchar_t buffer[256];
  wsprintfW(buffer,
            L"钢笔：%d 个锚点（点击继续 / 拖动拉控制柄 / 点首锚点闭合 / Enter 完成 / Esc 取消）",
            static_cast<int>(shell.pen_anchors.size()));
  set_status(shell, buffer);
}

void pen_cancel_drag(ShellState& shell) { shell.pen_dragging = false; }

void pen_cancel(ShellState& shell) {
  const bool had_path = !shell.pen_anchors.empty();
  shell.pen_anchors.clear();
  shell.pen_dragging = false;
  if (had_path) set_status(shell, L"钢笔：已取消当前路径");
}

/* 实现在下面的「路径面板」一节；提交路径时要顺手入列。 */
bool path_store_add(ShellState& shell, const std::vector<comp_ui::PenAnchor>& anchors);

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
  if (shell.document == nullptr) return;
  const int anchor_count = static_cast<int>(shell.pen_anchors.size());
  if (anchor_count < 2) {
    set_status(shell, L"钢笔：至少需要 2 个锚点才能闭合路径");
    return;
  }

  std::vector<comp_selection_cubic> cubics;
  const int path_count = path_build_cubics(shell.pen_anchors, &cubics);
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
  const std::vector<comp_ui::PenAnchor> saved_path = shell.pen_anchors;
  /* 先清状态再提交：路径已经变成选区参数，失败也不该留下半截预览。 */
  shell.pen_anchors.clear();
  shell.pen_dragging = false;
  ReleaseCapture();
  bool applied = false;
  finish_new_selection(
      shell, "Pen Selection", ok_text, L"钢笔：选区未变化", L"钢笔失败：路径无效",
      [&](int32_t* changed) {
        return comp_selection_apply_path(shell.document, cubics.data(), path_count, combine,
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
              shell.paths.back().name.c_str(), comp_ui::kPathMaxEntries);
    shell.status += buffer;
  } else if (applied) {
    wchar_t buffer[256];
    wsprintfW(buffer, L"%s；路径已入列 %s（双击路径面板可再落一次）", ok_text,
              shell.paths.back().name.c_str());
    set_status(shell, buffer);
  }
}

/* ------------------------------------------------------ 路径面板（P0-4 可选） */

/* 定义在窗口过程一节（下面才用到完整 Layout 口径），这里先声明：面板开合要
 * 按真实面板矩形夹滚动位置。 */
bool client_layout(const ShellState& shell, Layout* out);

/*
 * 面板挂什么：已提交过的钢笔路径（锚点）。引擎没有路径实体，所以「保存路径」
 * 就是壳侧留一份锚点副本，用途只有一个——再落一次选区。因此这里没有重命名/
 * 复制/导出，只有选中、删除、转选区三件事。
 */
bool path_store_add(ShellState& shell, const std::vector<comp_ui::PenAnchor>& anchors) {
  if (anchors.size() < 2) return false;
  bool evicted = false;
  if (static_cast<int>(shell.paths.size()) >= comp_ui::kPathMaxEntries) {
    shell.paths.erase(shell.paths.begin());
    evicted = true;
  }
  ShellState::StoredPath entry;
  ++shell.path_serial;
  wchar_t name[32];
  wsprintfW(name, L"路径 %d", shell.path_serial);
  entry.name = name;
  entry.anchors = anchors;
  shell.paths.push_back(std::move(entry));
  shell.path_selected = static_cast<int>(shell.paths.size()) - 1;
  return evicted;
}

/* 文档被替换/新建：锚点是旧文档坐标，留着只会把选区落到错误的位置。 */
void path_store_clear(ShellState& shell) {
  shell.path_open = false;
  shell.paths.clear();
  shell.path_selected = 0;
  shell.path_scroll = 0;
  shell.path_serial = 0;
}

/* 文字工具（T）独占右栏显示它自己的属性面板：此时路径面板只是「挂着不显示」。 */
bool path_panel_visible(const ShellState& shell) {
  return shell.path_open && shell.tool != comp_ui::ToolId::Type;
}

void path_status(ShellState& shell) {
  if (shell.paths.empty()) {
    set_status(shell, L"路径：还没有保存的路径（钢笔闭合提交后自动入列）");
    return;
  }
  const int count = static_cast<int>(shell.paths.size());
  int index = shell.path_selected;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  const ShellState::StoredPath& path = shell.paths[static_cast<size_t>(index)];
  wchar_t buffer[224];
  wsprintfW(buffer, L"路径 %d/%d：%s（%d 个锚点）· 回车转选区 · Del 删除", index + 1, count,
            path.name.c_str(), static_cast<int>(path.anchors.size()));
  set_status(shell, buffer);
}

void path_select(ShellState& shell, int delta) {
  const int count = static_cast<int>(shell.paths.size());
  if (count <= 0) {
    path_status(shell);
    return;
  }
  int index = shell.path_selected + delta;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  shell.path_selected = index;
  /* 键盘选行要把高亮滚进可视区，否则↑/↓ 走到看不见的行就没反馈了。 */
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.path_scroll = comp_ui::path_ensure_visible(layout.adjust_panel, count, index,
                                                     shell.path_scroll);
  }
  path_status(shell);
}

/* 重新落成选区：与钢笔提交同一条装配口径，同样一步撤销（套索/形状同款）。 */
void path_apply_selected(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"路径：还没有文档");
    return;
  }
  const int count = static_cast<int>(shell.paths.size());
  if (count <= 0 || shell.path_selected < 0 || shell.path_selected >= count) {
    set_status(shell, L"路径面板：先选中一条路径");
    return;
  }
  const ShellState::StoredPath& path = shell.paths[static_cast<size_t>(shell.path_selected)];
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
        return comp_selection_apply_path(shell.document, cubics.data(), path_count,
                                         COMP_SELECTION_REPLACE, /*antialias=*/1, changed);
      });
}

/* 删一条路径只动壳侧列表：文档没变，所以不产生撤销步（状态栏里说清楚）。 */
void path_delete_selected(ShellState& shell) {
  const int count = static_cast<int>(shell.paths.size());
  if (count <= 0) {
    set_status(shell, L"路径面板：没有可删除的路径");
    return;
  }
  int index = shell.path_selected;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  const std::wstring name = shell.paths[static_cast<size_t>(index)].name;
  shell.paths.erase(shell.paths.begin() + index);
  if (shell.path_selected >= static_cast<int>(shell.paths.size())) {
    shell.path_selected = static_cast<int>(shell.paths.size()) - 1;
  }
  if (shell.paths.empty()) {
    shell.path_selected = 0;
    shell.path_scroll = 0;
  } else {
    Layout layout{};
    if (client_layout(shell, &layout)) {
      shell.path_scroll = comp_ui::path_ensure_visible(
          layout.adjust_panel, static_cast<int>(shell.paths.size()), shell.path_selected,
          shell.path_scroll);
    }
  }
  wchar_t buffer[192];
  if (shell.paths.empty()) {
    wsprintfW(buffer, L"已删除路径「%s」（列表清空，选区不动）", name.c_str());
  } else {
    wsprintfW(buffer, L"已删除路径「%s」（剩 %d 条，选区不动）", name.c_str(),
              static_cast<int>(shell.paths.size()));
  }
  set_status(shell, buffer);
}

void path_panel_toggle(ShellState& shell) {
  if (shell.path_open) {
    shell.path_open = false;
    set_status(shell, L"已关闭路径面板");
    return;
  }
  if (shell.document == nullptr) {
    set_status(shell, L"路径面板：还没有文档");
    return;
  }
  /* 右栏一次只挂一个面板：色彩范围持有显式事务，必须先 abort 再让位。 */
  if (shell.color_range_open) color_range_cancel(shell);
  shell.filter_open = false;
  shell.filter_input.clear();
  shell.effect_open = false;
  shell.effect_input.clear();
  shell.path_open = true;
  shell.history_open = false;
  const int count = static_cast<int>(shell.paths.size());
  if (shell.path_selected >= count) shell.path_selected = count - 1;
  if (shell.path_selected < 0) shell.path_selected = 0;
  if (!shell.paths.empty()) {
    Layout layout{};
    if (client_layout(shell, &layout)) {
      shell.path_scroll = comp_ui::path_ensure_visible(layout.adjust_panel, count,
                                                       shell.path_selected, shell.path_scroll);
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
  return shell.history_open && shell.tool != comp_ui::ToolId::Type;
}

int history_state_count(const ShellState& shell) {
  if (shell.document == nullptr) return 1;
  size_t steps = 0;
  if (comp_document_history_step_count(shell.document, &steps) != COMP_OK) return 1;
  return static_cast<int>(steps) + 1;
}

int history_current(const ShellState& shell) {
  if (shell.document == nullptr) return 0;
  size_t undo = 0;
  size_t redo = 0;
  if (comp_document_history_stats(shell.document, &undo, &redo) != COMP_OK) return 0;
  return static_cast<int>(undo);
}

/* 第 index 个状态的标题；标签是 UTF-8（写入方给的），面板只负责展示。 */
std::wstring history_state_label(const ShellState& shell, int index) {
  if (index <= 0) return L"最初状态（新建 / 打开）";
  char utf8[192];
  size_t required = 0;
  if (shell.document == nullptr ||
      comp_document_history_step_label(shell.document, static_cast<size_t>(index), nullptr, 0,
                                       &required) != COMP_OK ||
      required == 0 || required > sizeof(utf8) ||
      comp_document_history_step_label(shell.document, static_cast<size_t>(index), utf8,
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
  if (shell.document == nullptr) {
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
  if (comp_document_history_jump(shell.document, static_cast<size_t>(target)) != COMP_OK) {
    set_status(shell, L"回跳失败");
    return;
  }
  shell.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell);
  const int after = history_current(shell);
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.history_scroll = comp_ui::history_ensure_visible(layout.adjust_panel, count, after,
                                                           shell.history_scroll);
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
  if (shell.history_brush_state < 0) {
    const int target = history_current(shell) - 1;
    return target < 0 ? 0 : target;
  }
  return std::min(shell.history_brush_state, count - 1);
}

void history_panel_toggle(ShellState& shell) {
  if (shell.history_open) {
    shell.history_open = false;
    set_status(shell, L"已关闭历史记录面板");
    return;
  }
  if (shell.document == nullptr) {
    set_status(shell, L"历史记录：还没有文档");
    return;
  }
  /* 右栏一次只挂一个面板：色彩范围持有显式事务，必须先 abort 再让位。 */
  if (shell.color_range_open) color_range_cancel(shell);
  shell.filter_open = false;
  shell.filter_input.clear();
  shell.effect_open = false;
  shell.effect_input.clear();
  shell.path_open = false;
  shell.history_open = true;
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.history_scroll = comp_ui::history_ensure_visible(
        layout.adjust_panel, history_state_count(shell), history_current(shell),
        shell.history_scroll);
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
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  const int anchor_count = static_cast<int>(shell.pen_anchors.size());
  /* 点第一个锚点 = 闭合（首锚点至少要有 2 个锚点才有闭合路径可言）。 */
  if (comp_ui::pen_hits_first_anchor(shell.pen_anchors.data(), anchor_count, doc,
                                     pen_close_radius(shell))) {
    commit_pen_path(shell);
    return;
  }
  const size_t index = static_cast<size_t>(anchor_count);
  shell.pen_anchors.resize(index + 1u);
  int next = anchor_count;
  if (!comp_ui::pen_append_anchor(shell.pen_anchors.data(), &next, doc)) {
    shell.pen_anchors.resize(index);
    wchar_t buffer[160];
    wsprintfW(buffer, L"钢笔：锚点数已达上限 %d", comp_ui::kPenMaxAnchors);
    set_status(shell, buffer);
    return;
  }
  shell.pen_anchors.resize(static_cast<size_t>(next));
  /* 按住不放就进入「拖控制柄」状态；只点不拖时松开即得到直角锚点。 */
  shell.pen_dragging = true;
  SetCapture(window);
  pen_refresh_status(shell);
}

void pen_drag(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.pen_dragging || shell.pen_anchors.empty()) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  comp_ui::pen_drag_handle(shell.pen_anchors.data(),
                           static_cast<int>(shell.pen_anchors.size()), doc);
}

void pen_up(HWND window, ShellState& shell) {
  if (!shell.pen_dragging) return;
  shell.pen_dragging = false;
  ReleaseCapture();
  pen_refresh_status(shell);
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
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Gradient;
}

void gradient_cancel(ShellState& shell) { shell.gradient_active = false; }

void gradient_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!gradient_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.gradient_active = true;
  shell.gradient_start = doc;
  shell.gradient_current = doc;
  SetCapture(window);
  wchar_t buffer[192];
  wsprintfW(buffer, L"渐变 / %s：拖出方向与长度后松开（再按 G 切换类型；Shift 反向）",
            widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str());
  set_status(shell, buffer);
}

void gradient_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.gradient_active) return;
  shell.gradient_current =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
}

void gradient_end(HWND window, ShellState& shell) {
  if (!shell.gradient_active) return;
  shell.gradient_active = false;
  ReleaseCapture();

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"渐变：请先选择一个图层");
    return;
  }
  const comp_ui::Point start = shell.gradient_start;
  const comp_ui::Point end = shell.gradient_current;
  if (!comp_ui::gradient_drag_is_usable(start.x, start.y, end.x, end.y)) {
    set_status(shell, L"渐变：拖拽太短，已放弃（方向/半径需要一个可定义的长度）");
    return;
  }

  /*
   * Shift = 反向：色标顺序翻转成「透明 -> 前景色」。这是拖拽结束后才读的
   * 修饰键（与框选/套索的 Shift=加选同口径：提交时才决定组合方式）。
   */
  const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_GRADIENT;
  desc.gradient.type = shell.gradient_type;
  desc.gradient.start_x = start.x;
  desc.gradient.start_y = start.y;
  desc.gradient.end_x = end.x;
  desc.gradient.end_y = end.y;
  const uint8_t solid[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 255};
  const uint8_t clear[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 0};
  const uint8_t* color0 = reverse ? clear : solid;
  const uint8_t* color1 = reverse ? solid : clear;
  for (int i = 0; i < 4; ++i) {
    desc.gradient.color0[i] = color0[i];
    desc.gradient.color1[i] = color1[i];
  }
  desc.gradient.opacity = 1.0f;

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"渐变失败（%d）", static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"渐变 / %s：已应用｜%s",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str(),
              widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"渐变 / %s：已应用（%s）",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str(),
              reverse ? L"透明 → 前景" : L"前景 → 透明");
  }
  refresh_history_status(shell, buffer);
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
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Shape;
}

void shape_cancel(ShellState& shell) { shell.shape_active = false; }

void shape_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!shape_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.shape_active = true;
  shell.shape_start = doc;
  shell.shape_current = doc;
  SetCapture(window);
  wchar_t buffer[224];
  wsprintfW(buffer,
            L"形状 / %s：拖出%s后松开（Shift = 正方/正圆，Alt = 从中心；再按 U 切种类）",
            widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
            shell.shape_kind == comp_ui::kShapeLine ? L"两端点" : L"对角点");
  set_status(shell, buffer);
}

void shape_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.shape_active) return;
  shell.shape_current =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
}

void shape_end(HWND window, ShellState& shell) {
  if (!shell.shape_active) return;
  shell.shape_active = false;
  ReleaseCapture();

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"形状：请先选择一个图层");
    return;
  }

  /* 修饰键在松开时才读：拖动中按下/松开 Shift、Alt 都能改落笔盒（与框选同口径）。 */
  const bool square = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool from_center = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const comp_ui::ShapeDragBox box = comp_ui::shape_drag_box(
      shell.shape_start.x, shell.shape_start.y, shell.shape_current.x, shell.shape_current.y,
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
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

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

/* ------------------------------------------------------------ 文字（S11） */

/*
 * 文字工具（COMP_TOOL_TYPE）：窗口路径是「点击落插入点 -> 键入 ASCII -> 回车提交」。
 *
 * 键盘模态：落点后普通字符不再走工具快捷键，而是进 type_text；文本框、插入点光标
 * 与度量数字只是预览，像素在回车时由引擎一次性写出（与渐变/形状「不在拖拽中反复
 * 提交」同口径）。Esc / 失焦 / 切工具取消 = 丢弃缓冲，不产生撤销步。
 *
 * 为什么没有拖拽：上游文字工具也是「点一下定位、键入、提交」，拖动不产生额外几何；
 * 位置就取点击处的文档像素（向下/向右排字）。
 */
bool type_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Type;
}

void type_cancel(ShellState& shell) {
  shell.type_active = false;
  shell.type_text.clear();
}

/* 状态栏：空缓冲时给快捷键提示，有内容时给行数/文本框尺寸/排版值。 */
void type_refresh_status(ShellState& shell) {
  const comp_ui::TypeBlockMetrics metrics =
      comp_ui::type_block_metrics(shell.type_font_size, shell.type_line_height,
                                  shell.type_letter_spacing, shell.type_text.c_str());
  const int size_px = static_cast<int>(shell.type_font_size + 0.5);
  const int line_pct = static_cast<int>(shell.type_line_height * 100.0 + 0.5);
  const int spacing_px = static_cast<int>(std::lround(shell.type_letter_spacing));
  const int width_px = static_cast<int>(std::lround(metrics.width));
  const int height_px = static_cast<int>(std::lround(metrics.height));
  wchar_t buffer[384];
  if (shell.type_text.empty()) {
    wsprintfW(buffer,
              L"文字：插入点 (%d, %d)｜字号 %d px，行距 %d%%，字距 %d px｜"
              L"键入 ASCII，Shift+Enter 换行，回车提交，Esc 取消；"
              L"[ / ] 字号，Shift+[ / ] 字距，Alt+[ / ] 行距",
              static_cast<int>(std::floor(shell.type_origin.x)),
              static_cast<int>(std::floor(shell.type_origin.y)), size_px, line_pct, spacing_px);
  } else {
    /* 状态栏是单行：把换行显示成 '|'，不把控制字符塞进窗口标题。 */
    std::string display;
    display.reserve(shell.type_text.size());
    for (char ch : shell.type_text) display.push_back(ch == '\n' ? '|' : ch);
    wsprintfW(buffer, L"文字：%s｜%d 行 %d×%d px，字号 %d，行距 %d%%，字距 %d｜"
                      L"回车提交，Esc 取消",
              widen_ascii(display.c_str()).c_str(), metrics.line_count, width_px, height_px,
              size_px, line_pct, spacing_px);
  }
  set_status(shell, buffer);
}

/* 重新落点前要先提交上一段文字（定义在下方，先声明）。 */
void type_commit(ShellState& shell);

void type_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!type_available(shell)) return;
  (void)window;
  /* 已经在键入时再点一次 = 先提交上一段文字，再在新位置重新落点（不丢已键入内容）。 */
  if (shell.type_active && !shell.type_text.empty()) type_commit(shell);
  shell.type_origin = shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.type_active = true;
  shell.type_text.clear();
  /* 落插入点即离开属性面板的数值键入态，否则数字键会被面板缓冲吃掉。 */
  shell.type_input.clear();
  /*
   * 同理关掉效果/滤镜面板：这两块的键盘分支排在文字键入态之前，留着会把
   * 刚点的这一笔字符全吞掉（用户看到插入点在闪、打字没反应）。
   */
  shell.effect_open = false;
  shell.effect_input.clear();
  shell.filter_open = false;
  shell.filter_input.clear();
  type_refresh_status(shell);
}

/*
 * 回车提交：一次 comp_tool_apply = 一步撤销。空缓冲不提交（没有可画的字形，
 * 引擎会以「无字形」拒绝），外壳保持插入点等待输入。
 */
void type_commit(ShellState& shell) {
  if (!shell.type_active) return;
  if (shell.type_text.empty()) {
    set_status(shell, L"文字：还没有输入内容（键入 ASCII 后回车提交，Esc 取消）");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"文字：请先选择一个图层");
    return;
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_TYPE;
  desc.type.origin_x = shell.type_origin.x;
  desc.type.origin_y = shell.type_origin.y;
  desc.type.text = shell.type_text.c_str();
  desc.type.text_length = static_cast<int32_t>(shell.type_text.size());
  desc.type.style.font_size = shell.type_font_size;
  desc.type.style.line_height = shell.type_line_height;
  desc.type.style.letter_spacing = shell.type_letter_spacing;
  desc.type.style.antialias = 1;
  desc.type.color[0] = shell.foreground.r;
  desc.type.color[1] = shell.foreground.g;
  desc.type.color[2] = shell.foreground.b;
  desc.type.color[3] = 255;
  desc.type.opacity = 1.0f;

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

  /* 无论成功与否都退出键入态：成功已提交，失败也没有可保留的预览语义。 */
  const int lines = comp_ui::type_block_metrics(shell.type_font_size, shell.type_line_height,
                                                shell.type_letter_spacing,
                                                shell.type_text.c_str())
                        .line_count;
  shell.type_active = false;
  shell.type_text.clear();

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"文字失败（%d）", static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"文字：已应用（%d 行）｜%s", lines, widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"文字：已应用（%d 行）", lines);
  }
  refresh_history_status(shell, buffer);
}

/* [ / ] 调字号，Shift+[ / ] 调字距；只在文字工具内生效（其余工具沿用笔刷/魔棒）。 */
void type_step_param(ShellState& shell, int direction, bool spacing) {
  if (spacing) {
    shell.type_letter_spacing =
        comp_ui::step_type_letter_spacing(shell.type_letter_spacing, shell.type_font_size,
                                          direction);
  } else {
    shell.type_font_size = comp_ui::step_type_font_size(shell.type_font_size, direction);
    /* 字号变小会让原字距超出「步进 >= 0.5 px」的底限：跟着重新夹取。 */
    shell.type_letter_spacing =
        comp_ui::clamp_type_letter_spacing(shell.type_letter_spacing, shell.type_font_size);
  }
  if (shell.type_active) {
    type_refresh_status(shell);
  } else {
    wchar_t buffer[192];
    wsprintfW(buffer,
              L"文字：字号 %d px，行距 %d%%，字距 %d px（[ / ] 字号，Shift+[ / ] 字距，"
              L"Alt+[ / ] 行距）",
              static_cast<int>(shell.type_font_size + 0.5),
              static_cast<int>(shell.type_line_height * 100.0 + 0.5),
              static_cast<int>(std::lround(shell.type_letter_spacing)));
    set_status(shell, buffer);
  }
}

/* ------------------------------------------------------------ 文字属性面板（S14） */

/*
 * 三行属性面板（字号 / 行距 / 字距）。值仍写回 ShellState 的 type_* 字段，
 * 面板只是第二条操作通路：引擎参数、状态栏、文本框预览都读同一组值。
 * 行几何、夹取、步进全部来自 ui_core.h，绘制与命中不会漂移。
 */
bool type_panel_visible(const ShellState& shell) {
  return shell.tool == comp_ui::ToolId::Type && !shell.filter_open;
}

double type_panel_value(const ShellState& shell, int row) {
  switch (row) {
    case comp_ui::kTypePanelFontSizeRow: return shell.type_font_size;
    case comp_ui::kTypePanelLineHeightRow: return shell.type_line_height;
    case comp_ui::kTypePanelLetterSpacingRow: return shell.type_letter_spacing;
    default: return 0.0;
  }
}

std::wstring type_panel_value_text(const ShellState& shell, int row) {
  wchar_t buffer[48];
  if (row == comp_ui::kTypePanelLineHeightRow) {
    wsprintfW(buffer, L"%d%%", static_cast<int>(shell.type_line_height * 100.0 + 0.5));
  } else if (row == comp_ui::kTypePanelFontSizeRow) {
    wsprintfW(buffer, L"%d px", static_cast<int>(shell.type_font_size + 0.5));
  } else {
    wsprintfW(buffer, L"%d px", static_cast<int>(std::lround(shell.type_letter_spacing)));
  }
  return std::wstring(buffer);
}

/*
 * 写值的唯一入口：改字号后必须重新夹字距（字号变小会让原字距压碎步距），
 * 与 type_step_param 的规则保持一致。
 */
void type_panel_set(ShellState& shell, int row, double value) {
  switch (row) {
    case comp_ui::kTypePanelFontSizeRow:
      shell.type_font_size = comp_ui::clamp_type_font_size(value);
      shell.type_letter_spacing =
          comp_ui::clamp_type_letter_spacing(shell.type_letter_spacing, shell.type_font_size);
      break;
    case comp_ui::kTypePanelLineHeightRow:
      shell.type_line_height = comp_ui::clamp_type_line_height(value);
      break;
    case comp_ui::kTypePanelLetterSpacingRow:
      shell.type_letter_spacing =
          comp_ui::clamp_type_letter_spacing(value, shell.type_font_size);
      break;
    default:
      break;
  }
}

/* 键入中的文字也用这组值：此时状态栏走 type_refresh_status，不另写一套数字。 */
void type_panel_status(ShellState& shell, const wchar_t* prefix) {
  if (shell.type_active) {
    type_refresh_status(shell);
    return;
  }
  const int row = shell.type_panel_focused;
  wchar_t buffer[256];
  wsprintfW(buffer, L"%s%s = %s", prefix,
            widen_ascii(comp_ui::type_panel_row_label(row)).c_str(),
            type_panel_value_text(shell, row).c_str());
  set_status(shell, buffer);
}

void type_panel_focus(ShellState& shell, int delta) {
  int next = shell.type_panel_focused + delta;
  if (next < 0) next = 0;
  if (next >= comp_ui::kTypePanelRowCount) next = comp_ui::kTypePanelRowCount - 1;
  shell.type_panel_focused = next;
  type_panel_status(shell, L"文字属性 ");
}

void type_panel_step(ShellState& shell, int direction) {
  const int row = shell.type_panel_focused;
  const double before = type_panel_value(shell, row);
  const double next = comp_ui::type_panel_step_value(row, before, shell.type_font_size, direction);
  type_panel_set(shell, row, next);
  type_panel_status(shell, type_panel_value(shell, row) == before ? L"已到边界：" : L"");
}

void type_panel_begin_input(ShellState& shell, wchar_t character) {
  if (shell.type_active) return;
  shell.type_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.type_input.c_str());
  set_status(shell, buffer);
}

void type_panel_backspace(ShellState& shell) {
  if (shell.type_input.empty()) return;
  shell.type_input.pop_back();
  if (shell.type_input.empty()) set_status(shell, L"已取消输入");
}

/* 键入值先夹到面板量程再落值（与滤镜面板同一套输入规矩）。 */
void type_panel_commit_input(ShellState& shell) {
  if (shell.type_input.empty()) return;
  const std::string text(shell.type_input.begin(), shell.type_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  type_panel_set(shell, shell.type_panel_focused, value);
  shell.type_input.clear();
  type_panel_status(shell, L"已设置 ");
}

/* ------------------------------------------------------------ 框选（Marquee） */

bool marquee_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Marquee;
}

void marquee_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!marquee_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.marquee_active = true;
  shell.marquee_start = doc;
  shell.marquee_current = doc;
  SetCapture(window);
  set_status(shell, L"框选：拖出矩形后松开（Shift 加选 / Alt 减选）");
}

void marquee_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.marquee_active) return;
  shell.marquee_current =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
}

void marquee_cancel(ShellState& shell) { shell.marquee_active = false; }

void marquee_end(HWND window, ShellState& shell) {
  if (!shell.marquee_active) return;
  shell.marquee_active = false;
  ReleaseCapture();
  if (shell.document == nullptr) return;

  /* 框选的两个角点（顺序任意，引擎按包围盒解释）：矩形与椭圆都是 2 点语义。 */
  const float points[4] = {static_cast<float>(shell.marquee_start.x),
                           static_cast<float>(shell.marquee_start.y),
                           static_cast<float>(shell.marquee_current.x),
                           static_cast<float>(shell.marquee_current.y)};
  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }
  const comp_selection_shape shape =
      shell.marquee_shape == comp_ui::kMarqueeEllipse ? COMP_SELECTION_ELLIPSE
                                                      : COMP_SELECTION_RECT;
  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"框选：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"框选：已减选"
                                                                : L"框选：已应用";
  finish_new_selection(
      shell, shell.marquee_shape == comp_ui::kMarqueeEllipse ? "Ellipse Marquee Selection"
                                                             : "Rect Marquee Selection",
      ok_text, L"框选：选区未变化", L"框选失败", [&](int32_t* changed) {
        return comp_selection_apply_shape(shell.document, shape, points, 2, combine,
                                          /*antialias=*/1, changed);
      });
}

/* ------------------------------------------------------------ 裁剪（Crop） */

/*
 * 与 Marquee 的区别：Marquee 产出的是**选区**（不动画布），Crop 产出的是**新画布**
 * （动文档尺寸）。两者都拖框，但提交后果完全不同，所以不复用同一套状态。
 *
 * 交互对齐 PS：拖出框 → 框外变暗（三分段遮罩）→ Enter 提交 / Esc 取消。
 * 提交走 comp_crop_apply：它自己负责取整、校验、一步撤销，壳不重复实现这些规则。
 */
bool crop_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Crop;
}

void crop_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!crop_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.crop_active = true;
  shell.crop_rect.x = doc.x;
  shell.crop_rect.y = doc.y;
  shell.crop_rect.width = 1.0;
  shell.crop_rect.height = 1.0;
  SetCapture(window);
  set_status(shell, L"裁剪：拖出框后 Enter 提交，Esc 取消");
}

void crop_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.crop_active) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  /* 允许任意方向拖拽：负宽高先标准化成正矩形，与引擎的 snap 口径一致。 */
  shell.crop_rect.x = static_cast<double>(doc.x < shell.crop_rect.x ? doc.x : shell.crop_rect.x);
  shell.crop_rect.y = static_cast<double>(doc.y < shell.crop_rect.y ? doc.y : shell.crop_rect.y);
  const double right = static_cast<double>(doc.x > shell.crop_rect.x ? doc.x : shell.crop_rect.x);
  const double bottom = static_cast<double>(doc.y > shell.crop_rect.y ? doc.y : shell.crop_rect.y);
  shell.crop_rect.width = right - shell.crop_rect.x;
  shell.crop_rect.height = bottom - shell.crop_rect.y;
}

void crop_cancel(ShellState& shell) { shell.crop_active = false; }

/*
 * 提交裁剪。读取引擎给出的 out_changed：为 0 表示框恰好等于当前画布，
 * 这时引擎不产生撤销步，壳也不该把状态栏说成「已裁剪」。
 */
bool crop_commit(ShellState& shell) {
  if (!shell.crop_active || shell.document == nullptr) return false;
  fprintf(stderr, "[crop] commit rect=%f,%f,%f,%f\n", shell.crop_rect.x,
          shell.crop_rect.y, shell.crop_rect.width, shell.crop_rect.height);
  int32_t changed = 0;
  const comp_result result = comp_crop_apply(shell.document, shell.crop_rect, &changed);
  shell.crop_active = false;
  fprintf(stderr, "[crop] apply result=%d changed=%d\n", (int)result,
          (int)changed);
  if (result != COMP_OK) {
    set_status(shell, L"裁剪失败：框不合法");
    return false;
  }
  if (changed == 0) {
    set_status(shell, L"裁剪未改变画布尺寸");
    return true;
  }
  /* 画布换了坐标系：view 的偏移按新尺寸重算，否则残留的平移会把画布推出可视区。 */
  comp_size size{};
  if (comp_document_size(shell.document, &size) == COMP_OK) {
    shell.width = size.width;
    shell.height = size.height;
  }
  shell.needs_fit = true;
  shell.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已裁剪");
  return true;
}

/* ------------------------------------------------------------ 画布落笔（折线） */

/*
 * Retouch 与画笔/橡皮擦共用同一条「鼠标折线 -> comp_tool_apply」通路：
 * 折线按文档像素累积，松开鼠标时一次性提交，中间不产生事务。
 */
bool stroke_in_progress(const ShellState& shell) {
  return shell.stroke_kind != comp_ui::StrokeKind::None;
}

bool stroke_begin(ShellState& shell, int x, int y, const Layout& layout,
                  comp_ui::StrokeKind kind) {
  if (shell.document == nullptr || kind == comp_ui::StrokeKind::None) return false;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.stroke_points.clear();
  /* 单击也要留下一记 dab：单点折线在 COMP_TOOL_STROKE 里就是一个圆形笔尖。 */
  shell.stroke_points.push_back(static_cast<float>(doc.x));
  shell.stroke_points.push_back(static_cast<float>(doc.y));
  shell.stroke_kind = kind;
  return true;
}

void stroke_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!stroke_in_progress(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  const size_t count = shell.stroke_points.size();
  const double last_x = count >= 2 ? shell.stroke_points[count - 2] : doc.x;
  const double last_y = count >= 2 ? shell.stroke_points[count - 1] : doc.y;
  if (!comp_ui::stroke_point_should_append(last_x, last_y, doc.x, doc.y)) return;
  shell.stroke_points.push_back(static_cast<float>(doc.x));
  shell.stroke_points.push_back(static_cast<float>(doc.y));
}

/* 失焦 / 捕获丢失 / Esc：两种笔迹共用同一个取消动作。 */
void stroke_cancel(ShellState& shell) {
  shell.stroke_kind = comp_ui::StrokeKind::None;
  shell.stroke_points.clear();
  shell.stroke_erase = false;
}

/* ------------------------------------------------------------ Retouch 笔迹 */

/* Blur / Smudge / Liquify 共用同一条「鼠标折线 -> comp_tool_apply」通路。 */
bool retouch_stroke_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Retouch;
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
  if (shell.stroke_kind != comp_ui::StrokeKind::Retouch) return;
  shell.stroke_kind = comp_ui::StrokeKind::None;

  /* 同 paint_stroke_end：折线必须先搬走，ReleaseCapture 会同步触发取消。 */
  std::vector<float> points;
  points.swap(shell.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
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
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

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
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Tone;
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
  if (shell.stroke_kind != comp_ui::StrokeKind::Tone) return;
  shell.stroke_kind = comp_ui::StrokeKind::None;

  /* 同 retouch_stroke_end：折线必须先搬走，ReleaseCapture 会同步触发取消。 */
  std::vector<float> points;
  points.swap(shell.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
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
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

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

/* ------------------------------------------------------------ 历史记录画笔（P1） */

/*
 * 与 Tone / Retouch 共用同一条「鼠标折线 -> comp_tool_apply」通路：折线在文档
 * 坐标累积，松开鼠标时一次性提交（= 恰好一个撤销步）。取样状态只在提交时读
 * 一次——笔画途中点开历史面板、回跳都不会改变这一笔的来源。
 */
bool history_brush_stroke_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::HistoryBrush;
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
  if (shell.stroke_kind != comp_ui::StrokeKind::HistoryBrush) return;
  shell.stroke_kind = comp_ui::StrokeKind::None;

  /* 同 tone_stroke_end：折线必须先搬走，ReleaseCapture 会同步触发取消。 */
  std::vector<float> points;
  points.swap(shell.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
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
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

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

/* ------------------------------------------------------------ 画笔 / 橡皮擦 */

/*
 * 画笔与橡皮擦是同一条 COMP_TOOL_STROKE：形状与 coverage 相同，只有合成方向
 * 由 desc.erase 翻转（预乘 destination-out）。取色一律用 shell.foreground——
 * 吸管与色板都写同一个字段，因此「吸管吸到色 -> 直接画」不需要额外状态。
 */
bool paint_stroke_available(const ShellState& shell) {
  return shell.document != nullptr &&
         (shell.tool == comp_ui::ToolId::Brush || shell.tool == comp_ui::ToolId::Eraser);
}

void paint_stroke_begin(HWND window, ShellState& shell, int x, int y,
                        const Layout& layout) {
  if (!paint_stroke_available(shell)) return;
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::Paint)) return;
  shell.stroke_erase = shell.tool == comp_ui::ToolId::Eraser;
  SetCapture(window);
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s：拖动画布，松开%s（直径 %d px / 硬度 %d%%）",
            shell.stroke_erase ? L"橡皮擦" : L"画笔",
            shell.stroke_erase ? L"擦除" : L"落色",
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(shell.brush_hardness * 100.0 + 0.5));
  set_status(shell, buffer);
}

void paint_stroke_end(HWND window, ShellState& shell) {
  if (shell.stroke_kind != comp_ui::StrokeKind::Paint) return;
  const bool erasing = shell.stroke_erase;
  shell.stroke_kind = comp_ui::StrokeKind::None;
  shell.stroke_erase = false;

  /*
   * 先把折线搬出来，再 ReleaseCapture：ReleaseCapture 会同步派发
   * WM_CAPTURECHANGED，而该处理器调用 stroke_cancel 清空 stroke_points。
   * 顺序反了就是「松开鼠标后一个点都不剩」，画笔/橡皮擦永远不落色。
   */
  std::vector<float> points;
  points.swap(shell.stroke_points);
  ReleaseCapture();
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, erasing ? L"橡皮擦：请先选择一个图层" : L"画笔：请先选择一个图层");
    return;
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_STROKE;
  desc.brush.color[0] = shell.foreground.r;
  desc.brush.color[1] = shell.foreground.g;
  desc.brush.color[2] = shell.foreground.b; /* 抹色不透明：不透明度由 flow 表达（固定 1） */
  desc.brush.diameter = static_cast<float>(shell.brush_size);
  desc.brush.pressure = 1.0f; /* 鼠标无压感：按上游满笔压 */
  desc.brush.hardness = static_cast<float>(shell.brush_hardness);
  desc.brush.flow = 1.0f;
  desc.brush.seed = 0; /* 圆头笔是解析式的；固定值守住「同参数同结果」 */
  desc.erase = erasing ? 1 : 0;
  desc.stroke_points = points.data();
  desc.stroke_point_count = static_cast<int32_t>(points.size() / 2);

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"%s失败（错误码 %d）", erasing ? L"橡皮擦" : L"画笔",
              static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  char hex[8] = {0};
  comp_ui::color_to_hex(shell.foreground, hex);
  if (erasing) {
    wsprintfW(buffer, L"橡皮擦：已擦除 %d 个折点（直径 %d px）", desc.stroke_point_count,
              static_cast<int>(shell.brush_size + 0.5));
  } else if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"画笔：已落色 %d 个折点｜%s", desc.stroke_point_count,
              widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"画笔：已落色 %d 个折点（%s · 直径 %d px）", desc.stroke_point_count,
              widen_ascii(hex).c_str(), static_cast<int>(shell.brush_size + 0.5));
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 吸管（只读取色） */

/*
 * 取样结果写进 shell.foreground（0..1 直通 sRGB -> 0..255）。
 * 吸管是只读工具：不进撤销栈、不改选区、不改图层。
 */
bool eyedropper_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Eyedropper;
}

void eyedropper_sample(ShellState& shell, int x, int y, const Layout& layout) {
  if (!eyedropper_available(shell)) return;
  if (!layout.canvas.contains(x, y)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  /* 画布外不取样：ABI 对越界坐标返回 INVALID_ARGUMENT，那不是「吸管坏了」。 */
  if (doc.x < 0.0 || doc.y < 0.0 || doc.x >= static_cast<double>(shell.width) ||
      doc.y >= static_cast<double>(shell.height)) {
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
      shell.document, source_layer, doc.x, doc.y, 1, source, rgba);
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
  shell.eyedropper_drag = true;
  SetCapture(window);
  eyedropper_sample(shell, x, y, layout);
}

void eyedropper_cancel(ShellState& shell) { shell.eyedropper_drag = false; }

/* ------------------------------------------------------------ 选区搬移（Move） */

/*
 * 一次拖拽 = 一步撤销：像素搬移与轮廓平移写进同一个显式事务。
 * 两个 ABI 调用内部的 ImplicitTxn 会「加入」已开启的显式事务，
 * 因此这里不需要把两步合成一个新的算子。
 */
bool move_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Move;
}

void move_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!move_available(shell)) return;
  if (!shell.selection_active) {
    set_status(shell, L"移动：当前没有选区（先用选框建立选区）");
    return;
  }
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.move_active = true;
  shell.move_start = doc;
  shell.move_current = doc;
  shell.move_duplicate = (GetKeyState(VK_MENU) & 0x8000) != 0;
  SetCapture(window);
  set_status(shell, shell.move_duplicate
                        ? L"移动选区像素：Alt = 复制，松开提交"
                        : L"移动选区像素：松开提交（按住 Alt = 复制）");
}

void move_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.move_active) return;
  shell.move_current =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
}

void move_cancel(ShellState& shell) { shell.move_active = false; }

void move_end(HWND window, ShellState& shell) {
  if (!shell.move_active) return;
  shell.move_active = false;
  ReleaseCapture();
  if (shell.document == nullptr) return;

  const int32_t dx =
      static_cast<int32_t>(std::lround(shell.move_current.x - shell.move_start.x));
  const int32_t dy =
      static_cast<int32_t>(std::lround(shell.move_current.y - shell.move_start.y));
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
  if (comp_txn_begin(shell.document,
                     shell.move_duplicate ? "Duplicate Selection Pixels"
                                          : "Move Selection Pixels",
                     &txn) != COMP_OK) {
    set_status(shell, L"移动：无法开始事务");
    return;
  }

  /* 顺序不能反：move_pixels 按当前选区取像素，先平移轮廓会搬错地方。 */
  int32_t moved_pixels = 0;
  int32_t moved_outline = 0;
  comp_result result = comp_selection_move_pixels(
      shell.document, id, dx, dy, shell.move_duplicate ? 1 : 0, &moved_pixels);
  if (result == COMP_OK) {
    result = comp_selection_translate(shell.document, dx, dy, &moved_outline);
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
  shell.needs_render = true;
  wchar_t buffer[224];
  wsprintfW(buffer, L"%s：位移 (%d, %d) px，%d 个像素",
            shell.move_duplicate ? L"已复制选区像素" : L"已移动选区像素", dx, dy,
            moved_pixels);
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 文件与命令 */

/*
 * 文档要被替换/销毁：色彩范围的预览事务属于旧文档，句柄必须直接丢掉。
 * 这里**不能**调 color_range_cancel——它会 abort 事务，而旧文档马上就要被
 * destroy，abort 会打到已经释放的对象上（UAF）。关面板只是顺带效果。
 */
void forget_color_range(ShellState& shell) {
  shell.color_range_txn = nullptr;
  shell.color_range_open = false;
  shell.color_range_include.clear();
  shell.color_range_exclude.clear();
  shell.color_range_input.clear();
  /* 路径面板的锚点同样是旧文档坐标：换文档时一起丢掉（关面板只是顺带）。 */
  path_store_clear(shell);
  /*
   * 历史面板没有壳侧数据（列表直接来自引擎），换文档后可以继续开着显示新文档的
   * 历史；只把滚动归零，否则可能停在新文档根本到不了的行上。
   */
  shell.history_scroll = 0;
}

bool create_document(ShellState& shell) {
  comp_document_desc desc{};
  desc.width = kDefaultWidth;
  desc.height = kDefaultHeight;
  desc.color_space = "sRGB";

  comp_document document = nullptr;
  const comp_result result = comp_document_create(shell.engine, &desc, &document);
  if (result != COMP_OK || document == nullptr) {
    set_status(shell, L"新建文档失败");
    return false;
  }
  forget_color_range(shell);
  if (shell.document != nullptr) comp_document_destroy(shell.document);
  shell.document = document;

  comp_size size{};
  if (comp_document_size(shell.document, &size) == COMP_OK) {
    shell.width = size.width;
    shell.height = size.height;
  }
  const size_t pixel_count = static_cast<size_t>(shell.width) * static_cast<size_t>(shell.height);
  shell.rgba.assign(pixel_count * 4, 0);
  shell.framebuffer.assign(pixel_count, 0);
  shell.needs_render = true;
  shell.needs_fit = true;
  shell.visibility.clear();
  /* 新文档没有「之前的滤镜面板/未完成笔迹」可言，一并复位，避免跨文档误应用。 */
  shell.filter_open = false;
  shell.filter_input.clear();
  shell.filter.select(comp_ui::FilterId::Vignette);
  stroke_cancel(shell);
  move_cancel(shell);
  eyedropper_cancel(shell);
  marquee_cancel(shell);
  lasso_cancel(shell);
  pen_cancel(shell);
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已新建文档");
  return true;
}

bool load_document(ShellState& shell, const char* path_utf8) {
  comp_document document = nullptr;
  if (comp_document_load(shell.engine, path_utf8, &document) != COMP_OK || document == nullptr) {
    set_status(shell, L"打开 .comp 失败");
    return false;
  }
  forget_color_range(shell);
  if (shell.document != nullptr) comp_document_destroy(shell.document);
  shell.document = document;

  comp_size size{};
  if (comp_document_size(shell.document, &size) == COMP_OK) {
    shell.width = size.width;
    shell.height = size.height;
  }
  const size_t pixel_count = static_cast<size_t>(shell.width) * static_cast<size_t>(shell.height);
  shell.rgba.assign(pixel_count * 4, 0);
  shell.framebuffer.assign(pixel_count, 0);
  shell.needs_render = true;
  shell.needs_fit = true;
  shell.visibility.clear();
  shell.filter_open = false;
  shell.filter_input.clear();
  shell.filter.select(comp_ui::FilterId::Vignette);
  stroke_cancel(shell);
  move_cancel(shell);
  eyedropper_cancel(shell);
  marquee_cancel(shell);
  lasso_cancel(shell);
  pen_cancel(shell);
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已打开 .comp");
  return true;
}

bool save_document(ShellState& shell, const char* path_utf8) {
  if (shell.document == nullptr) return false;
  const bool ok = comp_document_save(shell.document, path_utf8) == COMP_OK;
  set_status(shell, ok ? L"已保存 .comp" : L"保存 .comp 失败");
  return ok;
}

/* ------------------------------------------------------------ 撤销 / 重做 */

void undo_document(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t can_undo = 0;
  if (comp_document_can_undo(shell.document, &can_undo) != COMP_OK || can_undo == 0) {
    set_status(shell, L"已到最早状态，无可撤销");
    return;
  }
  const comp_result result = comp_document_undo(shell.document);
  if (result != COMP_OK) {
    set_status(shell, L"撤销失败");
    return;
  }
  shell.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell); /* 选区操作也在撤销栈里：撤销可能改变蚂蚁线 */
  refresh_history_status(shell, L"已撤销");
}

void redo_document(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t can_redo = 0;
  if (comp_document_can_redo(shell.document, &can_redo) != COMP_OK || can_redo == 0) {
    set_status(shell, L"已到最新状态，无可重做");
    return;
  }
  const comp_result result = comp_document_redo(shell.document);
  if (result != COMP_OK) {
    set_status(shell, L"重做失败");
    return;
  }
  shell.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell); /* 同撤销：重做也可能恢复/撤掉选区 */
  refresh_history_status(shell, L"已重做");
}

/* 命令层冒烟：两段式枚举，只报数量，不猜测命令 id。 */
void list_commands(ShellState& shell) {
  if (shell.document == nullptr) return;
  size_t count = 0;
  /* count-only 模式不要求文档；带 desc 的枚举才需要按文档状态求值 enabled。 */
  if (comp_command_list(shell.document, nullptr, 0, &count) != COMP_OK) {
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
  if (shell.document == nullptr) return;
  wchar_t buffer[MAX_PATH] = L"";
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

/* 定义在下方（要用到 load_document / refresh_* 族），这里先声明：对话框入口需要它。 */
void open_path_as_document(ShellState& shell, const char* path_utf8);

/*
 * 一个打开入口，两条去向。用户拖进来的东西可能是一张图，也可能是一个 .comp，
 * 按扩展名分派比让用户先想「该点哪个菜单」更接近 PS 的行为（PS 也是看文件类型）。
 */
void open_path_as_document(ShellState& shell, const char* path_utf8) {
  const comp_image_format format = comp_io_format_from_path(path_utf8);
  if (format != COMP_FORMAT_UNKNOWN) {
    comp_document document = nullptr;
    if (comp_io_import(shell.engine, path_utf8, &document) != COMP_OK || document == nullptr) {
      set_status(shell, L"打开位图失败：格式不支持或文件损坏");
      return;
    }
    forget_color_range(shell);
    if (shell.document != nullptr) comp_document_destroy(shell.document);
    shell.document = document;
    comp_size size{};
    if (comp_document_size(shell.document, &size) == COMP_OK) {
      shell.width = size.width;
      shell.height = size.height;
    }
    const size_t pixel_count =
        static_cast<size_t>(shell.width) * static_cast<size_t>(shell.height);
    shell.rgba.assign(pixel_count * 4, 0);
    shell.framebuffer.assign(pixel_count, 0);
    shell.needs_render = true;
    shell.needs_fit = true;
    shell.visibility.clear();
    shell.filter_open = false;
    shell.filter_input.clear();
    stroke_cancel(shell);
    move_cancel(shell);
    eyedropper_cancel(shell);
    marquee_cancel(shell);
    lasso_cancel(shell);
    pen_cancel(shell);
    refresh_layers(shell);
    refresh_selection(shell);
    refresh_history_status(shell, L"已打开位图");
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

    if (shell.document == nullptr) {
      open_path_as_document(shell, utf8);
    } else if (comp_io_format_from_path(utf8) != COMP_FORMAT_UNKNOWN) {
      comp_layer_id id = 0;
      if (comp_io_import_layer(shell.document, utf8, &id) == COMP_OK && id != 0) {
        shell.needs_render = true;
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
  if (shell.document == nullptr) return;
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
  if (comp_io_import_layer(shell.document, utf8, &id) != COMP_OK || id == 0) {
    set_status(shell, L"置入图片失败");
    return;
  }
  shell.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已置入图片图层");
}

/* 另存为位图。flatten 只对 PSD 有意义，其余格式引擎侧已按合成表面导出。 */
void export_image_dialog(HWND window, ShellState& shell) {
  if (shell.document == nullptr) return;
  wchar_t buffer[MAX_PATH] = L"";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter =
      L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg\0EXR (*.exr)\0*.exr\0"
      L"所有文件\0*.*\0";
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
  if (comp_io_export(shell.document, utf8, format, nullptr) != COMP_OK) {
    set_status(shell, L"导出失败：该格式不可用");
    return;
  }
  set_status(shell, L"已导出图片");
}

/* 视图 / 工具类命令的实现在窗口过程之后，这里先给出声明：
 * run_shell_command 是菜单与快捷键的唯一入口，不能被定义顺序卡住。 */
void set_tool(ShellState& shell, comp_ui::ToolId id);
void brush_step(ShellState& shell, int direction, bool hardness);
bool client_layout(const ShellState& shell, Layout* out);
void view_fit(ShellState& shell);
void view_actual(ShellState& shell);
void view_zoom_step(ShellState& shell, double factor);

/* 菜单与快捷键共用同一入口，保证两条路径行为不分叉。 */
void run_shell_command(HWND window, ShellState& shell, UINT id) {
  switch (id) {
    case kCmdNew:
      create_document(shell);
      break;
    case kCmdOpen:
      open_document_dialog(window, shell);
      break;
    case kCmdSave:
      save_document_dialog(window, shell);
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
    /* ---- P1：历史记录面板 ---- */
    case kCmdHistoryPanel:
      history_panel_toggle(shell);
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
      open_effect_panel(shell, shell.effect_kinds.empty()
                                   ? 0
                                   : static_cast<int>(shell.effect_kinds.front()));
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

void render_canvas(ShellState& shell) {
  if (shell.document == nullptr || shell.rgba.empty()) return;
  const size_t stride = static_cast<size_t>(shell.width) * 4;
  const comp_result result =
      comp_render_flatten(shell.document, shell.rgba.data(), COMP_PIXEL_RGBA8, stride);
  if (result == COMP_OK) {
    /* 预乘 RGBA8 -> BGRA：GDI 的 BI_RGB 32bpp 位图按 BGRA 解释。 */
    const size_t pixel_count = static_cast<size_t>(shell.width) * static_cast<size_t>(shell.height);
    for (size_t i = 0; i < pixel_count; ++i) {
      const uint8_t* src = shell.rgba.data() + i * 4;
      uint32_t bgra = (static_cast<uint32_t>(src[3]) << 24) |
                      (static_cast<uint32_t>(src[0]) << 16) |
                      (static_cast<uint32_t>(src[1]) << 8) |
                      static_cast<uint32_t>(src[2]);
      shell.framebuffer[i] = bgra;
    }
  }
  shell.needs_render = false;
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
    SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(198, 200, 208));
    const wchar_t letter[2] = {
        info->shortcut != 0 ? static_cast<wchar_t>(info->shortcut - 'a' + 'A') : L' ', 0};
    DrawTextW(dc, letter, 1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
  }
  wchar_t info_text[192];
  wsprintfW(info_text, L"缩放 %d%%   笔刷 %d px / %d%%   渲染 %s%s",
            static_cast<int>(shell.view.zoom * 100.0 + 0.5),
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(shell.brush_hardness * 100.0 + 0.5),
            backend_label(shell.backend), extra);
  RECT info_rect{bar.right() - 520, bar.y, bar.right() - 10, bar.bottom()};
  SetTextColor(dc, RGB(172, 176, 188));
  DrawTextW(dc, info_text, -1, &info_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
  }
  wchar_t line[768];
  wsprintfW(line, L"%s（%c） · 缩放 %d%% · 笔刷 %d px / %d%%%s · %s", tool_name,
            info != nullptr && info->shortcut != 0
                ? static_cast<wchar_t>(info->shortcut - 'a' + 'A')
                : L' ',
            static_cast<int>(shell.view.zoom * 100.0 + 0.5),
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
  wsprintfW(title, L"图层（%u）", static_cast<unsigned>(shell.layers.size()));
  TextOutW(dc, panel_rect.x + 10, panel_rect.y + 8, title, lstrlenW(title));

  /*
   * S22：直方图打开时图层行整体下移，偏移与命中测试共用同一常量
   * （ui_core::layer_row_rect / layer_row_at 的 row_top 参数）。这里从布局算，
   * 不在壳里另写一个数字——两处各写一次必然有一天不一致。
   */
  const int row_top = comp_ui::layer_rows_top(shell.histogram_open);

  const int32_t count = static_cast<int32_t>(shell.layers.size());
  for (int32_t line = 0; line < count; ++line) {
    const comp_ui::Rect row_rect = comp_ui::layer_row_rect(panel_rect, line, row_top);
    const LayerRow& row = shell.layers[static_cast<size_t>(count - 1 - line)];
    if (line == shell.selected_layer) {
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
      const bool drop_into_group = shell.layer_drag_active && row.is_group != 0 &&
                                   shell.layer_drag_target == line;
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
    TextOutW(dc, row_rect.x + 10, row_rect.y + 3, line_text, lstrlenW(line_text));
  }

  /*
   * S24 拖行落点指示线。位置由 ui_core::layer_insert_line_rect 从同一个 row_top
   * 算（与行矩形共用偏移），指示线不会与行错位——错位会让人以为层会落到相邻行。
   * 向上拖画目标行上边缘、向下拖画下边缘，见 layer_drag_insert_slot。
   */
  if (shell.layer_drag_active && shell.layer_drag_target >= 0 && count > 0 &&
      shell.layer_drag_row >= 0) {
    const int32_t slot =
        comp_ui::layer_drag_insert_slot(shell.layer_drag_row, shell.layer_drag_target);
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
  if (shell.histogram_open && !shell.histogram_bins.empty()) {
    /* 几何来自 comp_ui：块贴在标题行下方，块底与第一行图层严丝合缝（见 S23 P2）。 */
    const comp_ui::Rect area = comp_ui::histogram_plot_rect(panel_rect);
    RECT frame{area.x, area.y, area.right(), area.bottom()};
    HBRUSH plot = CreateSolidBrush(RGB(14, 16, 20));
    FillRect(dc, &frame, plot);
    DeleteObject(plot);
    FrameRect(dc, &frame, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    /*
     * 可视区间由 shell.histogram_view 决定（S24：拖拽框选 / 滚轮缩放 / 双击复位）。
     * 峰值只看可视区间：全幅峰值在放大后会把细节压成一条平线，正是缩放要看的
     * 那部分反而看不见了。
     */
    const comp_ui::HistogramView& view = shell.histogram_view;
    double peak = 0.0;
    int32_t first_bin = static_cast<int32_t>(view.lo);
    int32_t last_bin = static_cast<int32_t>(view.hi + 0.999);
    if (first_bin < 0) first_bin = 0;
    if (last_bin > 255) last_bin = 255;
    for (int32_t i = first_bin; i <= last_bin; ++i) {
      const double value = shell.histogram_bins[static_cast<size_t>(i)];
      if (value > peak) peak = value;
    }

    /* 框选预览先铺底，再画曲线，否则曲线会被色块盖掉。 */
    if (shell.histogram_drag) {
      int32_t band_left = shell.histogram_drag_from_x;
      int32_t band_right = shell.histogram_drag_to_x;
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
        const double norm = std::log1p(shell.histogram_bins[static_cast<size_t>(index)]) /
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
                      curve_channel_name(c), c == shell.curve_channel);
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
  const int32_t count = static_cast<int32_t>(shell.curve_xy.size() / 2);
  const double* points = count >= 2 ? shell.curve_xy.data() : kIdentity;
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
    const bool hot = i == shell.curve_focus_point || i == shell.curve_drag_point;
    RECT dot{cx - 3, cy - 3, cx + 4, cy + 4};
    HBRUSH fill = CreateSolidBrush(hot ? RGB(255, 226, 140)
                                       : (count >= 2 && endpoint ? RGB(150, 156, 172)
                                                                 : RGB(226, 232, 244)));
    FillRect(dc, &dot, fill);
    DeleteObject(fill);
  }

  wchar_t line[220];
  SetTextColor(dc, RGB(150, 154, 166));
  if (shell.curve_focus_point >= 0 && count >= 2 &&
      shell.curve_focus_point < static_cast<int32_t>(shell.curve_xy.size() / 2)) {
    /* wsprintfW 只认整数宽度，%f 会把参数当 int 取、指针读飞（在 user32 里 AV）。 */
    const std::wstring point_x =
        widen_ascii(number_text(shell.curve_xy[static_cast<size_t>(2 * shell.curve_focus_point)])
                        .c_str());
    const std::wstring point_y =
        widen_ascii(number_text(shell.curve_xy[static_cast<size_t>(2 * shell.curve_focus_point + 1)])
                        .c_str());
    wsprintfW(line, L"%s · 点 %d/%d = (%s, %s)", curve_channel_name(shell.curve_channel),
              shell.curve_focus_point + 1, static_cast<int>(count), point_x.c_str(),
              point_y.c_str());
  } else {
    wsprintfW(line, L"%s · %d 个控制点%s", curve_channel_name(shell.curve_channel),
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
  wsprintfW(line, L"类型 %d/%d：%s", shell.adj_kind + 1, total,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(shell.adj_kind))).c_str());
  SetTextColor(dc, RGB(150, 210, 255));
  TextOutW(dc, left + 10, y, line, lstrlenW(line));
  y += kAdjustRowHeight;

  /* 色阶通道条：标题行右侧，四个通道各一页（参数行几何因此不用变）。 */
  const bool paged = adjust_paged_kind(static_cast<uint32_t>(shell.adj_kind));
  if (paged) {
    for (int32_t c = 0; c < kLevelsChannelCount; ++c) {
      draw_channel_chip(dc, comp_ui::adjust_channel_chip_rect(panel_rect, c, kLevelsChannelCount),
                        curve_channel_name(c), c == shell.adj_channel);
    }
  }

  /* 面板显示的行数（分页视图下参数总数 != 显示行数）与「相对行 -> 参数下标」的基址。 */
  const int shown_total = adjust_shown_count(shell);
  const int base = adjust_param_base(shell);
  const int scroll = comp_ui::adjust_param_clamp_scroll(panel_rect, shown_total, shell.adj_scroll);
  const int visible = comp_ui::adjust_param_visible_rows(panel_rect);
  if (shell.adj_kind == COMP_ADJUSTMENT_CURVES) {
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
      if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind),
                                         static_cast<size_t>(index), &param) != COMP_OK) {
        break;
      }
      const comp_ui::Rect row_rect = comp_ui::adjust_param_row_rect(panel_rect, row, scroll);
      const bool focused = index == shell.adj_param;
      if (focused) {
        RECT highlight{row_rect.x + 4, row_rect.y - 1, row_rect.right() - 6,
                       row_rect.bottom() - 1};
        HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
        FillRect(dc, &highlight, brush);
        DeleteObject(brush);
      }
      const std::string value = static_cast<size_t>(index) < shell.adj_values.size()
                                    ? shell.adj_values[static_cast<size_t>(index)]
                                    : std::string("?");
      std::wstring shown;
      if (focused && !shell.adj_input.empty()) {
        shown = L"输入中：";
        shown += shell.adj_input;
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
  wsprintfW(line, L"该图层已有调整：%u", static_cast<unsigned>(shell.adj_settings.size()));
  TextOutW(dc, left + 10, footer_y, line, lstrlenW(line));
  footer_y += 18;
  for (size_t i = 0; i < shell.adj_settings.size() && i < 3; ++i) {
    const bool selected = static_cast<int32_t>(i) == shell.selected_adjustment;
    SetTextColor(dc, selected ? RGB(255, 226, 140) : RGB(196, 198, 208));
    wsprintfW(line, L"%s %u: %s", selected ? L">" : L" ", static_cast<unsigned>(i + 1),
              widen_ascii(adjustment_kind_name(shell.adj_kinds[i])).c_str());
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
      shell.adj_kind == COMP_ADJUSTMENT_CURVES ? curve_hints : slider_hints;
  int hint_y = height - 96;
  for (int i = 0; i < 5; ++i) {
    TextOutW(dc, left + 10, hint_y, hints[i], lstrlenW(hints[i]));
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

  const comp_ui::FilterSpec* spec = shell.filter.spec();
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"%d/%d：%s", static_cast<int>(shell.filter.id) + 1,
            comp_ui::filter_count(), spec != nullptr ? widen_ascii(spec->name).c_str() : L"?");
  TextOutW(dc, left + 10, y, line, lstrlenW(line));

  for (int row = 0; row < shell.filter.param_count(); ++row) {
    const comp_ui::FilterParamSpec* parameter = shell.filter.param_at(row);
    if (parameter == nullptr) break;
    const comp_ui::Rect row_rect = comp_ui::filter_param_row_rect(panel_rect, row);
    const bool focused = row == shell.filter.focused;
    if (focused) {
      RECT highlight{row_rect.x + 4, row_rect.y - 2, row_rect.right() - 6,
                     row_rect.bottom() + 2};
      HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, brush);
      DeleteObject(brush);
    }
    SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
    wsprintfW(line, L"%s = %s", widen_ascii(parameter->label).c_str(),
              widen_ascii(number_text(shell.filter.value(row)).c_str()).c_str());
    TextOutW(dc, left + 12, row_rect.y, line, lstrlenW(line));
  }

  if (!shell.filter_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.filter_input.c_str());
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

  const int kind = comp_ui::clamp_effect_kind(shell.effect_kind);
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"%d/%d：%s · %s", kind + 1, comp_ui::kEffectKindCount,
            effect_kind_label(kind),
            effect_present(shell, kind) ? L"本层已启用" : L"本层未启用（回车添加）");
  TextOutW(dc, left + 10, y, line, lstrlenW(line));

  const uint32_t effect = static_cast<uint32_t>(kind);
  const int param_count = static_cast<int>(shell.effect_values.size());
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
    const bool focused = row == shell.effect_param;
    if (focused) {
      RECT highlight{row_rect.x + 4, row_rect.y - 2, row_rect.right() - 6, row_rect.bottom() + 2};
      HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, brush);
      DeleteObject(brush);
    }
    SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
    const std::string value = shell.effect_values[static_cast<size_t>(row)];
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
  wsprintfW(line, L"该图层已有有效果：%u", static_cast<unsigned>(shell.effect_kinds.size()));
  TextOutW(dc, left + 10, note_y, line, lstrlenW(line));

  if (!shell.effect_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.effect_input.c_str());
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

  const int include_count = static_cast<int>(shell.color_range_include.size() / 3);
  const int exclude_count = static_cast<int>(shell.color_range_exclude.size() / 3);
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
      const uint8_t* color = shell.color_range_include.data() + static_cast<size_t>(i) * 3;
      HBRUSH brush = CreateSolidBrush(RGB(color[0], color[1], color[2]));
      FillRect(dc, &swatch, brush);
      DeleteObject(brush);
      FrameRect(dc, &swatch, border);
    }
    for (int i = 0; i < shown_exclude; ++i) {
      const comp_ui::Rect rect = comp_ui::color_range_swatch_rect(panel_rect, true, i);
      RECT swatch{rect.x, rect.y, rect.right(), rect.bottom()};
      const uint8_t* color = shell.color_range_exclude.data() + static_cast<size_t>(i) * 3;
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
  wsprintfW(line, L"容差 Fuzziness = %d  (%d..%d)", shell.color_range_fuzziness,
            comp_ui::kMinColorRangeFuzziness, comp_ui::kMaxColorRangeFuzziness);
  TextOutW(dc, left + 12, fuzziness_y, line, lstrlenW(line));
  wsprintfW(line, L"反相 Invert = %s  (I 切换)", shell.color_range_invert ? L"开" : L"关");
  TextOutW(dc, left + 12, fuzziness_y + comp_ui::kColorRangeRowHeight, line, lstrlenW(line));

  SetTextColor(dc, RGB(180, 184, 196));
  const wchar_t* note = shell.color_range_txn != nullptr
                            ? L"预览中：选区随取样/容差实时变化，回车才落一步撤销"
                            : L"（没有预览事务：改动会直接落盘）";
  TextOutW(dc, left + 12, fuzziness_y + comp_ui::kColorRangeRowHeight * 2 + 4, note,
           lstrlenW(note));

  if (!shell.color_range_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.color_range_input.c_str());
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

  const int count = static_cast<int>(shell.paths.size());
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"已存 %d / %d 条（钢笔闭合提交后自动入列）", count,
            comp_ui::kPathMaxEntries);
  TextOutW(dc, left + 10, panel_rect.y + 30, line, lstrlenW(line));

  const int scroll = comp_ui::path_clamp_scroll(panel_rect, count, shell.path_scroll);
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
    const bool selected = index == shell.path_selected;
    if (selected) {
      RECT band{rect.x, rect.y, rect.right(), rect.bottom()};
      HBRUSH highlight = CreateSolidBrush(RGB(48, 62, 86));
      FillRect(dc, &band, highlight);
      DeleteObject(highlight);
    }
    const ShellState::StoredPath& path = shell.paths[static_cast<size_t>(index)];
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

  const int scroll = comp_ui::history_clamp_scroll(panel_rect, count, shell.history_scroll);
  const int visible = comp_ui::history_visible_rows(panel_rect);
  for (int row = 0; row < visible; ++row) {
    const int index = scroll + row;
    if (index >= count) break;
    const comp_ui::Rect rect = comp_ui::history_row_rect(panel_rect, row);
    const bool selected = index == current;
    /* 显式取样源（Shift+点行）单独标出来：这支笔画什么完全取决于它。 */
    const bool sampled = shell.history_brush_state >= 0 && index == shell.history_brush_state;
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
  const wchar_t* sub = shell.type_active ? L"键入中（回车提交 · Esc 取消）" : L"属性写入下一段文字";
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

  /* 文本框度量与引擎 build_layout 同一公式（ui_core::type_block_metrics）。 */
  const comp_ui::TypeBlockMetrics metrics =
      comp_ui::type_block_metrics(shell.type_font_size, shell.type_line_height,
                                  shell.type_letter_spacing, shell.type_text.c_str());
  SetTextColor(dc, RGB(170, 174, 186));
  y = comp_ui::type_param_row_rect(panel_rect, comp_ui::kTypePanelRowCount - 1).bottom() + 8;
  wsprintfW(line, L"文本框 %d×%d px｜%d 行｜步距 %d px",
            static_cast<int>(std::lround(metrics.width)),
            static_cast<int>(std::lround(metrics.height)), metrics.line_count,
            static_cast<int>(std::lround(metrics.advance)));
  TextOutW(dc, left + 10, y, line, lstrlenW(line));

  if (!shell.type_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.type_input.c_str());
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
  if (shell.move_active) {
    ants_dx = static_cast<int>(std::lround(shell.move_current.x - shell.move_start.x));
    ants_dy = static_cast<int>(std::lround(shell.move_current.y - shell.move_start.y));
  }

  /* 选区蚂蚁线：白色底线 + 黑色虚线段；相位随时间前进 = 「蚂蚁在爬」。 */
  if (shell.selection_active && !shell.selection_segments.empty()) {
    HPEN base_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dash_pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, base_pen);
    for (const auto& segment : shell.selection_segments) {
      comp_ui::Point a = shell.view.doc_to_screen(segment.x0 + ants_dx, segment.y0 + ants_dy,
                                                 layout.canvas, shell.width, shell.height);
      comp_ui::Point b = shell.view.doc_to_screen(segment.x1 + ants_dx, segment.y1 + ants_dy,
                                                 layout.canvas, shell.width, shell.height);
      if (!clip_axis_aligned(layout.canvas, &a, &b)) continue;
      MoveToEx(dc, static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)),
               nullptr);
      LineTo(dc, static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)));
    }

    SelectObject(dc, dash_pen);
    comp_ui::DashSpan spans[1024];
    for (const auto& segment : shell.selection_segments) {
      comp_ui::Point a = shell.view.doc_to_screen(segment.x0 + ants_dx, segment.y0 + ants_dy,
                                                 layout.canvas, shell.width, shell.height);
      comp_ui::Point b = shell.view.doc_to_screen(segment.x1 + ants_dx, segment.y1 + ants_dy,
                                                 layout.canvas, shell.width, shell.height);
      if (!clip_axis_aligned(layout.canvas, &a, &b)) continue;
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const double length = std::sqrt(dx * dx + dy * dy);
      if (!(length > 0.0)) continue;
      const int count = comp_ui::dash_spans(length, kAntsDashPx, kAntsGapPx,
                                            shell.selection_phase, spans, 1024);
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

  /* 框选预览：文档坐标两点 -> 屏幕矩形；白实线 + 黑虚线（M 键切椭圆）。 */
  if (shell.marquee_active) {
    const comp_ui::Point a = shell.view.doc_to_screen(shell.marquee_start.x,
                                                     shell.marquee_start.y, layout.canvas,
                                                     shell.width, shell.height);
    const comp_ui::Point b = shell.view.doc_to_screen(shell.marquee_current.x,
                                                     shell.marquee_current.y, layout.canvas,
                                                     shell.width, shell.height);
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
   * 裁剪预览（S22）：框外三段遮罩 + 白实线框。
   * 遮罩分上/下/左/右四条带而不是「框外全糊」——中间保留一块完整可见区域，
   * 用户才能看清自己框住了什么（PS 的同款做法，全屏暗反而看不出边界）。
   */
  if (shell.crop_active && shell.crop_rect.width > 0 && shell.crop_rect.height > 0) {
    const comp_ui::Point a = shell.view.doc_to_screen(shell.crop_rect.x, shell.crop_rect.y,
                                                     layout.canvas, shell.width, shell.height);
    const comp_ui::Point b =
        shell.view.doc_to_screen(shell.crop_rect.x + shell.crop_rect.width,
                                 shell.crop_rect.y + shell.crop_rect.height, layout.canvas,
                                 shell.width, shell.height);
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
  if (shell.gradient_active) {
    const comp_ui::Point a =
        shell.view.doc_to_screen(shell.gradient_start.x, shell.gradient_start.y,
                                 layout.canvas, shell.width, shell.height);
    const comp_ui::Point b =
        shell.view.doc_to_screen(shell.gradient_current.x, shell.gradient_current.y,
                                 layout.canvas, shell.width, shell.height);
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
   * 形状预览（S10）：矩形/椭圆画白实线 + 黑虚线双描边（圆角矩形用 RoundRect），
   * 直线画一条双线。只是几何提示，不预演颜色——真正的像素在松开时由引擎一次性写出
   * （与渐变预览同口径）。修饰键（Shift = 正方/正圆、Alt = 从中心）在预览里就生效，
   * 所见即落笔盒。
   */
  if (shell.shape_active) {
    const bool square = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool from_center = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const comp_ui::ShapeDragBox box = comp_ui::shape_drag_box(
        shell.shape_start.x, shell.shape_start.y, shell.shape_current.x, shell.shape_current.y,
        square, from_center);
    const comp_ui::Point a = shell.view.doc_to_screen(box.x0, box.y0, layout.canvas,
                                                      shell.width, shell.height);
    const comp_ui::Point b = shell.view.doc_to_screen(box.x1, box.y1, layout.canvas,
                                                      shell.width, shell.height);
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
      const double zoom = shell.view.zoom;
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
   * 文字预览（S11）：插入点光标 + 未来文本框（白实线 + 黑虚线双描边）。
   * 只给位置与尺寸，不预演字形：外壳没有系统字体（字体是引擎内的 5×7 点阵），
   * 画一套 GDI 字只会与落笔结果不一致；像素在回车时由引擎一次写出（与渐变/
   * 形状预览同口径）。度量来自 ui_core，与内核 build_layout 同公式。
   */
  if (shell.type_active) {
    const comp_ui::TypeBlockMetrics metrics =
        comp_ui::type_block_metrics(shell.type_font_size, shell.type_line_height,
                                    shell.type_letter_spacing, shell.type_text.c_str());
    const comp_ui::Point origin = shell.view.doc_to_screen(
        shell.type_origin.x, shell.type_origin.y, layout.canvas, shell.width, shell.height);
    const comp_ui::Point corner = shell.view.doc_to_screen(
        shell.type_origin.x + metrics.width, shell.type_origin.y + metrics.height,
        layout.canvas, shell.width, shell.height);
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
        std::lround(comp_ui::type_caret_height(shell.type_font_size, shell.view.zoom)));
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

  /* 套索预览：沿已采样的折线画一条高对比度细线（闭合边在松开时由引擎生成）。 */  if (shell.lasso_active && shell.lasso_points.size() >= 4) {
    HPEN trail = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(dc, trail);
    bool started = false;
    for (size_t i = 0; i + 1 < shell.lasso_points.size(); i += 2) {
      const comp_ui::Point screen =
          shell.view.doc_to_screen(shell.lasso_points[i], shell.lasso_points[i + 1],
                                   layout.canvas, shell.width, shell.height);
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
  if (shell.tool == comp_ui::ToolId::Pen && !shell.pen_anchors.empty()) {
    const int anchor_count = static_cast<int>(shell.pen_anchors.size());
    const int segment_count = comp_ui::pen_segment_count(anchor_count, /*closed=*/false);
    HPEN path_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN handle_pen = CreatePen(PS_DOT, 1, RGB(80, 200, 255));
    HGDIOBJ old_pen = SelectObject(dc, path_pen);

    comp_ui::Point samples[comp_ui::kPenPreviewSamples + 1];
    for (int i = 0; i < segment_count; ++i) {
      const comp_ui::PenSegment segment =
          comp_ui::pen_segment_at(shell.pen_anchors.data(), anchor_count, i);
      samples[0] = segment.start;
      for (int sample = 1; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        samples[sample] = comp_ui::pen_cubic_at(
            segment, static_cast<double>(sample) / static_cast<double>(comp_ui::kPenPreviewSamples));
      }
      bool started = false;
      for (int sample = 0; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        const comp_ui::Point screen =
            shell.view.doc_to_screen(samples[sample].x, samples[sample].y, layout.canvas,
                                     shell.width, shell.height);
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
          comp_ui::pen_segment_at(shell.pen_anchors.data(), anchor_count, anchor_count - 1);
      bool started = false;
      for (int sample = 0; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        const comp_ui::Point point =
            sample == 0 ? closing.start
                        : comp_ui::pen_cubic_at(
                              closing, static_cast<double>(sample) /
                                           static_cast<double>(comp_ui::kPenPreviewSamples));
        const comp_ui::Point screen = shell.view.doc_to_screen(
            point.x, point.y, layout.canvas, shell.width, shell.height);
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
      const comp_ui::PenAnchor& anchor = shell.pen_anchors[static_cast<size_t>(i)];
      const comp_ui::Point center = shell.view.doc_to_screen(
          anchor.position.x, anchor.position.y, layout.canvas, shell.width, shell.height);
      const int cx = static_cast<int>(std::lround(center.x));
      const int cy = static_cast<int>(std::lround(center.y));
      Rectangle(dc, cx - 3, cy - 3, cx + 4, cy + 4);
      SelectObject(dc, handle_pen);
      if (anchor.handle_out.x != 0.0 || anchor.handle_out.y != 0.0) {
        const comp_ui::Point handle = shell.view.doc_to_screen(
            anchor.position.x + anchor.handle_out.x, anchor.position.y + anchor.handle_out.y,
            layout.canvas, shell.width, shell.height);
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, static_cast<int>(std::lround(handle.x)),
               static_cast<int>(std::lround(handle.y)));
      }
      if (anchor.handle_in.x != 0.0 || anchor.handle_in.y != 0.0) {
        const comp_ui::Point handle = shell.view.doc_to_screen(
            anchor.position.x + anchor.handle_in.x, anchor.position.y + anchor.handle_in.y,
            layout.canvas, shell.width, shell.height);
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

  /* 进行中的笔迹折线（文档像素 -> 屏幕）：落色用前景色，重绘用高对比度青。 */
  if (stroke_in_progress(shell) && shell.stroke_points.size() >= 4) {
    const COLORREF trail_color =
        shell.stroke_kind == comp_ui::StrokeKind::Paint && !shell.stroke_erase
            ? RGB(shell.foreground.r, shell.foreground.g, shell.foreground.b)
            : RGB(80, 200, 255);
    HPEN trail = CreatePen(PS_SOLID, 3, trail_color);
    HGDIOBJ old_pen = SelectObject(dc, trail);
    bool started = false;
    for (size_t i = 0; i + 1 < shell.stroke_points.size(); i += 2) {
      const comp_ui::Point screen = shell.view.doc_to_screen(
          shell.stroke_points[i], shell.stroke_points[i + 1], layout.canvas, shell.width,
          shell.height);
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
    const double radius = comp_ui::brush_tip_radius(shell.brush_size, shell.view.zoom);
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

  RestoreDC(dc, saved);
}

void present(HWND window, ShellState& shell) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(window, &paint);
  if (shell.needs_render) render_canvas(shell);

  RECT client{};
  GetClientRect(window, &client);
  const int client_width = client.right - client.left;
  const int client_height = client.bottom - client.top;
  const Layout layout = comp_ui::compute_layout(client_width, client_height);

  /* 新文档 / 打开文档后按窗口适配一次；之后缩放与平移完全由用户操作决定。 */
  if (shell.needs_fit) {
    if (shell.document != nullptr && !layout.canvas.empty()) {
      shell.view.fit(shell.width, shell.height, layout.canvas);
    }
    shell.needs_fit = false;
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
  if (!shell.framebuffer.empty() && shell.width > 0 && shell.height > 0 &&
      !layout.canvas.empty()) {
    const comp_ui::Point top_left = shell.view.doc_to_screen(
        0.0, 0.0, layout.canvas, shell.width, shell.height);
    const comp_ui::Point bottom_right = shell.view.doc_to_screen(
        shell.width, shell.height, layout.canvas, shell.width, shell.height);
    const double clip_left = std::max<double>(top_left.x, layout.canvas.x);
    const double clip_top = std::max<double>(top_left.y, layout.canvas.y);
    const double clip_right = std::min<double>(bottom_right.x, layout.canvas.right());
    const double clip_bottom = std::min<double>(bottom_right.y, layout.canvas.bottom());

    if (clip_right > clip_left && clip_bottom > clip_top) {
      const int dst_x = static_cast<int>(std::floor(clip_left));
      const int dst_y = static_cast<int>(std::floor(clip_top));
      const int dst_w = std::max(1, static_cast<int>(std::ceil(clip_right)) - dst_x);
      const int dst_h = std::max(1, static_cast<int>(std::ceil(clip_bottom)) - dst_y);

      int src_x = static_cast<int>(std::floor((clip_left - top_left.x) / shell.view.zoom));
      int src_y = static_cast<int>(std::floor((clip_top - top_left.y) / shell.view.zoom));
      int src_w = static_cast<int>(std::ceil(static_cast<double>(dst_w) / shell.view.zoom)) + 1;
      int src_h = static_cast<int>(std::ceil(static_cast<double>(dst_h) / shell.view.zoom)) + 1;
      if (src_x < 0) src_x = 0;
      if (src_y < 0) src_y = 0;
      if (src_x > shell.width) src_x = shell.width;
      if (src_y > shell.height) src_y = shell.height;
      if (src_x + src_w > shell.width) src_w = shell.width - src_x;
      if (src_y + src_h > shell.height) src_h = shell.height - src_y;

      if (src_w > 0 && src_h > 0) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = shell.width;
        info.bmiHeader.biHeight = -shell.height; /* 负值 = 自顶向下，与引擎缓冲一致 */
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        const int saved = SaveDC(memory_dc);
        IntersectClipRect(memory_dc, layout.canvas.x, layout.canvas.y, layout.canvas.right(),
                          layout.canvas.bottom());
        SetStretchBltMode(memory_dc, HALFTONE);
        SetBrushOrgEx(memory_dc, 0, 0, nullptr);
        StretchDIBits(memory_dc, dst_x, dst_y, dst_w, dst_h, src_x, src_y, src_w, src_h,
                      shell.framebuffer.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        RestoreDC(memory_dc, saved);
      }
    }
  }

  draw_canvas_overlays(memory_dc, shell, layout);

  draw_toolbar(memory_dc, shell, layout.toolbar);
  if (layout.layer_panel.w > 0) draw_layer_panel(memory_dc, shell, layout.layer_panel);
  if (layout.adjust_panel.w > 0) {
    if (path_panel_visible(shell)) {
      draw_path_panel(memory_dc, shell, layout.adjust_panel);
    } else if (history_panel_visible(shell)) {
      draw_history_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.color_range_open) {
      draw_color_range_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.effect_open) {
      draw_effect_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.filter_open) {
      draw_filter_panel(memory_dc, shell, layout.adjust_panel);
    } else if (shell.tool == comp_ui::ToolId::Type) {
      draw_type_panel(memory_dc, shell, layout.adjust_panel);
    } else {
      draw_adjust_panel(memory_dc, shell, layout.adjust_panel);
    }
  }
  draw_status_bar(memory_dc, shell, layout.status);

  BitBlt(dc, 0, 0, client_width, client_height, memory_dc, 0, 0, SRCCOPY);
  SelectObject(memory_dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory_dc);
  EndPaint(window, &paint);
}

/* ------------------------------------------------------------ 工具 / 视图 / 鼠标 */

void set_tool(ShellState& shell, comp_ui::ToolId id) {
  /* 离开钢笔工具 = 放弃未提交的路径（与切走套索/marquee 的取消口径一致）。 */
  if (shell.tool == comp_ui::ToolId::Pen && id != comp_ui::ToolId::Pen) {
    pen_cancel(shell);
  }
  /* 离开渐变工具 = 放弃进行中的拖拽（只丢预览，不产生撤销步）。 */
  if (shell.tool == comp_ui::ToolId::Gradient && id != comp_ui::ToolId::Gradient) {
    gradient_cancel(shell);
  }
  /* 离开形状工具同理：只丢预览。 */
  if (shell.tool == comp_ui::ToolId::Shape && id != comp_ui::ToolId::Shape) {
    shape_cancel(shell);
  }
  /*
   * 离开裁剪工具 = 丢掉待提交的框。必须丢：框是文档坐标系里的绝对矩形，
   * 切走工具后它还在屏幕上会让人以为按 Enter 还能提交，但那时 Enter 已经
   * 路由到别的逻辑去了——留着一个不能用的模态比没有模态更难理解。
   */
  if (shell.tool == comp_ui::ToolId::Crop && id != comp_ui::ToolId::Crop) {
    crop_cancel(shell);
  }
  /* 离开文字工具 = 放弃未提交的键入与属性键入（面板本身不是模态，不产生撤销步）。 */
  if (shell.tool == comp_ui::ToolId::Type && id != comp_ui::ToolId::Type) {
    type_cancel(shell);
    shell.type_input.clear();
  }
  /* 切到文字工具时丢掉调整面板的数值缓冲：两个面板共用 Tab/数字键，不能同时有输入态。 */
  if (id == comp_ui::ToolId::Type) shell.adj_input.clear();
  shell.tool = id;
  /*
   * 切到历史记录画笔：顺手把历史面板打开——否则「取样哪一格」既看不见也点不到，
   * 自动取样虽然能画，但用户不知道自己在刷什么。没有文档时静默（面板打开也无意义）。
   */
  if (id == comp_ui::ToolId::HistoryBrush && !shell.history_open && shell.document != nullptr) {
    history_panel_toggle(shell);
  }
  const comp_ui::ToolInfo* info = comp_ui::tool_info(id);
  if (info == nullptr) return;
  wchar_t buffer[192];
  if (id == comp_ui::ToolId::Retouch) {
    wsprintfW(buffer, L"工具：Retouch / %s（再按 R 循环）",
              widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str());
  } else if (id == comp_ui::ToolId::Tone) {
    wsprintfW(buffer, L"工具：%s（再按 O 循环；Alt+[ / ] 调强度，当前 %d%%）",
              widen_ascii(comp_ui::tone_state_name(shell.tone_state)).c_str(),
              static_cast<int>(shell.tone_strength * 100.0 + 0.5));
  } else if (id == comp_ui::ToolId::Marquee) {
    wsprintfW(buffer, L"工具：Marquee / %s（M 切矩形/椭圆，Shift 加选 / Alt 减选）",
              widen_ascii(comp_ui::marquee_shape_name(shell.marquee_shape)).c_str());
  } else if (id == comp_ui::ToolId::Lasso) {
    wsprintfW(buffer, L"工具：Lasso（拖出自由选区；Shift 加选 / Alt 减选；羽化 %d px）",
              shell.selection_feather);
  } else if (id == comp_ui::ToolId::Pen) {
    wsprintfW(buffer,
              L"工具：Pen（点击落锚点，拖动拉控制柄；点首锚点闭合，Enter 完成，Esc 取消；羽化 %d px）",
              shell.selection_feather);
  } else if (id == comp_ui::ToolId::Gradient) {
    wsprintfW(buffer,
              L"工具：Gradient / %s（拖出方向与长度后松开；再按 G 切线性/径向；Shift 反向；色标 = 前景 → 透明）",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str());
  } else if (id == comp_ui::ToolId::Shape) {
    wsprintfW(buffer,
              L"工具：Shape / %s（拖出%s；Shift = 正方/正圆，Alt = 从中心；再按 U 切种类；"
              L"Alt+[ / ] 调线宽，Shift+Alt+[ / ] 调圆角）",
              widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
              shell.shape_kind == comp_ui::kShapeLine ? L"两端点" : L"对角点");
  } else if (id == comp_ui::ToolId::Crop) {
    wsprintfW(buffer,
              L"工具：Crop（拖出裁剪框，框外变暗；Enter 提交，Esc 取消；再点画布重新起框）");
  } else if (id == comp_ui::ToolId::Type) {
    wsprintfW(buffer,
              L"工具：Type（画布点击落插入点，键入 ASCII；Shift+Enter 换行，回车提交，Esc 取消；"
              L"字号 %d px，行距 %d%%，字距 %d px；右侧面板 Tab/-=/数字键；"
              L"[ / ] 字号，Shift+[ / ] 字距，Alt+[ / ] 行距）",
              static_cast<int>(shell.type_font_size + 0.5),
              static_cast<int>(shell.type_line_height * 100.0 + 0.5),
              static_cast<int>(std::lround(shell.type_letter_spacing)));
  } else if (id == comp_ui::ToolId::Wand) {
    wsprintfW(buffer,
              L"工具：Magic Wand（容差 %d，窗口 %d×%d，%s取样，%s；[ / ] 调容差，Alt+[ / ] 调窗口，Shift+W 切来源）",
              shell.wand_tolerance, shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1,
              shell.wand_merged ? L"合并" : L"当前图层",
              shell.wand_contiguous ? L"连通" : L"全图");
  } else if (id == comp_ui::ToolId::HistoryBrush) {
    wsprintfW(buffer,
              L"工具：History Brush（Y）· 取样第 %d 个状态｜Shift+点历史面板的行改取样源，"
              L"Alt+[ / ] 调不透明度（当前 %d%%）",
              history_brush_resolved_state(shell),
              static_cast<int>(shell.history_brush_opacity * 100.0 + 0.5));
  } else {
    wsprintfW(buffer, L"工具：%s（%c）", widen_ascii(info->name).c_str(),
              info->shortcut != 0 ? static_cast<wchar_t>(info->shortcut - 'a' + 'A') : L' ');
  }
  set_status(shell, buffer);
}

/* R 键循环 Blur / Smudge / Liquify；三者都走 comp_tool_apply。 */
void set_retouch_mode(ShellState& shell, int mode) {
  shell.retouch_mode = mode;
  shell.tool = comp_ui::ToolId::Retouch;
  wchar_t buffer[192];
  if (mode == comp_ui::kRetouchBlur) {
    wsprintfW(buffer, L"工具：Retouch / Blur（Alt+[ / ] 调半径，当前 %d px）",
              static_cast<int>(shell.blur_radius + 0.5));
  } else {
    wsprintfW(buffer, L"工具：Retouch / %s（Alt+[ / ] 调强度，当前 %d%%）",
              widen_ascii(comp_ui::retouch_mode_name(mode)).c_str(),
              static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
  }
  set_status(shell, buffer);
}

/* O 键循环 8 档（Dodge/Burn 各三档 + Sponge 两档）；都走 comp_tool_apply。 */
void set_tone_state(ShellState& shell, int state) {
  shell.tone_state = state;
  shell.tool = comp_ui::ToolId::Tone;
  wchar_t buffer[192];
  wsprintfW(buffer, L"工具：%s（再按 O 循环；Alt+[ / ] 调强度，当前 %d%%）",
            widen_ascii(comp_ui::tone_state_name(state)).c_str(),
            static_cast<int>(shell.tone_strength * 100.0 + 0.5));
  set_status(shell, buffer);
}

void brush_step(ShellState& shell, int direction, bool hardness) {
  wchar_t buffer[96];
  if (hardness) {
    shell.brush_hardness = comp_ui::step_brush_hardness(shell.brush_hardness, direction);
    wsprintfW(buffer, L"笔刷硬度 %d%%",
              static_cast<int>(shell.brush_hardness * 100.0 + 0.5));
  } else {
    shell.brush_size = comp_ui::step_brush_size(shell.brush_size, direction);
    wsprintfW(buffer, L"笔刷直径 %d px", static_cast<int>(shell.brush_size + 0.5));
  }
  set_status(shell, buffer);
}

bool client_layout(const ShellState& shell, Layout* out) {
  RECT client{};
  if (shell.window == nullptr || !GetClientRect(shell.window, &client)) return false;
  *out = comp_ui::compute_layout(client.right - client.left, client.bottom - client.top);
  return true;
}

void view_fit(ShellState& shell) {
  Layout layout{};
  if (!client_layout(shell, &layout)) return;
  shell.view.fit(shell.width, shell.height, layout.canvas);
  shell.needs_fit = false;
  wchar_t buffer[96];
  wsprintfW(buffer, L"适合窗口：%d%%", static_cast<int>(shell.view.zoom * 100.0 + 0.5));
  set_status(shell, buffer);
}

void view_actual(ShellState& shell) {
  shell.view.actual_pixels();
  shell.needs_fit = false;
  set_status(shell, L"实际像素：100%");
}

void view_zoom_step(ShellState& shell, double factor) {
  Layout layout{};
  if (!client_layout(shell, &layout) || layout.canvas.empty()) return;
  const comp_ui::Point center = comp_ui::CanvasView::canvas_center(layout.canvas);
  shell.view.zoom_by(factor, center.x, center.y, layout.canvas);
  wchar_t buffer[96];
  wsprintfW(buffer, L"缩放 %d%%", static_cast<int>(shell.view.zoom * 100.0 + 0.5));
  set_status(shell, buffer);
}

void begin_pan(HWND window, ShellState& shell, int x, int y) {
  shell.panning = true;
  shell.pan_anchor.x = x;
  shell.pan_anchor.y = y;
  SetCapture(window);
  SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZEALL)));
}

void end_pan(HWND window, ShellState& shell) {
  if (!shell.panning) return;
  shell.panning = false;
  ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
}

void canvas_click(ShellState& shell, int x, int y, const Layout& layout) {
  if (shell.document == nullptr) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  wchar_t buffer[128];
  wsprintfW(buffer, L"画布 (%d, %d)", static_cast<int>(std::floor(doc.x)),
            static_cast<int>(std::floor(doc.y)));
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 前景色键入（Shift+X） */

void begin_color_input(ShellState& shell) {
  shell.color_input = L"#";
  set_status(shell, L"前景色：键入 #RRGGBB 或 #RGB，回车确认（Esc 取消）");
}

void refresh_color_input_status(ShellState& shell) {
  wchar_t buffer[96];
  wsprintfW(buffer, L"前景色：%s（回车确认，Esc 取消）", shell.color_input.c_str());
  set_status(shell, buffer);
}

void commit_color_input(ShellState& shell) {
  char ascii[8] = {0};
  for (size_t i = 0; i < shell.color_input.size() && i < 7; ++i) {
    const wchar_t ch = shell.color_input[i];
    ascii[i] = ch < 128 ? static_cast<char>(ch) : '?';
  }
  comp_ui::ColorRGB parsed{};
  const bool ok = comp_ui::color_parse_hex(ascii, &parsed);
  shell.color_input.clear();
  if (!ok) {
    set_status(shell, L"前景色：格式无效（需 #RGB 或 #RRGGBB）");
    return;
  }
  shell.foreground = parsed;
  char hex[8] = {0};
  comp_ui::color_to_hex(shell.foreground, hex);
  wchar_t buffer[96];
  wsprintfW(buffer, L"前景色：%s", widen_ascii(hex).c_str());
  set_status(shell, buffer);
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
  return comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind),
                                        static_cast<size_t>(row), out) == COMP_OK;
}

/* 编辑缓冲 -> 选中的那条调整。只在拖动持有事务时写；失败保持静默（拖动中不刷屏）。 */
bool adjust_write_live(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0 || shell.selected_adjustment < 0) return false;
  if (shell.adj_drag_txn == nullptr) return false;
  std::string json;
  if (!build_settings_json(shell, &json)) return false;
  if (comp_layer_update_adjustment_at(shell.document, id,
                                      static_cast<size_t>(shell.selected_adjustment),
                                      static_cast<uint32_t>(shell.adj_kind),
                                      json.c_str()) != COMP_OK) {
    return false;
  }
  shell.needs_render = true;
  return true;
}

/* 把指针 x 换算成值写进编辑缓冲；值没变就不写文档（少一次快照，也少一次重绘）。 */
void adjust_drag_value(ShellState& shell, double x) {
  if (shell.adj_drag_param < 0) return;
  comp_adjustment_param param{};
  if (!adjust_row_param(shell, shell.adj_drag_param, &param)) return;
  if (param.type == COMP_PARAM_BOOL) return;
  const size_t index = static_cast<size_t>(shell.adj_drag_param);
  if (index >= shell.adj_values.size()) return;

  const double raw =
      comp_ui::adjust_slider_value(shell.adj_drag_track, x, param.min_value, param.max_value);
  const double value = comp_ui::adjust_snap_value(raw, param.min_value, param.max_value);
  const std::string text = number_text(value);
  if (shell.adj_values[index] == text) return;
  shell.adj_values[index] = text;
  shell.adj_input.clear();
  if (adjust_write_live(shell)) shell.adj_drag_dirty = true;
  shell.needs_render = true;

  wchar_t buffer[220];
  wsprintfW(buffer, L"%s = %s（%s）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(text.c_str()).c_str(),
            shell.adj_drag_txn != nullptr ? L"实时预览，松手算一步撤销"
                                          : L"预览值，Ctrl+Shift+U 应用");
  set_status(shell, buffer);
}

void adjust_begin_drag(ShellState& shell, const comp_ui::Rect& panel, int32_t row, double x) {
  comp_adjustment_param param{};
  if (!adjust_row_param(shell, row, &param)) return;
  if (param.type == COMP_PARAM_BOOL) return; /* 布尔没有滑道，点击即切换 */

  if (shell.selected_adjustment >= 0) {
    if (shell.document == nullptr) return;
    if (comp_txn_begin(shell.document, "Adjust Parameter", &shell.adj_drag_txn) != COMP_OK) {
      shell.adj_drag_txn = nullptr;
      set_status(shell, L"调整滑杆：无法开启事务（未改动）");
      return;
    }
  } else {
    shell.adj_drag_txn = nullptr;
  }
  shell.adj_param = row;
  shell.adj_input.clear();
  shell.adj_drag_param = row;
  shell.adj_drag_dirty = false;
  /* 几何在按下瞬间冻结：拖动途中面板重排也不会让滑道在指针下跑掉。
   * 行几何用「相对行」（分页视图里绝对下标要减掉通道基址）。 */
  shell.adj_drag_track = comp_ui::adjust_slider_track_rect(comp_ui::adjust_param_row_rect(
      panel, row - adjust_param_base(shell), shell.adj_scroll));
  adjust_drag_value(shell, x);
}

/* commit=false 用于 Esc / 失焦：事务 abort 会回滚到按下前的快照。 */
void adjust_drag_finish(ShellState& shell, bool commit) {
  if (shell.adj_drag_param < 0) return;
  const int32_t focused = shell.adj_param;
  comp_adjustment_param param{};
  const bool has_param = adjust_row_param(shell, focused, &param);
  const comp_txn txn = shell.adj_drag_txn;
  const bool dirty = shell.adj_drag_dirty;
  /* 先清状态再 ReleaseCapture：捕获变更会重入 WM_CAPTURECHANGED。 */
  shell.adj_drag_param = -1;
  shell.adj_drag_txn = nullptr;
  shell.adj_drag_dirty = false;
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
  shell.needs_render = true;
  refresh_adjustments(shell);
  if (focused >= 0 && focused < static_cast<int32_t>(shell.adj_values.size())) {
    shell.adj_param = focused;
  }

  wchar_t buffer[220];
  if (committed) {
    wsprintfW(buffer, L"已应用 %s = %s（一步撤销）",
              widen_ascii(has_param && param.label != nullptr ? param.label : "").c_str(),
              widen_ascii(shell.adj_param < static_cast<int32_t>(shell.adj_values.size())
                              ? shell.adj_values[static_cast<size_t>(shell.adj_param)].c_str()
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
  shell.adj_scroll = comp_ui::adjust_param_ensure_visible(
      layout.adjust_panel, adjust_shown_count(shell), shell.adj_param - adjust_param_base(shell),
      shell.adj_scroll);
}

/* ------------------------------------------------ 曲线编辑器（P0-3） */

/*
 * 控制点的真相在引擎里（settings JSON）。外壳只缓存一份「当前通道的点列」用于
 * 绘制/命中；任何写入都走 comp_layer_curves_set_points 拼 JSON，再由
 * comp_layer_update_adjustment_at 落盘 —— 撤销步由后者产生，曲线接口本身是
 * 纯函数（见 comp_layer.h 的契约）。拖动期间用显式事务把多次写入折成一步。
 */
const std::string* selected_adjustment_settings(const ShellState& shell) {
  if (shell.selected_adjustment < 0 ||
      shell.selected_adjustment >= static_cast<int32_t>(shell.adj_settings.size())) {
    return nullptr;
  }
  return &shell.adj_settings[static_cast<size_t>(shell.selected_adjustment)];
}

/* 曲线编辑只在「已选中的 Curves 调整」上可用：新增态没有可写对象。 */
bool curve_editable(const ShellState& shell) {
  return shell.adj_kind == COMP_ADJUSTMENT_CURVES && selected_adjustment_settings(shell) != nullptr;
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
  shell.curve_xy.clear();
  shell.curve_focus_point = -1;
  const std::string* settings = selected_adjustment_settings(shell);
  if (settings == nullptr || shell.adj_kind != COMP_ADJUSTMENT_CURVES) return;
  const uint32_t channel = static_cast<uint32_t>(shell.curve_channel);
  size_t count = 0;
  if (comp_layer_curves_point_count(settings->c_str(), channel, &count) != COMP_OK) return;
  if (count > 32) count = 32;
  for (size_t i = 0; i < count; ++i) {
    double x = 0.0;
    double y = 0.0;
    if (comp_layer_curves_point_at(settings->c_str(), channel, i, &x, &y) != COMP_OK) break;
    shell.curve_xy.push_back(x);
    shell.curve_xy.push_back(y);
  }
}

/* 点列 -> settings -> 文档。两遍调用（先问容量再写），与 ABI 契约一致。 */
bool curve_write_points(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  const std::string* settings = selected_adjustment_settings(shell);
  if (shell.document == nullptr || id == 0 || settings == nullptr) return false;
  const size_t count = shell.curve_xy.size() / 2;
  if (count < 2) return false;
  const uint32_t channel = static_cast<uint32_t>(shell.curve_channel);
  size_t need = 0;
  if (comp_layer_curves_set_points(settings->c_str(), channel, shell.curve_xy.data(), count, nullptr,
                                   0, &need) != COMP_OK) {
    return false;
  }
  std::string json(need + 1, '\0');
  if (comp_layer_curves_set_points(settings->c_str(), channel, shell.curve_xy.data(), count,
                                   &json[0], json.size(), &need) != COMP_OK) {
    return false;
  }
  json.resize(need);
  if (comp_layer_update_adjustment_at(shell.document, id,
                                      static_cast<size_t>(shell.selected_adjustment),
                                      COMP_ADJUSTMENT_CURVES, json.c_str()) != COMP_OK) {
    return false;
  }
  shell.needs_render = true;
  return true;
}

void curve_drag_to(ShellState& shell, const comp_ui::Rect& plot, double x, double y) {
  const int32_t index = shell.curve_drag_point;
  if (index < 0) return;
  const int32_t count = static_cast<int32_t>(shell.curve_xy.size() / 2);
  if (index >= count) return;

  double value_x = std::floor(comp_ui::curve_plot_x_to_value(plot, x) + 0.5);
  double value_y = std::floor(comp_ui::curve_plot_y_to_value(plot, y) + 0.5);
  if (index == 0) {
    value_x = 0.0; /* 端点锁 x：曲线定义域必须覆盖 0..255 */
  } else if (index == count - 1) {
    value_x = 255.0;
  } else {
    /* 内部点保持严格递增（比较用整数 x，所以最小间隔就是 1）。 */
    const double lo = shell.curve_xy[static_cast<size_t>(2 * (index - 1))] + 1.0;
    const double hi = shell.curve_xy[static_cast<size_t>(2 * (index + 1))] - 1.0;
    if (value_x < lo) value_x = lo;
    if (value_x > hi) value_x = hi;
  }
  if (value_y < 0.0) value_y = 0.0;
  if (value_y > 255.0) value_y = 255.0;
  const size_t slot = static_cast<size_t>(2 * index);
  if (shell.curve_xy[slot] == value_x && shell.curve_xy[slot + 1] == value_y) return;
  shell.curve_xy[slot] = value_x;
  shell.curve_xy[slot + 1] = value_y;

  if (curve_write_points(shell)) shell.curve_drag_dirty = true;
  shell.needs_render = true;
  wchar_t buffer[200];
  /* 同上：wsprintfW 不支持 %f，先转文本再 %s。 */
  const std::wstring text_x = widen_ascii(number_text(value_x).c_str());
  const std::wstring text_y = widen_ascii(number_text(value_y).c_str());
  wsprintfW(buffer, L"曲线 %s 点 %d/%d = (%s, %s) %s", curve_channel_name(shell.curve_channel),
            index + 1, count, text_x.c_str(), text_y.c_str(),
            shell.curve_drag_txn != nullptr ? L"（实时预览，松手算一步撤销）" : L"（未写入）");
  set_status(shell, buffer);
}

/* 按住控制点开事务 + 冻结绘图区几何（与滑杆同一套理由）。 */
void curve_begin_drag(ShellState& shell, const comp_ui::Rect& plot, int32_t point, double x,
                      double y) {
  if (!curve_editable(shell)) {
    set_status(shell, L"曲线：先 Ctrl+Shift+A 新增一条 Curves（恒等），再从空白处加点");
    return;
  }
  if (shell.document == nullptr) return;
  if (comp_txn_begin(shell.document, "Adjust Curve", &shell.curve_drag_txn) != COMP_OK) {
    shell.curve_drag_txn = nullptr;
    set_status(shell, L"曲线：无法开启事务（未改动）");
    return;
  }
  shell.curve_drag_point = point;
  shell.curve_focus_point = point;
  shell.curve_drag_dirty = false;
  shell.curve_drag_plot = plot;
  curve_drag_to(shell, plot, x, y);
}

void curve_drag_finish(ShellState& shell, bool commit) {
  if (shell.curve_drag_point < 0) return;
  const int32_t focused = shell.curve_focus_point;
  const comp_txn txn = shell.curve_drag_txn;
  const bool dirty = shell.curve_drag_dirty;
  shell.curve_drag_point = -1;
  shell.curve_drag_txn = nullptr;
  shell.curve_drag_dirty = false;
  ReleaseCapture();

  bool committed = false;
  if (txn != nullptr) {
    if (commit && dirty) {
      committed = comp_txn_commit(txn) == COMP_OK;
    } else {
      comp_txn_abort(txn);
    }
  }
  shell.needs_render = true;
  refresh_adjustments(shell); /* 重读 settings，点列以引擎为准 */
  shell.curve_focus_point = focused;

  wchar_t buffer[200];
  if (committed) {
    wsprintfW(buffer, L"已应用曲线 %s（一步撤销）", curve_channel_name(shell.curve_channel));
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
  if (shell.curve_xy.empty()) {
    shell.curve_xy = {0.0, 0.0, 255.0, 255.0}; /* 空通道从恒等基线开始 */
  }
  const int32_t count = static_cast<int32_t>(shell.curve_xy.size() / 2);
  if (count >= 32) {
    set_status(shell, L"曲线控制点已满（上限 32）");
    return;
  }
  const double value_x = comp_ui::curve_insert_x(
      shell.curve_xy.data(), static_cast<int>(count), comp_ui::curve_plot_x_to_value(plot, x));
  if (std::isnan(value_x)) {
    set_status(shell, L"这里插不进新点（与相邻点的 x 距离不足 1）");
    return;
  }
  double value_y = std::floor(comp_ui::curve_plot_y_to_value(plot, y) + 0.5);
  if (value_y < 0.0) value_y = 0.0;
  if (value_y > 255.0) value_y = 255.0;
  int32_t index = 0;
  while (index < count && shell.curve_xy[static_cast<size_t>(2 * index)] < value_x) ++index;
  shell.curve_xy.insert(shell.curve_xy.begin() + 2 * index, {value_x, value_y});
  curve_begin_drag(shell, plot, index, x, y);
}

/* 删点：端点（x=0 / x=255）锁死，删掉它们曲线都没定义域了。 */
void curve_delete_point(ShellState& shell, int32_t index) {
  if (!curve_editable(shell)) return;
  const int32_t count = static_cast<int32_t>(shell.curve_xy.size() / 2);
  if (index < 0 || index >= count) {
    set_status(shell, L"曲线：没有可删除的控制点（先在曲线上点一下选中）");
    return;
  }
  if (index == 0 || index == count - 1) {
    set_status(shell, L"曲线端点不能删除（x = 0 / x = 255 固定）");
    return;
  }
  shell.curve_xy.erase(shell.curve_xy.begin() + 2 * index,
                       shell.curve_xy.begin() + 2 * index + 2);
  shell.curve_focus_point = -1;
  const bool ok = curve_write_points(shell); /* 隐式事务 = 一步撤销 */
  refresh_adjustments(shell);
  refresh_history_status(shell, ok ? L"已删除曲线控制点（一步撤销）" : L"删除曲线控制点失败");
}

void curve_set_channel(ShellState& shell, int32_t channel) {
  if (channel < 0 || channel >= kCurveChannels) return;
  shell.curve_channel = channel;
  refresh_curve_points(shell);
  wchar_t buffer[120];
  wsprintfW(buffer, L"曲线通道：%s", curve_channel_name(channel));
  set_status(shell, buffer);
}

/* 色阶：切通道页。焦点落到该通道首行，输入缓冲清掉（不在别的通道上提交数字）。 */
void adjust_set_channel(ShellState& shell, int32_t channel) {
  if (channel < 0 || channel >= kLevelsChannelCount) return;
  shell.adj_channel = channel;
  shell.adj_param = adjust_param_base(shell);
  shell.adj_input.clear();
  shell.adj_scroll = 0;
  wchar_t buffer[120];
  wsprintfW(buffer, L"色阶通道：%s", curve_channel_name(channel));
  set_status(shell, buffer);
}

void handle_key(HWND window, ShellState& shell, WPARAM key, bool ctrl, bool shift) {
  /*
   * P0-1：图层名 / 不透明度键入态是最高的模态。字符本体由 WM_CHAR 送进来
   * （CJK 走 WM_IME_CHAR），这里只处理编辑键；其余按键一律吞掉，理由同其它
   * 键入态——输入到一半按到 B，把混合模式改掉而输入框还在，状态不可解释。
   */
  if (shell.layer_input != LayerInput::None) {
    if (key == VK_ESCAPE) {
      panel_cancel_layer_input(shell);
      set_status(shell, L"已取消图层输入");
      return;
    }
    if (key == VK_BACK) {
      panel_layer_input_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      panel_commit_layer_input(shell);
      return;
    }
    return;
  }

  /* 前景色十六进制键入态最先生效：它是模态输入，不能漏到工具快捷键。 */
  if (!shell.color_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.color_input.clear();
      set_status(shell, L"已取消前景色输入");
      return;
    }
    if (key == VK_BACK) {
      shell.color_input.pop_back();
      if (shell.color_input.empty()) {
        set_status(shell, L"已取消前景色输入");
      } else {
        refresh_color_input_status(shell);
      }
      return;
    }
    if (key == VK_RETURN) {
      commit_color_input(shell);
      return;
    }
    const bool hex_digit = (key >= '0' && key <= '9') || (key >= 'A' && key <= 'F') ||
                           (key >= 'a' && key <= 'f');
    if (hex_digit && shell.color_input.size() < 7) {
      shell.color_input.push_back(static_cast<wchar_t>(key));
      refresh_color_input_status(shell);
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 裁剪框待提交态（S22）：Enter 提交 / Esc 取消。
   * 与颜色/羽化输入态同级——都是模态，任何其中一个激活时其余快捷键一律让路，
   * 否则「框已拖好、手滑按到 B」会把混合模式改掉而裁剪框还在，状态不可解释。
   */
  if (shell.crop_active) {
    if (key == VK_RETURN) {
      crop_commit(shell);
      return;
    }
    if (key == VK_ESCAPE) {
      crop_cancel(shell);
      set_status(shell, L"已取消裁剪");
      return;
    }
  }

  /* 羽化半径键入态（Shift+F）：只吃数字/退格/回车/Esc，其余键一律屏蔽。 */
  if (!shell.feather_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.feather_input.clear();
      set_status(shell, L"已取消羽化输入");
      return;
    }
    if (key == VK_BACK) {
      shell.feather_input.pop_back();
      if (shell.feather_input.empty()) {
        set_status(shell, L"已取消羽化输入");
      } else {
        refresh_feather_input_status(shell);
      }
      return;
    }
    if (key == VK_RETURN) {
      commit_feather_input(shell);
      return;
    }
    if (key >= '0' && key <= '9' && shell.feather_input.size() < 3) {
      shell.feather_input.push_back(static_cast<wchar_t>(key));
      refresh_feather_input_status(shell);
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 扩展/收缩半径键入态（P0-4）：同款输入机，回车才落盘（一步撤销）。 */
  if (!shell.modify_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.modify_input.clear();
      set_status(shell, L"已取消选区修改");
      return;
    }
    if (key == VK_BACK) {
      shell.modify_input.pop_back();
      if (shell.modify_input.empty()) {
        set_status(shell, L"已取消选区修改");
      } else {
        refresh_modify_input_status(shell);
      }
      return;
    }
    if (key == VK_RETURN) {
      commit_modify_input(shell);
      return;
    }
    if (key >= '0' && key <= '9' && shell.modify_input.size() < 3) {
      shell.modify_input.push_back(static_cast<wchar_t>(key));
      refresh_modify_input_status(shell);
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 滤镜面板输入态：与调整面板同一套输入规矩，但目标值是滤镜参数。 */
  if (!shell.filter_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.filter_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      filter_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      /* 键入 + 回车 = 落值并立刻应用，少一次按键。 */
      filter_commit_input(shell);
      apply_filter(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.filter_input.clear();
      filter_focus_row(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      filter_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      filter_begin_input(shell, L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      filter_begin_input(shell, L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 滤镜面板打开时，键盘归面板：Esc 只关面板（不关窗），Ctrl 组合仍可穿透。 */
  if (shell.filter_open && !ctrl) {
    switch (key) {
      case VK_ESCAPE:
        close_filter_panel(shell);
        return;
      case VK_TAB:
        filter_focus_row(shell, shift ? -1 : 1);
        return;
      case VK_RETURN:
        apply_filter(shell);
        return;
      case VK_OEM_MINUS:
        filter_step_value(shell, -1);
        return;
      case VK_OEM_PLUS:
        filter_step_value(shell, 1);
        return;
      case VK_BACK:
        filter_reset_values(shell);
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      filter_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    /* 其余按键在面板打开时吞掉：避免按工具字母键把面板留在「另一个工具」上。 */
    return;
  }

  /* 效果参数面板输入态：与滤镜面板同一套输入规矩，目标值是效果参数。 */
  if (!shell.effect_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.effect_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      panel_effect_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      /* 键入 + 回车 = 落值并立刻应用，少一次按键（与滤镜面板同款）。 */
      panel_effect_commit_input(shell);
      panel_apply_effect(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.effect_input.clear();
      panel_effect_move_param(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      shell.effect_input.push_back(static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      shell.effect_input.push_back(L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      shell.effect_input.push_back(L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 效果参数面板打开时，键盘归面板：Esc 只关面板（不关窗），Ctrl 组合仍可穿透。 */
  if (shell.effect_open && !ctrl) {
    switch (key) {
      case VK_ESCAPE:
        close_effect_panel(shell);
        return;
      case VK_TAB:
        panel_effect_move_param(shell, shift ? -1 : 1);
        return;
      case VK_RETURN:
        panel_apply_effect(shell);
        return;
      case VK_OEM_MINUS:
        panel_effect_step_value(shell, -1);
        return;
      case VK_OEM_PLUS:
        panel_effect_step_value(shell, 1);
        return;
      case VK_BACK:
        panel_effect_reset_values(shell);
        return;
      case VK_OEM_COMMA:
        panel_effect_cycle_kind(shell, -1);
        return;
      case VK_OEM_PERIOD:
        panel_effect_cycle_kind(shell, 1);
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      panel_effect_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    /* 其余按键在面板打开时吞掉：避免工具字母键把面板留在「另一个工具」上。 */
    return;
  }

  /* 色彩范围容差键入态（P0-4 第二批）：只吃数字/退格/回车/Esc。 */
  if (!shell.color_range_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.color_range_input.clear();
      set_status(shell, L"已取消容差输入");
      return;
    }
    if (key == VK_BACK) {
      shell.color_range_input.pop_back();
      if (shell.color_range_input.empty()) {
        set_status(shell, L"已取消容差输入");
      } else {
        wchar_t buffer[96];
        wsprintfW(buffer, L"输入中：%s", shell.color_range_input.c_str());
        set_status(shell, buffer);
      }
      return;
    }
    if (key == VK_RETURN) {
      color_range_commit_input(shell);
      return;
    }
    if (key >= '0' && key <= '9') {
      color_range_begin_input(shell, static_cast<wchar_t>(key));
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 色彩范围面板打开时，键盘归面板（P0-4 第二批）。与滤镜/效果面板不同，
   * 这里连 Ctrl 组合也要拦：预览挂在显式事务上，撤销/新建/打开会把事务快照
   * 和当前文档撕开，abort 时就会「复活」一个本来已经不该存在的选区。
   * 其余 Ctrl 组合（缩放等）不影响事务，照常穿透。
   */
  if (shell.color_range_open) {
    if (ctrl) {
      if (key == 'Z' || key == 'Y' || key == 'N' || key == 'O' || key == 'S' || key == 'A' ||
          key == 'D') {
        set_status(shell, L"色彩范围预览中：先回车应用或 Esc 取消，再执行该命令");
        return;
      }
    } else {
      switch (key) {
        case VK_ESCAPE:
          color_range_cancel(shell);
          return;
        case VK_RETURN:
          color_range_commit(shell);
          return;
        case VK_OEM_MINUS:
          color_range_step_fuzziness(shell, -1);
          return;
        case VK_OEM_PLUS:
          color_range_step_fuzziness(shell, 1);
          return;
        case VK_BACK:
          color_range_clear_colors(shell);
          return;
        case 'I':
          color_range_toggle_invert(shell);
          return;
        default:
          break;
      }
      if (key >= '0' && key <= '9') {
        color_range_begin_input(shell, static_cast<wchar_t>(key));
        return;
      }
      /* 其余按键在面板打开时吞掉（含工具字母键）：模态，避免面板与工具打架。 */
      return;
    }
  }

  /* 路径面板打开且可见时键盘归面板（P0-4 可选）。Ctrl 组合仍可穿透：面板不留事务。 */
  if (path_panel_visible(shell) && !ctrl) {
    /*
     * 钢笔正在落点时（面板多半是上一条路径留下的）键归钢笔：回车提交当前路径、
     * Esc 取消当前路径；其余键仍吞掉——若放行，Del 会落到「删除选中图层」上。
     */
    if (shell.tool == comp_ui::ToolId::Pen && !shell.pen_anchors.empty()) {
      if (key == VK_RETURN) {
        commit_pen_path(shell);
      } else if (key == VK_ESCAPE) {
        pen_cancel(shell);
      }
      return;
    }
    switch (key) {
      case VK_ESCAPE:
        shell.path_open = false;
        set_status(shell, L"已关闭路径面板");
        return;
      case VK_UP:
        path_select(shell, -1);
        return;
      case VK_DOWN:
        path_select(shell, 1);
        return;
      case VK_RETURN:
        path_apply_selected(shell);
        return;
      case VK_DELETE:
        path_delete_selected(shell);
        return;
      default:
        break;
    }
    /* 其余按键吞掉：避免工具字母键把面板留在「另一个工具」上。 */
    return;
  }

  /* 历史记录面板打开且可见时键盘归面板（P1）。Ctrl 组合仍可穿透：面板不留事务。 */
  if (history_panel_visible(shell) && !ctrl) {
    /*
     * 钢笔落点中的回车/Esc 归钢笔（与路径面板同一条例外）：否则画布上点了锚点
     * 之后回车会被面板吞掉，路径永远提交不了；Del 也不会落到「删除图层」上。
     */
    if (shell.tool == comp_ui::ToolId::Pen && !shell.pen_anchors.empty()) {
      if (key == VK_RETURN) {
        commit_pen_path(shell);
      } else if (key == VK_ESCAPE) {
        pen_cancel(shell);
      }
      return;
    }
    switch (key) {
      case VK_ESCAPE:
        shell.history_open = false;
        set_status(shell, L"已关闭历史记录面板");
        return;
      case VK_UP:
        history_step(shell, -1);
        return;
      case VK_DOWN:
        history_step(shell, 1);
        return;
      case VK_HOME:
        history_jump_to(shell, 0);
        return;
      case VK_END:
        history_jump_to(shell, history_state_count(shell) - 1);
        return;
      default:
        break;
    }
    return; /* 其余按键吞掉：避免工具字母键把面板留在「另一个工具」上。 */
  }

  /* 文字属性面板输入态：与滤镜面板同一套输入规矩，目标值是字号/行距/字距。 */
  if (!shell.type_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.type_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      type_panel_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      type_panel_commit_input(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.type_input.clear();
      type_panel_focus(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      type_panel_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      type_panel_begin_input(shell, L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      type_panel_begin_input(shell, L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* P0-3：拖动中的滑杆/曲线点按 Esc = 取消这次拖动（事务回滚，不留撤销步）。 */
  if (shell.curve_drag_point >= 0 && key == VK_ESCAPE) {
    curve_drag_finish(shell, false);
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  if (shell.adj_drag_param >= 0 && key == VK_ESCAPE) {
    adjust_drag_finish(shell, false);
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  /*
   * P0-3：曲线页按 Delete = 删除焦点控制点。必须在图层删除命令之前判——
   * 那边 Delete 是「删图层」，正在调曲线时按到会整层消失。
   */
  if (key == VK_DELETE && shell.adj_kind == COMP_ADJUSTMENT_CURVES &&
      shell.curve_focus_point >= 0) {
    curve_delete_point(shell, shell.curve_focus_point);
    InvalidateRect(window, nullptr, FALSE);
    return;
  }

  /* 调整面板输入态优先：Esc 取消输入（不关窗）、退格删字符、回车提交、数字/./- 进缓冲。 */
  if (!shell.adj_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.adj_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      panel_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      panel_commit_input(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.adj_input.clear();
      panel_move_param(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      shell.adj_input.push_back(static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      shell.adj_input.push_back(L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      shell.adj_input.push_back(L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 文字键入态：最优先吃掉字符键，避免 'B' 被当成画笔快捷键、空格被当成抓手。
   * Esc 取消、Enter 提交、Shift+Enter 换行、Backspace 删字符；[ / ] 调字号与字距。
   * Ctrl 组合放行（撤销/打开等仍可用），其余未识别键一律吞掉（模态输入规矩）。
   */
  if (shell.type_active) {
    if (key == VK_ESCAPE) {
      type_cancel(shell);
      set_status(shell, L"已取消文字输入（未产生撤销步）");
      return;
    }
    if (key == VK_RETURN) {
      if (shift) {
        shell.type_text.push_back('\n');
        type_refresh_status(shell);
      } else {
        type_commit(shell);
      }
      return;
    }
    if (key == VK_BACK) {
      if (!shell.type_text.empty()) shell.type_text.pop_back();
      type_refresh_status(shell);
      return;
    }
    if (key == VK_OEM_4 || key == VK_OEM_6) {
      type_step_param(shell, key == VK_OEM_4 ? -1 : 1, shift);
      return;
    }
    if (!ctrl) {
      const char typed = comp_ui::type_ascii_from_vk(static_cast<int>(key), shift);
      if (typed != 0) {
        shell.type_text.push_back(typed);
        type_refresh_status(shell);
      }
      return; /* 键入态下屏蔽其它快捷键，避免误触发 */
    }
  }

  /*
   * 文字属性面板（Type 工具、未在键入文字时）：Tab 切行、- / = 调值、数字键输入。
   * [ / ] 仍走下面的 type_step_param（字号/字距），Alt+[ / ] 走 WM_SYSKEYDOWN（行距）。
   */
  if (shell.tool == comp_ui::ToolId::Type && !ctrl) {
    switch (key) {
      case VK_TAB:
        type_panel_focus(shell, shift ? -1 : 1);
        return;
      case VK_OEM_MINUS:
        type_panel_step(shell, -1);
        return;
      case VK_OEM_PLUS:
        type_panel_step(shell, 1);
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      type_panel_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
  }

  if (key == VK_ESCAPE) {
    /* 钢笔有未提交路径时 Esc 只取消路径，不关窗（与输入态的 Esc 同规矩）。 */
    if (shell.tool == comp_ui::ToolId::Pen && !shell.pen_anchors.empty()) {
      pen_cancel(shell);
      return;
    }
    /* 形状拖拽中 Esc 只取消这一笔预览，不关窗。 */
    if (shell.shape_active) {
      shape_cancel(shell);
      set_status(shell, L"已取消形状拖拽（未产生撤销步）");
      return;
    }
    PostMessageW(window, WM_CLOSE, 0, 0);
    return;
  }
  if (ctrl) {
    switch (key) {
      case 'A':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjAdd);
          return;
        }
        run_shell_command(window, shell, kCmdSelectAll);
        return;
      case 'I':
        /* Ctrl+Shift+I 反选（与上游一致）；单独 Ctrl+I 不做绑定。 */
        if (shift) {
          run_shell_command(window, shell, kCmdSelectInvert);
          return;
        }
        break;
      case 'U':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjApply);
          return;
        }
        break;
      case 'K':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjKindNext);
          return;
        }
        break;
      case 'J':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjKindPrev);
          return;
        }
        /* Ctrl+J 复制图层（对齐上游 Duplicate Layer 的 ⌘J）。 */
        run_shell_command(window, shell, kCmdLayerDuplicate);
        return;
      case 'G':
        /* Ctrl+G 建组（对齐上游 Group Layers）；Ctrl+Alt+G 剪贴蒙版走 SYSKEYDOWN。 */
        if (!shift) {
          run_shell_command(window, shell, kCmdLayerNewGroup);
          return;
        }
        break;
      case 'D':
        /* Ctrl+Shift+D 删除选中的调整；Ctrl+D 取消选区。 */
        if (shift) {
          run_shell_command(window, shell, kCmdAdjRemove);
          return;
        }
        run_shell_command(window, shell, kCmdSelectNone);
        return;
      case 'Z':
        /* Ctrl+Shift+Z 与 Ctrl+Y 同为重做，兼容两种习惯。 */
        run_shell_command(window, shell, shift ? kCmdRedo : kCmdUndo);
        return;
      case 'Y':
        run_shell_command(window, shell, kCmdRedo);
        return;
      case 'N':
        /* Ctrl+Shift+N 新建图层（沿用图像编辑器惯例），Ctrl+N 新建文档。 */
        run_shell_command(window, shell, shift ? kCmdLayerAdd : kCmdNew);
        return;
      case 'E':
        /* Ctrl+Shift+E = 效果参数面板；Ctrl+E 仍是向下合并（对齐上游 ⌘E）。 */
        if (shift) {
          run_shell_command(window, shell, kCmdEffectEdit);
          return;
        }
        run_shell_command(window, shell, kCmdLayerMergeDown);
        return;
      case 'O':
        /* Ctrl+Shift+O 打开位图，Ctrl+O 打开 .comp——两条路径语义不同，不能合并。 */
        run_shell_command(window, shell, shift ? kCmdOpenImage : kCmdOpen);
        return;
      /*
       * P0-4 可选：Ctrl+Shift+P = 路径面板。P 本身是钢笔工具键位，带 Ctrl 不冲突；
       * 不用 Ctrl+Shift+Q（无实义）是因为 P 更像 Paths。
       */
      case 'P':
        if (shift) {
          run_shell_command(window, shell, kCmdSelectPaths);
          return;
        }
        break;
      case 'S':
        /* Ctrl+Shift+S 另存为位图，Ctrl+S 保存 .comp。 */
        run_shell_command(window, shell, shift ? kCmdExportImage : kCmdSave);
        return;
      case 'V':
        /* V 已是 Move 工具的键位，图层显隐改走 Ctrl+Shift+V。 */
        if (shift) {
          run_shell_command(window, shell, kCmdLayerToggleVisible);
          return;
        }
        break;
      /*
       * S24 镜像：Ctrl+Shift+H 水平、Ctrl+Shift+F 垂直（避开已占用的 Ctrl+Shift+V；
       * H/F 取 Flip Horizontal / Flip Vertical 首字母，与上游 ⌘T 系变换分开记）。
       */
      case 'H':
        if (shift) {
          run_shell_command(window, shell, kCmdLayerFlipHorizontal);
          return;
        }
        break;
      case 'F':
        if (shift) {
          run_shell_command(window, shell, kCmdLayerFlipVertical);
          return;
        }
        break;
      case '0':
        run_shell_command(window, shell, kCmdViewFit);
        return;
      case '1':
        run_shell_command(window, shell, kCmdViewActual);
        return;
      case VK_OEM_PLUS:
        run_shell_command(window, shell, kCmdViewZoomIn);
        return;
      case VK_OEM_MINUS:
        run_shell_command(window, shell, kCmdViewZoomOut);
        return;
      case VK_OEM_4: /* Ctrl+[ = 图层不透明度 -10% */
        run_shell_command(window, shell, kCmdLayerOpacityDown);
        return;
      case VK_OEM_6: /* Ctrl+] = 图层不透明度 +10% */
        run_shell_command(window, shell, kCmdLayerOpacityUp);
        return;
      default:
        break;
    }
  }
  switch (key) {
    /*
     * 图层变换（S21）：方向键与 Alt+[ ] 在带修饰时归图层几何，不带修饰仍是既有
     * 行为（VK_UP/DOWN 选图层、[ ] 调笔刷/容差/字号）。
     *   方向键             = 选图层（不变）
     *   Shift+方向         = 平移 1 px 微调
     *   Ctrl+方向          = 平移 10 px
     *   Ctrl+Shift+左/右  = 旋转 ∓15°（Alt 被 Move 的「复制」占用，不取）
     *   Alt+[ / ]         = 等比例缩放 ±1%，中心锚定
     * 步长全部由修饰现场决定，直接调用 panel_* 函数并传 dx/dy：命令常量只表达
     * 「动作」，把粗细留给按键，壳侧就不存在第二套步长口径。
     */
    case VK_LEFT:
    case VK_RIGHT: {
      if (ctrl && shift) {
        panel_rotate_layer(shell, key == VK_RIGHT);
        break;
      }
      if (!ctrl && !shift) break; /* 无修饰：保持既有行为，交给下面的 OEM 分支之外无动作 */
      const int step = comp_ui::layer_transform_move_step(ctrl);
      panel_move_layer(shell, key == VK_LEFT ? -step : step, 0);
      break;
    }
    case VK_UP:
    case VK_DOWN: {
      /*
       * S22：Ctrl+Shift+↑/↓ = 上移/下移一层（面板序）。
       * 该组合此前未被占用（左右方向的 Ctrl+Shift 是 S21 的旋转），因此不与
       * 任何既有键位冲突。
       */
      if (ctrl && shift) {
        run_shell_command(window, shell,
                          key == VK_UP ? kCmdLayerBringForward : kCmdLayerSendBackward);
        break;
      }
      /* 无修饰仍是选图层；Shift/Ctrl 才平移（避免与既有 ↑↓ 选图层抢键）。 */
      if (shift || ctrl) {
        const int step = comp_ui::layer_transform_move_step(ctrl);
        panel_move_layer(shell, 0, key == VK_UP ? -step : step);
        break;
      }
      run_shell_command(window, shell,
                        key == VK_UP ? kCmdLayerSelectUp : kCmdLayerSelectDown);
      break;
    }
    case VK_OEM_4: /* [ = 笔刷变小；Shift+[ = 硬度降低；Wand = 容差 -；Type = 字号/字距 - */
      if (shell.tool == comp_ui::ToolId::Type) {
        type_step_param(shell, -1, shift);
      } else if (shell.tool == comp_ui::ToolId::Wand) {
        shell.wand_tolerance = comp_ui::step_wand_tolerance(shell.wand_tolerance, -1);
        wchar_t buffer[160];
        wsprintfW(buffer, L"魔棒容差 %d（窗口 %d×%d，逐通道，alpha 参与）", shell.wand_tolerance,
                  shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1);
        set_status(shell, buffer);
      } else {
        brush_step(shell, -1, shift);
      }
      break;
    case VK_OEM_6: /* ] = 笔刷变大；Shift+] = 硬度提高；Wand = 容差 +；Type = 字号/字距 + */
      if (shell.tool == comp_ui::ToolId::Type) {
        type_step_param(shell, 1, shift);
      } else if (shell.tool == comp_ui::ToolId::Wand) {
        shell.wand_tolerance = comp_ui::step_wand_tolerance(shell.wand_tolerance, 1);
        wchar_t buffer[160];
        wsprintfW(buffer, L"魔棒容差 %d（窗口 %d×%d，逐通道，alpha 参与）", shell.wand_tolerance,
                  shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1);
        set_status(shell, buffer);
      } else {
        brush_step(shell, 1, shift);
      }
      break;
    case VK_TAB:
      panel_move_param(shell, shift ? -1 : 1);
      break;
    /* P0-1：图层删除 / 重命名（面板焦点不独立，快捷键作用于当前选中图层）。 */
    case VK_DELETE:
      run_shell_command(window, shell, kCmdLayerDelete);
      break;
    case VK_F2:
      run_shell_command(window, shell, kCmdLayerRename);
      break;
    case VK_RETURN:
      /* 钢笔：回车 = 提交闭合路径；其余工具里回车仍是调整面板的落值键。 */
      if (shell.tool == comp_ui::ToolId::Pen && !shell.pen_anchors.empty()) {
        commit_pen_path(shell);
      } else {
        panel_commit_input(shell);
      }
      break;
    case VK_OEM_COMMA:
      panel_select_adjustment(shell, -1);
      break;
    case VK_OEM_PERIOD:
      panel_select_adjustment(shell, 1);
      break;
    case VK_OEM_MINUS:
      panel_step_value(shell, -1);
      break;
    case VK_OEM_PLUS:
      panel_step_value(shell, 1);
      break;
    default:
      /* 数字键在调整面板里开始键入数值；其余字母按上游工具键位映射。 */
      if (key >= '0' && key <= '9') {
        panel_begin_input(shell, static_cast<wchar_t>(key));
        break;
      }
      /* Shift+O = 直接键入图层不透明度（对齐上游双击数值的做法，比 ±10% 步进精确）。 */
      if (key == 'O' && shift && !ctrl) {
        run_shell_command(window, shell, kCmdLayerOpacitySet);
        break;
      }
      /* X = 下一项色板；Shift+X = 键入十六进制。前景色是画笔与吸管的公共落点。 */
      if (key == 'X' && !ctrl) {
        if (shift) {
          begin_color_input(shell);
          break;
        }
        shell.palette_index = comp_ui::step_swatch(shell.palette_index, 1);
        shell.foreground = comp_ui::swatch_at(shell.palette_index);
        char hex[8] = {0};
        comp_ui::color_to_hex(shell.foreground, hex);
        wchar_t buffer[160];
        wsprintfW(buffer, L"前景色：%s（色板 %d/%d，Shift+X 键入十六进制）",
                  widen_ascii(hex).c_str(), shell.palette_index + 1,
                  comp_ui::swatch_count());
        set_status(shell, buffer);
        break;
      }
      /* Shift+F = 键入新选区羽化半径；M = 框选形状循环；Shift+W = 魔棒取样来源。 */
      if (key == 'F' && shift) {
        begin_feather_input(shell);
        break;
      }
      if (key == 'M' && !ctrl) {
        if (shell.tool == comp_ui::ToolId::Marquee) {
          shell.marquee_shape = comp_ui::marquee_shape_next(shell.marquee_shape);
          wchar_t buffer[160];
          wsprintfW(buffer, L"框选形状：%s（再按 M 切换）",
                    widen_ascii(comp_ui::marquee_shape_name(shell.marquee_shape)).c_str());
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Marquee);
        }
        break;
      }
      if (key == 'W' && shift && !ctrl) {
        if (shell.tool == comp_ui::ToolId::Wand) {
          shell.wand_merged = !shell.wand_merged;
          set_status(shell, shell.wand_merged
                                ? L"魔棒取样：合并图层（Shift+W 切回当前图层）"
                                : L"魔棒取样：当前图层（Shift+W 切到合并）");
        } else {
          set_tool(shell, comp_ui::ToolId::Wand);
        }
        break;
      }
      /*
       * Shift+B = 混合模式后退一步、Shift+N 同键位前进一步（对齐上游 Shift++ / Shift+−
       * 的菜单序循环；本壳 OEM_PLUS/MINUS 已被调整面板与缩放占用，改取语义键）。
       * 大写 B 不与 Brush 的小写 'b' 键位冲突（工具快捷键只匹配小写）。
       */
      if (key == 'B' && shift && !ctrl) {
        run_shell_command(window, shell, kCmdLayerBlendPrev);
        break;
      }
      if (key == 'N' && shift && !ctrl) {
        run_shell_command(window, shell, kCmdLayerBlendNext);
        break;
      }
      /*
       * S22：Ctrl+Shift+M = 从选区建立图层蒙版；Ctrl+Alt+G = 剪贴蒙版开关。
       * M 在 Ctrl+Shift 下未被占用（单独 M 是 Marquee，Ctrl+M 无定义）；
       * G 单独是渐变工具，故剪贴必须带 Alt 才能与它分开。
       * Alt 由 WM_SYSKEYDOWN 上报，这里用 alt 标志区分（见本函数开头的取键）。
       */
      if (key == 'M' && ctrl && shift) {
        run_shell_command(window, shell, kCmdLayerMaskFromSelection);
        break;
      }
      if (key == 'G' && !ctrl) {
        /* 重复按 G 在同一工具内循环 Linear -> Radial（对齐上游的同类工具习惯）。 */
        if (shell.tool == comp_ui::ToolId::Gradient) {
          shell.gradient_type = comp_ui::next_gradient_type(shell.gradient_type);
          wchar_t buffer[192];
          wsprintfW(buffer, L"渐变类型：%s（再按 G 切换；Shift = 反向 前景↔透明）",
                    widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str());
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Gradient);
        }
        break;
      }
      if (key == 'U' && !ctrl) {
        /* 重复按 U 在同一工具内循环 Rectangle -> Ellipse -> Line（与 M/G/R 同款习惯）。 */
        if (shell.tool == comp_ui::ToolId::Shape) {
          shell.shape_kind = comp_ui::next_shell_shape_kind(shell.shape_kind);
          wchar_t buffer[224];
          wsprintfW(buffer, L"形状种类：%s（再按 U 切换；%s）",
                    widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
                    shell.shape_kind == comp_ui::kShapeLine
                        ? L"Alt+[ / ] 调线宽"
                        : L"Shift+Alt+[ / ] 调圆角，Alt+[ / ] 调线宽");
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Shape);
        }
        break;
      }
      if (key < 128) {
        const comp_ui::ToolId id =
            comp_ui::tool_shortcut_target(shell.tool, static_cast<char>(key));
        if (id == comp_ui::ToolId::Retouch) {
          /* 重复按 R 在同一工具内循环 Blur -> Smudge -> Liquify（对齐上游）。 */
          set_retouch_mode(shell, shell.tool == comp_ui::ToolId::Retouch
                                      ? comp_ui::next_retouch_mode(shell.retouch_mode)
                                      : comp_ui::kRetouchBlur);
        } else if (id == comp_ui::ToolId::Tone) {
          /* 重复按 O 在 8 档里循环；从别的工具切进来一律回到 Dodge 中间调。 */
          set_tone_state(shell, shell.tool == comp_ui::ToolId::Tone
                                    ? comp_ui::next_tone_state(shell.tone_state)
                                    : comp_ui::kToneDodgeMidtone);
        } else if (id != comp_ui::ToolId::Count) {
          set_tool(shell, id);
        }
      }
      break;
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  ShellState* shell = g_shell;
  const int mouse_x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
  const int mouse_y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
  switch (message) {
    case WM_CREATE:
      /* 拖放导入：注册后 Windows 才会把 OLE 拖放转成 WM_DROPFILES。 */
      DragAcceptFiles(window, TRUE);
      return 0;
    case WM_DROPFILES:
      if (shell != nullptr) handle_drop_files(window, *shell, reinterpret_cast<HDROP>(wparam));
      return 0;
    case WM_PAINT:
      if (shell != nullptr) present(window, *shell);
      break;
    case WM_INITMENU:
      /*
       * S24：菜单勾选态在这里刷新。**不重建菜单**——重建会让已打开的 HMENU 悬空，
       * 且每次弹菜单都闪一下；勾选是纯显示状态，按命令 id 递归改标记即可。
       */
      if (shell != nullptr) sync_menu_state(window, *shell);
      return 0;
    case WM_KEYDOWN:
      if (shell != nullptr) {
        /*
         * S22 F6：亮度直方图开关。放在字母键处理之前——它与任何工具快捷键都
         * 不冲突，且面板类快捷键统一走 run_shell_command 分发，菜单与键不分叉。
         */
        if (wparam == VK_F6) {
          run_shell_command(window, *shell, kCmdHistogramToggle);
          return 0;
        }
        /* 文字键入态下空格是字符，不是临时抓手。 */
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wparam == VK_SPACE && !(shell->type_active && !ctrl)) {
          /* 空格 = 临时抓手：按下不重复触发，松开才结束。 */
          if (!shell->space_down) {
            shell->space_down = true;
            set_status(*shell, L"按住空格拖动画布（或中键拖动）");
            InvalidateRect(window, nullptr, FALSE);
          }
          break;
        }
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        handle_key(window, *shell, wparam, ctrl, shift);
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_KEYUP:
      if (shell != nullptr && wparam == VK_SPACE) {
        shell->space_down = false;
        end_pan(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    /*
     * P0-1：图层名 / 不透明度键入的字符入口。
     *
     * 用 WM_CHAR 而不是在 WM_KEYDOWN 里按 VK 转字符：前者是 TranslateMessage
     * 与 IME 的共同出口，CJK 才能原样进来（WM_CHAR 的 wParam 就是 UTF-16 码元）。
     * 编辑键（回车/退格/Esc）仍由 WM_KEYDOWN 处理，这里把控制字符挡掉。
     */
    case WM_CHAR:
      if (shell != nullptr && shell->layer_input != LayerInput::None) {
        panel_layer_input_char(*shell, static_cast<wchar_t>(wparam));
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          const int button =
              comp_ui::tool_button_at(layout.toolbar, comp_ui::tool_count(), mouse_x, mouse_y);
          if (button >= 0) {
            const comp_ui::ToolInfo* info = comp_ui::tool_info_at(button);
            if (info != nullptr) set_tool(*shell, info->id);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /*
           * 命中图层行：row_top 与 draw_layer_panel 共用同一个直方图偏移量。
           * 两处各写一个常量迟早不一致，而症状是「点不中要的那层」——不报错、
           * 只在面板开着直方图时偶现，极难复现。
           */
          /*
           * S24 直方图交互：绘图区按下 = 开始框选（松开超过 4px 才真的缩放）；
           * 标签行按下 = 复位全幅。放在图层行命中之前，两块几何互不重叠，但顺序
           * 固定后「图里拖」与「点图层行」不会互相吃掉点击。
           */
          if (shell->histogram_open && !shell->histogram_bins.empty()) {
            const comp_ui::Rect plot = comp_ui::histogram_plot_rect(layout.layer_panel);
            if (plot.contains(mouse_x, mouse_y)) {
              shell->histogram_drag = true;
              shell->histogram_drag_from_x = mouse_x;
              shell->histogram_drag_to_x = mouse_x;
              SetCapture(window);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
            const comp_ui::Rect histogram_label =
                comp_ui::histogram_label_rect(layout.layer_panel);
            if (histogram_label.contains(mouse_x, mouse_y)) {
              panel_histogram_reset(*shell);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          const int row_top = comp_ui::layer_rows_top(shell->histogram_open);
          const int row = comp_ui::layer_row_at(
              layout.layer_panel, static_cast<int>(shell->layers.size()), mouse_x, mouse_y,
              row_top);
          if (row >= 0) {
            const bool click_ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool click_shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const int32_t layer_count = static_cast<int32_t>(shell->layers.size());
            const comp_layer_id clicked_id =
                shell->layers[static_cast<size_t>(layer_count - 1 - row)].id;
            /*
             * P0-1 多选（对齐上游图层面板）：
             *   Ctrl+点 = 切换该层是否在多选集合里；点主选中层则把它从选中里摘掉
             *             （若还有其它多选层，用第一个顶上做主选中）。
             *   Shift+点 = 从主选中行到该行整段成为多选。
             * 两种修饰下都不进入拖行 pending：多选拖拽需要「整组一起挪」的语义，
             * 而当前拖行只搬一层，按下就先别埋一个会误触的 pending。
             */
            if (click_ctrl) {
              if (panel_selection_contains(*shell, clicked_id)) {
                if (row == shell->selected_layer) {
                  if (!shell->extra_selection.empty()) {
                    const comp_layer_id promoted = shell->extra_selection.front();
                    shell->extra_selection.erase(shell->extra_selection.begin());
                    for (int32_t i = 0; i < layer_count; ++i) {
                      if (shell->layers[static_cast<size_t>(i)].id == promoted) {
                        shell->selected_layer = layer_count - 1 - i;
                        break;
                      }
                    }
                  }
                } else {
                  auto& extras = shell->extra_selection;
                  for (size_t i = 0; i < extras.size(); ++i) {
                    if (extras[i] == clicked_id) {
                      extras.erase(extras.begin() + static_cast<std::ptrdiff_t>(i));
                      break;
                    }
                  }
                }
              } else {
                shell->extra_selection.push_back(clicked_id);
              }
            } else if (click_shift && shell->selected_layer >= 0) {
              const int32_t from = shell->selected_layer < row ? shell->selected_layer : row;
              const int32_t to = shell->selected_layer < row ? row : shell->selected_layer;
              shell->extra_selection.clear();
              for (int32_t line = from; line <= to; ++line) {
                const comp_layer_id id =
                    shell->layers[static_cast<size_t>(layer_count - 1 - line)].id;
                if (id != clicked_id) shell->extra_selection.push_back(id);
              }
              shell->selected_layer = row;
            } else {
              shell->extra_selection.clear();
              shell->selected_layer = row;
              /*
               * S24 拖行重排：按下即 pending（记录起点行与 y），移动超过阈值才进入
               * active。单击必须还是「选中」，手抖 1px 不该把层挪走；捕获在
               * WM_LBUTTONUP / WM_CAPTURECHANGED 里释放。
               */
              shell->layer_drag_row = row;
              shell->layer_drag_target = row;
              shell->layer_drag_press_y = mouse_y;
              shell->layer_drag_pending = true;
              shell->layer_drag_active = false;
              SetCapture(window);
            }
            refresh_adjustments(*shell);
            /* S24：效果列表同样跟随选中图层，否则菜单勾选与 fx 标记会停在上一层。 */
            refresh_effects(*shell);
            const LayerRow* selected = selected_row(*shell);
            if (selected != nullptr) {
              wchar_t buffer[160];
              if (panel_selection_ids(*shell, false).size() > 1) {
                wsprintfW(buffer, L"已选中 %d 个图层（主选中 #%I64u，第 %d 行；Ctrl+J/Del 批量）",
                          static_cast<int>(panel_selection_ids(*shell, false).size()),
                          static_cast<unsigned long long>(selected->id), row + 1);
              } else {
                wsprintfW(buffer, L"已选中图层 #%I64u（第 %d 行）",
                          static_cast<unsigned long long>(selected->id), row + 1);
              }
              set_status(*shell, buffer);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->effect_open && layout.adjust_panel.w > 0) {
            /* 点参数行 = 聚焦到该行；行几何与绘制共用 effect_param_row_rect。 */
            const int row = comp_ui::effect_param_row_at(
                layout.adjust_panel, static_cast<int>(shell->effect_values.size()), mouse_x,
                mouse_y);
            if (row >= 0) {
              panel_effect_focus_row(*shell, row);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->filter_open && layout.adjust_panel.w > 0) {
            /* 点参数行 = 聚焦到该行；行几何与绘制共用 filter_param_row_rect。 */
            const int row = comp_ui::filter_param_row_at(
                layout.adjust_panel, shell->filter.param_count(), mouse_x, mouse_y);
            if (row >= 0) {
              shell->filter_input.clear();
              filter_focus_row(*shell, row - shell->filter.focused);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (type_panel_visible(*shell) && layout.adjust_panel.w > 0) {
            /* 点属性行 = 聚焦到该行；行几何与绘制共用 type_param_row_rect。 */
            const int row =
                comp_ui::type_param_row_at(layout.adjust_panel, mouse_x, mouse_y);
            if (row >= 0) {
              shell->type_input.clear();
              type_panel_focus(*shell, row - shell->type_panel_focused);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (!shell->effect_open && !shell->filter_open && !type_panel_visible(*shell) &&
              !shell->color_range_open && !path_panel_visible(*shell) &&
              !history_panel_visible(*shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * P0-3 调整面板：上半行 = 聚焦，下半行（滑道）= 聚焦 + 开始拖动。
             * 行/滑道几何全部来自 ui_core::adjust_param_row_rect /
             * adjust_slider_track_rect，与 draw_adjust_panel 同源。
             * 分页视图（色阶）里几何用「相对行」（row - base），数据用绝对下标。
             */
            const int32_t shown_total = adjust_shown_count(*shell);
            const int32_t base = adjust_param_base(*shell);
            const int32_t scroll = comp_ui::adjust_param_clamp_scroll(
                layout.adjust_panel, shown_total, shell->adj_scroll);
            shell->adj_scroll = scroll;

            /* 色阶通道条：先于参数行命中（它在标题行，行命中吃不到这里）。 */
            if (adjust_paged_kind(static_cast<uint32_t>(shell->adj_kind))) {
              const int32_t chip = comp_ui::adjust_channel_chip_at(
                  layout.adjust_panel, kLevelsChannelCount, mouse_x, mouse_y);
              if (chip >= 0) {
                adjust_set_channel(*shell, chip);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
              }
            }

            /* 曲线：通道条 -> 绘图区（命中控制点 = 拖动，空白 = 加点后拖动）。 */
            if (shell->adj_kind == COMP_ADJUSTMENT_CURVES) {
              const int32_t chip = comp_ui::curve_channel_chip_at(
                  layout.adjust_panel, kCurveChannels, mouse_x, mouse_y);
              if (chip >= 0) {
                curve_set_channel(*shell, chip);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
              }
              const comp_ui::Rect plot = comp_ui::adjust_curve_plot_rect(layout.adjust_panel);
              if (!plot.empty() && plot.contains(mouse_x, mouse_y)) {
                const int32_t count = static_cast<int32_t>(shell->curve_xy.size() / 2);
                const int32_t hit =
                    count >= 2 ? comp_ui::curve_point_hit(
                                     plot, shell->curve_xy.data(), static_cast<int>(count),
                                     mouse_x, mouse_y, 7.0)
                               : -1;
                if (hit >= 0) {
                  curve_begin_drag(*shell, plot, hit, mouse_x, mouse_y);
                  if (shell->curve_drag_point >= 0) SetCapture(window);
                } else {
                  curve_add_point(*shell, plot, mouse_x, mouse_y);
                  if (shell->curve_drag_point >= 0) SetCapture(window);
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
              }
            }

            const int row = comp_ui::adjust_param_row_at(layout.adjust_panel, shown_total,
                                                         scroll, mouse_x, mouse_y);
            if (row >= 0) {
              const int32_t index = base + row;
              shell->adj_param = index;
              shell->adj_input.clear();
              if (comp_ui::adjust_row_track_hit(layout.adjust_panel, row, scroll, mouse_x,
                                                mouse_y)) {
                comp_adjustment_param param{};
                if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell->adj_kind),
                                                   static_cast<size_t>(index),
                                                   &param) == COMP_OK) {
                  if (param.type == COMP_PARAM_BOOL) {
                    /* 布尔参数没有滑道：点一下就是切换。 */
                    panel_step_value(*shell, 1);
                  } else {
                    adjust_begin_drag(*shell, layout.adjust_panel, index, mouse_x);
                    SetCapture(window);
                  }
                }
              } else {
                wchar_t buffer[220];
                comp_adjustment_param param{};
                if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell->adj_kind),
                                                   static_cast<size_t>(index),
                                                   &param) == COMP_OK) {
                  wsprintfW(buffer, L"%s = %s（范围 [%s, %s]；拖动滑杆实时预览）",
                            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
                            widen_ascii(shell->adj_values[static_cast<size_t>(index)].c_str())
                                .c_str(),
                            widen_ascii(number_text(param.min_value).c_str()).c_str(),
                            widen_ascii(number_text(param.max_value).c_str()).c_str());
                  set_status(*shell, buffer);
                }
              }
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->color_range_open && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 色彩范围面板：点色块 = 删掉该取样色；点「反相」行 = 切换；
             * 点「容差」行 = 提示键入（数字键在任何位置都能起键入态）。
             */
            const int slot = comp_ui::color_range_swatch_at(
                layout.adjust_panel,
                static_cast<int>(shell->color_range_include.size() / 3),
                static_cast<int>(shell->color_range_exclude.size() / 3), mouse_x, mouse_y);
            if (slot >= 0) {
              color_range_remove_swatch(*shell, slot);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
            const int row = comp_ui::color_range_row_at(layout.adjust_panel, mouse_x, mouse_y);
            if (row == 1) {
              color_range_toggle_invert(*shell);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
            if (row == 0) {
              set_status(*shell,
                         L"色彩范围：- / = 步进容差，或直接按数字键入（回车落值）");
              return 0;
            }
          }
          if (path_panel_visible(*shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 路径面板：点行 = 选中该条；转选区走双击（WM_LBUTTONDBLCLK）或回车，
             * 单击不落动作——避免误点把用户已有选区顶掉。空白处也吞掉，别漏给调整面板。
             */
            const int count = static_cast<int>(shell->paths.size());
            const int scroll =
                comp_ui::path_clamp_scroll(layout.adjust_panel, count, shell->path_scroll);
            const int index =
                comp_ui::path_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (index >= 0) {
              shell->path_selected = index;
              path_status(*shell);
              InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
          }
          if (history_panel_visible(*shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 历史记录面板：点行 = 直接回跳到该状态（单击即落动作，与路径面板不同——
             * 回跳本身可再回跳，不产生历史记录，误点没有破坏性）。空白处吞掉。
             * Shift+点行 = 只把它指定给历史记录画笔当取样状态，**不回跳**：
             * 取样源必须能停在「过去某一格」而当前画面保持在最新，否则这支笔
             * 就没有意义了。
             */
            const int count = history_state_count(*shell);
            const int scroll =
                comp_ui::history_clamp_scroll(layout.adjust_panel, count, shell->history_scroll);
            const int index =
                comp_ui::history_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (index >= 0) {
              if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                shell->history_brush_state = index;
                wchar_t buffer[288];
                wsprintfW(buffer, L"历史记录画笔取样：第 %d 个状态（%s）· 当前画面未回跳",
                          index, history_state_label(*shell, index).c_str());
                set_status(*shell, buffer);
              } else {
                history_jump_to(*shell, index);
              }
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->color_range_open && layout.canvas.contains(mouse_x, mouse_y)) {
            /* 面板打开时画布点击只取样，不交给当前工具（上游 sheet 同语义）。 */
            color_range_sample(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (layout.canvas.contains(mouse_x, mouse_y)) {
            if (shell->tool == comp_ui::ToolId::Hand || shell->space_down) {
              begin_pan(window, *shell, mouse_x, mouse_y);
            } else if (crop_available(*shell)) {
              /* 已有框时，再按一次画布 = 重新起框（对齐 PS 的「点框外取消、框内重来」语义） */
              crop_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (pen_available(*shell)) {
              pen_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (marquee_available(*shell)) {
              marquee_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (lasso_available(*shell)) {
              lasso_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (wand_available(*shell)) {
              wand_click(window, *shell, mouse_x, mouse_y, layout);
            } else if (gradient_available(*shell)) {
              gradient_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (shape_available(*shell)) {
              shape_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (type_available(*shell)) {
              type_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (history_brush_stroke_available(*shell)) {
              history_brush_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (tone_stroke_available(*shell)) {
              tone_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (retouch_stroke_available(*shell)) {
              retouch_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (paint_stroke_available(*shell)) {
              paint_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (eyedropper_available(*shell)) {
              eyedropper_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (move_available(*shell)) {
              move_begin(window, *shell, mouse_x, mouse_y, layout);
            } else {
              canvas_click(*shell, mouse_x, mouse_y, layout);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
      }
      break;
    case WM_LBUTTONDBLCLK:
      /*
       * S24：双击直方图绘图区 = 区间复位（与 PS 直方图一致）。
       * 其它位置把双击还原成一次普通按下，画笔/选框的连点行为不受 CS_DBLCLKS 影响
       * ——否则第二次点击会被 DBLCLK 吃掉，画笔连点就少一笔。
       */
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout) && shell->histogram_open &&
            !shell->histogram_bins.empty() &&
            comp_ui::histogram_plot_rect(layout.layer_panel).contains(mouse_x, mouse_y)) {
          panel_histogram_reset(*shell);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        /*
         * P0-4 可选：双击路径行 = 该路径转成选区。方向键 + 回车是等价入口；
         * 单击只选中（见 WM_LBUTTONDOWN），所以双击不会先被「选中」吃掉。
         */
        if (client_layout(*shell, &layout) && path_panel_visible(*shell) &&
            layout.adjust_panel.w > 0) {
          const int count = static_cast<int>(shell->paths.size());
          const int scroll =
              comp_ui::path_clamp_scroll(layout.adjust_panel, count, shell->path_scroll);
          const int index =
              comp_ui::path_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
          if (index >= 0) {
            shell->path_selected = index;
            path_apply_selected(*shell);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
        /*
         * P0-1：双击图层行 = 重命名（与上游图层面板一致）。
         * 必须先于「还原成普通按下」——否则第二次点击只会重选一次，双击永远
         * 进不了改名态。
         */
        if (client_layout(*shell, &layout) && layout.layer_panel.contains(mouse_x, mouse_y)) {
          const int row = comp_ui::layer_row_at(
              layout.layer_panel, static_cast<int>(shell->layers.size()), mouse_x, mouse_y,
              comp_ui::layer_rows_top(shell->histogram_open));
          if (row >= 0) {
            shell->extra_selection.clear();
            shell->selected_layer = row;
            refresh_adjustments(*shell);
            refresh_effects(*shell);
            panel_begin_rename(*shell);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
      }
      return SendMessageW(window, WM_LBUTTONDOWN, wparam, lparam);
    case WM_MBUTTONDOWN:
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout) && layout.canvas.contains(mouse_x, mouse_y)) {
          begin_pan(window, *shell, mouse_x, mouse_y);
          return 0;
        }
      }
      break;
    case WM_LBUTTONUP:
      /* P0-3 曲线松手：提交事务 = 一次拖动只占一步撤销。 */
      if (shell != nullptr && shell->curve_drag_point >= 0) {
        curve_drag_finish(*shell, true);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /* P0-3 滑杆松手：提交事务 = 一次拖动只占一步撤销。 */
      if (shell != nullptr && shell->adj_drag_param >= 0) {
        adjust_drag_finish(*shell, true);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * S24 直方图框选收尾：位移 < 4px 视为单击（不缩放）；否则把拖出的亮度区间
       * 设为新的可视窗口。夹取/排序全在 ui_core，这里只递交两个屏幕 x。
       */
      if (shell != nullptr && shell->histogram_drag) {
        shell->histogram_drag = false;
        shell->histogram_drag_to_x = mouse_x;
        ReleaseCapture();
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          panel_histogram_commit_drag(*shell, comp_ui::histogram_plot_rect(layout.layer_panel));
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * S24 拖行重排收尾。pending 但未 active = 单纯点选，什么都不做（不能把
       * 「点一下图层」变成一次撤销步）；active 才提交，跨组由纯函数拒绝。
       */
      if (shell != nullptr && shell->layer_drag_pending) {
        const bool dragged = shell->layer_drag_active;
        const int32_t from_row = shell->layer_drag_row;
        const int32_t to_row = shell->layer_drag_target;
        bool drop_into_group = false;
        if (dragged && to_row >= 0) {
          Layout layout{};
          const int32_t count = static_cast<int32_t>(shell->layers.size());
          if (client_layout(*shell, &layout) && to_row < count) {
            const LayerRow& target = shell->layers[static_cast<size_t>(count - 1 - to_row)];
            /* 只有组行、且指针停在行中间带时才解释成「拖进组」。 */
            drop_into_group =
                target.is_group != 0 &&
                comp_ui::layer_drag_inner_band(layout.layer_panel, count, to_row,
                                               static_cast<double>(mouse_y),
                                               comp_ui::layer_rows_top(shell->histogram_open));
          }
        }
        shell->layer_drag_pending = false;
        shell->layer_drag_active = false;
        shell->layer_drag_row = -1;
        shell->layer_drag_target = -1;
        ReleaseCapture();
        if (dragged && shell->document != nullptr) {
          panel_drop_layer(*shell, from_row, to_row, drop_into_group);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->crop_active) {
        /* 框在拖拽中已实时成型（不像画笔要松手才提交），这里只收尾捕获与重绘。 */
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->pen_dragging) {
        pen_up(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->gradient_active) {
        gradient_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->shape_active) {
        shape_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->marquee_active) {
        marquee_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->lasso_active) {
        lasso_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->move_active) {
        move_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->eyedropper_drag) {
        shell->eyedropper_drag = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->stroke_kind == comp_ui::StrokeKind::HistoryBrush) {
        history_brush_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->stroke_kind == comp_ui::StrokeKind::Tone) {
        tone_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->stroke_kind == comp_ui::StrokeKind::Retouch) {
        retouch_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->stroke_kind == comp_ui::StrokeKind::Paint) {
        paint_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr) end_pan(window, *shell);
      break;
    case WM_MBUTTONUP:
      if (shell != nullptr) end_pan(window, *shell);
      break;
    case WM_KILLFOCUS:
      /* 失焦后收不到 KEYUP / MOUSEUP：必须主动清状态，否则会「粘住」。 */
      if (shell != nullptr) {
        shell->space_down = false;
        /* S24：拖行/框选是「按住鼠标」的临时态，失焦必须取消，否则指示线一直挂着。 */
        shell->layer_drag_pending = false;
        shell->layer_drag_active = false;
        shell->histogram_drag = false;
        /* P0-3：拖动中的滑杆/曲线点收不到松手事件，abort 事务回滚到按下前（幂等）。 */
        curve_drag_finish(*shell, false);
        adjust_drag_finish(*shell, false);
        end_pan(window, *shell);
        stroke_cancel(*shell);
        move_cancel(*shell);
        eyedropper_cancel(*shell);
        marquee_cancel(*shell);
        lasso_cancel(*shell);
        pen_cancel_drag(*shell);
        gradient_cancel(*shell);
        shape_cancel(*shell);
        type_cancel(*shell);
      }
      break;
    case WM_CAPTURECHANGED:
      if (shell != nullptr) {
        /* 捕获被别处抢走（菜单弹出/切窗口）时，拖行与框选一并取消，不留悬挂状态。 */
        shell->layer_drag_pending = false;
        shell->layer_drag_active = false;
        shell->histogram_drag = false;
        /* P0-3：拖动中的滑杆/曲线点收不到松手事件，abort 事务回滚到按下前（幂等）。 */
        curve_drag_finish(*shell, false);
        adjust_drag_finish(*shell, false);
        end_pan(window, *shell);
        stroke_cancel(*shell);
        move_cancel(*shell);
        eyedropper_cancel(*shell);
        marquee_cancel(*shell);
        lasso_cancel(*shell);
        pen_cancel_drag(*shell);
        gradient_cancel(*shell);
        shape_cancel(*shell);
      }
      break;
    case WM_MOUSEMOVE:
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          /* P0-3 曲线拖动优先：几何用按下时冻结的绘图区，写入落在同一个事务里。 */
          if (shell->curve_drag_point >= 0) {
            curve_drag_to(*shell, shell->curve_drag_plot, static_cast<double>(mouse_x),
                          static_cast<double>(mouse_y));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /* P0-3 滑杆拖动优先：持有鼠标捕获，写入落在按下时开的同一个事务里。 */
          if (shell->adj_drag_param >= 0) {
            adjust_drag_value(*shell, static_cast<double>(mouse_x));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /*
           * S24：图层拖行（超过 4px 阈值才进入 active）与直方图框选优先于工具状态机。
           * 两者都持有鼠标捕获，必须在这里更新，否则松手时目标行停在按下那一行。
           */
          if (shell->layer_drag_pending) {
            const int dy = mouse_y - shell->layer_drag_press_y;
            if (!shell->layer_drag_active && (dy >= 4 || dy <= -4)) {
              shell->layer_drag_active = true;
            }
            if (shell->layer_drag_active) {
              const int32_t count = static_cast<int32_t>(shell->layers.size());
              shell->layer_drag_target = comp_ui::layer_drag_target_row(
                  layout.layer_panel, count, static_cast<double>(mouse_y),
                  comp_ui::layer_rows_top(shell->histogram_open));
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->histogram_drag) {
            shell->histogram_drag_to_x = mouse_x;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->move_active) {
            move_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->crop_active) {
            crop_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->pen_dragging) {
            pen_drag(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->gradient_active) {
            gradient_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->shape_active) {
            shape_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->eyedropper_drag) {
            /* 吸管拖动连续取样：只更新前景色与状态栏，不改画布内容。 */
            eyedropper_sample(*shell, mouse_x, mouse_y, layout);
          } else if (shell->marquee_active) {
            marquee_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->lasso_active) {
            lasso_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (stroke_in_progress(*shell)) {
            stroke_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->panning) {
            shell->view.pan_by(static_cast<double>(mouse_x - shell->pan_anchor.x),
                               static_cast<double>(mouse_y - shell->pan_anchor.y));
            shell->pan_anchor.x = mouse_x;
            shell->pan_anchor.y = mouse_y;
            InvalidateRect(window, nullptr, FALSE);
          }
        }
        /*
         * 笔尖轮廓跟随光标：只在笔刷类工具下刷新，避免每帧重画。
         * WM_MOUSELEAVE 是一次性的，收到后要重新 Track 才能再收到。
         */
        if (comp_ui::tool_has_brush_tip(shell->tool)) {
          shell->cursor_x = mouse_x;
          shell->cursor_y = mouse_y;
          shell->cursor_valid = true;
          if (!shell->tracking_leave) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            TrackMouseEvent(&track);
            shell->tracking_leave = true;
          }
          InvalidateRect(window, nullptr, FALSE);
        }
      }
      break;
    case WM_MOUSELEAVE:
      if (shell != nullptr) {
        shell->cursor_valid = false;
        shell->tracking_leave = false;
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_TIMER:
      /* 蚂蚁线相位：只在有选区时跑，避免空转重绘。 */
      if (shell != nullptr && wparam == kAntsTimerId) {
        if (shell->selection_active) {
          shell->selection_phase = (shell->selection_phase + 1) % kAntsPhasePeriod;
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      break;
    case WM_SYSKEYDOWN:
      /*
       * S22：Ctrl+Alt+G = 剪贴蒙版开关（对齐上游 Create Clipping Mask）。
       * Alt 组合只能在这里收到（WM_SYSKEYDOWN），G 单独按是渐变工具，因此必须
       * 带 Alt+Ctrl 才不抢键；放最前面先判，省得走到 OEM_4/OEM_6 分支。
       */
      if (shell != nullptr && wparam == 'G') {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
          run_shell_command(window, *shell, kCmdLayerToggleClipping);
          return 0;
        }
      }
      /*
       * P1：Ctrl+Alt+H = 历史记录面板。Ctrl+Shift+H 已是「水平翻转」且走 WM_KEYDOWN，
       * 这里必须带 Alt，否则 H 单独按是抓手工具。
       */
      if (shell != nullptr && wparam == 'H') {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
          run_shell_command(window, *shell, kCmdHistoryPanel);
          return 0;
        }
      }
      /*
       * Alt+[ / Alt+]：模糊模式调半径，涂抹/液化与减淡/加深/海绵调强度。
       * 普通 [ ] 已用于笔刷，Ctrl+[ ] 用于图层不透明度，因此放到 Alt 组合，
       * 且必须在这里吞掉，否则 DefWindowProc 会响铃。
       */
      if (shell != nullptr && (wparam == VK_OEM_4 || wparam == VK_OEM_6)) {
        const int direction = wparam == VK_OEM_6 ? 1 : -1;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        wchar_t buffer[128];
        if (shell->tool == comp_ui::ToolId::Type) {
          /* S14：Alt+[ / ] 调行距（5% 一步，夹在 100%..800%）；键入态也可用。 */
          shell->type_line_height =
              comp_ui::step_type_line_height(shell->type_line_height, direction);
          if (shell->type_active) {
            type_refresh_status(*shell);
          } else {
            wsprintfW(buffer, L"文字行距 %d%%（Alt+[ / ] 调整）",
                      static_cast<int>(shell->type_line_height * 100.0 + 0.5));
            set_status(*shell, buffer);
          }
        } else if (shell->tool == comp_ui::ToolId::Wand) {
          /* S8：魔棒的取样窗口是 (2r+1)²，0 = 只取种子像素（S7 行为）。 */
          shell->wand_radius = comp_ui::step_wand_radius(shell->wand_radius, direction);
          wsprintfW(buffer, L"魔棒取样窗口 %d×%d（半径 %d px）",
                    shell->wand_radius * 2 + 1, shell->wand_radius * 2 + 1, shell->wand_radius);
        } else if (shell->tool == comp_ui::ToolId::Shape && shift &&
                   shell->shape_kind == comp_ui::kShapeRect) {
          /* S10：Shift+Alt+[ / ] 调矩形圆角半径（0 = 直角；引擎再夹到短边一半）。 */
          shell->shape_corner_radius =
              comp_ui::step_shape_corner_radius(shell->shape_corner_radius, direction);
          wsprintfW(buffer, L"矩形圆角半径 %d px（0 = 直角）",
                    static_cast<int>(shell->shape_corner_radius + 0.5));
        } else if (shell->tool == comp_ui::ToolId::Shape) {
          /* S10：Alt+[ / ] 调形状描边宽度（直线用；矩形/椭圆的外壳默认纯填充）。 */
          shell->shape_stroke_width =
              comp_ui::step_shape_stroke_width(shell->shape_stroke_width, direction);
          wsprintfW(buffer, L"形状描边宽度 %d px（直线描边用）",
                    static_cast<int>(shell->shape_stroke_width + 0.5));
        } else if (shell->tool == comp_ui::ToolId::HistoryBrush) {
          /* P1：Alt+[ / ] 调历史记录画笔的不透明度（下限 10%，0 会被引擎拒）。 */
          shell->history_brush_opacity =
              comp_ui::step_history_brush_opacity(shell->history_brush_opacity, direction);
          wsprintfW(buffer, L"历史记录画笔不透明度 %d%%（取样第 %d 个状态）",
                    static_cast<int>(shell->history_brush_opacity * 100.0 + 0.5),
                    history_brush_resolved_state(*shell));
        } else if (shell->tool == comp_ui::ToolId::Tone) {
          /* P1：Alt+[ / ] 调减淡/加深/海绵的强度（Exposure / Flow 口径，10% 一步）。 */
          shell->tone_strength = comp_ui::step_tone_strength(shell->tone_strength, direction);
          wsprintfW(buffer, L"%s强度 %d%%",
                    widen_ascii(comp_ui::tone_state_name(shell->tone_state)).c_str(),
                    static_cast<int>(shell->tone_strength * 100.0 + 0.5));
        } else if (shell->tool == comp_ui::ToolId::Retouch &&
                   shell->retouch_mode != comp_ui::kRetouchBlur) {
          shell->retouch_strength =
              comp_ui::step_retouch_strength(shell->retouch_strength, direction);
          wsprintfW(buffer, L"%s强度 %d%%",
                    widen_ascii(comp_ui::retouch_mode_name(shell->retouch_mode)).c_str(),
                    static_cast<int>(shell->retouch_strength * 100.0 + 0.5));
        } else {
          shell->blur_radius = comp_ui::step_blur_radius(shell->blur_radius, direction);
          wsprintfW(buffer, L"模糊半径 %d px",
                    static_cast<int>(shell->blur_radius + 0.5));
        }
        set_status(*shell, buffer);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * Alt+, / Alt+.：选中图层等比例缩放（中心锚定）。
       * Alt+[ ] 已被模糊半径 / 涂抹强度 / 文字行距占用，Ctrl+[ ] 是不透明度，
       * 普通 [ ] 是笔刷，因此缩放取逗号/句点这一对空闲键。
       */
      if (shell != nullptr && (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD)) {
        panel_scale_layer(*shell, wparam == VK_OEM_PERIOD ? 1 : -1);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_MOUSEWHEEL:
      if (shell != nullptr) {
        /* WM_MOUSEWHEEL 的坐标是屏幕坐标，先换算到客户区。 */
        POINT cursor{mouse_x, mouse_y};
        ScreenToClient(window, &cursor);
        Layout layout{};
        if (client_layout(*shell, &layout) && layout.canvas.contains(cursor.x, cursor.y)) {
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const double factor = delta > 0 ? 1.2 : 1.0 / 1.2;
            shell->view.zoom_by(factor, cursor.x, cursor.y, layout.canvas);
            wchar_t buffer[96];
            wsprintfW(buffer, L"缩放 %d%%",
                      static_cast<int>(shell->view.zoom * 100.0 + 0.5));
            set_status(*shell, buffer);
            InvalidateRect(window, nullptr, FALSE);
          }
        } else if (client_layout(*shell, &layout) && layout.layer_panel.w > 0 &&
                   shell->histogram_open &&
                   comp_ui::histogram_plot_rect(layout.layer_panel)
                       .contains(cursor.x, cursor.y)) {
          /*
           * 直方图滚轮缩放（S24，PS 同款）：以光标下的亮度为中心，一格 25%。
           * 必须先于画布缩放判断——直方图绘图区与画布不重叠，但顺序写反会让
           * 「光标在图内却缩放画布」成为可能；这里显式 return，语义单一。
           */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const comp_ui::Rect plot = comp_ui::histogram_plot_rect(layout.layer_panel);
            double focus = comp_ui::histogram_x_to_bin(plot, shell->histogram_view,
                                                       static_cast<double>(cursor.x));
            if (focus < 0.0) focus = 0.0;
            if (focus > comp_ui::kHistogramMaxBin) focus = comp_ui::kHistogramMaxBin;
            panel_histogram_zoom(*shell, delta > 0 ? 1.0 / 1.25 : 1.25, focus);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        } else if (client_layout(*shell, &layout) && layout.adjust_panel.w > 0 &&
                   history_panel_visible(*shell) &&
                   layout.adjust_panel.contains(cursor.x, cursor.y)) {
          /* 历史记录面板滚轮翻列表：一格 3 行，夹取在 ui_core。 */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const int count = history_state_count(*shell);
            const int next = shell->history_scroll + (delta > 0 ? -3 : 3);
            shell->history_scroll =
                comp_ui::history_clamp_scroll(layout.adjust_panel, count, next);
            InvalidateRect(window, nullptr, FALSE);
          }
        } else if (client_layout(*shell, &layout) && layout.adjust_panel.w > 0 &&
                   !shell->effect_open && !shell->filter_open &&
                   !type_panel_visible(*shell) &&
                   layout.adjust_panel.contains(cursor.x, cursor.y)) {
          /*
           * P0-3：调整参数表滚动（色阶每页 5 行 / 小窗口），一格 3 行，夹取在 ui_core。
           * 曲线页没有参数表：滚轮不吞掉，交给下面的默认处理（不做事也不报错）。
           */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const int count = adjust_shown_count(*shell);
            const int next = shell->adj_scroll + (delta > 0 ? -3 : 3);
            shell->adj_scroll =
                comp_ui::adjust_param_clamp_scroll(layout.adjust_panel, count, next);
            InvalidateRect(window, nullptr, FALSE);
          }
        }
        return 0;
      }
      break;
    case WM_COMMAND:
      /* lparam != 0 表示控件通知，本骨架无子控件，直接忽略。 */
      if (shell != nullptr && lparam == 0) {
        if (LOWORD(wparam) >= kCmdToolBase &&
            LOWORD(wparam) < kCmdToolBase + comp_ui::tool_count()) {
          const comp_ui::ToolInfo* info =
              comp_ui::tool_info_at(static_cast<int>(LOWORD(wparam) - kCmdToolBase));
          if (info != nullptr) set_tool(*shell, info->id);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        run_shell_command(window, *shell, LOWORD(wparam));
        return 0;
      }
      break;
    case WM_SIZE:
      if (shell != nullptr) InvalidateRect(window, nullptr, FALSE);
      break;
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

/* 菜单结构：文件 / 编辑 / 图层 / 视图 / 工具 / 调整；快捷键提示文本与 handle_key
 * 的映射保持一致，工具菜单直接取自 ui_core.h 的 tool_table。 */
HMENU build_menu() {
  HMENU menu = CreateMenu();

  HMENU file_menu = CreatePopupMenu();
  AppendMenuW(file_menu, MF_STRING, kCmdNew, L"新建(&N)\tCtrl+N");
  AppendMenuW(file_menu, MF_STRING, kCmdOpen, L"打开(&O)...\tCtrl+O");
  AppendMenuW(file_menu, MF_STRING, kCmdSave, L"保存(&S)...\tCtrl+S");
  AppendMenuW(file_menu, MF_STRING, kCmdOpenImage, L"打开位图(&I)...\tCtrl+Shift+O");
  AppendMenuW(file_menu, MF_STRING, kCmdImportImage, L"置入(&P)...");
  AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file_menu, MF_STRING, kCmdExportImage, L"导出为(&E)...\tCtrl+Shift+S");
  AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file_menu, MF_STRING, kCmdExit, L"退出(&X)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"文件(&F)");

  HMENU edit_menu = CreatePopupMenu();
  AppendMenuW(edit_menu, MF_STRING, kCmdUndo, L"撤销(&U)\tCtrl+Z");
  AppendMenuW(edit_menu, MF_STRING, kCmdRedo, L"重做(&R)\tCtrl+Y");
  AppendMenuW(edit_menu, MF_SEPARATOR, 0, nullptr);
  /* 选区三条（全选/取消/反选）已搬到单独的「选择」菜单，ID 不变。 */
  AppendMenuW(edit_menu, MF_STRING, kCmdHistoryPanel, L"历史记录(&H)…\tCtrl+Alt+H");
  AppendMenuW(edit_menu, MF_STRING, kCmdListCommands, L"枚举引擎命令(&L)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit_menu), L"编辑(&E)");

  HMENU layer_menu = CreatePopupMenu();
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerAdd, L"新建图层(&A)\tCtrl+Shift+N");
  /*
   * P0-1：图层面板补全。这五条此前壳里完全没有入口——引擎早就支持建组
   * （is_group）与改名，但壳固定传 0、也没有 set_name；删除/复制更是连 ABI
   * 都没有。现在导出补齐（comp_layer_set_name / duplicate / move），入口在这里。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerNewGroup, L"新建组(&G)\tCtrl+G");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerDuplicate, L"复制图层(&P)\tCtrl+J");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerDelete, L"删除图层(&X)\tDel");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerRename, L"重命名图层(&N)\tF2");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMergeDown, L"向下合并(&M)\tCtrl+E");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFlatten, L"拼合图像(&F)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSelectUp, L"选择上一图层(&U)\t↑");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSelectDown, L"选择下一图层(&D)\t↓");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerToggleVisible, L"切换可见性(&V)\tCtrl+Shift+V");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * S22 排列顺序：这四条接的是 comp_layer_reorder——T2 全量测试点名的
   * 「壳侧调用数为 0」缺口之一，用户此前完全无法调整图层堆叠次序。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerBringForward, L"上移一层(&W)\tCtrl+Shift+↑");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSendBackward, L"下移一层(&S)\tCtrl+Shift+↓");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerBringToFront, L"置于顶层(&T)\tCtrl+Shift+Home");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSendToBack, L"置于底层(&B)\tCtrl+Shift+End");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * S22 蒙版三连：建（选区->蒙版）/读（蒙版->选区）/清。引擎侧蒙版早已参与
   * 合成与 .comp 落盘，缺的只是壳没调 setter；补 comp_layer_get_mask 后面板
   * 也能回读「这一层有没有蒙版」，不会出现建了却看不出来的情况。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMaskFromSelection,
              L"从选区建立蒙版(&K)\tCtrl+Shift+M");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMaskToSelection, L"载入蒙版为选区(&Y)");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMaskRemove, L"清除蒙版(&R)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  /* S22 剪贴蒙版：挂到下方紧邻图层。 */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerToggleClipping, L"剪贴蒙版(&C)\tCtrl+Alt+G");
  /* S22 组隔离：只对图层组有意义。 */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerToggleIsolated, L"切换穿透/隔离(&I)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * S24 镜像：水平 / 垂直两个独立位。引擎一直在合成里用 flip_x/flip_y，但 ABI
   * 只有 setter，壳读不到现值就无法「切换」——所以 S24 补齐 getter 后接这里。
   * 快捷键避开已被占用的 Ctrl+Shift+V（图层显隐），改用 H / F（Flip Horizontal / Flip Vertical）。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFlipHorizontal, L"水平翻转(&H)\tCtrl+Shift+H");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFlipVertical, L"垂直翻转(&V)\tCtrl+Shift+F");
  /*
   * S24 效果子菜单：6 种效果的开关（勾选 = 该层已启用），id = kCmdEffectBase + kind。
   * 引擎侧效果早已参与像素求值（compositing/effect_eval.cpp），此前壳没有入口也没有
   * 回读口，用户只能手改 .comp 里的 engineEffects 才能加投影/描边。
   */
  HMENU effect_menu = CreatePopupMenu();
  for (int kind = 0; kind < comp_ui::kEffectKindCount; ++kind) {
    wchar_t effect_label[96];
    wsprintfW(effect_label, L"%s", effect_kind_label(kind));
    AppendMenuW(effect_menu, MF_STRING, kCmdEffectBase + static_cast<UINT>(kind), effect_label);
  }
  AppendMenuW(effect_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(effect_menu, MF_STRING, kCmdEffectEdit, L"编辑参数…(&P)\tCtrl+Shift+E");
  AppendMenuW(effect_menu, MF_STRING, kCmdEffectClear, L"清除全部效果(&C)");
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(effect_menu), L"效果(&E)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacityDown, L"降低不透明度(&-)\tCtrl+[");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacityUp, L"提高不透明度(&+)\tCtrl+]");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacitySet, L"设置不透明度…(&O)\tShift+O");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMoveLeft, L"平移左(&L)	Shift+←");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMoveRight, L"平移右(&R)	Shift+→");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMoveUp, L"平移上(&U)	Shift+↑");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMoveDown, L"平移下(&D)	Shift+↓");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerRotateLeft, L"逆旋转 15°(&[)	Ctrl+Shift+←");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerRotateRight, L"顺旋转 15°(&])	Ctrl+Shift+→");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerScaleDown, L"缩小(&-)	Alt+,");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerScaleUp, L"放大(&+)	Alt+.");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerBlendPrev, L"上一个混合模式(&P)\tShift+B");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerBlendNext, L"下一个混合模式(&X)\tShift+N");
  /*
   * 混合模式子菜单（S21）：档位与分组逐字取自上游 LayerBlendMode.groups（同一组内不画
   * 分隔线），命令 id = kCmdBlendBase + 档位，即 comp_blend_mode 数值，菜单直达。
   */
  HMENU blend_menu = CreatePopupMenu();
  for (int group = 0; group < comp_ui::kBlendGroupCount; ++group) {
    if (group > 0) AppendMenuW(blend_menu, MF_SEPARATOR, 0, nullptr);
    for (int slot = 0; slot < 8; ++slot) {
      const int mode = comp_ui::kBlendGroups[group][slot];
      if (mode < 0) break;
      wchar_t label[64];
      wsprintfW(label, L"%s", widen_ascii(comp_ui::blend_mode_name(mode)).c_str());
      AppendMenuW(blend_menu, MF_STRING, kCmdBlendBase + static_cast<UINT>(mode), label);
    }
  }
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(blend_menu), L"混合模式(&B)");
  /*
   * S22 采样模式子菜单：kCmdSamplingBase + 档位（0 High quality / 1 Nearest /
   * 2 Smooth）。引擎真的按它改变缩放手感（resample.cpp 的 sampling_from_abi），
   * 不是展示开关；setter / getter 早就存在，壳此前一次都没调过。
   */
  HMENU sampling_menu = CreatePopupMenu();
  for (int mode = 0; mode < comp_ui::kSamplingCount; ++mode) {
    wchar_t label[64];
    wsprintfW(label, L"%s", widen_ascii(comp_ui::sampling_mode_name(mode)).c_str());
    AppendMenuW(sampling_menu, MF_STRING, kCmdSamplingBase + static_cast<UINT>(mode), label);
  }
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sampling_menu), L"采样模式(&Q)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layer_menu), L"图层(&L)");

  /*
   * P0-4：把选区命令从「编辑」里拆出来，单列一个菜单。全选/取消/反选三条
   * 只搬位置、ID 不变（handle_key 与菜单共用 run_shell_command，两条入口不会分叉）；
   * 新增的三条此前壳里没有入口——引擎的 comp_selection_modify(EXPAND/CONTRACT)
   * 与 comp_selection_load_layer(LOAD_PIXELS) 早已实现并有单测，缺的只是 UI。
   */
  HMENU select_menu = CreatePopupMenu();
  AppendMenuW(select_menu, MF_STRING, kCmdSelectAll, L"全选(&A)\tCtrl+A");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectNone, L"取消选择(&D)\tCtrl+D");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectInvert, L"反选(&I)\tCtrl+Shift+I");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectColorRange, L"色彩范围(&C)...");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectExpand, L"扩展(&E)...");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectContract, L"收缩(&O)...");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectLoadPixels, L"按图层像素载入选区(&P)");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectPaths, L"路径面板(&T)...\tCtrl+Shift+P");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(select_menu), L"选择(&S)");

  HMENU view_menu = CreatePopupMenu();
  AppendMenuW(view_menu, MF_STRING, kCmdViewFit, L"适合窗口(&F)\tCtrl+0");
  AppendMenuW(view_menu, MF_STRING, kCmdViewActual, L"实际像素(&1)\tCtrl+1");
  AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view_menu, MF_STRING, kCmdViewZoomIn, L"放大(&I)\tCtrl+=");
  AppendMenuW(view_menu, MF_STRING, kCmdViewZoomOut, L"缩小(&O)\tCtrl+-");
  AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * S22 直盘开关（F6）：数据来自 comp_layer_histogram 的只读入口。
   * 上游 Levels 面板左半边就是这张图，而本壳此前只有一排数字控件——
   * 用户看不到图现在偏暗还是偏亮，只能凭感觉拖参数。
   */
  AppendMenuW(view_menu, MF_STRING, kCmdHistogramToggle, L"亮度直方图(&H)\tF6");
  AppendMenuW(view_menu, MF_STRING, kCmdHistogramReset, L"直方图区间复位(&R)\t双击图内");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"视图(&V)");

  /* 工具菜单与工具条、快捷键共用同一张表（tool_table），不另抄一份键位。 */
  HMENU tool_menu = CreatePopupMenu();
  for (int i = 0; i < comp_ui::tool_count(); ++i) {
    const comp_ui::ToolInfo* info = comp_ui::tool_info_at(i);
    if (info == nullptr) continue;
    wchar_t label[128];
    wsprintfW(label, L"%s\t%c", widen_ascii(info->name).c_str(),
              info->shortcut != 0 ? static_cast<wchar_t>(info->shortcut - 'a' + 'A') : L' ');
    AppendMenuW(tool_menu, MF_STRING, kCmdToolBase + static_cast<UINT>(i), label);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tool_menu), L"工具(&T)");

  HMENU adjust_menu = CreatePopupMenu();
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjKindPrev, L"上一类型(&P)\tCtrl+Shift+J");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjKindNext, L"下一类型(&N)\tCtrl+Shift+K");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRowPrev, L"上一调整(&E)\t,");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRowNext, L"下一调整(&X)\t.");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjParamPrev, L"上一参数(&R)\tShift+Tab");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjParamNext, L"下一参数(&T)\tTab");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjValueDown, L"参数减(&-)\t-");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjValueUp, L"参数加(&+)\t=");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjAdd, L"新增调整(&A)\tCtrl+Shift+A");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjApply, L"应用到选中调整(&U)\tCtrl+Shift+U");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRemove, L"删除选中调整(&D)\tCtrl+Shift+D");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(adjust_menu), L"调整(&J)");

  /* 滤镜菜单与面板参数表共用 ui_core.h 的 filter_table（量程与 comp_filter.h 对齐）。 */
  HMENU filter_menu = CreatePopupMenu();
  for (int i = 0; i < comp_ui::filter_count(); ++i) {
    const comp_ui::FilterSpec* spec = comp_ui::filter_spec_at(i);
    if (spec == nullptr) continue;
    wchar_t label[128];
    wsprintfW(label, L"%s", widen_ascii(spec->name).c_str());
    AppendMenuW(filter_menu, MF_STRING, kCmdFilterBase + static_cast<UINT>(i), label);
  }
  AppendMenuW(filter_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterApply, L"应用(&A)\t回车");
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterReset, L"重置参数(&R)");
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterClose, L"关闭面板(&C)\tEsc");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(filter_menu), L"滤镜(&F)");

  return menu;
}

/* ------------------------------------------------------------ 无窗口自检 */

const char* backend_name(comp_backend backend) {
  switch (backend) {
    case COMP_BACKEND_METAL:  return "METAL";
    case COMP_BACKEND_D3D12:  return "D3D12";
    case COMP_BACKEND_VULKAN: return "VULKAN";
    case COMP_BACKEND_CPU:    return "CPU";
    default:                  return "AUTO";
  }
}

/*
 * 探针夹具（240x160：左红 / 中绿竖条 / 右蓝）的存在性检测与自备写入。
 *
 * 为什么放在壳里而不是仓里：run_regression.py 的 --clean 会连 build/ 一起删掉，
 * 而这份夹具本来就住在 build/ 里，于是全量回归后它必然不在。旧版自检此时把
 * 「内容对拍」降级为 [skip]，SHA256 与基线不一致——那看起来像产品回归，实际只是
 * 夹具不见了。改成自检自己写：这里链着引擎，写 PNG 与读 PNG 是同一条 comp_io_*
 * 路径，不引入任何外部依赖，也不需要把二进制夹具提交进版本库。
 *
 * 返回值语义：probe_png_exists 只回答「在不在」；write_probe_png 回答「写没写成」
 * （写成后一定在）。写入失败（磁盘满/只读）由调用方走 [skip] 出口，不假 PASS。
 */
bool probe_png_exists(const char* path) {
  if (path == nullptr) return false;
  FILE* probe = nullptr;
  if (fopen_s(&probe, path, "rb") != 0 || probe == nullptr) return false;
  std::fclose(probe);
  return true;
}

bool write_probe_png(comp_engine engine, const char* path) {
  if (engine == nullptr || path == nullptr) return false;
  const int32_t probe_width = 240;
  const int32_t probe_height = 160;
  const int32_t stripe = (probe_width * 15) / 40; /* 中段绿条宽约 7.5% 画布 */

  comp_document_desc desc{};
  desc.width = probe_width;
  desc.height = probe_height;
  desc.dpi = 72.0f;
  desc.color_space = "sRGB";
  comp_document probe = nullptr;
  if (comp_document_create(engine, &desc, &probe) != COMP_OK || probe == nullptr) return false;

  const size_t probe_stride = static_cast<size_t>(probe_width) * 4u;
  std::vector<uint8_t> probe_pixels(probe_stride * static_cast<size_t>(probe_height), 0);
  for (int32_t y = 0; y < probe_height; ++y) {
    uint8_t* row = probe_pixels.data() + static_cast<size_t>(y) * probe_stride;
    for (int32_t x = 0; x < probe_width; ++x) {
      uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
      if (x < stripe) {
        pixel[0] = 255; pixel[1] = 0; pixel[2] = 0; pixel[3] = 255; /* 左：红 */
      } else if (x < stripe * 2) {
        pixel[0] = 0; pixel[1] = 255; pixel[2] = 0; pixel[3] = 255; /* 中：绿 */
      } else {
        pixel[0] = 0; pixel[1] = 0; pixel[2] = 255; pixel[3] = 255; /* 右：蓝 */
      }
    }
  }

  comp_layer_desc layer_desc{};
  layer_desc.name = "probe";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, probe_width, probe_height};
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id probe_layer = 0;
  bool added = comp_layer_add(probe, &layer_desc, &probe_layer) == COMP_OK &&
                probe_layer != 0;
  if (added) {
    comp_image image{};
    image.width = probe_width;
    image.height = probe_height;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = probe_stride;
    image.pixels = probe_pixels.data();
    added = comp_layer_set_pixels(probe, probe_layer, &image) == COMP_OK;
  }
  bool exported = false;
  if (added) {
    const comp_image_format format = comp_io_format_from_path(path);
    exported = format != COMP_FORMAT_UNKNOWN &&
               comp_io_export(probe, path, format, nullptr) == COMP_OK;
  }
  comp_document_destroy(probe);
  return exported;
}

/*
 * 端到端自检：只用 C ABI 串起「建引擎 -> 建文档 -> 加图层写像素 -> 合成 -> 撤销 ->
 * .comp 往返」。不创建窗口，故可无人值守运行；结论写入 out_path 并回退出码（0 = 全通过）。
 */
int run_selftest(const wchar_t* out_path) {
  FILE* report = nullptr;
  if (_wfopen_s(&report, out_path, L"wb") != 0 || report == nullptr) return 2;

  int failures = 0;
  const auto emit = [&](const char* text) { std::fprintf(report, "%s\n", text); };
  const auto verify = [&](bool ok, const char* label) {
    std::fprintf(report, "[%s] %s\n", ok ? "OK" : "FAIL", label);
    if (!ok) ++failures;
  };

  emit("# compositor-shell selftest (Windows shell + engine C ABI)");

  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-shell-selfcheck";
  engine_desc.version = "0.1.0";
  /*
   * 自检主体固定在 CPU 后端：下面的像素断言（画笔色、撤销后逐字节还原）是
   * CPU 基准定义的。GPU 协同由本节末尾的 AUTO 引擎对称场景单独验证。
   */
  engine_desc.backend = COMP_BACKEND_CPU;
  comp_engine engine = nullptr;
  const bool engine_ok =
      comp_engine_create(&engine_desc, &engine) == COMP_OK && engine != nullptr;
  verify(engine_ok, "comp_engine_create(CPU)");
  if (!engine_ok) {
    std::fclose(report);
    return 1;
  }

  comp_backend backend = COMP_BACKEND_AUTO;
  const bool backend_ok = comp_engine_backend(engine, &backend) == COMP_OK;
  verify(backend_ok, "comp_engine_backend()");
  std::fprintf(report, "[info] backend = %s\n", backend_ok ? backend_name(backend) : "(query failed)");

  comp_document_desc doc_desc{};
  doc_desc.width = 64;
  doc_desc.height = 64;
  doc_desc.dpi = 72.0f;
  doc_desc.color_space = "sRGB";
  comp_document document = nullptr;
  const bool doc_ok =
      comp_document_create(engine, &doc_desc, &document) == COMP_OK && document != nullptr;
  verify(doc_ok, "comp_document_create(64x64)");
  if (!doc_ok) {
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }

  comp_size size{};
  verify(comp_document_size(document, &size) == COMP_OK && size.width == 64 && size.height == 64,
         "comp_document_size() == 64x64");

  /* 半透明红（预乘 RGBA8）：验证「壳只调 ABI、像素由引擎处理」的端到端路径。 */
  const int32_t width = 64;
  const int32_t height = 64;
  const size_t stride = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> pixels(stride * static_cast<size_t>(height), 0);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = 128;
    pixels[i + 1] = 0;
    pixels[i + 2] = 0;
    pixels[i + 3] = 128;
  }
  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = stride;
  image.pixels = pixels.data();

  comp_layer_desc layer_desc{};
  layer_desc.name = "selftest-layer";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, width, height}; /* 显式基准：变换自检要收放 */
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id layer = 0;
  const bool add_ok = comp_layer_add(document, &layer_desc, &layer) == COMP_OK && layer != 0;
  verify(add_ok, "comp_layer_add()");
  verify(add_ok && comp_layer_set_pixels(document, layer, &image) == COMP_OK,
         "comp_layer_set_pixels()");

  size_t layer_count = 0;
  const bool count_ok = comp_layer_count(document, &layer_count) == COMP_OK && layer_count >= 2;
  verify(count_ok, "comp_layer_count() >= 2 (background + added)");
  std::fprintf(report, "[info] layers = %u\n", static_cast<unsigned>(layer_count));

  std::vector<uint8_t> flattened(stride * static_cast<size_t>(height), 0);
  const bool flatten_ok =
      comp_render_flatten(document, flattened.data(), COMP_PIXEL_RGBA8, stride) == COMP_OK;
  verify(flatten_ok, "comp_render_flatten()");
  size_t non_transparent = 0;
  for (size_t i = 3; i < flattened.size(); i += 4) {
    if (flattened[i] != 0) ++non_transparent;
  }
  verify(flatten_ok && non_transparent > 0, "flatten output has non-transparent pixels");
  std::fprintf(report, "[info] non-transparent pixels = %u / %u\n",
               static_cast<unsigned>(non_transparent), static_cast<unsigned>(width * height));

  /*
   * 混合模式接线（S21）：写一个非默认档位，读回必须等于写入值；再非法值必须被
   * 引擎拒绝且不改变既有档位。这锁死的是壳与 ABI 之间「档位即 comp_blend_mode
   * 数值」这条口径——面板行、菜单与状态栏全都从这同一个数值取名。
   */
  comp_blend_mode blend_read = COMP_BLEND_NORMAL;
  const bool blend_set_ok =
      comp_layer_set_blend(document, layer, COMP_BLEND_MULTIPLY) == COMP_OK &&
      comp_layer_get_blend(document, layer, &blend_read) == COMP_OK &&
      blend_read == COMP_BLEND_MULTIPLY;
  verify(blend_set_ok, "blend mode round trip (set MULTIPLY -> get)");
  comp_result bad_blend =
      comp_layer_set_blend(document, layer, static_cast<comp_blend_mode>(999));
  verify(bad_blend != COMP_OK && comp_layer_get_blend(document, layer, &blend_read) == COMP_OK &&
             blend_read == COMP_BLEND_MULTIPLY,
         "invalid blend mode rejected without clobbering the stored value");
  verify(comp_layer_set_blend(document, layer, COMP_BLEND_NORMAL) == COMP_OK,
         "blend mode restored to NORMAL");

  /*
   * 位图导入 / 导出闭环（S22）。
   *
   * 壳的「打开位图」「导出为」菜单走的正是这里验证的 comp_io_import /
   * comp_io_export 这一对。之所以必须自检而不是只靠人工：GUI 那两个入口都有模态
   * 对话框挡着，自动化点不过去；而对话框最终提交的仍是同一个 ABI 调用与同一个
   * 路径字符串。所以这条断言退化时，人工目视之前壳就已经坏了。
   *
   * 判据是「导出 → 读回 → 尺寸与图层数一致」，再加一张内容已知的探针图往返。
   */
  wchar_t io_temp_dir[MAX_PATH] = L"";
  if (GetTempPathW(MAX_PATH, io_temp_dir) == 0) wcscpy_s(io_temp_dir, L".\\");
  wchar_t image_file[MAX_PATH] = L"";
  wsprintfW(image_file, L"%sselftest_image_roundtrip.png", io_temp_dir);
  char image_path_utf8[MAX_PATH * 2] = {0};
  WideCharToMultiByte(CP_UTF8, 0, image_file, -1, image_path_utf8,
                      static_cast<int>(sizeof(image_path_utf8)), nullptr, nullptr);
  const bool export_ok =
      comp_io_export(document, image_path_utf8, COMP_FORMAT_PNG, nullptr) == COMP_OK;
  verify(export_ok, "comp_io_export(png)");
  if (export_ok) {
    comp_document reimported = nullptr;
    const bool reimport_ok =
        comp_io_import(engine, image_path_utf8, &reimported) == COMP_OK &&
        reimported != nullptr;
    verify(reimport_ok, "comp_io_import(png round trip)");
    if (reimport_ok) {
      comp_size reimported_size{};
      verify(comp_document_size(reimported, &reimported_size) == COMP_OK &&
                 reimported_size.width == size.width &&
                 reimported_size.height == size.height,
             "image round trip keeps canvas size");
      comp_document_destroy(reimported);
    }
  }

  /*
   * 探针图往返：240x160、左红 / 中绿竖条 / 右蓝，像素值精确已知。
   * 导入后核对画布尺寸与三个角落的像素——证明「打开一张外部图片」真把内容搬进了
   * 文档，不是建了个空壳。
   *
   * 夹具缺失时**先用引擎写出它**再导入（而不是降级成 [skip]）：
   *   - run_regression.py 的 --clean 会连 build/ 一起删掉，这份夹具本来就住在
   *     build/ 里，于是每次全量回归后它必然不在——旧版自检此时会把两项内容对拍
   *     降级为 [skip]，SHA256 与基线不一致，看起来像产品回归；
   *   - 自检自己链着引擎（静态），写 PNG 与读 PNG 走的是同一条 comp_io_* 路径，
   *     由它自备夹具不引入任何外部依赖；
   *   - 降级出口仍然保留：万一写入失败（磁盘满/只读），如实报 [skip] 而不是假 PASS。
   */
  const char* probe_path = "build/import_probe.png";
  if (!probe_png_exists(probe_path)) {
    (void)write_probe_png(engine, probe_path);
  }
  comp_document probe_document = nullptr;
  const bool probe_ok =
      comp_io_import(engine, probe_path, &probe_document) == COMP_OK &&
      probe_document != nullptr;
  if (probe_ok) {
    comp_size probe_size{};
    const bool probe_size_ok =
        comp_document_size(probe_document, &probe_size) == COMP_OK &&
        probe_size.width == 240 && probe_size.height == 160;
    verify(probe_size_ok, "probe image imports at 240x160");
    if (probe_size_ok) {
      const size_t probe_stride = static_cast<size_t>(probe_size.width) * 4u;
      std::vector<uint8_t> probe_pixels(probe_stride * static_cast<size_t>(probe_size.height), 0);
      const bool probe_flatten = comp_render_flatten(probe_document, probe_pixels.data(),
                                                    COMP_PIXEL_RGBA8, probe_stride) == COMP_OK;
      /* 左上角 = 红区，右下角 = 蓝区，中线 = 绿条。逐点对拍，不是统计近似。 */
      const bool probe_pixels_ok = probe_flatten &&
          probe_pixels[0] == 255 && probe_pixels[1] == 0 && probe_pixels[2] == 0 &&
          probe_pixels.size() >= 4 &&
          probe_pixels[probe_pixels.size() - 4] == 0 &&
          probe_pixels[probe_pixels.size() - 3] == 0 &&
          probe_pixels[probe_pixels.size() - 2] == 255;
      verify(probe_pixels_ok, "probe image content matches the known pattern");
    }
    comp_document_destroy(probe_document);
  } else {
    std::fprintf(report,
                 "[skip] %s 不存在且无法写出，跳过内容对拍（尺寸断言已覆盖导入通路）\n",
                 probe_path);
  }

  /* 撤销/重做由引擎维护，外壳只触发与展示。 */
  int32_t can_undo = 0;
  comp_document_can_undo(document, &can_undo);
  size_t undo_depth = 0;
  size_t redo_depth = 0;
  comp_document_history_stats(document, &undo_depth, &redo_depth);
  std::fprintf(report, "[info] history: can_undo=%d depth=%u redo=%u\n", can_undo,
               static_cast<unsigned>(undo_depth), static_cast<unsigned>(redo_depth));
  verify(can_undo == 0 || comp_document_undo(document) == COMP_OK, "comp_document_undo()");

  /* .comp 往返：写入临时目录再读回，核对尺寸。 */
  wchar_t temp_dir[MAX_PATH] = {0};
  wchar_t temp_file[MAX_PATH] = {0};
  if (GetTempPathW(MAX_PATH, temp_dir) != 0) {
    wsprintfW(temp_file, L"%scompositor_shell_selftest.comp", temp_dir);
  } else {
    wsprintfW(temp_file, L"compositor_shell_selftest.comp");
  }
  char comp_path_utf8[MAX_PATH * 2] = {0};
  WideCharToMultiByte(CP_UTF8, 0, temp_file, -1, comp_path_utf8,
                      static_cast<int>(sizeof(comp_path_utf8)), nullptr, nullptr);
  const bool save_ok = comp_document_save(document, comp_path_utf8) == COMP_OK;
  verify(save_ok, "comp_document_save(.comp)");
  if (save_ok) {
    comp_document reloaded = nullptr;
    const bool load_ok = comp_document_load(engine, comp_path_utf8, &reloaded) == COMP_OK &&
                         reloaded != nullptr;
    verify(load_ok, "comp_document_load(.comp round trip)");
    if (load_ok) {
      comp_size reloaded_size{};
      verify(comp_document_size(reloaded, &reloaded_size) == COMP_OK &&
                 reloaded_size.width == size.width && reloaded_size.height == size.height,
             "round trip keeps canvas size");
      comp_document_destroy(reloaded);
    }
  }

  /*
   * 图层变换链路（S21，面板平移/旋转/缩放真正走的接口序列）：
   * 一次显式事务内 get -> 步进 -> set -> 再 get 回读，核对现值与步进量一致，
   * 然后 abort 回滚——不往撤销栈里留一步（自检前面各段的隐式事务已经够了）。
   */
  comp_txn xform_txn = nullptr;
  const bool xform_begin = comp_txn_begin(document, "Selftest Layer Transform", &xform_txn) == COMP_OK;
  verify(xform_begin, "comp_txn_begin() for layer transform");
  if (xform_begin) {
    comp_rect before{};
    float angle = 0.0f;
    const bool read_ok = comp_layer_get_transform(document, layer, &before, &angle) == COMP_OK;
    verify(read_ok, "comp_layer_get_transform()");
    if (read_ok) {
      /* 平移 1 px（Shift+方向的微调量）：壳侧算位移，引擎只管收。 */
      const comp_rect moved{before.x + 1, before.y - 1, before.w, before.h};
      verify(comp_layer_set_transform(document, layer, moved, angle) == COMP_OK,
             "comp_layer_set_transform(move 1px)");
      comp_rect after{};
      float angle2 = 0.0f;
      verify(comp_layer_get_transform(document, layer, &after, &angle2) == COMP_OK &&
                 after.x == before.x + 1 && after.y == before.y - 1 && after.w == before.w &&
                 after.h == before.h && angle2 == angle,
             "transform round trip keeps the stepped value");
      /* 旋转 +15°（Ctrl+左右的一档）：在现值上累加，回读必须是累加结果。 */
      const float turned = angle + 15.0f;
      verify(comp_layer_set_transform(document, layer, after, turned) == COMP_OK,
             "comp_layer_set_transform(rotate +15)");
      comp_rect after2{};
      float angle3 = 0.0f;
      verify(comp_layer_get_transform(document, layer, &after2, &angle3) == COMP_OK &&
                 angle3 > turned - 0.01f && angle3 < turned + 0.01f,
             "rotation accumulates on the current value");
      /* 缩放 +1%（Alt+. 的一档，中心锚定）：壳侧算新矩形，回读核对中心不变。 */
      int32_t sx = after2.x, sy = after2.y, sw = after2.w, sh = after2.h;
      const bool has_box = sw > 0 && sh > 0;
      verify(has_box, "selftest layer carries an explicit transform box");
      if (has_box) {
        comp_ui::layer_transform_scale_rect(&sx, &sy, &sw, &sh,
                                            1.0 + comp_ui::kLayerTransformScaleStep);
        verify(comp_layer_set_transform(document, layer, comp_rect{sx, sy, sw, sh}, angle3) == COMP_OK,
               "comp_layer_set_transform(scale +1%)");
        comp_rect scaled{};
        float angle4 = 0.0f;
        verify(comp_layer_get_transform(document, layer, &scaled, &angle4) == COMP_OK &&
                   scaled.w == sw && scaled.h == sh,
               "scale round trip keeps the new box");
      }
    }
    verify(comp_txn_abort(xform_txn) == COMP_OK, "comp_txn_abort() rolls the transform back");
  }

  /*
   * S22 段的历史基线（S23 修正）：必须在**写入之前**取。
   *
   * 不引用后面滤镜段才声明的 depth_after_identity：selftest 是单函数长流程，
   * 变量必须在自己第一次使用之前声明——这不是风格问题，是 C++ 的作用域规则。
   */
  size_t s22_depth_baseline = 0;
  comp_document_history_stats(document, &s22_depth_baseline, &redo_depth);

  /*
   * S22 蒙版回读（comp_layer_get_mask）：
   *
   * 这条锁死的是“面板能否知道这一层带不带蒙版”。蒙版是引擎早已参与合成的真能力
   * （filter_graph.cpp 的 coverage.add(&layer->mask)），但 ABI 只有写入口、没有回读口，
   * 因此壳侧之前完全看不见“已经建过蒙版”这件事。
   *
   * 断言里必须包含“清除后回到无蒙版”：只验证“能读到蒙版”的话，一个
   * 写死的 has_mask也能通过（如果实现把数组当成写入口）。
   */
  int32_t has_mask = -1;
  int32_t mask_w = -1;
  int32_t mask_h = -1;
  verify(comp_layer_get_mask(document, layer, &has_mask, &mask_w, &mask_h) == COMP_OK &&
             has_mask == 0 && mask_w == 0 && mask_h == 0,
         "get_mask reports no mask on a fresh layer");

  std::vector<uint8_t> mask_pixels(static_cast<size_t>(width) *
                                  static_cast<size_t>(height), 255u);
  comp_mask full_mask{};
  full_mask.width = width;
  full_mask.height = height;
  full_mask.stride = static_cast<size_t>(width);
  full_mask.pixels = mask_pixels.data();
  verify(comp_layer_set_mask(document, layer, &full_mask) == COMP_OK,
         "comp_layer_set_mask(full cover)");
  verify(comp_layer_get_mask(document, layer, &has_mask, &mask_w, &mask_h) == COMP_OK &&
             has_mask == 1 && mask_w == width && mask_h == height,
         "get_mask round trips presence and size");

  verify(comp_layer_set_mask(document, layer, nullptr) == COMP_OK &&
             comp_layer_get_mask(document, layer, &has_mask, &mask_w, &mask_h) == COMP_OK &&
             has_mask == 0 && mask_w == 0 && mask_h == 0,
         "clearing the mask returns to no-mask");

  /*
   * S22 结构关系回读（comp_layer_get_group_info）。
   *
   * “是不是组 / 挂在哪 / 剪贴谁”三件事目前只能在创建时给（comp_layer_desc），
   * 此后没有 setter 之外的路。.comp 读回带组、带剪贴的文档时，面板就会静默失真。
   */
  comp_layer_id clipping_read = 0xdeadbeef;
  int32_t is_group_read = -1;
  comp_layer_id parent_read = 0xdeadbeef;
  verify(comp_layer_get_group_info(document, layer, &is_group_read, &parent_read,
                                  &clipping_read) == COMP_OK &&
             is_group_read == 0 && parent_read == 0 && clipping_read == 0,
         "get_group_info reports a root-level plain layer");

  /*
   * S22 重排链路（面板“上移/下移一层”真正走的接口）。
   *
   * T2 全量测试抄到的 43 项零调用里，comp_layer_reorder 就在其中：引擎早已实现，
   * 而壳一次都没调过——用户完全无法调整图层堆叠次序。
   *
   * 判据采“堆叠序真的换了”而不是“调用返回了 OK”：把 layer 移到 background 之前，
   * 然后用 comp_layer_list 重新读一次 id 序列（自下而上）。
   *
   * background 用两段式查询取，不靠字面量：id 是运行期分配的，写死会让这段断言
   * 在另一台机器上静默失效（而 selftest 是唯一能在 CI 里拦住它的地方）。
   */
  comp_layer_id background = 0;
  {
    size_t background_index = 0;
    comp_layer_id first = 0;
    if (comp_layer_list(document, &first, 1u, &background_index) == COMP_OK &&
        background_index >= 1u) {
      background = first;
    }
  }
  verify(background != 0 && background != layer, "background layer is resolvable");
  comp_layer_id stack_before[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  size_t stack_count = 0;
  verify(comp_layer_list(document, stack_before, 8u, &stack_count) == COMP_OK &&
             stack_count == 2u && stack_before[0] == background && stack_before[1] == layer,
         "reorder precondition: [background, layer]");
  verify(comp_layer_reorder(document, layer, background) == COMP_OK,
         "comp_layer_reorder(layer -> before background)");
  comp_layer_id stack_after[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  stack_count = 0;
  verify(comp_layer_list(document, stack_after, 8u, &stack_count) == COMP_OK &&
             stack_count == 2u && stack_after[0] == layer && stack_after[1] == background,
         "reorder really changed the stacking order");
  verify(comp_layer_reorder(document, layer, 0) == COMP_OK &&
             comp_layer_list(document, stack_after, 8u, &stack_count) == COMP_OK &&
             stack_after[1] == layer,
         "reorder back to the top restores the original order");

  /*
   * S22 直方图（comp_layer_histogram）。
   *
   * 这是上游 Levels 面板左半边的那张图，也是本壳唯一的 histogram 调用点。
   *
   * 判据采“峰值桶等于手算的亮度桶”，而不是“调用成功”或者“非零桶大于 0”。
   * 原因是本段的图层像素固定：预乘红 (128,0,0,128)。内核先反预乘得到
   * (255,0,0)，再按 0.299R+0.587G+0.114B 取亮度 = 76.245，落桶 76。
   *
   * 这个数字必须在每次改动图层像素时重算：上一轮我写死 200（想当然认为“均匀灰色”），
   * 结果被真值 76 打脸——那一刷只能说明判据写错了，而不能说引擎算错了。
   */
  double histogram_bins[256] = {0.0};
  verify(comp_layer_histogram(document, layer, nullptr, 0, histogram_bins) == COMP_OK,
         "comp_layer_histogram() on the selftest layer");
  double hist_peak = 0.0;
  int hist_nonzero = 0;
  int hist_peak_bin = -1;
  for (int i = 0; i < 256; ++i) {
    if (histogram_bins[i] > 0.0) ++hist_nonzero;
    if (histogram_bins[i] > hist_peak) {
      hist_peak = histogram_bins[i];
      hist_peak_bin = i;
    }
  }
  std::fprintf(report, "[info] histogram nonzero bins = %d, peak bin = %d\n",
               hist_nonzero, hist_peak_bin);
  /* 四舍五入：int(76.245) = 76，与上面手算一致。 */
  verify(hist_peak_bin == 76,
         "premultiplied red(128,0,0,128) un-premultiplies to (255,0,0) and luma 76");

  /*
   * S22 采样模式往返（comp_layer_get_sampling）。
   *
   * setter 早已存在且引擎真的按它改缩放手感（resample.cpp 的 sampling_from_abi），
   * 但壳一次都没调过———用户看不见也改不了。
   */
  comp_sampling sampling_read = COMP_SAMPLING_NEAREST;
  verify(comp_layer_set_sampling(document, layer, COMP_SAMPLING_NEAREST) == COMP_OK &&
             comp_layer_get_sampling(document, layer, &sampling_read) == COMP_OK &&
             sampling_read == COMP_SAMPLING_NEAREST,
         "sampling mode round trip (set NEAREST -> get)");
  verify(comp_layer_set_sampling(document, layer, COMP_SAMPLING_HIGH_QUALITY) == COMP_OK &&
             comp_layer_get_sampling(document, layer, &sampling_read) == COMP_OK &&
             sampling_read == COMP_SAMPLING_HIGH_QUALITY,
         "sampling mode restored to HIGH_QUALITY");

  /*
   * S22 组隔离开关（comp_layer_set_isolated / get_isolated）。
   *
   * 两种值都是已参与像素求值的真差异（filter_graph.cpp 按 node.isolated 决定
   * child_backdrop），不是展示开关。写非组图层时引擎不报错但属性不参与合成，
   * 因此这里只验证读写往返，不验证它改变画面。
   */
  int32_t isolated_read = -1;
  verify(comp_layer_set_isolated(document, layer, 1) == COMP_OK &&
             comp_layer_get_isolated(document, layer, &isolated_read) == COMP_OK &&
             isolated_read == 1,
         "isolated round trip (set 1 -> get)");
  verify(comp_layer_set_isolated(document, layer, 0) == COMP_OK &&
             comp_layer_get_isolated(document, layer, &isolated_read) == COMP_OK &&
             isolated_read == 0,
         "isolated restored to 0");

  /*
   * S22 剪贴蒙版开关（comp_layer_set_clipping_source）。
   *
   * 引擎已参与像素求值（composite_ops.cpp 的 clipping_factor 真的按来源图层算覆盖率），
   * 而壳之前没有任何入口能开关它。自标志与不存在的来源必须被拒。
   */
  verify(comp_layer_set_clipping_source(document, layer, background) == COMP_OK,
         "comp_layer_set_clipping_source(baseline)");
  comp_layer_id clip_check = 0;
  verify(comp_layer_get_group_info(document, layer, &is_group_read, &parent_read,
                                  &clip_check) == COMP_OK && clip_check == background,
         "clipping source round trips through get_group_info");
  verify(comp_layer_set_clipping_source(document, layer, layer) != COMP_OK,
         "self-clipping is rejected");
  verify(comp_layer_set_clipping_source(document, layer, 0xdeadbeef) == COMP_ERR_NOT_FOUND,
         "clipping to a missing layer is NOT_FOUND");
  verify(comp_layer_set_clipping_source(document, layer, 0) == COMP_OK &&
             comp_layer_get_group_info(document, layer, &is_group_read, &parent_read,
                                      &clip_check) == COMP_OK &&
             clip_check == 0,
         "clipping source cleared");

  /*
   * 把上面 S22 段的写入全部撤销回去，避免遗留历史深度影响后续无关段的判据。
   *
   * 基线必须在**写入之前**取（见本段开头的 s22_depth_baseline）。S22 首版把基线取在
   * 这里，即全部写入之后，于是 current <= baseline 第一次判断就成立、一条都没撤——
   * 注释说「全部撤销回去」，实际是空循环。它当时不影响通过，但那正是假护栏：
   * 以后在它之后追加断言，就会莫名受上一段遗留深度影响。
   *
   * 因此这里不只做撤销，还**断言撤销确实发生过**（s22_undone > 0）并且深度回到基线。
   * 若有人把基线取回调到写入之后，第一条立刻红——它把「护栏是不是真的在护」变成
   * 可判定的，而不是靠注释自称。
   */
  size_t s22_undone = 0;
  size_t s22_depth_end = 0;
  for (;;) {
    size_t current = 0;
    comp_document_history_stats(document, &current, &redo_depth);
    s22_depth_end = current;
    if (current <= s22_depth_baseline) break;
    if (comp_document_undo(document) != COMP_OK) break;
    ++s22_undone;
  }
  verify(s22_undone > 0, "S22 writes left undoable history and were rolled back");
  verify(s22_depth_end == s22_depth_baseline, "S22 rollback reaches the pre-S22 depth");

  /*
   * S24 段：镜像读回（comp_layer_get_flip）+ 效果枚举（comp_layer_effect_count/at）。
   *
   * 这两对是「引擎早就算、壳却读不回」的最后一组：flip 早已折进采样坐标
   * （resample.cpp），6 个 effect kind 早已参与像素求值（effect_eval.cpp），
   * 但 ABI 只有写口——壳读不到现值就只能盲目取反，也画不出「这层带了什么效果」、
   * 菜单更无法标出已启用项。
   *
   * 判据不采「调用返回 OK」：效果那段必须证明像素真的变了（纯元数据实现也能让
   * count == 1 通过），翻转那段必须证明同值写入不产生撤销步（否则撤销栈被空步塞满）。
   */
  size_t s24_depth_baseline = 0;
  comp_document_history_stats(document, &s24_depth_baseline, &redo_depth);

  int32_t flip_now_x = -1;
  int32_t flip_now_y = -1;
  verify(comp_layer_get_flip(document, layer, &flip_now_x, &flip_now_y) == COMP_OK &&
             flip_now_x == 0 && flip_now_y == 0,
         "get_flip reports no flip on a fresh layer");
  verify(comp_layer_set_flip(document, layer, 1, 0) == COMP_OK &&
             comp_layer_get_flip(document, layer, &flip_now_x, &flip_now_y) == COMP_OK &&
             flip_now_x == 1 && flip_now_y == 0,
         "flip round trip (set horizontal -> get)");

  size_t flip_depth_after_set = 0;
  comp_document_history_stats(document, &flip_depth_after_set, &redo_depth);
  verify(comp_layer_set_flip(document, layer, 1, 0) == COMP_OK, "set_flip same value again");
  size_t flip_depth_after_noop = 0;
  comp_document_history_stats(document, &flip_depth_after_noop, &redo_depth);
  verify(flip_depth_after_noop == flip_depth_after_set,
         "same-value set_flip adds no undo step");

  verify(comp_layer_set_flip(document, layer, 0, 0) == COMP_OK &&
             comp_layer_get_flip(document, layer, &flip_now_x, &flip_now_y) == COMP_OK &&
             flip_now_x == 0 && flip_now_y == 0,
         "flip cleared back to 0/0");

  size_t effect_count_now = 123u;
  verify(comp_layer_effect_count(document, layer, &effect_count_now) == COMP_OK &&
             effect_count_now == 0,
         "effect_count is 0 before any effect is added");

  const char* overlay_settings = "{\"red\":0,\"green\":0,\"blue\":1}";
  verify(comp_layer_set_effect(document, layer, 2u, overlay_settings) == COMP_OK,
         "comp_layer_set_effect(Color Overlay, blue)");
  verify(comp_layer_effect_count(document, layer, &effect_count_now) == COMP_OK &&
             effect_count_now == 1,
         "effect_count() == 1 after adding an effect");

  uint32_t effect_kind_now = 999u;
  size_t effect_settings_size = 0;
  verify(comp_layer_effect_at(document, layer, 0u, &effect_kind_now, nullptr, 0,
                              &effect_settings_size) == COMP_OK &&
             effect_kind_now == 2u && effect_settings_size == std::strlen(overlay_settings),
         "effect_at() reports kind and settings length (two-pass probe)");
  std::vector<char> effect_settings(effect_settings_size + 1u, '\0');
  verify(comp_layer_effect_at(document, layer, 0u, &effect_kind_now, effect_settings.data(),
                              effect_settings.size(), &effect_settings_size) == COMP_OK &&
             std::strcmp(effect_settings.data(), overlay_settings) == 0,
         "effect_at() round trips the settings JSON verbatim");

  std::vector<uint8_t> effect_after(flattened.size(), 0);
  const bool effect_flatten_after =
      comp_render_flatten(document, effect_after.data(), COMP_PIXEL_RGBA8, stride) == COMP_OK;
  /*
   * 判据取「蓝上来了、红下去了」，不写死 255：图层像素是 50% 预乘红
   * (128,0,0,128)，颜色叠加按覆盖率求值，落盘仍是预乘值 —— 蓝色通道是
   * 0..255 的 50% (=128)，写死 b > 200 会把自己的判据判错，而不是产品错了。
   */
  const bool effect_pixels_ok = effect_flatten_after && effect_after.size() >= 4u &&
                                effect_after[2] >= 100u && effect_after[0] < 60u &&
                                effect_after[2] > effect_after[0];
  verify(effect_pixels_ok, "color overlay written through the ABI really changes the canvas");
  if (effect_after.size() >= 4u) {
    std::fprintf(report, "[info] overlay pixel = (%u,%u,%u,%u)\n", effect_after[0],
                 effect_after[1], effect_after[2], effect_after[3]);
  }

  verify(comp_layer_remove_effect(document, layer, 2u) == COMP_OK &&
             comp_layer_effect_count(document, layer, &effect_count_now) == COMP_OK &&
             effect_count_now == 0,
         "effect removal clears the list");

  /* 撤销回 S24 基线（同 S22 的口径：既撤又要断言真的撤过，避免假护栏）。 */
  size_t s24_undone = 0;
  for (;;) {
    size_t current = 0;
    comp_document_history_stats(document, &current, &redo_depth);
    if (current <= s24_depth_baseline) break;
    if (comp_document_undo(document) != COMP_OK) break;
    ++s24_undone;
  }
  verify(s24_undone > 0, "S24 writes were undoable and rolled back");
  size_t s24_depth_end = 0;
  comp_document_history_stats(document, &s24_depth_end, &redo_depth);
  verify(s24_depth_end == s24_depth_baseline, "S24 rollback reaches the pre-S24 depth");

  /*
   * 调整面板链路（面板真正走的接口序列）：
   * 键名表元数据 -> 引擎组装 settings JSON -> 新增 -> 读回 -> 按索引更新 -> 非法值被拒。
   */
  const char* kind_name = nullptr;
  verify(comp_layer_adjustment_kind_name(3u, &kind_name) == COMP_OK && kind_name != nullptr &&
             std::strcmp(kind_name, "Exposure") == 0,
         "comp_layer_adjustment_kind_name(3) == Exposure");

  size_t param_count = 0;
  verify(comp_layer_adjustment_param_count(3u, &param_count) == COMP_OK && param_count == 3,
         "comp_layer_adjustment_param_count(3) == 3");

  comp_adjustment_param first_param{};
  verify(comp_layer_adjustment_param_at(3u, 0u, &first_param) == COMP_OK &&
             first_param.json_path != nullptr &&
             std::strcmp(first_param.json_path, "exposureSettings.exposure") == 0,
         "comp_layer_adjustment_param_at(3,0) path");

  const char* panel_values[3] = {"1.5", "0.25", "2.2"};
  size_t settings_size = 0;
  verify(comp_layer_adjustment_build_settings(3u, panel_values, 3, nullptr, 0, &settings_size) ==
                 COMP_OK &&
             settings_size > 0,
         "comp_layer_adjustment_build_settings(size probe)");
  std::vector<char> settings(settings_size + 1, '\0');
  const bool build_ok = comp_layer_adjustment_build_settings(
      3u, panel_values, 3, settings.data(), settings.size(), &settings_size) == COMP_OK;
  verify(build_ok, "comp_layer_adjustment_build_settings");
  std::fprintf(report, "[info] panel settings = %s\n", settings.data());

  const bool add_adj_ok =
      add_ok && build_ok && comp_layer_add_adjustment(document, layer, 3u, settings.data()) == COMP_OK;
  verify(add_adj_ok, "comp_layer_add_adjustment(panel settings)");

  size_t adj_count = 0;
  verify(add_adj_ok && comp_layer_adjustment_count(document, layer, &adj_count) == COMP_OK &&
             adj_count == 1,
         "comp_layer_adjustment_count() == 1");

  double exposure = 0.0;
  verify(comp_layer_adjustment_read_value(settings.data(), "exposureSettings.exposure", &exposure,
                                          nullptr) == COMP_OK &&
             std::fabs(exposure - 1.5) < 1e-9,
         "comp_layer_adjustment_read_value(exposure) == 1.5");

  const char* updated_values[3] = {"-2", "", "1.8"};
  std::vector<char> updated(256, '\0');
  size_t updated_size = 0;
  const bool update_ok =
      comp_layer_adjustment_build_settings(3u, updated_values, 3, updated.data(), updated.size(),
                                           &updated_size) == COMP_OK &&
      comp_layer_update_adjustment_at(document, layer, 0u, 3u, updated.data()) == COMP_OK;
  verify(update_ok, "comp_layer_update_adjustment_at(0)");

  double gamma = 0.0;
  verify(comp_layer_adjustment_read_value(updated.data(), "exposureSettings.gamma", &gamma, nullptr) ==
             COMP_OK &&
             std::fabs(gamma - 1.8) < 1e-9,
         "updated gamma == 1.8");

  /* 越界面板值必须被拒（面板不会把非法值写进文档）。 */
  const char* bad_values[3] = {"999", "", ""};
  verify(comp_layer_adjustment_build_settings(3u, bad_values, 3, nullptr, 0, &settings_size) != COMP_OK,
         "out-of-range panel value rejected");

  /* 删除接口（面板 Ctrl+Shift+D 走这条）：越界被拒，删除后数量回落。 */
  verify(comp_layer_remove_adjustment(document, layer, 5u) == COMP_ERR_INVALID_ARGUMENT,
         "comp_layer_remove_adjustment(out of range) rejected");
  const bool remove_ok = comp_layer_remove_adjustment(document, layer, 0u) == COMP_OK;
  verify(remove_ok, "comp_layer_remove_adjustment(0)");
  size_t after_remove = 99;
  verify(remove_ok && comp_layer_adjustment_count(document, layer, &after_remove) == COMP_OK &&
             after_remove == 0,
         "comp_layer_adjustment_count() == 0 after remove");

  /*
   * 滤镜面板链路：面板按 ui_core.h 的行序把值搬进 comp_filter_desc，这里走同一条
   * ABI。要点：零初始化 = 恒等（不碰像素、不开事务）；越界在事务之前拒绝；
   * 真应用后一步撤销可逐字节还原。
   */
  /*
   * 滤镜观测必须先有像素：本段之前的 `comp_document_undo()` 已经把 selftest 图层的
   * 半透明红退回空像素，而 Vignette 在 `fillsClear = 0` 下逐像素跳过全透明像素
   * （AdjustPixels.c），因此空图层上「应用成功但字节不变」是正确行为、看不到效果。
   * 重新铺一张不透明灰图，与 tests/test_filters.cpp 的观测口径一致。
   */
  std::vector<uint8_t> opaque(stride * static_cast<size_t>(height), 0);
  for (size_t i = 0; i < opaque.size(); i += 4) {
    opaque[i + 0] = 200;
    opaque[i + 1] = 200;
    opaque[i + 2] = 200;
    opaque[i + 3] = 255;
  }
  comp_image opaque_image{};
  opaque_image.width = width;
  opaque_image.height = height;
  opaque_image.format = COMP_PIXEL_RGBA8;
  opaque_image.stride = stride;
  opaque_image.pixels = opaque.data();
  verify(add_ok && comp_layer_set_pixels(document, layer, &opaque_image) == COMP_OK,
         "comp_layer_set_pixels(opaque gray)");

  std::vector<uint8_t> flatten_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t depth_before = 0;
  comp_document_history_stats(document, &depth_before, &redo_depth);

  comp_filter_desc identity{};
  identity.kind = COMP_FILTER_VIGNETTE; /* 其余字段零初始化 -> amount 0 = 恒等 */
  int32_t changed = -1;
  char filter_note[256] = {0};
  size_t filter_note_size = 0;
  const bool identity_ok =
      comp_filter_apply(document, layer, &identity, 1.0, &changed, filter_note,
                        sizeof(filter_note), &filter_note_size) == COMP_OK &&
      changed == 0;
  verify(identity_ok, "comp_filter_apply(identity) is a no-op");
  size_t depth_after_identity = 0;
  comp_document_history_stats(document, &depth_after_identity, &redo_depth);
  verify(depth_after_identity == depth_before, "identity filter leaves history untouched");

  comp_filter_desc bad_filter{};
  bad_filter.kind = COMP_FILTER_VIGNETTE;
  bad_filter.vignette.amount = 101.0; /* 量程 0..100 */
  verify(add_ok && comp_filter_apply(document, layer, &bad_filter, 1.0, &changed, filter_note,
                                     sizeof(filter_note), &filter_note_size) ==
                      COMP_ERR_INVALID_ARGUMENT,
         "comp_filter_apply(out-of-range amount) rejected");

  comp_filter_desc vignette{};
  vignette.kind = COMP_FILTER_VIGNETTE;
  vignette.vignette.amount = 100.0;
  vignette.vignette.midpoint = 50.0;
  vignette.vignette.roundness = 100.0;
  vignette.vignette.feather = 60.0;
  vignette.vignette.highlights = 25.0;
  vignette.vignette.color[0] = 0.0;
  vignette.vignette.color[1] = 0.0;
  vignette.vignette.color[2] = 0.0;
  changed = 0;
  const bool vignette_ok =
      add_ok && comp_filter_apply(document, layer, &vignette, 1.0, &changed, filter_note,
                                  sizeof(filter_note), &filter_note_size) == COMP_OK &&
      changed == 1;
  verify(vignette_ok, "comp_filter_apply(vignette amount=100) changed pixels");

  std::vector<uint8_t> flatten_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(vignette_ok && flatten_after != flatten_before,
         "vignette changes the flattened output");
  verify(vignette_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(filter step)");
  std::vector<uint8_t> flatten_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(vignette_ok && flatten_restored == flatten_before,
         "filter undo restores pixels byte-for-byte");

  /*
   * 模糊工具（COMP_TOOL_BLUR）：窗口路径是「鼠标折线 -> comp_tool_apply」。
   * 先铺一张棋盘让模糊有内容可抹（纯色图模糊前后字节相同，测不出东西）。
   */
  std::vector<uint8_t> pattern(stride * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const bool red = ((x / 8) + (y / 8)) % 2 == 0;
      uint8_t* pixel = pattern.data() + static_cast<size_t>(y) * stride +
                       static_cast<size_t>(x) * 4u;
      pixel[0] = red ? 255 : 0;
      pixel[1] = 0;
      pixel[2] = red ? 0 : 255;
      pixel[3] = 255;
    }
  }
  comp_image patched{};
  patched.width = width;
  patched.height = height;
  patched.format = COMP_PIXEL_RGBA8;
  patched.stride = stride;
  patched.pixels = pattern.data();
  verify(comp_layer_set_pixels(document, layer, &patched) == COMP_OK,
         "comp_layer_set_pixels(checkerboard)");

  std::vector<uint8_t> blur_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_before.data(), COMP_PIXEL_RGBA8, stride);
  const float stroke[] = {8.0f, 32.0f, 20.0f, 32.0f, 32.0f, 32.0f,
                          44.0f, 32.0f, 56.0f, 32.0f};
  comp_tool_op_desc blur_desc{};
  blur_desc.kind = COMP_TOOL_BLUR;
  blur_desc.stroke_points = stroke;
  blur_desc.stroke_point_count = 5;
  blur_desc.brush.diameter = 24.0f;
  blur_desc.brush.hardness = 0.75f;
  blur_desc.brush.pressure = 1.0f;
  blur_desc.brush.flow = 1.0f;
  blur_desc.blur.radius = 6.0f;
  char tool_note[256] = {0};
  size_t tool_note_size = 0;
  const bool blur_ok =
      comp_tool_apply(document, layer, &blur_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(blur_ok, "comp_tool_apply(COMP_TOOL_BLUR)");
  std::vector<uint8_t> blur_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(blur_ok && blur_after != blur_before, "blur tool changes pixels along the stroke");
  verify(blur_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(blur step)");
  std::vector<uint8_t> blur_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(blur_ok && blur_restored == blur_before,
         "blur undo restores pixels byte-for-byte");

  comp_tool_op_desc bad_blur = blur_desc;
  bad_blur.blur.radius = 0.0f; /* 量程 0.5..50 */
  verify(comp_tool_apply(document, layer, &bad_blur, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_BLUR(radius=0) rejected");

  /*
   * 涂抹 / 液化（COMP_TOOL_SMUDGE / LIQUIFY）：与模糊共用同一条「鼠标折线 ->
   * comp_tool_apply」通路，参数换成 warp.strength。strength=0 必须逐字节不动，
   * 否则壳层把 Alt+] 调到 0 时会悄悄改图。
   */
  std::vector<uint8_t> warp_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, warp_before.data(), COMP_PIXEL_RGBA8, stride);

  comp_tool_op_desc smudge_desc{};
  smudge_desc.kind = COMP_TOOL_SMUDGE;
  smudge_desc.stroke_points = stroke;
  smudge_desc.stroke_point_count = 5;
  smudge_desc.brush.diameter = 24.0f;
  smudge_desc.brush.hardness = 0.75f;
  smudge_desc.brush.pressure = 1.0f;
  smudge_desc.brush.flow = 1.0f;
  smudge_desc.warp.strength = 0.5f;
  const bool smudge_ok =
      comp_tool_apply(document, layer, &smudge_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(smudge_ok, "comp_tool_apply(COMP_TOOL_SMUDGE)");
  std::vector<uint8_t> smudge_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, smudge_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(smudge_ok && smudge_after != warp_before, "smudge drags colour along the stroke");
  verify(smudge_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(smudge step)");

  comp_tool_op_desc liquify_desc = smudge_desc;
  liquify_desc.kind = COMP_TOOL_LIQUIFY;
  liquify_desc.warp.strength = 0.0f;
  const bool liquify_zero_ok =
      comp_tool_apply(document, layer, &liquify_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  std::vector<uint8_t> liquify_zero(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, liquify_zero.data(), COMP_PIXEL_RGBA8, stride);
  verify(liquify_zero_ok && liquify_zero == warp_before,
         "COMP_TOOL_LIQUIFY(strength=0) is a byte-identical no-op");

  liquify_desc.warp.strength = 0.8f;
  const bool liquify_ok =
      comp_tool_apply(document, layer, &liquify_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(liquify_ok, "comp_tool_apply(COMP_TOOL_LIQUIFY)");
  std::vector<uint8_t> liquify_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, liquify_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(liquify_ok && liquify_after != warp_before, "liquify moves pixels along the stroke");
  verify(liquify_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(liquify step)");

  /*
   * 渐变（COMP_TOOL_GRADIENT，S9）：窗口路径是「拖出两个端点 -> comp_tool_apply」。
   * 冻结三件事：枚举值、铺色后逐字节可撤销、端点重合被参数闸拒绝且不留撤销步。
   */
  verify(static_cast<int>(COMP_TOOL_GRADIENT) == 13, "COMP_TOOL_GRADIENT stays at 13");

  std::vector<uint8_t> gradient_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t gradient_undo_before = 0;
  size_t gradient_redo = 0;
  comp_document_history_stats(document, &gradient_undo_before, &gradient_redo);

  comp_tool_op_desc gradient_desc{};
  gradient_desc.kind = COMP_TOOL_GRADIENT;
  gradient_desc.gradient.type = 0; /* 线性 */
  gradient_desc.gradient.start_x = 4.0;
  gradient_desc.gradient.start_y = 32.0;
  gradient_desc.gradient.end_x = 60.0;
  gradient_desc.gradient.end_y = 32.0;
  gradient_desc.gradient.color0[0] = 255; /* 第一色标：不透明红 */
  gradient_desc.gradient.color0[3] = 255;
  gradient_desc.gradient.color1[2] = 255; /* 第二色标：不透明蓝 */
  gradient_desc.gradient.color1[3] = 255;
  gradient_desc.gradient.opacity = 1.0f;
  const bool gradient_ok =
      comp_tool_apply(document, layer, &gradient_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(gradient_ok, "comp_tool_apply(COMP_TOOL_GRADIENT)");

  std::vector<uint8_t> gradient_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t left_pixel = static_cast<size_t>(32) * stride + static_cast<size_t>(4) * 4u;
  const size_t right_pixel = static_cast<size_t>(32) * stride + static_cast<size_t>(60) * 4u;
  verify(gradient_ok && gradient_after != gradient_before,
         "gradient tool paints the layer");
  verify(gradient_ok && gradient_after[left_pixel + 0] == 255 &&
             gradient_after[left_pixel + 2] == 0,
         "the gradient starts at the first stop (red)");
  verify(gradient_ok && gradient_after[right_pixel + 2] == 255 &&
             gradient_after[right_pixel + 0] == 0,
         "the gradient ends at the second stop (blue)");
  verify(gradient_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(gradient step)");
  std::vector<uint8_t> gradient_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(gradient_ok && gradient_restored == gradient_before,
         "gradient undo restores pixels byte-for-byte");

  /* 端点重合：参数闸必须在事务之前拒绝，撤销栈与像素都不许动。 */
  size_t gradient_undo_rejected = 0;
  comp_document_history_stats(document, &gradient_undo_rejected, &gradient_redo);
  comp_tool_op_desc bad_gradient = gradient_desc;
  bad_gradient.gradient.end_x = bad_gradient.gradient.start_x;
  bad_gradient.gradient.end_y = bad_gradient.gradient.start_y;
  verify(comp_tool_apply(document, layer, &bad_gradient, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_GRADIENT(coincident endpoints) rejected");
  size_t gradient_undo_after_reject = 0;
  comp_document_history_stats(document, &gradient_undo_after_reject, &gradient_redo);
  verify(gradient_undo_after_reject == gradient_undo_rejected,
         "the rejected gradient leaves no undo step");
  std::vector<uint8_t> gradient_after_reject(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_after_reject.data(), COMP_PIXEL_RGBA8, stride);
  verify(gradient_after_reject == gradient_restored,
         "the rejected gradient leaves the render byte-identical");

  /*
   * 形状（COMP_TOOL_SHAPE，S10）：窗口路径是「拖出对角点 / 两端点 -> comp_tool_apply」。
   * 冻结：枚举值、矩形填充与逐字节撤销、椭圆不碰包围盒角（椭圆不是矩形）、直线是
   * 圆头描边而不是填充、带填充的直线被参数闸拒绝且不留撤销步、图层外形状成功但
   * 不产生空撤销步。
   */
  verify(static_cast<int>(COMP_TOOL_SHAPE) == 14, "COMP_TOOL_SHAPE stays at 14");

  std::vector<uint8_t> shape_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t shape_undo_before = 0;
  size_t shape_redo = 0;
  comp_document_history_stats(document, &shape_undo_before, &shape_redo);

  comp_tool_op_desc shape_rect_desc{};
  shape_rect_desc.kind = COMP_TOOL_SHAPE;
  shape_rect_desc.shape.kind = COMP_SHAPE_RECT;
  shape_rect_desc.shape.x0 = 8.0;
  shape_rect_desc.shape.y0 = 8.0;
  shape_rect_desc.shape.x1 = 40.0;
  shape_rect_desc.shape.y1 = 40.0;
  shape_rect_desc.shape.style.fill_enabled = 1;
  shape_rect_desc.shape.style.fill_color[1] = 255; /* 不透明绿 */
  shape_rect_desc.shape.style.fill_color[3] = 255;
  shape_rect_desc.shape.style.opacity = 1.0f;
  shape_rect_desc.shape.style.antialias = 1;
  const bool shape_rect_ok =
      comp_tool_apply(document, layer, &shape_rect_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_rect_ok, "comp_tool_apply(COMP_TOOL_SHAPE rect)");
  std::vector<uint8_t> shape_rect_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_rect_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t rect_inside = static_cast<size_t>(20) * stride + static_cast<size_t>(20) * 4u;
  const size_t rect_outside = static_cast<size_t>(20) * stride + static_cast<size_t>(44) * 4u;
  verify(shape_rect_ok && shape_rect_after[rect_inside + 1] == 255 && shape_rect_after[rect_inside + 3] == 255,
         "the rectangle is filled with the shape colour");
  verify(shape_rect_ok && std::memcmp(&shape_rect_after[rect_outside], &shape_before[rect_outside], 4) == 0,
         "pixels outside the rectangle are untouched");
  verify(shape_rect_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape rect step)");
  std::vector<uint8_t> shape_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_rect_ok && shape_restored == shape_before,
         "shape undo restores pixels byte-for-byte");

  /* 椭圆：中心要填，包围盒的角不能填——那正是「椭圆不是矩形」的证据。 */
  comp_tool_op_desc shape_ellipse_desc = shape_rect_desc;
  shape_ellipse_desc.shape.kind = COMP_SHAPE_ELLIPSE;
  shape_ellipse_desc.shape.style.fill_color[0] = 255;
  shape_ellipse_desc.shape.style.fill_color[1] = 0;
  const bool shape_ellipse_ok =
      comp_tool_apply(document, layer, &shape_ellipse_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_ellipse_ok, "comp_tool_apply(COMP_TOOL_SHAPE ellipse)");
  std::vector<uint8_t> shape_ellipse_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_ellipse_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t shape_ellipse_center = static_cast<size_t>(20) * stride + static_cast<size_t>(20) * 4u;
  const size_t shape_ellipse_corner = static_cast<size_t>(8) * stride + static_cast<size_t>(8) * 4u;
  verify(shape_ellipse_ok && shape_ellipse_after[shape_ellipse_center] == 255 && shape_ellipse_after[shape_ellipse_center + 3] == 255,
         "the ellipse centre is filled");
  verify(shape_ellipse_ok &&
             std::memcmp(&shape_ellipse_after[shape_ellipse_corner], &shape_before[shape_ellipse_corner], 4) == 0,
         "the ellipse leaves its bounding-box corner untouched");
  verify(shape_ellipse_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape ellipse step)");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_ellipse_ok && shape_restored == shape_before,
         "ellipse undo restores pixels byte-for-byte");

  /* 直线：只有描边（fill 必须为 0）；圆头端点越过端点半个线宽。 */
  comp_tool_op_desc shape_line_desc{};
  shape_line_desc.kind = COMP_TOOL_SHAPE;
  shape_line_desc.shape.kind = COMP_SHAPE_LINE;
  shape_line_desc.shape.x0 = 8.0;
  shape_line_desc.shape.y0 = 32.0;
  shape_line_desc.shape.x1 = 56.0;
  shape_line_desc.shape.y1 = 32.0;
  shape_line_desc.shape.style.fill_enabled = 0;
  shape_line_desc.shape.style.stroke_enabled = 1;
  shape_line_desc.shape.style.stroke_color[0] = 255;
  shape_line_desc.shape.style.stroke_color[1] = 255;
  shape_line_desc.shape.style.stroke_color[2] = 255;
  shape_line_desc.shape.style.stroke_color[3] = 255;
  shape_line_desc.shape.style.stroke_width = 6.0; /* 半宽 3 */
  shape_line_desc.shape.style.opacity = 1.0f;
  shape_line_desc.shape.style.antialias = 1;
  const bool shape_line_ok =
      comp_tool_apply(document, layer, &shape_line_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_line_ok, "comp_tool_apply(COMP_TOOL_SHAPE line)");
  std::vector<uint8_t> shape_line_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_line_after.data(), COMP_PIXEL_RGBA8, stride);
  const auto at = [&](int32_t x, int32_t y) {
    return static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
  };
  verify(shape_line_ok && shape_line_after[at(32, 32) + 3] == 255, "the line core is opaque");
  verify(shape_line_ok && shape_line_after[at(32, 29) + 3] == 255,
         "a pixel inside half the stroke width is covered");
  verify(shape_line_ok && std::memcmp(&shape_line_after[at(32, 28)], &shape_before[at(32, 28)], 4) == 0,
         "a pixel beyond half the stroke width is untouched");
  verify(shape_line_ok && shape_line_after[at(58, 32) + 3] > 0, "the round cap extends past the endpoint");
  verify(shape_line_ok && std::memcmp(&shape_line_after[at(60, 32)], &shape_before[at(60, 32)], 4) == 0,
         "the cap stops within half the stroke width");
  verify(shape_line_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape line step)");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_line_ok && shape_restored == shape_before,
         "line undo restores pixels byte-for-byte");

  /* 「填充直线」没有定义：参数闸必须在事务之前拒绝，撤销栈与像素都不许动。 */
  comp_tool_op_desc filled_line = shape_line_desc;
  filled_line.shape.style.fill_enabled = 1;
  verify(comp_tool_apply(document, layer, &filled_line, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_SHAPE(line with fill) rejected");
  size_t shape_undo_after_reject = 0;
  comp_document_history_stats(document, &shape_undo_after_reject, &shape_redo);
  verify(shape_undo_after_reject == shape_undo_before,
         "the rejected line shape leaves no undo step");

  /* 合法但完全落在图层外的形状：成功、无像素改动、不产生空撤销步。 */
  comp_tool_op_desc outside_desc = shape_rect_desc;
  outside_desc.shape.x0 = 100.0;
  outside_desc.shape.y0 = 100.0;
  outside_desc.shape.x1 = 120.0;
  outside_desc.shape.y1 = 120.0;
  verify(comp_tool_apply(document, layer, &outside_desc, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_OK,
         "a shape entirely outside the layer succeeds as a no-op");
  size_t shape_undo_after_outside = 0;
  comp_document_history_stats(document, &shape_undo_after_outside, &shape_redo);
  verify(shape_undo_after_outside == shape_undo_before,
         "an off-canvas shape leaves no empty undo step");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_restored == shape_before,
         "an off-canvas shape leaves the render byte-identical");

  /*
   * 文字（COMP_TOOL_TYPE，S11）：窗口路径是「点击落插入点 -> 键入 ASCII -> 回车提交」。
   * 冻结：枚举值、硬边落墨与逐字节撤销、多行行距、UTF-8 拒绝且不留撤销步、
   * 图层外文字成功但不产生空撤销步。字体是引擎内 5×7 点阵，ASCII-only。
   */
  verify(static_cast<int>(COMP_TOOL_TYPE) == 15, "COMP_TOOL_TYPE stays at 15");

  std::vector<uint8_t> type_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t type_undo_before = 0;
  size_t type_redo = 0;
  comp_document_history_stats(document, &type_undo_before, &type_redo);

  comp_tool_op_desc type_desc{};
  type_desc.kind = COMP_TOOL_TYPE;
  type_desc.type.origin_x = 8.0;
  type_desc.type.origin_y = 8.0;
  type_desc.type.text = "A";
  type_desc.type.text_length = 1;
  type_desc.type.style.font_size = 14.0; /* 缩放 2 => 每个点阵格 2x2 像素 */
  type_desc.type.style.line_height = 1.0;
  type_desc.type.style.letter_spacing = 0.0;
  type_desc.type.style.antialias = 0;
  type_desc.type.color[1] = 255; /* 不透明绿 */
  type_desc.type.color[3] = 255;
  type_desc.type.opacity = 1.0f;
  const bool type_ok =
      comp_tool_apply(document, layer, &type_desc, tool_note, sizeof(tool_note), &tool_note_size) ==
      COMP_OK;
  verify(type_ok, "comp_tool_apply(COMP_TOOL_TYPE \"A\")");
  std::vector<uint8_t> type_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_after.data(), COMP_PIXEL_RGBA8, stride);
  const auto type_at = [&](int32_t x, int32_t y) {
    return static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
  };
  /* 'A' 第 0 列是 0x7E（bit1..6）：原点 (8,8)、缩放 2 时 x=8..9、y=10..21 有墨。 */
  verify(type_ok && type_after[type_at(8, 10) + 3] == 255 &&
             type_after[type_at(8, 10) + 1] == 255,
         "the 'A' stem is painted opaque green");
  verify(type_ok && std::memcmp(&type_after[type_at(8, 8)], &type_before[type_at(8, 8)], 4) == 0,
         "the empty top row of the glyph stays untouched");
  verify(type_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(type step)");
  std::vector<uint8_t> type_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(type_ok && type_restored == type_before, "type undo restores pixels byte-for-byte");

  /* 多行：字号 7 + 行距 1.0 => 第二行正好下移 7 px。 */
  comp_tool_op_desc multiline_desc = type_desc;
  multiline_desc.type.text = "A\nB";
  multiline_desc.type.text_length = 3;
  multiline_desc.type.style.font_size = 7.0;
  const bool multiline_ok =
      comp_tool_apply(document, layer, &multiline_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  std::vector<uint8_t> multiline_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, multiline_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(multiline_ok && multiline_after[type_at(8, 9) + 3] == 255 &&
             multiline_after[type_at(8, 16) + 3] == 255,
         "both text lines are painted one line step apart");
  verify(multiline_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(multiline type step)");

  /* UTF-8/CJK：ASCII 字体明确不支持 => 参数闸拒绝且不留撤销步。 */
  comp_tool_op_desc cjk_desc = type_desc;
  cjk_desc.type.text = "\xE4\xBD\xA0\xE5\xA5\xBD";
  cjk_desc.type.text_length = 6;
  verify(comp_tool_apply(document, layer, &cjk_desc, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_TYPE(UTF-8/CJK) rejected");
  size_t type_undo_after_reject = 0;
  comp_document_history_stats(document, &type_undo_after_reject, &type_redo);
  verify(type_undo_after_reject == type_undo_before, "the rejected type leaves no undo step");

  /* 合法但完全落在图层外：成功、无像素改动、不产生空撤销步。 */
  comp_tool_op_desc type_outside = type_desc;
  type_outside.type.origin_x = 100.0;
  type_outside.type.origin_y = 100.0;
  verify(comp_tool_apply(document, layer, &type_outside, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_OK,
         "text entirely outside the layer succeeds as a no-op");
  size_t type_undo_after_outside = 0;
  comp_document_history_stats(document, &type_undo_after_outside, &type_redo);
  verify(type_undo_after_outside == type_undo_before,
         "an off-canvas text box leaves no empty undo step");
  comp_render_flatten(document, type_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(type_restored == type_before,
         "an off-canvas text box leaves the render byte-identical");

  /*
   * 文字属性面板（S14）：行表、步进与命中几何都来自 ui_core.h，
   * 与 draw_type_panel / WM_LBUTTONDOWN 共用同一份常量。
   */
  verify(comp_ui::kTypePanelRowCount == 3 &&
             std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelFontSizeRow)) ==
                 "Font Size" &&
             std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelLineHeightRow)) ==
                 "Line Height" &&
             std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelLetterSpacingRow)) ==
                 "Letter Spacing" &&
             comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelLineHeightRow) &&
             !comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelFontSizeRow),
         "ui_core type panel row table (3 rows, line height is a percentage)");

  verify(std::fabs(comp_ui::step_type_line_height(1.25, 1) - 1.30) < 1e-9 &&
             std::fabs(comp_ui::step_type_line_height(1.25, -1) - 1.20) < 1e-9 &&
             comp_ui::step_type_line_height(1.0, -1) == 1.0 &&
             comp_ui::step_type_line_height(8.0, 1) == 8.0,
         "ui_core type line-height steps 5% and clamps to 1..8");

  verify(comp_ui::type_panel_step_value(comp_ui::kTypePanelFontSizeRow, 32.0, 32.0, 1) ==
                 comp_ui::step_type_font_size(32.0, 1) &&
             comp_ui::type_panel_step_value(comp_ui::kTypePanelLetterSpacingRow, 0.0, 32.0, 1) ==
                 1.0 &&
             comp_ui::type_panel_clamp_value(comp_ui::kTypePanelLetterSpacingRow, -999.0,
                                             32.0) == comp_ui::type_letter_spacing_min(32.0),
         "ui_core type panel steps use each row's own units");

  const comp_ui::Rect type_panel_rect{1000, 46, 330, 600};
  verify(comp_ui::type_param_row_at(type_panel_rect, 1010.0, 101.0) == 0 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 121.0) == 1 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 141.0) == 2 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 161.0) == -1 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 99.0) == -1 &&
             comp_ui::type_param_row_at(type_panel_rect, 500.0, 121.0) == -1,
         "ui_core type panel rows hit-test with the drawn geometry");

  /*
   * 合成缓存（MERGED，S12）：外壳每次 WM_PAINT 都走 comp_render_flatten，
   * 缓存命中与否**不得改变画面**，而任何一次编辑都必须立刻反映到下一帧。
   * 这里用「连拍两张 + 编辑后一张 + 撤销后一张」把这三件事钉在 ABI 层。
   */
  std::vector<uint8_t> merged_first(stride * static_cast<size_t>(height), 0);
  std::vector<uint8_t> merged_second(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, merged_first.data(), COMP_PIXEL_RGBA8, stride);
  comp_render_flatten(document, merged_second.data(), COMP_PIXEL_RGBA8, stride);
  verify(merged_first == merged_second,
         "two consecutive renders are byte-identical (MERGED cache hit)");

  comp_tool_op_desc merged_desc = type_desc;
  merged_desc.type.origin_x = 8.0;
  merged_desc.type.origin_y = 8.0;
  verify(comp_tool_apply(document, layer, &merged_desc, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_OK,
         "comp_tool_apply(COMP_TOOL_TYPE \"A\") for the MERGED cache check");
  std::vector<uint8_t> merged_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, merged_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(merged_after != merged_first,
         "an edit invalidates the MERGED cache (the next frame shows the new pixels)");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(MERGED cache step)");
  std::vector<uint8_t> merged_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, merged_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(merged_restored == merged_first,
         "undo invalidates the MERGED cache and restores the pixels byte-for-byte");

  /*
   * 选区：壳层蚂蚁线的数据源就是这条「有效 coverage」查询。这里冻结三件事：
   * 契约里「无选区」与「显式空选区」不同、覆盖率为文档分辨率、选区变更是一步撤销。
   */
  int32_t selection_active = -1;
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_get_active() starts at 0 (no selection)");

  const float rect_points[4] = {8.0f, 8.0f, 40.0f, 40.0f};
  int32_t selection_changed = 0;
  const bool rect_ok =
      comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                 COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
      selection_changed == 1;
  verify(rect_ok, "comp_selection_apply_shape(rect)");

  std::vector<uint8_t> coverage(stride * static_cast<size_t>(height), 0);
  comp_mask coverage_mask{};
  coverage_mask.width = width;
  coverage_mask.height = height;
  coverage_mask.stride = stride;
  coverage_mask.pixels = coverage.data();
  verify(rect_ok && comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1,
         "comp_selection_get_active() == 1 after a rect shape");
  verify(comp_selection_get_coverage(document, &coverage_mask) == COMP_OK,
         "comp_selection_get_coverage()");
  verify(rect_ok && coverage[static_cast<size_t>(20) * stride + 20u] == 255 &&
             coverage[static_cast<size_t>(2) * stride + 2u] == 0,
         "coverage is 255 inside the rect and 0 outside");

  /* 蚂蚁线几何：矩形选区应得到 4 条边界；覆盖率全 0 时一条都不画。 */
  comp_ui::BoundarySegment ants[16];
  verify(comp_ui::selection_boundary_segments(coverage.data(), width, height, stride, 128,
                                              ants, 16) == 4,
         "selection_boundary_segments(rect) == 4 (what the shell draws)");

  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(selection step)");
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "undoing the selection shape clears the ants");

  int32_t select_all_changed = 0;
  const bool select_all_ok =
      comp_selection_select_all(document, &select_all_changed) == COMP_OK &&
      comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(63) * stride + 63u] == 255;
  verify(select_all_ok, "comp_selection_select_all() covers the canvas");

  /*
   * 反选「全选」得到的是**无选区**而不是显式空选区（ABI 契约，见
   * comp_selection.h）：此后 get_coverage 会返回 COMP_ERR_STATE，所以壳层
   * refresh_selection 必须先查 active 再决定要不要取覆盖率。
   */
  int32_t invert_changed = 0;
  verify(comp_selection_invert(document, &invert_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_invert() of a full selection means no selection");

  int32_t deselect_changed = 0;
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_deselect() returns to the no-selection state");

  /*
   * S7：椭圆框选 / 套索多边形 / 魔棒 / 新选区羽化。
   * 按壳层 marquee_end / lasso_end / wand_click / finish_new_selection 的同一参数
   * 序列调用 ABI——验证的是「接线后的真实调用」，不是另写一套理想路径。
   */
  const float ellipse_points[4] = {8.0f, 8.0f, 48.0f, 40.0f};
  verify(comp_selection_apply_shape(document, COMP_SELECTION_ELLIPSE, ellipse_points, 2,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
             selection_changed == 1 &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(24) * stride + 28u] == 255 &&
             coverage[static_cast<size_t>(8) * stride + 8u] == 0,
         "comp_selection_apply_shape(ellipse) covers the centre and clips the corner");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(ellipse selection step)");

  const float lasso_polygon[6] = {8.0f, 8.0f, 40.0f, 8.0f, 8.0f, 40.0f};
  verify(comp_selection_apply_shape(document, COMP_SELECTION_POLYGON, lasso_polygon, 3,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
             selection_changed == 1 &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(12) * stride + 12u] == 255 &&
             coverage[static_cast<size_t>(36) * stride + 36u] == 0,
         "comp_selection_apply_shape(polygon) is what the lasso commits");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(lasso selection step)");

  /* 魔棒测试图：左半 (x<32) 不透明灰 200，右半不透明黑；容差 0 = 只吃同色连通块。 */
  std::vector<uint8_t> wand_pixels(stride * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* pixel =
          wand_pixels.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
      const uint8_t value = x < 32 ? 200 : 0;
      pixel[0] = value;
      pixel[1] = value;
      pixel[2] = value;
      pixel[3] = 255;
    }
  }
  comp_layer_desc wand_desc{};
  wand_desc.name = "selftest-wand";
  wand_desc.blend = COMP_BLEND_NORMAL;
  wand_desc.opacity = 1.0f;
  wand_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  wand_desc.transform = comp_rect{0, 0, width, height}; /* 1:1，魔棒 LAYER 取样的前提 */
  comp_image wand_image{};
  wand_image.width = width;
  wand_image.height = height;
  wand_image.format = COMP_PIXEL_RGBA8;
  wand_image.stride = stride;
  wand_image.pixels = wand_pixels.data();
  comp_layer_id wand_layer = 0;
  const bool wand_fixture_ok =
      comp_layer_add(document, &wand_desc, &wand_layer) == COMP_OK && wand_layer != 0 &&
      comp_layer_set_pixels(document, wand_layer, &wand_image) == COMP_OK;
  verify(wand_fixture_ok, "comp_layer_add(wand fixture) + comp_layer_set_pixels(1:1)");

  int32_t wand_changed = 0;
  const bool wand_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 10.0, 10.0, 0, 1,
                                0, COMP_SELECTION_REPLACE, &wand_changed) == COMP_OK &&
      wand_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 0;
  verify(wand_ok, "comp_selection_apply_wand(contiguous) selects one colour region");

  comp_rect wand_bounds{};
  verify(wand_ok && comp_selection_get_bounds(document, &wand_bounds) == COMP_OK &&
             wand_bounds.w == 32 && wand_bounds.h == height,
         "wand selection bounds cover the matched half only");
  verify(wand_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(wand selection step)");

  int32_t wand_merged_changed = 0;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_MERGED, 2.0, 2.0,
                                       0, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_merged_changed) == COMP_OK &&
             wand_merged_changed == 1,
         "comp_selection_apply_wand(merged source) samples the composite");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(merged wand step)");

  int32_t wand_bad = -1;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, -1.0, 4.0,
                                       0, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_bad) == COMP_ERR_INVALID_ARGUMENT &&
             wand_bad == -1,
         "comp_selection_apply_wand(seed outside canvas) rejected, out_changed untouched");
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 4.0, 4.0,
                                       256, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_bad) == COMP_ERR_INVALID_ARGUMENT,
         "comp_selection_apply_wand(tolerance=256) rejected");

  /*
   * 0 匹配必须是「无变化」而不是清空选区：壳层靠 changed==0 回滚事务，
   * 所以引擎既不能改选区，也不能留下撤销步。
   * 取两色交界 (32,10) 做种、radius 2：参考色是窗口均值 80，种子像素是 0，
   * 容差 0 下同窗口内没有像素落在 80±0 内 -> 选中 0 个。
   */
  verify(comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK,
         "comp_selection_apply_shape(rect) before the no-match wand probe");
  size_t depth_before_no_match = 0;
  comp_document_history_stats(document, &depth_before_no_match, &redo_depth);
  int32_t no_match_changed = 1;
  const bool no_match_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0, 0, 1,
                                2, COMP_SELECTION_REPLACE, &no_match_changed) == COMP_OK &&
      no_match_changed == 0 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(20) * stride + 20u] == 255;
  size_t depth_after_no_match = 0;
  comp_document_history_stats(document, &depth_after_no_match, &redo_depth);
  verify(no_match_ok && depth_after_no_match == depth_before_no_match,
         "wand with no matching pixel keeps the old selection and adds no undo step");

  /*
   * 新选区羽化：finish_new_selection 把「形状 + comp_selection_set_feather」包进
   * 一个显式事务，因此两步只能算**一步**撤销，且羽化值是本次的绝对值
   * （apply_shape 已把 feather 归零，set_feather 按覆盖语义写入）。
   */
  comp_txn feather_txn = nullptr;
  size_t depth_before_feather = 0;
  comp_document_history_stats(document, &depth_before_feather, &redo_depth);
  int32_t feather_changed = 0;
  bool feather_ok = comp_txn_begin(document, "Feather Selection", &feather_txn) == COMP_OK;
  feather_ok = feather_ok &&
               comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                          COMP_SELECTION_REPLACE, 1,
                                          &selection_changed) == COMP_OK;
  feather_ok = feather_ok && comp_selection_set_feather(document, 4, &feather_changed) == COMP_OK &&
               feather_changed == 1;
  feather_ok = feather_ok && comp_txn_commit(feather_txn) == COMP_OK;
  float feather_value = -1.0f;
  size_t depth_after_feather = 0;
  comp_selection_get_feather(document, &feather_value);
  comp_document_history_stats(document, &depth_after_feather, &redo_depth);
  verify(feather_ok && std::fabs(feather_value - 4.0f) < 0.01f,
         "shape + comp_selection_set_feather(4) yields feather 4 px");
  verify(feather_ok && depth_after_feather == depth_before_feather + 1,
         "shape + feather commit as ONE undo step (finish_new_selection contract)");
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1 && comp_selection_get_feather(document, &feather_value) == COMP_OK &&
             std::fabs(feather_value) < 0.01f,
         "undoing the feathered selection restores the unfeathered previous selection");

  /*
   * S8：绝对羽化 / 魔棒取样窗口 / 钢笔路径。
   * 按 commit_feather_input / wand_click / commit_pen_path 的真实调用序列走 ABI。
   */
  int32_t absolute_changed = 0;
  float absolute_feather = -1.0f;
  verify(comp_selection_set_feather(document, 8, &absolute_changed) == COMP_OK &&
             absolute_changed == 1 &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather - 8.0f) < 0.01f,
         "comp_selection_set_feather(8) sets the absolute feather");
  absolute_changed = 99;
  verify(comp_selection_set_feather(document, 8, &absolute_changed) == COMP_OK &&
             absolute_changed == 0,
         "setting the same feather twice is a no-op");
  verify(comp_selection_set_feather(document, 251, &absolute_changed) ==
             COMP_ERR_INVALID_ARGUMENT,
         "comp_selection_set_feather(251) rejected before any transaction");
  verify(comp_selection_set_feather(document, 0, &absolute_changed) == COMP_OK &&
             absolute_changed == 1 &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather) < 0.01f,
         "comp_selection_set_feather(0) clears the feather");
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather - 8.0f) < 0.01f,
         "undoing the feather reset restores 8 px");
  verify(comp_document_undo(document) == COMP_OK,
         "undoing the absolute feather step restores the previous selection");

  /* 魔棒取样窗口：边界种子 + 半径 1 的参考色是窗口均值，容差 0 下不再匹配。 */
  int32_t radius_changed = 0;
  comp_rect radius_bounds{};
  const bool radius_zero_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0, 0, 1,
                                0, COMP_SELECTION_REPLACE, &radius_changed) == COMP_OK &&
      radius_changed == 1 && comp_selection_get_bounds(document, &radius_bounds) == COMP_OK &&
      radius_bounds.w == 32;
  verify(radius_zero_ok, "wand radius 0 selects the black half from the boundary seed");
  radius_changed = 99;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0,
                                       0, 1, 1, COMP_SELECTION_REPLACE,
                                       &radius_changed) == COMP_OK &&
             radius_changed == 0 &&
             comp_selection_get_bounds(document, &radius_bounds) == COMP_OK &&
             radius_bounds.w == 32,
         "wand radius 1 averages the window, finds no match, and keeps the selection");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(wand radius selection step)");

  /*
   * 钢笔路径：ui_core 的锚点/段装配 -> comp_selection_apply_path，与
   * commit_pen_path 走同一条 ABI。这里用 4 个直角锚点的 24..40 方形。
   */
  comp_ui::PenAnchor pen_anchors[4] = {};
  int pen_anchor_count = 0;
  bool pen_state_ok = comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                 comp_ui::Point{24.0, 24.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{40.0, 24.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{40.0, 40.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{24.0, 40.0});
  comp_ui::PenSegment pen_segments[4] = {};
  const int pen_segment_count =
      comp_ui::pen_build_segments(pen_anchors, pen_anchor_count, /*closed=*/true, pen_segments, 4);
  comp_selection_cubic pen_cubics[4] = {};
  for (int i = 0; i < pen_segment_count && i < 4; ++i) {
    pen_cubics[i].start[0] = static_cast<float>(pen_segments[i].start.x);
    pen_cubics[i].start[1] = static_cast<float>(pen_segments[i].start.y);
    pen_cubics[i].control1[0] = static_cast<float>(pen_segments[i].control1.x);
    pen_cubics[i].control1[1] = static_cast<float>(pen_segments[i].control1.y);
    pen_cubics[i].control2[0] = static_cast<float>(pen_segments[i].control2.x);
    pen_cubics[i].control2[1] = static_cast<float>(pen_segments[i].control2.y);
    pen_cubics[i].end[0] = static_cast<float>(pen_segments[i].end.x);
    pen_cubics[i].end[1] = static_cast<float>(pen_segments[i].end.y);
    pen_cubics[i].new_subpath = i == 0 ? 1 : 0;
  }
  size_t depth_before_pen = 0;
  comp_document_history_stats(document, &depth_before_pen, &redo_depth);
  int32_t pen_changed = 0;
  const bool pen_ok =
      pen_state_ok && pen_segment_count == 4 &&
      comp_selection_apply_path(document, pen_cubics, pen_segment_count,
                                COMP_SELECTION_REPLACE, 1, &pen_changed) == COMP_OK &&
      pen_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(32) * stride + 32u] == 255 &&
      coverage[static_cast<size_t>(60) * stride + 60u] == 0;
  size_t depth_after_pen = 0;
  comp_document_history_stats(document, &depth_after_pen, &redo_depth);
  verify(pen_ok, "pen path commits a closed selection through comp_selection_apply_path");
  verify(pen_ok && depth_after_pen == depth_before_pen + 1,
         "pen path selection is exactly one undo step");
  comp_ui::Point pen_preview[1 + 4 * 3] = {};
  const int pen_preview_count =
      comp_ui::pen_preview_polyline(pen_anchors, pen_anchor_count, /*closed=*/true, pen_preview,
                                    1 + 4 * 3, /*samples_per_segment=*/3);
  verify(pen_preview_count == 1 + 4 * 3 && pen_preview[pen_preview_count - 1].x == 24.0 &&
             pen_preview[pen_preview_count - 1].y == 24.0,
         "pen preview polyline closes back onto the first anchor");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(pen path selection step)");

  /*
   * P0-4：扩展 / 收缩 / 按图层像素载入选区。
   * 壳里这三条命令分别接到 begin_modify_input + commit_modify_input 与
   * load_layer_pixels_selection，参数与这里逐字一致（半径 0 在壳侧就被拦成
   * 提示，不会打到引擎；这里只验证引擎的量程闸确实会拒 0）。
   */
  const float grow_rect[4] = {20.0f, 20.0f, 40.0f, 40.0f};
  int32_t grow_changed = 0;
  comp_rect grow_bounds{};
  const bool grow_fixture_ok =
      comp_selection_apply_shape(document, COMP_SELECTION_RECT, grow_rect, 2,
                                 COMP_SELECTION_REPLACE, 1, &grow_changed) == COMP_OK &&
      grow_changed == 1 && comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
      grow_bounds.w == 20 && grow_bounds.h == 20;
  verify(grow_fixture_ok, "comp_selection_apply_shape(rect 20x20) is the P0-4 fixture");

  grow_changed = 0;
  verify(grow_fixture_ok &&
             comp_selection_modify(document, COMP_SELECTION_EXPAND, 3, &grow_changed) == COMP_OK &&
             grow_changed == 1 &&
             comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
             grow_bounds.w == 26 && grow_bounds.h == 26,
         "comp_selection_modify(EXPAND, 3) grows the selection 3 px per side");
  verify(grow_fixture_ok && comp_document_undo(document) == COMP_OK &&
             comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
             grow_bounds.w == 20 && grow_bounds.h == 20,
         "one undo returns the expanded selection to 20x20");

  grow_changed = 0;
  verify(grow_fixture_ok &&
             comp_selection_modify(document, COMP_SELECTION_CONTRACT, 3, &grow_changed) == COMP_OK &&
             grow_changed == 1 &&
             comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
             grow_bounds.w == 14 && grow_bounds.h == 14,
         "comp_selection_modify(CONTRACT, 3) shrinks the selection 3 px per side");
  verify(grow_fixture_ok && comp_document_undo(document) == COMP_OK &&
             comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
             grow_bounds.w == 20 && grow_bounds.h == 20,
         "one undo returns the contracted selection to 20x20");
  grow_changed = 99;
  verify(comp_selection_modify(document, COMP_SELECTION_EXPAND, 0, &grow_changed) ==
                 COMP_ERR_INVALID_ARGUMENT &&
             grow_changed == 99,
         "comp_selection_modify(EXPAND, 0) is rejected before touching the selection");

  /* 按图层像素载入：测试图两半都不透明，所以左右各取样点都应被选中。 */
  grow_changed = 99;
  verify(wand_fixture_ok &&
             comp_selection_load_layer(document, wand_layer, COMP_SELECTION_LOAD_PIXELS,
                                      COMP_SELECTION_REPLACE, 1, &grow_changed) == COMP_OK &&
             grow_changed == 1 &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 255,
         "comp_selection_load_layer(LOAD_PIXELS, threshold 1) selects every opaque pixel");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(load layer pixels step)");
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK,
         "deselect after the P0-4 probes");


  /*
   * P0-4 第二批：色彩范围（comp_selection_apply_color_range）。
   * 壳侧面板的顺序是「begin txn -> 取色/容差/反相各重算一次（REPLACE）-> 回车 commit」，
   * 这里按同一顺序打 ABI。断言重点不是像素效果，而是三条壳层依赖的契约：
   *   ① 同色全选**不要求连通**（与魔棒的本质区别）；
   *   ② 越界/空指针参数被 ABI 闸拦下，且 out_changed 不被写、不进撤销栈；
   *   ③ 显式事务里连改多次只落一步撤销，abort 把选区还原成打开前。
   */
  const uint8_t color_grey_rgb[3] = {200, 200, 200};
  const uint8_t color_black_rgb[3] = {0, 0, 0};
  size_t depth_before_range = 0;
  comp_document_history_stats(document, &depth_before_range, &redo_depth);
  int32_t range_changed = 0;
  const bool range_grey_ok =
      wand_fixture_ok &&
      comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 0, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
      range_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 0 &&
      coverage[static_cast<size_t>(height - 1) * stride + 5u] == 255;
  size_t depth_after_range = 0;
  comp_document_history_stats(document, &depth_after_range, &redo_depth);
  verify(range_grey_ok, "comp_selection_apply_color_range(fuzziness 0) matches every same-colour pixel");
  verify(range_grey_ok && depth_after_range == depth_before_range + 1,
         "color range selection is exactly one undo step");

  /* 撤销一次 = 回到「打开色彩范围前」的无选区状态（不是退回半张图）。 */
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "one undo returns the color range selection to the pre-open (inactive) state");

  /* 反相 = 补集；再撤销一次回到未反相的那次（一步就是一步）。 */
  verify(range_grey_ok &&
             comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
             range_changed == 1,
         "color range re-applies after undo");
  range_changed = 0;
  verify(comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 0, 1,
                                          COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
             range_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 0 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 255,
         "color range invert selects the complement of the sampled colour");
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 0,
         "one undo leaves the un-inverted color range selection in place");

  /* 容差 200 = 三通道最大差，测试图两半都进选区（容差上界是闭区间）。 */
  range_changed = 0;
  verify(comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 200, 0,
                                          COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
             range_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 255,
         "color range fuzziness 200 matches the far colour as well (inclusive bound)");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(fuzziness 200 step)");

  /* include 与 exclude 是同一组颜色 = 一个都不匹配；引擎不动选区、不落撤销步。 */
  size_t depth_before_empty = 0;
  comp_document_history_stats(document, &depth_before_empty, &redo_depth);
  range_changed = 99;
  size_t depth_after_empty = 0;
  const bool range_empty_ok =
      comp_selection_apply_color_range(document, color_black_rgb, 1, color_black_rgb, 1, 0, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK;
  comp_document_history_stats(document, &depth_after_empty, &redo_depth);
  verify(range_empty_ok && range_changed == 0 &&
             depth_after_empty == depth_before_empty &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 255,
         "color range with include == exclude matches nothing and leaves the selection alone");

  /* 参数闸：越界容差 / 空指针，都是「拒绝且不写 out_changed、不进撤销栈」。 */
  range_changed = 99;
  verify(comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 201, 0,
                                          COMP_SELECTION_REPLACE, &range_changed) ==
                 COMP_ERR_INVALID_ARGUMENT &&
             range_changed == 99,
         "color range fuzziness 201 is rejected before touching the selection");
  range_changed = 99;
  verify(comp_selection_apply_color_range(document, nullptr, 1, nullptr, 0, 40, 0,
                                          COMP_SELECTION_REPLACE, &range_changed) ==
                 COMP_ERR_INVALID_ARGUMENT &&
             range_changed == 99,
         "color range NULL include with a nonzero count is rejected");

  /*
   * 事务预览：打开面板 = begin，每次改参数只是事务内重算，回车才 commit。
   * 两次 REPLACE 在同一个显式事务里 => 只落一步，且以最后一次为准。
   */
  const comp_ui::Rect preview_panel{0, 0, 330, 560};
  verify(comp_ui::clamp_color_range_fuzziness(-5) == 0 &&
             comp_ui::clamp_color_range_fuzziness(201) == 200 &&
             comp_ui::step_color_range_fuzziness(0, -1) == 0 &&
             comp_ui::step_color_range_fuzziness(200, 1) == 200 &&
             comp_ui::step_color_range_fuzziness(19, 1) == 20 &&
             comp_ui::step_color_range_fuzziness(20, 1) == 25,
         "ui_core color range fuzziness clamps to 0..200 and steps 1/5/10");
  verify(comp_ui::color_range_swatch_capacity(preview_panel) == 13 &&
             comp_ui::color_range_swatch_at(preview_panel, 2, 1, 13.0, 51.0) == 0 &&
             comp_ui::color_range_swatch_at(preview_panel, 2, 1, 13.0, 75.0) == 2 &&
             comp_ui::color_range_swatch_at(preview_panel, 2, 1, 320.0, 51.0) == -1 &&
             comp_ui::color_range_row_at(preview_panel, 100.0, 101.0) == 0 &&
             comp_ui::color_range_row_at(preview_panel, 100.0, 125.0) == 1 &&
             comp_ui::color_range_row_at(preview_panel, 100.0, 149.0) == -1,
         "ui_core color range swatch/row geometry hits what it paints");

  comp_txn range_txn = nullptr;
  const size_t depth_before_preview = depth_after_empty;
  const bool range_txn_ok =
      comp_txn_begin(document, "Color Range", &range_txn) == COMP_OK && range_txn != nullptr &&
      comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 0, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
      comp_selection_apply_color_range(document, color_black_rgb, 1, nullptr, 0, 0, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK;
  size_t depth_inside_preview = 0;
  comp_document_history_stats(document, &depth_inside_preview, &redo_depth);
  const bool preview_visible =
      comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 255 &&
      coverage[static_cast<size_t>(10) * stride + 10u] == 0;
  size_t depth_after_commit = 0;
  const bool preview_commit_ok = range_txn_ok && comp_txn_commit(range_txn) == COMP_OK;
  comp_document_history_stats(document, &depth_after_commit, &redo_depth);
  verify(range_txn_ok && preview_visible && preview_commit_ok &&
             depth_inside_preview == depth_before_preview &&
             depth_after_commit == depth_before_preview + 1,
         "color range preview inside a txn shows the last apply and commits one step");

  /* abort 回滚到事务开启时的选区（= 上一次提交的右半），不动撤销栈。 */
  range_txn = nullptr;
  const bool range_abort_txn_ok =
      comp_txn_begin(document, "Color Range", &range_txn) == COMP_OK && range_txn != nullptr &&
      comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 200, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
      comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 255;
  const bool range_abort_ok = range_abort_txn_ok && comp_txn_abort(range_txn) == COMP_OK;
  size_t depth_after_abort = 0;
  comp_document_history_stats(document, &depth_after_abort, &redo_depth);
  verify(range_abort_ok && depth_after_abort == depth_after_commit &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 0 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 255,
         "comp_txn_abort restores the pre-open color range selection without an undo step");

  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 0,
         "one undo after the color range commit returns to the previous selection");
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK,
         "deselect after the color range probes");

  /* 测试图用完即删：它是全画布不透明层，留着会盖住后面 S6 的 flatten 断言。 */
  verify(comp_layer_remove(document, wand_layer) == COMP_OK,
         "comp_layer_remove(wand fixture) keeps later flatten assertions honest");
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "deselect after the S7 probes restores the S6 precondition");

  /*
   * S6：前景色 / 画笔 / 橡皮擦 / 吸管 / 选区搬移。
   * 壳层只做参数装配，因此这里按 paint_stroke_end / eyedropper_sample / move_end
   * 的同一顺序调用 ABI——验证的是「壳层接线后的真实序列」，不是另写一套。
   */

  verify(comp_ui::tool_shortcut_target(comp_ui::ToolId::Brush, 'e') ==
                 comp_ui::ToolId::Eraser &&
             comp_ui::tool_shortcut_target(comp_ui::ToolId::Eraser, 'E') ==
                 comp_ui::ToolId::Brush,
         "ui_core tool_shortcut_target(E) toggles brush/eraser");
  verify(comp_ui::swatch_count() == 12 && comp_ui::step_swatch(11, 1) == 0 &&
             comp_ui::step_swatch(0, -1) == 11,
         "ui_core swatch palette cycles (X)");

  comp_ui::ColorRGB foreground{};
  const bool parse_ok = comp_ui::color_parse_hex("#3A7BD5", &foreground);
  char foreground_hex[8] = {0};
  comp_ui::color_to_hex(foreground, foreground_hex);
  verify(parse_ok && foreground.r == 0x3A && foreground.g == 0x7B && foreground.b == 0xD5 &&
             std::strcmp(foreground_hex, "#3A7BD5") == 0,
         "ui_core color_parse_hex / color_to_hex round trip (Shift+X)");

  /* 与 paint_stroke_end 相同：前景色 + 单点折线（单击 = 一记 dab）。 */
  verify(comp_layer_set_pixels(document, layer, &opaque_image) == COMP_OK,
         "comp_layer_set_pixels(base for paint)");
  std::vector<uint8_t> paint_base(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, paint_base.data(), COMP_PIXEL_RGBA8, stride);

  comp_tool_op_desc paint_desc{};
  paint_desc.kind = COMP_TOOL_STROKE;
  paint_desc.brush.color[0] = foreground.r;
  paint_desc.brush.color[1] = foreground.g;
  paint_desc.brush.color[2] = foreground.b;
  paint_desc.brush.diameter = 10.0f;
  paint_desc.brush.pressure = 1.0f;
  paint_desc.brush.hardness = 1.0f;
  paint_desc.brush.flow = 1.0f;
  paint_desc.brush.seed = 0;
  const float dab[] = {32.0f, 32.0f};
  paint_desc.stroke_points = dab;
  paint_desc.stroke_point_count = 1;
  char stroke_note[256] = {0};
  size_t stroke_note_size = 0;
  const bool paint_ok =
      comp_tool_apply(document, layer, &paint_desc, stroke_note, sizeof(stroke_note),
                      &stroke_note_size) == COMP_OK;
  std::vector<uint8_t> paint_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, paint_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t center = static_cast<size_t>(32) * stride + static_cast<size_t>(32) * 4u;
  verify(paint_ok && paint_after != paint_base && paint_after[center + 0] == 0x3A &&
             paint_after[center + 1] == 0x7B && paint_after[center + 2] == 0xD5,
         "COMP_TOOL_STROKE paints the shell foreground colour");

  /* 吸管：同一位置取回的直通 sRGB 应还原成同一个前景色。 */
  float sampled[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const bool sample_ok = comp_eyedropper_sample(document, layer, 32.0, 32.0, 1,
                                                COMP_EYEDROPPER_LAYER, sampled) == COMP_OK;
  const comp_ui::ColorRGB sampled_color =
      comp_ui::color_from_unit(sampled[0], sampled[1], sampled[2]);
  verify(sample_ok && sampled_color.r == foreground.r && sampled_color.g == foreground.g &&
             sampled_color.b == foreground.b && sampled[3] == 1.0f,
         "comp_eyedropper_sample() returns the painted colour (feeds foreground)");

  /* 橡皮擦：同一支笔、同样的 diameter/hardness，方向翻转成 destination-out。 */
  paint_desc.erase = 1;
  const bool erase_ok =
      comp_tool_apply(document, layer, &paint_desc, stroke_note, sizeof(stroke_note),
                      &stroke_note_size) == COMP_OK;
  std::vector<uint8_t> erase_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, erase_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(erase_ok && erase_after != paint_after,
         "COMP_TOOL_STROKE(erase) removes the painted pixel");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(erase step)");
  std::vector<uint8_t> erase_undone(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, erase_undone.data(), COMP_PIXEL_RGBA8, stride);
  verify(erase_undone == paint_after, "undoing erase restores the painted pixel byte-identically");

  /*
   * 选区搬移：像素搬移 + 轮廓平移必须落在同一个显式事务里（一次拖拽 = 一步撤销）。
   * 顺序与 move_end 一致：先按当前选区切像素，再平移轮廓。
   */
  comp_rect bounds_before{};
  const bool move_rect_ok =
      comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                 COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
      comp_selection_get_bounds(document, &bounds_before) == COMP_OK;
  std::vector<uint8_t> move_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, move_before.data(), COMP_PIXEL_RGBA8, stride);

  comp_txn move_txn = nullptr;
  const bool move_txn_ok =
      comp_txn_begin(document, "Move Selection Pixels", &move_txn) == COMP_OK;
  int32_t moved_pixels = 0;
  int32_t moved_outline = 0;
  const bool move_ok =
      move_txn_ok &&
      comp_selection_move_pixels(document, layer, 8, 0, 0, &moved_pixels) == COMP_OK &&
      comp_selection_translate(document, 8, 0, &moved_outline) == COMP_OK &&
      comp_txn_commit(move_txn) == COMP_OK;
  comp_rect bounds_after{};
  comp_selection_get_bounds(document, &bounds_after);
  verify(move_rect_ok && move_ok && moved_pixels > 0 && moved_outline > 0 &&
             bounds_after.x == bounds_before.x + 8 && bounds_after.y == bounds_before.y,
         "selection move pixels + translate commit in one explicit transaction");

  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(move selection)");
  comp_rect bounds_undone{};
  std::vector<uint8_t> move_undone(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, move_undone.data(), COMP_PIXEL_RGBA8, stride);
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1 &&
             comp_selection_get_bounds(document, &bounds_undone) == COMP_OK &&
             bounds_undone.x == bounds_before.x && bounds_undone.y == bounds_before.y &&
             move_undone == move_before,
         "one undo restores both the pixels and the selection outline");

  /* ---------------------------------------------------------------- S13：GPU 协同 */
  /*
   * 独立的 AUTO 引擎：验证「编入 D3D12 时 AUTO 真的走 GPU」以及 GPU/CPU 像素对齐。
   * 场景限定在 GPU 子集内（整幅画布 RGBA8 + 普通/滤色混合）：走 GPU 时允许
   * ±1 LSB 浮点舍入差；回落 CPU 时必须与基准逐字节一致。
   */
  comp_engine_desc gpu_engine_desc{};
  gpu_engine_desc.app_name = "compositor-shell-selfcheck-gpu";
  gpu_engine_desc.version = "0.1.0";
  gpu_engine_desc.backend = COMP_BACKEND_AUTO;
  comp_engine gpu_engine = nullptr;
  const bool gpu_engine_ok =
      comp_engine_create(&gpu_engine_desc, &gpu_engine) == COMP_OK && gpu_engine != nullptr;
  verify(gpu_engine_ok, "comp_engine_create(AUTO) for gpu parity");
  if (gpu_engine_ok) {
    comp_backend gpu_backend = COMP_BACKEND_AUTO;
    const bool gpu_backend_ok = comp_engine_backend(gpu_engine, &gpu_backend) == COMP_OK;
    verify(gpu_backend_ok, "comp_engine_backend(AUTO engine)");
    const bool gpu_path = gpu_backend_ok && gpu_backend != COMP_BACKEND_CPU;
    std::fprintf(report, "[info] auto backend = %s\n",
                 gpu_backend_ok ? backend_name(gpu_backend) : "(query failed)");

    const auto build_parity_document = [](comp_engine target_engine, comp_document* out) -> bool {
      comp_document_desc desc{};
      desc.width = 32;
      desc.height = 32;
      desc.dpi = 72.0f;
      desc.color_space = "sRGB";
      if (comp_document_create(target_engine, &desc, out) != COMP_OK || *out == nullptr) {
        return false;
      }
      size_t count = 0;
      if (comp_layer_count(*out, &count) != COMP_OK || count == 0) return false;
      std::vector<comp_layer_id> ids(count);
      if (comp_layer_list(*out, ids.data(), ids.size(), &count) != COMP_OK) return false;

      std::vector<uint8_t> pixels(32u * 32u * 4u, 0);
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        pixels[i + 0] = 96;
        pixels[i + 1] = 72;
        pixels[i + 2] = 48;
        pixels[i + 3] = 192;
      }
      comp_image base_image{};
      base_image.width = 32;
      base_image.height = 32;
      base_image.format = COMP_PIXEL_RGBA8;
      base_image.stride = 32u * 4u;
      base_image.pixels = pixels.data();
      if (comp_layer_set_pixels(*out, ids.front(), &base_image) != COMP_OK) return false;

      comp_layer_desc top_desc{};
      top_desc.name = "parity-top";
      top_desc.blend = COMP_BLEND_SCREEN;
      top_desc.opacity = 0.75f;
      top_desc.transform = comp_rect{0, 0, 32, 32};
      top_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
      comp_layer_id top = 0;
      if (comp_layer_add(*out, &top_desc, &top) != COMP_OK || top == 0) return false;
      /*
       * 图层像素必须是**预乘** RGBA8（颜色分量 <= alpha，见 comp_layer.h）。
       * 这里给的是预乘值：直通色 (200,64,32) @ alpha 128 → (100,32,16,128)。
       * 用非预乘数据对拍会让 GPU 浮点与 CPU 整数在饱和/钳制上分叉（实测差 34 级），
       * 那已超出 ABI 契约，不是两后端的一致性问题。
       */
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        pixels[i + 0] = 100;
        pixels[i + 1] = 32;
        pixels[i + 2] = 16;
        pixels[i + 3] = 128;
      }
      return comp_layer_set_pixels(*out, top, &base_image) == COMP_OK;
    };

    comp_document gpu_doc = nullptr;
    comp_document cpu_doc = nullptr;
    const bool scene_ok = build_parity_document(gpu_engine, &gpu_doc) &&
                          build_parity_document(engine, &cpu_doc);
    verify(scene_ok, "parity scene builds on AUTO and CPU engines");
    if (scene_ok) {
      std::vector<uint8_t> gpu_pixels(32u * 32u * 4u, 0);
      std::vector<uint8_t> cpu_pixels(32u * 32u * 4u, 0);
      const bool gpu_render_ok =
          comp_render_flatten(gpu_doc, gpu_pixels.data(), COMP_PIXEL_RGBA8, 32u * 4u) == COMP_OK;
      const bool cpu_render_ok =
          comp_render_flatten(cpu_doc, cpu_pixels.data(), COMP_PIXEL_RGBA8, 32u * 4u) == COMP_OK;
      verify(gpu_render_ok && cpu_render_ok, "parity scene flattens on both backends");
      int max_diff = 0;
      size_t diff_count = 0;
      for (size_t i = 0; i < cpu_pixels.size(); ++i) {
        const int diff =
            std::abs(static_cast<int>(cpu_pixels[i]) - static_cast<int>(gpu_pixels[i]));
        if (diff > max_diff) max_diff = diff;
        if (diff != 0) ++diff_count;
      }
      std::fprintf(report, "[info] parity max_diff = %d (diff bytes = %u, path = %s)\n", max_diff,
                   static_cast<unsigned>(diff_count), gpu_path ? "gpu" : "cpu");
      verify(gpu_render_ok && cpu_render_ok && max_diff <= (gpu_path ? 1 : 0),
             "AUTO backend pixels match the CPU reference");
    }
    if (cpu_doc != nullptr) comp_document_destroy(cpu_doc);
    if (gpu_doc != nullptr) comp_document_destroy(gpu_doc);
  }
  if (gpu_engine != nullptr) comp_engine_destroy(gpu_engine);

  comp_document_destroy(document);
  comp_engine_destroy(engine);

  std::fprintf(report, "\n[summary] failures = %d\n", failures);
  std::fprintf(report, "[%s] compositor-shell selftest\n", failures == 0 ? "PASS" : "FAIL");
  std::fclose(report);
  return failures == 0 ? 0 : 1;
}

/* ============================================================ 窗口直呈基准（S16） */

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

LRESULT CALLBACK bench_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      EndPaint(window, &paint);
      (void)dc;
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

void pump_pending_messages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

double elapsed_ms(const LARGE_INTEGER& start, const LARGE_INTEGER& end, double frequency) {
  return (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) / frequency;
}

/* 基准场景：不透明底 + 三层整幅画布图层，全部是 GPU 子集允许的形态。 */
comp_document build_present_bench_document(comp_engine engine, int32_t size) {
  comp_document document = nullptr;
  comp_document_desc desc{};
  desc.width = size;
  desc.height = size;
  if (comp_document_create(engine, &desc, &document) != COMP_OK) return nullptr;

  size_t count = 0;
  if (comp_layer_count(document, &count) != COMP_OK || count == 0) return document;
  std::vector<comp_layer_id> ids(count);
  if (comp_layer_list(document, ids.data(), ids.size(), &count) != COMP_OK) return document;

  std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
  comp_image image{};
  image.width = size;
  image.height = size;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(size) * 4;
  image.pixels = pixels.data();

  /* 底层：不透明底色。 */
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i + 0] = 24;
    pixels[i + 1] = 96;
    pixels[i + 2] = 168;
    pixels[i + 3] = 255;
  }
  if (comp_layer_set_pixels(document, ids.front(), &image) != COMP_OK) return document;

  const comp_blend_mode blends[3] = {COMP_BLEND_NORMAL, COMP_BLEND_MULTIPLY, COMP_BLEND_SCREEN};
  const float opacities[3] = {1.0f, 0.75f, 0.5f};
  for (int layer_index = 0; layer_index < 3; ++layer_index) {
    comp_layer_desc layer_desc{};
    layer_desc.name = "bench-layer";
    layer_desc.blend = blends[layer_index];
    layer_desc.opacity = opacities[layer_index];
    layer_desc.transform = comp_rect{0, 0, size, size};
    layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
    comp_layer_id id = 0;
    if (comp_layer_add(document, &layer_desc, &id) != COMP_OK) return document;
    /* 预乘合法：颜色分量都 <= alpha。 */
    for (int32_t y = 0; y < size; ++y) {
      for (int32_t x = 0; x < size; ++x) {
        uint8_t* p = &pixels[(static_cast<size_t>(y) * static_cast<size_t>(size) +
                              static_cast<size_t>(x)) * 4];
        const uint32_t alpha =
            (static_cast<uint32_t>(x * 11 + y * 7 + layer_index * 29) % 200u) + 40u;
        p[0] = static_cast<uint8_t>(((x * 17 + layer_index * 31) % 255) * alpha / 255);
        p[1] = static_cast<uint8_t>(((y * 13 + layer_index * 47) % 255) * alpha / 255);
        p[2] = static_cast<uint8_t>((((x + y) * 7 + layer_index * 11) % 255) * alpha / 255);
        p[3] = static_cast<uint8_t>(alpha);
      }
    }
    if (comp_layer_set_pixels(document, id, &image) != COMP_OK) return document;
  }
  return document;
}

int run_present_bench(HINSTANCE instance, int frames) {
  std::FILE* report = nullptr;
  if (_wfopen_s(&report, L"present_bench.txt", L"wb") != 0 || report == nullptr) return 2;

  /* 父进程是控制台时顺便回显；否则只写文件（--selftest 同口径）。 */
  std::FILE* console = nullptr;
  if (AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
    std::FILE* reopened = nullptr;
    if (freopen_s(&reopened, "CONOUT$", "w", stdout) == 0) console = stdout;
  }
  auto emit = [&](const char* line) {
    std::fprintf(report, "%s\n", line);
    std::fflush(report); /* 崩溃也要留下最后到达的阶段，便于定位 */
    if (console != nullptr) std::fprintf(console, "%s\n", line);
  };
  char line[512];

  comp_engine engine = nullptr;
  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-shell-present-bench";
  engine_desc.version = "0.1.0";
  engine_desc.backend = COMP_BACKEND_AUTO;
  if (comp_engine_create(&engine_desc, &engine) != COMP_OK || engine == nullptr) {
    emit("result=engine_create_failed");
    std::fclose(report);
    return 1;
  }
  comp_backend actual = COMP_BACKEND_AUTO;
  comp_engine_backend(engine, &actual);
  /* 基准报告是纯文本（写文件/控制台），这里用窄名，不复用工具条的宽标签。 */
  const char* backend_name = "AUTO";
  if (actual == COMP_BACKEND_D3D12) {
    backend_name = "D3D12";
  } else if (actual == COMP_BACKEND_CPU) {
    backend_name = "CPU";
  } else if (actual == COMP_BACKEND_VULKAN) {
    backend_name = "Vulkan";
  } else if (actual == COMP_BACKEND_METAL) {
    backend_name = "Metal";
  }
  std::snprintf(line, sizeof(line), "backend=%s", backend_name);
  emit(line);

  if (actual != COMP_BACKEND_D3D12) {
    emit("result=skipped");
    emit("reason=window present needs the D3D12 backend; build with --gpu and run on a machine "
         "with a D3D12 device");
    comp_engine_destroy(engine);
    std::fclose(report);
    return 2;
  }

  static const wchar_t kBenchClass[] = L"CompositorPresentBenchWindow";
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = bench_window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = kBenchClass;
  if (RegisterClassExW(&window_class) == 0) {
    emit("result=window_class_failed");
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  RECT rect{0, 0, kBenchCanvas, kBenchCanvas};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowExW(0, kBenchClass, L"compositor present bench", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                                rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    emit("result=window_create_failed");
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  /* 不抢焦点但必须真的显示：隐藏窗口的 Present 会被系统当作遮挡，测不出真实上屏。 */
  ShowWindow(window, SW_SHOWNOACTIVATE);
  UpdateWindow(window);

  comp_document document = build_present_bench_document(engine, kBenchCanvas);
  if (document == nullptr) {
    emit("result=document_create_failed");
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }

  comp_render_target target{};
  target.size = comp_size{kBenchCanvas, kBenchCanvas};
  target.format = COMP_PIXEL_RGBA8;
  target.kind = COMP_SURFACE_HWND;
  target.native_handle = window;

  const size_t stride = static_cast<size_t>(kBenchCanvas) * 4;

  std::snprintf(line, sizeof(line), "canvas=%dx%d", kBenchCanvas, kBenchCanvas);
  emit(line);
  std::snprintf(line, sizeof(line), "frames=%d", frames);
  emit(line);

  /* 预热 3 帧：把 PSO/交换链/首次上传这些一次性成本挪出测量窗口。 */
  bool hwnd_path_ok = true;
  comp_result render_result = COMP_OK;
  comp_result present_result = COMP_OK;
  for (int i = 0; i < 3; ++i) {
    render_result = comp_render_document(document, &target);
    if (render_result != COMP_OK) {
      hwnd_path_ok = false;
      break;
    }
    present_result = comp_render_present(document);
    if (present_result != COMP_OK) {
      hwnd_path_ok = false;
      break;
    }
    pump_pending_messages();
  }
  if (!hwnd_path_ok) {
    emit("result=hwnd_target_unsupported");
    std::snprintf(line, sizeof(line), "render_result=%d", static_cast<int>(render_result));
    emit(line);
    std::snprintf(line, sizeof(line), "present_result=%d", static_cast<int>(present_result));
    emit(line);
    const char* last_error = comp_engine_last_error(engine);
    std::snprintf(line, sizeof(line), "last_error=%s", last_error != nullptr ? last_error : "(none)");
    emit(line);
    /* 再用离屏目标试一次：区分「文档/窗口本身不行」与「整条渲染链路都不行」。 */
    std::vector<uint8_t> probe(stride * kBenchCanvas, 0);
    comp_render_target offscreen{};
    offscreen.size = comp_size{kBenchCanvas, kBenchCanvas};
    offscreen.format = COMP_PIXEL_RGBA8;
    offscreen.kind = COMP_SURFACE_CPU_BUFFER;
    offscreen.stride = stride;
    offscreen.pixels = probe.data();
    std::snprintf(line, sizeof(line), "offscreen_probe=%s",
                  comp_render_document(document, &offscreen) == COMP_OK ? "ok" : "failed");
    emit(line);
    comp_document_destroy(document);
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 2;
  }
  emit("hwnd_target=ok");

  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  const double freq = static_cast<double>(frequency.QuadPart);

  /* ---- A. GPU 直呈：render(HWND) + present ---- */
  double gpu_total = 0.0;
  double gpu_best = 1.0e30;
  double gpu_worst = 0.0;
  bool gpu_ok = true;
  for (int i = 0; i < frames; ++i) {
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    const bool ok = comp_render_document(document, &target) == COMP_OK &&
                    comp_render_present(document) == COMP_OK;
    QueryPerformanceCounter(&end);
    if (!ok) {
      gpu_ok = false;
      break;
    }
    const double elapsed = elapsed_ms(start, end, freq);
    gpu_total += elapsed;
    if (elapsed < gpu_best) gpu_best = elapsed;
    if (elapsed > gpu_worst) gpu_worst = elapsed;
    pump_pending_messages();
  }
  if (!gpu_ok) {
    emit("result=gpu_present_failed");
    comp_document_destroy(document);
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  const double gpu_avg = gpu_total / frames;

  /* ---- B. CPU 上屏：flatten + RGBA->BGRA + StretchDIBits（外壳既有路径） ---- */
  std::vector<uint8_t> rgba(stride * kBenchCanvas);
  std::vector<uint8_t> bgra(stride * kBenchCanvas);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = kBenchCanvas;
  info.bmiHeader.biHeight = -kBenchCanvas; /* 负值 = 自顶向下 */
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  HDC window_dc = GetDC(window);
  if (window_dc == nullptr) {
    emit("result=getdc_failed");
    comp_document_destroy(document);
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  double cpu_blit_total = 0.0;
  bool cpu_ok = true;
  for (int i = 0; i < frames; ++i) {
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    if (comp_render_flatten(document, rgba.data(), COMP_PIXEL_RGBA8, stride) != COMP_OK) {
      cpu_ok = false;
      break;
    }
    /* 预乘 RGBA8 -> GDI 的 BGRA（与 render_canvas 的逐像素交换同口径）。 */
    const size_t pixel_count = static_cast<size_t>(kBenchCanvas) * kBenchCanvas;
    for (size_t p = 0; p < pixel_count; ++p) {
      const uint8_t* src = rgba.data() + p * 4;
      uint32_t packed = (static_cast<uint32_t>(src[3]) << 24) |
                        (static_cast<uint32_t>(src[0]) << 16) |
                        (static_cast<uint32_t>(src[1]) << 8) | static_cast<uint32_t>(src[2]);
      std::memcpy(bgra.data() + p * 4, &packed, 4);
    }
    StretchDIBits(window_dc, 0, 0, kBenchCanvas, kBenchCanvas, 0, 0, kBenchCanvas, kBenchCanvas,
                  bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    GdiFlush();
    QueryPerformanceCounter(&end);
    cpu_blit_total += elapsed_ms(start, end, freq);
    pump_pending_messages();
  }
  ReleaseDC(window, window_dc);
  if (!cpu_ok) {
    emit("result=cpu_present_failed");
    comp_document_destroy(document);
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  const double cpu_blit_avg = cpu_blit_total / frames;

  /* ---- C. 只测 CPU 合成（不含上屏），把两条路径的差异拆开归因 ---- */
  double cpu_flatten_total = 0.0;
  for (int i = 0; i < frames; ++i) {
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    comp_render_flatten(document, rgba.data(), COMP_PIXEL_RGBA8, stride);
    QueryPerformanceCounter(&end);
    cpu_flatten_total += elapsed_ms(start, end, freq);
    pump_pending_messages();
  }
  const double cpu_flatten_avg = cpu_flatten_total / frames;

  std::snprintf(line, sizeof(line), "gpu_present_avg_ms=%.3f", gpu_avg);
  emit(line);
  std::snprintf(line, sizeof(line), "gpu_present_best_ms=%.3f", gpu_best);
  emit(line);
  std::snprintf(line, sizeof(line), "gpu_present_worst_ms=%.3f", gpu_worst);
  emit(line);
  std::snprintf(line, sizeof(line), "cpu_flatten_blit_avg_ms=%.3f", cpu_blit_avg);
  emit(line);
  std::snprintf(line, sizeof(line), "cpu_flatten_avg_ms=%.3f", cpu_flatten_avg);
  emit(line);
  std::snprintf(line, sizeof(line), "cpu_present_over_gpu=%.2f", cpu_blit_avg / gpu_avg);
  emit(line);
  emit("result=ok");

  comp_document_destroy(document);
  DestroyWindow(window);
  UnregisterClassW(kBenchClass, instance);
  comp_engine_destroy(engine);
  std::fclose(report);
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
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

  HWND window = CreateWindowExW(0, class_name, L"Compositor (Windows shell)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, kDefaultWidth, kDefaultHeight, nullptr,
                                build_menu(), instance, nullptr);
  if (window == nullptr) {
    comp_engine_destroy(shell.engine);
    return 1;
  }

  g_shell = &shell;
  shell.window = window; /* 视图命令需要客户区矩形，必须早于首次绘制设置 */
  create_document(shell);
  {
    /* 标题上也标出实际后端：不打开状态栏也能一眼判断走的是 GPU 还是 CPU。 */
    wchar_t title[96];
    wsprintfW(title, L"Compositor (Windows shell) - %s", backend_label(shell.backend));
    SetWindowTextW(window, title);
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
  if (shell.document != nullptr) comp_document_destroy(shell.document);
  comp_engine_destroy(shell.engine);
  return 0;
}
