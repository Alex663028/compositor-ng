// AI生成
/*
 * layer.h — 图层领域模型（内部 C++ 类型）。
 *
 * 与 ABI 的关系：comp_layer_desc / comp_image / comp_mask 是 ABI 视图，
 * 本文件是引擎内部持有的完整模型（含像素、蒙版、调整、效果与子节点）。
 *
 * 数据布局与 10 个像素内核对齐：RGBA8 预乘、4 字节/像素、行优先、自上而下。
 */
#ifndef COMPOSITOR_DOMAIN_LAYER_H
#define COMPOSITOR_DOMAIN_LAYER_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_layer.h"
#include "core/comp_geometry.h"
#include "domain/text_object.h"

namespace compositor {

/*
 * 像素资产模型（对齐上游 DocumentHistory.swift 的
 * "Value snapshots share immutable CGImages; no pixel copies for layer edits."）：
 *
 * ImageBuffer / MaskBuffer 的像素数据由 shared_ptr 持有。拷贝缓冲（文档快照、
 * 历史记录、Layer 复制）只增引用，不复制像素；写入走 writable_pixels()，
 * 引用计数 > 1（被历史或其它缓冲共享）时先复制（copy-on-write），
 * 保证任何写入不会污染共享方。
 *
 * 读路径一律走 row()（const 语义，绝不触发复制）；
 * 写路径必须显式调 writable_row() / writable_pixels()。
 */
struct ImageBuffer {
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  /* 懒创建：nullptr 等价于空缓冲；见文件头「像素资产模型」。 */
  std::shared_ptr<std::vector<uint8_t>> pixels;

  /* 只读视图：惰性空缓冲，绝不触发 COW。 */
  const std::vector<uint8_t>& pixel_data() const {
    static const std::vector<uint8_t> kEmpty;
    return pixels != nullptr ? *pixels : kEmpty;
  }

  /* 可写视图：被共享时先复制（COW）。拿裸指针写入前必须经此函数。 */
  std::vector<uint8_t>& writable_pixels() {
    if (pixels == nullptr) {
      pixels = std::make_shared<std::vector<uint8_t>>();
    } else if (pixels.use_count() > 1) {
      pixels = std::make_shared<std::vector<uint8_t>>(*pixels);
    }
    return *pixels;
  }

  /* 格式是否被引擎认识（bytes_per_pixel > 0）。合法像素宽，未知枚举一律拒绝。 */
  static bool valid_format(comp_pixel_format fmt) { return bytes_per_pixel(fmt) > 0; }

  size_t stride() const {
    return static_cast<size_t>(width) * static_cast<size_t>(bytes_per_pixel(format));
  }
  bool empty() const {
    /*
     * 未知像素格式一律视为空：否则 stride() 会是 0，调用方按 format 算出的
     * 步长与真实行宽不符，越界写堆。宁可"没有像素"也不要错误的像素。
     */
    if (!valid_format(format)) return true;
    return width <= 0 || height <= 0 || pixels == nullptr || pixels->empty();
  }
  size_t byte_size() const { return stride() * static_cast<size_t>(height); }

  void resize(int32_t w, int32_t h, comp_pixel_format fmt);
  void fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
  void clear_transparent() { fill(0, 0, 0, 0); }

  static ImageBuffer solid(int32_t w, int32_t h, comp_pixel_format fmt,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  /* 只读行指针（const 语义；非 const 对象调用也绝不触发复制）。 */
  const uint8_t* row(int32_t y) const {
    return pixel_data().data() + static_cast<size_t>(y) * stride();
  }
  /* 可写行指针：触发 COW。仅写入路径使用。 */
  uint8_t* writable_row(int32_t y) {
    return writable_pixels().data() + static_cast<size_t>(y) * stride();
  }
};

/*
 * 16 位资产的 8 位工作副本（上游 DeepLayer.dithered 同款内核）。
 *
 * 为什么需要它：合成、缩略图、裁剪基底 alpha、8 位像素工具全部按
 * 「RGBA8 预乘、4 字节/像素」解释像素缓冲。RGBA16 走同一条路会按
 * 错误的步长取值——不报错，画面直接是垃圾（AUDIT3 P2-13 的原观察）。
 * 上游的解法不是拒绝 16 位资产，而是**为每个 16 位图层维护一份 8 位副本**：
 * 「its `image` is a dithered 8-bit copy that the canvas, thumbnails and every
 * tool keep using unchanged」（DeepLayers.swift:4），本函数就是那份副本的
 * 唯一生产者，保证「哪里读像素」都得到同一个 8 位视图。
 *
 * 口径：src 必须是 COMP_PIXEL_RGBA16（预乘、native-endian、行紧排，即
 * ImageBuffer 的存储形态）。加约一阶噪声后取整，平滑梯度不出台阶；噪声按
 * (x,y,通道) 固定，同一输入恒得同一输出（可复现、可回归）。dst 会重置为
 * 同尺寸的 RGBA8。src 为空或不是 RGBA16 返回 false 且不写 dst。
 */
bool deep_to_rgba8(const ImageBuffer& src, ImageBuffer* dst);

/* ---------------------------------------------------------------------------
 * HDR 图层（COMP_PIXEL_RGBA16F）：半精度浮点、extended sRGB、预乘、native-endian，
 * 4 个 uint16 一像素——与 RGBA16 同样的步长，只是每个通道是 half 而不是 0..65535。
 *
 * 「哪里读像素」统一走 hdr_to_rgba8：它就是上游 DeepLayer.dithered 在 HDR 上的那条
 * 路径（DeepLayers.swift:176），即 `dithered(image(pixels))`——
 *   1. `image(_:)`（DeepLayers.swift:31）把半精度**重新解释回 RGBA16 布局**；
 *      white 以上的亮度在此被截到 65535（"anything brighter than white shows
 *      as white"，DeepLayers.swift:175）。
 *   2. `dithered(_:)` 在这份 16 位布局上加约一阶噪声后取整成 8 位。
 * 不能把半精度直接喂给 dither_quantize16：那会把 half 的位模式当整数，
 * 0x3C00（1.0）会变成 15360 而非 255。
 *
 * 口径：src 必须是 COMP_PIXEL_RGBA16F（预乘、native-endian、行紧排，即
 * ImageBuffer 的存储形态）。dst 会重置为同尺寸的 RGBA8。src 为空或不是
 * RGBA16F 返回 false 且不写 dst。
 */
bool hdr_to_rgba8(const ImageBuffer& src, ImageBuffer* dst);

/* 8 位工作副本回写 HDR 图层（C 方案 C2 的合并/合成消费点）。
 * 口径：src 必须 COMP_PIXEL_RGBA8、dst 必须 COMP_PIXEL_RGBA16F 且同尺寸；
 * 按分量写半精度（extended sRGB 编码、预乘），不经过 normalize/restore
 * ——上游的 8 位工具用完后图层整体降为 8 位（DeepLayers.swift:297
 * "they're dithered down, halving the layer's memory and file size"），
 * 高光因此不回填。尺寸不符或格式不对返回 false 且不写 dst。
 */
bool rgba8_to_hdr(const ImageBuffer& src, ImageBuffer* dst);

/* 图层是否带 HDR 深度（半精度而非 16 位整数）。16 位与 8 位都返回 false。 */
inline bool is_hdr_format(comp_pixel_format fmt) {
  return fmt == COMP_PIXEL_RGBA16F;
}

/* ---------------------------------------------------------------------------
 * C 方案 C3：宽位深文件（浮点解码）的入库判决——上游 `DeepLayer.wideAsset`
 * （DeepLayers.swift:164-178）那一步。
 *
 * 输入是文件解出来的**线性光**直通 alpha 浮点 RGBA（4 个 float 一像素、行紧排），
 * 也就是 EXR / Radiance HDR 的原生取值。判决与上游同口径：
 *   1. 先按 HDR 图层的布局收一遍（`deep_hdr_take_linear`：sRGB 曲线过白点延伸、
 *      乘 alpha、写 half）；
 *   2. 对结果取 `deep_half_peak`（反预乘后的最亮分量）；
 *   3. `peak > 1.001` -> **HDR 图层**（COMP_PIXEL_RGBA16F），高光留住；
 *      否则 -> **16 位图层**（COMP_PIXEL_RGBA16，`deep_half_to_u16` 取值）。
 *
 * 为什么要有 1.001 这道闸（上游原话，DeepLayers.swift:173「A thousandth over:
 * a file brushing white through rounding isn't HDR.」）：只在取整处蹭到白点的文件
 * 不是 HDR 素材，按 HDR 图层收会让它丢掉「整数 16 位」这份更细的存储，
 * 而它本来没有高光可保。
 *
 * 空/非正尺寸/空指针返回 false 且不写 out。注意 out 会被**整体重置**，
 * 包括它原有的共享像素（走 writable_pixels 的 COW 语义）。
 */
bool wide_asset_from_linear(const float* rgba, int32_t width, int32_t height, ImageBuffer* out);

/* ---------------------------------------------------------------------------
 * 锁定透明像素的还原（P1-2 第一批）：before 是写入前的像素（共享持有即可，
 * 写路径的 COW 会保住它），after 是写完的像素。语义见 layer.cpp 的实现注释：
 * 全透明处整份还原、被抹成全透明的整份还原、其余只还原 alpha 并重新预乘。
 *
 * `out_changed` 回传「最终像素与 before 是否有任何字节不同」——全部被还原时
 * 调用方应整步撤回事务（不留空撤销步）。只支持 RGBA8（16 位/HDR 各有一份
 * 需要按自身编码缩放的 alpha 口径，本批如实拒绝）。尺寸/格式不符返回 false。
 */
bool restore_transparent_lock(const ImageBuffer& before, ImageBuffer* after, bool* out_changed);

/* 8 位灰度蒙版：1 字节/像素，0 = 全透明，255 = 完全不透明。
 * 资产模型同 ImageBuffer（共享 + COW）。 */
struct MaskBuffer {
  int32_t width = 0;
  int32_t height = 0;
  std::shared_ptr<std::vector<uint8_t>> pixels;

  const std::vector<uint8_t>& pixel_data() const {
    static const std::vector<uint8_t> kEmpty;
    return pixels != nullptr ? *pixels : kEmpty;
  }

  std::vector<uint8_t>& writable_pixels() {
    if (pixels == nullptr) {
      pixels = std::make_shared<std::vector<uint8_t>>();
    } else if (pixels.use_count() > 1) {
      pixels = std::make_shared<std::vector<uint8_t>>(*pixels);
    }
    return *pixels;
  }

  size_t stride() const { return static_cast<size_t>(width); }
  bool empty() const {
    return width <= 0 || height <= 0 || pixels == nullptr || pixels->empty();
  }
  size_t byte_size() const { return stride() * static_cast<size_t>(height); }

  void resize(int32_t w, int32_t h, uint8_t fill_value = 255);
  void fill(uint8_t value);

  const uint8_t* row(int32_t y) const {
    return pixel_data().data() + static_cast<size_t>(y) * stride();
  }
  uint8_t* writable_row(int32_t y) {
    return writable_pixels().data() + static_cast<size_t>(y) * stride();
  }
};

/* 调整图层参数：kind 与 .comp 规范一致，settings 为 JSON（后向兼容扩展）。 */
struct Adjustment {
  uint32_t kind = 0;
  std::string settings_json = "{}";
};

/* 图层效果参数：effect 为效果枚举，settings 为 JSON。 */
struct Effect {
  uint32_t effect = 0;
  std::string settings_json = "{}";
};

struct Layer {
  comp_layer_id id = 0;
  std::string name;
  comp_blend_mode blend = COMP_BLEND_NORMAL;
  float opacity = 1.0f;
  comp_rect transform{0, 0, 0, 0}; /* 文档像素空间；w/h<=0 表示按像素尺寸 */
  /* 缩画布把图层整体裁到画布外时置位：此时包围盒为空矩形，不再回落到
   * 像素尺寸（否则契约里的「超出部分被裁剪」对完全离屏图层失效）。 */
  bool clipped_empty = false;
  float rotation = 0.0f;
  bool flip_x = false;
  bool flip_y = false;
  /* 采样模式：与上游 transform.sampling 对齐，缺省 High quality。 */
  comp_sampling sampling = COMP_SAMPLING_HIGH_QUALITY;
  bool is_group = false;
  /* 图层组语义：false = 穿透（默认，对齐上游：子层直接叠加到组下方内容）；
   * true = 隔离（子树先自合成一张缓冲，再作为一个单元与下层混合）。非组图层忽略。 */
  bool isolated = false;
  bool visible = true;
  /*
   * 图层锁（P1-2 第一批，位掩码，取值同 comp_layer_lock）：
   *   1 = 锁定透明像素、2 = 锁定图像像素、4 = 锁定位置、8 = 锁定全部。
   * 位 8 是「上述全部的快捷方式」，不是独立语义的一部分——判定时
   * `LOCK_ALL` 同时蕴含前三位（见 abi/layer_lock_guard）。
   *
   * 语义口径：锁是**宿主输入的闸**，不是合成参数——合成、缩略图、合并、
   * 拼合、撤销/重做、读盘一律不看锁（否则「锁住导出」会静默改画面）。
   * 闸一律放在 ABI 写入口，域层内部操作（merge/flatten/undo）不受阻。
   */
  uint32_t lock_flags = 0;
  /*
   * 颜色标签（P1-2 第一批）：0 = 无，1..7 = 红/橙/黄/绿/蓝/紫/灰（PS 同一套）。
   * 纯 UI 元数据：不进合成、不改像素；进 `.comp` 的 `engineColorLabel`
   * （缺键 = 无），撤销/重做随快照回退。
   */
  uint32_t color_label = 0;
  comp_layer_id parent = 0;
  comp_layer_id clipping_source = 0;
  /* .comp 互通：上游 live-mask 的来源图层 UUID（manifest 键 maskSourceID）。
   * 内核暂不参与合成（这是显式记录的能力边界），但读写时原样保真，
   * 避免跨端往返把该引用丢掉。 */
  std::string mask_source_uuid;
  std::vector<comp_layer_id> children; /* 仅图层组使用 */

  ImageBuffer pixels;
  MaskBuffer mask;
  std::vector<Adjustment> adjustments;
  std::vector<Effect> effects;

  /*
   * 可编辑文字对象（P0-6 ①，语义见 domain/text_object.h）。
   *
   * 有值时这一层是**文字图层**：`text` 是唯一真源，`pixels` 是它当前渲染结果的
   * 缓存（合成/缩略图/合并/GPU 纹理全走既有像素路径，合成内核不感知文字）。
   * 改文本/字号只需重渲染这一层的像素，其它图层逐字节不动。
   *
   * 不变量：
   *   - 文字图层的像素写入**只能**经 comp_layer_set_text / comp_layer_add_text；
   *     工具算子与 comp_layer_set_pixels 遇到文字图层一律 COMP_ERR_STATE，
   *     要求宿主先 rasterize（否则「改字号」会抹掉手绘像素）；
   *   - 随 DocumentState 快照一起进事务/历史，撤销天然一致；
   *   - 进 `.comp` 的 `engineText` 键（可选键，缺键 = 普通图层）。
   */
  std::optional<TextObject> text;

  /*
   * 内容修订号（S18 / R1，设计见 docs/gpu-layer-revision.md §3）。
   *
   * 标识「这一层的图形输入」（像素 / 蒙版 / 效果 / 调整 / 几何 / 混合参数）处于哪个
   * 版本。0 = 尚未分配（新建图层由 Document::create_layer 立即分配，正常运行看不到 0）。
   *
   * 不变量：
   *   - **单调**：Document::allocate_revision() 只增不减；回滚/撤销/重做不让计数器回落。
   *   - **同值 ⟺ 同内容**：一个号一旦分配就只对应那一份内容；内容再变必须换号。
   *     推论：撤销/重做把快照里的号原样恢复是**安全**的（内容确实回到了那一份）。
   *   - **不参与序列化**：不进 `.comp`、不进事务/历史的语义比较；历史快照带它只是因为
   *     快照是 DocumentState 的整份拷贝。
   *
   * 为什么不能拿 pixels 地址/哈希当缓存键：ImageBuffer 是 shared_ptr + COW，图层是唯一
   * 持有者时画笔/滤镜/液化全是**原地写**，地址不变；哈希则要每帧全量读一遍 4 MB。
   * 所以必须由写路径显式推进，见 Document::touch_layer()。
   */
  uint64_t content_revision = 0;

  /* 图层在文档像素空间的包围盒：transform 未指定时取像素尺寸 + 原点 (0,0)。
   * clipped_empty 为真时返回空矩形（缩画布把图层整个裁掉的显式标记，
   * 与「transform 未指定尺寸」区分开，否则会被上面的回落逻辑放大回来）。 */
  comp_rect effective_bounds() const;

  /*
   * 文档像素坐标 → 图层/蒙版像素坐标（最近邻）。source_width/height 是目标
   * 缓冲的尺寸（像素用 pixels.width/height，蒙版用 mask.width/height）。
   * 映射与 compositing/resample.cpp 的逆变换一致：归一化 → 逆旋转 → 翻转。
   * 落在源缓冲外返回 false。
   */
  bool document_to_pixel(int32_t source_width, int32_t source_height, double doc_x,
                         double doc_y, int32_t* out_x, int32_t* out_y) const;

  /*
   * document_to_pixel 的**连续**版：同一套映射，但按图层自身像素尺寸求值、
   * 保留小数、**不**判断是否落在缓冲内（点可以在缓冲外，坐标是仿射延拓）。
   *
   * 用途：需要亚像素几何的操作（Progressive Blur 的渐变轴端点）。取整版
   * document_to_pixel 在缩放/旋转下会把同一对端点抖动 ±1 px，渐变方向随之跳变。
   */
  bool document_to_pixel_continuous(double doc_x, double doc_y, double* out_x,
                                    double* out_y) const;

  /*
   * 上面的逆映射：图层/蒙版像素 (px, py) 的**像素中心**落在文档坐标的哪一点。
   * 同样按 归一化 → 翻转 → 旋转 → 反归一化 的次序展开，与 document_to_pixel
   * 严格互逆（那是「文档→像素」的逆旋转 + 反归一化，这里是正变换）。
   *
   * 用途：选区混合要给**每一个图层像素**取它在文档空间里的 coverage，旋转/翻转
   * 后图层像素网格与文档网格不再轴对齐，只能用正变换逐点取样；轴对齐时调用方
   * 应走整数平移快路径，别绕这里做浮点往返。
   * 落在源缓冲外返回 false。
   */
  bool pixel_to_document(int32_t source_width, int32_t source_height, int32_t px, int32_t py,
                         double* out_doc_x, double* out_doc_y) const;

  bool has_pixels() const { return !pixels.empty(); }
  /* 是否为可编辑文字图层（pixels 只是 text 的渲染缓存）。 */
  bool has_text() const { return text.has_value(); }
};

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_LAYER_H */
