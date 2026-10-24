// AI生成
/*
 * effect_table.h — 图层效果参数的单一真源（键名/量程/缺省值表）。
 *
 * 键名与取值范围逐字对齐 `compositing/effect_eval.cpp` 的 `read_*` 系列
 * （`s.ranged(key, fallback, lo, hi)` / `s.unit(key, fallback)`）：
 * 效果 settings 是**扁平** JSON 对象，键就是 "size" / "opacity" / "red" 等，
 * 没有调整图层那种嵌套 Settings 子树。
 *
 * 为什么需要这张表：`comp_layer_set_effect` 的 settings 按契约是不透明字符串
 * （见 comp_layer.h），内核不校验；但外壳要画参数面板，就必须知道「有哪些键、
 * 量程多少、缺省多少」。此前这几个数被抄在 `shells/windows/src/main.cpp` 的
 * `effect_kind_defaults()` 里，只用于状态栏摘要——面板一旦要读写，抄一份就必然
 * 与 `effect_eval.cpp` 漂移。本表是唯一一份，ABI 的 param_count/param_at 直接
 * 转出它，外壳不再自持数值。
 *
 * 边界（刻意为之）：
 *   - 只描述**已知的 10 个 kind**；未知 kind 由 set_effect 原样落盘，不在这里拒绝
 *     （否则 `.comp` 与上游互通时会丢未知效果）。
 *   - 表里没有的键不会被 build_settings 删除：它只在既有 JSON 上做深合并，
 *     未知键原样保留（前向兼容）。
 */
#ifndef COMPOSITOR_DOMAIN_EFFECT_TABLE_H
#define COMPOSITOR_DOMAIN_EFFECT_TABLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace compositor {

/* 参数类型（与 ABI `comp_param_type` 数值一致）。 */
enum EffectParamType : uint32_t {
  kEffectParamNumber = 0,
  kEffectParamBool = 1,
};

struct EffectParam {
  const char* json_path; /* settings 顶层键，如 "size"（效果的键都是单段） */
  const char* label;     /* 面板显示名（ASCII，避免 GBK 编译风险） */
  uint32_t type;         /* EffectParamType */
  double min_value;      /* 数值型下界（含）；非数值型忽略 */
  double max_value;      /* 数值型上界（含） */
  double default_value;  /* 缺省值（布尔型：0 = false）；必须与 effect_eval.cpp 一致 */
};

struct EffectKindInfo {
  uint32_t effect;               /* .comp 的 engineEffects[].effect */
  const EffectParam* params;     /* 可编辑参数表 */
  size_t param_count;
};

/*
 * 已知 kind 总数（0..9 合法）。不叫 kEffectKindCount：effect_eval.h 的同名枚举
 * 已在同一命名空间里，重名会在同时包含两者的翻译单元里直接冲突。
 */
constexpr uint32_t kEffectParamKindCount = 10;

/* effect 非法（>= kEffectKindCount）时返回 nullptr。 */
const EffectKindInfo* effect_kind_info(uint32_t effect);

/*
 * 从 settings JSON 按 json_path 读标量叶子，供面板回填既有值：
 * 命中数值写 out_number、命中布尔写 out_boolean（0/1），两者都可为 nullptr。
 * 路径缺失、中间段非对象、或叶子不是标量时返回 false（调用方回落到
 * param.default_value）。空串按 "{}" 处理。
 */
bool effect_read_value(const std::string& settings_json, const char* json_path,
                       double* out_number, int32_t* out_boolean);

/*
 * 用面板的扁平值数组在**既有 settings** 上合并出新 settings：
 *   - `base_json` 为已有 settings（空串按 "{}"）；未知键与未编辑的键原样保留，
 *     只覆盖 values 里非空的那几项。这是与 adjustment_build_settings 的关键差别：
 *     效果 settings 允许未知键（前向兼容），不能整棵重建。
 *   - values[i] 对应参数表第 i 项；长度必须等于 param_count；
 *     每项是 ASCII 文本（数值如 "12.5"，布尔用 "0"/"1"），空串 = 该项不动。
 *   - 数值越界 / 非有限 / 解析失败都返回 false 并写 error（ABI 侧再拒绝，
 *     与「面板夹取、ABI 拒绝」的既有口径一致）。
 *   - 结果不保证通过任何校验（契约上效果 settings 本来就不校验）。
 */
bool effect_build_settings(uint32_t effect, const std::vector<std::string>& values,
                           const std::string& base_json, std::string* out_json,
                           std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_EFFECT_TABLE_H */
