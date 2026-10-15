// AI生成
/*
 * codec_lib.h — 真实编解码后端（vendor 在 engine/third_party）的统一入口。
 *
 * 与 comp_png 的分工：
 *   comp_png  自研零依赖 PNG，负责 **.comp 工程内部** 图层像素——工程包必须能在
 *             没有任何第三方库的机器上读写，因此这条路径不允许依赖外部库。
 *   codec_lib 负责 **外部图像文件** 的导入 / 导出，使用真实库：
 *             PNG  -> libpng + zlib（真实 deflate 压缩、全位深/色彩类型归一）
 *             JPEG -> stb_image / stb_image_write（读写）
 *             PSD  -> stb_image（只读）
 *
 * 预乘约定与 comp_png 完全一致：引擎内部是预乘 RGBA8，文件内是直通（非预乘）颜色；
 * 因此解码后预乘、编码前反预乘，保证与上游 macOS 版颜色一致。
 *
 * 可用性在编译期用 __has_include 探测：third_party 缺失时全部返回 false，由
 * codec_registry 如实标记不可用，comp_io 回落到自研 PNG 或 UNSUPPORTED。
 */
#ifndef COMPOSITOR_FORMAT_CODEC_LIB_H
#define COMPOSITOR_FORMAT_CODEC_LIB_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace compositor {

/* 解码结果：始终为 **预乘** RGBA8（紧凑排布，stride = width * 4）。 */
struct CodecImage {
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> rgba;

  bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

/* 编译期能力探测（third_party 目录存在且头文件可包含时为真）。 */
bool codec_lib_has_png();
bool codec_lib_has_stb();

/* 真实后端标识（含实际版本号，由库自身 API 提供，不写死），供登记表 note 使用。 */
const char* codec_lib_png_backend();
const char* codec_lib_stb_backend();

/* PNG 导出压缩级别（0..9；越界按缺省 6 处理）。 */
void codec_lib_set_png_compression(int level);

/* PNG（libpng + zlib）：直通 RGBA8 文件 <-> 预乘 RGBA8 内存。 */
bool codec_png_decode(const uint8_t* data, size_t size, CodecImage* out, std::string* error);
bool codec_png_encode(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                      std::vector<uint8_t>* out, std::string* error);

/*
 * stb 通道：format 取 comp_image_format 数值（COMP_FORMAT_JPEG=2 / COMP_FORMAT_PSD=7）。
 * JPEG 可读可写（quality 0..100）；PSD 只读（写会返回 false 并写 error）。
 * 输出同样为预乘 RGBA8。
 */
bool codec_stb_decode(const uint8_t* data, size_t size, uint32_t format, CodecImage* out,
                      std::string* error);
bool codec_stb_encode(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                      uint32_t format, int quality, std::vector<uint8_t>* out, std::string* error);

/* 该 format 是否可读 / 可写（综合 libpng 与 stb 能力）。 */
bool codec_lib_can_decode(uint32_t format);
bool codec_lib_can_encode(uint32_t format);

/* 统一入口：按 format 分派到上面两条通道，供 comp_io 使用。 */
bool codec_lib_decode(const uint8_t* data, size_t size, uint32_t format, CodecImage* out,
                      std::string* error);
bool codec_lib_encode(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                      uint32_t format, int quality, std::vector<uint8_t>* out, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_FORMAT_CODEC_LIB_H */
