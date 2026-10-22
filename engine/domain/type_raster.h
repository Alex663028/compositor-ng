// AI生成
#ifndef COMPOSITOR_DOMAIN_TYPE_RASTER_H
#define COMPOSITOR_DOMAIN_TYPE_RASTER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace compositor {

class FontFace; /* 字体轨道用（P5-2），见 domain/font_face.h。 */

/*
 * 文字工具（S11）的位图字体与栅格化内核：文本 + 样式 -> 覆盖率。
 *
 * 双轨（P5 起）：
 *   - **旧轨道**（`type_measure_text` / `type_rasterize`）：本仓自带的 5×7 位图
 *     ASCII 字体，零依赖，95 个可打印字符；语义与 P5 之前逐字节一致。
 *   - **字体轨道**（`*_with_font`，P5-2）：宿主通过 `comp_font_*` 加载一份
 *     TrueType/OpenType 字体（见 domain/font_face.h），按**字号 = em 高**排版，
 *     支持 UTF-8/CJK。两轨不互调：没加载字体时永远走旧轨道，宿主不会因为
 *     本批改动看到任何像素差异。
 *
 * 与上游 TypeTool 的关系（只复刻语义，不搬 Swift 代码）：上游把文字交给
 * CoreText 排版成字形轮廓再栅格化；旧轨道**自带一张 5×7 的 ASCII 位图字体**：
 * 95 个可打印字符（0x20..0x7E），每字符 5 列 × 7 行，列优先、bit0 = 最上一行；
 * 排列口径按「行」走：'\\n' 换行，其它字符水平排；字形步进 = 6 列
 * （5 列字形 + 1 列字间空隙）× 缩放 + 字距，行距 = font_size × line_height。
 *
 * 旧轨道明确不支持（如实拒绝，不静默替换成别的字形）：
 *   - UTF-8 / CJK：本字体只有 ASCII，把「你好」画成方框或问号都是在撒谎
 *     （字体轨道支持，但必须由宿主显式加载字体）；
 *   - 换行符以外的控制字符（含 Tab）：位图字体没有制表位语义；
 *   - 字体族 / 粗体 / 斜体 / 对齐 / 旋转 / 字距微调：本批不做，宿主拿到的是
 *     「左对齐、无变换」的一行行文字。
 *
 * 本模块只产 coverage，不碰图层、不碰选区、不开事务：合成（直通 sRGB +
 * source-over 预乘、选区掩码、16 位工作副本）在 domain/tool_ops.cpp 里做，
 * 与形状/渐变/落笔族同一处口径。
 *
 * 坐标：origin 是文本框左上角（**文档像素**，可带小数），origin_x/origin_y
 * 是图层 `effective_bounds()` 的原点；内核先把原点减掉再在图层局部坐标里
 * 栅格化，输出的窗口坐标系也是图层局部坐标。先把原点减掉再算 float/double，
 * 避免 1e6 量级坐标丢掉亚像素精度。
 */

/* 位图字体单元：5 列 × 7 行。缩放 = font_size / 7（font_size 是单元高）。 */
inline constexpr int32_t kTypeFontColumns = 5;
inline constexpr int32_t kTypeFontRows = 7;
/* 字形步进列数 = 5 列字形 + 1 列字间空隙（与经典 5×7 点阵字体一致）。 */
inline constexpr double kTypeFontAdvanceColumns = 6.0;

/* 样式（语义与 ABI `comp_type_style` 一一对应）。 */
struct TypeRasterStyle {
  /* 单元高（文档像素）。量程 [6, 2048]：更小的字号在 1×1 像素上不可辨认。 */
  double font_size = 32.0;
  /* 行距倍数：行步进 = font_size * line_height。必须 >= 1，否则相邻两行的
   * 字身高低重叠——那不是「行距」，是叠字；上限 8 防止宿主误传。 */
  double line_height = 1.25;
  /* 附加字距（文档像素）：可负（收紧），但 步进 = 6*scale + letter_spacing 必须 > 0。 */
  double letter_spacing = 0.0;
  /* 非 0 = 抗锯齿（逐像素精确面积覆盖率）；0 = 像素中心一票（硬边）。 */
  bool antialias = true;
};

/* 文本度量（文档像素；width/height 是文本框尺寸）。 */
struct TypeMeasure {
  int32_t line_count = 0;  /* 行数（至少 1） */
  int64_t glyph_count = 0; /* 字形总数（空格算字形，'\n' 不算） */
  double advance = 0.0;    /* 单字形步进（文档像素）；字体轨道为 0（逐字形步进不同） */
  double width = 0.0;      /* 最长行的步进宽度 */
  double height = 0.0;     /* font_size + (line_count-1) * 行步进 */
  /* 字体轨道专用：该字体里没有字形的字符数（它们不占像素，只占步进）。 */
  int64_t missing_glyphs = 0;
};

enum class TypeRasterStatus {
  kOk = 0,
  kBadText = 1,   /* 空文本 / 不支持的字节（含 UTF-8、Tab 等控制字符） */
  kTooLong = 2,   /* 字符数超过 kMaxTypeCharacters */
  kBadStyle = 3,  /* 字号/行距/字距越界或非有限 */
  kEmpty = 4,     /* 文本框与图层不相交：成功但没有任何像素要改 */
  kFontError = 5, /* 字体栅格化失败（句柄失效 / FreeType 报错）：宿主态错误，不是参数错误 */
};

struct TypeRasterReport {
  TypeRasterStatus status = TypeRasterStatus::kOk;
  const char* message = "";
};

/* 字符数上限（含 '\\n'）。远超上游单次点入的文字量，再大属宿主误传。 */
inline constexpr size_t kMaxTypeCharacters = 1u << 16;
inline constexpr double kMinTypeFontSize = 6.0;
inline constexpr double kMaxTypeFontSize = 2048.0;
inline constexpr double kMinTypeLineHeight = 1.0;
inline constexpr double kMaxTypeLineHeight = 8.0;
/* |letter_spacing| 上限（文档像素）：4096 已远超任何可读排版。 */
inline constexpr double kMaxTypeLetterSpacing = 4096.0;

/*
 * 窗口：图层局部像素坐标；coverage 为 w×h 覆盖率（0..255），空 = 不画。
 * 与 ShapeCoverage 同款（单通道即可——文字只有一种颜色）。
 */
struct TypeCoverage {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  std::vector<uint8_t> coverage;

  bool empty() const { return w <= 0 || h <= 0 || coverage.empty(); }
};

/*
 * 文本是否被本字体支持（不做排版）。失败时 out_error 写中文原因。
 * 只接受 0x20..0x7E 与 '\\n'；空文本（或只有换行）也算不支持——那不是一个
 * 可定义的文字操作，引擎的参数闸会把它当非法参数拒绝。
 */
bool type_text_is_supported(const char* text, size_t size, const char** out_error);

/*
 * 量文本：校验样式与文本并给出文本框。返回 kBadText / kTooLong / kBadStyle
 * 时 out 保持空状态，message 指向静态说明。
 */
TypeRasterReport type_measure_text(const char* text, size_t size, const TypeRasterStyle& style,
                                   TypeMeasure* out);

/*
 * 把文本栅格化成覆盖率窗口。
 *   - origin_x/origin_y：文本框左上角（文档像素，可带小数）；
 *   - layer_x/layer_y/width/height：图层 `effective_bounds()` 原点与图层像素尺寸；
 *   - antialias：逐像素**精确面积**（抗锯齿）或像素中心一票（硬边）。
 * 返回 kEmpty 时窗口为空且 out 保持空状态，宿主应当「成功且无操作」。
 */
TypeRasterReport type_rasterize(const char* text, size_t size, const TypeRasterStyle& style,
                                double origin_x, double origin_y, int32_t layer_x, int32_t layer_y,
                                int32_t layer_width, int32_t layer_height, TypeCoverage* out);

/* ============================================================== 字体轨道（P5-2） */

/*
 * 用宿主加载的字体（domain/font_face.h）排版并量文本。
 *
 * 口径与旧轨道的差异（都是语义差异，不是遗漏）：
 *   - `font` 必须非空；为空报 kFontError（**不**静默回落到 5×7——那会让宿主
 *     以为字体被用上了）。字体不可缩放 / 场馆度量全 0 同样是 kFontError；
 *   - 文本是 UTF-8：'\n' 断行，非法序列、过长编码、代理对与 Tab 等控制字符
 *     按 kBadText 拒绝（不做「尽力解码」）；
 *   - `style.font_size` 是 **em 高**（旧轨道是 5×7 单元高）。行步进 =
 *     font_size × line_height；首行基线 = 文本框顶 + ascender × font_size；
 *     文本框高 = (ascender - descender) × font_size + (行数-1) × 行步进；
 *   - 文本框宽 = 各字形步进之和 + 字形之间的字距（行尾不加字距）；
 *     `measure.advance` 为 0（字体轨道没有单一「字形步进」）；
 *   - 字体里没有字形的码位计入 `measure.missing_glyphs`，它们只占步进、不画
 *     .notdef 方框（缺字如实回报，不假装画上了）。
 */
TypeRasterReport type_measure_text_with_font(const char* text, size_t size,
                                             const TypeRasterStyle& style, FontFace* font,
                                             TypeMeasure* out);

/*
 * 字体轨道的栅格化。几何口径（origin、图层局部坐标、kEmpty 语义）与
 * type_rasterize 完全一致；像素口径：
 *   - FT_LOAD_NO_HINTING + 灰度 coverage（不做网格拟合，任意字号形状一致）；
 *   - 字形原点取整到整数像素（不做亚像素插值，已知取舍）；
 *   - `antialias` 非 0 时用 FreeType 灰度值；0 时 >= 128 记 255（硬边）；
 *   - 字形重叠取最大覆盖率，不叠加（与旧轨道「不叠亮度」同口径）；
 *   - `out_missing_glyphs`（可为 nullptr）写入缺字个数，供 note 如实回报。
 * 返回 kFontError 时 out 已清空（句柄失效 / FreeType 失败，属于宿主态错误）。
 */
TypeRasterReport type_rasterize_with_font(const char* text, size_t size,
                                          const TypeRasterStyle& style, FontFace* font,
                                          double origin_x, double origin_y, int32_t layer_x,
                                          int32_t layer_y, int32_t layer_width,
                                          int32_t layer_height, TypeCoverage* out,
                                          int64_t* out_missing_glyphs);

}  // namespace compositor

#endif  // COMPOSITOR_DOMAIN_TYPE_RASTER_H
