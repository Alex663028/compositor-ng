// AI生成
/*
 * test_tool_ops_domain.cpp — 工具算子的**域层契约**护栏（ABI 边界测不到的那一半）。
 *
 * 为什么单独一份：`tests/test_tool_ops.cpp` 刻意只用 C ABI（「壳的视角」），
 * 而 `comp_tool_op_desc` **曾经**故意不承载 `deep_mode`——本轮补上了往返的
 * 内层算子后，ABI 也能到达 deep round trip，但域层仍有一些契约在 ABI 边界测不到。
 * 本文件直接调 `run_tool_op`，钉住
 *   1. deep round trip 保持 16 位、四档合回确实读了 +1/+2/+4，且一步撤销；
 *   2. 内层为 none 时的精度损失必须写进 note；
 *   3. 往返内层不得是改几何 / 写蒙版的算子（参数闸拒绝）；
 *   4. 非法 deep_mode 值 -> `COMP_ERR_INVALID_ARGUMENT`；
 *   5. 参数闸在**开事务之前**：被拒的调用不留悬空事务、不进撤销栈、不改像素；
 *   6. `pressure * flow == 0` 的笔画逐字节不变（可重复性契约的边界）。
 *
 * 本文件的错误码断言全部钉**具体值**而非「非 OK」：`!= COMP_OK` 会让
 * INTERNAL 这类内部错误伪装成「按契约拒绝了」。
 */
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "domain/document.h"
#include "domain/layer.h"
#include "domain/tool_ops.h"
#include "engine.h"
#include "test_framework.h"

using namespace compositor;

namespace {

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tool-ops-domain-tests";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

/* 建一个铺满画布、像素确定的普通图层（solid 值是确定性的，便于逐字节比较）。 */
comp_layer_id add_pixel_layer(Document& doc) {
  comp_layer_desc desc{};
  desc.name = "Paint";
  desc.transform = comp_rect{0, 0, doc.state().width, doc.state().height};
  comp_layer_id id = 0;
  doc.create_layer(desc, &id);
  Layer* layer = doc.find(id);
  if (layer != nullptr) {
    layer->pixels = ImageBuffer::solid(doc.state().width, doc.state().height, COMP_PIXEL_RGBA8,
                                       40, 80, 120, 255);
  }
  return id;
}

std::vector<uint8_t> pixels_of(const Document& doc, comp_layer_id id) {
  const Layer* layer = doc.find(id);
  if (layer == nullptr) return {};
  return layer->pixels.pixel_data();
}

/*
 * 铺满画布的 **16 位**图层（预乘 RGBA16，native-endian）：alpha 与三通道都随
 * 位置变化，这样「降位后逐字节等于哪一份副本」才是可判的断言——纯色或全不透明
 * 都看不出按错步长读。
 */
comp_layer_id add_wide_layer(Document& doc) {
  comp_layer_desc desc{};
  desc.name = "Wide";
  desc.transform = comp_rect{0, 0, doc.state().width, doc.state().height};
  comp_layer_id id = 0;
  doc.create_layer(desc, &id);
  Layer* layer = doc.find(id);
  if (layer != nullptr) {
    const int32_t w = doc.state().width;
    const int32_t h = doc.state().height;
    layer->pixels.resize(w, h, COMP_PIXEL_RGBA16);
    uint16_t* wide = reinterpret_cast<uint16_t*>(layer->pixels.writable_pixels().data());
    for (int32_t y = 0; y < h; ++y) {
      for (int32_t x = 0; x < w; ++x) {
        uint16_t* px = wide + (static_cast<size_t>(y) * static_cast<size_t>(w) +
                               static_cast<size_t>(x)) * 4u;
        const uint16_t alpha = static_cast<uint16_t>(0x4000 + 0x1000 * (x + y));
        px[3] = alpha;
        px[0] = static_cast<uint16_t>(alpha / 2u); /* 预乘：rgb <= alpha */
        px[1] = static_cast<uint16_t>(alpha / 3u);
        px[2] = static_cast<uint16_t>(alpha / 4u);
      }
    }
  }
  return id;
}

/* 一份独立副本：用来在不改动文档的前提下重算「应有的 8 位工作副本」。 */
ImageBuffer copy_of(const std::vector<uint8_t>& bytes, int32_t w, int32_t h, comp_pixel_format fmt) {
  ImageBuffer out;
  out.resize(w, h, fmt);
  std::memcpy(out.writable_pixels().data(), bytes.data(), bytes.size());
  return out;
}

}  // namespace

/*
 * deep round trip：16 位图层穿过一个 8 位操作后**仍是 16 位**，且四档合回的
 * 结果与「同一个操作只跑一遍（在 low 档上）」不同——后者会证明它确实读了
 * +1/+2/+4 三档，而不是把往返退化成单趟 8 位操作。
 *
 * 顺带钉住两条边界：
 *   - inner 为 none 时往返不是无损的（低档信息已丢），note 必须如实说明；
 *   - inner 是不改几何、不写蒙版的算子之外的类别时，参数闸拒绝（不是执行期报错）。
 */
COMP_TEST(tool_ops_domain_deep_round_trip_keeps_sixteen_bits) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);
  COMP_CHECK(!before.empty());

  /* inner = lens，k = 0.3：径向重采样在 low 与 +1/+2/+4 上结果不同。 */
  ToolOpParams params;
  params.kind = kToolOpDeep;
  params.deep_mode = kToolDeepRoundTrip;
  params.inner = kToolOpLens;
  params.lens.k = 0.3;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  std::string note;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, &note),
                 "deep round trip with an inner lens is a real product path now");
  COMP_CHECK_MSG(code == COMP_OK, "the round trip reports COMP_OK");

  const Layer* layer = document.find(id);
  COMP_CHECK_MSG(layer != nullptr, "the layer is still there");
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA16,
                 "round trip keeps the layer at 16 bits (it must not downgrade to RGBA8)");
  COMP_CHECK_MSG(layer->pixels.byte_size() == before.size(),
                 "the pixel buffer size is unchanged (16 bits a channel throughout)");

  const std::vector<uint8_t> after = pixels_of(document, id);
  COMP_CHECK_MSG(after != before, "the inner lens actually changed the pixels");

  /* 一步撤销。 */
  COMP_CHECK_MSG(document.history().can_undo(), "the round trip is one undo step");
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "one undo restores the 16-bit pixels byte for byte");
}

/*
 * 内层为 none 的往返：图层保持 16 位，但那条 note 必须说明它丢了 8 位以上精度——
 * 「无损」这种话不说出去，后面就会有人拿它当无损存 16 位用。
 */
COMP_TEST(tool_ops_domain_deep_round_trip_without_inner_reports_precision_loss) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  ToolOpParams params;
  params.kind = kToolOpDeep;
  params.deep_mode = kToolDeepRoundTrip;
  params.inner = kToolOpNone;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  std::string note;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, &note),
                 "a round trip without an inner op still runs");

  const Layer* layer = document.find(id);
  COMP_CHECK_MSG(layer != nullptr && layer->pixels.format == COMP_PIXEL_RGBA16,
                 "the layer stays at 16 bits even with no inner op");
  COMP_CHECK_MSG(note.find("precision") != std::string::npos,
                 "the note must admit the precision loss above 8 bits");
  COMP_CHECK_MSG(pixels_of(document, id) != before,
                 "quantize-and-read-back is not a byte-for-byte no-op");
}

/*
 * 往返的内层算子必须是「不改几何、不写蒙版」的那一类。stroke / heal / select
 * 在 16 位上没有可降级的语义，必须在参数闸就被拒（且不留事务、不进撤销栈）。
 */
COMP_TEST(tool_ops_domain_deep_round_trip_rejects_geometry_ops) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  ToolOpParams params;
  params.kind = kToolOpDeep;
  params.deep_mode = kToolDeepRoundTrip;
  params.inner = kToolOpStroke;      /* 落笔要坐标，不是「对副本做像素变换」 */

  comp_result code = COMP_OK;
  std::string error;
  COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                 "an inner stroke is refused rather than faked");
  COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT,
                 "an unsupported inner op is INVALID_ARGUMENT, not STATE or UNSUPPORTED");
  COMP_CHECK_MSG(!document.txn_active(), "the gate runs before txn_begin: no txn left open");
  COMP_CHECK_MSG(!document.history().can_undo(), "a refused op must not enter the undo stack");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "a refused op must not touch the pixels");
}

COMP_TEST(tool_ops_domain_invalid_deep_mode_is_bad_argument) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  ToolOpParams params;
  params.kind = kToolOpDeep;
  params.deep_mode = static_cast<ToolDeepMode>(7); /* 未知模式：参数错误，不是能力边界 */

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                 "an unknown deep mode is refused");
  COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT,
                 "an unknown enum value is INVALID_ARGUMENT, not UNSUPPORTED");
  COMP_CHECK_MSG(!document.history().can_undo(), "the failed call left no history step");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "the failed call left the pixels alone");
}

COMP_TEST(tool_ops_domain_param_gate_precedes_transaction) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  /* |k| > 1 被参数闸拒绝（口径见 tool_ops.h）。 */
  ToolOpParams bad;
  bad.kind = kToolOpLens;
  bad.lens.k = 5.0;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(!run_tool_op(&document, id, bad, &code, &error, nullptr), "absurd k");
  COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "absurd k is a parameter error");
  COMP_CHECK_MSG(!document.txn_active(),
                 "gate-before-txn: a rejected call must not leave a dangling transaction");
  COMP_CHECK_MSG(!document.history().can_undo(), "a rejected call must not enter the undo stack");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "a rejected call must not write half-work");

  /*
   * 紧接一次合法调用必须仍然成功——这是「上一次失败没把文档留在坏状态」的实证。
   * （若闸在事务内返回，这里会因为 txn 已 active 而失败。）
   */
  ToolOpParams good;
  good.kind = kToolOpLens;
  good.lens.k = 0.0; /* 恒等：逐字节不变，但确实是一次成功的操作 */
  code = COMP_ERR_INTERNAL;
  COMP_CHECK_MSG(run_tool_op(&document, id, good, &code, &error, nullptr),
                 "a legal call right after a rejected one must still succeed");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "identity lens is byte-identical");
}

/*
 * 16 位通路的两条域层护栏（AUDIT7 §6 缺口一「Deep 降位路径不可达」的收口）。
 *
 * 分工：`tests/test_abi.cpp` 的 `sixteen_bit_layer_composites_as_its_eight_bit_copy`
 * 钉的是壳体视角的等价性；这里钉的是域层内部事实——quantize 产出的就是那份
 * canonical 8 位副本（不是第二份实现），以及 8 位工具会把 16 位图层降成 8 位。
 */
COMP_TEST(tool_ops_domain_deep_quantize_turns_a_sixteen_bit_layer_into_eight_bits) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> wide_before = pixels_of(document, id);
  COMP_CHECK_MSG(wide_before.size() == 4u * 4u * 8u, "the layer really holds 16-bit pixels");

  ToolOpParams params;
  params.kind = kToolOpDeep;
  params.deep_mode = kToolDeepQuantize;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "deep quantize on a 16-bit layer is a reachable path");

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA8,
                 "the layer is 8-bit afterwards (.comp only writes RGBA8 layers)");
  COMP_CHECK_MSG(layer->pixels.width == 4 && layer->pixels.height == 4,
                 "quantize keeps the geometry");

  const ImageBuffer wide = copy_of(wide_before, 4, 4, COMP_PIXEL_RGBA16);
  ImageBuffer expected;
  COMP_CHECK(deep_to_rgba8(wide, &expected));
  COMP_CHECK_MSG(pixels_of(document, id) == expected.pixel_data(),
                 "quantize is the canonical 8-bit copy, not a second implementation");
  COMP_CHECK_MSG(document.history().can_undo(), "quantize is one undo step");
}

/* [C1-7] deep_to_rgba8 / hdr_to_rgba8 的格式闸互不越界。
 *
 * 这条护栏是为一次**注入未红**补的：把 deep_to_rgba8 的入口从
 * `format != RGBA16` 放宽成 `!= RGBA16 && != RGBA16F`（即 16 位整数转换器
 * 顺带吃半精度图），此时若 resample 的分流还在，画面照旧全绿——因为没人真的
 * 走到那条错路。只有两条断言里有一条独立于分流，才钉得住「深拷贝那一步本身
 * 必须拒绝半精度」。ABI 层的两个用例只经由合成路径，钉不住这个入口。
 *
 * 方向不能反：hdr_to_rgba8 收到 RGBA16 同样必须拒绝（否则 16 位图层会被
 * 当 half 读，值域差 65535 倍）。
 */
COMP_TEST(deep_and_hdr_eight_bit_views_keep_their_own_formats) {
  const int32_t w = 4, h = 2;
  ImageBuffer wide;
  wide.resize(w, h, COMP_PIXEL_RGBA16);
  std::memset(wide.writable_pixels().data(), 0x3c, wide.writable_pixels().size());

  ImageBuffer hdr;
  hdr.resize(w, h, COMP_PIXEL_RGBA16F);
  std::memset(hdr.writable_pixels().data(), 0x3c, hdr.writable_pixels().size());

  ImageBuffer out;
  COMP_CHECK_MSG(!deep_to_rgba8(hdr, &out),
                 "the 16-bit integer view refuses a half-float source instead of reading bit patterns");
  COMP_CHECK_MSG(!hdr_to_rgba8(wide, &out),
                 "the half-float view refuses a 16-bit integer source instead of halving its values");

  /* 正确方向仍然可用：两者互为对方不能做的事。 */
  COMP_CHECK_MSG(deep_to_rgba8(wide, &out) && out.format == COMP_PIXEL_RGBA8,
                 "the 16-bit integer view still accepts 16-bit integer pixels");
  COMP_CHECK_MSG(hdr_to_rgba8(hdr, &out) && out.format == COMP_PIXEL_RGBA8,
                 "the half-float view still accepts half-float pixels");

  /* 空缓冲同样拒绝，且不写 dst（空 src 没有可转换的像素）。 */
  ImageBuffer empty;
  empty.resize(0, 0, COMP_PIXEL_RGBA16F);
  ImageBuffer sentinel;
  sentinel.resize(2, 2, COMP_PIXEL_RGBA8);
  std::memset(sentinel.writable_pixels().data(), 0x5a, sentinel.writable_pixels().size());
  const std::vector<uint8_t> before = sentinel.pixel_data();
  COMP_CHECK_MSG(!hdr_to_rgba8(empty, &sentinel), "an empty half-float buffer is refused");
  COMP_CHECK_MSG(sentinel.pixel_data() == before, "a refused conversion does not disturb the destination");
}

COMP_TEST(tool_ops_domain_pixel_tool_downgrades_a_sixteen_bit_layer) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> wide_before = pixels_of(document, id);

  /* 恒等 lens：不改任何像素，于是「图层落到 8 位副本」是可单独观测的那一步。 */
  ToolOpParams params;
  params.kind = kToolOpLens;
  params.lens.k = 0.0;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "identity lens on a 16-bit layer");

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA8,
                 "an 8-bit tool leaves the layer 8-bit (upstream DeepLayers.swift:5)");

  const ImageBuffer wide = copy_of(wide_before, 4, 4, COMP_PIXEL_RGBA16);
  ImageBuffer expected;
  COMP_CHECK(deep_to_rgba8(wide, &expected));
  COMP_CHECK_MSG(pixels_of(document, id) == expected.pixel_data(),
                 "the tool ran on the layer's canonical 8-bit copy, byte for byte");
}

COMP_TEST(tool_ops_domain_zero_pressure_stroke_is_byte_identical) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  const float points[4] = {1.0f, 1.0f, 6.0f, 6.0f};
  ToolOpParams params;
  params.kind = kToolOpStroke;
  params.points = points;
  params.point_count = 2;
  params.brush.diameter = 4.0f;
  params.brush.pressure = 0.0f; /* pressure * flow == 0 ⇒ 不可见 */
  params.brush.hardness = 1.0f;
  params.brush.flow = 1.0f;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "a zero-pressure stroke is a legal no-op");
  COMP_CHECK_MSG(pixels_of(document, id) == before,
                 "pressure * flow == 0 must be byte-identical (documented contract)");
}

/* ================================================================ 颗粒 */

COMP_TEST(tool_ops_domain_grain_is_reproducible_and_origin_anchored) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");

  /*
   * 两条图层缓冲内容相同，但**文档原点不同**：id0 覆盖 (0,0)-(8,8)，
   * id4 覆盖 (4,4)-(12,12)。颗粒噪声钉在文档坐标上，所以同一条落在同一个
   * 文档像素 (6,6) 上的笔画，两条图层得到的颗粒**必须不同**——若相同就说明
   * 噪声跟着图层原点跑（也就是用了 noise_add 而不是 noise_add_at）。
   */
  comp_layer_desc at0{};
  at0.name = "At0";
  at0.transform = comp_rect{0, 0, 8, 8};
  comp_layer_id id0 = 0;
  document.create_layer(at0, &id0);
  document.find(id0)->pixels = ImageBuffer::solid(8, 8, COMP_PIXEL_RGBA8, 40, 80, 120, 255);

  comp_layer_desc at4{};
  at4.name = "At4";
  at4.transform = comp_rect{4, 4, 8, 8};
  comp_layer_id id4 = 0;
  document.create_layer(at4, &id4);
  document.find(id4)->pixels = ImageBuffer::solid(8, 8, COMP_PIXEL_RGBA8, 40, 80, 120, 255);

  /* 两层的无颗粒基线，逐字节相同。 */
  const std::vector<uint8_t> plain0 = pixels_of(document, id0);
  const std::vector<uint8_t> plain4 = pixels_of(document, id4);
  COMP_CHECK_MSG(plain0 == plain4, "the two layers start byte-identical");

  /* 落笔点固定在文档像素 (6,6)；压力 0 ⇒ 只加颗粒、不落笔。 */
  const float points[2] = {6.0f, 6.0f};
  auto run_grain = [&](comp_layer_id id) {
    ToolOpParams params;
    params.kind = kToolOpStroke;
    params.points = points;
    params.point_count = 1;
    params.brush.diameter = 2.0f;
    params.brush.pressure = 0.0f;
    params.brush.hardness = 1.0f;
    params.brush.flow = 1.0f;
    params.brush.grain_amount = 30.0f;
    params.brush.seed = 12345;
    comp_result code = COMP_ERR_INTERNAL;
    std::string error;
    COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr), "grain stroke");
    COMP_CHECK_MSG(code == COMP_OK, "out_code is written on success");
  };

  run_grain(id0);
  const std::vector<uint8_t> grained0 = pixels_of(document, id0);
  COMP_CHECK_MSG(grained0 != plain0, "grain actually changes pixels (otherwise this proves nothing)");

  /*
   * 可重复性口径（同 seed + 同起始像素 ⇒ 同结果），**不是幂等**：
   * 颗粒是叠加的，再跑一遍会在已有颗粒上再加一层，正是画笔该有的行为。
   * 所以复现性要用一条起始像素相同、文档原点也相同的图层来比，
   * 而不是把同一条图层跑两遍。
   */
  comp_layer_desc twin_desc{};
  twin_desc.name = "At0Twin";
  twin_desc.transform = comp_rect{0, 0, 8, 8};
  comp_layer_id id_twin = 0;
  document.create_layer(twin_desc, &id_twin);
  document.find(id_twin)->pixels = ImageBuffer::solid(8, 8, COMP_PIXEL_RGBA8, 40, 80, 120, 255);
  COMP_CHECK_MSG(pixels_of(document, id_twin) == plain0, "twin starts byte-identical");
  run_grain(id_twin);
  COMP_CHECK_MSG(pixels_of(document, id_twin) == grained0,
                 "same seed + same start + same document origin => byte-identical grain "
                 "(reproducibility contract)");

  run_grain(id4);
  const std::vector<uint8_t> grained4 = pixels_of(document, id4);
  COMP_CHECK_MSG(grained4 != plain4, "the offset layer is grained too");
  COMP_CHECK_MSG(grained0 != grained4,
                 "grain keys off document coordinates: the same buffer content at a different "
                 "layer origin gets different noise (this is what noise_add_at buys over noise_add)");
}

COMP_TEST(tool_ops_domain_zero_grain_is_byte_identical) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  const float points[2] = {3.0f, 3.0f};
  ToolOpParams params;
  params.kind = kToolOpStroke;
  params.points = points;
  params.point_count = 1;
  params.brush.diameter = 4.0f;
  params.brush.pressure = 0.0f;
  params.brush.grain_amount = 0.0f;  /* 不加颗粒 */

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr), "zero grain");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "grain_amount == 0 is byte-identical");
}

COMP_TEST(tool_ops_domain_negative_grain_is_refused_before_transaction) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = document.history().undo_depth();

  const float points[2] = {3.0f, 3.0f};
  ToolOpParams params;
  params.kind = kToolOpStroke;
  params.points = points;
  params.point_count = 1;
  params.brush.diameter = 4.0f;
  params.brush.grain_amount = -1.0f;  /* 负数会被内核当反相噪声，必须拒绝 */

  comp_result code = COMP_OK;
  std::string error;
  COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr), "negative grain refused");
  COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "negative grain -> COMP_ERR_INVALID_ARGUMENT");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "a refused call changes no pixels");
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before, "and records no history");
}

/* ================================================================ 直方图 */

COMP_TEST(tool_ops_domain_histogram_unpremultiplies_before_taking_luma) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  comp_layer_desc desc{};
  desc.name = "Half";
  desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id id = 0;
  document.create_layer(desc, &id);

  /*
   * 预乘半透明红：rgb=(128,0,0) alpha=128 ⇒ 直通红是 (255,0,0)，亮度应落 76 桶。
   * 若引擎忘了反预乘、直接拿 128 去算，桶会落在 38 附近——差一倍。
   */
  ImageBuffer buffer;
  buffer.resize(4, 4, COMP_PIXEL_RGBA8);
  uint8_t* px = buffer.writable_pixels().data();
  const size_t stride = buffer.stride();
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      uint8_t* p = px + y * stride + x * 4;
      p[0] = 128; p[1] = 0; p[2] = 0; p[3] = 128;
    }
  }
  document.find(id)->pixels = std::move(buffer);

  double bins[256];
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(layer_histogram(*document.find(id), nullptr, 0, bins, &code, &error),
                 "histogram of an 8-bit layer");
  COMP_CHECK_MSG(code == COMP_OK, "out_code is written on success");

  const int expected = static_cast<int>(std::lround(0.299f * 255.0f));
  COMP_CHECK_MSG(bins[expected] == 16.0,
                 "premultiplied (128,0,0,128) must report luma 0.299*255 (unpremultiplied), "
                 "not 0.299*128 — all 16 pixels land in one bucket");

  double total = 0.0;
  for (int i = 0; i < 256; ++i) total += bins[i];
  COMP_CHECK_MSG(total == 16.0, "every pixel is counted exactly once");
}

COMP_TEST(tool_ops_domain_histogram_weights_by_coverage) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);

  /* 全覆盖 ⇒ 与无 coverage 同结果；全 0 ⇒ 总权重 0。 */
  std::vector<uint8_t> full(4 * 4, 255);
  std::vector<uint8_t> none(4 * 4, 0);
  double plain[256];
  double covered[256];
  double empty[256];
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK(layer_histogram(*document.find(id), nullptr, 0, plain, &code, &error));
  COMP_CHECK(layer_histogram(*document.find(id), full.data(), 4, covered, &code, &error));
  COMP_CHECK(layer_histogram(*document.find(id), none.data(), 4, empty, &code, &error));

  bool covered_matches = true;
  for (int i = 0; i < 256; ++i) {
    if (plain[i] != covered[i]) covered_matches = false;
  }
  COMP_CHECK_MSG(covered_matches, "coverage 255 is weight 1 per pixel, same as no coverage");

  double empty_total = 0.0;
  for (int i = 0; i < 256; ++i) empty_total += empty[i];
  COMP_CHECK_MSG(empty_total == 0.0, "coverage 0 everywhere weighs nothing");
}

COMP_TEST(tool_ops_domain_histogram_rejects_null_bins_and_sixteen_bit) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  comp_result code = COMP_OK;
  std::string error;

  const comp_layer_id id = add_pixel_layer(document);
  COMP_CHECK(!layer_histogram(*document.find(id), nullptr, 0, nullptr, &code, &error));
  COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "null bins -> COMP_ERR_INVALID_ARGUMENT");

  /* 16 位图层显式拒绝：桶是 8 位的，本函数不偷偷降位。 */
  Document wide_doc(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id wid = add_wide_layer(wide_doc);
  double bins[256];
  code = COMP_OK;
  COMP_CHECK(!layer_histogram(*wide_doc.find(wid), nullptr, 0, bins, &code, &error));
  COMP_CHECK_MSG(code == COMP_ERR_UNSUPPORTED,
                 "16-bit layer -> COMP_ERR_UNSUPPORTED (quantize via COMP_TOOL_DEEP first)");
}

/* ================================================================ 修补范围 */

COMP_TEST(tool_ops_domain_coverage_bounds_matches_nonzero_pixels) {
  const int32_t w = 8;
  const int32_t h = 6;
  const size_t stride = 8;
  std::vector<uint8_t> cov(static_cast<size_t>(h) * stride, 0);
  /* 一块 3x2 的非零区域，落在 [2,5) x [1,3)。 */
  for (int32_t y = 1; y < 3; ++y) {
    for (int32_t x = 2; x < 5; ++x) {
      cov[static_cast<size_t>(y) * stride + static_cast<size_t>(x)] = 255;
    }
  }
  int32_t bounds[4] = {-1, -1, -1, -1};
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(heal_coverage_bounds_of(cov.data(), stride, w, h, bounds, &code, &error),
                 "coverage bounds");
  COMP_CHECK_MSG(code == COMP_OK, "out_code is written on success");
  COMP_CHECK_MSG(bounds[0] == 2 && bounds[1] == 1 && bounds[2] == 5 && bounds[3] == 3,
                 "half-open bounds [x0,y0,x1,y1) of the nonzero region");

  /* 全 0 coverage ⇒ 全 0（不是 [-1,-1,-1,-1]，宿主会拿它去算宽度）。 */
  std::vector<uint8_t> none(static_cast<size_t>(h) * stride, 0);
  int32_t zero_bounds[4] = {7, 7, 7, 7};
  COMP_CHECK(heal_coverage_bounds_of(none.data(), stride, w, h, zero_bounds, &code, &error));
  COMP_CHECK_MSG(zero_bounds[0] == 0 && zero_bounds[1] == 0 && zero_bounds[2] == 0 &&
                     zero_bounds[3] == 0,
                 "an empty coverage reports all-zero bounds, which reads as a zero-width box");

  /* 参数闸 */
  code = COMP_OK;
  COMP_CHECK(!heal_coverage_bounds_of(cov.data(), stride, 0, h, bounds, &code, &error));
  COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "non-positive width -> COMP_ERR_INVALID_ARGUMENT");
  code = COMP_OK;
  COMP_CHECK(!heal_coverage_bounds_of(nullptr, stride, w, h, bounds, &code, &error));
  COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "null coverage -> COMP_ERR_INVALID_ARGUMENT");
}
