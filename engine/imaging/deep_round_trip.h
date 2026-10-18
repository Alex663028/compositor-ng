// AI生成
/*
 * deep_round_trip.h — 16 位 / HDR 图层穿过 8 位操作的唯一实现。
 *
 * 从 domain/tool_ops.cpp 抽出：C5 让调整求值也落在 half 表面上，同一条
 * normalize → split/merge → restore 的算法若在调整求值里再写一份，两处一旦
 * 漂移，同一个调整在「工具路径」和「合成路径」上会给出不同像素。因此本模块
 * 是唯一实现，tool_ops 与 compositing/adjustment_eval 都调用它。
 *
 * 口径逐字对齐上游 DeepLayer.run / DeepLayer.normalizing（DeepLayers.swift:196）：
 *   - 不混合通道时跑 4 趟：step = 0,1,2,4，每档 channel = -1；
 *   - mixes 时跑 10 趟：step 0 加 (1/2/4) × (红/绿/蓝各自单抬)，供跨通道
 *     操作使用（上游 `run(mixesChannels: true)`）；
 *   - HDR 版本先 deep_hdr_normalize 把高光除回白内，跑完 deep_hdr_restore 乘回。
 *
 * 所有 stride 以**字节**计；src/dst 可以指向同一对象（结果先落在本地缓冲）。
 */
#ifndef COMPOSITOR_IMAGING_DEEP_ROUND_TRIP_H
#define COMPOSITOR_IMAGING_DEEP_ROUND_TRIP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace compositor {

/* 一个作用在 8 位预乘 RGBA 缓冲上的操作：buf / width / height / stride(字节)。 */
using DeepRoundTripOp =
    std::function<bool(uint8_t*, size_t, size_t, size_t)>;

/* 16 位整数图层（预乘 RGBA16）穿过 op。mixes 见文件头。 */
bool deep_round_trip(const uint16_t* deep, size_t deep_stride, uint16_t* out, size_t out_stride,
                     size_t width, size_t height, const DeepRoundTripOp& op, int mixes,
                     std::string* error);

/* HDR（预乘 half）图层穿过 op：normalize → 16 位往返 → restore。 */
bool deep_hdr_round_trip(const uint16_t* hdr, size_t hdr_stride, uint16_t* out, size_t out_stride,
                         size_t width, size_t height, const DeepRoundTripOp& op, int mixes,
                         std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_IMAGING_DEEP_ROUND_TRIP_H */
