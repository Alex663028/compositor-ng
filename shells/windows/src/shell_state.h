// AI生成
/*
 * shell_state.h — Windows 外壳层的共享状态定义（M3 拆分第一步）。
 *
 * 桌面外壳原先是一个 1.7 万行的 main.cpp：常量、面板状态、文档状态与全部命令
 * 混在一个匿名命名空间里，任何一处改动都要全量重编，多文档更是无从谈起。
 * 这里把「壳的状态」提到头文件，并把它切成两层：
 *
 *   DocumentContext : 单个文档的全部运行态（引擎文档句柄、图层镜像、选区、
 *                     面板模态、拖拽中间态、视图）。**换文档 = 换一份它**，
 *                     这正是多文档 / 面板停靠的前提。
 *   ShellState      : 应用会话态（引擎、工具选择、笔刷参数、面板宽度、窗口）。
 *                     跨文档保留，不随文档切换复位。
 *
 * 划分口径只有一条：**切文档之后必须重建或复位的，放进 DocumentContext**；
 * 其余留在 ShellState。字段文本与原 main.cpp 逐字一致（含注释），本轮只是
 * 搬家，没有任何行为变化。
 *
 * 约束不变：外壳只依赖 abi/include，不包含 engine/ 内部头。
 */
#ifndef COMPOSITOR_SHELL_STATE_H
#define COMPOSITOR_SHELL_STATE_H

#include <windows.h>

#include <cstddef>
#include <cstdint>
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

namespace comp_shell {

/* ===== 常量与命令 id（原 main.cpp 顶部，逐字搬入） ===== */
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;

/* 蚂蚁线：虚线 4 px / 间隔 4 px，每 120 ms 前进 1 px（一个周期 8 px）。 */
constexpr UINT_PTR kAntsTimerId = 1;
constexpr UINT kAntsTimerPeriodMs = 120;
/*
 * 字体目录扫描：字体文件会有上百个、单个几十 MB，一次性同步扫完会冻住 UI
 * （实测 153 个文件 1.2 s）。所以扫描按 WM_TIMER 分片推进，每片只探测几支字体；
 * 面板在扫描期间照常可开、可滚、可用已探测到的条目。
 */
constexpr UINT_PTR kFontScanTimerId = 2;
constexpr UINT kFontScanPeriodMs = 30;
constexpr int kFontScanPerTick = 6;   /* 每片探测的文件数（约 50 ms） */
constexpr int kFontScanMaxFaces = 8;  /* .ttc 集合最多探测的面数 */
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
 * S22 命令 id 段（1400 起，避开 1001..1046 单体、1100..1116 工具、1200..1222 滤镜、
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
/* §5.43 面板整块收起（停靠最小版）：F7 = 图层面板、F8 = 调整面板（PS 同键位）。 */
constexpr UINT kCmdPanelLayers = 1535;
constexpr UINT kCmdPanelAdjust = 1536;
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
/* P0-5：边界/平滑复用一个键入选区半径的输入态（op 决定量程 1..500 / 1..100）。 */
constexpr UINT kCmdSelectBorder = 1058;
constexpr UINT kCmdSelectSmooth = 1059;
constexpr UINT kCmdSelectLoadPixels = 1049;
/* P0-4 第二批：色彩范围面板（打开即进预览事务）。 */
constexpr UINT kCmdSelectColorRange = 1050;
constexpr UINT kCmdSelectPaths = 1051; /* P0-4 可选：路径面板（Ctrl+Shift+P） */
constexpr UINT kCmdHistoryPanel = 1052; /* P1：历史记录面板（Ctrl+Alt+H） */
/*
 * P1 图像菜单（1053 段）：转画布 / 修边 / 反相。三项的 ABI 自 §6.35 就在
 * （comp_document_rotate_canvas / comp_document_trim / comp_layer_invert），
 * 壳此前一条调用点都没有——这里只补入口，不动引擎口径。
 */
constexpr UINT kCmdImageRotateCW = 1053;  /* 画布顺时针 90° */
constexpr UINT kCmdImageRotateCCW = 1054; /* 画布逆时针 90° */
constexpr UINT kCmdImageRotate180 = 1055; /* 画布 180° */
constexpr UINT kCmdImageTrim = 1056;      /* 修边（模态面板，回车应用） */
constexpr UINT kCmdImageInvert = 1057;    /* 图层反相（Ctrl+I） */
/*
 * P0 文档级对话框（1560 段，空号段，不挤占工具/滤镜/混合模式的连续 id）。
 * 四条入口此前壳里根本没有：新建只有固定的 1280×720、没有分辨率与背景，
 * 图像大小 / 画布大小完全没有，历史容量只能靠改 .comp。全走既有 ABI，
 * 不新增导出（ABI 仍为 145）。
 */
constexpr UINT kCmdNewDocumentDialog = 1560; /* 文件 > 新建文档…（Ctrl+Alt+N） */
constexpr UINT kCmdSaveAs = 1561;            /* 文件 > 另存为…（Ctrl+Alt+S） */
constexpr UINT kCmdImageSize = 1562;         /* 图像 > 图像大小…（Ctrl+Alt+I） */
constexpr UINT kCmdCanvasSize = 1563;        /* 图像 > 画布大小…（Ctrl+Alt+C） */
constexpr UINT kCmdHistorySettings = 1564;   /* 编辑 > 历史记录容量… */
/*
 * P1-5 Camera Raw：引擎侧的三个导出（表加载 / 是否已加载 / 应用）自 §6.36
 * 就在，外壳 0 处调用——763 张实测表与整条管线用户完全拿不到。这条入口
 * 只是把已有能力接到菜单上，不新增 ABI。
 */
constexpr UINT kCmdCameraRaw = 1565; /* 滤镜 > Camera Raw 滤镜…（Ctrl+Alt+R） */
/*
 * P0-4 剪贴板：编辑菜单此前只有撤销/重做/历史，拷贝粘贴在壳里根本不存在
 * （Ctrl+C 被当成裁剪工具快捷键）。应用内往返走私有格式，跨应用走 CF_DIBV5。
 */
constexpr UINT kCmdEditCopy = 1566;       /* 编辑 > 拷贝（Ctrl+C） */
constexpr UINT kCmdEditCopyMerged = 1567; /* 编辑 > 合并拷贝（Ctrl+Shift+C） */
constexpr UINT kCmdEditCut = 1568;        /* 编辑 > 剪切（Ctrl+X） */
constexpr UINT kCmdEditPaste = 1569;      /* 编辑 > 粘贴（Ctrl+V） */
constexpr UINT kCmdEditPasteInto = 1570;  /* 编辑 > 贴入选区（Ctrl+Alt+V） */
constexpr UINT kCmdEditClear = 1571;      /* 编辑 > 清除（Backspace） */
/*
 * P0-6 ①：可编辑文字图层。文字工具提交后选中的是一层「文字对象 + 渲染缓存」，
 * 要再画像素必须先显式栅格化（与上游同一规矩）。入口在图层菜单。
 */
constexpr UINT kCmdLayerRasterizeText = 1572; /* 图层 > 栅格化文字 */
/*
 * P0-6 ②：字体枚举与选择器。ABI 零新增——`comp_font_load_file` 本来就能加载
 * 任意 TTF/OTF/TTC，`comp_font_get_metrics` 给 face_count，`comp_font_name`
 * 给族名/样式名。缺的只是「把系统里的字体列出来让用户挑」这条产品通路。
 */
constexpr UINT kCmdLayerFonts = 1573; /* 图层 > 字体… */
constexpr UINT kCmdToolBase = 1100; /* + 工具序号，范围 1100..1118 */
constexpr UINT kCmdFilterBase = 1200; /* + 滤镜序号 0..10（1200..1210） */
/*
 * 面板动作号落在 1220 段：滤镜档位从 9 个涨到 11 个（尾追加 Sharpen /
 * Reduce Noise）后，1200..1210 已被档位占满，原先放在 1210..1212 的
 * 应用/重置/关闭会与 Sharpen 抢同一个 id（菜单项永远打不开锐化）。
 * 1220 起给档位留下 1211..1219 的余量，后续再加滤镜不需要再搬家。
 */
constexpr UINT kCmdFilterApply = 1220;
constexpr UINT kCmdFilterReset = 1221;
constexpr UINT kCmdFilterClose = 1222;

/* ===== 面板行 / 键入态 ===== */
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

/* 整组移动的成员快照（原 ShellState::GroupMemberMove）。 */
  struct GroupMemberMove {
    comp_layer_id id = 0;
    comp_rect base{};
    float rotation = 0.0f;
  };

/* 已存路径（原 ShellState::StoredPath）。 */
  struct StoredPath {
    std::wstring name;
    std::vector<comp_ui::PenAnchor> anchors;
  };

/*
 * 字体目录里的一项 = 一个**可加载的面**（.ttc 集合按面拆成多项）。
 * 三个标记各有用处：
 *   builtin —— index 0 的「5×7 点阵」轨道，path 为空，永远可用；
 *   shipped —— 随仓 Noto Sans SC（exe/resources/fonts），排最前；
 *   cjk     —— 该面含 U+4F60（comp_font_metrics.has_cjk），面板标 [CJK]。
 * 扫描期只探测、不缓存句柄：FontFace 没有全局缓存，句柄留着就是几十 MB 常驻。
 */
struct ShellFontEntry {
  std::wstring label;
  std::string path; /* UTF-8；builtin 为空串 */
  int32_t face_index = 0;
  bool builtin = false;
  bool shipped = false;
  bool cjk = false;
};

/* 待探测的候选文件（目录枚举阶段就定好顺序与去重）。 */
struct FontCandidate {
  std::wstring path_w;
  std::string path_utf8;
  bool shipped = false;
};

/*
 * 单个文档的运行态。构造/销毁不碰引擎——句柄的所有权仍在文档生命周期
 * 函数里（create_document / load_document / close），这里只是把字段归位，
 * 让「换文档」变成一次整体替换而不是逐个字段擦洗。
 */
struct DocumentContext {
  comp_document document = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint32_t> framebuffer;   /* BGRA，供 GDI 直接使用 */
  std::vector<uint8_t> rgba;           /* 引擎输出的预乘 RGBA8 */
  bool needs_render = true;
  std::vector<LayerRow> layers;
  std::vector<std::pair<comp_layer_id, uint8_t>> visibility; /* id -> visible */
  int32_t selected_layer = 0; /* 面板行索引（0 = 最上层显示行） */
  std::vector<int32_t> layer_display;
  std::vector<int32_t> layer_depth;
  std::vector<comp_layer_id> collapsed_groups;
  std::vector<comp_layer_id> extra_selection;
  comp_txn nudge_txn = nullptr;
  comp_layer_id nudge_anchor = 0;
  bool nudge_dirty = false;
  LayerInput layer_input = LayerInput::None;
  std::wstring layer_input_text;
  comp_layer_id layer_input_id = 0;
  std::vector<uint32_t> adj_kinds;       /* 选中图层已有调整的 kind（数组序） */
  std::vector<std::string> adj_settings; /* 与 adj_kinds 对齐的 settings JSON */
  int32_t selected_adjustment = -1;      /* 已有调整的选中索引；-1 = 未选（新增态） */
  int32_t adj_kind = 3;                  /* 当前编辑 kind，默认 3 = Exposure */
  int32_t adj_param = 0;                 /* 聚焦的可编辑参数序号 */
  std::vector<std::string> adj_values;   /* 与参数表对齐的 ASCII 编辑值 */
  std::wstring adj_input;                /* 键入缓冲；非空 = 输入态 */
  int32_t adj_drag_param = -1;
  comp_ui::Rect adj_drag_track{};
  comp_txn adj_drag_txn = nullptr;
  bool adj_drag_dirty = false;
  int32_t adj_scroll = 0;
  int32_t adj_channel = 0;
  int32_t curve_channel = 0;
  std::vector<double> curve_xy;
  int32_t curve_focus_point = -1;
  int32_t curve_drag_point = -1;
  comp_ui::Rect curve_drag_plot; /* 拖动期间冻结的绘图区（指针 -> 值换算用） */
  comp_txn curve_drag_txn = nullptr;
  bool curve_drag_dirty = false;
  comp_ui::CanvasView view;
  int history_brush_state = -1;
  bool clone_source_set = false;
  double clone_source_x = 0.0;
  double clone_source_y = 0.0;
  bool clone_has_offset = false;
  double clone_offset_x = 0.0;
  double clone_offset_y = 0.0;
  comp_ui::FilterPanel filter;
  bool filter_open = false;
  std::wstring filter_input; /* 滤镜面板的键入缓冲；非空 = 输入态 */
  bool filter_preview_pending = false;
  bool progressive_axis_set = false;
  bool progressive_axis_dragging = false;
  comp_ui::Point progressive_axis_start{};
  comp_ui::Point progressive_axis_final{};   /* 已提交的终点 */
  comp_ui::Point progressive_axis_current{}; /* 拖拽中的终点（预览用） */
  comp_ui::TrimPanel trim;
  bool trim_open = false;
  std::wstring trim_input; /* 容差键入缓冲；非空 = 输入态 */
  comp_ui::DocDialogKind doc_dialog = comp_ui::DocDialogKind::None;
  int doc_dialog_focus = 0;
  int doc_dialog_width = kDefaultWidth;
  int doc_dialog_height = kDefaultHeight;
  int doc_dialog_dpi = 72;
  int doc_dialog_background = 1; /* 0 白 / 1 透明 / 2 前景色；缺省透明 = Ctrl+N 旧行为 */
  int doc_dialog_lock_aspect = 1;
  int doc_dialog_anchor = 4; /* 居中 */
  int doc_dialog_steps = 0;
  int doc_dialog_budget_mb = 0;
  int doc_dialog_close_action = comp_ui::kCloseActionSave;
  int doc_dialog_base_width = kDefaultWidth;
  int doc_dialog_base_height = kDefaultHeight;
  std::wstring doc_dialog_input;
  int camera_raw_values[comp_ui::kCameraRawRowCount] = {};
  std::string document_path;
  int document_dpi = 0;
  bool histogram_open = false;
  std::vector<double> histogram_bins;
  int32_t histogram_layer = 0; /* 取数时对应的图层 id；换层即重取 */
  comp_ui::HistogramView histogram_view;
  bool histogram_drag = false;
  int32_t histogram_drag_from_x = 0;
  int32_t histogram_drag_to_x = 0;
  int32_t layer_drag_row = -1;
  int32_t layer_drag_target = -1;
  int32_t layer_drag_press_y = 0;
  bool layer_drag_pending = false;
  bool layer_drag_active = false;
  std::vector<uint32_t> effect_kinds;
  bool effect_open = false;
  std::vector<std::string> effect_settings;
  int32_t effect_kind = 0;
  int32_t effect_param = 0;
  std::vector<std::string> effect_values;
  std::wstring effect_input;
  std::string effect_base_json;
  comp_ui::StrokeKind stroke_kind = comp_ui::StrokeKind::None;
  std::vector<float> stroke_points;
  bool stroke_erase = false;
  bool move_active = false;
  comp_ui::Point move_start{};
  comp_ui::Point move_current{};
  bool move_duplicate = false;
  bool layer_move_active = false;
  bool layer_move_dirty = false;
  comp_document layer_move_doc = nullptr;
  comp_txn layer_move_txn = nullptr;
  comp_layer_id layer_move_id = 0;
  comp_rect layer_move_base{};
  float layer_move_rotation = 0.0f;
  comp_ui::Point layer_move_start{};
  int32_t layer_move_applied_dx = 0;
  int32_t layer_move_applied_dy = 0;
  bool layer_move_alt = false;
  bool layer_move_duplicated = false;
  std::vector<GroupMemberMove> layer_move_members;
  bool layer_move_is_group = false;
  bool layer_move_multi = false;
  std::vector<comp_layer_id> layer_move_ids;
  bool layer_move_copy_failed = false;
  bool layer_handle_active = false;
  int32_t layer_handle_kind = comp_ui::kTransformHandleNone; /* 0..7 缩放，8 旋转 */
  comp_layer_id layer_handle_id = 0;
  comp_rect layer_handle_base{};
  float layer_handle_base_rotation = 0.0f;
  comp_ui::Point layer_handle_press{};
  comp_txn layer_handle_txn = nullptr;
  comp_document layer_handle_doc = nullptr;
  bool layer_handle_dirty = false;
  comp_rect layer_handle_current{};
  float layer_handle_current_rotation = 0.0f;
  bool eyedropper_drag = false;
  bool selection_active = false;
  std::vector<uint8_t> selection_mask;
  std::vector<comp_ui::BoundarySegment> selection_segments;
  int32_t selection_phase = 0;    /* 蚂蚁线相位 0..7 px */
  bool selection_truncated = false; /* 线段数超过缓冲上限（如实提示） */
  comp_ui::Rect selection_base{};
  int selection_handle_drag = -1;           /* -1 = 未拖；0..7 = 手柄序号 */
  comp_ui::Rect selection_handle_source{};  /* 按下瞬间的紧包围盒（拖拽基准） */
  comp_ui::Rect selection_handle_current{}; /* 拖拽预览落点（文档坐标） */
  bool marquee_active = false;
  comp_ui::Point marquee_start{};
  comp_ui::Point marquee_current{};
  bool zoom_marquee_active = false;
  POINT zoom_marquee_start{};
  POINT zoom_marquee_current{};
  bool crop_active = false;
  bool crop_dragging = false;
  comp_crop_rect crop_rect{};
  comp_ui::Point crop_drag_start{};
  bool lasso_active = false;
  std::vector<float> lasso_points;
  bool gradient_active = false;
  comp_ui::Point gradient_start{};
  comp_ui::Point gradient_current{};
  bool shape_active = false;
  comp_ui::Point shape_start{};
  comp_ui::Point shape_current{};
  bool type_active = false; /* 已落插入点，正在键入/等待提交 */
  comp_ui::Point type_origin{}; /* 插入点（文档像素，文本框左上角） */
  std::string type_text;        /* UTF-8 键入缓冲，'\n' = 换行（Shift+Enter） */
  double type_font_size = comp_ui::kDefaultTypeFontSize;
  double type_line_height = comp_ui::kDefaultTypeLineHeight;
  double type_letter_spacing = 0.0;
  wchar_t type_pending_high = 0;
  bool type_skip_char = false;
  std::wstring type_input;
  std::wstring feather_input; /* 羽化键入缓冲 */
  bool feather_input_active = false;
  comp_selection_modify_op modify_op = COMP_SELECTION_EXPAND;
  std::wstring modify_input; /* 扩展/收缩/边界/平滑半径键入缓冲 */
  bool modify_input_active = false;
  bool color_range_open = false;
  comp_txn color_range_txn = nullptr;
  std::vector<uint8_t> color_range_include;
  std::vector<uint8_t> color_range_exclude;
  int color_range_fuzziness = comp_ui::kDefaultColorRangeFuzziness;
  int color_range_invert = 0;
  std::wstring color_range_input; /* 容差键入缓冲；非空 = 输入态 */
  std::vector<comp_ui::PenAnchor> pen_anchors;
  bool pen_dragging = false;
  bool path_open = false;
  std::vector<StoredPath> paths;
  int path_selected = 0;
  int path_scroll = 0;
  int path_serial = 0; /* 命名用流水号（删除后不复用，避免重名） */
  bool history_open = false;
  int history_scroll = 0;
  bool needs_fit = true; /* 新文档/打开文档后，下一次绘制时适配窗口 */
};

/*
 * 应用会话态：引擎、工具选择、笔刷/面板等跨文档保留的偏好，以及窗口句柄。
 * doc 目前只有一个实例；多文档落地时这里换成「文档标签页 -> DocumentContext」，
 * 本结构体其余字段保持不动。
 */
struct ShellState {
  comp_engine engine = nullptr;
  comp_backend backend = COMP_BACKEND_AUTO;
  std::wstring status;
  comp_ui::ToolId tool = comp_ui::ToolId::Select;
  double brush_size = 24.0;
  double brush_hardness = 0.75;
  int retouch_mode = comp_ui::kRetouchBlur;
  double blur_radius = 8.0;
  double retouch_strength = 0.5; /* Smudge / Liquify 的 warp.strength */
  int tone_state = comp_ui::kToneDodgeMidtone;
  double tone_strength = 0.5; /* comp_tool_tone.strength（Exposure / Flow） */
  double history_brush_opacity = 1.0;
  bool clone_sample_all = false;
  double clone_opacity = 1.0;
  bool clone_aligned = true;
  int heal_mode = comp_ui::kHealContentAware;
  double heal_opacity = 1.0;
  int new_doc_width = kDefaultWidth;
  int new_doc_height = kDefaultHeight;
  int new_doc_dpi = 72;
  int new_doc_background = 1;
  bool camera_raw_tables_loaded = false;
  std::wstring window_title_cache;
  comp_ui::ColorRGB foreground{};
  int32_t palette_index = 0; /* X 键循环到色板的哪一项 */
  std::wstring color_input;  /* Shift+X 的十六进制键入；非空 = 输入态 */
  int layer_panel_width = comp_ui::Layout::kLayerPanelWidth;
  int adjust_panel_width = comp_ui::Layout::kAdjustPanelWidth;
  int panel_resize_target = 0;
  int panel_resize_base_width = 0;
  int panel_resize_start_x = 0;
  int panel_splitter_hover = 0;
  bool layer_panel_collapsed = false;
  bool adjust_panel_collapsed = false;
  int panel_toggle_hover = 0;
  int marquee_shape = comp_ui::kMarqueeRect;
  int wand_tolerance = comp_ui::kDefaultWandTolerance;
  bool wand_contiguous = true;
  bool wand_merged = false;
  int wand_radius = comp_ui::kDefaultWandRadius;
  int gradient_type = comp_ui::kGradientLinear;
  int shape_kind = comp_ui::kShapeRect;
  double shape_stroke_width = comp_ui::kDefaultShapeStrokeWidth;
  double shape_corner_radius = comp_ui::kDefaultShapeCornerRadius;
  comp_font type_font = nullptr;
  bool type_use_font = false; /* 面板第 4 行的开关；没加载成功时永远为 false */
  double type_font_ascender = 0.0;  /* em 比例，来自 comp_font_metrics */
  double type_font_descender = 0.0; /* em 比例，通常为负 */
  std::wstring type_font_label;     /* 面板显示名（"Noto Sans SC Subset" / 未加载） */
  /* 字体文件路径（UTF-8）：可编辑文字图层把它写进 .comp，重开时原样还原。
   * 走内置 5×7 点阵轨道时为空串，comp_text_desc.font_path_utf8 传 nullptr。 */
  std::string type_font_path;
  int type_panel_focused = comp_ui::kTypePanelFontSizeRow;
  /*
   * 字体面板（P0-6②）。目录是**应用会话态**：扫描一次，跨文档复用；
   * 选择/滚动/过滤词同理，换文档不重置。条目的 usable 判断在探测阶段做完，
   * 面板只按 label 过滤——扫描结果不再重算。
   */
  std::vector<ShellFontEntry> font_entries; /* [0] 恒为内置 5×7 点阵 */
  std::vector<int> font_filtered;           /* 过滤后的 font_entries 下标 */
  std::vector<FontCandidate> font_scan_queue;
  size_t font_scan_cursor = 0;
  bool font_scan_started = false;
  bool font_scan_done = false;
  int font_scan_skipped = 0; /* 探不出族名/不可缩放的候选数 */
  bool font_open = false;
  int font_selected = 0; /* 指向 font_filtered 的行号 */
  int font_scroll = 0;
  std::wstring font_filter; /* 面板第 3 行的过滤词（空 = 不过滤） */
  int type_face_index = 0;  /* comp_text_desc.font_face_index：.ttc 里的面号 */
  int selection_feather = comp_ui::kMinSelectionFeather;
  int selection_grow = comp_ui::kMinSelectionGrow;
  bool cursor_valid = false;
  bool tracking_leave = false;
  int cursor_x = 0;
  int cursor_y = 0;
  bool space_down = false; /* 按住空格 = 临时抓手 */
  bool panning = false;
  POINT pan_anchor{};
  HWND window = nullptr;

  /* 当前活动文档的上下文（本轮先内嵌一份，切换文档即整体替换）。 */
  DocumentContext doc;
};

extern ShellState* g_shell; /* 单窗口外壳，生命周期与窗口一致 */

}  // namespace comp_shell

#endif  // COMPOSITOR_SHELL_STATE_H
