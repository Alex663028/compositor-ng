// AI生成
/*
 * camera_raw_tables.cpp — Camera Raw 实测表的解码与 stage 装配。
 *
 * 与上游 CameraRawTables.swift 的对应关系逐条写在函数上方；这里的每个
 * 公式都照抄上游，不做「等效改写」，因为表与权重差一点都会变成可见色偏。
 */
#include "domain/camera_raw_tables.h"

#include <cmath>
#include <cstring>
#include <vector>

#if defined(__has_include)
#  if __has_include("zlib.h")
#    define COMPOSITOR_CAMERA_RAW_ZLIB 1
#  endif
#endif

#ifdef COMPOSITOR_CAMERA_RAW_ZLIB
#  include "zlib.h"
#endif

namespace compositor {
namespace {

constexpr size_t kHeaderSize = 10;

uint16_t read_u16(const uint8_t* bytes, size_t at) {
  return static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8));
}

int level_for_axis(int axis) {
  return static_cast<int>(std::lround(static_cast<double>(axis) * 255.0 /
                                      static_cast<double>(kCameraRawSize - 1)));
}

/* 上游 `running &+= byte` 的有符号读法：UInt8 的补码在 Int8 上解释。 */
int signed_byte(uint8_t value) { return value < 128 ? static_cast<int>(value)
                                                    : static_cast<int>(value) - 256; }

}  // namespace

std::shared_ptr<const CameraRawTableSet> CameraRawTableSet::decode(const void* data, size_t size,
                                                                   std::string* error) {
  auto fail = [&](const std::string& message) -> std::shared_ptr<const CameraRawTableSet> {
    if (error != nullptr) *error = message;
    return nullptr;
  };
  if (data == nullptr) return fail("camera raw table blob is null");
  if (size <= kHeaderSize) return fail("camera raw table blob is too short");
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  if (std::memcmp(bytes, "CRT4", 4) != 0) {
    return fail("camera raw table blob does not start with CRT4");
  }
  const int raw_size = read_u16(bytes, 4);
  const int raw_count = read_u16(bytes, 6);
  const int step = read_u16(bytes, 8);
  if (raw_size != kCameraRawSize) {
    return fail("camera raw table size is " + std::to_string(raw_size) + ", expected " +
                std::to_string(kCameraRawSize));
  }
  if (raw_count != kCameraRawCount) {
    return fail("camera raw table count is " + std::to_string(raw_count) + ", expected " +
                std::to_string(kCameraRawCount));
  }
  if (step == 0) return fail("camera raw table step is zero");

#ifndef COMPOSITOR_CAMERA_RAW_ZLIB
  return fail("zlib is not available in this build");
#else
  const size_t expected = static_cast<size_t>(raw_count) * kCameraRawEntries;
  std::vector<uint8_t> planes(expected);
  uLongf unpacked = static_cast<uLongf>(expected);
  const uint8_t* payload = bytes + kHeaderSize;
  const size_t payload_size = size - kHeaderSize;
  if (payload_size > static_cast<size_t>(0xFFFFFFFFu)) {
    return fail("camera raw table payload is too large");
  }
  const int status = ::uncompress(planes.data(), &unpacked, payload,
                                  static_cast<uLong>(payload_size));
  if (status != Z_OK) {
    return fail("camera raw table payload is not a valid zlib stream");
  }
  if (unpacked != expected) {
    return fail("camera raw table payload is " + std::to_string(unpacked) + " bytes, expected " +
                std::to_string(expected));
  }

  auto decoded = std::shared_ptr<CameraRawTableSet>(new CameraRawTableSet());
  decoded->bytes_.assign(static_cast<size_t>(raw_count) * kCameraRawEntries, 0);
  decoded->table_count_ = static_cast<size_t>(raw_count);

  const int cells = kCameraRawCells;
  for (int table = 0; table < raw_count; ++table) {
    for (int channel = 0; channel < 3; ++channel) {
      const size_t base = (static_cast<size_t>(table) * 3u + static_cast<size_t>(channel)) *
                          static_cast<size_t>(cells);
      uint8_t running = 0;
      for (int cell = 0; cell < cells; ++cell) {
        running = static_cast<uint8_t>(running + planes[base + static_cast<size_t>(cell)]);
        const int axis = channel == 0 ? cell / (kCameraRawSize * kCameraRawSize)
                                      : (channel == 1 ? (cell / kCameraRawSize) % kCameraRawSize
                                                      : cell % kCameraRawSize);
        const int value = level_for_axis(axis) + signed_byte(running) * step;
        const int clamped = value < 0 ? 0 : (value > 255 ? 255 : value);
        decoded->bytes_[static_cast<size_t>(table) * kCameraRawEntries +
                        static_cast<size_t>(cell) * 3u + static_cast<size_t>(channel)] =
            static_cast<uint8_t>(clamped);
      }
    }
  }
  if (error != nullptr) error->clear();
  return decoded;
#endif
}

const uint8_t* CameraRawTableSet::table(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= table_count_) return nullptr;
  return bytes_.data() + static_cast<size_t>(index) * kCameraRawEntries;
}

/* ============================================================ stage 装配 */

CameraRawStage camera_raw_make_stage(const CameraRawStagePart* parts, int count) {
  CameraRawStage stage{};
  if (parts == nullptr) return stage;
  if (count > 8) count = 8;
  for (int i = 0; i < count; ++i) {
    stage.table[i] = parts[i].table;
    stage.weight[i] = static_cast<float>(parts[i].weight);
  }
  return stage;
}

void camera_raw_between(double value, const double* points, int count, int* index,
                        double* fraction) {
  if (index == nullptr || fraction == nullptr) return;
  *index = 0;
  *fraction = 0.0;
  if (points == nullptr || count < 2) return;
  if (value < points[0]) value = points[0];
  if (value > points[count - 1]) value = points[count - 1];
  int cursor = 0;
  while (cursor < count - 2 && value > points[cursor + 1]) ++cursor;
  const double span = points[cursor + 1] - points[cursor];
  *index = cursor;
  *fraction = span != 0.0 ? (value - points[cursor]) / span : 0.0;
}

CameraRawStage camera_raw_table_stage(const CameraRawTableSet& tables, int start, double value,
                                      double low, double step, int count) {
  if (count < 2 || step == 0.0) return CameraRawStage{};
  const double position = std::min(static_cast<double>(count - 1),
                                   std::max(0.0, (value - low) / step));
  int index = static_cast<int>(position);
  if (index > count - 2) index = count - 2;
  const double fraction = position - static_cast<double>(index);
  const CameraRawStagePart parts[2] = {
      {tables.table(start + index), 1.0 - fraction},
      {tables.table(start + index + 1), fraction},
  };
  return camera_raw_make_stage(parts, 2);
}

CameraRawStage camera_raw_fifty_stage(const CameraRawTableSet& tables, int start, double value) {
  return camera_raw_table_stage(tables, start, value, -100.0, 50.0, 5);
}

CameraRawStage camera_raw_white_balance_stage(const CameraRawTableSet& tables, double temperature,
                                              double tint) {
  int row = 0, column = 0;
  double down = 0.0, across = 0.0;
  {
    /* 温标/色调各 9 张，−100..100 步长 25：与上游 `between(_, low: -100, step: 25, tables: 9)`
     * 相同，这里给出显式采样点是因为 `between` 收的是折线表而不是 low/step。 */
    const double values[9] = {-100, -75, -50, -25, 0, 25, 50, 75, 100};
    camera_raw_between(temperature, values, 9, &row, &down);
    camera_raw_between(tint, values, 9, &column, &across);
  }
  const CameraRawStagePart parts[4] = {
      {tables.table(row * 9 + column), (1.0 - down) * (1.0 - across)},
      {tables.table(row * 9 + column + 1), (1.0 - down) * across},
      {tables.table((row + 1) * 9 + column), down * (1.0 - across)},
      {tables.table((row + 1) * 9 + column + 1), down * across},
  };
  return camera_raw_make_stage(parts, 4);
}

CameraRawStage camera_raw_adaptive_stage(const CameraRawTableSet& tables, int start, double value,
                                         double brightness) {
  int row = 0, column = 0;
  double down = 0.0, across = 0.0;
  camera_raw_between(brightness, kCameraRawBrightnesses, 7, &row, &down);
  camera_raw_between(value, kCameraRawAdaptiveValues, 7, &column, &across);
  const int width = 7;
  const int base = start + row * width + column;
  const CameraRawStagePart parts[4] = {
      {tables.table(base), (1.0 - down) * (1.0 - across)},
      {tables.table(base + 1), (1.0 - down) * across},
      {tables.table(base + width), down * (1.0 - across)},
      {tables.table(base + width + 1), down * across},
  };
  return camera_raw_make_stage(parts, 4);
}

/* ============================================================ 合成 */

std::vector<float> camera_raw_identity_table() {
  std::vector<float> out(static_cast<size_t>(kCameraRawGrid) * kCameraRawGrid * kCameraRawGrid * 3u,
                         0.0f);
  ::camera_raw_compose(out.data(), kCameraRawGrid, nullptr, 0, kCameraRawSize);
  return out;
}

std::vector<float> camera_raw_compose_stages(const CameraRawStage* stages, int count) {
  if (stages == nullptr || count <= 0) return {};
  std::vector<float> out(static_cast<size_t>(kCameraRawGrid) * kCameraRawGrid * kCameraRawGrid * 3u,
                         0.0f);
  ::camera_raw_compose(out.data(), kCameraRawGrid, stages, count, kCameraRawSize);
  return out;
}

void camera_raw_compose_onto(std::vector<float>* table, const CameraRawStage* stages, int count) {
  if (table == nullptr || stages == nullptr || count <= 0) return;
  const size_t entries = static_cast<size_t>(kCameraRawGrid) * kCameraRawGrid * kCameraRawGrid;
  if (table->size() != entries * 3u) return;
  ::camera_raw_compose_onto(table->data(), kCameraRawGrid, stages, count, kCameraRawSize);
}

void camera_raw_apply_table_curves(std::vector<float>* table, const float* tone, const float* red,
                                   const float* green, const float* blue) {
  if (table == nullptr) return;
  const size_t entries = static_cast<size_t>(kCameraRawGrid) * kCameraRawGrid * kCameraRawGrid;
  if (table->size() != entries * 3u) return;

  /* 内核按 256 项 LUT 直取，不给空指针兜底；缺省用恒等 LUT，语义与上游
   * `channelTable` 对线性曲线返回的表一致。 */
  float identity[256];
  for (int i = 0; i < 256; ++i) identity[i] = static_cast<float>(i) / 255.0f;
  const float* tone_lut = tone != nullptr ? tone : identity;
  const float* red_lut = red != nullptr ? red : identity;
  const float* green_lut = green != nullptr ? green : identity;
  const float* blue_lut = blue != nullptr ? blue : identity;
  ::camera_raw_table_curves(table->data(), kCameraRawGrid, tone_lut, red_lut, green_lut, blue_lut);
}

/* ============================================================ 亮度 */

CameraRawBrightness camera_raw_measure_brightness(const ImageBuffer& pixels) {
  CameraRawBrightness result;
  if (pixels.format != COMP_PIXEL_RGBA8 || pixels.width <= 0 || pixels.height <= 0 ||
      pixels.pixel_data().empty()) {
    return result;
  }
  double out[3] = {0.0, 0.0, 0.0};
  ::camera_raw_statistics(pixels.pixel_data().data(), static_cast<size_t>(pixels.width),
                          static_cast<size_t>(pixels.height), pixels.stride(), out);
  result.brightest = out[0] * 255.0;
  result.luminance = out[1] * 255.0;
  result.log_brightest = out[2] * 255.0;
  return result;
}

}  // namespace compositor
