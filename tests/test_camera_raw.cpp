// AI生成
/*
 * test_camera_raw.cpp — Camera Raw（F4）端到端与领域契约。
 *
 * 覆盖：
 *   - CRT4 表 blob 的加载/查询/坏数据拒绝（含「失败不破坏旧表」的语义）；
 *   - 恒等设置：不开事务、不动像素、out_changed = 0；
 *   - 曝光改变像素且恰好一步撤销；
 *   - JSON 严格性：未知键 / 类型不符 / 标量越界 / 嵌套未知键一律拒绝；
 *   - 非默认几何 -> COMP_ERR_UNSUPPORTED；未加载表 -> COMP_ERR_STATE；
 *   - 图层不存在 -> COMP_ERR_NOT_FOUND；
 *   - 可折叠通路（folded）与逐像素通路（per-pixel）的 note 区分；
 *   - 颗粒按 seed 确定、不同 seed 不同；
 *   - 细节 / 光学真的改像素；
 *   - note 两段式拷贝（capacity = 0 只回报长度）；
 *   - 16 位图层落 8 位工作副本；
 *   - 全透明像素不被曝光改写。
 *
 * 表的数值来自 Photoshop 实测，断言只看「方向 / 是否改变 / 确定性」，
 * 不钉具体字节——那属于 AdjustPixels 已由 test_camera_raw_align.cpp 钉住的部分。
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "compositor/comp_camera_raw.h"
#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "domain/camera_raw.h"
#include "domain/camera_raw_tables.h"
#include "domain/document.h"
#include "domain/layer.h"
#include "engine.h"
#include "test_framework.h"

namespace {

using compositor::Document;
using compositor::Engine;
using compositor::ImageBuffer;
using compositor::Layer;

const char* kTablePath = "resources/camera_raw_tables.bin";

std::vector<uint8_t> read_table_blob() {
  std::ifstream input(kTablePath, std::ios::binary);
  if (!input) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-camera-raw-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

comp_engine as_engine_handle(Engine* engine) {
  return reinterpret_cast<comp_engine>(engine);
}

comp_document as_document_handle(Document* document) {
  return reinterpret_cast<comp_document>(document);
}

/* 加载仓库自带的实测表；成功才继续跑依赖表的用例。 */
bool load_tables(Engine* engine) {
  const std::vector<uint8_t> blob = read_table_blob();
  if (blob.empty()) return false;
  if (comp_engine_load_camera_raw_tables(as_engine_handle(engine), blob.data(), blob.size()) !=
      COMP_OK) {
    return false;
  }
  int32_t loaded = 0;
  return comp_engine_camera_raw_tables_loaded(as_engine_handle(engine), &loaded) == COMP_OK &&
         loaded == 1;
}

comp_layer_id add_solid_layer(Document* document, uint8_t r, uint8_t g, uint8_t b) {
  comp_layer_desc desc{};
  desc.name = "solid";
  desc.transform = comp_rect{0, 0, document->state().width, document->state().height};
  comp_layer_id id = 0;
  Layer* layer = document->create_layer(desc, &id);
  if (layer == nullptr) return 0;
  layer->pixels = ImageBuffer::solid(document->state().width, document->state().height,
                                     COMP_PIXEL_RGBA8, r, g, b, 255);
  return id;
}

std::vector<uint8_t> pixels_of(const Document& document, comp_layer_id id) {
  const Layer* layer = document.find(id);
  if (layer == nullptr) return {};
  return layer->pixels.pixel_data();
}

double mean_luma(const std::vector<uint8_t>& rgba) {
  if (rgba.size() < 4) return 0.0;
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
    ++count;
  }
  return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

size_t undo_depth(const Document& document) { return document.history().undo_depth(); }

}  // namespace

/* ── 1. 表的加载与坏数据拒绝 ─────────────────────────────────────────── */
COMP_TEST(camera_raw_tables_reject_a_corrupt_blob) {
  Engine engine(test_engine_desc());
  comp_engine handle = as_engine_handle(&engine);

  int32_t loaded = -1;
  COMP_CHECK(comp_engine_camera_raw_tables_loaded(handle, &loaded) == COMP_OK);
  COMP_CHECK_MSG(loaded == 0, "新引擎不应声称已加载表");

  const uint8_t junk[16] = {'N', 'O', 'T', '4', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  COMP_CHECK_MSG(comp_engine_load_camera_raw_tables(handle, junk, sizeof(junk)) ==
                     COMP_ERR_DECODE,
                 "魔数不是 CRT4 应报 COMP_ERR_DECODE");
  COMP_CHECK(comp_engine_camera_raw_tables_loaded(handle, &loaded) == COMP_OK);
  COMP_CHECK_MSG(loaded == 0, "坏 blob 不应把引擎标记成已加载");

  COMP_CHECK_MSG(comp_engine_load_camera_raw_tables(handle, nullptr, 0) ==
                     COMP_ERR_INVALID_ARGUMENT,
                 "空指针应报 COMP_ERR_INVALID_ARGUMENT");

  /* 截断的合法头 + 伪负载：解压长度对不上，仍是 DECODE。 */
  std::vector<uint8_t> truncated(64, 0);
  truncated[0] = 'C';
  truncated[1] = 'R';
  truncated[2] = 'T';
  truncated[3] = '4';
  COMP_CHECK_MSG(comp_engine_load_camera_raw_tables(handle, truncated.data(), truncated.size()) ==
                     COMP_ERR_DECODE,
                 "截断负载应报 COMP_ERR_DECODE");
}

COMP_TEST(camera_raw_tables_load_from_the_shipped_blob) {
  const std::vector<uint8_t> blob = read_table_blob();
  COMP_CHECK_MSG(!blob.empty(), "仓库应随带 resources/camera_raw_tables.bin");

  std::string error;
  std::shared_ptr<const compositor::CameraRawTableSet> tables =
      compositor::CameraRawTableSet::decode(blob.data(), blob.size(), &error);
  COMP_CHECK_MSG(tables != nullptr, error.c_str());
  if (tables != nullptr) {
    COMP_CHECK(tables->valid());
    COMP_CHECK(tables->table_count() == 763u);
    COMP_CHECK(tables->table(0) != nullptr);
    COMP_CHECK_MSG(tables->table(763) == nullptr, "越界表序号应返回 nullptr");
  }

  Engine engine(test_engine_desc());
  COMP_CHECK_MSG(load_tables(&engine), "ABI 加载实测表应成功");

  /* 失败路径不破坏旧表：再喂一次坏 blob，查询仍为已加载。 */
  const uint8_t junk[10] = {0};
  COMP_CHECK(comp_engine_load_camera_raw_tables(as_engine_handle(&engine), junk, sizeof(junk)) ==
             COMP_ERR_DECODE);
  int32_t loaded = 0;
  COMP_CHECK(comp_engine_camera_raw_tables_loaded(as_engine_handle(&engine), &loaded) == COMP_OK);
  COMP_CHECK_MSG(loaded == 1, "失败的加载不应清掉已加载的表");
}

/* ── 2. 恒等设置：无事务、无像素变化 ─────────────────────────────────── */
COMP_TEST(camera_raw_identity_settings_leave_pixels_and_history_alone) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 90, 110, 130);
  COMP_CHECK(id != 0);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t depth = undo_depth(document);

  int32_t changed = -1;
  size_t note_size = 0;
  COMP_CHECK_MSG(comp_camera_raw_apply(as_document_handle(&document), id, "{}", 1.0, 0, &changed,
                                       nullptr, 0, &note_size) == COMP_OK,
                 "空设置对象应被接受为恒等设置");
  COMP_CHECK_MSG(changed == 0, "恒等设置 out_changed 必须是 0");
  COMP_CHECK_MSG(note_size > 1, "note 两段式查询应回报长度");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "恒等设置不应改像素");
  COMP_CHECK_MSG(undo_depth(document) == depth, "恒等设置不应开事务");

  /* 空串等价 "{}"。 */
  changed = -1;
  COMP_CHECK(comp_camera_raw_apply(as_document_handle(&document), id, "", 1.0, 0, &changed,
                                   nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth(document) == depth);
}

/* ── 3. 曝光：真的提亮，且恰好一步撤销 ───────────────────────────────── */
COMP_TEST(camera_raw_exposure_brightens_and_is_one_undo_step) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 100, 100, 100);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t depth = undo_depth(document);

  int32_t changed = -1;
  COMP_CHECK(comp_camera_raw_apply(as_document_handle(&document), id, R"({"exposure":1})", 1.0, 0,
                                   &changed, nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "非恒等设置应报告 changed");
  const std::vector<uint8_t> after = pixels_of(document, id);
  COMP_CHECK_MSG(after != before, "曝光应改变像素");
  COMP_CHECK_MSG(mean_luma(after) > mean_luma(before) + 5.0, "+1 档曝光应明显提亮");
  COMP_CHECK_MSG(undo_depth(document) == depth + 1, "整次 Camera Raw 恰好一步撤销");
  COMP_CHECK_MSG(after[3] == 255 && after[7] == 255, "不透明像素的 alpha 应保持");
}

/* ── 4. JSON 严格性 ─────────────────────────────────────────────────── */
COMP_TEST(camera_raw_json_rejects_unknown_keys_and_bad_scalars) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 60, 70, 80);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t depth = undo_depth(document);
  comp_document handle = as_document_handle(&document);

  int32_t changed = -1;
  /* 根级未知键。 */
  COMP_CHECK_MSG(comp_camera_raw_apply(handle, id, R"({"exposure":1,"bogus":1})", 1.0, 0, &changed,
                                       nullptr, 0, nullptr) == COMP_ERR_INVALID_ARGUMENT,
                 "未知键必须拒绝，不能静默忽略");
  /* 嵌套未知键。 */
  COMP_CHECK(comp_camera_raw_apply(handle, id, R"({"curve":{"nope":1}})", 1.0, 0, &changed,
                                   nullptr, 0, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  /* 类型不符。 */
  COMP_CHECK(comp_camera_raw_apply(handle, id, R"({"exposure":"1"})", 1.0, 0, &changed, nullptr, 0,
                                   nullptr) == COMP_ERR_INVALID_ARGUMENT);
  /* 越界标量（曝光上限 5）。 */
  COMP_CHECK_MSG(comp_camera_raw_apply(handle, id, R"({"exposure":9})", 1.0, 0, &changed, nullptr,
                                       0, nullptr) == COMP_ERR_INVALID_ARGUMENT,
                 "越界标量必须拒绝而不是 clamp");
  /* 非法 JSON。 */
  COMP_CHECK(comp_camera_raw_apply(handle, id, "{", 1.0, 0, &changed, nullptr, 0, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  /* 根不是对象。 */
  COMP_CHECK(comp_camera_raw_apply(handle, id, "[]", 1.0, 0, &changed, nullptr, 0, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  /* NULL 指针。 */
  COMP_CHECK(comp_camera_raw_apply(handle, id, nullptr, 1.0, 0, &changed, nullptr, 0, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);

  COMP_CHECK_MSG(pixels_of(document, id) == before, "解析失败不应产生任何像素修改");
  COMP_CHECK_MSG(undo_depth(document) == depth, "解析失败不应开事务");
}

COMP_TEST(camera_raw_non_default_geometry_is_unsupported) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 100, 100, 100);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t depth = undo_depth(document);

  int32_t changed = 0;
  COMP_CHECK_MSG(comp_camera_raw_apply(as_document_handle(&document), id,
                                       R"({"geometry":{"vertical":10}})", 1.0, 0, &changed,
                                       nullptr, 0, nullptr) == COMP_ERR_UNSUPPORTED,
                 "F4 不做透视变换，非默认几何应报 UNSUPPORTED");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "不支持的几何不应动像素");
  COMP_CHECK_MSG(undo_depth(document) == depth, "不支持的几何不应开事务");
}

/* ── 5. 状态类错误 ─────────────────────────────────────────────────── */
COMP_TEST(camera_raw_requires_loaded_tables) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 100, 100, 100);

  int32_t changed = 0;
  COMP_CHECK_MSG(comp_camera_raw_apply(as_document_handle(&document), id, R"({"exposure":1})", 1.0,
                                       0, &changed, nullptr, 0, nullptr) == COMP_ERR_STATE,
                 "未加载表时非恒等设置应报 COMP_ERR_STATE");
  /* 恒等设置在缺表时仍应短路成功（它根本不需要表）。 */
  COMP_CHECK(comp_camera_raw_apply(as_document_handle(&document), id, "{}", 1.0, 0, &changed,
                                   nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK(changed == 0);
}

COMP_TEST(camera_raw_missing_layer_is_not_found) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");

  int32_t changed = 0;
  COMP_CHECK(comp_camera_raw_apply(as_document_handle(&document), 123456, R"({"exposure":1})", 1.0,
                                   0, &changed, nullptr, 0, nullptr) == COMP_ERR_NOT_FOUND);
  /* 坏句柄也应是 INVALID_ARGUMENT，而不是解引用。 */
  COMP_CHECK(comp_camera_raw_apply(nullptr, 1, "{}", 1.0, 0, &changed, nullptr, 0, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
}

/* ── 6. 两条颜色通路：折叠 vs 逐像素 ─────────────────────────────────── */
COMP_TEST(camera_raw_note_reports_folded_and_per_pixel_paths) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 100, 120, 140);
  comp_document handle = as_document_handle(&document);

  char note[256] = {0};
  size_t note_size = 0;
  int32_t changed = 0;
  /* 参数曲线走实测表 + 点曲线可折叠 → 单次查找。 */
  COMP_CHECK(comp_camera_raw_apply(handle, id,
                                   R"({"curve":{"rgb":[{"x":0,"y":0},{"x":1,"y":0.8}]}})", 1.0,
                                   0, &changed, note, sizeof(note), &note_size) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(std::string(note).find("folded") != std::string::npos,
                 "可折叠的颜色设置应报告 folded 通路");
  COMP_CHECK(note_size > 0 && note_size <= sizeof(note));

  /* refineSaturation 强制逐像素曲线通路。 */
  std::memset(note, 0, sizeof(note));
  COMP_CHECK(comp_camera_raw_apply(handle, id, R"({"curve":{"refineSaturation":40}})", 1.0, 0,
                                   &changed, note, sizeof(note), &note_size) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(std::string(note).find("per-pixel") != std::string::npos,
                 "refineSaturation 应报告 per-pixel 通路");
}

/* ── 7. note 两段式拷贝 ─────────────────────────────────────────────── */
COMP_TEST(camera_raw_note_is_copied_in_two_stages) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 100, 100, 100);
  comp_document handle = as_document_handle(&document);

  int32_t changed = 0;
  size_t needed = 0;
  COMP_CHECK(comp_camera_raw_apply(handle, id, R"({"exposure":0.5})", 1.0, 0, &changed, nullptr, 0,
                                   &needed) == COMP_OK);
  COMP_CHECK_MSG(needed > 1, "capacity = 0 时应回报含 '\\0' 的长度");

  /* 截断：写入 capacity-1 字节并在末位补 '\0'。 */
  char buffer[8];
  std::memset(buffer, 'x', sizeof(buffer));
  size_t written = 0;
  COMP_CHECK(comp_camera_raw_apply(handle, id, R"({"exposure":-0.5})", 1.0, 0, &changed, buffer,
                                   sizeof(buffer), &written) == COMP_OK);
  COMP_CHECK(written == needed);
  COMP_CHECK_MSG(std::strlen(buffer) <= sizeof(buffer) - 1, "note 必须 NUL 终止");
  COMP_CHECK_MSG(buffer[sizeof(buffer) - 1] == '\0' || std::strlen(buffer) < sizeof(buffer) - 1,
                 "截断后末位应是 '\\0' 或字符串更短");
}

/* ── 8. 颗粒按 seed 确定 ────────────────────────────────────────────── */
namespace {

std::vector<uint8_t> grain_result(uint32_t seed) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 100, 100, 100);
  if (!load_tables(&engine)) return {};
  int32_t changed = 0;
  const comp_result result =
      comp_camera_raw_apply(as_document_handle(&document), id,
                            R"({"grainAmount":60,"grainSize":50,"grainRoughness":50})", 1.0,
                            seed, &changed, nullptr, 0, nullptr);
  if (result != COMP_OK || changed != 1) return {};
  return pixels_of(document, id);
}

}  // namespace

COMP_TEST(camera_raw_grain_is_deterministic_per_seed) {
  const std::vector<uint8_t> first = grain_result(7);
  const std::vector<uint8_t> again = grain_result(7);
  const std::vector<uint8_t> other = grain_result(8);
  COMP_CHECK_MSG(!first.empty(), "颗粒用例应成功产出像素");
  COMP_CHECK_MSG(first == again, "同一 seed 的颗粒必须逐字节一致");
  COMP_CHECK_MSG(first != other, "不同 seed 的颗粒应不同");
}

/* ── 9. 细节 / 光学真的改像素 ───────────────────────────────────────── */
COMP_TEST(camera_raw_detail_and_optics_touch_pixels) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));

  {
    Document document(&engine, 16, 16, 72.0f, "sRGB");
    const comp_layer_id id = add_solid_layer(&document, 100, 100, 100);
    Layer* layer = document.find(id);
    COMP_CHECK(layer != nullptr);
    if (layer == nullptr) return;
    /* 降噪只对「有噪声」的画面有意义：平场前后必然相同，改用逐像素噪声。 */
    std::vector<uint8_t>& bytes = layer->pixels.writable_pixels();
    for (size_t i = 0; i < bytes.size(); i += 4) {
      const size_t pixel = i / 4;
      const uint8_t value = static_cast<uint8_t>(((pixel * 37 + (pixel / 16) * 53) % 160) + 40);
      bytes[i] = value;
      bytes[i + 1] = static_cast<uint8_t>(255 - value);
      bytes[i + 2] = value;
    }
    const std::vector<uint8_t> before = pixels_of(document, id);
    int32_t changed = 0;
    COMP_CHECK(comp_camera_raw_apply(as_document_handle(&document), id,
                                     R"({"detail":{"noiseLuminance":100,"noiseColor":60}})", 1.0,
                                     0, &changed, nullptr, 0, nullptr) == COMP_OK);
    COMP_CHECK_MSG(changed == 1, "细节非零应报告 changed");
    COMP_CHECK_MSG(pixels_of(document, id) != before, "降噪/细节应改变像素");
  }

  {
    Document document(&engine, 16, 16, 72.0f, "sRGB");
    const comp_layer_id id = add_solid_layer(&document, 120, 120, 120);
    const std::vector<uint8_t> before = pixels_of(document, id);
    int32_t changed = 0;
    COMP_CHECK(comp_camera_raw_apply(as_document_handle(&document), id,
                                     R"({"optics":{"vignetteAmount":-60}})", 1.0, 0, &changed,
                                     nullptr, 0, nullptr) == COMP_OK);
    COMP_CHECK_MSG(changed == 1, "光学非零应报告 changed");
    const std::vector<uint8_t> after = pixels_of(document, id);
    COMP_CHECK_MSG(after != before, "暗角应改变像素");
    /* 负暗角 = 压暗四角：角比中心更暗。 */
    const uint8_t corner = after[0];
    const uint8_t center = after[(8 * 16 + 8) * 4];
    COMP_CHECK_MSG(corner < center, "负暗角应把角电压到中心以下");
  }
}

/* ── 10. 16 位图层落 8 位工作副本 ───────────────────────────────────── */
COMP_TEST(camera_raw_sixteen_bit_layer_lands_on_eight_bit) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  comp_layer_desc desc{};
  desc.name = "wide";
  desc.transform = comp_rect{0, 0, 8, 8};
  comp_layer_id id = 0;
  Layer* layer = document.create_layer(desc, &id);
  COMP_CHECK(layer != nullptr);
  if (layer == nullptr) return;
  layer->pixels.resize(8, 8, COMP_PIXEL_RGBA16);
  uint16_t* wide = reinterpret_cast<uint16_t*>(layer->pixels.writable_pixels().data());
  for (size_t i = 0; i < static_cast<size_t>(8 * 8 * 4); i += 4) {
    wide[i] = 0x6000;
    wide[i + 1] = 0x6000;
    wide[i + 2] = 0x6000;
    wide[i + 3] = 0xFFFF;
  }

  int32_t changed = 0;
  COMP_CHECK(comp_camera_raw_apply(as_document_handle(&document), id, R"({"exposure":0.5})", 1.0,
                                   0, &changed, nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA8, "16 位图层处理后应变成 8 位工作副本");
}

/* ── 11. 全透明像素不被曝光改写 ─────────────────────────────────────── */
COMP_TEST(camera_raw_leaves_transparent_pixels_alone) {
  Engine engine(test_engine_desc());
  COMP_CHECK(load_tables(&engine));
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 100, 100, 100);
  Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  if (layer == nullptr) return;
  std::vector<uint8_t>& bytes = layer->pixels.writable_pixels();
  for (size_t i = 0; i < bytes.size(); i += 4) {
    if ((i / 4) % 2 == 0) {
      bytes[i] = 0;
      bytes[i + 1] = 0;
      bytes[i + 2] = 0;
      bytes[i + 3] = 0;
    }
  }

  int32_t changed = 0;
  COMP_CHECK(comp_camera_raw_apply(as_document_handle(&document), id, R"({"exposure":1})", 1.0, 0,
                                   &changed, nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> after = pixels_of(document, id);
  bool transparent_intact = true;
  for (size_t i = 0; i < after.size(); i += 4) {
    if ((i / 4) % 2 == 0) {
      if (after[i] != 0 || after[i + 1] != 0 || after[i + 2] != 0 || after[i + 3] != 0) {
        transparent_intact = false;
      }
    }
  }
  COMP_CHECK_MSG(transparent_intact, "alpha = 0 的像素应保持全零");
}

/* ── 12. 曲线求值器 ─────────────────────────────────────────────────── */
COMP_TEST(camera_raw_curve_value_is_bounded_and_monotone) {
  using compositor::CameraRawCurvePoint;
  using compositor::camera_raw_curve_value;
  const std::vector<CameraRawCurvePoint> linear = {{0.0, 0.0}, {1.0, 1.0}};
  COMP_CHECK(std::fabs(camera_raw_curve_value(linear, 0.25) - 0.25) < 1e-9);
  COMP_CHECK(std::fabs(camera_raw_curve_value(linear, 0.75) - 0.75) < 1e-9);

  /* 少于两个点直接返回 x（上游 guard）。 */
  COMP_CHECK(std::fabs(camera_raw_curve_value({}, 0.3) - 0.3) < 1e-12);
  COMP_CHECK(std::fabs(camera_raw_curve_value({{0.4, 0.9}}, 0.3) - 0.3) < 1e-12);

  /* S 曲线：单调不减，端点贴合。 */
  const std::vector<CameraRawCurvePoint> s = {{0.0, 0.0}, {0.25, 0.15}, {0.75, 0.85}, {1.0, 1.0}};
  double previous = -1.0;
  for (int i = 0; i <= 100; ++i) {
    const double x = i / 100.0;
    const double y = camera_raw_curve_value(s, x);
    COMP_CHECK(y >= previous - 1e-9);
    COMP_CHECK(y >= 0.0 && y <= 1.0);
    previous = y;
  }
  COMP_CHECK(std::fabs(camera_raw_curve_value(s, 0.0)) < 1e-9);
  COMP_CHECK(std::fabs(camera_raw_curve_value(s, 1.0) - 1.0) < 1e-9);
}
