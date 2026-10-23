// AI生成
/*
 * psd_codec.h — 自研 PSD 分层读写器（M1）。
 *
 * 为什么自研而不是继续走 stb_image：stb 的 PSD 通道**只解合成结果**，
 * 图层名、位置、不透明度、蒙版、组全部丢掉。而本产品的核心卖点是
 * 「打开 PSD 再存回去还是那些图层」，一份被压平的 PSD 往返一次就等于
 * 把用户的分层工程销毁了。所以这里要的是**分层容器**，不是一张图。
 *
 * 能力边界（超出的一律**显式拒绝并写明原因**，不静默丢弃——静默丢弃
 * 等于在用户不知情的情况下改了图）：
 *   读：8 / 16 位 RGB(A)，压缩 raw(0) / RLE(1)，图层记录（名字、可见性、
 *       不透明度、偏移、剪贴标志）、图层蒙版（-2）、组（lsct）。
 *       拒绝：PSB（版本 2）、32 位、灰度 / CMYK / 索引色、ZIP / ZIP+predictor
 *       压缩、`diss`/`dkCl`/`lgCl` 混合模式、真实蒙版通道 -3、
 *       文字层（TySh）、智能对象（SoLd/SoLE/PlLd/vmsk/vsms）、
 *       图层特效（lmfx/lrFX/lfx2）、调整 / 填充图层（brit/levl/curv/…）。
 *   写：同样的子集。RLE 通道数据，16 位走 `Lr16` 附加块（主 layer-info
 *       段长度写 0，与 Photoshop / psd-tools 一致）。
 *
 * 口径：
 *   - PSD 图层 alpha 是**直通**（非预乘），引擎内部是**预乘**。
 *     解码预乘、编码反预乘，与 codec_lib 的既有约定一致。
 *   - 蒙版在 PSD 里有自己的矩形，可能与图层矩形不同；读入时按
 *     `mask_rect - layer_rect` 展开成**图层像素空间**（与
 *     resample.cpp 的取样口径一致：蒙版与图层像素同尺寸、逐像素对应），
 *     矩形外的取默认色，`invert_mask` 取反。
 *   - 组：PSD 的组由两条记录组成——底下一条 `lsct=3` 的**边界分隔**记录，
 *     上面一条 `lsct=1|2` 的**文件夹**记录（名字 / 不透明度 / 混合都在它上面）。
 *     混合模式写在 lsct 块里（`8BIM`+4 字符键）而不是记录头：`pass` 是
 *     穿透组，其余键是隔离组。
 */
#ifndef COMPOSITOR_FORMAT_PSD_CODEC_H
#define COMPOSITOR_FORMAT_PSD_CODEC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_layer.h"
#include "domain/layer.h"

namespace compositor {

enum class PsdStatus {
  kOk = 0,
  kIo,          /* 数据被截断 / 结构自相矛盾 */
  kUnsupported, /* 属于本读写器明确不支持的子集，error 里写明是哪一条 */
  kDecode       /* 压缩流或通道数据损坏 */
};

const char* psd_status_name(PsdStatus status);

/* PSD 通道数据压缩方式（一个通道一个标记）。 */
enum PsdCompression {
  kPsdCompressionRaw = 0,
  kPsdCompressionRle = 1
};

/* 一个图层（组也在这张表里，is_group = true）。
 *
 * bounds 为文档像素空间的矩形；组的 bounds 为 (0,0,0,0)。pixels 的尺寸恒
 * 等于 bounds.w × bounds.h（与 PSD 的图层矩形逐像素对应），空矩形则为空缓冲。
 * mask 非空时尺寸同样等于 bounds.w × bounds.h（已展开到图层像素空间）。
 */
struct PsdLayerData {
  std::string name;
  comp_blend_mode blend = COMP_BLEND_NORMAL;
  uint8_t opacity = 255; /* 0..255，PSD 原生量纲 */

  bool visible = true;
  bool clipping = false; /* 剪贴到下方第一个非剪贴图层 */
  bool is_group = false;
  /* 仅组有意义：穿透组把子层累积到背影上（引擎 isolated=false）。 */
  bool pass_through = false;

  /* parent = 该层在 PsdDocument::layers 里的下标；-1 表示根层。
   * 组的子层**必须连续**（PSD 的分段结构要求如此，写侧会校验）。 */
  int32_t parent = -1;
  /* 剪贴源：下方第一个非剪贴层的下标；-1 表示没找到（此时 clipping 已置 false）。 */
  int32_t clipping_source = -1;

  comp_rect bounds{0, 0, 0, 0};
  ImageBuffer pixels; /* RGBA8（depth=8）或 RGBA16（depth=16），**预乘** */
  MaskBuffer mask;    /* 空 = 无蒙版 */
};

/* 一份 PSD 的分层内容。layers 顺序与 PSD 记录顺序一致：**下标 0 在最底下**。 */
struct PsdDocument {
  int32_t width = 0;
  int32_t height = 0;
  uint16_t depth = 8;
  std::vector<PsdLayerData> layers;

  bool empty() const { return layers.empty(); }
};

/* 只读文件头里的尺寸，不碰像素、不分配大缓冲。
 * 失败（不是 PSD、PSB、损坏、尺寸超界等）返回 false 并写 error。 */
bool psd_declared_size(const uint8_t* data, size_t size, int32_t* width, int32_t* height,
                       uint16_t* depth, std::string* error);

/* 分层解码。成功返回 kOk；其余状态在 error 里写明原因（不静默近似）。 */
PsdStatus psd_decode_document(const uint8_t* data, size_t size, PsdDocument* out,
                              std::string* error);

/* 分层编码。
 *
 * merged / merged_format / merged_stride 提供**合成结果**，写进文件的合并
 * 图像数据段（Photoshop 用它做缩略图预览，也用于只认合并图的老读取器）。
 * 传 nullptr 表示没有合成结果，此时写一张全透明合并图（尺寸正确的空画布），
 * 而不是省略该段——PSD 规定该段必须存在。
 * 支持 COMP_PIXEL_RGBA8 / COMP_PIXEL_RGBA16；其余格式拒绝（浮点合成结果
 * 没有 PSD 原生对应物，先拒绝而不是悄悄截断）。
 *
 * depth 取 8 或 16；16 位时图层像素缓冲必须同为 RGBA16。
 */
bool psd_encode_document(const PsdDocument& document, const uint8_t* merged,
                         comp_pixel_format merged_format, size_t merged_stride,
                         std::vector<uint8_t>* out, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_FORMAT_PSD_CODEC_H */
