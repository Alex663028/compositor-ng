// AI生成
/*
 * exr_codec.h — 自研 OpenEXR scanline 读写器（C 方案 C3b）。
 *
 * 为什么自研而不是 vendor OpenEXR：本仓库的第三方依赖是**按文件夹 vendored**
 * 的真库（zlib / libpng / stb），没有包管理器；OpenEXR 本体带 Imath 等一串
 * 依赖，塞不进当前构建。而「读 16/32 位浮点 EXR」这一件事只需要 scanline
 * 容器 + 三种压缩，正好可以落在已经 vendored 的 zlib 上。
 *
 * 能力边界（全部**显式拒绝**，不静默近似）：
 *   读：scanline、单部件、非 tile、非 deep；压缩 NONE / RLE / ZIPS / ZIP；
 *       通道类型 HALF(1) / FLOAT(2)（UINT 拒绝）；采样率全 1（子采样拒绝）；
 *       行序 INCREASING_Y / DECREASING_Y。
 *       压缩 PIZ / PXR24 / B44 / B44A / DWAA / DWAB 一律拒绝并写明原因。
 *   写：未压缩（NONE）scanline，通道 A/B/G/R 全 HALF 或全 FLOAT。
 *       RGBA16F 资产按 HALF 写（与图层存储同精度，往返逐位一致）；
 *       RGBA16 资产按 FLOAT 写——half 只有 11 位尾数，存不下 16 位整数的
 *       低 5 位，走 HALF 会把 16 位资产悄悄降精度。
 *
 * 口径（与 codec_lib.h 一致）：本层只做容器与压缩，**不碰曲线也不预乘**。
 * 读出的像素是文件原生的**线性光、直通 alpha** float RGBA，写进图层时的
 * sRGB 曲线与预乘由 domain 层统一施加（wide_asset_from_linear /
 * deep_hdr_take_linear），这样 Radiance 与 EXR 两条入口得到同一种颜色。
 */
#ifndef COMPOSITOR_FORMAT_EXR_CODEC_H
#define COMPOSITOR_FORMAT_EXR_CODEC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "format/codec_lib.h"

namespace compositor {

/* 读取器是否编进本构建：本文件始终参与编译，因此恒为 true；保留函数是为了与
 * codec_lib 的 __has_include 能力探测同形（调用方不必知道实现细节）。 */
bool exr_codec_available();

/* ZIP/ZIPS 需要的 zlib 是否可用（树内 zlib 探测结果）。 */
bool exr_codec_has_zlib();

/* 真实后端标识（含实际 zlib 版本），供登记表 note 使用。 */
const char* exr_codec_backend();

/* 只读文件头里的 dataWindow 尺寸，不碰像素、不分配大缓冲。
 * 失败（不是 EXR、头部损坏、tile/deep/multipart、尺寸超界等）返回 false 并写 error。 */
bool exr_declared_size(const uint8_t* data, size_t size, int32_t* width, int32_t* height,
                       std::string* error);

/* 解码为线性光、直通 alpha 的浮点 RGBA（4 个 float 一像素、行紧排）。
 * 失败返回 false 并写 error（不假装成功）。 */
bool exr_decode_float(const uint8_t* data, size_t size, CodecFloatImage* out, std::string* error);

/* 写出的通道采样类型：数值与 EXR 的 pixelType 一致（HALF=1 / FLOAT=2）。 */
enum ExrSampleType { kExrSampleHalf = 1, kExrSampleFloat = 2 };

/* 编码未压缩 scanline EXR（A/B/G/R 同一种采样类型）。输入为线性光、直通 alpha。
 * FLOAT 写出的是无损的 float，HALF 写出按最近偶数舍入。 */
bool exr_encode_float(const float* rgba, int32_t width, int32_t height, ExrSampleType sample_type,
                      std::vector<uint8_t>* out, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_FORMAT_EXR_CODEC_H */
