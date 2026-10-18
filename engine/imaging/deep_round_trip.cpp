// AI生成
#include "imaging/deep_round_trip.h"

#include <vector>

#include "imaging/DeepPixels.h"

namespace compositor {

/*
 * 把一个 16 位缓冲穿过 `op`（一个 8 位像素操作）再变回 16 位。
 *
 * 每档副本用 w*4 的紧排 stride（内核按 width 步进，与 ImageBuffer 在 RGBA8
 * 下的存储一致）。结果先算进 out，**不写 deep**：这样调用方可以把 src/dst 指同一
 * 个对象而不会边读边写。
 */
bool deep_round_trip(const uint16_t* deep, size_t deep_stride, uint16_t* out, size_t out_stride,
                     size_t width, size_t height, const DeepRoundTripOp& op, int mixes,
                     std::string* error) {
  if (deep == nullptr || out == nullptr || !op) {
    if (error != nullptr) *error = "deep round trip got null arguments";
    return false;
  }
  if (width == 0 || height == 0) return true;

  const size_t stride = width * 4u;
  const size_t bytes = stride * height;
  const int steps[3] = {1, 2, 4};

  /*
   * splits 的布局必须与 deep_merge 的读取顺序逐字一致：
   *   results[0] = low (step 0)；results[1 + i*per + j]，i = 0,1,2 对应 step
   *   1/2/4，per = mixes ? 3 : 1，j = mixes 时是该颜色通道，否则固定 0。
   */
  const int per = mixes ? 3 : 1;
  std::vector<std::vector<uint8_t>> results;
  results.reserve(static_cast<size_t>(1) + static_cast<size_t>(per) * 3u);
  std::vector<uint8_t> buffer;

  buffer.assign(bytes, 0);   /* low 档 */
  deep_split(deep, deep_stride, buffer.data(), stride, width, height, 0, -1);
  if (!op(buffer.data(), width, height, stride)) {
    if (error != nullptr) *error = "deep round trip: the 8-bit operation failed on the low step";
    return false;
  }
  results.push_back(std::move(buffer));

  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < per; ++j) {
      const int channel = mixes ? j : -1;
      buffer.assign(bytes, 0);
      deep_split(deep, deep_stride, buffer.data(), stride, width, height, steps[i], channel);
      if (!op(buffer.data(), width, height, stride)) {
        if (error != nullptr) *error = "deep round trip: the 8-bit operation failed on a near step";
        return false;
      }
      results.push_back(std::move(buffer));
    }
  }

  /* 按内核的读取顺序组装三组指针（非 mixes 时三组各自指向同一档）。 */
  const uint8_t* low_result = results[0].data();
  const uint8_t* near_results[3];
  const uint8_t* mid_results[3];
  const uint8_t* far_results[3];
  for (int i = 0; i < 3; ++i) {
    const size_t base = 1u + static_cast<size_t>(i) * static_cast<size_t>(per);
    near_results[i] = results[base].data();
    mid_results[i] = results[base + (per > 1 ? 1u : 0u)].data();
    far_results[i] = results[base + (per > 1 ? 2u : 0u)].data();
  }
  deep_merge(deep, deep_stride, low_result, near_results, mid_results, far_results, stride,
             out, out_stride, width, height, mixes);
  return true;
}

/*
 * HDR 图层上的 8 位操作：先把高光除回白内，再让操作穿过 16 位往返，最后乘回去。
 *
 * 为什么先 normalize 再往返：`deep_round_trip` 的内核（deep_split/deep_merge）读的是
 * 16 位整数布局，而 HDR 的缓冲是 half——把 half 的位模式当整数读，0x3C00（1.0）
 * 会得 15360，画面是噪声而不是「白点以上显示为白」。
 */
bool deep_hdr_round_trip(const uint16_t* hdr, size_t hdr_stride, uint16_t* out, size_t out_stride,
                         size_t width, size_t height, const DeepRoundTripOp& op, int mixes,
                         std::string* error) {
  if (hdr == nullptr || out == nullptr || !op) {
    if (error != nullptr) *error = "hdr round trip got null arguments";
    return false;
  }
  if (width == 0 || height == 0) return true;

  /* 16 位整数布局：4 通道 × 2 字节。上游 normal 上下文同宽同高（DeepLayers.swift:102）。 */
  const size_t normal_stride = width * 8u;
  std::vector<uint16_t> normal(width * height * 4u, 0);
  std::vector<float> scales(width * height, 1.0f);
  deep_hdr_normalize(hdr, hdr_stride, normal.data(), normal_stride, scales.data(), width, height);

  std::vector<uint16_t> merged(width * height * 4u, 0);
  if (!deep_round_trip(normal.data(), normal_stride, merged.data(), normal_stride, width, height, op,
                       mixes, error)) {
    return false;
  }

  /* restore 写 half：颜色按 scales 乘回去，alpha 原样（内核自己按预乘处理）。 */
  deep_hdr_restore(merged.data(), normal_stride, scales.data(), out, out_stride, width, height);
  return true;
}

}  // namespace compositor
