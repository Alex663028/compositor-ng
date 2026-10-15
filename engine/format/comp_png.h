// AI生成
/*
 * comp_png.h — 最小 PNG 编解码（零第三方依赖）。
 *
 * 为什么需要它：.comp 工程包按上游规范把图层像素存成 PNG，且图层文件名必须是
 * <uuid>.png。M1 阶段用无压缩 .rgba 落盘只是临时简化，M2 必须回到 PNG 才能与
 * 上游 macOS 版互通。
 *
 * 覆盖范围：
 *   编码 — 8 位 RGBA（色彩类型 6）与 8 位灰度（色彩类型 0），zlib 使用「存储块」
 *          不做压缩。产物是完全合法的 PNG，任何标准解码器（含 CoreGraphics）都能读。
 *   解码 — 位深 1/2/4/8/16、色彩类型 0/2/3/4/6、非隔行；zlib 解压含存储块、
 *          固定 Huffman 与动态 Huffman 三种块类型（覆盖上游 CoreGraphics 产物）。
 *          隔行（Adam7）不在范围内，遇到显式报错。
 *
 * 预乘约定（重要）：引擎内部像素是预乘 RGBA8，而 PNG 按行业惯例存「直通（非预乘）」
 * 颜色。因此编码时反预乘、解码时预乘，保证与上游 macOS 版颜色一致。
 */
#ifndef COMPOSITOR_FORMAT_COMP_PNG_H
#define COMPOSITOR_FORMAT_COMP_PNG_H

#include <cstdint>
#include <string>
#include <vector>

namespace compositor {

/* 解码结果：始终为预乘 RGBA8（紧凑排布，stride = width * 4）。 */
struct PngImage {
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> rgba;

  bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

/* 预乘 RGBA8 -> PNG 字节流（色彩类型 6，位深 8）。 */
bool png_encode_rgba8(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                      std::vector<uint8_t>* out, std::string* error);

/* 8 位灰度 -> PNG 字节流（色彩类型 0，位深 8）。用于图层蒙版。 */
bool png_encode_gray8(const uint8_t* gray, int32_t width, int32_t height,
                      std::vector<uint8_t>* out, std::string* error);

/* PNG 字节流 -> 预乘 RGBA8。失败时返回 false 并写 error。 */
bool png_decode_rgba8(const uint8_t* data, size_t size, PngImage* out, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_FORMAT_COMP_PNG_H */
