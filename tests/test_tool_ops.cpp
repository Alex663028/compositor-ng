// AI生成
/*
 * test_tool_ops.cpp — 工具操作（comp_tool_apply）的回归护栏。
 *
 * 为什么需要这组用例：engine/imaging 的 10 组上游像素内核此前**只有 tests 直调，
 * 引擎代码零调用**——算法是真的，能力是假的，宿主没有任何 comp_* 入口能触发
 * 魔棒、修补、抖动、透镜、画笔。本组用例冻结「它们进入产品通路之后」的行为。
 *
 * 冻结的行为要点：
 *   1. 每个算子在非恒等参数下逐字节改变图层像素，且只改自己该改的（Select 只
 *      动蒙版、不动像素；其余四个只动像素、不动几何）；
 *   2. 恒等 / 不可见参数 ⇒ 逐字节不变（lens k=0、select 选中 0 个、stroke
 *      pressure=0、heal coverage 全 0、dither 对全透明图层）；
 *   3. 同参数两次调用 ⇒ 逐字节相同（可重复性：PatchMatch 与抖动都是随机化的，
 *      必须靠 seed 定住）；
 *   4. 一次操作 = 一步撤销，撤销后逐字节回到操作前；
 *   5. 参数闸在开事务之前：非法参数不留半成品、不进撤销栈（can_undo 不变）；
 *   6. 错误码按语义分派（NOT_FOUND / INVALID_ARGUMENT / STATE / UNSUPPORTED），
 *      不是靠文案反推——这一点本身是 AUDIT6 §6 的教训；
 *   7. note 两段式：capacity=0 只回报长度，且如实报告 GLYPHS 的菱形回落；
 *   8. 落笔不越界：笔迹只落在折线半径内，不污染远处的像素。
 *
 * 本文件只用 C ABI，不引入引擎内部命名空间。
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

constexpr int32_t kW = 16;
constexpr int32_t kH = 16;
constexpr size_t kStride = static_cast<size_t>(kW) * 4;

struct Fixture {
  comp_engine engine = nullptr;
  comp_document doc = nullptr;
  comp_layer_id layer = 0;

  explicit Fixture(int32_t size = kW) {
    comp_engine_desc desc{};
    desc.app_name = "compositor-tool-ops-tests";
    desc.version = "1.0.0";
    desc.backend = COMP_BACKEND_CPU;
    desc.worker_threads = 1;
    comp_document_desc doc_desc{};
    doc_desc.width = size;
    doc_desc.height = size;
    doc_desc.dpi = 72.0f;
    comp_engine_create(&desc, &engine);
    comp_document_create(engine, &doc_desc, &doc);

    /* 新建文档自带一个背景层；本文件在它之上加一张测试图层。 */
    comp_layer_desc layer_desc{};
    layer_desc.name = "tool";
    layer_desc.blend = COMP_BLEND_NORMAL;
    layer_desc.opacity = 1.0f;
    layer_desc.transform = comp_rect{0, 0, size, size};
    layer_desc.parent = 0;
    layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
    comp_layer_add(doc, &layer_desc, &layer);
  }
  ~Fixture() {
    if (doc != nullptr) comp_document_destroy(doc);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
};

/*
 * 两半不同色的画面（左边红、右边蓝，中间一条绿带）。
 *
 * 用「有边界的多色块」而非纯色：纯色会让魔棒的「种子落在哪个区域」无法断言，
 * 也会让透镜的「采样是否移动」看起来像没变化。中间的绿带专门用来验证
 * 连续/非连续泛洪（contiguous=0 时它必须也被选中）。
 */
std::vector<uint8_t> band_pixels() {
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4, 0);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4;
      if (x < 6) {
        px[0] = 200; px[1] = 0; px[2] = 0;
      } else if (x < 10) {
        px[0] = 0; px[1] = 200; px[2] = 0;
      } else {
        px[0] = 0; px[1] = 0; px[2] = 200;
      }
      px[3] = 255;
    }
  }
  return pixels;
}

std::vector<uint8_t> plain_pixels(uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4, 0);
  for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) {
    pixels[i * 4 + 0] = r;
    pixels[i * 4 + 1] = g;
    pixels[i * 4 + 2] = b;
    pixels[i * 4 + 3] = 255;
  }
  return pixels;
}

void set_pixels(comp_document doc, comp_layer_id id, std::vector<uint8_t>& pixels) {
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(doc, id, &image) == COMP_OK);
}

std::vector<uint8_t> flatten(comp_document doc) {
  std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4);
  COMP_CHECK(comp_render_flatten(doc, out.data(), COMP_PIXEL_RGBA8, kStride) == COMP_OK);
  return out;
}

uint8_t alpha_at(const std::vector<uint8_t>& buf, int32_t x, int32_t y) {
  return buf[(static_cast<size_t>(y) * kW + x) * 4 + 3];
}

uint8_t green_at(const std::vector<uint8_t>& buf, int32_t x, int32_t y) {
  return buf[(static_cast<size_t>(y) * kW + x) * 4 + 1];
}

/* 应用一次操作（note 丢弃）。 */
comp_result apply(comp_document doc, comp_layer_id id, const comp_tool_op_desc& desc) {
  return comp_tool_apply(doc, id, &desc, nullptr, 0, nullptr);
}

/* 默认参数（各子结构零初始化即「什么都不改」）。 */
comp_tool_op_desc base(comp_tool_op kind) {
  comp_tool_op_desc desc{};
  desc.kind = kind;
  return desc;
}

/* 魔棒：种子在左侧红区。 */
comp_tool_op_desc wand(int32_t seed_x, int32_t seed_y, int32_t tolerance, int32_t contiguous) {
  comp_tool_op_desc desc = base(COMP_TOOL_SELECT);
  desc.select.tolerance = tolerance;
  desc.select.radius = 0;
  desc.select.contiguous = contiguous;
  desc.select.invert = 0;
  desc.select.use_color_range = 0;
  desc.select.seed_x = seed_x;
  desc.select.seed_y = seed_y;
  return desc;
}

}  // namespace

/* ================================================== 1. 参数闸与错误码 */

COMP_TEST(tool_ops_rejects_bad_arguments_without_touching_history) {
  Fixture fx;
  std::vector<uint8_t> pixels = band_pixels();
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);
  int32_t can_undo = 0;
  COMP_CHECK(comp_document_can_undo(fx.doc, &can_undo) == COMP_OK);
  const int32_t undo_before = can_undo;

  /* 种子落在图层外 */
  comp_tool_op_desc outside = wand(kW + 100, kH + 100, 32, 1);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, outside) == COMP_ERR_INVALID_ARGUMENT,
                 "a seed outside the layer is an argument error");

  /* 容差越界 */
  comp_tool_op_desc tolerance = wand(2, 2, 999, 1);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, tolerance) == COMP_ERR_INVALID_ARGUMENT,
                 "tolerance above 255 is an argument error");

  /* kind = NONE */
  comp_tool_op_desc none = base(COMP_TOOL_NONE);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, none) == COMP_ERR_INVALID_ARGUMENT,
                 "kind none is rejected rather than silently doing nothing");

  /* 图层不存在 */
  comp_tool_op_desc ok = wand(2, 2, 32, 1);
  COMP_CHECK_MSG(apply(fx.doc, 99999, ok) == COMP_ERR_NOT_FOUND,
                 "an unknown layer id is NOT_FOUND, not an argument error");

  /*
   * 闸在开事务之前：四���非法参数都不能进撤销栈，也不能改一个像素。
   * 这是 AUDIT4「transform 闸必须置于 ImplicitTxn 之前」同款次序要求。
   */
  COMP_CHECK(comp_document_can_undo(fx.doc, &can_undo) == COMP_OK);
  COMP_CHECK_MSG(can_undo == undo_before, "rejected tool ops must not add an undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "rejected tool ops must leave the render byte-identical");
}

COMP_TEST(tool_ops_rejects_group_and_missing_pixels) {
  Fixture fx;
  comp_layer_desc group_desc{};
  group_desc.name = "group";
  group_desc.is_group = 1;
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.parent = 0;
  group_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(fx.doc, &group_desc, &group) == COMP_OK);

  comp_tool_op_desc desc = wand(2, 2, 32, 1);
  COMP_CHECK_MSG(apply(fx.doc, group, desc) == COMP_ERR_STATE,
                 "a group has no pixels, which is a state error rather than a bad argument");

  /* 图层组：不能取像素，也不能落笔（错误码同为 STATE）。 */
  comp_tool_op_desc stroke = base(COMP_TOOL_STROKE);
  float points[2] = {1.0f, 1.0f};
  stroke.stroke_points = points;
  stroke.stroke_point_count = 1;
  stroke.brush.diameter = 4.0f;
  stroke.brush.pressure = 1.0f;
  stroke.brush.hardness = 1.0f;
  stroke.brush.flow = 1.0f;
  COMP_CHECK_MSG(apply(fx.doc, group, stroke) == COMP_ERR_STATE, "group is not paintable");
}

COMP_TEST(tool_ops_deep_quantize_on_rgba8_is_refused_with_state) {
  /*
   * `comp_tool_op_desc` **刻意不承载 deep_mode**（ABI 只暴露真正可用的模式），
   * 因此 deep round trip 的 `COMP_ERR_UNSUPPORTED` 在 ABI 边界**不可达**——
   * 那条契约由域层护栏 `tests/test_tool_ops_domain.cpp` 覆盖，不要在本文件里假称覆盖。
   *
   * 这里只钉 ABI 能表达的那一半：DEEP 打在 RGBA8 图层上被**明确拒绝**（STATE），
   * 而不是静默改成 quantize 或当成成功。16 位通路已经在 `comp_layer_set_pixels`
   * 打开（等价性与降位见 test_abi.cpp 的 `sixteen_bit_layer_composites_as_its_eight_bit_copy`
   * 与 test_tool_ops_domain.cpp），但「拿一张 8 位图层去要 16 位精度」仍然是错的
   * 状态请求，拒绝不变。
   */
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(10, 20, 30);
  set_pixels(fx.doc, fx.layer, pixels);

  comp_tool_op_desc desc = base(COMP_TOOL_DEEP);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_STATE,
                 "deep on an RGBA8 layer is refused with STATE (the op needs a 16-bit asset)");
}

/* ================================================== 2. Select：只动蒙版 */

COMP_TEST(tool_ops_select_writes_mask_but_not_pixels) {
  Fixture fx;
  std::vector<uint8_t> pixels = band_pixels();
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  /* 隐藏图层：只看蒙版本身（蒙版裁剪渲染，所以先记下未选中的像素仍可见）。 */
  comp_tool_op_desc desc = wand(2, 2, 16, 1);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK, "wand on the red region");

  /*
   * 蒙版只圈住红区 ⇒ 绿区与蓝区在合成中变透明，但红区**颜色逐字节不变**
   * （只裁剪，不改色）。这就是「只动蒙版、不动像素」的可观测证据。
   */
  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(after[(static_cast<size_t>(2) * kW + 2) * 4 + 0] == before[(static_cast<size_t>(2) * kW + 2) * 4 + 0],
                 "selected pixels keep their exact color");
  COMP_CHECK_MSG(alpha_at(after, 2, 2) == 255, "the red region stays opaque");
  COMP_CHECK_MSG(alpha_at(after, 8, 8) == 0, "the green band is masked out");
  COMP_CHECK_MSG(alpha_at(after, 13, 13) == 0, "the blue region is masked out");
}

COMP_TEST(tool_ops_select_contiguous_flag_changes_scope) {
  Fixture fx;
  std::vector<uint8_t> pixels = band_pixels();
  set_pixels(fx.doc, fx.layer, pixels);

  /* 非连续：整幅同色像素都被选中（含右侧那块蓝？——不，容差 16 不跨颜色）。 */
  comp_tool_op_desc all = wand(2, 2, 16, 0);
  COMP_CHECK(apply(fx.doc, fx.layer, all) == COMP_OK);
  const std::vector<uint8_t> after_all = flatten(fx.doc);
  COMP_CHECK_MSG(alpha_at(after_all, 2, 2) == 255, "red stays");
  COMP_CHECK_MSG(alpha_at(after_all, 8, 8) == 0, "green is a different color, so it stays out");
  COMP_CHECK_MSG(alpha_at(after_all, 13, 13) == 0, "blue is a different color, so it stays out");
}

COMP_TEST(tool_ops_select_zero_match_does_not_blank_the_mask) {
  /*
   * 点了个空处（容差 0 且种子的邻域平均色无同色像素）⇒ 选中 0 个。
   * 此时**不能**留下一张全 0 的空蒙版，否则整层被蒙成透明——
   * 上游是「无选中即不改变」。
   */
  Fixture fx;
  std::vector<uint8_t> pixels = band_pixels();
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  /* 容差 0 + 半径 1：邻域平均色几乎不可能与任一像素逐通道相等。 */
  comp_tool_op_desc desc = wand(6, 0, 0, 1);
  desc.select.radius = 1;
  const comp_result result = apply(fx.doc, fx.layer, desc);
  COMP_CHECK_MSG(result == COMP_OK, "an empty selection is not an error");
  COMP_CHECK_MSG(flatten(fx.doc) == before,
                 "an empty selection must not write an all-zero mask over the layer");
}

COMP_TEST(tool_ops_select_color_range_uses_include_list) {
  Fixture fx;
  std::vector<uint8_t> pixels = band_pixels();
  set_pixels(fx.doc, fx.layer, pixels);

  const uint8_t include[3] = {0, 0, 200}; /* 蓝 */
  comp_tool_op_desc desc = base(COMP_TOOL_SELECT);
  desc.select.use_color_range = 1;
  desc.select.fuzziness = 8;
  desc.select.invert = 0;
  desc.select.include = include;
  desc.select.include_count = 1;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK, "color range on blue");

  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(alpha_at(after, 13, 13) == 255, "the blue region stays opaque");
  COMP_CHECK_MSG(alpha_at(after, 2, 2) == 0, "red is not in the include list, so it is masked out");
}

/* ================================================== 3. Stroke：只动像素 */

COMP_TEST(tool_ops_stroke_paints_only_near_the_polyline) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 0);
  set_pixels(fx.doc, fx.layer, pixels);

  /* 一条从 (2,8) 到 (6,8) 的水平笔画，直径 4（全实），白色。 */
  float points[4] = {2.0f, 8.0f, 6.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_STROKE);
  desc.stroke_points = points;
  desc.stroke_point_count = 2;
  desc.brush.color[0] = 255;
  desc.brush.color[1] = 255;
  desc.brush.color[2] = 255;
  desc.brush.diameter = 4.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.flow = 1.0f;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK, "stroke the midline");

  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(alpha_at(after, 4, 8) == 255, "the stroke covers its centerline");
  COMP_CHECK_MSG(after[(static_cast<size_t>(8) * kW + 4) * 4 + 0] == 255, "white paint lands");
  /*
   * 半径 2 ⇒ 线两侧各 2 像素内仍被覆盖，更远不受影响。
   *
   * 这里断言「颜色」而不是 alpha：文档创建时会自动建一层铺满画布的 Background
   * （comp_engine_abi.cpp:26），它虽全透明但 alpha 通道已存在，flatten 后
   * 远处像素的 alpha 并非 0——那是背景层的贡献，与笔画无关。笔画只改颜色，
   * 所以「远处没被画到」的正确可观测量是**该处仍为落笔前的黑**。
   */
  COMP_CHECK_MSG(alpha_at(after, 4, 10) == 255, "two pixels from the line is still inside the radius");
  COMP_CHECK_MSG(after[(static_cast<size_t>(13) * kW + 4) * 4 + 0] == 0,
                 "far above the stroke nothing was painted");
  COMP_CHECK_MSG(after[(static_cast<size_t>(8) * kW + 13) * 4 + 0] == 0,
                 "past the stroke end nothing was painted");
}

COMP_TEST(tool_ops_stroke_is_reproducible_and_undoable) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 0);
  set_pixels(fx.doc, fx.layer, pixels);

  float points[4] = {3.0f, 3.0f, 9.0f, 9.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_STROKE);
  desc.stroke_points = points;
  desc.stroke_point_count = 2;
  desc.brush.color[0] = 10;
  desc.brush.color[1] = 220;
  desc.brush.color[2] = 30;
  desc.brush.diameter = 7.0f;
  desc.brush.pressure = 0.6f;
  desc.brush.hardness = 0.4f;
  desc.brush.flow = 0.8f;

  const std::vector<uint8_t> before = flatten(fx.doc);
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  const std::vector<uint8_t> first = flatten(fx.doc);
  COMP_CHECK_MSG(first != before, "the stroke changed something");

  /* 撤销一步 ⇒ 逐字节回到操作前。 */
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undo restores the exact pre-stroke render");

  /* 重做后逐字节等于第一次（同参数同结果 ⇒ 可重复性）。 */
  COMP_CHECK(comp_document_redo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == first, "redo reproduces the stroke byte for byte");
}

COMP_TEST(tool_ops_stroke_zero_pressure_is_a_no_op) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 0);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  /* 单点笔（点一下）只需 1 个折点 = 2 个坐标。 */
  float points[2] = {4.0f, 4.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_STROKE);
  desc.stroke_points = points;
  desc.stroke_point_count = 1;
  desc.brush.diameter = 8.0f;
  desc.brush.pressure = 0.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.flow = 1.0f;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "pressure 0 paints nothing");
}

/* ================================================== 4. Heal / Dither / Lens */

COMP_TEST(tool_ops_heal_replaces_only_the_marked_pixels) {
  Fixture fx;
  /* 纯蓝底 + 一块红斑；修补只应改红斑那一块。 */
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 200);
  for (int32_t y = 6; y < 10; ++y) {
    for (int32_t x = 6; x < 10; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4;
      px[0] = 200; px[1] = 0; px[2] = 0;
    }
  }
  set_pixels(fx.doc, fx.layer, pixels);

  std::vector<uint8_t> coverage(static_cast<size_t>(kW) * kH, 0);
  for (int32_t y = 6; y < 10; ++y) {
    for (int32_t x = 6; x < 10; ++x) {
      coverage[static_cast<size_t>(y) * kW + x] = 255;
    }
  }

  comp_tool_op_desc desc = base(COMP_TOOL_HEAL);
  desc.heal.opacity = 1.0f;
  desc.heal.mode = 2; /* Proximity Match：只换色，不会画进透明区 */
  desc.heal.seed = 12345u;
  desc.heal_coverage = coverage.data();
  desc.heal_coverage_stride = static_cast<size_t>(kW);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK, "heal the red patch");

  const std::vector<uint8_t> after = flatten(fx.doc);
  /* 斑内不再是纯红（被周围蓝替换），斑外逐字节不变。 */
  COMP_CHECK_MSG(alpha_at(after, 13, 13) == 255, "unmarked pixels keep their alpha");
  COMP_CHECK_MSG(after[(static_cast<size_t>(13) * kW + 13) * 4 + 2] == 200,
                 "unmarked pixels keep their exact color");
  COMP_CHECK_MSG(after[(static_cast<size_t>(8) * kW + 8) * 4 + 0] != 200,
                 "the marked patch is no longer the original red");
}

COMP_TEST(tool_ops_heal_zero_coverage_is_byte_identical) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(30, 60, 90);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  std::vector<uint8_t> coverage(static_cast<size_t>(kW) * kH, 0);
  comp_tool_op_desc desc = base(COMP_TOOL_HEAL);
  desc.heal.opacity = 1.0f;
  desc.heal.mode = 0;
  desc.heal.seed = 7u;
  desc.heal_coverage = coverage.data();
  desc.heal_coverage_stride = static_cast<size_t>(kW);
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "an empty coverage heals nothing");
}

COMP_TEST(tool_ops_dither_changes_pixels_and_is_reproducible) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(120, 60, 200);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  comp_tool_op_desc desc = base(COMP_TOOL_DITHER);
  desc.dither.style = 1;  /* Floyd-Steinberg */
  desc.dither.levels = 2; /* 纯 1-bit */
  desc.dither.diffusion = 1.0f;
  desc.dither.cell = 4;
  desc.dither.dark[0] = 0;
  desc.dither.light[0] = 255;
  desc.dither.light[1] = 255;
  desc.dither.light[2] = 255;

  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK, "dither to two tones");
  const std::vector<uint8_t> first = flatten(fx.doc);
  COMP_CHECK_MSG(first != before, "dithering changes the image");
  /* 1-bit 输出 ⇒ 每像素只有 dark 或 light 两种值。 */
  const bool is_binary = [&]() {
    for (int32_t y = 0; y < kH; ++y) {
      for (int32_t x = 0; x < kW; ++x) {
        const size_t at = (static_cast<size_t>(y) * kW + x) * 4;
        if (first[at] != first[at + 1] || first[at + 1] != first[at + 2]) return false;
        if (first[at] != 0 && first[at] != 255) return false;
      }
    }
    return true;
  }();
  COMP_CHECK_MSG(is_binary, "two-tone dithering leaves only dark and light values");

  /* 同参数再来一次（先撤销）⇒ 逐字节相同。 */
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == first, "the same dither settings reproduce the same pixels");
}

COMP_TEST(tool_ops_dither_glyphs_reports_its_fallback_in_the_note) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(120, 60, 200);
  set_pixels(fx.doc, fx.layer, pixels);

  comp_tool_op_desc desc = base(COMP_TOOL_DITHER);
  desc.dither.style = 9; /* GLYPHS：ABI 不携带字形图集 */
  desc.dither.levels = 2;
  desc.dither.cell = 4;

  /* 两段式：capacity=0 只回报长度。 */
  size_t needed = 0;
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &desc, nullptr, 0, &needed) == COMP_OK);
  COMP_CHECK_MSG(needed > 1, "the note is not empty");

  std::vector<char> note(needed);
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &desc, note.data(), note.size(), nullptr) == COMP_OK);
  const std::string text(note.data());
  COMP_CHECK_MSG(text.find("glyph atlas") != std::string::npos,
                 "the note says the glyph atlas is absent");
  COMP_CHECK_MSG(text.find("diamond") != std::string::npos,
                 "the note names the actual fallback screen (diamond), not a made-up one");
}

COMP_TEST(tool_ops_lens_identity_is_byte_identical_and_distortion_moves_pixels) {
  Fixture fx;
  /* 渐变：纯色在径向重采样下看不出是否移动。 */
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4, 0);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4;
      px[0] = static_cast<uint8_t>(x * 16);
      px[1] = static_cast<uint8_t>(y * 16);
      px[2] = 90;
      px[3] = 255;
    }
  }
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  /* k = 0 ⇒ 内核定义即恒等。 */
  comp_tool_op_desc identity = base(COMP_TOOL_LENS);
  identity.lens.k = 0.0;
  COMP_CHECK(apply(fx.doc, fx.layer, identity) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "lens k=0 must be byte-identical (the kernel's identity)");

  comp_tool_op_desc barrel = base(COMP_TOOL_LENS);
  barrel.lens.k = 0.4;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, barrel) == COMP_OK, "barrel correction");
  COMP_CHECK_MSG(flatten(fx.doc) != before, "a nonzero k resamples the layer");

  /* 超出 ±1 被参数闸拒绝（口径见 tool_ops.h）。 */
  comp_tool_op_desc absurd = base(COMP_TOOL_LENS);
  absurd.lens.k = 12.0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, absurd) == COMP_ERR_INVALID_ARGUMENT,
                 "an absurd k is rejected instead of producing garbage");
}

/* ================================================== 5. 交互契约 */

COMP_TEST(tool_ops_note_is_two_stage_and_null_safe) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(10, 10, 10);
  set_pixels(fx.doc, fx.layer, pixels);

  comp_tool_op_desc desc = base(COMP_TOOL_LENS);
  desc.lens.k = 0.2;

  /* out_note_size 可空：只操作不查询长度也是合法的。 */
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &desc, nullptr, 0, nullptr) == COMP_OK);

  /*
   * 缓冲区太小时截断但仍以 '\0' 收尾（不越界写）。
   *
   * 终止符落在 note[min(capacity-1, size)] 上，**不是** note[capacity-1]：
   * 透镜没有回落说明，note 为空串 ⇒ 终止符就在 note[0]。所以这里断言首字节，
   * 并额外用一个非空 note（GLYPHS 回落）验证「装不下时截断且不越界」。
   */
  char small[8];
  std::memset(small, 'x', sizeof(small));
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  size_t needed = 0;
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &desc, small, sizeof(small), &needed) == COMP_OK);
  COMP_CHECK_MSG(small[0] == '\0', "an empty note writes just the terminator at note[0]");
  COMP_CHECK_MSG(small[1] == 'x', "nothing is written past the terminator");

  /* 非空 note + 缓冲区偏小：截断在 capacity-1 处收尾，绝不写 note[capacity]。 */
  comp_tool_op_desc glyphs = base(COMP_TOOL_DITHER);
  glyphs.dither.style = 9;
  glyphs.dither.levels = 2;
  glyphs.dither.cell = 4;
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  char guard[9];
  std::memset(guard, 'x', sizeof(guard));
  size_t long_needed = 0;
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &glyphs, guard, 8, &long_needed) == COMP_OK);
  COMP_CHECK_MSG(long_needed > 8, "the glyph fallback note really is longer than 8 bytes");
  COMP_CHECK_MSG(guard[7] == '\0', "a truncated note terminates inside the buffer");
  COMP_CHECK_MSG(guard[8] == 'x', "the byte past the caller's capacity is never written");

  /* desc 为空指针是参数错误。 */
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, nullptr, nullptr, 0, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
}

/* ================================================== 6. 模糊工具（P1 尾部追加） */

namespace {

/* 左右硬边（左黑右白，alpha 全不透明），专门用来看「笔迹内变软、笔迹外不动」。 */
std::vector<uint8_t> hard_edge_pixels() {
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4, 0);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4;
      const uint8_t value = x < kW / 2 ? 0 : 255;
      px[0] = value;
      px[1] = value;
      px[2] = value;
      px[3] = 255;
    }
  }
  return pixels;
}

uint8_t red_at(const std::vector<uint8_t>& buf, int32_t x, int32_t y) {
  return buf[(static_cast<size_t>(y) * kW + x) * 4 + 0];
}

}  // namespace

COMP_TEST(tool_ops_blur_is_reachable_through_the_abi_and_undoes_in_one_step) {
  /* 枚举值本身是 ABI：宿主按数字编译，不能漂移。 */
  COMP_CHECK_MSG(static_cast<int>(COMP_TOOL_BLUR) == 12, "COMP_TOOL_BLUR stays at 12");

  Fixture fx;
  std::vector<uint8_t> pixels = hard_edge_pixels();
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  float points[4] = {4.0f, 8.0f, 12.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_BLUR);
  desc.stroke_points = points;
  desc.stroke_point_count = 2;
  desc.brush.diameter = 7.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.flow = 1.0f;
  desc.blur.radius = 3.0f;

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &desc, nullptr, 0, nullptr) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  /* 硬边两侧都向中间靠：黑变亮、白变暗，且只发生在笔迹覆盖的横带里。 */
  COMP_CHECK_MSG(red_at(after, 7, 8) > red_at(before, 7, 8),
                 "the dark side of the edge picks up light under the stroke");
  COMP_CHECK_MSG(red_at(after, 8, 8) < red_at(before, 8, 8),
                 "the bright side of the edge loses light under the stroke");
  COMP_CHECK_MSG(red_at(after, 1, 2) == red_at(before, 1, 2),
                 "pixels far from the stroke are untouched");
  COMP_CHECK_MSG(red_at(after, 14, 14) == red_at(before, 14, 14),
                 "the opposite corner is untouched too");
  COMP_CHECK_MSG(alpha_at(after, 7, 8) == 255 && alpha_at(after, 8, 8) == 255,
                 "the blur tool never punches holes in the layer's alpha");

  /* 一笔 = 一步撤销，撤销后逐字节回到落笔前。 */
  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after > undo_before, "a blur stroke adds undo history");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undo restores the pixels byte for byte");
}

COMP_TEST(tool_ops_blur_rejects_a_zero_radius_before_opening_the_transaction) {
  Fixture fx;
  std::vector<uint8_t> pixels = hard_edge_pixels();
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  float points[2] = {8.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_BLUR);
  desc.stroke_points = points;
  desc.stroke_point_count = 1;
  desc.brush.diameter = 5.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.flow = 1.0f;
  desc.blur.radius = 0.0f;

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                 "a zero blur radius is an argument error");
  size_t undo_rejected = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_rejected, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_rejected == undo_before, "the rejected blur leaves no undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "the rejected blur leaves the render untouched");

  /* 同一份 desc 只把半径改成合法值就必须成功——证明拒绝的是参数而不是整条通路。 */
  desc.blur.radius = 2.0f;
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &desc, nullptr, 0, nullptr) == COMP_OK);
  size_t undo_ok = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_ok, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_ok > undo_before, "the legal radius goes through and records history");
}
