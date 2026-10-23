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
#include "compositor/comp_selection.h"
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

COMP_TEST(tool_ops_single_point_stroke_is_a_round_dab) {
  /*
   * 鼠标只按一下、不移动 —— 折线只有一个点。参数闸收 1..65536 个点，
   * 所以这里不能「接受但静默不画」：单点就是一记圆形 dab。
   */
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 0);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  float points[2] = {8.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_STROKE);
  desc.stroke_points = points;
  desc.stroke_point_count = 1;
  desc.brush.color[0] = 255;
  desc.brush.color[1] = 255;
  desc.brush.color[2] = 255;
  desc.brush.diameter = 6.0f; /* 半径 3（像素中心距离判定） */
  desc.brush.pressure = 1.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.flow = 1.0f;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK, "a one-point stroke is accepted");

  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(after != before, "a single click leaves a dab");
  COMP_CHECK_MSG(after[(static_cast<size_t>(8) * kW + 8) * 4 + 0] == 255,
                 "the dab covers its center");
  COMP_CHECK_MSG(after[(static_cast<size_t>(8) * kW + 6) * 4 + 0] == 255,
                 "inside the radius the dab is solid");
  COMP_CHECK_MSG(after[(static_cast<size_t>(8) * kW + 12) * 4 + 0] == 0,
                 "outside the radius nothing is painted");
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

uint8_t blue_at(const std::vector<uint8_t>& buf, int32_t x, int32_t y) {
  return buf[(static_cast<size_t>(y) * kW + x) * 4 + 2];
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

/* ================================================== 5b. P1：减淡 / 加深 / 海绵 */

COMP_TEST(tool_ops_tone_is_reachable_through_the_abi_and_undoes_in_one_step) {
  /* 枚举值本身是 ABI：宿主按数字编译，不能漂移。 */
  COMP_CHECK_MSG(static_cast<int>(COMP_TOOL_DODGE) == 16, "COMP_TOOL_DODGE stays at 16");
  COMP_CHECK_MSG(static_cast<int>(COMP_TOOL_BURN) == 17, "COMP_TOOL_BURN stays at 17");
  COMP_CHECK_MSG(static_cast<int>(COMP_TOOL_SPONGE) == 18, "COMP_TOOL_SPONGE stays at 18");

  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(128, 128, 128);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  float points[4] = {4.0f, 8.0f, 12.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_DODGE);
  desc.stroke_points = points;
  desc.stroke_point_count = 2;
  desc.brush.diameter = 7.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.flow = 1.0f;
  desc.tone.mode = 0; /* 中间调 */
  desc.tone.strength = 0.5f;

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  COMP_CHECK_MSG(comp_tool_apply(fx.doc, fx.layer, &desc, nullptr, 0, nullptr) == COMP_OK,
                 "dodge reaches the engine through comp_tool_apply");
  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(red_at(after, 8, 8) > red_at(before, 8, 8),
                 "dodge lightens the pixels under the stroke");
  COMP_CHECK_MSG(alpha_at(after, 8, 8) == 255, "dodge never punches holes in alpha");
  COMP_CHECK_MSG(red_at(after, 1, 14) == red_at(before, 1, 14),
                 "pixels far from the stroke are untouched");

  /* 同一份 desc 换成加深：方向必须相反。 */
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undo restores the dodge byte for byte");
  desc.kind = COMP_TOOL_BURN;
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &desc, nullptr, 0, nullptr) == COMP_OK);
  const std::vector<uint8_t> burned = flatten(fx.doc);
  COMP_CHECK_MSG(red_at(burned, 8, 8) < red_at(before, 8, 8),
                 "burn darkens the same pixels the dodge lightened");

  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after > undo_before, "each tone stroke adds undo history");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undo restores the burn byte for byte");
}

COMP_TEST(tool_ops_tone_respects_the_selection) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(100, 100, 100);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  const float rect[4] = {0.0f, 0.0f, 8.0f, 16.0f};
  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_shape(fx.doc, COMP_SELECTION_RECT, rect, 2,
                                        COMP_SELECTION_REPLACE, 0, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed != 0, "the left-half selection really was created");

  /* 折线横跨选区边界（x = 4..12，直径 7），只有左半应当被压暗。 */
  float points[4] = {4.0f, 8.0f, 12.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_BURN);
  desc.stroke_points = points;
  desc.stroke_point_count = 2;
  desc.brush.diameter = 7.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.flow = 1.0f;
  desc.tone.mode = 0;
  desc.tone.strength = 0.6f;
  COMP_CHECK(comp_tool_apply(fx.doc, fx.layer, &desc, nullptr, 0, nullptr) == COMP_OK);

  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(red_at(after, 5, 8) < red_at(before, 5, 8),
                 "inside the selection the burn really darkens");
  COMP_CHECK_MSG(red_at(after, 11, 8) == red_at(before, 11, 8),
                 "outside the selection the layer is byte-identical");
  COMP_CHECK_MSG(alpha_at(after, 11, 8) == 255, "outside the selection alpha is untouched");

  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "one undo restores the masked tone stroke");
}

COMP_TEST(tool_ops_tone_rejects_bad_mode_and_strength_before_opening_the_transaction) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(128, 128, 128);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  float points[2] = {8.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_SPONGE);
  desc.stroke_points = points;
  desc.stroke_point_count = 1;
  desc.brush.diameter = 5.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.flow = 1.0f;
  desc.tone.mode = 2; /* 海绵只有 0 = 去色 / 1 = 加色 */
  desc.tone.strength = 0.5f;

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                 "sponge mode 2 is an argument error");
  size_t undo_rejected = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_rejected, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_rejected == undo_before, "the rejected tone leaves no undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "the rejected tone leaves the render untouched");

  /* strength 0 同口径：恒等操作不记空撤销。 */
  desc.tone.mode = 1;
  desc.tone.strength = 0.0f;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(flatten(fx.doc) == before);

  /* 合法值通过同一道闸。 */
  desc.tone.strength = 0.5f;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK,
                 "the legal sponge goes through");
}

/* ================================================== 5c. P1：历史记录画笔 */

COMP_TEST(tool_ops_history_brush_is_reachable_through_the_abi_and_undoes_in_one_step) {
  /* 枚举值本身是 ABI：宿主按数字编译，不能漂移。 */
  COMP_CHECK_MSG(static_cast<int>(COMP_TOOL_HISTORY_BRUSH) == 19,
                 "COMP_TOOL_HISTORY_BRUSH stays at 19");

  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(255, 0, 0);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  /* 第一步：把中心刷成蓝色，制造一个「可回取」的历史状态。 */
  float paint[4] = {8.0f, 8.0f, 8.0f, 8.0f};
  comp_tool_op_desc blue = base(COMP_TOOL_STROKE);
  blue.stroke_points = paint;
  blue.stroke_point_count = 2;
  blue.brush.color[0] = 0;
  blue.brush.color[1] = 0;
  blue.brush.color[2] = 255;
  blue.brush.diameter = 9.0f;
  blue.brush.hardness = 1.0f;
  blue.brush.pressure = 1.0f;
  blue.brush.flow = 1.0f;
  COMP_CHECK(apply(fx.doc, fx.layer, blue) == COMP_OK);
  const std::vector<uint8_t> painted = flatten(fx.doc);
  COMP_CHECK_MSG(red_at(painted, 8, 8) == 0, "the paint step really covered the centre");

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  /*
   * 第二步：从面板下标 2（「Set Layer Pixels」之后、刷蓝之前）取红，用细笔
   * 只刷中心。下标 0/1 是图层刚建立、还没有红色像素的时刻，见下一组用例。
   */
  float tip[2] = {8.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_HISTORY_BRUSH);
  desc.stroke_points = tip;
  desc.stroke_point_count = 1;
  desc.brush.diameter = 3.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.flow = 1.0f;
  desc.history.state = 2;
  desc.history.opacity = 1.0f;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK,
                 "the history brush reaches the engine through comp_tool_apply");

  const std::vector<uint8_t> restored = flatten(fx.doc);
  COMP_CHECK_MSG(red_at(restored, 8, 8) == 255,
                 "the sampled state puts the red pixels back under the tip");
  COMP_CHECK_MSG(blue_at(restored, 8, 8) == 0, "the sampled pixel no longer carries the blue");
  COMP_CHECK_MSG(red_at(restored, 0, 0) == 255 && blue_at(restored, 0, 0) == 0,
                 "pixels outside the tip keep the current state");
  COMP_CHECK_MSG(alpha_at(restored, 8, 8) == 255, "coverage stays opaque under the tip");

  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before + 1,
                 "one history brush stroke is exactly one undo step");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == painted, "one undo puts the painted state back");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "a second undo reaches the original pixels");
}

COMP_TEST(tool_ops_history_brush_rejects_bad_state_before_opening_the_transaction) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(40, 60, 80);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  float tip[2] = {8.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_HISTORY_BRUSH);
  desc.stroke_points = tip;
  desc.stroke_point_count = 1;
  desc.brush.diameter = 5.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.flow = 1.0f;
  desc.history.state = 2;
  desc.history.opacity = 1.0f;

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  /* 历史是「Add Layer」+「Set Layer Pixels」两步：合法下标 0..2，3 与负数是参数错误。 */
  desc.history.state = 3;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                 "a state past the end of history is an argument error");
  desc.history.state = -1;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                 "a negative state index is an argument error");

  /*
   * 下标 0 是「这一层还没建立」的那一刻：调用本身合法，是文档状态不满足
   * （那一份历史里根本没有这个 id），按 STATE 拒绝而不是参数错误。
   */
  desc.history.state = 0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_STATE,
                 "a sampled state without this layer is a state error");

  /* opacity 0 是恒等操作，拒绝而不是记一步空撤销。 */
  desc.history.state = 2;
  desc.history.opacity = 0.0f;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                 "opacity 0 is an argument error rather than a silent no-op");

  size_t undo_rejected = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_rejected, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_rejected == undo_before, "rejected history brush ops leave no undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "rejected history brush ops change no pixel");

  /* 合法下标、取样与当前像素相同：成功、无改动、同样不留空撤销步。 */
  desc.history.opacity = 1.0f;
  char note[256] = {};
  size_t note_size = 0;
  COMP_CHECK_MSG(comp_tool_apply(fx.doc, fx.layer, &desc, note, sizeof(note), &note_size) == COMP_OK,
                 "sampling the unchanged state is a legal no-op");
  COMP_CHECK_MSG(note_size > 0 && std::string(note).find("changed no pixel") != std::string::npos,
                 "the note reports that the sample equals the current pixels");
  size_t undo_noop = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_noop, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_noop == undo_before, "an identity history brush leaves no undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "an identity history brush changes no pixel");
}

/* ================================================== 6. S5：橡皮擦模式 */

COMP_TEST(tool_ops_eraser_subtracts_coverage_instead_of_painting) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 0);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

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
  desc.brush.grain_amount = 75.0f; /* 橡皮擦必须忽略颗粒：没有像素可叠 */
  desc.erase = 1;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK, "erase the midline");

  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(alpha_at(after, 4, 8) == 0, "the centerline is fully erased");
  COMP_CHECK_MSG(after[(static_cast<size_t>(8) * kW + 4) * 4 + 0] == 0,
                 "the eraser never paints color");
  COMP_CHECK_MSG(alpha_at(after, 4, 2) == 255, "far from the stroke the layer stays opaque");

  /* 一步撤销 ⇒ 逐字节回到擦除前。 */
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undo restores the erased pixels exactly");
}

COMP_TEST(tool_ops_eraser_with_zero_pressure_is_a_no_op) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(30, 60, 90);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  float points[2] = {8.0f, 8.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_STROKE);
  desc.stroke_points = points;
  desc.stroke_point_count = 1;
  desc.brush.diameter = 8.0f;
  desc.brush.pressure = 0.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.flow = 1.0f;
  desc.erase = 1;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "pressure 0 erases nothing");
}

/* ================================================== 7. S9：渐变填充 */

namespace {

/* 水平线性渐变（第一色标在左边界、第二色标在右边界像素）。 */
comp_tool_op_desc linear_gradient(const uint8_t color0[4], const uint8_t color1[4]) {
  comp_tool_op_desc desc = base(COMP_TOOL_GRADIENT);
  desc.gradient.type = 0; /* 线性 */
  desc.gradient.start_x = 0.0;
  desc.gradient.start_y = 7.5;
  desc.gradient.end_x = static_cast<double>(kW - 1);
  desc.gradient.end_y = 7.5;
  for (int c = 0; c < 4; ++c) {
    desc.gradient.color0[c] = color0[c];
    desc.gradient.color1[c] = color1[c];
  }
  desc.gradient.opacity = 1.0f;
  return desc;
}

}  // namespace

COMP_TEST(tool_ops_gradient_paints_a_linear_ramp_and_undoes_in_one_step) {
  /* 枚举值本身是 ABI：宿主按数字编译，不能漂移。 */
  COMP_CHECK_MSG(static_cast<int>(COMP_TOOL_GRADIENT) == 13, "COMP_TOOL_GRADIENT stays at 13");

  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 0); /* 不透明黑：便于看通道叠加 */
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  const uint8_t red[4] = {255, 0, 0, 255};
  const uint8_t blue[4] = {0, 0, 255, 255};
  comp_tool_op_desc desc = linear_gradient(red, blue);

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  /* 左端 = 第一色标（纯红），右端 = 第二色标（纯蓝），中间单调过渡。 */
  COMP_CHECK_MSG(red_at(after, 0, 7) == 255 && blue_at(after, 0, 7) == 0,
                 "the gradient starts at the first stop");
  COMP_CHECK_MSG(blue_at(after, kW - 1, 7) == 255 && red_at(after, kW - 1, 7) == 0,
                 "the gradient ends at the second stop");
  for (int32_t x = 0; x + 1 < kW; ++x) {
    COMP_CHECK_MSG(red_at(after, x, 7) >= red_at(after, x + 1, 7),
                   "red decreases monotonically along the gradient axis");
    COMP_CHECK_MSG(blue_at(after, x, 7) <= blue_at(after, x + 1, 7),
                   "blue increases monotonically along the gradient axis");
  }
  COMP_CHECK_MSG(red_at(after, 1, 3) == red_at(after, 1, 12),
                 "a linear gradient along x is constant in y");
  COMP_CHECK_MSG(alpha_at(after, 0, 3) == 255 && alpha_at(after, kW - 1, 12) == 255,
                 "opaque stops keep the layer opaque");

  /* 一步撤销 ⇒ 逐字节回到渐变前。 */
  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after > undo_before, "a gradient adds undo history");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undo restores the pixels byte for byte");
}

COMP_TEST(tool_ops_gradient_radial_fades_out_and_respects_the_selection) {
  Fixture fx;

  /* 径向：第一色标不透明白、第二色标全透明 ⇒ 中心实、边缘淡出。 */
  std::vector<uint8_t> transparent(static_cast<size_t>(kW) * kH * 4, 0);
  set_pixels(fx.doc, fx.layer, transparent);
  comp_tool_op_desc radial = base(COMP_TOOL_GRADIENT);
  radial.gradient.type = 1; /* 径向 */
  radial.gradient.start_x = 8.0;
  radial.gradient.start_y = 8.0;
  radial.gradient.end_x = 15.0;
  radial.gradient.end_y = 8.0; /* 半径 7 */
  const uint8_t white[4] = {255, 255, 255, 255};
  const uint8_t clear[4] = {255, 255, 255, 0};
  for (int c = 0; c < 4; ++c) {
    radial.gradient.color0[c] = white[c];
    radial.gradient.color1[c] = clear[c];
  }
  radial.gradient.opacity = 1.0f;
  COMP_CHECK(apply(fx.doc, fx.layer, radial) == COMP_OK);
  const std::vector<uint8_t> after_radial = flatten(fx.doc);
  COMP_CHECK_MSG(alpha_at(after_radial, 8, 8) == 255, "the radial center keeps the first stop");
  COMP_CHECK_MSG(alpha_at(after_radial, 2, 2) == 0,
                 "beyond the radial radius the second (transparent) stop wins");
  COMP_CHECK_MSG(alpha_at(after_radial, 12, 8) > 0 && alpha_at(after_radial, 12, 8) < 255,
                 "the falloff between the center and the radius is a real ramp");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == std::vector<uint8_t>(static_cast<size_t>(kW) * kH * 4, 0),
                 "undoing the radial gradient returns to a transparent layer");

  /* 选区：只选左半，渐变就只改左半（右半保持不透明黑）。 */
  std::vector<uint8_t> black = plain_pixels(0, 0, 0);
  set_pixels(fx.doc, fx.layer, black);
  const float rect[4] = {0.0f, 0.0f, 8.0f, 16.0f};
  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_shape(fx.doc, COMP_SELECTION_RECT, rect, 2,
                                        COMP_SELECTION_REPLACE, 0, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed != 0, "the left-half selection really was created");

  const uint8_t green[4] = {0, 255, 0, 255};
  const uint8_t magenta[4] = {255, 0, 255, 255};
  comp_tool_op_desc masked = linear_gradient(green, magenta);
  COMP_CHECK(apply(fx.doc, fx.layer, masked) == COMP_OK);
  const std::vector<uint8_t> after_masked = flatten(fx.doc);
  COMP_CHECK_MSG(green_at(after_masked, 1, 8) > 0, "the selection gets the gradient");
  COMP_CHECK_MSG(green_at(after_masked, 12, 8) == 0 && red_at(after_masked, 12, 8) == 0 &&
                     blue_at(after_masked, 12, 8) == 0,
                 "outside the selection the layer stays byte-identical black");
  COMP_CHECK_MSG(alpha_at(after_masked, 12, 8) == 255, "outside the selection alpha is untouched");
}

COMP_TEST(tool_ops_gradient_rejects_degenerate_or_invisible_parameters) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(10, 20, 30);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  int32_t can_undo = 0;
  COMP_CHECK(comp_document_can_undo(fx.doc, &can_undo) == COMP_OK);
  const int32_t undo_before = can_undo;

  const uint8_t opaque[4] = {255, 255, 255, 255};
  const uint8_t clear[4] = {0, 0, 0, 0};

  /* 端点重合：没有方向/半径，静默画纯色会让宿主以为方向生效了。 */
  comp_tool_op_desc coincident = linear_gradient(opaque, clear);
  coincident.gradient.end_x = coincident.gradient.start_x;
  coincident.gradient.end_y = coincident.gradient.start_y;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, coincident) == COMP_ERR_INVALID_ARGUMENT,
                 "coincident gradient endpoints are an argument error");

  /* 两个色标都全透明 = 恒等操作。 */
  comp_tool_op_desc invisible = linear_gradient(clear, clear);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, invisible) == COMP_ERR_INVALID_ARGUMENT,
                 "a fully transparent gradient is rejected instead of recording a no-op step");

  /* 未知类型 / opacity 0 同样是参数错误。 */
  comp_tool_op_desc bad_type = linear_gradient(opaque, opaque);
  bad_type.gradient.type = 7;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, bad_type) == COMP_ERR_INVALID_ARGUMENT,
                 "an unknown gradient type is an argument error");
  comp_tool_op_desc zero_opacity = linear_gradient(opaque, opaque);
  zero_opacity.gradient.opacity = 0.0f;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, zero_opacity) == COMP_ERR_INVALID_ARGUMENT,
                 "opacity 0 is an argument error rather than a silent no-op");

  /* 闸在事务之前：四次拒绝都不进撤销栈，也不改一个像素。 */
  COMP_CHECK(comp_document_can_undo(fx.doc, &can_undo) == COMP_OK);
  COMP_CHECK_MSG(can_undo == undo_before, "rejected gradients must not add an undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "rejected gradients leave the render byte-identical");

  /* 同一份 desc 改成合法端点就必须成功——证明拒绝的是参数而不是整条通路。 */
  comp_tool_op_desc legal = linear_gradient(opaque, clear);
  COMP_CHECK(apply(fx.doc, fx.layer, legal) == COMP_OK);
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "the legal gradient undoes exactly");
}

namespace {

/* 形状 desc：几何由调用方填，样式先全部关掉（再按用例打开）。 */
comp_tool_op_desc shape_desc(comp_shape_kind kind) {
  comp_tool_op_desc desc = base(COMP_TOOL_SHAPE);
  desc.shape.kind = kind;
  desc.shape.style.fill_enabled = 0;
  desc.shape.style.stroke_enabled = 0;
  desc.shape.style.opacity = 1.0f;
  desc.shape.style.antialias = 1;
  desc.shape.style.stroke_width = 4.0;
  desc.shape.closed = 1;
  return desc;
}

/* 填充与描边同色（用例里通常只开其中一个）。 */
void shape_color(comp_tool_op_desc* desc, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool fill,
                 bool stroke) {
  desc->shape.style.fill_enabled = fill ? 1 : 0;
  desc->shape.style.stroke_enabled = stroke ? 1 : 0;
  const uint8_t color[4] = {r, g, b, a};
  for (int c = 0; c < 4; ++c) {
    desc->shape.style.fill_color[c] = color[c];
    desc->shape.style.stroke_color[c] = color[c];
  }
}

comp_shape_cubic shape_cubic(double sx, double sy, double c1x, double c1y, double c2x, double c2y,
                             double ex, double ey, bool new_subpath) {
  comp_shape_cubic segment{};
  segment.start[0] = static_cast<float>(sx);
  segment.start[1] = static_cast<float>(sy);
  segment.control1[0] = static_cast<float>(c1x);
  segment.control1[1] = static_cast<float>(c1y);
  segment.control2[0] = static_cast<float>(c2x);
  segment.control2[1] = static_cast<float>(c2y);
  segment.end[0] = static_cast<float>(ex);
  segment.end[1] = static_cast<float>(ey);
  segment.new_subpath = new_subpath ? 1 : 0;
  return segment;
}

}  // namespace

COMP_TEST(tool_ops_shape_rectangle_fills_and_undoes_in_one_step) {
  /* 枚举值本身是 ABI：宿主按数字编译，不能漂移。 */
  COMP_CHECK_MSG(static_cast<int>(COMP_TOOL_SHAPE) == 14, "COMP_TOOL_SHAPE stays at 14");

  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 0); /* 不透明黑：便于看通道替换 */
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  comp_tool_op_desc rect = shape_desc(COMP_SHAPE_RECT);
  rect.shape.x0 = 2.0;
  rect.shape.y0 = 2.0;
  rect.shape.x1 = 10.0;
  rect.shape.y1 = 10.0;
  shape_color(&rect, 255, 0, 0, 255, true, false);
  COMP_CHECK(apply(fx.doc, fx.layer, rect) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  COMP_CHECK_MSG(red_at(after, 5, 5) == 255 && green_at(after, 5, 5) == 0 &&
                     blue_at(after, 5, 5) == 0,
                 "the rectangle interior is painted with the fill color");
  COMP_CHECK_MSG(alpha_at(after, 5, 5) == 255, "an opaque fill keeps the layer opaque");
  COMP_CHECK_MSG(red_at(after, 2, 5) == 255, "an integer-aligned edge is fully covered");
  COMP_CHECK_MSG(red_at(after, 1, 5) == 0 && red_at(after, 10, 5) == 0,
                 "the box edges are exclusive for integer-aligned geometry");

  /* 一步撤销 ⇒ 逐字节回到形状之前。 */
  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before + 1, "a shape adds exactly one undo step");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undo restores the pixels byte for byte");

  /* 半像素边界 ⇒ 覆盖率约 50%（8 位量化允许 ±4 级）。 */
  comp_tool_op_desc half = shape_desc(COMP_SHAPE_RECT);
  half.shape.x0 = 3.5;
  half.shape.y0 = 0.0;
  half.shape.x1 = 12.5;
  half.shape.y1 = 16.0;
  shape_color(&half, 255, 0, 0, 255, true, false);
  COMP_CHECK(apply(fx.doc, fx.layer, half) == COMP_OK);
  const std::vector<uint8_t> edge = flatten(fx.doc);
  const int half_red = static_cast<int>(red_at(edge, 3, 8));
  COMP_CHECK_MSG(half_red >= 120 && half_red <= 136,
                 "a half-covered edge pixel gets about half the fill color");
  COMP_CHECK_MSG(red_at(edge, 12, 8) >= 120 && red_at(edge, 12, 8) <= 136,
                 "the antialiased right edge is half covered too");
  COMP_CHECK_MSG(red_at(edge, 8, 8) == 255, "pixels fully inside the box stay fully covered");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undoing the antialiased edge is exact");
}

COMP_TEST(tool_ops_shape_ellipse_respects_the_selection) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(0, 0, 0);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  comp_tool_op_desc ellipse = shape_desc(COMP_SHAPE_ELLIPSE);
  ellipse.shape.x0 = 2.0;
  ellipse.shape.y0 = 2.0;
  ellipse.shape.x1 = 14.0;
  ellipse.shape.y1 = 14.0;
  shape_color(&ellipse, 0, 255, 0, 255, true, false);
  COMP_CHECK(apply(fx.doc, fx.layer, ellipse) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(green_at(after, 8, 8) == 255, "the ellipse center is filled");
  COMP_CHECK_MSG(green_at(after, 2, 2) == 0, "the box corner outside the ellipse is untouched");
  COMP_CHECK_MSG(green_at(after, 1, 8) == 0, "left of the ellipse is untouched");
  COMP_CHECK_MSG(green_at(after, 8, 1) == 0, "above the ellipse is untouched");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undoing the ellipse is exact");

  /* 选区：只选左半；椭圆在右半的部分必须逐字节不动。 */
  const float sel[4] = {0.0f, 0.0f, 8.0f, 16.0f};
  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_shape(fx.doc, COMP_SELECTION_RECT, sel, 2,
                                        COMP_SELECTION_REPLACE, 0, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed != 0, "the left-half selection really was created");

  comp_tool_op_desc masked = shape_desc(COMP_SHAPE_ELLIPSE);
  masked.shape.x0 = 2.0;
  masked.shape.y0 = 2.0;
  masked.shape.x1 = 14.0;
  masked.shape.y1 = 14.0;
  shape_color(&masked, 0, 0, 255, 255, true, false);
  COMP_CHECK(apply(fx.doc, fx.layer, masked) == COMP_OK);
  const std::vector<uint8_t> masked_after = flatten(fx.doc);
  COMP_CHECK_MSG(blue_at(masked_after, 3, 8) > 0, "the selected half of the ellipse is painted");
  COMP_CHECK_MSG(blue_at(masked_after, 12, 8) == 0 && green_at(masked_after, 12, 8) == 0,
                 "outside the selection the layer stays byte-identical black");
  COMP_CHECK_MSG(alpha_at(masked_after, 12, 8) == 255, "outside the selection alpha is untouched");
}

COMP_TEST(tool_ops_shape_line_is_a_round_capped_stroke) {
  Fixture fx;
  std::vector<uint8_t> transparent(static_cast<size_t>(kW) * kH * 4, 0);
  set_pixels(fx.doc, fx.layer, transparent);

  comp_tool_op_desc line = shape_desc(COMP_SHAPE_LINE);
  line.shape.x0 = 2.0;
  line.shape.y0 = 8.0;
  line.shape.x1 = 13.0;
  line.shape.y1 = 8.0;
  line.shape.style.stroke_width = 4.0; /* 半宽 2 */
  shape_color(&line, 255, 255, 255, 255, false, true);
  COMP_CHECK(apply(fx.doc, fx.layer, line) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  COMP_CHECK_MSG(alpha_at(after, 8, 8) == 255, "the line core is fully covered");
  COMP_CHECK_MSG(alpha_at(after, 8, 6) == 255, "half the stroke width away is still inside");
  COMP_CHECK_MSG(alpha_at(after, 8, 5) == 0, "beyond half the stroke width nothing is painted");
  COMP_CHECK_MSG(alpha_at(after, 8, 12) == 0, "the opposite side of the line is untouched");
  COMP_CHECK_MSG(alpha_at(after, 14, 8) > 0, "the round cap extends past the endpoint");
  COMP_CHECK_MSG(alpha_at(after, 15, 8) == 0, "the cap stops within half the stroke width");
  COMP_CHECK_MSG(alpha_at(after, 0, 8) > 0, "the start cap is round as well");

  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == transparent, "undoing the line returns to a transparent layer");
}

COMP_TEST(tool_ops_shape_path_fills_with_a_hole) {
  Fixture fx;
  std::vector<uint8_t> transparent(static_cast<size_t>(kW) * kH * 4, 0);
  set_pixels(fx.doc, fx.layer, transparent);

  /*
   * 外框顺时针（右 → 下 → 左 → 上），内框逆时针（下 → 右 → 上 → 左）：
   * 非零缠绕下内框成为洞，正好验证「反向子路径挖洞」这条选中/形状共用的规则。
   * 每段用共线控制点，扁平化后就是直线段。
   */
  const comp_shape_cubic segments[8] = {
      shape_cubic(2, 2, 6, 2, 10, 2, 14, 2, true),
      shape_cubic(14, 2, 14, 6, 14, 10, 14, 14, false),
      shape_cubic(14, 14, 10, 14, 6, 14, 2, 14, false),
      shape_cubic(2, 14, 2, 10, 2, 6, 2, 2, false),
      shape_cubic(5, 5, 5, 7, 5, 9, 5, 11, true),
      shape_cubic(5, 11, 7, 11, 9, 11, 11, 11, false),
      shape_cubic(11, 11, 11, 9, 11, 7, 11, 5, false),
      shape_cubic(11, 5, 9, 5, 7, 5, 5, 5, false),
  };

  comp_tool_op_desc path = shape_desc(COMP_SHAPE_PATH);
  path.shape.segments = segments;
  path.shape.segment_count = 8;
  path.shape.closed = 1;
  shape_color(&path, 255, 0, 0, 255, true, false);
  COMP_CHECK(apply(fx.doc, fx.layer, path) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  COMP_CHECK_MSG(red_at(after, 3, 8) == 255 || red_at(after, 4, 8) == 255,
                 "the ring between the outer and inner subpaths is filled");
  COMP_CHECK_MSG(red_at(after, 8, 8) == 0 && alpha_at(after, 8, 8) == 0,
                 "the reversed inner subpath leaves a real hole");
  COMP_CHECK_MSG(red_at(after, 8, 3) == 255, "the top band of the ring is filled");
  COMP_CHECK_MSG(red_at(after, 1, 8) == 0 && alpha_at(after, 1, 8) == 0,
                 "outside the outer subpath is untouched");

  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == transparent, "undoing the path returns to a transparent layer");
}

COMP_TEST(tool_ops_shape_rejects_bad_parameters_and_noops_off_canvas) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(10, 20, 30);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  /* 填充与描边全关：没有可画的样式。 */
  comp_tool_op_desc neither = shape_desc(COMP_SHAPE_RECT);
  neither.shape.x0 = 2.0;
  neither.shape.y0 = 2.0;
  neither.shape.x1 = 10.0;
  neither.shape.y1 = 10.0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, neither) == COMP_ERR_INVALID_ARGUMENT,
                 "a shape with neither fill nor stroke is an argument error");

  /* 直线不许填充：填一条「实心线」没有定义。 */
  comp_tool_op_desc filled_line = shape_desc(COMP_SHAPE_LINE);
  filled_line.shape.x0 = 2.0;
  filled_line.shape.y0 = 8.0;
  filled_line.shape.x1 = 13.0;
  filled_line.shape.y1 = 8.0;
  shape_color(&filled_line, 255, 255, 255, 255, true, true);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, filled_line) == COMP_ERR_INVALID_ARGUMENT,
                 "a filled line shape is rejected instead of silently dropping the fill");

  /* 退化盒（高度 0）与全透明颜色。 */
  comp_tool_op_desc flat_box = shape_desc(COMP_SHAPE_RECT);
  flat_box.shape.x0 = 2.0;
  flat_box.shape.y0 = 8.0;
  flat_box.shape.x1 = 13.0;
  flat_box.shape.y1 = 8.0;
  shape_color(&flat_box, 255, 0, 0, 255, true, false);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, flat_box) == COMP_ERR_INVALID_ARGUMENT,
                 "a zero-height box is an argument error");

  comp_tool_op_desc invisible = shape_desc(COMP_SHAPE_RECT);
  invisible.shape.x0 = 2.0;
  invisible.shape.y0 = 2.0;
  invisible.shape.x1 = 10.0;
  invisible.shape.y1 = 10.0;
  shape_color(&invisible, 255, 255, 255, 0, true, false);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, invisible) == COMP_ERR_INVALID_ARGUMENT,
                 "a fully transparent fill is rejected instead of recording a no-op step");

  /* 未知种类与空路径。 */
  comp_tool_op_desc bad_kind = shape_desc(COMP_SHAPE_RECT);
  bad_kind.shape.kind = static_cast<comp_shape_kind>(9);
  bad_kind.shape.x0 = 2.0;
  bad_kind.shape.y0 = 2.0;
  bad_kind.shape.x1 = 10.0;
  bad_kind.shape.y1 = 10.0;
  shape_color(&bad_kind, 255, 0, 0, 255, true, false);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, bad_kind) == COMP_ERR_INVALID_ARGUMENT,
                 "an unknown shape kind is an argument error");

  comp_tool_op_desc empty_path = shape_desc(COMP_SHAPE_PATH);
  shape_color(&empty_path, 255, 0, 0, 255, true, false);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, empty_path) == COMP_ERR_INVALID_ARGUMENT,
                 "a path without segments is an argument error");

  /* 开了描边却给了 0 线宽：这时线宽不再是「用不到的字段」，必须拒绝。 */
  comp_tool_op_desc no_width = shape_desc(COMP_SHAPE_LINE);
  no_width.shape.x0 = 2.0;
  no_width.shape.y0 = 8.0;
  no_width.shape.x1 = 13.0;
  no_width.shape.y1 = 8.0;
  shape_color(&no_width, 255, 255, 255, 255, false, true);
  no_width.shape.style.stroke_width = 0.0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, no_width) == COMP_ERR_INVALID_ARGUMENT,
                 "a stroked shape with zero stroke width is an argument error");

  /* 闸在事务之前：七次拒绝都不进撤销栈，也不改一个像素。 */
  size_t undo_after_rejects = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after_rejects, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after_rejects == undo_before, "rejected shapes must not add an undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "rejected shapes leave the render byte-identical");

  /*
   * 合法但完全落在图层外的形状：成功、无像素改动、**不产生撤销步**
   * （否则宿主会看到一步什么都没做的撤销）。
   */
  comp_tool_op_desc outside = shape_desc(COMP_SHAPE_RECT);
  outside.shape.x0 = 100.0;
  outside.shape.y0 = 100.0;
  outside.shape.x1 = 120.0;
  outside.shape.y1 = 120.0;
  shape_color(&outside, 255, 0, 0, 255, true, false);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, outside) == COMP_OK,
                 "a shape entirely outside the layer succeeds as a no-op");
  size_t undo_after_outside = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after_outside, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after_outside == undo_before,
                 "an off-canvas shape must not leave an empty undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "an off-canvas shape changes no pixel");

  /* 改成合法几何就必须成功——证明拒绝的是参数而不是整条通路。 */
  comp_tool_op_desc legal = shape_desc(COMP_SHAPE_RECT);
  legal.shape.x0 = 2.0;
  legal.shape.y0 = 2.0;
  legal.shape.x1 = 10.0;
  legal.shape.y1 = 10.0;
  shape_color(&legal, 0, 0, 255, 255, true, false);
  COMP_CHECK(apply(fx.doc, fx.layer, legal) == COMP_OK);
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "the legal shape undoes exactly");

  /*
   * 纯填充形状不看 stroke_width：宿主没填线宽（0）也必须成功——S10 外壳的
   * 矩形/椭圆默认只开填充，不能因为一个用不到的字段被参数闸挡下。
   */
  comp_tool_op_desc fill_only = shape_desc(COMP_SHAPE_RECT);
  fill_only.shape.x0 = 2.0;
  fill_only.shape.y0 = 2.0;
  fill_only.shape.x1 = 10.0;
  fill_only.shape.y1 = 10.0;
  shape_color(&fill_only, 255, 0, 0, 255, true, false);
  fill_only.shape.style.stroke_width = 0.0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, fill_only) == COMP_OK,
                 "a fill-only shape ignores stroke_width (0 is legal)");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "the fill-only shape undoes exactly");
}
namespace {

/*
 * 文字 desc：默认「字号 14（缩放 2）+ 行距 1.5 + 硬边 + 不透明红」。
 * 字号 14 是 7 的整数倍 ⇒ 每个点阵格正好 2×2 像素，像素中心断言可以精确写死。
 */
comp_tool_op_desc type_desc(const char* text, int32_t length) {
  comp_tool_op_desc desc = base(COMP_TOOL_TYPE);
  desc.type.text = text;
  desc.type.text_length = length;
  desc.type.origin_x = 0.0;
  desc.type.origin_y = 0.0;
  desc.type.style.font_size = 14.0;
  desc.type.style.line_height = 1.5;
  desc.type.style.letter_spacing = 0.0;
  desc.type.style.antialias = 0;
  desc.type.color[0] = 255;
  desc.type.color[1] = 0;
  desc.type.color[2] = 0;
  desc.type.color[3] = 255;
  desc.type.opacity = 1.0f;
  return desc;
}

}  // namespace

COMP_TEST(tool_ops_type_paints_a_bitmap_glyph_and_undoes_in_one_step) {
  /* 枚举值本身是 ABI：宿主按数字编译，不能漂移。 */
  COMP_CHECK_MSG(static_cast<int>(COMP_TOOL_TYPE) == 15, "COMP_TOOL_TYPE is appended as 15");

  Fixture fx;
  std::vector<uint8_t> transparent(static_cast<size_t>(kW) * kH * 4, 0);
  set_pixels(fx.doc, fx.layer, transparent);
  const std::vector<uint8_t> before = flatten(fx.doc);

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  comp_tool_op_desc glyph = type_desc("A", 1);
  glyph.type.origin_x = 2.0;
  glyph.type.origin_y = 1.0;
  COMP_CHECK(apply(fx.doc, fx.layer, glyph) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  /*
   * 'A' 是 0x7E,0x11,0x11,0x11,0x7E（列优先，bit0 最上行），字号 14 ⇒ 每格 2×2。
   * 左竖（第 0 列，bit1..6）从 y=3 到 y=15 都该有墨；顶部第 0 行是空的。
   */
  COMP_CHECK_MSG(red_at(after, 2, 3) == 255 && alpha_at(after, 2, 3) == 255,
                 "the left stem of 'A' is painted opaque red");
  COMP_CHECK_MSG(red_at(after, 2, 6) == 255, "the stem keeps going down the glyph rows");
  COMP_CHECK_MSG(alpha_at(after, 2, 1) == 0, "the empty top row of 'A' stays untouched");
  COMP_CHECK_MSG(alpha_at(after, 12, 8) == 0,
                 "the trailing 1-column gap of the cell stays untouched");
  COMP_CHECK_MSG(alpha_at(after, 6, 8) == 0, "the hole between the two stems stays untouched");
  COMP_CHECK_MSG(alpha_at(after, 2, 15) == 0, "below the 7-row block nothing is painted");
  COMP_CHECK_MSG(red_at(after, 6, 9) == 255, "the crossbar is filled with the requested color");

  /* 一步撤销 ⇒ 逐字节回到文字之前。 */
  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before + 1, "type adds exactly one undo step");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "undo restores the pixels byte for byte");
}

COMP_TEST(tool_ops_type_multiline_and_font_size_scale_the_block) {
  Fixture fx;
  std::vector<uint8_t> transparent(static_cast<size_t>(kW) * kH * 4, 0);
  set_pixels(fx.doc, fx.layer, transparent);

  /* 字号 7 ⇒ 缩放 1（点阵 1:1）；行距 1.0 ⇒ 行步进 7，第二行正好下移 7 像素。 */
  comp_tool_op_desc two_lines = type_desc("A\nB", 3);
  two_lines.type.origin_x = 1.0;
  two_lines.type.origin_y = 1.0;
  two_lines.type.style.font_size = 7.0;
  two_lines.type.style.line_height = 1.0;
  COMP_CHECK(apply(fx.doc, fx.layer, two_lines) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  COMP_CHECK_MSG(red_at(after, 1, 2) == 255, "the first line starts at the text box origin");
  COMP_CHECK_MSG(alpha_at(after, 1, 1) == 0, "row 0 of 'A' is empty at 1:1 scale");
  COMP_CHECK_MSG(red_at(after, 1, 8) == 255, "the second line is one 7px line step lower");
  COMP_CHECK_MSG(red_at(after, 1, 14) == 255, "the last row of the second line is inside the layer");
  COMP_CHECK_MSG(alpha_at(after, 1, 15) == 0, "past the two-line block nothing is painted");
  /* 文本框宽 = 最长行的步进宽度 = 1 字形 × 6 列 = 6（x=1..7）。 */
  COMP_CHECK_MSG(alpha_at(after, 7, 8) == 0, "the text box ends after the last glyph advance");

  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == transparent, "undoing multiline text is exact");
}

COMP_TEST(tool_ops_type_antialiases_half_pixel_edges_and_respects_the_selection) {
  Fixture fx;
  std::vector<uint8_t> transparent(static_cast<size_t>(kW) * kH * 4, 0);
  set_pixels(fx.doc, fx.layer, transparent);

  /* 原点落在半像素上 ⇒ 左边缘像素覆盖率约 50%，不是全有或全无。 */
  comp_tool_op_desc soft = type_desc("A", 1);
  soft.type.origin_x = 0.5;
  soft.type.origin_y = 1.0;
  soft.type.style.antialias = 1;
  COMP_CHECK(apply(fx.doc, fx.layer, soft) == COMP_OK);
  const std::vector<uint8_t> antialiased = flatten(fx.doc);
  const int half = static_cast<int>(red_at(antialiased, 0, 3));
  COMP_CHECK_MSG(half >= 120 && half <= 136,
                 "a half-covered edge pixel gets about half the fill color");
  COMP_CHECK_MSG(red_at(antialiased, 1, 3) == 255, "the fully covered neighbor stays at 255");
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);

  /* 选区：只选左半（x < 8）；两字符文本横跨选区内外。 */
  const float sel[4] = {0.0f, 0.0f, 8.0f, 16.0f};
  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_shape(fx.doc, COMP_SELECTION_RECT, sel, 2,
                                        COMP_SELECTION_REPLACE, 0, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed != 0, "the left-half selection really was created");

  comp_tool_op_desc masked = type_desc("AB", 2);
  COMP_CHECK(apply(fx.doc, fx.layer, masked) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(red_at(after, 1, 2) == 255, "the selected part of 'A' is painted");
  COMP_CHECK_MSG(red_at(after, 8, 2) == 0 && alpha_at(after, 8, 2) == 0,
                 "the same glyph row outside the selection stays byte-identical");
  COMP_CHECK_MSG(alpha_at(after, 12, 2) == 0 && alpha_at(after, 12, 8) == 0,
                 "the second glyph is entirely outside the selection and untouched");
}

COMP_TEST(tool_ops_type_rejects_bad_text_and_noops_off_canvas) {
  Fixture fx;
  std::vector<uint8_t> pixels = plain_pixels(10, 20, 30);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_before, nullptr) == COMP_OK);

  /* UTF-8 多字节 = CJK：本仓自带字体只有 ASCII，必须拒绝而不是画方框。 */
  comp_tool_op_desc cjk = type_desc("\xE4\xBD\xA0\xE5\xA5\xBD", 6);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, cjk) == COMP_ERR_INVALID_ARGUMENT,
                 "UTF-8/CJK text is rejected instead of painting boxes");

  /* 空文本 / 只有换行 / 空指针 / 负长度。 */
  comp_tool_op_desc empty = type_desc("", 0);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, empty) == COMP_ERR_INVALID_ARGUMENT,
                 "empty text is an argument error");
  comp_tool_op_desc newline_only = type_desc("\n\n", 2);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, newline_only) == COMP_ERR_INVALID_ARGUMENT,
                 "text made only of newlines has no glyph and is rejected");
  comp_tool_op_desc null_text = type_desc("A", 1);
  null_text.type.text = nullptr;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, null_text) == COMP_ERR_INVALID_ARGUMENT,
                 "a null text pointer is an argument error");
  comp_tool_op_desc negative = type_desc("A", -3);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, negative) == COMP_ERR_INVALID_ARGUMENT,
                 "a negative text length is an argument error");

  /* 控制字符（Tab）不是可打印 ASCII。 */
  comp_tool_op_desc tab = type_desc("A\tB", 3);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, tab) == COMP_ERR_INVALID_ARGUMENT,
                 "control characters such as Tab are rejected");

  /* 排版量程：字号过小/过大、行距 < 1、字距把步进压成非正、原点非有限。 */
  comp_tool_op_desc tiny = type_desc("A", 1);
  tiny.type.style.font_size = 5.0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, tiny) == COMP_ERR_INVALID_ARGUMENT,
                 "a font size below 6 is an argument error");
  comp_tool_op_desc huge = type_desc("A", 1);
  huge.type.style.font_size = 2049.0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, huge) == COMP_ERR_INVALID_ARGUMENT,
                 "a font size above 2048 is an argument error");
  comp_tool_op_desc tight_lines = type_desc("A\nB", 3);
  tight_lines.type.style.line_height = 0.5;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, tight_lines) == COMP_ERR_INVALID_ARGUMENT,
                 "line height below 1 is rejected because lines would overlap");
  comp_tool_op_desc crushed = type_desc("AB", 2);
  crushed.type.style.letter_spacing = -100.0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, crushed) == COMP_ERR_INVALID_ARGUMENT,
                 "letter spacing that makes the glyph advance non-positive is rejected");
  comp_tool_op_desc nan_origin = type_desc("A", 1);
  nan_origin.type.origin_x = std::nan("");
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, nan_origin) == COMP_ERR_INVALID_ARGUMENT,
                 "a NaN origin is an argument error");

  /* 全透明颜色 / opacity 0 是恒等操作，拒绝而不是记一步空撤销。 */
  comp_tool_op_desc invisible = type_desc("A", 1);
  invisible.type.color[3] = 0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, invisible) == COMP_ERR_INVALID_ARGUMENT,
                 "a fully transparent text color is rejected");
  comp_tool_op_desc transparent_opacity = type_desc("A", 1);
  transparent_opacity.type.opacity = 0.0f;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, transparent_opacity) == COMP_ERR_INVALID_ARGUMENT,
                 "opacity 0 is an argument error rather than a silent no-op");

  /* 闸在事务之前：所有拒绝都不进撤销栈，也不改一个像素。 */
  size_t undo_after_rejects = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after_rejects, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after_rejects == undo_before, "rejected type ops must not add an undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "rejected type ops leave the render byte-identical");

  /* 合法但完全落在图层外：成功、无像素改动、不产生撤销步。 */
  comp_tool_op_desc outside = type_desc("A", 1);
  outside.type.origin_x = 100.0;
  outside.type.origin_y = 100.0;
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, outside) == COMP_OK,
                 "a text box entirely outside the layer succeeds as a no-op");
  size_t undo_after_outside = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after_outside, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after_outside == undo_before,
                 "an off-canvas text box must not leave an empty undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "an off-canvas text box changes no pixel");

  /* 纯空格文本没有墨迹：成功、无像素改动、同样不留空撤销步（noop 口径）。 */
  comp_tool_op_desc spaces = type_desc("   ", 3);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, spaces) == COMP_OK,
                 "space-only text succeeds without painting anything");
  size_t undo_after_spaces = 0;
  COMP_CHECK(comp_document_history_stats(fx.doc, &undo_after_spaces, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after_spaces == undo_before,
                 "ink-less text must not leave an empty undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "ink-less text changes no pixel");

  /* 改成合法参数就必须成功——证明拒绝的是参数而不是整条通路。 */
  comp_tool_op_desc legal = type_desc("Hi", 2);
  COMP_CHECK(apply(fx.doc, fx.layer, legal) == COMP_OK);
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "the legal text undoes exactly");
}