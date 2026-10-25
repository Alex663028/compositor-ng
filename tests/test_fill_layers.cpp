// AI生成
/*
 * test_fill_layers.cpp — P1-2 收尾 ⑧：新建填充层（纯色 / 渐变 / 图案）的端到端测试。
 *
 * 判据是可观测事实，不是「返回 COMP_OK」：
 *   1. 新建：kind + settings 决定像素，画面**逐字节可预测**（预乘 RGBA8）；
 *      一步撤销，撤销后图层连同参数一起消失；
 *   2. 参数闸先于副作用：非法 settings / 越界值 / 非法 kind 一律 INVALID_ARGUMENT，
 *      不建层、不落撤销步；
 *   3. 参数表：kind_name / param_count / param_at / read_value / build_settings
 *      与渲染共用同一份量程（含布尔参数的 "0"/"1" 文本）；
 *   4. 几何：渐变的 angle 从 +x 轴逆时针量（0° 左→右、90° 下→上），reverse 交换两端；
 *      图案按 size 平铺且**确定性**（同一参数两次渲染逐字节相同）；
 *   5. set_fill：改参数 = 整层重渲，一步撤销；参数一字未变 -> out_changed = 0 不落步；
 *   6. 像素写入被闸住：comp_layer_set_pixels / comp_filter_apply / comp_tool_apply
 *      一律 COMP_ERR_STATE；智能对象转换也拒绝（同一层只能有一个真源）；
 *   7. 栅格化：丢掉参数、保留当前画面，幂等（第二次不落撤销步），之后可以手绘；
 *   8. 锁：锁定全部拦 set_fill 与 rasterize_fill，且不落撤销步；
 *   9. `.comp` 往返：engineFill 保真，普通图层不写该键。
 *
 * 只用 comp_*.h 里的 C 符号，与三端外壳的集成路径一致。
 */
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_filter.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

constexpr int32_t kW = 32;
constexpr int32_t kH = 32;

comp_engine_desc engine_desc_make() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-fill-layer-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

comp_document make_document(comp_engine engine, int32_t width = kW, int32_t height = kH) {
  comp_document_desc desc{};
  desc.width = width;
  desc.height = height;
  comp_document document = nullptr;
  if (comp_document_create(engine, &desc, &document) != COMP_OK) return nullptr;
  return document;
}

comp_layer_id add_layer(comp_document document, const char* name, bool group = false) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  desc.is_group = group ? 1 : 0;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  return id;
}

comp_layer_id bottom_layer(comp_document document) {
  comp_layer_id id = 0;
  size_t count = 0;
  if (comp_layer_list(document, &id, 1u, &count) != COMP_OK || count < 1u) return 0;
  return id;
}

size_t layer_count(comp_document document) {
  size_t count = 0;
  if (comp_layer_list(document, nullptr, 0u, &count) != COMP_OK) return 0xFFFFFFFFu;
  return count;
}

size_t history_steps(comp_document document) {
  size_t steps = 0;
  if (comp_document_history_step_count(document, &steps) != COMP_OK) return 0xFFFFFFFFu;
  return steps;
}

struct Pixels {
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  std::vector<uint8_t> bytes;

  bool empty() const { return bytes.empty(); }
  const uint8_t* at(int32_t x, int32_t y) const {
    return bytes.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) +
                           static_cast<size_t>(x)) * 4u;
  }
};

Pixels read_pixels(comp_document document, comp_layer_id id) {
  Pixels out;
  if (comp_layer_get_pixels(document, id, nullptr, 0, &out.width, &out.height, &out.format) !=
      COMP_OK) {
    return out;
  }
  if (out.width <= 0 || out.height <= 0) return out;
  const size_t stride = static_cast<size_t>(out.width) * 4u;
  out.bytes.resize(stride * static_cast<size_t>(out.height));
  if (comp_layer_get_pixels(document, id, out.bytes.data(), stride, &out.width, &out.height,
                            &out.format) != COMP_OK) {
    out.bytes.clear();
  }
  return out;
}

bool is_fill(comp_document document, comp_layer_id id) {
  int32_t value = -1;
  if (comp_layer_is_fill(document, id, &value) != COMP_OK) return false;
  return value == 1;
}

/* 堆叠里第一个填充图层（读盘后不知道 id，按谓词找最稳）。 */
comp_layer_id find_fill_layer(comp_document document) {
  size_t count = 0;
  if (comp_layer_list(document, nullptr, 0u, &count) != COMP_OK || count == 0u) return 0;
  std::vector<comp_layer_id> ids(count);
  if (comp_layer_list(document, ids.data(), ids.size(), &count) != COMP_OK) return 0;
  for (comp_layer_id id : ids) {
    if (is_fill(document, id)) return id;
  }
  return 0;
}

/* 两段式读 settings：返回 (kind, settings 文本)。失败时 kind = 0xFFFFFFFF。 */
uint32_t fill_settings_of(comp_document document, comp_layer_id id, std::string* out_settings) {
  uint32_t kind = 0xFFFFFFFFu;
  size_t size = 0;
  if (comp_layer_get_fill(document, id, &kind, nullptr, 0, &size) != COMP_OK) return 0xFFFFFFFFu;
  std::string text(size, '\0');
  if (size > 0) {
    if (comp_layer_get_fill(document, id, &kind, &text[0], size + 1, &size) != COMP_OK) {
      return 0xFFFFFFFFu;
    }
    text.resize(size);
  }
  if (out_settings != nullptr) *out_settings = text;
  return kind;
}

std::string default_settings(uint32_t kind) {
  size_t size = 0;
  if (comp_fill_default_settings(kind, nullptr, 0, &size) != COMP_OK) return std::string();
  std::string text(size, '\0');
  if (size > 0 && comp_fill_default_settings(kind, &text[0], size + 1, &size) != COMP_OK) {
    return std::string();
  }
  text.resize(size);
  return text;
}

std::string settings_from_values(uint32_t kind, const std::vector<const char*>& values) {
  size_t count = 0;
  if (comp_fill_kind_param_count(kind, &count) != COMP_OK) return std::string();
  std::vector<const char*> flat(count, "");
  for (size_t i = 0; i < count && i < values.size(); ++i) flat[i] = values[i];
  size_t size = 0;
  if (comp_fill_build_settings(kind, flat.data(), flat.size(), nullptr, 0, &size) != COMP_OK) {
    return std::string();
  }
  std::string text(size, '\0');
  if (size > 0 && comp_fill_build_settings(kind, flat.data(), flat.size(), &text[0], size + 1,
                                           &size) != COMP_OK) {
    return std::string();
  }
  text.resize(size);
  return text;
}

std::string work_path(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/fill_layers", ec);
  return std::string("build/fill_layers/") + name;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

/* 建一个已存在的填充图层（纯色 #3366CC，不透明）。 */
struct FillFixture {
  comp_engine engine = nullptr;
  comp_document document = nullptr;
  comp_layer_id layer = 0;
  std::string settings;

  bool build(uint32_t kind = COMP_FILL_SOLID, const char* custom = nullptr) {
    comp_engine_desc desc = engine_desc_make();
    if (comp_engine_create(&desc, &engine) != COMP_OK) return false;
    document = make_document(engine);
    if (document == nullptr) return false;
    settings = custom != nullptr ? std::string(custom) : settings_from_values(kind, {"51", "102", "204", "255"});
    if (settings.empty()) return false;
    if (comp_layer_add_fill(document, kind, settings.c_str(), &layer) != COMP_OK) return false;
    return layer != 0;
  }

  ~FillFixture() {
    if (document != nullptr) comp_document_destroy(document);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
};

}  // namespace

/* -------------------------------------------------- 1. 新建 / 幂等 / 撤销 */

COMP_TEST(fill_layer_add_renders_settings_and_undoes) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  const size_t layers_before = layer_count(document);
  const size_t steps_before = history_steps(document);
  const std::string settings = settings_from_values(COMP_FILL_SOLID, {"51", "102", "204", "255"});
  COMP_CHECK(!settings.empty());

  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_SOLID, settings.c_str(), &id) == COMP_OK);
  COMP_CHECK(id != 0);
  COMP_CHECK_MSG(layer_count(document) == layers_before + 1u, "add_fill creates exactly one layer");
  COMP_CHECK_MSG(history_steps(document) == steps_before + 1u, "add_fill is one undo step");
  COMP_CHECK(is_fill(document, id));

  /* 纯色：整幅同一像素，预乘口径 = color * alpha / 255（alpha 255 时不缩放）。 */
  const Pixels picture = read_pixels(document, id);
  COMP_CHECK(picture.width == kW && picture.height == kH);
  COMP_CHECK(!picture.empty());
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      const uint8_t* px = picture.at(x, y);
      if (px[0] != 51u || px[1] != 102u || px[2] != 204u || px[3] != 255u) {
        COMP_CHECK_MSG(false, "every pixel of a solid fill must carry the same premultiplied color");
        y = kH;
        break;
      }
    }
  }

  uint32_t kind = 0;
  size_t size = 0;
  COMP_CHECK(comp_layer_get_fill(document, id, &kind, nullptr, 0, &size) == COMP_OK);
  COMP_CHECK(kind == COMP_FILL_SOLID);
  COMP_CHECK_MSG(size == settings.size(), "get_fill reports the settings length without the NUL");

  /* 缺省名由 kind 决定。 */
  char name[64] = {0};
  size_t required = 0;
  COMP_CHECK(comp_layer_get_name(document, id, name, sizeof(name), &required) == COMP_OK);
  COMP_CHECK_MSG(std::string(name) == "Color Fill", "a solid fill layer gets the Photoshop name");

  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK_MSG(layer_count(document) == layers_before, "undo removes the fill layer entirely");
  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK_MSG(layer_count(document) == layers_before + 1u, "redo brings it back");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(fill_layer_add_rejects_bad_input_before_any_side_effect) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  const size_t layers_before = layer_count(document);
  const size_t steps_before = history_steps(document);

  /* kind 0 不是填充层，越界 kind 也不是。 */
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_NONE, "{}", &id) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_add_fill(document, 99u, "{}", &id) == COMP_ERR_INVALID_ARGUMENT);

  /* 未知键：参数表封闭，拼错的键不会静默变成缺省值。 */
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_SOLID, "{\"colour\":{\"red\":1}}", &id) ==
             COMP_ERR_INVALID_ARGUMENT);
  /* 类型不符与越界。 */
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_SOLID, "{\"color\":{\"red\":\"x\"}}", &id) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_SOLID, "{\"color\":{\"red\":300}}", &id) ==
             COMP_ERR_INVALID_ARGUMENT);
  /* 非对象根。 */
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_GRADIENT, "[1,2]", &id) ==
             COMP_ERR_INVALID_ARGUMENT);
  /* 渐变的布尔参数必须是布尔，不接受数字。 */
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_GRADIENT, "{\"reverse\":1}", &id) ==
             COMP_ERR_INVALID_ARGUMENT);

  COMP_CHECK_MSG(layer_count(document) == layers_before, "no rejected call creates a layer");
  COMP_CHECK_MSG(history_steps(document) == steps_before, "no rejected call leaves an undo step");

  /* 缺键合法：整份 {} 就是缺省参数。 */
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_SOLID, "{}", &id) == COMP_OK);
  COMP_CHECK(is_fill(document, id));
  COMP_CHECK_MSG(read_pixels(document, id).at(0, 0)[3] == 255u,
                 "the solid default alpha is opaque");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ---------------------------------------------------------- 2. 参数表 */

COMP_TEST(fill_layer_param_table_matches_the_renderer) {
  const char* none_name = nullptr;
  COMP_CHECK(comp_fill_kind_name(COMP_FILL_NONE, &none_name) == COMP_OK);
  COMP_CHECK_MSG(std::string(none_name) == "None", "kind 0 is named None");
  const char* gradient_name = nullptr;
  COMP_CHECK(comp_fill_kind_name(COMP_FILL_GRADIENT, &gradient_name) == COMP_OK);
  COMP_CHECK(std::string(gradient_name) == "Gradient");
  const char* bad_name = nullptr;
  COMP_CHECK(comp_fill_kind_name(4u, &bad_name) == COMP_ERR_INVALID_ARGUMENT);

  /* 0 是「不是填充层」：参数数量 0，但没有参数表可列。 */
  size_t count = 0xFFFFFFFFu;
  COMP_CHECK(comp_fill_kind_param_count(COMP_FILL_NONE, &count) == COMP_OK);
  COMP_CHECK(count == 0u);
  comp_fill_param param{};
  COMP_CHECK(comp_fill_kind_param_at(COMP_FILL_NONE, 0u, &param) == COMP_ERR_INVALID_ARGUMENT);

  size_t solid_count = 0;
  COMP_CHECK(comp_fill_kind_param_count(COMP_FILL_SOLID, &solid_count) == COMP_OK);
  COMP_CHECK(solid_count == 4u);
  COMP_CHECK(comp_fill_kind_param_at(COMP_FILL_SOLID, 0u, &param) == COMP_OK);
  COMP_CHECK(std::string(param.json_path) == "color.red");
  COMP_CHECK(param.type == COMP_PARAM_NUMBER);
  COMP_CHECK(param.min_value == 0.0 && param.max_value == 255.0);
  COMP_CHECK(param.flags == 0u);
  COMP_CHECK(comp_fill_kind_param_at(COMP_FILL_SOLID, solid_count, &param) ==
             COMP_ERR_INVALID_ARGUMENT);

  /* 渐变的第 10 个参数是布尔 reverse。 */
  size_t gradient_count = 0;
  COMP_CHECK(comp_fill_kind_param_count(COMP_FILL_GRADIENT, &gradient_count) == COMP_OK);
  COMP_CHECK(gradient_count == 10u);
  COMP_CHECK(comp_fill_kind_param_at(COMP_FILL_GRADIENT, 9u, &param) == COMP_OK);
  COMP_CHECK(std::string(param.json_path) == "reverse");
  COMP_CHECK(param.type == COMP_PARAM_BOOL);

  /* 缺省 settings 与参数表一致：每个叶子都能按路径读回缺省值。 */
  const std::string defaults = default_settings(COMP_FILL_PATTERN);
  COMP_CHECK(!defaults.empty());
  size_t pattern_count = 0;
  COMP_CHECK(comp_fill_kind_param_count(COMP_FILL_PATTERN, &pattern_count) == COMP_OK);
  for (size_t i = 0; i < pattern_count; ++i) {
    COMP_CHECK(comp_fill_kind_param_at(COMP_FILL_PATTERN, i, &param) == COMP_OK);
    if (param.type == COMP_PARAM_BOOL) {
      int32_t value = -1;
      COMP_CHECK(comp_fill_read_value(defaults.c_str(), param.json_path, nullptr, &value) ==
                 COMP_OK);
      COMP_CHECK(value == (param.default_value != 0.0 ? 1 : 0));
      continue;
    }
    double value = -1.0;
    COMP_CHECK(comp_fill_read_value(defaults.c_str(), param.json_path, &value, nullptr) ==
               COMP_OK);
    COMP_CHECK(value == param.default_value);
  }

  /* build_settings：ASCII 文本进、settings 出；空串 = 沿用缺省；布尔接受 "0"/"1"。 */
  const std::string built = settings_from_values(
      COMP_FILL_GRADIENT, {"10", "20", "30", "40", "50", "60", "70", "80", "45", "1"});
  COMP_CHECK(!built.empty());
  double number = -1.0;
  int32_t boolean = -1;
  COMP_CHECK(comp_fill_read_value(built.c_str(), "start.red", &number, nullptr) == COMP_OK);
  COMP_CHECK(number == 10.0);
  COMP_CHECK(comp_fill_read_value(built.c_str(), "angle", &number, nullptr) == COMP_OK);
  COMP_CHECK(number == 45.0);
  COMP_CHECK(comp_fill_read_value(built.c_str(), "reverse", nullptr, &boolean) == COMP_OK);
  COMP_CHECK(boolean == 1);
  COMP_CHECK(comp_fill_validate(COMP_FILL_GRADIENT, built.c_str()) == COMP_OK);

  /* 非数字文本 / 越界值 / 非布尔文本都拒绝。 */
  size_t out_size = 0;
  const char* bad_values[10] = {"abc", "0", "0", "255", "0", "0", "255", "255", "0", "0"};
  COMP_CHECK(comp_fill_build_settings(COMP_FILL_GRADIENT, bad_values, 10u, nullptr, 0, &out_size) ==
             COMP_ERR_INVALID_ARGUMENT);
  const char* out_of_range[10] = {"256", "0", "0", "255", "0", "0", "255", "255", "0", "0"};
  COMP_CHECK(comp_fill_build_settings(COMP_FILL_GRADIENT, out_of_range, 10u, nullptr, 0, &out_size) ==
             COMP_ERR_INVALID_ARGUMENT);
  const char* bad_bool[10] = {"255", "0", "0", "255", "0", "0", "255", "255", "0", "yes"};
  COMP_CHECK(comp_fill_build_settings(COMP_FILL_GRADIENT, bad_bool, 10u, nullptr, 0, &out_size) ==
             COMP_ERR_INVALID_ARGUMENT);

  /* read_value：缺失路径是 NOT_FOUND，不是「返回 0」。 */
  COMP_CHECK(comp_fill_read_value(built.c_str(), "start.alpha.extra", &number, nullptr) ==
             COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_fill_validate(COMP_FILL_SOLID, "{\"nope\":1}") == COMP_ERR_INVALID_ARGUMENT);
}

/* ------------------------------------------------------- 3. 渐变几何 */

COMP_TEST(fill_layer_gradient_follows_the_angle) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  /* angle 0 = 左→右：start 红、end 蓝。 */
  const std::string horizontal = settings_from_values(
      COMP_FILL_GRADIENT, {"255", "0", "0", "255", "0", "0", "255", "255", "0", "0"});
  COMP_CHECK(!horizontal.empty());
  comp_layer_id left_to_right = 0;
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_GRADIENT, horizontal.c_str(),
                                 &left_to_right) == COMP_OK);
  const Pixels h = read_pixels(document, left_to_right);
  COMP_CHECK(!h.empty());
  COMP_CHECK_MSG(h.at(0, kH / 2)[0] > h.at(0, kH / 2)[2], "the left edge starts at the start color");
  COMP_CHECK_MSG(h.at(kW - 1, kH / 2)[2] > h.at(kW - 1, kH / 2)[0],
                 "the right edge ends at the end color");
  /* 同一列必须同色（渐变轴水平 -> 与 y 无关）。 */
  for (int32_t x = 0; x < kW; ++x) {
    if (std::memcmp(h.at(x, 0), h.at(x, kH - 1), 4u) != 0) {
      COMP_CHECK_MSG(false, "a 0-degree gradient must not vary along y");
      break;
    }
  }

  /* angle 90 = 下→上（y 向下，角度逆时针为正）：start 在底部。 */
  const std::string vertical = settings_from_values(
      COMP_FILL_GRADIENT, {"255", "0", "0", "255", "0", "0", "255", "255", "90", "0"});
  comp_layer_id bottom_to_top = 0;
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_GRADIENT, vertical.c_str(),
                                 &bottom_to_top) == COMP_OK);
  const Pixels v = read_pixels(document, bottom_to_top);
  COMP_CHECK(!v.empty());
  COMP_CHECK_MSG(v.at(kW / 2, kH - 1)[0] > v.at(kW / 2, kH - 1)[2],
                 "90 degrees starts at the bottom edge");
  COMP_CHECK_MSG(v.at(kW / 2, 0)[2] > v.at(kW / 2, 0)[0], "90 degrees ends at the top edge");

  /* reverse 交换两端。 */
  const std::string reversed = settings_from_values(
      COMP_FILL_GRADIENT, {"255", "0", "0", "255", "0", "0", "255", "255", "0", "1"});
  comp_layer_id flipped = 0;
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_GRADIENT, reversed.c_str(), &flipped) ==
             COMP_OK);
  const Pixels r = read_pixels(document, flipped);
  COMP_CHECK(!r.empty());
  COMP_CHECK_MSG(r.at(0, kH / 2)[2] > r.at(0, kH / 2)[0], "reverse puts the end color on the left");
  COMP_CHECK_MSG(r.at(kW - 1, kH / 2)[0] > r.at(kW - 1, kH / 2)[2],
                 "reverse puts the start color on the right");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------------- 4. 图案确定性 */

COMP_TEST(fill_layer_pattern_tiles_deterministically) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  /* 棋盘 8px：格 (0,0) 是背景（白），格 (1,0) 是前景（黑），周期 2 * size = 16。 */
  const std::string checker = settings_from_values(
      COMP_FILL_PATTERN, {"0", "8", "0", "0", "0", "255", "255", "255", "255", "255"});
  COMP_CHECK(!checker.empty());
  comp_layer_id first = 0;
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_PATTERN, checker.c_str(), &first) == COMP_OK);
  const Pixels a = read_pixels(document, first);
  COMP_CHECK(!a.empty());
  COMP_CHECK_MSG(a.at(0, 0)[0] == 255u && a.at(0, 0)[3] == 255u, "the first checker cell is the background");
  COMP_CHECK_MSG(a.at(8, 0)[0] == 0u && a.at(8, 0)[3] == 255u,
                 "the neighbouring checker cell is the foreground");
  COMP_CHECK_MSG(std::memcmp(a.at(0, 0), a.at(16, 0), 4u) == 0,
                 "the checker pattern tiles with period 2 * size");
  COMP_CHECK_MSG(std::memcmp(a.at(0, 0), a.at(0, 16), 4u) == 0, "tiling is square, not just horizontal");

  /* 同一参数再建一层：逐字节相同（无随机源、无时钟）。 */
  comp_layer_id second = 0;
  COMP_CHECK(comp_layer_add_fill(document, COMP_FILL_PATTERN, checker.c_str(), &second) == COMP_OK);
  COMP_CHECK_MSG(read_pixels(document, second).bytes == a.bytes, "pattern rendering is deterministic");

  /* 竖条 / 横条 / 圆点都铺满且非空。 */
  const uint32_t kinds[3] = {1u, 2u, 3u};
  for (uint32_t pattern : kinds) {
    const std::string pattern_text = std::to_string(pattern);
    const std::string settings =
        settings_from_values(COMP_FILL_PATTERN,
                             {pattern_text.c_str(), "8", "0", "0", "0", "255", "255", "255",
                              "255", "255"});
    comp_layer_id id = 0;
    if (comp_layer_add_fill(document, COMP_FILL_PATTERN, settings.c_str(), &id) != COMP_OK) {
      COMP_CHECK_MSG(false, "every built-in pattern must render");
      continue;
    }
    const Pixels px = read_pixels(document, id);
    COMP_CHECK(!px.empty());
    COMP_CHECK_MSG(px.at(0, 0)[3] == 255u, "built-in patterns are fully opaque");
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* --------------------------------------------------- 5. set_fill 重渲与闸 */

COMP_TEST(fill_layer_set_fill_rerenders_and_gates_pixel_writes) {
  FillFixture fx;
  COMP_CHECK(fx.build());
  const Pixels before = read_pixels(fx.document, fx.layer);
  COMP_CHECK(!before.empty());

  /* 同参数：不重渲、不落步。 */
  size_t steps = history_steps(fx.document);
  int32_t changed = -1;
  COMP_CHECK(comp_layer_set_fill(fx.document, fx.layer, COMP_FILL_SOLID, fx.settings.c_str(),
                                 &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "an identical settings string is a no-op");
  COMP_CHECK_MSG(history_steps(fx.document) == steps, "the no-op leaves no undo step");

  /* 换颜色：整层重渲，一步撤销。 */
  const std::string other = settings_from_values(COMP_FILL_SOLID, {"255", "0", "0", "128"});
  COMP_CHECK(!other.empty());
  COMP_CHECK(comp_layer_set_fill(fx.document, fx.layer, COMP_FILL_SOLID, other.c_str(), &changed) ==
             COMP_OK);
  COMP_CHECK_MSG(changed == 1, "a different setting re-renders the layer");
  COMP_CHECK_MSG(history_steps(fx.document) == steps + 1u, "set_fill is one undo step");
  const Pixels after = read_pixels(fx.document, fx.layer);
  /* 预乘：255 * 128 / 255 = 128。 */
  COMP_CHECK(after.at(0, 0)[0] == 128u && after.at(0, 0)[3] == 128u);

  /* 非法参数：不重渲、不落步、画面不变。 */
  steps = history_steps(fx.document);
  COMP_CHECK(comp_layer_set_fill(fx.document, fx.layer, COMP_FILL_SOLID, "{\"color\":{\"red\":-1}}",
                                 &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK_MSG(history_steps(fx.document) == steps, "a rejected set_fill leaves no undo step");
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == after.bytes);

  /* kind 与 settings 一起换：同一条入口。 */
  const std::string gradient = settings_from_values(
      COMP_FILL_GRADIENT, {"255", "0", "0", "255", "0", "0", "255", "255", "0", "0"});
  COMP_CHECK(comp_layer_set_fill(fx.document, fx.layer, COMP_FILL_GRADIENT, gradient.c_str(),
                                 &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  std::string settings_text;
  COMP_CHECK(fill_settings_of(fx.document, fx.layer, &settings_text) == COMP_FILL_GRADIENT);
  COMP_CHECK(settings_text == gradient);

  /* 像素写入一律被闸住（参数是真源）。 */
  const Pixels picture = read_pixels(fx.document, fx.layer);
  steps = history_steps(fx.document);
  std::vector<uint8_t> replacement(picture.bytes.size(), 0u);
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(kW) * 4u;
  image.pixels = replacement.data();
  COMP_CHECK_MSG(comp_layer_set_pixels(fx.document, fx.layer, &image) == COMP_ERR_STATE,
                 "set_pixels on a fill layer asks the host to rasterize first");

  comp_filter_desc blur{};
  blur.kind = COMP_FILTER_GAUSSIAN_BLUR;
  blur.gaussian_blur.radius = 2.0;
  COMP_CHECK_MSG(comp_filter_apply(fx.document, fx.layer, &blur, 1.0, &changed, nullptr, 0,
                                   nullptr) == COMP_ERR_STATE,
                 "the whole-layer filter path refuses fill layers");

  comp_tool_op_desc tool{};
  tool.kind = COMP_TOOL_DITHER;
  COMP_CHECK_MSG(comp_tool_apply(fx.document, fx.layer, &tool, nullptr, 0, nullptr) ==
                     COMP_ERR_STATE,
                 "pixel tools refuse fill layers");

  /* 智能对象转换同样拒绝：同一层只能有一个真源。 */
  COMP_CHECK_MSG(comp_layer_smart_object_create(fx.document, fx.layer, &changed) == COMP_ERR_STATE,
                 "a fill layer cannot also become a smart object");

  COMP_CHECK_MSG(history_steps(fx.document) == steps, "all rejections leave no undo step");
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == picture.bytes);

  comp_document_destroy(fx.document);
  comp_engine_destroy(fx.engine);
  fx.document = nullptr;
  fx.engine = nullptr;
}

/* ------------------------------------------------------------ 6. 类型闸 */

COMP_TEST(fill_layer_rejects_type_confusion) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  const comp_layer_id plain = bottom_layer(document);
  COMP_CHECK(plain != 0);
  int32_t changed = -1;
  COMP_CHECK_MSG(comp_layer_set_fill(document, plain, COMP_FILL_SOLID, "{}", &changed) ==
                     COMP_ERR_STATE,
                 "set_fill refuses a plain pixel layer");

  uint32_t kind = 0;
  size_t size = 0;
  COMP_CHECK_MSG(comp_layer_get_fill(document, plain, &kind, nullptr, 0, &size) == COMP_ERR_STATE,
                 "get_fill reports STATE for a plain layer (not an empty fill)");

  int32_t is = -1;
  COMP_CHECK(comp_layer_is_fill(document, plain, &is) == COMP_OK);
  COMP_CHECK_MSG(is == 0, "is_fill is a safe predicate for any layer");

  /* 组层不能变成填充层（也没有像素可换）。 */
  const comp_layer_id group = add_layer(document, "group", true);
  COMP_CHECK(comp_layer_set_fill(document, group, COMP_FILL_SOLID, "{}", &changed) ==
             COMP_ERR_STATE);

  /* 不存在的图层：NOT_FOUND，而不是把它当成普通图层。 */
  COMP_CHECK(comp_layer_is_fill(document, 99999u, &is) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------------- 7. 栅格化（幂等） */

COMP_TEST(fill_layer_rasterize_keeps_the_render) {
  FillFixture fx;
  COMP_CHECK(fx.build());
  COMP_CHECK(fx.layer != 0);
  const Pixels rendered = read_pixels(fx.document, fx.layer);
  COMP_CHECK(!rendered.empty());

  size_t steps = history_steps(fx.document);
  COMP_CHECK(comp_layer_rasterize_fill(fx.document, fx.layer) == COMP_OK);
  COMP_CHECK_MSG(history_steps(fx.document) == steps + 1u, "rasterize is one undo step");
  COMP_CHECK_MSG(!is_fill(fx.document, fx.layer), "rasterize drops the fill parameters");
  COMP_CHECK_MSG(read_pixels(fx.document, fx.layer).bytes == rendered.bytes,
                 "rasterize keeps the current render");

  /* 幂等：普通图层再栅格化不落撤销步。 */
  steps = history_steps(fx.document);
  COMP_CHECK(comp_layer_rasterize_fill(fx.document, fx.layer) == COMP_OK);
  COMP_CHECK_MSG(history_steps(fx.document) == steps, "re-rasterizing a plain layer is a no-op");

  /* 栅格化之后它就是普通像素层：整份像素写入重新放行。 */
  std::vector<uint8_t> replacement(rendered.bytes.size(), 0u);
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(kW) * 4u;
  image.pixels = replacement.data();
  COMP_CHECK(comp_layer_set_pixels(fx.document, fx.layer, &image) == COMP_OK);

  /* 第一步撤销的是整份像素写入，第二步才轮到栅格化本身。 */
  COMP_CHECK(comp_document_undo(fx.document) == COMP_OK);
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == rendered.bytes);
  COMP_CHECK(comp_document_undo(fx.document) == COMP_OK);
  COMP_CHECK_MSG(is_fill(fx.document, fx.layer), "undo brings the fill parameters back");
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == rendered.bytes);
}

/* ---------------------------------------------------------------- 8. 锁 */

COMP_TEST(fill_layer_locks_gate_parameter_writes) {
  FillFixture fx;
  COMP_CHECK(fx.build());
  const Pixels picture = read_pixels(fx.document, fx.layer);
  COMP_CHECK(comp_layer_set_lock(fx.document, fx.layer, COMP_LAYER_LOCK_ALL) == COMP_OK);

  const size_t steps = history_steps(fx.document);
  int32_t changed = -1;
  const std::string other = settings_from_values(COMP_FILL_SOLID, {"1", "2", "3", "255"});
  COMP_CHECK(comp_layer_set_fill(fx.document, fx.layer, COMP_FILL_SOLID, other.c_str(), &changed) ==
             COMP_ERR_STATE);
  COMP_CHECK_MSG(comp_layer_rasterize_fill(fx.document, fx.layer) == COMP_ERR_STATE,
                 "rasterize is a one-way edit and is gated by LOCK_ALL");
  COMP_CHECK_MSG(history_steps(fx.document) == steps, "locked rejections leave no undo step");
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == picture.bytes);
  COMP_CHECK(is_fill(fx.document, fx.layer));

  comp_document_destroy(fx.document);
  comp_engine_destroy(fx.engine);
  fx.document = nullptr;
  fx.engine = nullptr;
}

/* --------------------------------------------------- 9. `.comp` 往返保真 */

COMP_TEST(fill_layer_comp_round_trip) {
  FillFixture fx;
  const std::string gradient = settings_from_values(
      COMP_FILL_GRADIENT, {"12", "34", "56", "255", "78", "90", "123", "200", "135", "1"});
  COMP_CHECK(!gradient.empty());
  COMP_CHECK(fx.build(COMP_FILL_GRADIENT, gradient.c_str()));
  const Pixels expected = read_pixels(fx.document, fx.layer);

  const std::string path = work_path("round_trip.comp");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  COMP_CHECK(comp_io_save_project(fx.document, path.c_str()) == COMP_OK);

  const std::string manifest = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("engineFill") != std::string::npos,
                 "the manifest records fill layers");
  COMP_CHECK_MSG(manifest.find("imageFile") != std::string::npos,
                 "the rendered cache is still written as a normal image");

  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(fx.engine, path.c_str(), &loaded) == COMP_OK);
  /* 工程里还有新建文档自带的那层空图层：按谓词找填充层，不假定它是最底层。 */
  const comp_layer_id loaded_layer = find_fill_layer(loaded);
  COMP_CHECK_MSG(loaded_layer != 0, "the loaded document still contains a fill layer");
  COMP_CHECK_MSG(is_fill(loaded, loaded_layer), "the fill parameters survive a save/load");
  std::string settings_text;
  COMP_CHECK_MSG(fill_settings_of(loaded, loaded_layer, &settings_text) == COMP_FILL_GRADIENT,
                 "the fill kind survives a save/load");
  COMP_CHECK_MSG(settings_text == gradient, "the settings JSON round-trips byte for byte");
  COMP_CHECK_MSG(read_pixels(loaded, loaded_layer).bytes == expected.bytes,
                 "the rendered cache is the same picture");

  /* 读回来的仍然可编辑：改参数就重渲（不是一具死像素）。 */
  const std::string solid = settings_from_values(COMP_FILL_SOLID, {"5", "6", "7", "255"});
  int32_t changed = -1;
  COMP_CHECK(comp_layer_set_fill(loaded, loaded_layer, COMP_FILL_SOLID, solid.c_str(), &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(read_pixels(loaded, loaded_layer).at(0, 0)[0] == 5u);

  comp_document_destroy(loaded);
}

/* 没有填充层的工程不写 engineFill（缺键 = 普通图层），落盘结果与旧版一致。 */
COMP_TEST(plain_project_has_no_fill_keys) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  const std::string path = work_path("plain.comp");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  COMP_CHECK(comp_io_save_project(document, path.c_str()) == COMP_OK);
  const std::string manifest = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("engineFill") == std::string::npos,
                 "a plain project must not grow an engineFill key");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
