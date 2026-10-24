// AI生成
#ifndef COMPOSITOR_DOMAIN_FONT_FACE_H
#define COMPOSITOR_DOMAIN_FONT_FACE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace compositor {

/*
 * 字体面（P5-1）：一份加载进来的 TrueType/OpenType 字体，供文字工具排版用。
 *
 * 与 type_raster.h 的关系：`type_raster` 原有的 5×7 点阵字体路径**不动**（旧 ASCII
 * 用例逐字节不变）；本类只在宿主显式加载了字体时才被用上（P5-2 起）。
 *
 * 为什么自带 FT_Library：FreeType 的 `FT_Library` 与 `FT_Face` 都不是线程安全的，
 * 而引擎的合成路径是并行的。每个 FontFace 独占一个 library + face，并用一把互斥锁
 * 串行化对该 face 的访问——字体排版不是每帧热路径（一次文字操作栅格化一次），
 * 串行化的代价远小于「多个线程共用一个 face 导致的未定义行为」。
 *
 * 文件读取：**自己把文件读进内存**再交给 FreeType（`FT_New_Memory_Face`），
 * 不用 `FT_New_Face` 的窄字符路径——在 Windows 上它是 fopen（本地代码页），
 * 含中文/空格的路径会静默打不开。顺带让「文件」与「内存」两条入口共用一套代码。
 *
 * 本类不支持：gzip 压缩字体、WOFF/WOFF2（本仓的 FreeType 裁剪版没编 zlib，
 * 见 engine/third_party/freetype/VENDOR-NOTES.md）。这类文件如实报 kNotAFont。
 */

/* 加载失败的原因（ABI 层据此映射到 comp_result）。 */
enum class FontLoadStatus {
  kOk = 0,
  kBadArgument = 1,    /* path/data 为空、data_size 为 0、face_index < 0、字体超过 100 MB */
  kNotFound = 2,       /* 文件不存在或不可读 */
  kNotAFont = 3,       /* 不是可识别的字体（含 gzip / WOFF，本构建不支持） */
  kBadFaceIndex = 4,   /* .ttc 里没有该面 */
  kOutOfMemory = 5,    /* 内存不足 */
  kInternal = 6,       /* FreeType 初始化或其它未归类失败 */
};

/* 字体度量：全部是 **em 比例**（1.0 = 一个 em），乘字号即文档像素。 */
struct FontMetricsInfo {
  int32_t face_index = 0;
  int32_t face_count = 1;
  int32_t glyph_count = 0;
  uint32_t units_per_em = 0;
  bool scalable = false;
  /* U+4F60「你」有字形：判断该字体能否排中文的最低门槛（不做完整覆盖统计）。 */
  bool has_cjk = false;
  double ascender = 0.0;
  double descender = 0.0; /* 通常为负 */
  double line_gap = 0.0;
  double height = 0.0;    /* ascender - descender + line_gap（FreeType 的 height） */
  double max_advance = 0.0;
};

/*
 * 一次文字渲染需要的字形位图（灰度 coverage，行优先，stride == width）。
 * 由 FontFace::render_glyph 填写；调用方负责把位图按基线偏移叠到目标窗口。
 */
struct FontGlyph {
  int32_t width = 0;
  int32_t height = 0;
  int32_t bearing_x = 0; /* 位图左上角相对「笔位 + 基线」的偏移，像素 */
  int32_t bearing_y = 0; /* 向上为正（FreeType 口径） */
  double advance = 0.0;  /* 笔位前进，像素 */
  /*
   * 该字体里没有这个码位（FreeType 字形 0 = .notdef）。此时位图为空，
   * advance 取字体最大步进，宿主据此如实回报「缺字」而不是画一个方框。
   */
  bool missing = false;
  std::vector<uint8_t> bitmap;

  bool empty() const { return width <= 0 || height <= 0 || bitmap.empty(); }
};

class FontFace {
 public:
  /*
   * 加载。失败返回 nullptr 并把原因写进 *status / *message（两者都可为 nullptr）。
   * `face_index` 是 .ttc 内的面序号，0 = 第一面。
   */
  static FontFace* load_file(const char* path_utf8, int32_t face_index, FontLoadStatus* status,
                             std::string* message);
  /* 内存加载：引擎**拷贝**字节，返回后宿主即可释放自己的缓冲。 */
  static FontFace* load_memory(const void* data, size_t size, int32_t face_index,
                               FontLoadStatus* status, std::string* message);

  ~FontFace();
  FontFace(const FontFace&) = delete;
  FontFace& operator=(const FontFace&) = delete;

  /* 句柄有效性（与 Engine/Document 同一套 magic 约定）。 */
  bool is_alive() const noexcept;

  const FontMetricsInfo& metrics() const noexcept { return metrics_; }
  const std::string& family_name() const noexcept { return family_name_; }
  const std::string& style_name() const noexcept { return style_name_; }
  /* 无 PostScript 名时为空串。 */
  const std::string& postscript_name() const noexcept { return postscript_name_; }

  /*
   * 按像素字号（em 高，可带小数）栅格化一个码位（P5-2）。
   *
   * 口径：FT_Set_Char_Size 用 26.6 定点（保留小数字号）+ FT_LOAD_NO_HINTING
   * （不做网格拟合，任意字号下字形形状一致）+ FT_LOAD_RENDER（灰度 coverage，
   * 与合成路径的单通道覆盖率同款）。字体里没有该码位时 out->missing = true、
   * 位图为空、advance 取字体最大步进（如实回报缺字，不画 .notdef 方框）。
   *
   * 返回 false = 句柄/参数无效或 FreeType 失败（不是「缺字」）；此时 out 已清空。
   * 内部持锁：同一个 FT_Face 不能被并发访问（见类注释）。
   */
  bool render_glyph(uint32_t codepoint, double pixel_size, FontGlyph* out);

  /* 内部：P5-2 的栅格化入口需要 face。非本模块代码不要直接调用。 */
  void* native_face() noexcept { return face_; }

 private:
  FontFace();

  /* 建对象并初始化（文件/内存两条入口共用，避免字节被拷两遍）；失败返回 nullptr。 */
  static FontFace* create_from_bytes(std::vector<uint8_t> bytes, int32_t face_index,
                                     FontLoadStatus* status, std::string* message);

  /* 成功返回 true（face_ 已建立）；失败时 message 写原因。 */
  bool init_from_bytes(std::vector<uint8_t> bytes, int32_t face_index, FontLoadStatus* status,
                       std::string* message);
  void read_identity();
  void read_metrics();

  uint32_t handle_magic_ = 0;
  /* 串行化对该 face 的访问（FT_Face 非线程安全）；P5-2 的栅格化入口持锁。 */
  std::mutex mutex_{};
  void* library_ = nullptr; /* FT_Library */
  void* face_ = nullptr;    /* FT_Face */
  std::vector<uint8_t> bytes_{};
  FontMetricsInfo metrics_{};
  std::string family_name_{};
  std::string style_name_{};
  std::string postscript_name_{};
};

/* ABI 句柄安全（as_font 先查表再解引用）。
 *
 * 背景：句柄就是 FontFace* 的 reinterpret_cast，靠对象内置 magic 校验。但**完全野
 * 的**句柄（宿主传了个 0xDEADBEEF 之类的未映射地址）在解引用读 magic 那一刻就
 * 段错误了，根本走不到比较。FontFace 构造成功时把自己登记进共享「存活对象登记表」
 * （core/comp_live_handles.h，Engine / Document 共用同一张），析构时注销；
 * as_font 先查表命中才允许读对象里的 magic。
 *
 * 表带一把全局互斥锁；字体不是每像素热路径（一次文字操作栅格化一次），一次
 * 哈希查找的代价可以忽略。查询只哈希指针值本身，不解引用，所以野指针查询本身
 * 也是安全的。表项带类型标签（LiveHandleKind::kFont）：Engine/Document 指针即便
 * 撞进表里也会在解引用之前被否掉——不能等 kFontMagic 比较，因为按 FontFace 的
 * 布局去读 magic 会越界（见 core/comp_live_handles.h 的 F2 说明）。
 */
void register_live_font(FontFace* face) noexcept;
void unregister_live_font(FontFace* face) noexcept;
bool is_live_font(FontFace* face) noexcept;

}  // namespace compositor

#endif  // COMPOSITOR_DOMAIN_FONT_FACE_H
