// AI生成
/*
 * settings_json_util.h — 「参数表 ↔ settings JSON」共用的路径工具。
 *
 * 调整图层（domain/adjustment_table）与填充图层（domain/fill_layer）用同一套约定：
 *   - `json_path` 是相对 settings 根对象的点分键路径，纯数字段表示数组下标
 *     （如 `levels.ranges.0.gamma`）；
 *   - 缺失的中间段视为「未提供」（合法），中间段存在但类型不符视为格式错误；
 *   - 面板给的是扁平文本值数组，构造结果要能再过一次同一套校验。
 *
 * 这些函数原来藏在 adjustment_table.cpp 的匿名命名空间里；抽出成共享实现是为了让
 * 填充图层复用**同一份**解析语义，而不是复制一份容易走样的副本。
 */
#ifndef COMPOSITOR_DOMAIN_SETTINGS_JSON_UTIL_H
#define COMPOSITOR_DOMAIN_SETTINGS_JSON_UTIL_H

#include <cstddef>
#include <string>
#include <vector>

#include "format/comp_json.h"

namespace compositor {
namespace settings_json {

/* 纯数字段 = 数组下标（最多 6 位，避免把超长数字当合法下标）。 */
bool segment_index(const std::string& text, size_t* out);

/* 按点分路径定位叶子；`malformed` 回报「中间段类型不符」（叶子缺失不算错误）。 */
const JsonValue* locate_leaf(const JsonValue& root, const char* path, bool* malformed);

/* 数值的紧凑文本（整数值不带小数点）。 */
std::string number_to_text(double value);

/* 点分路径切段。 */
std::vector<std::string> split_path(const char* path);

/*
 * 在（可能是新建的）对象/数组树里按段路径放置叶子。已存在的中间对象/数组会被
 * 保留并继续向下合并，而非整棵替换；数字段按数组下标处理，缺位补 null。
 */
JsonValue set_path(const JsonValue& node, const std::vector<std::string>& segments, size_t index,
                   JsonValue value);

/* 面板文本 → 布尔（"1"/"true" 与 "0"/"false"，大小写各认两种写法）。 */
bool parse_bool_text(const std::string& text, bool* out);

}  // namespace settings_json
}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_SETTINGS_JSON_UTIL_H */
