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
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "domain/adjustment_table.h"
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
      /* 逐段下探：中间段缺失即为「叶子不完整」。 */
      const JsonValue* leaf = &root;
      const std::string path(param.json_path);
      size_t start = 0;
      while (leaf != nullptr && leaf->is_object()) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        leaf = leaf->find(key);
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
