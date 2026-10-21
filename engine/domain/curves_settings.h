// AI生成
/*
 * curves_settings.h — kind 2（Curves）settings 的控制点读写与结构校验。
 *
 * 与上游 `Compositor/Document/Curves.swift` 同口径：
 *   `curves.channels` 是 4 个通道的控制点列表（0 = RGB 合成，1..3 = 红/绿/蓝）；
 *   每通道 2..32 个点，x 严格递增、首点 x = 0、末点 x = 255，x/y 都在 0..255。
 * 点既接受上游 Codable 的对象形式 `{"x":N,"y":N}`，也接受紧凑的 `[x, y]` 形式
 * （`.comp` 互通里两种都出现过；求值侧 `adjustment_eval.cpp` 读法相同）。
 *
 * 分工：
 *   - `curves_read_points` / `curves_set_points` 供 C ABI 的面板接口使用（纯函数，
 *     只在 settings JSON 上操作，不碰文档与撤销栈）；
 *   - `curves_validate_settings` 由 `adjustment_validate_settings` 调用，把变长控制点
 *     的结构约束补进写侧校验（静态参数表表达不了数组）。
 */
#ifndef COMPOSITOR_DOMAIN_CURVES_SETTINGS_H
#define COMPOSITOR_DOMAIN_CURVES_SETTINGS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace compositor {

/* 与 ABI `COMP_CURVE_CHANNEL_COUNT` 数值一致；0 = RGB 合成，1..3 = R/G/B。 */
constexpr uint32_t kCurveChannelCount = 4;
/* 上游 `CurvesSettings.isValid`：每通道 2..32 个点。 */
constexpr size_t kCurveMinPoints = 2;
constexpr size_t kCurveMaxPoints = 32;

/*
 * 读取某通道的控制点（按 x 升序，同 x 去重）。
 * settings 非法、通道越界或通道缺失时返回空数组（调用方按恒等处理）。
 */
std::vector<std::pair<double, double>> curves_read_points(const std::string& settings_json,
                                                          uint32_t channel);

/*
 * 改写某通道的控制点，保留 settings 里的其余键（其他通道、`curves.channel` 等）。
 * points 为空 ⇒ 写入恒等曲线 (0,0)-(255,255)（数组下标有语义，不能删除元素）。
 * 传入顺序不要求有序：内部按 x 排序后校验（相邻点交叉 = 交换，与编辑器语义一致）。
 * 失败原因写入 error（UTF-8），返回 false。
 */
bool curves_set_points(const std::string& settings_json, uint32_t channel,
                       const std::vector<std::pair<double, double>>& points, std::string* out_json,
                       std::string* error);

/*
 * 结构校验：`curves` 若存在必须是对象；`channels` 若存在必须是数组（最多 4 项）；
 * 每项若存在必须是控制点数组：空数组按恒等放行，否则 2..32 点、x 严格递增、
 * 首点 x = 0、末点 x = 255、x/y 都在 [0,255] 且有限。
 */
bool curves_validate_settings(const std::string& settings_json, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_CURVES_SETTINGS_H */
