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

#include "ui_core.h"
#include "shell_dock.h" /* 平台无关的视图变换 / 布局 / 命中 / 工具表 */

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
constexpr UINT kCmdEffectBase = 1510;          /* S24：+ 效果档位 0..9，菜单勾选 = 已启用 */
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
/* P0-5 收尾：扩大选取 / 选取相似。容差直接取壳上的魔棒容差（shift+/ 调），
 * 点菜单即落墨、一步撤销——两条都是「按颜色长选区」，没有可调面板。 */
constexpr UINT kCmdSelectGrow = 1060;
constexpr UINT kCmdSelectSimilar = 1061;
/* P0-5 收尾：变换选区（宽 / 高 / 角度数值输入；手柄只做轴对齐缩放）。 */
constexpr UINT kCmdSelectTransform = 1062;
/* P1-3：数值变换 / 再次变换。前者是右栏三行面板（Ctrl+T），后者复读上一次
 * 的比例与角度增量（Ctrl+Shift+T）——两者都只调 comp_layer_set_transform，
 * 零 ABI 新增；再次变换的「上一次」按文档记录，换文档即失效。 */
constexpr UINT kCmdLayerTransformNumeric = 1063;
constexpr UINT kCmdLayerTransformAgain = 1064;
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
 * ROI #8 图像 > 调整（1590 段，空号段）：PS 的 Image > Adjustments 是**破坏性**
 * 一次性落墨——与「调整」面板里的可编辑调整层是两个对象，引擎侧对应
 * comp_layer_apply_adjustment（ABI 尾部第 164 项）。
 *
 * 命令 id = kCmdImageAdjustBase + kind，只给「逐像素可破坏」的 kind 发号
 * （见 kImageAdjustKinds）；无参数的 Desaturate 点菜单即落墨，其余进破坏性
 * 面板态：调参 → Ctrl+Shift+U 应用 → Esc 退出。
 */
constexpr UINT kCmdImageAdjustBase = 1590;
constexpr uint32_t kImageAdjustKinds[] = {
    COMP_ADJUSTMENT_HUE_SATURATION, COMP_ADJUSTMENT_LEVELS,  COMP_ADJUSTMENT_CURVES,
    COMP_ADJUSTMENT_EXPOSURE,       COMP_ADJUSTMENT_GRADIENT_MAP,
    COMP_ADJUSTMENT_BLACK_WHITE,    COMP_ADJUSTMENT_COLOR_BALANCE,
    COMP_ADJUSTMENT_BRIGHTNESS_CONTRAST, COMP_ADJUSTMENT_DESATURATE,
    COMP_ADJUSTMENT_THRESHOLD,      COMP_ADJUSTMENT_POSTERIZE,
    COMP_ADJUSTMENT_VIBRANCE,       COMP_ADJUSTMENT_SELECTIVE_COLOR,
    COMP_ADJUSTMENT_PHOTO_FILTER,   COMP_ADJUSTMENT_CHANNEL_MIXER,
    COMP_ADJUSTMENT_COLOR_LOOKUP,   COMP_ADJUSTMENT_SHADOWS_HIGHLIGHTS,
    COMP_ADJUSTMENT_AUTO_TONE,      COMP_ADJUSTMENT_AUTO_CONTRAST,
    COMP_ADJUSTMENT_AUTO_COLOR,
};
constexpr size_t kImageAdjustKindCount = sizeof(kImageAdjustKinds) / sizeof(kImageAdjustKinds[0]);
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
/*
 * ROI #3 视图开关（1580 段，空号段）：标尺 / 参考线 / 网格 / 吸附 / 清除参考线。
 * 键位与上游同义（Ctrl+R / Ctrl+; / Ctrl+' / Ctrl+Shift+;）。壳层零新增 ABI——
 * 参考线的读写走同批尾部追加的 6 个 comp_guide_* 导出（152 → 158）。
 */
constexpr UINT kCmdViewRulers = 1580;      /* 视图 > 标尺（Ctrl+R） */
constexpr UINT kCmdViewGuides = 1581;      /* 视图 > 参考线（Ctrl+;） */
constexpr UINT kCmdViewGrid = 1582;        /* 视图 > 网格（Ctrl+'） */
constexpr UINT kCmdViewSnap = 1583;        /* 视图 > 对齐（Ctrl+Shift+;） */
constexpr UINT kCmdViewClearGuides = 1584; /* 视图 > 清除参考线 */
/*
 * ROI #5 存储选区通道（1585 段）：PS 的 Alpha 通道 / Select > Save Selection。
 * 通道是文档状态（随 .comp 存取、随撤销回退），ABI 尾部追加了 5 个
 * comp_selection_channel_*（158 → 163）。面板与历史面板同款列表排版。
 */
constexpr UINT kCmdSelectChannels = 1585;    /* 选择 > 通道面板… */
constexpr UINT kCmdSelectSaveChannel = 1586; /* 选择 > 存储选区到通道… */
/*
 * P1-2 命令号（1660 段）：图层锁与颜色标签。**不能落在 1590..1653**——
 * 那是 kCmdImageAdjustBase + kind 的号段（kind 是 COMP_ADJUSTMENT_* 枚举值，
 * 已用到 24，即 1590..1614），早先按「1600 空段」发号时 1610..1614 正好压住
 * Color Lookup / Shadows-Highlights / 自动色调 / 自动对比度 / 自动颜色五项，
 * 菜单点下去变成给图层上锁（视觉探针 Auto Tone 一条当场报红）。1660 起
 * 在 64 宽宽容区间（1590..1653）之外，也不会被它的无条件 break 吞掉。
 *
 * 锁定用 5 个独立 id 而不是 kCmdLayerLockBase + 位号：菜单是 radio 组，
 * 「无」不是位组合能表达的槽位，混在一个基址里会让 0 号位与「透明」撞车。
 * 颜色标签用基址 + 序号（0..7）。
 */
constexpr UINT kCmdLayerLockNone = 1660;        /* 图层 > 锁定 > 无 */
constexpr UINT kCmdLayerLockTransparent = 1661; /* 图层 > 锁定 > 透明像素 */
constexpr UINT kCmdLayerLockPixels = 1662;      /* 图层 > 锁定 > 图像像素 */
constexpr UINT kCmdLayerLockPosition = 1663;    /* 图层 > 锁定 > 位置 */
constexpr UINT kCmdLayerLockAll = 1664;         /* 图层 > 锁定 > 全部 */
constexpr UINT kCmdLayerColorBase = 1665;       /* 图层 > 颜色标签 > 无/红/橙/黄/绿/蓝/紫/灰 */
/*
 * P1-2 第二批（1673 段，紧接颜色标签 1665..1672）：合并可见 / 反向排列。
 * 两者都是「一次点击改整篇结构」的命令，不需要面板，也没有可调参数。
 */
constexpr UINT kCmdLayerMergeVisible = 1673; /* 图层 > 合并可见（Ctrl+Alt+E） */
constexpr UINT kCmdLayerReverse = 1674;      /* 图层 > 反向排列 */
/* P1-2 第三批（1675）：链接图层开关（同组一起移动/缩放/旋转）。 */
constexpr UINT kCmdLayerLinkToggle = 1675;   /* 图层 > 链接图层 */
/*
 * P1-2 第四批（1676 段）：图层样式的拷贝 / 粘贴。
 * 上游的 Ctrl+Alt+C / Ctrl+Alt+V 在本壳已分别是「画布大小…」与「贴入选区」，
 * 所以这两条只做菜单入口，不抢既有按键（面板里会把旧命令的提示写清楚）。
 */
constexpr UINT kCmdLayerCopyStyle = 1676;  /* 图层 > 图层样式 > 拷贝图层样式 */
constexpr UINT kCmdLayerPasteStyle = 1677; /* 图层 > 图层样式 > 粘贴图层样式 */
/*
 * P1-2 收尾：图层面板过滤（1680 段）。过滤是**纯视图态**：不改文档、不进撤销、
 * 不落 .comp，只决定面板上列出哪些行。三种维度（类型 / 名称 / 颜色标签）可以
 * 叠加，菜单里的勾选态直接读这三个字段。
 *
 * 类型用基址 + 枚举（0 全部 / 1 像素 / 2 文字 / 3 组 / 4 调整 / 5 填充 / 6 智能）；
 * 颜色分「全部」「无标签」「红..灰」三段——PS 的「无颜色」是独立槽位，不是
 * 颜色枚举里的 0，混在基址里会与「全部」撞车。
 */
constexpr UINT kCmdLayerFilterKindBase = 1680;   /* +0..6 */
constexpr UINT kCmdLayerFilterColorAll = 1690;  /* 图层 > 过滤 > 颜色 > 全部 */
constexpr UINT kCmdLayerFilterColorNone = 1691; /* 图层 > 过滤 > 颜色 > 无标签 */
constexpr UINT kCmdLayerFilterColorBase = 1692; /* +0..6 = 红/橙/黄/绿/蓝/紫/灰 */
constexpr UINT kCmdLayerFilterName = 1699;      /* 图层 > 过滤 > 按名称… */
constexpr UINT kCmdLayerFilterClear = 1700;     /* 图层 > 过滤 > 清除过滤 */
/*
 * P1-2 收尾 ⑤：图层复合（1701 段）。
 *
 * 复合本身是**文档状态**（进 `.comp`、随撤销/重做），壳里只多一条视图态：
 * 「当前选中的复合 id」。菜单里没有展开的动态列表——面板/菜单每次都从引擎现读
 * （comp_layer_comp_count / _id_at / _name），壳不缓存名字，也就不会与撤销后的
 * 文档漂移。捕获需要一个名字，走与「重命名图层」同一套键入态（LayerInput）。
 */
constexpr UINT kCmdLayerCompCapture = 1701; /* 图层 > 图层复合 > 捕获为新复合… */
constexpr UINT kCmdLayerCompApply = 1702;   /* 应用当前复合 */
constexpr UINT kCmdLayerCompUpdate = 1703;  /* 用当前状态更新当前复合 */
constexpr UINT kCmdLayerCompDelete = 1704;  /* 删除当前复合 */
constexpr UINT kCmdLayerCompPrev = 1705;    /* 选择上一个复合 */
constexpr UINT kCmdLayerCompNext = 1706;    /* 选择下一个复合 */
constexpr UINT kCmdLayerCompRename = 1707;  /* 重命名当前复合… */
constexpr UINT kCmdLayerCompStatus = 1708;  /* 状态栏报当前复合（菜单项不用，快捷键用） */
/*
 * P1-2 收尾 ⑦：智能对象 / 智能滤镜（1710 段）。
 *
 * 「像素是 source + 滤镜栈的求值缓存」这件事由引擎持有；壳只暴露三个动作：
 * 转换、栅格化、清空滤镜栈。滤镜的**添加**不走这里——选中智能图层时
 * 「滤镜 > 应用」本身就是添加一条智能滤镜（见 shell_filter.cpp 的 apply_filter），
 * 与 PS 同一条路：同一个面板、同一次回车，普通层是破坏性应用，智能层是叠加。
 */
constexpr UINT kCmdLayerSmartConvert = 1710;   /* 图层 > 智能对象 > 转换为智能对象 */
constexpr UINT kCmdLayerSmartRasterize = 1711; /* 图层 > 智能对象 > 栅格化 */
constexpr UINT kCmdLayerSmartClear = 1712;     /* 图层 > 智能对象 > 清除全部智能滤镜 */
constexpr UINT kCmdLayerSmartStatus = 1713;    /* 状态栏报智能滤镜条数 */
/*
 * P1-2 收尾 ⑧：新建填充层（1714 段）。
 *
 * 纯色 / 渐变 / 图案三种 kind 共用一个参数面板（参数表由引擎的
 * comp_fill_kind_param_at 驱动），所以菜单只报三个「新建」，编辑参数与栅格化
 * 各一个动作。填充层的「参数是真源、像素是缓存」由引擎持有。
 */
constexpr UINT kCmdLayerFillSolid = 1714;     /* 图层 > 新建填充图层 > 纯色… */
constexpr UINT kCmdLayerFillGradient = 1715;  /* 图层 > 新建填充图层 > 渐变… */
constexpr UINT kCmdLayerFillPattern = 1716;   /* 图层 > 新建填充图层 > 图案… */
constexpr UINT kCmdLayerFillEdit = 1717;      /* 编辑当前填充层的参数 */
constexpr UINT kCmdLayerFillRasterize = 1718; /* 栅格化填充层 */
constexpr UINT kCmdLayerFillStatus = 1719;    /* 状态栏报填充层种类/参数 */
/*
 * P1-2 收尾 ⑨：智能滤镜栈面板（1720 段）。
 *
 * ⑦ 只把「叠加一条 / 整栈清除」接到 UI；单条参数编辑与单条开关当时只有引擎导出。
 * 这一段把 comp_layer_smart_filter_at / _set / _remove 接上：面板逐条列出滤镜栈，
 * 空格开关、回车改参数（滤镜面板进入「编辑该条」模式，回车走 _set）、Del 删除。
 * 命令号取自 1719 之后的空段，避开 1590..1653 的调整段与 1660..1719 的已用段。
 */
constexpr UINT kCmdLayerSmartPanel = 1720;        /* 图层 > 智能对象 > 智能滤镜面板… */
constexpr UINT kCmdLayerSmartPanelToggle = 1721;  /* 开/关当前条（不改参数） */
constexpr UINT kCmdLayerSmartPanelEdit = 1722;    /* 编辑当前条参数（滤镜面板） */
constexpr UINT kCmdLayerSmartPanelRemove = 1723;  /* 删除当前条 */
/*
 * P1-2 收尾 ⑩：栈序与展示名（1724..1726）。
 *
 * ⑨ 把逐条操作接上 UI 之后还剩两条能力边界：不能调序、不能命名。两者都是纯
 * 面板动作（引擎导出 comp_layer_smart_filter_move / _set_name / _name）。
 * 命令号接在 1723 之后，仍避开 1100/1200 段与已用的 1660..1723。
 */
constexpr UINT kCmdLayerSmartPanelRename = 1724;   /* 重命名当前条…（F2） */
constexpr UINT kCmdLayerSmartPanelMoveUp = 1725;   /* 当前条上移一位（Alt+↑） */
constexpr UINT kCmdLayerSmartPanelMoveDown = 1726; /* 当前条下移一位（Alt+↓） */

/* 文件 > 新建预设（P0-1 收尾，2026-10-26）：四个高频画布直接建，不进对话框。
 * 预设的语义是「马上要一张能画的白纸」，所以背景固定白；透明/前景色仍走
 * 「新建文档…」对话框。 */
constexpr UINT kCmdNewPresetHd = 1730;    /* 1920x1080 @72 */
constexpr UINT kCmdNewPresetA4 = 1731;    /* A4 300dpi：2480x3508 */
constexpr UINT kCmdNewPresetWeb = 1732;   /* 网页 1440x900 @72 */
constexpr UINT kCmdNewPresetPhoto = 1733; /* 照片 4000x3000 @300 */
/* P0-3 多文档 */
constexpr UINT kCmdCloseDocument = 1734; /* 文件 > 关闭文档（Ctrl+W） */
constexpr UINT kCmdNextTab = 1735;       /* 下一个文档（Ctrl+Tab） */
constexpr UINT kCmdPrevTab = 1736;       /* 上一个文档（Ctrl+Shift+Tab） */
constexpr UINT kCmdRecent = 1737;        /* 文件 > 最近打开…（Ctrl+Alt+O） */
/* P0-10 界面基线 */
constexpr UINT kCmdCursorStandard = 1738; /* 光标：标准 */
constexpr UINT kCmdCursorCross = 1739;    /* 光标：十字 */
constexpr UINT kCmdCursorHidden = 1740;   /* 光标：隐藏 */
constexpr UINT kCmdPanelDockRight = 1741; /* 调整面板停靠右侧（默认） */
constexpr UINT kCmdPanelDockLeft = 1742;  /* 调整面板停靠左侧 */
constexpr UINT kCmdFloatAdjust = 1743;    /* 浮动/停回 调整面板 */
constexpr UINT kCmdFloatLayers = 1744;    /* 浮动/停回 图层面板 */
/* P0-10 面板分组（tab strip）：两块面板合并到一列 / 拆开回各占一列 */
constexpr UINT kCmdPanelGroupMerge = 1745; /* 合并成一组 */
constexpr UINT kCmdPanelGroupSplit = 1746; /* 拆开分组 */
constexpr UINT kCmdToolBase = 1100; /* + 工具序号，范围 1100..1118 */
/*
 * 布局查询（视觉探针用）。
 *
 * 探针原本靠 Python 复算 ui_core 的布局公式，那是第二份真源——壳里改了布局
 * 公式（新增面板分组就是一种），复算就跟不上，探针量到的位置与壳画出来的
 * 位置对不上。这个通道让探针直接问壳要**真实**布局。
 *
 * 为什么用「共享内存 + WM_APP」而不是 WM_COPYDATA：
 *   - WM_COPYDATA 是**单向**的：系统在发送时把数据复制给接收方，接收方对
 *     COPYDATASTRUCT（含 lpData 指向的缓冲）的修改**不会回传给发送方**。
 *     实测 2026-10-26：壳写了 JSON、也回填了 dwData/cbData，发送方读到的
 *     仍是原值（缓冲还是全 0）。用它做「问一句、答一句」的路子根本不成立。
 *   - 共享内存是双向的：探针创建命名映射，壳打开**同名**映射写入，探针随后
 *     直接读映射内容。返回值只带「写了几字节」这个数。
 *
 * 口径：探针先 CreateFileMapping 一个 8 KB 的命名映射，再发消息；壳在消息
 * 里 OpenFileMapping 打开同名映射并写入 JSON，把写入长度作为消息返回值。
 * 探针按返回值读映射前 N 字节。壳打不开映射（探针没建）时返回 0。
 */
constexpr UINT kMsgLayoutQuery = WM_APP + 1;
/* 共享内存名（跨进程共享，命名不带 Local\ 前缀 = 会话内共享）。 */
constexpr wchar_t kLayoutQueryMappingName[] = L"CompositorLayoutProbe";
constexpr UINT kCmdFilterBase = 1200; /* + 滤镜序号 0..15（1200..1215） */
/*
 * 面板动作号落在 1220 段：滤镜档位从 9 个涨到 11 个（尾追加 Sharpen /
 * Reduce Noise）后，1200..1210 已被档位占满，原先放在 1210..1212 的
 * 应用/重置/关闭会与 Sharpen 抢同一个 id（菜单项永远打不开锐化）。
 * 1220 起给档位留下 1211..1219 的余量，后续再加滤镜不需要再搬家。
 * 现在档位用到 14（Filter > Other 组追加 High Pass / Minimum / Maximum /
 * Offset），仍在余量内，动作号不动。
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
   * P1-2：图层锁与颜色标签。两者同属「引擎已持久化、面板应该看得见」的属性：
   *   - lock_flags 决定菜单标勾与行内标记（L/T/P/G），也是引擎拒绝写入口的依据；
   *   - color_label 只影响面板配色（PS 同款），不参与合成。
   * 依旧是只读快照——写路径一律回到 comp_layer_set_lock / set_color_label，
   * 撤销、合并与 .comp 读回都不会让面板与文档漂移。
   */
  uint32_t lock_flags = 0;  /* comp_layer_lock 位组合 */
  uint32_t color_label = 0; /* comp_layer_color_label，0 = 无 */
  /* 链接组号（0 = 未链接）；只读快照，写路径走 comp_layer_link/unlink。 */
  uint32_t link_group = 0;
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
   * P1-2 收尾：图层面板过滤用的只读类型快照。三者都从 ABI 回读，与 has_mask /
   * is_group 同一口径；过滤只读它们，不改文档。
   *   - is_text：可编辑文字图层（text 对象存在，像素只是渲染缓存）；
   *   - adjustment_count：调整层判定（>0 即调整层；PS 的「调整图层」过滤项）；
   *   - fill_kind：填充层种类（0 = 非填充层，1 纯色 / 2 渐变 / 3 图案）；
   *   - is_smart：智能对象（像素只读、滤镜非破坏）。
   */
  uint8_t is_text = 0;
  int32_t adjustment_count = 0;
  uint32_t fill_kind = 0;
  uint8_t is_smart = 0;
  /*
   * P0-1：名字来自 comp_layer_get_name（UTF-8），面板只读回读结果。
   * 壳侧**不再自持名字**——重命名、撤销、合并、读盘之后名字都可能变，
   * 存第二份就会与文档漂移（与 opacity / blend 同一口径）。
   */
  std::string name;
};

/*
 * 图层面板键入态（重命名 / 精确不透明度 / 过滤词 / 复合名 / 智能滤镜名）。
 *
 * `FilterName` 是「按名称过滤图层」的过滤词，不是滤镜的名字；智能滤镜的展示名
 * 用 `SmartFilterName`（P1-2 收尾 ⑩），两者别混。
 */
enum class LayerInput { None, Rename, Opacity, FilterName, CompName, SmartFilterName };

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
  /*
   * 图层列表的滚动量（顶部被滚过去的行数）。
   *
   * 此前图层列表**没有滚动**：所有行都画出来、超出面板的部分直接裁掉，
   * 层一多就看不到底下的层——不是「不方便」，是**看不到**。
   * 调整栏里的滑杆区 / 历史 / 通道都有滚动，只有图层列表漏了。
   */
  int32_t layer_scroll = 0;
  /*
   * 上次做「把选中行滚进可视区」时的选中行（-1 = 还没做过）。
   *
   * 为什么要有：把「选中永远可见」放在**每次渲染**里做的话，用户滚轮往下看
   * 下面的层，滚动位置会被立刻拉回选中行——**滚轮根本滚不动**。
   * 所以只在**选中行真的变了**时才 ensure，其余时候只夹取滚动量。
   */
  int32_t layer_scroll_anchor = -1;
  /* 正在拖动图层列表的滚动条滑块（拖动期间吃掉鼠标消息）。 */
  bool layer_scroll_drag = false;
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
  /* 键入态的第二坐标：智能滤镜改名时的栈内下标（layer_input_id 只装得下图层）。
   * 其它键入态不用它，panel_cancel_layer_input 一律清零。 */
  size_t layer_input_slot = 0;
  std::vector<uint32_t> adj_kinds;       /* 选中图层已有调整的 kind（数组序） */
  std::vector<std::string> adj_settings; /* 与 adj_kinds 对齐的 settings JSON */
  int32_t selected_adjustment = -1;      /* 已有调整的选中索引；-1 = 未选（新增态） */
  int32_t adj_kind = 3;                  /* 当前编辑 kind，默认 3 = Exposure */
  /*
   * ROI #8：破坏性调整态。true 时调整面板的同一批参数行服务
   * comp_layer_apply_adjustment（Image > Adjustments），Ctrl+Shift+U 直接落墨，
   * 不产生调整层；selected_adjustment 恒为 -1（没有可编辑对象）。
   */
  bool adj_destructive = false;
  /*
   * 破坏性 Curves：点列没有可写的调整对象，先寄存在这条 settings 串里（写入走
   * comp_layer_curves_set_points 纯函数），Ctrl+Shift+U 时由 build_settings_json
   * 原样交出。只在该态且 kind = Curves 时有效。
   */
  std::string destructive_curve_settings;
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
  /* P0-1：新建对话框的位深与颜色模式。取值直接是 ABI 枚举（0/1），
   * 「记住上次设置」与背景行同一口径——面板收起后这些字段仍留着。 */
  int doc_dialog_pixel_format = 0; /* 0 = 8 位 / 1 = 16 位 */
  int doc_dialog_color_mode = 0;   /* 0 = RGB / 1 = 灰度 */
  int doc_dialog_lock_aspect = 1;
  int doc_dialog_anchor = 4; /* 居中 */
  int doc_dialog_steps = 0;
  int doc_dialog_budget_mb = 0;
  int doc_dialog_close_action = comp_ui::kCloseActionSave;
  int doc_dialog_base_width = kDefaultWidth;
  int doc_dialog_base_height = kDefaultHeight;
  /* 变换选区：第三行是角度（度，顺时针为正）；宽/高复用上面两个字段。 */
  int doc_dialog_angle = 0;
  /*
   * P1-3：数值变换面板打开时的角度行原值。应用时若角度行没被改过
   * （doc_dialog_angle == doc_dialog_angle_base），写回**当前**的浮点角度而不是
   * 取整后的显示值——只改宽高不该把 22.5° 悄悄变成 23°。
   */
  int doc_dialog_angle_base = 0;
  /*
   * P1-3：再次变换复读的「上一次」——比例与角度增量（度）。按文档记录，换文档
   * 即清零（见 forget_color_range）。移动/箭头微调刻意不记录：它们走的是 nudge
   * 事务，与 set_transform 的矩形不是同一个量；PS 的 Transform Again 也复读
   * 移动，但这里没有「拖动变换」这个可复读的步。
   */
  bool transform_step_valid = false;
  double transform_step_scale_x = 1.0;
  double transform_step_scale_y = 1.0;
  double transform_step_angle = 0.0;
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
  /*
   * P1-2 收尾 ⑧：新建填充层。三种 kind 共用一套参数行（参数表由引擎驱动），
   * 所以只有一个面板；fill_values 与参数表按下标对齐，写入时整体交给
   * comp_fill_build_settings 组装 JSON（缺省项/越界由引擎校验）。
   */
  bool fill_open = false;
  uint32_t fill_kind = 0;               /* 1 纯色 / 2 渐变 / 3 图案 */
  comp_layer_id fill_layer = 0;         /* 面板编辑的目标层；0 = 新建态 */
  int32_t fill_param = 0;               /* 聚焦的可编辑参数序号 */
  std::vector<std::string> fill_values; /* 与参数表对齐的 ASCII 编辑值 */
  std::wstring fill_input;              /* 键入缓冲；非空 = 输入态 */
  int32_t fill_drag_param = -1;
  comp_ui::Rect fill_drag_track{}; /* 拖动期间冻结的轨道（指针 -> 值换算用） */
  comp_txn fill_drag_txn = nullptr;
  bool fill_drag_dirty = false;
  int32_t fill_scroll = 0;
  /*
   * P1-2 收尾 ⑨：智能滤镜栈面板。列表每次绘制都从引擎现读（count / _at），壳里
   * 不缓存滤镜参数，撤销 / 重做后不会出现「面板还停在旧栈」。
   * smart_edit_* 是「滤镜面板改成编辑某一条」的那层绑定：smart_edit_base 保存读回
   * 的那条 desc，写回时只覆盖面板行承载的字段——Vignette 的边缘色、Progressive
   * Blur 的显式轴不在面板行里，必须原样带回，否则一次回车就把它们悄悄清零。
   */
  bool smart_open = false;
  int32_t smart_focus = 0;
  int32_t smart_scroll = 0;
  comp_layer_id smart_layer = 0;      /* 面板绑定的层；refresh 时与选中层同步 */
  comp_layer_id smart_edit_layer = 0; /* 非 0 = 滤镜面板正编辑该层的某一条 */
  size_t smart_edit_index = 0;
  int32_t smart_edit_enabled = 1;
  comp_filter_desc smart_edit_base{};
  /*
   * 面板内拖拽重排（P1-2 收尾 ⑩）：按下即 pending，纵向移动超过 4 px 才 active
   * （单击必须仍是「选中」）。active 期间 smart_drag_to 是「松手后这条会落在的
   * 行」，松手才写引擎（comp_layer_smart_filter_move = 一步撤销）。
   */
  bool smart_drag_pending = false;
  bool smart_drag_active = false;
  int32_t smart_drag_from = -1;
  int32_t smart_drag_to = -1;
  int32_t smart_drag_press_y = 0;
  comp_ui::StrokeKind stroke_kind = comp_ui::StrokeKind::None;
  std::vector<float> stroke_points;
  /* P1-4：一笔内 WM_POINTER 笔压采样（整笔取平均；_begin 清零）。 */
  double stroke_pressure_sum = 0.0;
  int stroke_pressure_samples = 0;
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
  /*
   * P0-6③：段落框（文档像素）。0 = 自动（点文字），> 0 = 段落框；框高只裁切。
   * 与字号/行距同属文档的键入状态（随文档重置），由选项栏改、提交时写进
   * comp_text_desc 的尾部字段，随后随文字对象一起存进 .comp。
   */
  double type_box_width = 0.0;
  double type_box_height = 0.0;
  int type_align = comp_ui::kTypeAlignLeft;
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
  /*
   * ROI #5：存储的选区通道（PS Alpha 通道）。列表本身在引擎文档里（随 .comp
   * 往返、随撤销回退），壳侧只留面板的显示状态。命名输入走 WM_CHAR，与图层
   * 改名同一条通路；channel_name_active 期间键盘模态（退格/回车/Esc 在
   * WM_KEYDOWN，字符在 WM_CHAR）。
   */
  bool channel_open = false;
  int channel_selected = 0;
  int channel_scroll = 0;
  bool channel_name_active = false;
  /*
   * 面板里按 S 开命名时，TranslateMessage 会把同一个按键变成一条 WM_CHAR
   * （字符 's'）。置位后在 WM_CHAR 里吞掉恰好一个字符——否则默认名会先被
   * 塞进一个 s。从菜单进命名不置位。
   */
  bool channel_name_skip_char = false;
  std::wstring channel_name_input; /* 存储通道的命名缓冲 */
  /*
   * ROI #3 视图开关。按**文档**记而不是按会话记：换文档时连同文档一起复位，
   * 否则会出现「A 文档关掉的标尺在 B 文档莫名打开」。参考线本身不是开关，
   * 它存在引擎文档里（随 .comp 往返、随撤销回退）。
   */
  bool show_rulers = false; /* 标尺条（Ctrl+R）：默认关，否则盖住画布最外 18px */
  bool show_guides = true;  /* 参考线可见（Ctrl+;）：没有参考线时画不出东西 */
  bool show_grid = false;   /* 网格（Ctrl+'）：50 文档像素一格 */
  bool snap_enabled = true; /* 拖动吸附（Ctrl+Shift+;）：只改落点，不改像素 */
  /*
   * 参考线拖动（持有鼠标捕获）。整次拖动包在一个显式事务里 = 一步撤销；
   * 拖到标尺上松手 = 删除（这条是本次新建的则 abort，不留空撤销步）。
   * guide_drag_doc 与 layer_move_doc 同一用途：捕获被抢走/换文档时先核对
   * 句柄再 abort，避免对已销毁文档调用引擎。
   */
  bool guide_drag_active = false;
  bool guide_drag_created = false; /* true = 本次拖动新建（松手落回标尺要 abort） */
  int32_t guide_drag_index = -1;
  uint32_t guide_drag_axis = 0;
  double guide_drag_drop = 0.0; /* 当前落点（文档像素，已吸附） */
  bool guide_drag_changed = false;
  comp_txn guide_drag_txn = nullptr;
  comp_document guide_drag_doc = nullptr;
  int32_t guide_hover = -1; /* 悬停高亮的参考线序号；-1 = 无 */
  bool needs_fit = true; /* 新文档/打开文档后，下一次绘制时适配窗口 */
};

/*
 * 一个文档标签槽（P0-3）。
 *
 * 不变式：`tabs` 至少一项，且**恰好一项** front = true —— 那一项的内容不在这儿，
 * 而在 `ShellState::doc`。前台文档始终只有一份副本，避免「两处都有内容、谁说了算」
 * 这种最难查的双写。
 *
 * 槽里只放文档态，标题从 `doc.document_path` 现算：路径是唯一真源，另存一份
 * title 迟早会跟路径分叉（另存为之后标签还顶着旧名字）。
 */
struct DocTabSlot {
  bool front = false;
  DocumentContext doc;
};

/*
 * 应用会话态：引擎、工具选择、笔刷/面板等跨文档保留的偏好，以及窗口句柄。
 *
 * P0-3 起 doc 不再是唯一实例：`tabs` 里放**非前台**文档，前台那一份永远在
 * `doc`。这样 34k 行里所有 `shell.doc.xxx` 一个都不用改，换文档 = 整体 move
 * 一份 DocumentContext——比给 189 个字段改成 `shell.tabs[i].doc.xxx` 安全得多。
 */
/* 调整栏里可拖动的滚动条（对应三个可滚子面板：历史 / 通道 / 字体）。 */
enum class ScrollDragTarget {
  None = 0,
  History = 1,
  Channel = 2,
  Font = 3,
};

struct ShellState {
  /*
   * 调整栏里正在拖哪条滚动条（None = 没在拖）。
   *
   * 三个子面板各有自己的滚动量与可见滚动条，拖动期间要知道自己动的是哪一条
   * —— 记在枚举里，不靠「当前显示的是哪个面板」反推（面板可能在拖动中途切换）。
   */
  ScrollDragTarget scroll_drag_target = ScrollDragTarget::None;

  comp_engine engine = nullptr;
  comp_backend backend = COMP_BACKEND_AUTO;
  std::wstring status;
  comp_ui::ToolId tool = comp_ui::ToolId::Select;
  double brush_size = 24.0;
  double brush_hardness = 0.75;
  int brush_preset = 0;       /* P1-4：只读预设表下标；0 = 自定义（不覆盖当前值） */
  double brush_flow = 1.0;    /* P1-4：COMP_TOOL_STROKE 的每 dab 覆盖面峰值 */
  bool brush_pressure = true; /* P1-4：压感开关；鼠标恒 1.0，关掉后笔也按 1.0 */
  /*
   * 最近一次 WM_POINTER 报告的笔压（0..1）与「是否来自笔」。这两个不是文档
   * 状态、不进 .comp：它们是输入设备的即时读数，只喂下一笔与状态栏。
   */
  double pen_pressure = 1.0;
  bool pen_pressure_active = false;
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
  /* P0-1：新建面板的「记住上次设置」——与尺寸/分辨率/背景同一口径。 */
  int new_doc_pixel_format = 0;
  int new_doc_color_mode = 0;
  /*
   * P0-10 数字直输：点选项栏的数值区进入键入态。放会话态而不是文档态——
   * 它改的是工具参数（跨文档保留），换标签不该把半截输入带走。
   */
  /*
   * P0-10 光标模式：0 = 标准（画布内用系统箭头，笔尖轮廓由画布叠加层画）、
   * 1 = 十字（大面积涂抹时看得见落点、又不会被箭头挡住像素）、2 = 隐藏
   * （画笔预览已经足够定位时用）。只改画布内的光标，面板/工具条不受影响。
   */
  int cursor_mode = 0;
  bool option_input_active = false;
  int option_input_index = -1;  /* 组序号，绘制时用来判断哪一格画输入框 */
  comp_ui::ToolOptionId option_input_id = comp_ui::ToolOptionId::None;
  std::wstring option_input;    /* 键入缓冲（ASCII 数字） */
  /*
   * true = 缓冲里还是「点开时的旧值」，用户还没动过手。第一个数字键进来时把
   * 旧值整段替换掉（PS 的做法是全选后输入）。没有这个标志的话，点开就是
   * 「24」+ 敲「8」= 「248」——想改小得先按十几次退格。
   */
  bool option_input_fresh = true;
  bool camera_raw_tables_loaded = false;
  std::wstring window_title_cache;
  comp_ui::ColorRGB foreground{};
  int32_t palette_index = 0; /* X 键循环到色板的哪一项 */
  std::wstring color_input;  /* Shift+X 的十六进制键入；非空 = 输入态 */
  /*
   * P0-10 停靠子系统：面板的停靠/收起/浮动状态全在这里（shell_dock.h）。
   * 此前散成 5 个壳字段（两种宽度 + 两个收起标志 + 一个停靠边），每加一种形态
   * （浮动）就要再散一层；收进一个状态机之后，窗口只负责「把状态画出来」。
   */
  comp_dock::DockState dock;
  /*
   * P0-10 浮动面板窗口：**每块面板各一个**独立浮窗（2026-10-26 起放开「至多一块」）。
   * 索引 = comp_dock::PanelId 的整数值；nullptr = 该面板没浮在外面。
   *
   * 此前是「一个 HWND + 一个 PanelId」，因为当时认定「两块都浮要两套命中与焦点
   * 通路」。实际不需要：浮动窗口过程从 GWLP_USERDATA 取自己的 PanelId，命中层
   * 是无状态函数（shell_panel_hit），键盘转发给主窗口即可。按面板索引存句柄
   * 之后，「谁浮在外面」这件事不再有全局单例约束。
   */
  HWND float_windows[static_cast<int>(comp_dock::PanelId::Count)] = {nullptr, nullptr};
  /*
   * 拖出状态：按住面板标题行往外拖，越过阈值就浮成独立窗口。
   * tear_panel = Count 表示当前没有拖出动作。
   */
  comp_dock::PanelId tear_panel = comp_dock::PanelId::Count;
  int tear_grab_x = 0;
  int tear_grab_y = 0;
  int tear_last_x = 0;
  int tear_last_y = 0;
  bool tear_active = false;
  bool tear_moved = false;
  int panel_resize_target = 0;
  int panel_resize_base_width = 0;
  int panel_resize_start_x = 0;
  int panel_splitter_hover = 0;
  int panel_toggle_hover = 0;
  int marquee_shape = comp_ui::kMarqueeRect;
  int wand_tolerance = comp_ui::kDefaultWandTolerance;
  bool wand_contiguous = true;
  bool wand_merged = false;
  int wand_radius = comp_ui::kDefaultWandRadius;
  int gradient_type = comp_ui::kGradientLinear;
  /* P1-7：多色标预设（只读表）+ 抖动开关；都不是文档状态，换图层不重置。 */
  int gradient_preset = 0;
  bool gradient_dither = false;
  /*
   * P1-7 收尾：自定义色标表与面板里的选中项。同样是**工具状态**，不进 .comp、
   * 不进撤销栈——与预设表一样，换图层/存盘不会把它带走。只有「自定义」档
   * （kGradientPresetCustom）在落笔时读这张表；其余档位仍按预设现展开。
   */
  std::vector<comp_ui::GradientPresetStop> gradient_custom_stops;
  int gradient_stop = 0;        /* 面板里选中的色标下标（0 基） */
  int gradient_stop_action = 0; /* 增删行的瞬时动作；执行后立刻归零 */
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
  /*
   * ROI #7：当前窗口所在显示器的 DPI（96 = 100%）。壳层的全部排版常量都是
   * **逻辑像素**（DIP），上屏时由 MM_ANISOTROPIC 映射统一放大到设备像素；
   * 只有窗口尺寸、位图尺寸与鼠标坐标需要在这两个坐标系之间换算。
   */
  int dpi = 96;
  /*
   * P1-2 ④：图层样式剪贴板（引擎导出的整份报文，空串 = 尚未拷贝）。
   * 与位图剪贴板不同，样式报文很小、也不需要和系统剪贴板互通，所以由外壳
   * 自己持有；放会话态而非文档态，拷贝后换文档仍能粘贴。
   */
  std::string layer_style_clipboard;
  /*
   * P1-2 收尾：图层面板过滤（视图态）。三项叠加生效：
   *   - layer_filter_kind：0 全部 / 1 像素 / 2 文字 / 3 组 / 4 调整 / 5 填充 / 6 智能；
   *   - layer_filter_color：-1 全部 / 0 无标签 / 1..7 红..灰；
   *   - layer_filter_name：UTF-16 子串（不分大小写），空串 = 不过滤。
   * 三者都不进文档、不进撤销、不落 .comp，换文档时保持——这是「看的方式」，
   * 不是文档属性（与折叠集合同一类）。
   */
  int layer_filter_kind = 0;
  int layer_filter_color = -1;
  std::wstring layer_filter_name;

  /*
   * 当前选中的图层复合 id（0 = 尚未选）。纯视图态：复合数据在文档里，这里只记
   * 「菜单上的应用/更新/删除指向哪一个」。文档里已不存在该 id 时（撤销、换文档）
   * 由 panel_current_layer_comp() 退回到第一个复合，不在这里做失效清理——
   * 撤销栈随时会把它变回来。
   */
  comp_layer_comp_id layer_comp_current = 0;

  /* 当前活动文档的上下文（本轮先内嵌一份，切换文档即整体替换）。 */
  DocumentContext doc;

  /* ---------------------------------------------------------- P0-3 多文档 */
  /*
   * 标签槽：`tabs[active_tab]` 是前台（内容在 `doc`，槽里是空壳），其余槽各存
   * 一份完整的 DocumentContext。懒初始化：第一次用到时补一个前台槽，不必改
   * main() 的启动流程。
   */
  std::vector<DocTabSlot> tabs;
  int active_tab = 0;
  /* true = 当前文档已经停进 tabs，正在建/载入新文档（成功后插入新槽，失败则回滚）。 */
  bool tab_parked = false;
  /*
   * 关闭流程的意图。0 = 关窗（退出）；1 = 只关闭当前标签。
   * 退出时逐标签提示未保存：每答完一个就前进到下一个脏标签，直到全部收口。
   */
  int close_intent = 0;
  /* 最近打开列表（UTF-8 全路径，最新在前，上限 kRecentLimit）。 */
  std::vector<std::string> recent_files;
  /* 最近文件面板里的当前选中项（行 0 的 Choice 值）。 */
  int recent_index = 0;
};

extern ShellState* g_shell; /* 单窗口外壳，生命周期与窗口一致 */

}  // namespace comp_shell

#endif  // COMPOSITOR_SHELL_STATE_H
