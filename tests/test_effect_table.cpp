// AI生成
/*
 * test_effect_table.cpp — 效果参数元数据（P0-2）的 ABI 契约与默认值漂移护栏。
 *
 * 为什么需要这组用例：
 *   1. 外壳效果面板不再自持参数/量程/缺省值，全部经
 *      `comp_layer_effect_param_count/param_at` 取；一旦表被改坏（行数、键名、
 *      量程），面板会在用户面前读到错的量程，而不是构建期报错。
 *   2. 参数的 `default_value` 必须是 `effect_eval.cpp` 的缺省值——两处各写一份
 *      数值，漂移了不会崩，只会「面板显示 90°，落笔却按 0° 算」。因此这里做
 *      像素级对拍：`set_effect(kind, "{}")` 与「把参数表缺省值全部显式写进去」
 *      必须得到逐字节相同的展平结果。
 *   3. `build_settings` 只做合并：未知键必须保留（效果 settings 是前向兼容的
 *      不透明字符串，整棵重建会丢上游新增字段）。
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "test_framework.h"

namespace {

constexpr int32_t kW = 12;
constexpr int32_t kH = 12;
constexpr size_t kStride = static_cast<size_t>(kW) * 4;

struct Fixture {
  comp_engine engine = nullptr;
  comp_document doc = nullptr;

  Fixture() {
    comp_engine_desc desc{};
    desc.app_name = "compositor-effect-table-tests";
    desc.version = "1.0.0";
    desc.backend = COMP_BACKEND_CPU;
    desc.worker_threads = 1;
    comp_document_desc doc_desc{};
    doc_desc.width = kW;
    doc_desc.height = kH;
    doc_desc.dpi = 72.0f;
    COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
    COMP_CHECK(comp_document_create(engine, &doc_desc, &doc) == COMP_OK);
  }
  ~Fixture() {
    if (doc != nullptr) comp_document_destroy(doc);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
};

std::vector<uint8_t> square_pixels(int32_t lo, int32_t hi) {
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4, 0);
  for (int32_t y = lo; y < hi; ++y) {
    for (int32_t x = lo; x < hi; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4;
      px[0] = 200;
      px[1] = 100;
      px[2] = 50;
      px[3] = 255;
    }
  }
  return pixels;
}

comp_layer_id add_square(comp_document doc) {
  comp_layer_desc desc{};
  desc.name = "square";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.parent = 0;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add(doc, &desc, &id) == COMP_OK);
  std::vector<uint8_t> pixels = square_pixels(4, 8);
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(doc, id, &image) == COMP_OK);
  return id;
}

std::vector<uint8_t> flatten(comp_document doc) {
  std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4);
  COMP_CHECK(comp_render_flatten(doc, out.data(), COMP_PIXEL_RGBA8, kStride) == COMP_OK);
  return out;
}

std::string param_default_text(const comp_effect_param& param) {
  if (param.type == COMP_PARAM_BOOL) return param.default_value != 0.0 ? "1" : "0";
  char text[64];
  std::snprintf(text, sizeof(text), "%.17g", param.default_value);
  return std::string(text);
}

/* 两段式调用：先量长度，再取值。返回 settings 正文（可能为空串表示调用失败）。 */
comp_result build_settings(uint32_t effect, const std::vector<std::string>& values,
                           const std::string& base, std::string* out) {
  std::vector<const char*> raw;
  raw.reserve(values.size());
  for (const std::string& value : values) raw.push_back(value.c_str());
  const char* const* array = raw.empty() ? nullptr : raw.data();

  size_t needed = 0;
  comp_result code = comp_layer_effect_build_settings(effect, array, raw.size(), base.c_str(),
                                                      nullptr, 0, &needed);
  if (code != COMP_OK) return code;
  std::vector<char> buffer(needed + 1, '\0');
  code = comp_layer_effect_build_settings(effect, array, raw.size(), base.c_str(), buffer.data(),
                                          buffer.size(), &needed);
  if (code != COMP_OK) return code;
  *out = std::string(buffer.data(), needed);
  return COMP_OK;
}

std::vector<std::string> all_params_default_text(uint32_t effect, size_t* out_count) {
  size_t count = 0;
  COMP_CHECK(comp_layer_effect_param_count(effect, &count) == COMP_OK);
  std::vector<std::string> values;
  values.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    comp_effect_param param{};
    COMP_CHECK(comp_layer_effect_param_at(effect, i, &param) == COMP_OK);
    values.push_back(param_default_text(param));
  }
  *out_count = count;
  return values;
}

}  // namespace

COMP_TEST(effect_table_known_kinds_expose_params) {
  const size_t expected[COMP_EFFECT_KIND_COUNT] = {6, 7, 4, 7, 5, 5, 12, 8, 9};
  for (uint32_t kind = 0; kind < COMP_EFFECT_KIND_COUNT; ++kind) {
    size_t count = 0;
    COMP_CHECK(comp_layer_effect_param_count(kind, &count) == COMP_OK);
    COMP_CHECK(count == expected[kind]);
    for (size_t i = 0; i < count; ++i) {
      comp_effect_param param{};
      COMP_CHECK(comp_layer_effect_param_at(kind, i, &param) == COMP_OK);
      COMP_CHECK(param.json_path != nullptr && param.json_path[0] != '\0');
      COMP_CHECK(param.label != nullptr && param.label[0] != '\0');
      COMP_CHECK(param.flags == 0u);
      COMP_CHECK(param.type == COMP_PARAM_NUMBER || param.type == COMP_PARAM_BOOL);
      /* 缺省值必须落在自己的量程里，否则面板一打开就是越界值。 */
      COMP_CHECK(param.default_value >= param.min_value);
      COMP_CHECK(param.default_value <= param.max_value);
    }
    comp_effect_param out_of_range{};
    COMP_CHECK(comp_layer_effect_param_at(kind, count, &out_of_range) ==
               COMP_ERR_INVALID_ARGUMENT);
  }
  size_t count = 0;
  COMP_CHECK(comp_layer_effect_param_count(COMP_EFFECT_KIND_COUNT, &count) ==
             COMP_ERR_INVALID_ARGUMENT);

  /* 键名与缺省值的抽样核对：这些数就是 effect_eval.cpp 的 read_* 实参。 */
  comp_effect_param param{};
  COMP_CHECK(comp_layer_effect_param_at(COMP_EFFECT_STROKE, 0, &param) == COMP_OK);
  COMP_CHECK(std::strcmp(param.json_path, "size") == 0);
  COMP_CHECK(param.min_value == 0.0 && param.max_value == 500.0 && param.default_value == 4.0);
  COMP_CHECK(comp_layer_effect_param_at(COMP_EFFECT_DROP_SHADOW, 0, &param) == COMP_OK);
  COMP_CHECK(std::strcmp(param.json_path, "angle") == 0 && param.default_value == 90.0);
  COMP_CHECK(comp_layer_effect_param_at(COMP_EFFECT_INNER_SHADOW, 1, &param) == COMP_OK);
  COMP_CHECK(std::strcmp(param.json_path, "distance") == 0 && param.default_value == 10.0);
  COMP_CHECK(comp_layer_effect_param_at(COMP_EFFECT_OUTER_GLOW, 1, &param) == COMP_OK);
  COMP_CHECK(std::strcmp(param.json_path, "opacity") == 0 && param.default_value == 0.75);
  COMP_CHECK(comp_layer_effect_param_at(COMP_EFFECT_INNER_GLOW, 0, &param) == COMP_OK);
  COMP_CHECK(std::strcmp(param.json_path, "size") == 0 && param.default_value == 10.0);
  COMP_CHECK(comp_layer_effect_param_at(COMP_EFFECT_STROKE, 2, &param) == COMP_OK);
  COMP_CHECK(param.type == COMP_PARAM_BOOL && param.default_value == 0.0);
}

COMP_TEST(effect_table_build_settings_merges_into_base) {
  /* 只写 size（第 0 项），其余留空 = 不动；未知键 foo 必须保留。 */
  std::string json;
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {"3"}, "{\"foo\":1,\"size\":9}", &json) ==
             COMP_OK);

  double number = 0.0;
  COMP_CHECK(comp_layer_effect_read_value(json.c_str(), "foo", &number, nullptr) == COMP_OK);
  COMP_CHECK(number == 1.0);
  COMP_CHECK(comp_layer_effect_read_value(json.c_str(), "size", &number, nullptr) == COMP_OK);
  COMP_CHECK(number == 3.0);

  /* 空 values：整串一个键都不改（不是「重置为缺省」）。 */
  std::string untouched;
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {}, "{\"size\":9,\"inside\":true}",
                            &untouched) == COMP_OK);
  COMP_CHECK(comp_layer_effect_read_value(untouched.c_str(), "size", &number, nullptr) ==
             COMP_OK);
  COMP_CHECK(number == 9.0);

  /* base 为空 = 只写显式给的项。 */
  std::string fresh;
  COMP_CHECK(build_settings(COMP_EFFECT_INNER_GLOW, {"12.5"}, "", &fresh) == COMP_OK);
  COMP_CHECK(comp_layer_effect_read_value(fresh.c_str(), "size", &number, nullptr) == COMP_OK);
  COMP_CHECK(number == 12.5);
}

COMP_TEST(effect_table_build_settings_rejects_bad_values) {
  std::string json;
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {"", "1.5"}, "{}", &json) ==
             COMP_ERR_INVALID_ARGUMENT); /* opacity > 1 */
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {"-1"}, "{}", &json) ==
             COMP_ERR_INVALID_ARGUMENT); /* size < 0 */
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {"abc"}, "{}", &json) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {"", "", "maybe"}, "{}", &json) ==
             COMP_ERR_INVALID_ARGUMENT); /* inside 不是布尔文本 */
  /* 多出来的值被忽略（与 adjustment_build_settings 同口径），不是错误。 */
  std::string extra;
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {"1", "", "", "", "", "", "99"}, "{}", &extra) ==
             COMP_OK);
  double size_value = 0.0;
  COMP_CHECK(comp_layer_effect_read_value(extra.c_str(), "size", &size_value, nullptr) ==
             COMP_OK);
  COMP_CHECK(size_value == 1.0);
  COMP_CHECK(build_settings(COMP_EFFECT_KIND_COUNT, {"1"}, "{}", &json) ==
             COMP_ERR_INVALID_ARGUMENT); /* 未知 kind */
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {"1"}, "{oops", &json) ==
             COMP_ERR_INVALID_ARGUMENT); /* 坏 base JSON */
  COMP_CHECK(build_settings(COMP_EFFECT_STROKE, {"1"}, "[1,2]", &json) ==
             COMP_ERR_INVALID_ARGUMENT); /* base 不是对象 */
}

COMP_TEST(effect_table_read_value_reports_missing_paths) {
  const char* settings = "{\"size\":2.5,\"inside\":true,\"color\":{\"r\":1}}";
  double number = -1.0;
  int32_t boolean = -1;
  COMP_CHECK(comp_layer_effect_read_value(settings, "size", &number, &boolean) == COMP_OK);
  COMP_CHECK(number == 2.5);
  COMP_CHECK(comp_layer_effect_read_value(settings, "inside", &number, &boolean) == COMP_OK);
  COMP_CHECK(boolean == 1);
  COMP_CHECK(comp_layer_effect_read_value(settings, "missing", &number, &boolean) ==
             COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_effect_read_value(settings, "color", &number, &boolean) ==
             COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_effect_read_value(nullptr, "size", &number, &boolean) ==
             COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_effect_read_value(settings, nullptr, &number, &boolean) ==
             COMP_ERR_INVALID_ARGUMENT);
}

/*
 * 漂移对拍：每种 kind 都做「{}（引擎缺省）」与「参数表缺省值显式写入」两次展平，
 * 结果必须逐字节相同。这是本文件存在的核心理由——两处数值表没有编译期联动。
 */
COMP_TEST(effect_table_defaults_match_evaluator) {
  for (uint32_t kind = 0; kind < COMP_EFFECT_KIND_COUNT; ++kind) {
    Fixture fixture;
    const comp_layer_id id = add_square(fixture.doc);

    COMP_CHECK(comp_layer_set_effect(fixture.doc, id, kind, "{}") == COMP_OK);
    const std::vector<uint8_t> implicit_defaults = flatten(fixture.doc);
    COMP_CHECK(comp_layer_remove_effect(fixture.doc, id, kind) == COMP_OK);

    size_t count = 0;
    const std::vector<std::string> values = all_params_default_text(kind, &count);
    COMP_CHECK(count > 0);
    std::string explicit_defaults;
    COMP_CHECK(build_settings(kind, values, "{}", &explicit_defaults) == COMP_OK);
    COMP_CHECK(comp_layer_set_effect(fixture.doc, id, kind, explicit_defaults.c_str()) == COMP_OK);
    const std::vector<uint8_t> explicit_pixels = flatten(fixture.doc);

    COMP_CHECK(implicit_defaults.size() == explicit_pixels.size());
    COMP_CHECK(std::memcmp(implicit_defaults.data(), explicit_pixels.data(),
                           implicit_defaults.size()) == 0);
  }
}

/* 面板改一个参数后落盘，再读回：值确实变了，且未知键还在（往返不丢数据）。 */
COMP_TEST(effect_table_round_trips_through_layer_effect) {
  Fixture fixture;
  const comp_layer_id id = add_square(fixture.doc);

  COMP_CHECK(comp_layer_set_effect(fixture.doc, id, COMP_EFFECT_DROP_SHADOW,
                                   "{\"size\":77,\"angle\":90}" ) == COMP_OK);

  size_t needed = 0;
  COMP_CHECK(comp_layer_effect_at(fixture.doc, id, 0, nullptr, nullptr, 0, &needed) == COMP_OK);
  std::vector<char> current(needed + 1, '\0');
  COMP_CHECK(comp_layer_effect_at(fixture.doc, id, 0, nullptr, current.data(), current.size(),
                                  &needed) == COMP_OK);

  /* 面板把 angle 改成 45，其余留空；base 来自上面的回读。 */
  std::string updated;
  COMP_CHECK(build_settings(COMP_EFFECT_DROP_SHADOW, {"45"}, current.data(), &updated) ==
             COMP_OK);
  COMP_CHECK(comp_layer_set_effect(fixture.doc, id, COMP_EFFECT_DROP_SHADOW, updated.c_str()) ==
             COMP_OK);

  std::vector<char> readback(updated.size() + 1, '\0');
  size_t readback_size = 0;
  COMP_CHECK(comp_layer_effect_at(fixture.doc, id, 0, nullptr, readback.data(), readback.size(),
                                  &readback_size) == COMP_OK);
  COMP_CHECK(std::string(readback.data(), readback_size) == updated);

  double number = 0.0;
  COMP_CHECK(comp_layer_effect_read_value(readback.data(), "angle", &number, nullptr) == COMP_OK);
  COMP_CHECK(number == 45.0);
  COMP_CHECK(comp_layer_effect_read_value(readback.data(), "size", &number, nullptr) == COMP_OK);
  COMP_CHECK(number == 77.0);
}
