// AI生成
/*
 * camera_raw_tables.h — Photoshop Camera Raw 的实测表（Light / Color 面板）。
 *
 * 语义唯一真源是上游 `Compositor/Document/CameraRawTables.swift`：763 张
 * 17³ 色彩表，每张是「一个滑块在一个值上」时 Photoshop 实际渲染出来的颜色。
 * 本仓把上游的 CRT3（xz 压缩）离线转成 CRT4（zlib 压缩，见
 * `tools/convert_camera_raw_tables.py`），随仓库分发 `resources/camera_raw_tables.bin`；
 * 引擎不引入 LZMA 解码器，树内 zlib 已够用。
 *
 * 分层：
 *   - 本文件只做「表」：解码、取表、stage 装配、表合成、亮度测量。全部是
 *     **无设置、无文档**的纯函数，可以单独测；
 *   - 设置结构、JSON 解析、曲线求值、完整管线在 `camera_raw.h/cpp`。
 *
 * 表序号（与上游逐字一致，勿改）：
 *   白平衡 9×9（温度按行、色调按列，各 −100..100 步长 25）= 0..80；
 *   曝光 −5..5 步长 0.5 = 81..101；
 *   高光/阴影/白色/黑色/饱和度/自然饱和度各 9 张；
 *   对比度 / 高光 / 阴影各 7×7（亮度 × 滑块值）= 138..284；
 *   颜色混合器 24 组 ×5；校准 6 组 ×5；参数曲线 4 组 ×5；
 *   颜色分级：影子/高光在混合 0、四轮在 50、影子/高光在 100，最后是
 *   四个亮度滑块。
 */
#ifndef COMPOSITOR_DOMAIN_CAMERA_RAW_TABLES_H
#define COMPOSITOR_DOMAIN_CAMERA_RAW_TABLES_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "domain/layer.h"
#include "imaging/AdjustPixels.h"

namespace compositor {

/* 每轴取样数、合成网格、表数（上游 CameraRawTables.size / grid / count）。 */
constexpr int kCameraRawSize = 17;
constexpr int kCameraRawGrid = 33;
constexpr int kCameraRawCount = 763;
constexpr int kCameraRawCells = kCameraRawSize * kCameraRawSize * kCameraRawSize;
constexpr int kCameraRawEntries = kCameraRawCells * 3;

/* 表序号（CameraRawTables.swift:17-26）。 */
constexpr int kCameraRawExposureStart = 81;
constexpr int kCameraRawWhitesStart = 102;
constexpr int kCameraRawBlacksStart = 111;
constexpr int kCameraRawSaturationStart = 120;
constexpr int kCameraRawVibranceStart = 129;
constexpr int kCameraRawContrastStart = 138;
constexpr int kCameraRawHighlightsStart = 187;
constexpr int kCameraRawShadowsStart = 236;
constexpr int kCameraRawMixerStart = 285;
constexpr int kCameraRawCalibrationStart = 405;
constexpr int kCameraRawParametricStart = 435;
constexpr int kCameraRawGradingStart = 455;
constexpr int kCameraRawGradingLuminanceStart = 743;

/*
 * 自适应滑块的七个亮度基准与七个滑块值（CameraRawTables.swift:27-28）：
 * 对比度读「最亮通道的均值」，高光读「最亮通道的几何均值」，阴影读「亮度均值」。
 */
constexpr double kCameraRawBrightnesses[7] = {53, 80, 104, 130, 160, 190, 224};
constexpr double kCameraRawAdaptiveValues[7] = {-100, -50, -25, 0, 25, 50, 100};

/* 亮度测量结果，sRGB 0..255；默认值 = 上游 `Brightness()` 的缺省。 */
struct CameraRawBrightness {
  double brightest = 128.0;
  double luminance = 128.0;
  double log_brightest = 128.0;
};

/*
 * 一套解码后的表。不可变；用 shared_ptr<const ...> 挂到 Engine 上共享
 * （一个引擎一次加载，所有文档复用）。
 */
class CameraRawTableSet {
 public:
  /*
   * 解码 CRT4 文件。失败时返回 nullptr 并把原因写进 `error`：
   *   - 头不足 10 字节 / 魔数不是 "CRT4"；
   *   - size != 17、count != 763、step == 0；
   *   - zlib 流损坏，或解压后长度不是 count * size³ * step。
   *
   * 负载是**逐通道、逐格**的差分（上游 CRT3 原样）：每张表每个通道一条
   * 运行和，`value = level[axis] + int8(running) * step`，夹到 0..255。
   */
  static std::shared_ptr<const CameraRawTableSet> decode(const void* data, size_t size,
                                                         std::string* error);

  /* 第 index 张表的 17³×3 字节；越界返回 nullptr。 */
  const uint8_t* table(int index) const;

  /* 曝光 21 张表（镜头暗角校正按位置取曝光量）。 */
  const uint8_t* exposure_tables() const { return table(kCameraRawExposureStart); }

  size_t table_count() const { return table_count_; }
  bool valid() const { return table_count_ == kCameraRawCount; }

 private:
  std::vector<uint8_t> bytes_;
  size_t table_count_ = 0;
};

/* 一个 stage 的一部分：一张表（nullptr = 恒等）与权重。 */
struct CameraRawStagePart {
  const uint8_t* table = nullptr;
  double weight = 0.0;
};

/* 把至多 8 个部分装成一个内核 stage（与上游 `blend` 逐字一致）。 */
CameraRawStage camera_raw_make_stage(const CameraRawStagePart* parts, int count);

/*
 * 升序折线上 value 落点的下标与小数部分（上游 `between(_:in:)`）：
 * value 先夹到两端；下标保证 <= count-2，因此总会取到下标 +1。
 */
void camera_raw_between(double value, const double* points, int count, int* index,
                        double* fraction);

/* 一组连续表的线性混合：start 起、低值 low、步长 step、共 tables 张。 */
CameraRawStage camera_raw_table_stage(const CameraRawTableSet& tables, int start, double value,
                                      double low, double step, int count);

/* 参数曲线 / 校准 / 混合器 / 分级亮度滑块：−100..100 步长 50 的 5 张表。 */
CameraRawStage camera_raw_fifty_stage(const CameraRawTableSet& tables, int start, double value);

/* 温度 / 色调的 9×9 网格，四角混合（白平衡 0..80 号表）。 */
CameraRawStage camera_raw_white_balance_stage(const CameraRawTableSet& tables, double temperature,
                                              double tint);

/* 自适应滑块：亮度行 × 滑块值列的四张表混合。 */
CameraRawStage camera_raw_adaptive_stage(const CameraRawTableSet& tables, int start, double value,
                                         double brightness);

/* 合成网格上的「无变化」表（grid³×3）。 */
std::vector<float> camera_raw_identity_table();

/* stages 依次作用在恒等网格上；stages 为空或 count <= 0 时返回空（上游 `compose` 返回 nil）。 */
std::vector<float> camera_raw_compose_stages(const CameraRawStage* stages, int count);

/* 把 stages 折叠进既有网格（上游 `camera_raw_compose_onto`）。空 stage 无操作。 */
void camera_raw_compose_onto(std::vector<float>* table, const CameraRawStage* stages, int count);

/*
 * 曲线折叠进合成表（上游 `camera_raw_table_curves`）。LUT 都是 256 项，
 * 值域 0..1；`tone` 必填，三个通道 LUT 可空（空 = 恒等）。
 */
void camera_raw_apply_table_curves(std::vector<float>* table, const float* tone, const float* red,
                                   const float* green, const float* blue);

/*
 * 自适应滑块读的亮度（上游 `brightness`，`camera_raw_statistics * 255`）。
 * `pixels` 必须是 RGBA8 预乘；其它格式返回缺省值（调用方先落到 8 位工作副本）。
 */
CameraRawBrightness camera_raw_measure_brightness(const ImageBuffer& pixels);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_CAMERA_RAW_TABLES_H */
