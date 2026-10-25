// AI生成
/*
 * fill_layer.h — 新建填充层（纯色 / 渐变 / 图案）的参数表与渲染。
 *
 * 语义与文字图层、智能对象同族：**填充参数是真源，图层像素是渲染缓存**。
 *   - `Layer::fill`（domain/layer.h）持有 kind + settings JSON；
 *   - `fill_render()` 按参数在图层像素空间（原点 0,0、不读选区）渲出一整幅
 *     预乘 RGBA8，写进图层的像素缓存；
 *   - 合成、缩略图、合并、GPU 纹理仍走既有像素路径，合成内核不感知填充层。
 *
 * 因此本文件是「参数 → 像素」的唯一实现：ABI 的 add/set_fill、`.comp` 读盘后的
 * 重渲、外壳面板的实时预览都调它，不存在第二套渐变/图案算法。
 *
 * 键名与量程是本仓自定的（上游没有对应的 Swift 结构），所以不参与与上游的
 * kind 互操作；`.comp` 里以 `engineFill` 键原样保存 settings JSON。
 */
#ifndef COMPOSITOR_DOMAIN_FILL_LAYER_H
#define COMPOSITOR_DOMAIN_FILL_LAYER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "domain/layer.h"

namespace compositor {

/* 填充种类（数值即 ABI `comp_fill_kind` / .comp `engineFill.kind`）。 */
enum FillKind : uint32_t {
  kFillKindNone = 0,     /* 不是填充层 */
  kFillKindSolid = 1,    /* 纯色 */
  kFillKindGradient = 2, /* 两色标线性渐变 */
  kFillKindPattern = 3,  /* 内置图案平铺 */
};

constexpr uint32_t kFillKindCount = 4;

/* 参数类型（与 ABI `comp_fill_param.type` 数值一致，也和调整面板同一套）。 */
enum FillParamType : uint32_t {
  kFillParamNumber = 0,
  kFillParamBool = 1,
};

struct FillParam {
  const char* json_path; /* 相对 settings 的点分键路径 */
  const char* label;     /* 面板显示名（ASCII） */
  uint32_t type;         /* FillParamType */
  double min_value;
  double max_value;
  double default_value;
};

struct FillKindInfo {
  uint32_t kind;
  const char* name;           /* 显示名（面板标题 / .comp 可读性） */
  const char* summary;        /* 面板提示语 */
  const char* layer_name;     /* 新建图层的缺省名 */
  const FillParam* params;
  size_t param_count;
};

/* kind 非法时返回 nullptr（0 是「不是填充层」，同样返回 nullptr）。 */
const FillKindInfo* fill_kind_info(uint32_t kind);

/* kind 的显示名：0 也合法（返回 "None"）；kind 越界返回 nullptr。
 * 与 fill_kind_info 分开是因为 ABI 的 comp_fill_kind_name 必须能回报 "None"。 */
const char* fill_kind_name(uint32_t kind);

/* 新建图层用的缺省名；kind 非法时返回空串。 */
const char* fill_layer_default_name(uint32_t kind);

/*
 * 校验 settings JSON：必须是对象；顶层键必须在参数表白名单内；已声明的叶子必须
 * 类型正确且在量程内。空串按 "{}" 处理。缺键合法（= 用缺省值）。
 */
bool fill_validate_settings(uint32_t kind, const std::string& settings_json, std::string* error);

/* 该 kind 的缺省 settings JSON（紧凑）。 */
std::string fill_default_settings_json(uint32_t kind);

/* 按点分路径读标量叶子（面板回填用）；缺失/类型不符返回 false。 */
bool fill_read_value(const std::string& settings_json, const char* json_path, double* out_number,
                     int32_t* out_boolean);

/*
 * 用面板的扁平文本值构造 settings JSON：values[i] 对应参数表第 i 项，空串 = 沿用
 * 缺省值。构造结果会再过一次 fill_validate_settings 才回报成功。
 */
bool fill_build_settings(uint32_t kind, const std::vector<std::string>& values,
                         std::string* out_json, std::string* error);

/*
 * 按参数渲出一整幅预乘 RGBA8：`out` 的 width/height/format 被重置为
 * (width, height, COMP_PIXEL_RGBA8)，像素缓冲整体替换（不读旧内容）。
 *
 * 参数非法 / 尺寸非法 / 分配失败 -> false 并写 error；此时 `out` 为空缓冲。
 * 渲染是**确定性**的：同一份 settings + 同一尺寸逐字节相同，不依赖随机源或时钟。
 */
bool fill_render(uint32_t kind, const std::string& settings_json, int32_t width, int32_t height,
                 ImageBuffer* out, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_FILL_LAYER_H */
