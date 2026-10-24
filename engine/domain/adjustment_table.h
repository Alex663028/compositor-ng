// AI生成
/*
 * adjustment_table.h — 调整图层参数的单一真源（键名表）。
 *
 * 键名逐字对齐上游 `Compositor/Document/LayerAdjustment.swift` 及其嵌套 Settings 结构
 * （ExposureSettings / GradientMapSettings / GrainSettings / BlackWhiteSettings /
 *  ColorBalanceSettings / LevelsSettings / HueSaturationSettings），供三处共用：
 *
 *   1. `comp_layer_add_adjustment` / `comp_layer_update_adjustment` 的入参校验；
 *   2. 外壳调整面板的参数控件生成（经 C ABI 的 param 枚举接口）；
 *   3. `docs/adjustment-json-keys.md` 键名表文档。
 *
 * 命名：`json_path` 是相对 settings 对象的键路径（点分），**就是上游 Codable 的字段名**，
 * 不做任何重命名或展平，保证 `.comp` 往返保真。
 */
#ifndef COMPOSITOR_DOMAIN_ADJUSTMENT_TABLE_H
#define COMPOSITOR_DOMAIN_ADJUSTMENT_TABLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace compositor {

/* 参数类型（与 ABI `comp_param_type` 数值一致）。 */
enum AdjustmentParamType : uint32_t {
  kAdjustmentParamNumber = 0,
  kAdjustmentParamBool = 1,
};

struct AdjustmentParam {
  const char* json_path; /* 相对 settings 的点分键路径，如 "exposureSettings.gamma" */
  const char* label;     /* 面板显示名（ASCII，避免 GBK 编译风险） */
  uint32_t type;         /* AdjustmentParamType */
  double min_value;      /* 数值型下界（含）；非数值型忽略 */
  double max_value;      /* 数值型上界（含） */
  double default_value;  /* 缺省值（布尔型：0 = false） */
};

struct AdjustmentKindInfo {
  uint32_t kind;                 /* .comp 的 engineAdjustments[].kind（= 上游 CaseIterable 序号） */
  const char* name;              /* 上游 `AdjustmentKind` rawValue，逐字一致 */
  const char* summary;           /* 面板提示语 */
  const AdjustmentParam* params; /* 可编辑参数表；无则 nullptr */
  size_t param_count;
  bool editable;                 /* 面板是否提供参数编辑（Curves/Levels 需专用编辑器） */
  const char* extra_roots;       /* 允许出现但不由面板编辑的顶层键（空格分隔，可空串） */
};

/*
 * kind 编码总数（0..24 合法）。
 *
 * 0..11 与上游 `AdjustmentKind` 的 CaseIterable 顺序一致（冻结，不得改动序号）；
 * 12..15 是本仓追加的破坏性调整（Brightness/Contrast、Desaturate、Threshold、
 * Posterize），16..24 是补齐的 Photoshop「Image > Adjustments」其余条目
 * （Vibrance … Auto Color）。全部追加在尾部，既不挪动既有 kind 的编码，也不影响
 * `.comp` 里既有的 `engineAdjustments[].kind`。
 */
constexpr uint32_t kAdjustmentKindCount = 25;

/* 上游 `AdjustmentKind` 声明的种类数：0..11。追加 kind 从此之后开始编号。 */
constexpr uint32_t kUpstreamAdjustmentKindCount = 12;

/* 该 kind 是否可作为**破坏性调整**（Image > Adjustments）作用在像素图层上。 */
bool adjustment_kind_is_destructive(uint32_t kind);

/* kind 非法（>= kAdjustmentKindCount）时返回 nullptr。 */
const AdjustmentKindInfo* adjustment_kind_info(uint32_t kind);

/*
 * 校验 settings JSON：必须是 JSON 对象；顶层键必须在白名单内；已声明的数值/布尔叶子
 * 必须在范围内且类型正确。空串按 "{}" 处理。失败时写 error（UTF-8）。
 */
bool adjustment_validate_settings(uint32_t kind, const std::string& settings_json, std::string* error);

/* 该 kind 的缺省 settings JSON（紧凑）。无可编辑参数时返回 "{}"。 */
std::string adjustment_default_settings_json(uint32_t kind);

/*
 * 从 settings JSON 按点分路径读取标量叶子（供外壳面板回填既有调整的值）。
 * 命中数值写 out_number、命中布尔写 out_boolean（0/1），两者都可为 nullptr。
 * 路径缺失、中间段非对象、或叶子不是标量时返回 false（调用方回落到缺省值）。
 */
bool adjustment_read_value(const std::string& settings_json, const char* json_path,
                          double* out_number, int32_t* out_boolean);

/*
 * 用面板的扁平值数组构造 settings JSON：values[i] 对应参数表第 i 项。
 * 每项为 ASCII 文本（数值如 "1.5"，布尔用 "0"/"1"），空串表示沿用该参数缺省值。
 * values 长度必须等于该 kind 的 param_count。构造结果会再过一次校验后才回报成功。
 */
bool adjustment_build_settings(uint32_t kind, const std::vector<std::string>& values,
                              std::string* out_json, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_ADJUSTMENT_TABLE_H */
