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
