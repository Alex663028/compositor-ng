// AI生成
/*
 * tool_ops.h — 工具操作领域层（像素内核的产品通路）。
 *
 * 背景（为什么有这层）：
 *   engine/imaging 下有 10 组上游像素内核。它们是**纯 C 算法**，签名按上游
 *   原工程保留（`wand_mask(rgba, w, h, stride, seedX, seedY, radius, tolerance,
 *   contiguous, mask)` 这种形态）。历史上这些函数只有 tests 直调，引擎代码
 *   零调用——**有实现、无功能**：宿主没有任何 comp_* 入口能触发它们。
 *
 *   缺的不是把函数调用起来，而是**参数契约**：笔压、选区、容差、种子、蒙版来源
 *   在上游是工具层的交互概念（宿主跟踪鼠标采样、笔压曲线、选区），内核签名里
 *   只留下结果。本文件把这些交互概念收敛为引擎持有的稳定结构，并在这里把
 *   「领域参数 + 图层几何」装配成内核签名所需的原始形态。
 *
 * 设计原则（与 adjustment_table 的单一真源一致）：
 *   1. 参数结构**只追加、不改语义**，与 ABI 结构体布局同规矩；
 *   2. 写操作一步撤销（与 resize_image / flatten 同款）；
 *   3. 参数非法 → COMP_ERR_INVALID_ARGUMENT，且**不写半成品**（闸在事务之前，
 *      对齐上游 EditorSession.swift:516 先 guard 再 beginEdit 的次序）；
 *   4. 合成不变性：每个算子要么「产出新像素」，要么「只写蒙版」，不改
 *      transform / blend / opacity——合并、撤销与 .comp 往返的既有护栏因此不受影响。
 */
#ifndef COMPOSITOR_DOMAIN_TOOL_OPS_H
#define COMPOSITOR_DOMAIN_TOOL_OPS_H

#include <cstdint>
#include <string>

#include "compositor/comp_core.h"
#include "compositor/comp_layer.h"
#include "domain/layer.h"

namespace compositor {

class Document;

/* ============================================================== 选区 */

/*
 * 选区参数：魔棒 / 色彩范围的输入。上游 Select/ColorRange 面板的全部可编辑项
 * 都落在这五个字段里（容差、种子、参考半径、是否连续、反选）。
 *
 * 语义逐字对齐 WandPixels.h：
 *   - tolerance 是**逐通道**距离阈值（0..255），alpha 也参与比较；
 *   - radius 是「种子邻域」的半边长，参考色 = 该 (2r+1)² 方窗内的平均色，
 *     方窗按图像边界裁剪；
 *   - contiguous 非 0 时只从种子做 4-连通泛洪（种子自身不匹配则选中 0 个）；
 *   - invert 非 0 时选中的取反。
 */
struct SelectParams {
  int32_t tolerance = 32;
  int32_t radius = 2;
  int32_t contiguous = 1;
  int32_t invert = 0;
};

/* 色彩范围的颜色集合（Color Range 面板的吸管列表）。 */
struct ColorRangeParams {
  int32_t fuzziness = 32;
  int32_t invert = 0;
};

/* ============================================================== 笔刷 */

/*
 * 笔刷参数：画笔施加到图层上的笔迹（Stroke）。
 *
 * 语义（对齐上游 BrushTools）：
 *   - `diameter` 是笔迹在**图层像素空间**的直径（像素），<=0 视为 1；
 *   - `pressure` 是整体笔压 0..1，乘进 coverage；分段笔压曲线由宿主负责
 *     （它是交互采样，不是内核参数，见文件头设计原则 1）；
 *   - `hardness` 0..1 决定羽化带宽（0 = 最软，1 = 全实）；
 *   - `flow` 与 pressure 相乘后作为单次落笔的覆盖率上限（避免 100% 笔压糊死细节）；
 *   - `color` 是**直通（premultiplied 约定下的非预乘）sRGB**笔画色，alpha 随 flow；
 *   - `seed` 固定：同一组参数两次调用产出逐字节相同的笔迹（可重复性护栏）。
 *
 * 笔迹形状是 coverage 的 8-bit 灰度（与 MaskBuffer 同布局），因此内核
 * `layer_extract_alpha` / `brush_alpha_bounds` 一族直接可用。
 */
struct BrushStrokeParams {
  uint8_t color[3] = {0, 0, 0};
  float diameter = 1.0f;
  float pressure = 1.0f;
  float hardness = 1.0f;
  float flow = 1.0f;
  uint32_t seed = 0;
  /*
   * 颗粒（NoisePixels.h `noise_add_at`）。`grain_amount` 是 Photoshop 百分比口径
   * （均匀噪声 ±amount% 半程，高斯标准差为其 2/3），0 = 不加颗粒。
   *
   * **用 noise_add_at 而不是 noise_add**：内核的噪声只取决于像素位置与 seed，
   * 笔画以文档坐标给出、以图层原点平移后落笔，若用 noise_add 则同一笔在图层
   * 移动后会得到不同的颗粒（噪声跟着图层原点跑）。noise_add_at 带 origin，
   * 把噪声钉在文档坐标系上，跨笔画/跨图层移动都稳定。
   *
   * `grain_gaussian` 非 0 = 高斯，`grain_monochromatic` 非 0 = 三通道同值。
   */
  float grain_amount = 0.0f;
  int32_t grain_gaussian = 0;
  int32_t grain_monochromatic = 0;
};

/* ============================================================== 修补 */

/*
 * 修补参数（内容感知填充 / 修复画笔）。上游 Heal 与 Content-Aware Fill 面板项：
 *   - `mode` 0 = Content-Aware（PatchMatch，PatchPixels.h）；1 = Create Texture；
 *     2 = Proximity Match。三者都是 HealPixels.h `spot_heal` 的 mode。
 *   - `opacity` 0..1，结果按 coverage × opacity 替换原像素。
 *   - `seed` 固定 ⇒ 同参数同结果（PatchMatch 是随机化的，必须靠它定住）。
 */
struct HealParams {
  float opacity = 1.0f;
  int32_t mode = 0;
  uint32_t seed = 0;
};

/* ============================================================== 滤镜 */

/*
 * 抖动/半调参数。**直接用内核的 `DitherParams`（DitherPixels.h）而不是另定一份
 * 同构的结构**：两份同构结构会漂移（字段改名 / 顺序变），而本层的职责是「搬运 +
 * 闸」，不是重新定义内核参数。
 *
 * 刻意**不进 ABI** 的字段（见 comp_layer.h 的 comp_tool_dither 注释）：
 *   - glyphWidth / glyphHeight / glyphs / glyphCoverage / glyphCount —— 字形图集是
 *     宿主资源，从 ABI 灌一整套字形位图进引擎不合适。它们置 0 后内核的 GLYPHS
 *     走 spot() 的 default 分支（菱形网点），并在 note 里如实回报回落。
 */
struct DitherParams {
  int32_t style = 0;   /* DITHER_ATKINSON .. DITHER_GLYPHS */
  int32_t levels = 2;  /* 2..16；2 = 纯 1-bit */
  float diffusion = 1.0f;
  float density = 0.0f;
  float contrast = 0.0f;
  int32_t cell = 4;        /* 半调单元边长（像素） */
  float angle = 0.0f;      /* 半调网屏角度（弧度） */
  int32_t light_on_dark = 0;
  int32_t original_colors = 0;
  uint8_t dark[3] = {0, 0, 0};
  uint8_t light[3] = {255, 255, 255};
};

/* ============================================================== 透镜 */

/*
 * 径向透镜畸变（LensPixels.h `lens_distort`）。
 *   k > 0 拉直桶形畸变（边角裁切），k < 0 拉直枕形畸变（边角转透明），k = 0 恒等。
 * 作用在**图层像素**上（不是整篇合成）：输出层几何不变，像素被重采样。
 */
struct LensParams {
  double k = 0.0;
};

/* ============================================================== 修饰 */

/*
 * 仿制图章参数（上游 CloneStamp.swift 的 CloneSettings + cloneStrokeOffset）。
 *
 * 笔刷形状复用 `BrushStrokeParams`（diameter / hardness / pressure / flow），
 * 本结构只承载**源**与采样策略：
 *   - `source_x/y` 是宿主吸管落点（文档像素）。`has_source` 为 0 且没有可用的
 *     历史偏移时，按上游「Nothing to copy until a source is set」拒绝；
 *   - `offset_x/y` 是「源 - 首笔落点」的整数像素偏移。`aligned` 且
 *     `has_offset` 时沿用它（同一会话内保持首笔偏移，上游 `cloneOffset`）；
 *     否则按 `round(source - first_point)` 现算；
 *   - `sample_all_layers` 非 0 时源快照取整篇实时合成（上游 drawLiveComposite），
 *     否则取目标图层自身的原始像素；
 *   - `opacity` 0..1，乘进 coverage 峰值（上游 CloneSettings.opacity）。
 *
 * 快照在**一笔开始时**取好：上游在同一笔里既画又采样会吃到自己刚画的像素，
 * 那不是仿制图章的行为。
 */
struct CloneParams {
  double source_x = 0.0;
  double source_y = 0.0;
  int32_t has_source = 0;
  double offset_x = 0.0;
  double offset_y = 0.0;
  int32_t has_offset = 0;
  int32_t aligned = 0;
  int32_t sample_all_layers = 0;
  float opacity = 1.0f;
};

/*
 * Smudge / Liquify（Push）参数。笔刷直径与硬度同样复用 `BrushStrokeParams`；
 *   - `strength` = 上游 settings.opacity（上游夹到 0.01..1；本仓显式允许 0，
 *     0 即逐字节不动，便于宿主实现「强度 0 预览」）。
 */
struct WarpParams {
  float strength = 1.0f;
};

/* ============================================================== 扫描线 */

/*
 * 扫描线参数（ScanlinesSettings，ScanlinesPixels.h `scanlines_apply`）。
 *
 * 逐字对齐上游 Scanlines.swift 的 normalized 之后的取值域：
 *   - spacing  2..32 像素行距；
 *   - thickness 0.05..1，一条满亮线填满间隙的比例（暗处画得更细）；
 *   - glow / dots / threshold / smoothness / density / contrast / black_level 0..1；
 *   - wobble 0..64 横向摆动像素，displace -100..100 亮度位移，split 0..16 通道分离。
 */
struct ScanlinesParams {
  int32_t spacing = 4;
  float thickness = 0.70f;
  float glow = 0.0f;
  float dots = 0.0f;
  float wobble = 0.0f;
  float displace = 0.0f;
  float smoothness = 0.50f;
  float threshold = 0.0f;
  float split = 0.0f;
  float density = 0.0f;
  float contrast = 0.0f;
  float black_level = 0.0f;
  int32_t original_colors = 0;
  uint8_t dark[3] = {0, 0, 0};
  uint8_t light[3] = {255, 255, 255};
};

/* ============================================================== 像素化 */

/*
 * 像素化参数（上游 Dither.swift 的 chunky 路径）。
 *   - block 2..64 像素块边长：先把图降采样到 1/block 抖动，再放大回来；
 *   - dot 非 0 时用 `dither_dots` 把块间缝隙填成 dark 色（菱形网点路径）；
 *   - two_colors 非 0 时缝隙取 dark 色，否则取黑（上游 `.twoColors` 同款）。
 * 上游先在缩小的副本上抖动、放大时**不做平滑**（`interpolationQuality` 默认），
 * 本仓按同一口径：最近邻放大，避免把刚做出的网点糊掉。
 */
struct ChunkyParams {
  int32_t block = 4;
  int32_t dot = 0;
  int32_t two_colors = 0;
  uint8_t gap[3] = {0, 0, 0};
};

/* ============================================================== 16 位层 */

/*
 * 深度保持操作的模式。DeepPixels 是一族「让 16 位层穿过 8 位操作」的函数，
 * 引擎把它们封装成两个对宿主直观的产品动作：
 *
 *   - kToolDeepQuantize：16 位 → 8 位（`dither_quantize16`，加约一阶噪声后取整）。
 *     本仓 .comp 存盘只支持 RGBA8，所以这是 16 位资产**落盘的正式路径**，
 *     而非丢弃路径。
 *   - kToolDeepRoundTrip：16 位 → 8 位操作 → 16 位（deep_split/deep_merge
 *     三趟 + deep_take_painted），用于「32 位资产穿过 8 位工具后不出现台阶」。
 */
enum ToolDeepMode {
  kToolDeepQuantize = 0,
  kToolDeepRoundTrip = 1,
};

/* 选区轮廓：把图层的蒙版变成闭合折线（见 SelectionOutline）。 */

/*
 * 选区轮廓（上游 MagicWand.outline，对齐 `wand_trace`，WandPixels.h）。
 *
 * 为什么要有它：上游魔棒不止产出蒙版，还把蒙版的非零区域沿像素边缘描成
 * 闭合环（外边界顺时针、洞逆时针，于是缠绕规则正好填出那些像素），宿主
 * 拿它做「选区转可编辑路径」。本仓此前只做了「选中 -> 累积蒙版」那半，
 * 轮廓这半的落点只有一个：它必须跟着**文档像素坐标**给出，因为宿主要把它
 * 接到画布上的路径上，而蒙版缓冲区是图层像素坐标。两者差一个
 * `effective_bounds()` 的原点。
 *
 * 语义（不改内核口径）：
 *   - 只读：不写图层像素、不写蒙版、不开事务、不进撤销栈；
 *   - 蒙版为空 → 成功且 loop_count == 0（对齐上游 `guard loopCount > 0` 返回 nil）；
 *   - 内核 -2（轮廓过于碎，不值得描）→ COMP_ERR_STATE，与上游 tooDetailed 同义；
 *   - 内核 -1（内存不足）→ COMP_ERR_OUT_OF_MEMORY。
 */
struct SelectionOutline {
  /* 每环的角点数（像素边缘坐标下的闭合折线，顶点成对解释）。 */
  std::vector<int32_t> loops;
  /* 全部角点 x/y 成对，坐标 = 图层像素坐标 + 图层包围盒原点。 */
  std::vector<int32_t> points;
};

/* 操作种类（与 ABI `comp_tool_op` 数值一一对应；尾部追加，不改既有值）。 */
enum ToolOpKind {
  kToolOpNone = 0,
  kToolOpSelect = 1,      /* 魔棒 / 色彩范围 -> 写入图层蒙版 */
  kToolOpStroke = 2,      /* 笔刷 -> 累加笔画到图层像素 */
  kToolOpHeal = 3,        /* 修补 -> 按 coverage 替换图层像素 */
  kToolOpDither = 4,      /* 抖动/半调 -> 图层像素 */
  kToolOpLens = 5,        /* 透镜畸变 -> 图层像素 */
  kToolOpDeep = 6,        /* 深度保持 -> 16 位资产降位或往返 */
  kToolOpScanlines = 7,   /* 扫描线 -> 图层像素（CRT 形态） */
  kToolOpChunky = 8,      /* 像素化 -> 抖动后放大并补 dot 间隙 */
  /*
   * F3 的修饰类工具。**数值从 9 起，刻意跳过 7/8**：7/8 已被域层的
   * scanlines/chunky 占用但未进 ABI（宿主没有入口）。若这里复用 7/8，
   * 同一个数值在域层与 ABI 会指两个不同的算子——那是最难查的一类错误。
   */
  kToolOpClone = 9,       /* 仿制图章 -> 从快照拷贝像素 */
  kToolOpSmudge = 10,     /* 涂抹 -> carried 缓冲拖色 */
  kToolOpLiquify = 11,    /* 液化（Push）-> 前向位移重采样 */
};

/* 全部工具操作。宿主按需填参数结构，零初始化即可调用（缺省即「什么都不改」）。 */
struct ToolOpParams {
  ToolOpKind kind = kToolOpNone;

  /* Select 用。seed_x/seed_y 为种子（文档像素坐标，必须落在目标图层内）。 */
  SelectParams select;
  ColorRangeParams color_range;
  int32_t seed_x = 0;
  int32_t seed_y = 0;

  /* Select 的色彩范围模式用（kind == kToolOpSelect 且 use_color_range 非 0）。 */
  int32_t use_color_range = 0;
  /* include/exclude 为直通 sRGB 三元组数组，长度 = *_count（各 ≤ 8，见校验）。 */
  const uint8_t* include = nullptr;
  int32_t include_count = 0;
  const uint8_t* exclude = nullptr;
  int32_t exclude_count = 0;

  /* Stroke / Clone / Smudge / Liquify 用。points 为文档像素坐标的折线折点
   * （x, y 成对，共 point_count*2 个）；后三者与 Stroke 共用同一套笔刷几何。 */
  BrushStrokeParams brush;
  const float* points = nullptr;
  int32_t point_count = 0;

  /* Clone / Smudge / Liquify 用（定义见各自的参数结构）。 */
  CloneParams clone;
  WarpParams warp;

  /* Heal 用。coverage 为 8 位灰度蒙版（0..255，255 = 待修补），尺寸须等于图层像素。 */
  HealParams heal;
  const uint8_t* coverage = nullptr;
  size_t coverage_stride = 0;

  /* Dither / Lens / Deep / Scanlines / Chunky 用。 */
  DitherParams dither;
  LensParams lens;
  ScanlinesParams scanlines;
  ChunkyParams chunky;
  ToolDeepMode deep_mode = kToolDeepQuantize;

  /*
   * kToolDeepRoundTrip 的内层 8 位操作（上游 DeepLayer.run 的 `operation`）。
   *
   * deep_split / deep_merge 需要的是「同一个 8 位操作在 low 与 +1/+2/+4 四档上
   * 的结果」，所以引擎自己跑这四趟再交给 deep_merge——**这是往返唯一能成立的
   * 方式**。因此契约不是「宿主传一组结果」，而是「宿主说要做哪个 8 位操作，
   * 引擎负责跑四遍」：宿主不能自己造像素，只能选内核。
   *
   * `inner` 非 none 时，round trip 在 16 位图层上依次执行它（跑在四档副本上），
   * 图层**保持 16 位**；`inner` 为 none 时 round trip 退化为「量化再原样合回」，
   * 这条路径仍然有意义（把 16 位落在某个 8 位操作之后），但会丢掉超过 8 位的
   * 精度，如实记入 note。
   */
  ToolOpKind inner = kToolOpNone;
};

/* ============================================================== 执行 */

/*
 * 执行一次工具操作。成功返回 true。
 *
 * 通用契约：
 *   - 一步撤销（加入调用方已开启的显式事务，或自成一个隐式步）；
 *   - 参数闸在开事务**之前**（非法参数不留半成品、不进撤销栈）；
 *   - `target` 必须存在、持有像素、且**不是图层组**（组不持有像素，无从落笔）；
 *   - `note`（可空）回传非致命说明，例如「GLYPHS 风格缺字形图集，已回落菱形网点」；
 *     致命原因写 `error`（UTF-8）并经 `out_code` 回报错误码，此时返回 false。
 *
 * 错误码由本层**直接回报**（out_code），不由调用方从文案反推——文案会变，
 * 两边各写一份匹配串的写法在前一轮审核里已被判为静默错报的隐患
 * （见 AUDIT6 §6）。
 *
 * `out_code` 在**成功时也写 COMP_OK**：调用方常把一个初值为 COMP_ERR_INTERNAL
 * 的变量传进来当「有没有出错」的标志，只在失败时写就等于把上次的残值留下来，
 * 把成功读成失败。
 *
 * 逐算子的确定性与合成不变性：
 *   - kToolOpSelect  只写 mask，图层像素与几何不动 ⇒ 合成结果仅按新蒙版变化；
 *   - kToolOpStroke  只累加 coverage×flow 的笔画（上限 100%，不会越叠越深）；
 *   - kToolOpHeal    按 coverage×opacity 替换，等价上游 Spot Heal；
 *   - kToolOpDither  图层像素就地半调（含 alpha 保真与全透明像素不动）；
 *   - kToolOpLens    以图层像素中心为原点做径向重采样，k=0 时**逐字节恒等**；
 *   - kToolOpDeep    像素格式转换（RGBA16 → RGBA8）或深度往返。
 *   - kToolOpScanlines  按 ScanlinesParams 把图层画成 CRT 扫描线；
 *   - kToolOpChunky   先按 block 降采样抖动，再最近邻放大，可选补 dot 缝隙。
 */
bool run_tool_op(Document* document, comp_layer_id target, const ToolOpParams& params,
                 comp_result* out_code, std::string* error, std::string* note);

/* 图层蒙版的轮廓折线（上游 MagicWand.outline）。见 SelectionOutline 的注释。 */
bool selection_outline(const Layer& layer, SelectionOutline* out, comp_result* out_code,
                       std::string* error);

/*
 * 图层像素的 256 桶亮度直方图（上游 Levels 面板的直方图，`levels_histogram`）。
 *
 * `bins` 必须是 `double[256]`，函数不分配内存。`coverage` 可空（空 = 权重 1）；
 * 非空时尺寸须等于图层像素的 `height × stride` 布局。
 *
 * 关键口径：**预乘像素先反预乘再取亮度**，coverage 权重按 alpha 走。否则
 * 半透明区域的颜色被 alpha 压低，面板曲线会系统性偏向暗部，与实际 levels
 * 调整对不上。16 位图层显式 `COMP_ERR_UNSUPPORTED`——桶是 8 位的，量化归
 * `COMP_TOOL_DEEP`，本函数不偷偷降位。
 */
bool layer_histogram(const Layer& layer, const uint8_t* coverage, size_t coverage_stride,
                     double* bins, comp_result* out_code, std::string* error);

/*
 * 修补 coverage 的非零包围盒（`heal_coverage_bounds`），半开区间
 * `[bounds[0], bounds[2]) × [bounds[1], bounds[3])`；coverage 空或全 0 时四个
 * 值都是 0。`spot_heal` 内部算的是同一份，本函数把它暴露给宿主，
 * 免得宿主再扫一遍、然后与内核漂移。只读，不开事务、不进撤销栈。
 */
bool heal_coverage_bounds_of(const uint8_t* coverage, size_t coverage_stride,
                             int32_t width, int32_t height, int32_t bounds[4],
                             comp_result* out_code, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_TOOL_OPS_H */
