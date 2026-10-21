// AI生成
/*
 * test_adjustment.cpp — 调整图层参数键名表与校验的冻结护栏。
 *
 * 冻结依据：robbietilton/Compositor 的 `AdjustmentKind`（Codable enum，rawValue 即名称）
 * 与 `LayerAdjustment` 及其嵌套 Settings 结构。本仓 `.comp` 的 engineAdjustments[].kind
 * 取值 = 该枚举 CaseIterable 的序号；settings 内的键名逐字取自该结构的字段名。
 * 两者任一漂移，既有 .comp 工程就会在两端被解读成不同参数。
 */
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "domain/adjustment_table.h"
#include "domain/curves_settings.h"
#include "format/comp_json.h"
#include "test_framework.h"

using namespace compositor;

namespace {

/* 上游 `AdjustmentKind` 的 rawValue，按声明顺序逐字抄录。 */
const char* const kUpstreamKindNames[] = {
    "Hue/Saturation", "Levels", "Curves", "Exposure", "Gradient Map", "Grain",
    "Add Noise",      "Gaussian Blur", "Motion Blur", "Invert", "Black & White", "Color Balance",
};

comp_engine_desc make_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-adjustment-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

std::filesystem::path temp_comp_path(const char* name) {
  const std::filesystem::path base = std::filesystem::temp_directory_path() / "compositor_ng_tests";
  std::error_code error;
  std::filesystem::create_directories(base, error);
  std::filesystem::remove_all(base / name, error);
  return base / name;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::string();
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

}  // namespace

COMP_TEST(adjustment_kind_names_match_upstream) {
  const size_t upstream_count = sizeof(kUpstreamKindNames) / sizeof(kUpstreamKindNames[0]);
  COMP_CHECK(upstream_count == kAdjustmentKindCount);
  for (uint32_t kind = 0; kind < kAdjustmentKindCount; ++kind) {
    const AdjustmentKindInfo* info = adjustment_kind_info(kind);
    COMP_CHECK_MSG(info != nullptr, "kind info");
    if (info == nullptr) continue;
    COMP_CHECK_MSG(info->kind == kind, "kind index");
    COMP_CHECK_MSG(std::string(info->name) == kUpstreamKindNames[kind], info->name);
  }
  /* 越界 kind 必须显式失败，不得回落到某个默认类型。 */
  COMP_CHECK(adjustment_kind_info(kAdjustmentKindCount) == nullptr);
  COMP_CHECK(adjustment_kind_info(999u) == nullptr);
}

COMP_TEST(adjustment_defaults_pass_validation) {
  for (uint32_t kind = 0; kind < kAdjustmentKindCount; ++kind) {
    const std::string defaults = adjustment_default_settings_json(kind);
    std::string error;
    COMP_CHECK_MSG(adjustment_validate_settings(kind, defaults, &error), error.c_str());
    /* 空的 settings 合法但无参数的类型必须显式返回 "{}"，不得写进半截默认值。 */
    const AdjustmentKindInfo* info = adjustment_kind_info(kind);
    if (info != nullptr && info->param_count == 0) {
      COMP_CHECK_MSG(defaults == "{}", info->name);
    }
    COMP_CHECK(adjustment_validate_settings(kind, "", &error));
  }
}

/*
 * 缺省 JSON 必须**逐参数完整**：参数表里声明的每个 json_path 都要落到默认值上。
 *
 * 这条断言针对 build_defaults 的真实合并缺陷：参数表中多个参数共享同一顶层段时
 * （gradientMapSettings.shadows.* / blackWhiteSettings.* / colorBalanceSettings.*），
 * 「顶层段已存在就跳过」会把后续参数的整棵子树丢掉，只剩第一个参数的叶子。
 * 而 adjustment_validate_settings 允许缺键，原有「默认值能过校验」断言对缺陷免疫——
 * 必须显式检查完整性，且用引擎自己的 json_parse 回读，不引入第二套解析。
 */
COMP_TEST(adjustment_defaults_contain_every_param) {
  for (uint32_t kind = 0; kind < kAdjustmentKindCount; ++kind) {
    const AdjustmentKindInfo* info = adjustment_kind_info(kind);
    COMP_CHECK_MSG(info != nullptr, "kind in range");
    if (info == nullptr) continue;

    const std::string defaults = adjustment_default_settings_json(kind);
    if (info->param_count == 0) {
      COMP_CHECK_MSG(defaults == "{}", info->name);
      continue;
    }
    /* json_parse 仅在失败时写 error（成功时不动），故不能靠 error 是否为空判成败。 */
    std::string parse_error;
    const JsonValue root = json_parse(defaults, &parse_error);
    if (!root.is_object()) {
      COMP_CHECK_MSG(false, info->name);
      continue;
    }

    for (size_t i = 0; i < info->param_count; ++i) {
      const AdjustmentParam& param = info->params[i];
      const char* context = param.json_path;
      /* 逐段下探：中间段缺失即为「叶子不完整」；纯数字段是数组下标（Levels 的 ranges.0）。 */
      const JsonValue* leaf = &root;
      const std::string path(param.json_path);
      size_t start = 0;
      while (leaf != nullptr) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        bool numeric = !key.empty() && key.size() <= 6 &&
                       key.find_first_not_of("0123456789") == std::string::npos;
        if (numeric) {
          const size_t slot = static_cast<size_t>(std::stoul(key));
          leaf = leaf->is_array() && slot < leaf->as_array().size() ? &leaf->as_array()[slot]
                                                                    : nullptr;
        } else {
          leaf = leaf->is_object() ? leaf->find(key) : nullptr;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
      }
      COMP_CHECK_MSG(leaf != nullptr, context);
      if (leaf == nullptr) continue;
      if (param.type == kAdjustmentParamNumber) {
        COMP_CHECK_MSG(leaf->is_number(), context);
        COMP_CHECK_MSG(leaf->as_number() == param.default_value, context);
      } else {
        COMP_CHECK_MSG(leaf->is_bool(), context);
        COMP_CHECK_MSG(leaf->as_bool() == (param.default_value != 0.0), context);
      }
    }
  }
}

COMP_TEST(adjustment_validation_rejects_bad_input) {
  std::string error;

  /* 未知 kind。 */
  COMP_CHECK(!adjustment_validate_settings(99u, "{}", &error));

  /* 未知键。 */
  COMP_CHECK(!adjustment_validate_settings(3u, "{\"bogus\":1}", &error));

  /* 超范围：ExposureSettings.exposureRange -20...20。 */
  COMP_CHECK(!adjustment_validate_settings(3u, "{\"exposureSettings\":{\"exposure\":21}}", &error));
  COMP_CHECK(adjustment_validate_settings(3u, "{\"exposureSettings\":{\"exposure\":20}}", &error));
  /* gammaRange 0.01...9.99：下界 0 必须被拒。 */
  COMP_CHECK(!adjustment_validate_settings(3u, "{\"exposureSettings\":{\"gamma\":0}}", &error));

  /* 类型不符：布尔字段收到数值。 */
  COMP_CHECK(!adjustment_validate_settings(0u, "{\"colorize\":1}", &error));
  /* 类型不符：数值字段收到字符串。 */
  COMP_CHECK(!adjustment_validate_settings(7u, "{\"blurRadius\":\"10\"}", &error));

  /* 非对象。 */
  COMP_CHECK(!adjustment_validate_settings(3u, "[1,2,3]", &error));
  COMP_CHECK(!adjustment_validate_settings(3u, "{oops", &error));

  /* Levels / Curves 的结构复杂，只做浅层对象校验：对象或缺失都放行，标量拒绝。 */
  COMP_CHECK(adjustment_validate_settings(1u, "{\"levels\":{\"channel\":\"RGB\"}}", &error));
  COMP_CHECK(adjustment_validate_settings(1u, "{}", &error));
  COMP_CHECK(!adjustment_validate_settings(1u, "{\"levels\":7}", &error));
  COMP_CHECK(adjustment_validate_settings(2u, "{\"curves\":{}}", &error));
}

COMP_TEST(adjustment_abi_metadata_surface) {
  const char* name = nullptr;
  COMP_CHECK(comp_layer_adjustment_kind_name(3u, &name) == COMP_OK);
  COMP_CHECK_MSG(name != nullptr && std::string(name) == "Exposure", "kind name");
  COMP_CHECK(comp_layer_adjustment_kind_name(12u, &name) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_adjustment_kind_name(3u, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  size_t count = 0;
  COMP_CHECK(comp_layer_adjustment_param_count(3u, &count) == COMP_OK);
  COMP_CHECK_MSG(count == 3, "Exposure param count");
  /* Invert 无参数；面板必须据此不渲染编辑控件。 */
  COMP_CHECK(comp_layer_adjustment_param_count(9u, &count) == COMP_OK);
  COMP_CHECK(count == 0);
  COMP_CHECK(comp_layer_adjustment_param_count(12u, &count) == COMP_ERR_INVALID_ARGUMENT);

  comp_adjustment_param param{};
  COMP_CHECK(comp_layer_adjustment_param_at(3u, 0u, &param) == COMP_OK);
  COMP_CHECK_MSG(std::string(param.json_path) == "exposureSettings.exposure", "param path");
  COMP_CHECK(param.type == COMP_PARAM_NUMBER);
  COMP_CHECK(param.min_value == -20.0 && param.max_value == 20.0 && param.default_value == 0.0);
  COMP_CHECK(param.flags == 0u);
  COMP_CHECK(comp_layer_adjustment_param_at(3u, 2u, &param) == COMP_OK);
  COMP_CHECK_MSG(param.default_value == 1.0, "gamma default");
  /* 越界索引与非法 kind 都必须拒绝。 */
  COMP_CHECK(comp_layer_adjustment_param_at(3u, 3u, &param) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_adjustment_param_at(12u, 0u, &param) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_adjustment_param_at(3u, 0u, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  COMP_CHECK(comp_layer_adjustment_validate(3u, "{}") == COMP_OK);
  COMP_CHECK(comp_layer_adjustment_validate(3u, "{\"nope\":1}") == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_adjustment_validate(99u, "{}") == COMP_ERR_INVALID_ARGUMENT);
}

COMP_TEST(adjustment_abi_add_update_and_comp_round_trip) {
  comp_engine_desc desc = make_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  if (engine == nullptr) return;

  comp_document_desc doc_desc{};
  doc_desc.width = 4;
  doc_desc.height = 4;
  doc_desc.dpi = 72.0f;
  doc_desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &document) == COMP_OK);
  if (document == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  comp_layer_desc layer_desc{};
  layer_desc.name = "adjustment-holder";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &layer) == COMP_OK);

  /* 非法入参必须被拒且不落库。 */
  COMP_CHECK(comp_layer_add_adjustment(document, layer, 99u, "{}") == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_add_adjustment(document, layer, 3u, "{\"exposureSettings\":{\"exposure\":99}}") ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_add_adjustment(document, layer, 3u, "{\"unknown\":1}") == COMP_ERR_INVALID_ARGUMENT);

  /* 合法入参：键名沿用上游字段名（嵌套原样，不展平）。 */
  const char* settings = "{\"exposureSettings\":{\"exposure\":1.5,\"offset\":0.25,\"gamma\":2.2}}";
  COMP_CHECK(comp_layer_add_adjustment(document, layer, 3u, settings) == COMP_OK);
  /* update 只换 settings，沿用既有 kind。 */
  COMP_CHECK(comp_layer_update_adjustment(document, layer, "{\"exposureSettings\":{\"gamma\":1.8}}") ==
             COMP_OK);
  COMP_CHECK(comp_layer_update_adjustment(document, layer, "{\"exposureSettings\":{\"gamma\":50}}") ==
             COMP_ERR_INVALID_ARGUMENT);

  const std::filesystem::path comp = temp_comp_path("adjustment.comp");
  COMP_CHECK(comp_document_save(document, comp.string().c_str()) == COMP_OK);

  /* `.comp` 落盘必须用 engineAdjustments 键承载，且 settings 原样（含未改的字段）。 */
  const std::string manifest = read_text(comp / "manifest.json");
  COMP_CHECK_MSG(!manifest.empty(), "manifest.json 可读");
  COMP_CHECK_MSG(manifest.find("engineAdjustments") != std::string::npos, "engineAdjustments 键");
  COMP_CHECK_MSG(manifest.find("gamma") != std::string::npos, "settings 内容");

  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, comp.string().c_str(), &loaded) == COMP_OK);
  if (loaded != nullptr) {
    comp_size size{};
    COMP_CHECK(comp_document_size(loaded, &size) == COMP_OK);
    COMP_CHECK(size.width == 4 && size.height == 4);

    /* 往返后必须恰好剩一个调整，且 kind 与 settings 逐字节保真（未被展平或重排）。 */
    size_t layers = 0;
    COMP_CHECK(comp_layer_list(loaded, nullptr, 0, &layers) == COMP_OK);
    std::vector<comp_layer_id> ids(layers, 0);
    COMP_CHECK(comp_layer_list(loaded, ids.data(), ids.size(), &layers) == COMP_OK);

    size_t total = 0;
    uint32_t found_kind = 0xFFFFFFFFu;
    std::string found_settings;
    for (comp_layer_id candidate : ids) {
      size_t count = 0;
      if (comp_layer_adjustment_count(loaded, candidate, &count) != COMP_OK) continue;
      for (size_t index = 0; index < count; ++index) {
        uint32_t kind = 0xFFFFFFFFu;
        size_t need = 0;
        /* 两段式：先问长度。 */
        COMP_CHECK(comp_layer_adjustment_at(loaded, candidate, index, &kind, nullptr, 0, &need) == COMP_OK);
        std::vector<char> buffer(need + 1, '\0');
        COMP_CHECK(comp_layer_adjustment_at(loaded, candidate, index, &kind, buffer.data(),
                                            buffer.size(), &need) == COMP_OK);
        found_kind = kind;
        found_settings.assign(buffer.data(), need);
        ++total;
      }
    }
    COMP_CHECK_MSG(total == 1, "往返后调整数量");
    COMP_CHECK_MSG(found_kind == 3u, "往返后 kind");
    COMP_CHECK_MSG(found_settings == "{\"exposureSettings\":{\"gamma\":1.8}}", found_settings.c_str());

    /* 越界索引必须显式拒绝。 */
    size_t probe = 0;
    COMP_CHECK(comp_layer_adjustment_at(loaded, ids.back(), 99u, nullptr, nullptr, 0, &probe) ==
               COMP_ERR_INVALID_ARGUMENT);
    comp_document_destroy(loaded);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
  std::error_code error;
  std::filesystem::remove_all(comp, error);
}

COMP_TEST(adjustment_abi_remove_semantics) {
  comp_engine_desc desc = make_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  if (engine == nullptr) return;

  comp_document_desc doc_desc{};
  doc_desc.width = 4;
  doc_desc.height = 4;
  doc_desc.dpi = 72.0f;
  doc_desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &document) == COMP_OK);

  comp_layer_desc layer_desc{};
  layer_desc.name = "remove-holder";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &layer) == COMP_OK);

  COMP_CHECK(comp_layer_add_adjustment(document, layer, 3u,
                                       "{\"exposureSettings\":{\"exposure\":1}}") == COMP_OK);
  COMP_CHECK(comp_layer_add_adjustment(document, layer, 9u, "{}") == COMP_OK);

  size_t count = 0;
  COMP_CHECK(comp_layer_adjustment_count(document, layer, &count) == COMP_OK && count == 2u);

  /* 越界与不存在的图层必须显式失败，且不改动既有调整。 */
  COMP_CHECK(comp_layer_remove_adjustment(document, layer, 9u) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_remove_adjustment(document, 9999u, 0u) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_adjustment_count(document, layer, &count) == COMP_OK && count == 2u);

  /* 删除第 0 个后仅剩 1 个，且原第 1 个（Invert）前移到索引 0。 */
  COMP_CHECK(comp_layer_remove_adjustment(document, layer, 0u) == COMP_OK);
  COMP_CHECK(comp_layer_adjustment_count(document, layer, &count) == COMP_OK && count == 1u);
  uint32_t kind = 0xFFFFFFFFu;
  size_t need = 0;
  COMP_CHECK(comp_layer_adjustment_at(document, layer, 0u, &kind, nullptr, 0, &need) == COMP_OK);
  COMP_CHECK(kind == 9u);

  /* 删空后再删必须回到越界错误，而不是静默成功。 */
  COMP_CHECK(comp_layer_remove_adjustment(document, layer, 0u) == COMP_OK);
  COMP_CHECK(comp_layer_remove_adjustment(document, layer, 0u) == COMP_ERR_INVALID_ARGUMENT);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------- P0-3：Levels 参数元数据（数组路径） */

/*
 * Levels 的 20 个参数全部走 `levels.ranges.<ch>.<key>` 的数组下标路径：
 * 参数表只有点分键，下标能力必须由 locate_leaf / set_path 提供，否则
 * 面板读不到、写不回，红/绿/蓝通道等于没有编辑器。
 */
COMP_TEST(levels_param_table_covers_four_channels) {
  const AdjustmentKindInfo* info = adjustment_kind_info(1u);
  COMP_CHECK(info != nullptr);
  if (info == nullptr) return;
  COMP_CHECK_MSG(info->editable, "Levels must be editable now that it has real params");
  COMP_CHECK_MSG(info->param_count == 20u, "4 channels x 5 level fields");

  const char* const keys[] = {"black", "gamma", "white", "outputBlack", "outputWhite"};
  for (uint32_t channel = 0; channel < 4u; ++channel) {
    for (const char* key : keys) {
      const std::string expected =
          "levels.ranges." + std::to_string(channel) + "." + std::string(key);
      bool found = false;
      for (size_t i = 0; i < info->param_count; ++i) {
        if (expected == info->params[i].json_path) {
          found = true;
          const AdjustmentParam& param = info->params[i];
          /* 缺省值必须落在量程内，否则默认 settings 自己就过不了校验。 */
          COMP_CHECK_MSG(param.min_value <= param.default_value, expected.c_str());
          COMP_CHECK_MSG(param.default_value <= param.max_value, expected.c_str());
        }
      }
      COMP_CHECK_MSG(found, expected.c_str());
    }
  }

  /*
   * 外壳按「通道 × 5 参」分页（色阶面板的 RGB / R / G / B 通道条）：第 N 通道的
   * 5 行必须是 params[N*5 .. N*5+4]，字段顺序与 keys 一致。这条不变式一旦破了，
   * 面板会把 A 通道的滑杆画到 B 通道的值上——这里钉死，外壳不再重复猜。
   */
  const size_t per_channel = sizeof(keys) / sizeof(keys[0]);
  for (size_t i = 0; i < info->param_count; ++i) {
    const std::string expected = "levels.ranges." + std::to_string(i / per_channel) + "." +
                                 keys[i % per_channel];
    COMP_CHECK_MSG(expected == info->params[i].json_path, expected.c_str());
  }
}

/* 缺省 settings 必须长成完整的 4 段 ranges，且能按数组路径读回。 */
COMP_TEST(levels_defaults_and_read_value_use_array_paths) {
  const std::string defaults = adjustment_default_settings_json(1u);
  std::string error;
  COMP_CHECK_MSG(adjustment_validate_settings(1u, defaults, &error), error.c_str());

  const double expected_defaults[4][5] = {
      {0.0, 1.0, 255.0, 0.0, 255.0},
      {0.0, 1.0, 255.0, 0.0, 255.0},
      {0.0, 1.0, 255.0, 0.0, 255.0},
      {0.0, 1.0, 255.0, 0.0, 255.0},
  };
  const char* const keys[] = {"black", "gamma", "white", "outputBlack", "outputWhite"};
  for (uint32_t channel = 0; channel < 4u; ++channel) {
    for (size_t k = 0; k < 5u; ++k) {
      const std::string path =
          "levels.ranges." + std::to_string(channel) + "." + keys[k];
      double value = -1.0;
      COMP_CHECK_MSG(adjustment_read_value(defaults, path.c_str(), &value, nullptr),
                     path.c_str());
      COMP_CHECK_MSG(value == expected_defaults[channel][k], path.c_str());
    }
  }

  /* 非恒等的值也要能从数组路径读回（写侧由 build_settings 覆盖）。 */
  const std::string custom =
      R"({"levels":{"ranges":[{"black":40,"gamma":2,"white":200,"outputBlack":10,"outputWhite":240}]}})";
  double value = 0.0;
  COMP_CHECK(adjustment_read_value(custom, "levels.ranges.0.black", &value, nullptr) && value == 40.0);
  COMP_CHECK(adjustment_read_value(custom, "levels.ranges.0.gamma", &value, nullptr) && value == 2.0);
  COMP_CHECK(adjustment_read_value(custom, "levels.ranges.0.outputWhite", &value, nullptr) &&
             value == 240.0);
  /* 数组下标越界 = 键缺失，不是「解析失败」。 */
  COMP_CHECK(!adjustment_read_value(custom, "levels.ranges.3.black", &value, nullptr));
  /* 把数组当对象下探必须被识别为坏结构。 */
  COMP_CHECK(!adjustment_validate_settings(1u, R"({"levels":{"ranges":{"black":0}}})", &error));
}

/* 面板扁平值 → settings：20 个空串应原样产出完整缺省数组。 */
COMP_TEST(levels_build_settings_emits_four_ranges) {
  const AdjustmentKindInfo* info = adjustment_kind_info(1u);
  COMP_CHECK(info != nullptr);
  if (info == nullptr) return;

  std::vector<std::string> values(info->param_count);
  std::string json;
  std::string error;
  COMP_CHECK_MSG(adjustment_build_settings(1u, values, &json, &error), error.c_str());

  std::string parse_error;
  const JsonValue root = json_parse(json, &parse_error);
  const JsonValue* levels = root.find("levels");
  COMP_CHECK(levels != nullptr && levels->is_object());
  if (levels == nullptr) return;
  const JsonValue* ranges = levels->find("ranges");
  COMP_CHECK(ranges != nullptr && ranges->is_array());
  if (ranges == nullptr) return;
  COMP_CHECK_MSG(ranges->as_array().size() == 4u, "one range per channel");
  for (const JsonValue& range : ranges->as_array()) {
    COMP_CHECK(range.is_object());
    for (const char* key : {"black", "gamma", "white", "outputBlack", "outputWhite"}) {
      COMP_CHECK_MSG(range.find(key) != nullptr, key);
    }
  }

  /* 只改一个通道：其余通道的缺省值必须保留（数组下标不能互相覆盖）。 */
  values[0] = "40"; /* levels.ranges.0.black */
  COMP_CHECK_MSG(adjustment_build_settings(1u, values, &json, &error), error.c_str());
  double value = 0.0;
  COMP_CHECK(adjustment_read_value(json, "levels.ranges.0.black", &value, nullptr) && value == 40.0);
  COMP_CHECK(adjustment_read_value(json, "levels.ranges.1.gamma", &value, nullptr) && value == 1.0);
}

/* ------------------------------------------------ P0-3：Curves 控制点读写 */

COMP_TEST(curves_set_points_round_trip) {
  std::string json;
  std::string error;
  const std::vector<std::pair<double, double>> points = {
      {0.0, 0.0}, {64.0, 96.0}, {255.0, 255.0}};
  COMP_CHECK_MSG(curves_set_points("{}", 0u, points, &json, &error), error.c_str());
  COMP_CHECK_MSG(curves_validate_settings(json, &error), error.c_str());

  const std::vector<std::pair<double, double>> read_back = curves_read_points(json, 0u);
  COMP_CHECK(read_back.size() == points.size());
  for (size_t i = 0; i < points.size() && i < read_back.size(); ++i) {
    COMP_CHECK(read_back[i].first == points[i].first);
    COMP_CHECK(read_back[i].second == points[i].second);
  }
  /* 未写入的通道仍是缺省（读空数组 = 恒等）。 */
  COMP_CHECK(curves_read_points(json, 1u).empty());
}

/* 改写一个通道不能动其余键与其余通道。 */
COMP_TEST(curves_set_points_preserves_other_keys) {
  const std::string base =
      R"({"curves":{"channel":2,"channels":[[{"x":0,"y":255},{"x":255,"y":0}],[],[]]}})";
  std::string json;
  std::string error;
  COMP_CHECK_MSG(curves_set_points(base, 1u, {{0.0, 10.0}, {255.0, 200.0}}, &json, &error),
                 error.c_str());

  std::string parse_error;
  const JsonValue root = json_parse(json, &parse_error);
  const JsonValue* curves = root.find("curves");
  COMP_CHECK(curves != nullptr && curves->is_object());
  if (curves == nullptr) return;
  COMP_CHECK(curves->integer_or("channel", -1) == 2);
  /* 通道 0 原样保留（含方向翻转）。 */
  const std::vector<std::pair<double, double>> channel0 = curves_read_points(json, 0u);
  COMP_CHECK(channel0.size() == 2u);
  if (channel0.size() == 2u) {
    COMP_CHECK(channel0[0].second == 255.0);
    COMP_CHECK(channel0[1].second == 0.0);
  }
  const std::vector<std::pair<double, double>> channel1 = curves_read_points(json, 1u);
  COMP_CHECK(channel1.size() == 2u);
  if (channel1.size() == 2u) {
    COMP_CHECK(channel1[0].second == 10.0);
    COMP_CHECK(channel1[1].second == 200.0);
  }
}

/* 空点列 = 复位恒等；乱序点按 x 归位（编辑器拖交叉 = 交换）。 */
COMP_TEST(curves_set_points_sorts_and_defaults_to_identity) {
  std::string json;
  std::string error;
  COMP_CHECK_MSG(curves_set_points("{}", 2u, {}, &json, &error), error.c_str());
  const std::vector<std::pair<double, double>> identity = curves_read_points(json, 2u);
  COMP_CHECK(identity.size() == 2u);
  if (identity.size() == 2u) {
    COMP_CHECK(identity[0] == std::make_pair(0.0, 0.0));
    COMP_CHECK(identity[1] == std::make_pair(255.0, 255.0));
  }

  COMP_CHECK_MSG(curves_set_points(json, 2u, {{255.0, 255.0}, {0.0, 0.0}, {128.0, 64.0}}, &json,
                                    &error),
                 error.c_str());
  const std::vector<std::pair<double, double>> sorted = curves_read_points(json, 2u);
  COMP_CHECK(sorted.size() == 3u);
  for (size_t i = 1; i < sorted.size(); ++i) COMP_CHECK(sorted[i - 1].first < sorted[i].first);
}

/* 结构校验：每条上游 isValid 的约束都必须拒绝。 */
COMP_TEST(curves_validation_rejects_invalid_channels) {
  std::string error;
  COMP_CHECK(curves_validate_settings("", &error));
  COMP_CHECK(curves_validate_settings("{}", &error));
  COMP_CHECK(curves_validate_settings(R"({"curves":{}})", &error));
  COMP_CHECK(curves_validate_settings(R"({"curves":{"channels":[]}})", &error));
  COMP_CHECK(curves_validate_settings(R"({"curves":{"channels":[null,[],[]]}})", &error));
  /* 紧凑 [x,y] 形式与对象形式等价。 */
  COMP_CHECK(curves_validate_settings(R"({"curves":{"channels":[[[0,0],[255,255]]]}})", &error));

  COMP_CHECK(!curves_validate_settings(R"({"curves":7})", &error));
  COMP_CHECK(!curves_validate_settings(R"({"curves":{"channels":{}}})", &error));
  COMP_CHECK(!curves_validate_settings(R"({"curves":{"channels":[[{"x":0,"y":0}]]}})", &error));
  COMP_CHECK(!curves_validate_settings(
      R"({"curves":{"channels":[[{"x":0,"y":0},{"x":0,"y":255},{"x":255,"y":255}]]}})", &error));
  COMP_CHECK(!curves_validate_settings(
      R"({"curves":{"channels":[[{"x":0,"y":0},{"x":128,"y":255}]]}})", &error));
  COMP_CHECK(!curves_validate_settings(
      R"({"curves":{"channels":[[{"x":10,"y":0},{"x":255,"y":255}]]}})", &error));
  COMP_CHECK(!curves_validate_settings(
      R"({"curves":{"channels":[[{"x":0,"y":0},{"x":255,"y":256}]]}})", &error));
  COMP_CHECK(!curves_validate_settings(
      R"({"curves":{"channels":[[{"x":0,"y":0},{"x":128,"y":255},{"x":64,"y":0},{"x":255,"y":255}]]}})",
      &error));
  /* 超过 4 个通道。 */
  COMP_CHECK(!curves_validate_settings(
      R"({"curves":{"channels":[[{"x":0,"y":0},{"x":255,"y":255}],[],[],[],[]]}})", &error));
}

/* Curves 的写侧校验必须接进 adjustment_validate_settings（kind 2）。 */
COMP_TEST(curves_kind_validation_uses_point_rules) {
  std::string error;
  COMP_CHECK(adjustment_validate_settings(2u, R"({"curves":{"channels":[[{"x":0,"y":0},{"x":255,"y":128}]]}})",
                                          &error));
  COMP_CHECK(!adjustment_validate_settings(2u, R"({"curves":{"channels":[[{"x":0,"y":0}]]}})", &error));
}

/* 面板传入非法点列必须整体拒绝，且不产生半截 JSON。 */
COMP_TEST(curves_set_points_rejects_invalid_input) {
  std::string json = "SENTINEL";
  std::string error;
  COMP_CHECK(!curves_set_points("{}", 0u, {{0.0, 0.0}}, &json, &error));
  COMP_CHECK(json == "SENTINEL"); /* 失败不改 out_json */
  COMP_CHECK(!curves_set_points("{}", 0u, {{0.0, 0.0}, {128.0, 1.0}}, &json, &error));
  COMP_CHECK(!curves_set_points("{}", 0u, {{0.0, 0.0}, {0.0, 255.0}, {255.0, 255.0}}, &json,
                                 &error));
  COMP_CHECK(!curves_set_points("{}", 4u, {{0.0, 0.0}, {255.0, 255.0}}, &json, &error));
}

/* ABI 面：数量/读点/写点 + 两段式缓冲区协议（面板直接调这一层）。 */
/*
 * P0-3 验收：曲线控制点落盘/重载后逐点同一。
 *
 * 面板每次拖动都把整条通道的点列重写进 settings，工程文件再原样带走 settings，
 * 所以「往返后点列不变」是这条交互的底线——不是重采样、不是退回恒等。
 */
COMP_TEST(curves_points_survive_comp_round_trip) {
  comp_engine_desc desc = make_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  if (engine == nullptr) return;

  comp_document_desc doc_desc{};
  doc_desc.width = 4;
  doc_desc.height = 4;
  doc_desc.dpi = 72.0f;
  doc_desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &document) == COMP_OK);
  if (document == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  comp_layer_desc layer_desc{};
  layer_desc.name = "curve-holder";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &layer) == COMP_OK);

  /* 通道 0（RGB）抬中间调、通道 2（G）压暗端点，其余通道留空（= 恒等）。 */
  const std::vector<std::pair<double, double>> rgb_points = {
      {0.0, 0.0}, {128.0, 190.0}, {255.0, 255.0}};
  const std::vector<std::pair<double, double>> green_points = {
      {0.0, 8.0}, {96.0, 140.0}, {255.0, 250.0}};
  std::string first;
  std::string settings;
  std::string error;
  COMP_CHECK_MSG(curves_set_points("{}", 0u, rgb_points, &first, &error), error.c_str());
  COMP_CHECK_MSG(curves_set_points(first, 2u, green_points, &settings, &error), error.c_str());
  COMP_CHECK(comp_layer_adjustment_validate(COMP_ADJUSTMENT_CURVES, settings.c_str()) == COMP_OK);
  COMP_CHECK(comp_layer_add_adjustment(document, layer, COMP_ADJUSTMENT_CURVES, settings.c_str()) ==
             COMP_OK);

  const std::filesystem::path comp = temp_comp_path("curve-points.comp");
  COMP_CHECK(comp_document_save(document, comp.string().c_str()) == COMP_OK);

  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, comp.string().c_str(), &loaded) == COMP_OK);
  if (loaded != nullptr) {
    size_t layers = 0;
    COMP_CHECK(comp_layer_list(loaded, nullptr, 0, &layers) == COMP_OK);
    std::vector<comp_layer_id> ids(layers, 0);
    COMP_CHECK(comp_layer_list(loaded, ids.data(), ids.size(), &layers) == COMP_OK);

    std::string found;
    uint32_t found_kind = 0xFFFFFFFFu;
    size_t found_total = 0;
    for (comp_layer_id candidate : ids) {
      size_t count = 0;
      if (comp_layer_adjustment_count(loaded, candidate, &count) != COMP_OK) continue;
      for (size_t index = 0; index < count; ++index) {
        uint32_t kind = 0xFFFFFFFFu;
        size_t need = 0;
        COMP_CHECK(comp_layer_adjustment_at(loaded, candidate, index, &kind, nullptr, 0, &need) ==
                   COMP_OK);
        std::vector<char> buffer(need + 1u, '\0');
        COMP_CHECK(comp_layer_adjustment_at(loaded, candidate, index, &kind, buffer.data(),
                                            buffer.size(), &need) == COMP_OK);
        found.assign(buffer.data(), need);
        found_kind = kind;
        ++found_total;
      }
    }
    COMP_CHECK_MSG(found_total == 1u, "往返后调整数量");
    COMP_CHECK_MSG(found_kind == COMP_ADJUSTMENT_CURVES, "往返后 kind");
    /* settings 逐字节同一：写盘不许重排点、不许改数值写法。 */
    COMP_CHECK_MSG(found == settings, found.c_str());

    for (uint32_t channel = 0; channel < COMP_CURVE_CHANNEL_COUNT; ++channel) {
      size_t count = 0;
      COMP_CHECK(comp_layer_curves_point_count(found.c_str(), channel, &count) == COMP_OK);
      const std::vector<std::pair<double, double>>* expected = nullptr;
      if (channel == 0u) expected = &rgb_points;
      if (channel == 2u) expected = &green_points;
      const size_t wanted = expected != nullptr ? expected->size() : 0u;
      COMP_CHECK_MSG(count == wanted, "往返后控制点数量");
      for (size_t i = 0; i < count && i < wanted; ++i) {
        double x = -1.0;
        double y = -1.0;
        COMP_CHECK(comp_layer_curves_point_at(found.c_str(), channel, i, &x, &y) == COMP_OK);
        COMP_CHECK(x == (*expected)[i].first);
        COMP_CHECK(y == (*expected)[i].second);
      }
    }
    comp_document_destroy(loaded);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
  std::error_code remove_error;
  std::filesystem::remove_all(comp, remove_error);
}

COMP_TEST(curves_abi_reads_and_writes_points) {
  size_t channels = 0;
  COMP_CHECK(comp_layer_curves_channel_count(&channels) == COMP_OK);
  COMP_CHECK_MSG(channels == COMP_CURVE_CHANNEL_COUNT, "channel count");
  COMP_CHECK(comp_layer_curves_channel_count(nullptr) == COMP_ERR_INVALID_ARGUMENT);

  size_t count = 99u;
  /* 未写入的通道 = 0 个存点（语义即恒等曲线）。 */
  COMP_CHECK(comp_layer_curves_point_count("{}", 0u, &count) == COMP_OK);
  COMP_CHECK(count == 0u);
  COMP_CHECK(comp_layer_curves_point_count(nullptr, 0u, &count) == COMP_OK);
  COMP_CHECK(comp_layer_curves_point_count("{}", COMP_CURVE_CHANNEL_COUNT, &count) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_curves_point_count("{}", 0u, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  double x = -1.0;
  double y = -1.0;
  COMP_CHECK(comp_layer_curves_point_at("{}", 0u, 0u, &x, &y) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(x == -1.0 && y == -1.0); /* 失败不写 out */

  const double points[] = {0.0, 0.0, 64.0, 96.0, 255.0, 255.0};
  char buffer[256] = {};
  size_t size = 0;
  COMP_CHECK(comp_layer_curves_set_points("{}", 0u, points, 3u, buffer, sizeof(buffer), &size) ==
             COMP_OK);
  COMP_CHECK_MSG(std::string(buffer).size() == size, "out_size excludes the NUL");
  COMP_CHECK(comp_layer_adjustment_validate(COMP_ADJUSTMENT_CURVES, buffer) == COMP_OK);

  COMP_CHECK(comp_layer_curves_point_count(buffer, 0u, &count) == COMP_OK);
  COMP_CHECK(count == 3u);
  COMP_CHECK(comp_layer_curves_point_at(buffer, 0u, 1u, &x, &y) == COMP_OK);
  COMP_CHECK(x == 64.0 && y == 96.0);
  /* 写一个通道不动其余通道。 */
  COMP_CHECK(comp_layer_curves_point_count(buffer, 1u, &count) == COMP_OK);
  COMP_CHECK(count == 0u);

  /* 两段式：容量 0 只报长度；长度与通道无关地等于同一份 JSON 的字节数。 */
  char reference[256] = {};
  size_t reference_size = 0;
  COMP_CHECK(comp_layer_curves_set_points("{}", 1u, points, 3u, reference, sizeof(reference),
                                          &reference_size) == COMP_OK);
  size_t needed = 0;
  COMP_CHECK(comp_layer_curves_set_points("{}", 1u, points, 3u, nullptr, 0u, &needed) == COMP_OK);
  COMP_CHECK(needed == reference_size);
  /* 容量不足：按 NUL 截断，仍报完整长度。 */
  char tiny[8] = {};
  size_t tiny_size = 0;
  COMP_CHECK(comp_layer_curves_set_points("{}", 1u, points, 3u, tiny, sizeof(tiny), &tiny_size) ==
             COMP_OK);
  COMP_CHECK(tiny_size == reference_size);
  COMP_CHECK(std::string(tiny).size() == sizeof(tiny) - 1u);

  /* 非法输入整体拒绝，不写半成品。 */
  const double duplicate_x[] = {0.0, 0.0, 0.0, 255.0, 255.0, 255.0};
  char untouched[256] = "SENTINEL";
  size_t untouched_size = 0;
  COMP_CHECK(comp_layer_curves_set_points("{}", 0u, duplicate_x, 3u, untouched, sizeof(untouched),
                                          &untouched_size) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(std::string(untouched) == "SENTINEL");
  COMP_CHECK(comp_layer_curves_set_points("{}", 0u, nullptr, 2u, untouched, sizeof(untouched),
                                          &untouched_size) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_curves_set_points("{}", COMP_CURVE_CHANNEL_COUNT, points, 3u, untouched,
                                          sizeof(untouched), &untouched_size) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(std::string(untouched) == "SENTINEL");
}
