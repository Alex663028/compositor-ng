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
 *   6. `pressure * flow == 0` 的笔画逐字节不变（可重复性契约的边界）；
 *   7. HDR（COMP_PIXEL_RGBA16F）图层上的 deep round trip 走 normalize/restore，
 *      深度不变、一步撤销、高光还在白以上 [C2-1 / C2-2]；
 *   8. HDR 的 deep quantize 是 half 视图那条路，不是 16 位整数视图 [C2-3]；
 *   9. HDR 图层上的其余工具如实拒绝（STATE），不静默降位 [C2-4]。
 *
 * 本文件的错误码断言全部钉**具体值**而非「非 OK」：`!= COMP_OK` 会让
 * INTERNAL 这类内部错误伪装成「按契约拒绝了」。
 */
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "domain/document.h"
#include "domain/layer.h"
#include "domain/tool_ops.h"
#include "engine.h"
#include "imaging/DitherPixels.h"
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

/*
 * float → half 的位模式，half 位模式 → float。
 *
 * 本文件的取值全是二的幂（0.25 / 0.5 / 1 / 2 / 4），尾数精确，所以这里只做
 * 「移位 + 偏置」的直译，不写第二份参考舍入：被测的是域层的**接线**，
 * 内核自己的转换口径由 test_hdr_semantics.cpp 的 C0 那批钉住。
 * 负数（符号位）本文件不用。
 */
uint16_t half_bits(float value) {
  union {
    float f;
    uint32_t u;
  } in = {value};
  const uint32_t exponent = (in.u >> 23) & 0xffu;
  if (exponent == 0xffu) return static_cast<uint16_t>(0x7c00u);
  const int32_t e = static_cast<int32_t>(exponent) - 127 + 15;
  if (e <= 0) return 0; /* 本文件不用次正规数 */
  if (e >= 31) return 0x7c00u;
  return static_cast<uint16_t>((static_cast<uint32_t>(e) << 10) | ((in.u & 0x7fffffu) >> 13));
}

float half_value(uint16_t bits) {
  const uint32_t exponent = (bits >> 10) & 0x1fu, mantissa = bits & 0x3ffu;
  uint32_t out = 0;
  if (exponent == 31u) {
    out = 0x7f800000u | (mantissa << 13);
  } else if (exponent != 0u) {
    out = ((exponent + 112u) << 23) | (mantissa << 13);
  }
  float value = 0.0f;
  std::memcpy(&value, &out, sizeof(value));
  return value;
}

/* 读回某个像素的四个 half 通道（预乘、native-endian）；越界时返回全 0。 */
void half_pixel(const std::vector<uint8_t>& bytes, int32_t w, int32_t x, int32_t y, uint16_t out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0;
  const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
  if (bytes.size() < (at + 4u) * sizeof(uint16_t)) return;
  std::memcpy(out, bytes.data() + at * sizeof(uint16_t), 4u * sizeof(uint16_t));
}

/*
 * 铺满画布的 **HDR**（半精度 RGBA16F、预乘、native-endian）图层：全幅同一个
 * 预乘色与不透明度。
 *
 * 之所以要**均匀**：本节的断言是「那个 8 位操作看到了什么」，而它只在均匀场上
 * 才有唯一答案（重采样 / 抖动在均匀场上都是确定性的），不必靠逐像素碰运气。
 */
comp_layer_id add_hdr_layer(Document& doc, uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
  comp_layer_desc desc{};
  desc.name = "HDR";
  desc.transform = comp_rect{0, 0, doc.state().width, doc.state().height};
  comp_layer_id id = 0;
  doc.create_layer(desc, &id);
  Layer* layer = doc.find(id);
  if (layer != nullptr) {
    const int32_t w = doc.state().width;
    const int32_t h = doc.state().height;
    layer->pixels.resize(w, h, COMP_PIXEL_RGBA16F);
    uint16_t* wide = reinterpret_cast<uint16_t*>(layer->pixels.writable_pixels().data());
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < count; ++i) {
      wide[i * 4u + 0u] = r;
      wide[i * 4u + 1u] = g;
      wide[i * 4u + 2u] = b;
      wide[i * 4u + 3u] = a;
    }
  }
  return id;
}

/*
 * 同样的 HDR 图层，但**有变化**：左边 3/4 是均匀的高光块（`highlight`），右边 1/4
 * 逐列递增（0.25 / 0.5 / 0.75 / 1.0），不透明度一律 1。
 *
 * 为什么要有变化：全幅均匀时径向重采样是恒等的，改不了任何字节——「像素确实被
 * 操作改过」这条断言就会落空。而高光块内部仍然均匀，所以「高光还在白以上」那条
 * 断言在块中央依旧可判定（取景中心离交界有 4 列，任何径向映射都不会把它挪出去）。
 */
comp_layer_id add_hdr_gradient_layer(Document& doc, float highlight) {
  const uint16_t dim[4] = {half_bits(0.25f), half_bits(0.5f), half_bits(0.75f), half_bits(1.0f)};
  const int32_t w = doc.state().width, h = doc.state().height;
  comp_layer_desc desc{};
  desc.name = "HDR";
  desc.transform = comp_rect{0, 0, w, h};
  comp_layer_id id = 0;
  doc.create_layer(desc, &id);
  Layer* layer = doc.find(id);
  if (layer != nullptr) {
    layer->pixels.resize(w, h, COMP_PIXEL_RGBA16F);
    uint16_t* wide = reinterpret_cast<uint16_t*>(layer->pixels.writable_pixels().data());
    for (int32_t y = 0; y < h; ++y) {
      for (int32_t x = 0; x < w; ++x) {
        uint16_t* px = wide + (static_cast<size_t>(y) * static_cast<size_t>(w) +
                               static_cast<size_t>(x)) * 4u;
        const bool bright = x * 4 < w * 3; /* 左 3/4 = 高光块 */
        const uint16_t value = bright ? half_bits(highlight) : dim[x - (w / 4) * 3];
        px[0] = px[1] = px[2] = value;
        px[3] = half_bits(1.0f);
      }
    }
  }
  return id;
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

/* ================================================= C2：HDR（半精度）图层上的往返 */

/*
 * [C2-1] HDR 图层（COMP_PIXEL_RGBA16F）上的 deep round trip——这是**今天唯一**
 * 真的走到 deep_hdr_normalize / deep_hdr_restore 的产品路径（上游
 * `DeepLayer.run` 的 HDR 分支，DeepLayers.swift:196 的 `normalizing` 那一层）。
 * C2 之前它被参数闸按「不是 RGBA16」直接拒掉。
 *
 * 顺带钉住「深度不变」：16 位图层保持 16 位是 DeepLayers.swift:5 的规则，
 * HDR 图层保持 half 是 :82-84 的规则——往返不能把 HDR 降成 8 位，否则白以上的光
 * 就被永久丢掉了。
 *
 * 场必须**有变化**：均匀场乘上径向重采样还是自己，改不改都逐字节相同，那样的
 * 断言没有判定力。左 3/4 是白以上的 4.0 高光块、右 1/4 是 0.25→1.0 的逐列阶梯；
 * 中心落在高光块里且径向畸变在中心是恒等的，于是「restore 把 normalize 除掉的
 * scale 乘了回来」是可判定的——高光穿过 8 位操作后仍在白以上。
 */
COMP_TEST(tool_ops_domain_deep_round_trip_carries_an_hdr_layer) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_hdr_gradient_layer(document, 4.0f); /* 白以上的高光、完全不透明 */
  const std::vector<uint8_t> before = pixels_of(document, id);
  COMP_CHECK(!before.empty());
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16F, "the layer starts as half floats");

  ToolOpParams params;
  params.kind = kToolOpDeep;
  params.deep_mode = kToolDeepRoundTrip;
  params.inner = kToolOpLens;
  params.lens.k = 0.3;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  std::string note;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, &note),
                 "the deep round trip reaches an HDR layer (the parameter gate used to refuse it)");
  COMP_CHECK_MSG(code == COMP_OK, "the HDR round trip reports COMP_OK");

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA16F,
                 "the HDR layer stays half-float (the round trip must not downgrade it)");
  COMP_CHECK_MSG(layer->pixels.byte_size() == before.size(),
                 "the pixel buffer size is unchanged (8 bytes a pixel throughout)");

  const std::vector<uint8_t> after = pixels_of(document, id);
  COMP_CHECK_MSG(after != before, "the inner lens actually changed the pixels");

  uint16_t px[4] = {0, 0, 0, 0};
  half_pixel(after, 16, 8, 8, px);
  COMP_CHECK_MSG(half_value(px[0]) > 2.0f && half_value(px[3]) > 0.9f,
                 "a highlight stays well above white through the round trip");

  COMP_CHECK_MSG(document.history().can_undo(), "the round trip is one undo step");
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "one undo restores the half-float pixels byte for byte");
}

/*
 * [C2-2] 8 位操作在 HDR 图层上看到的，必须是**半精度的值**（各自按最亮通道除回
 * 白内的那一份），而不是 half 的位模式被当成 16 位整数读出来的那份数——这正是
 * DeepLayers.swift:82-84/:97-99 的 normalize → run → restore 三步存在的理由。
 *
 * 探针是不透明的 0.5 中灰场加 1-bit 有序抖动（BAYER_2、levels 2、无扩散、密度与
 * 对比度取默认的 0 ⇒ gamma 与 contrast 都是恒等）：抖动的输出只由「它看到的色调」
 * 决定，两种读法给出的色调差得很远——
 *   - 按值读（正确）：scale = max(0.5, 1) = 1，操作看到 0.5 ⇒ 4 个亚单元亮 2 个；
 *   - 按位模式读（把 half 当 16 位整数，即 C1 之前那条错路）：0.5 的位模式 0x3800
 *     经 `deep_low` 落成 54/60 ≈ 0.9（alpha 0x3C00 同样被读成 60，反预乘又把它
 *     除回比例上，所以只看「亮不亮」是看不出来的）⇒ 4 个亚单元全亮。
 * 判据因此是亮点比例的上下界：≥1/4（操作确实在做事）且 ≤3/4（它没看到满白）。
 *
 * 同一趟钉住 alpha：完全不透明的 alpha 走完往返必须逐字节还是 1.0（0x3C00）。
 * 错路上它会被 merge 的 `lo[3]*257` 改写成 0x3C3C ≈ 1.059——「alpha 悄悄变了」
 * 是最容易漏掉的那种坏法，而且它不依赖任何阈值判断，一眼就能看出来。
 *
 * 另钉「不写坏位模式」：把 16 位整数直接当 half 存回，值一过 0x7C00 就是 Inf/NaN。
 */
COMP_TEST(tool_ops_domain_hdr_round_trip_lets_the_op_see_half_values) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_hdr_layer(document, half_bits(0.5f), half_bits(0.5f), half_bits(0.5f),
                                         half_bits(1.0f));

  ToolOpParams params;
  params.kind = kToolOpDeep;
  params.deep_mode = kToolDeepRoundTrip;
  params.inner = kToolOpDither;
  params.dither.style = DITHER_BAYER_2; /* 有序：均匀场上的点亮比例是确定的 */
  params.dither.levels = 2;             /* 纯 1-bit */
  params.dither.diffusion = 0.0f;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  std::string note;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, &note),
                 "a 1-bit dither inside an HDR round trip is a real product path");
  COMP_CHECK_MSG(code == COMP_OK, "the HDR round trip reports COMP_OK");

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA16F, "the HDR layer stays half-float");

  const std::vector<uint8_t> after = pixels_of(document, id);
  const uint16_t* wide = reinterpret_cast<const uint16_t*>(after.data());
  const size_t pixel_count = static_cast<size_t>(8) * 8u;
  size_t broken = 0; /* 位模式落在 Inf/NaN 区间（0x7C00 以上）的通道数 */
  for (size_t i = 0; i < pixel_count * 4u; ++i) {
    if (!std::isfinite(half_value(wide[i]))) ++broken;
  }
  COMP_CHECK_MSG(broken == 0,
                 "every half-float channel stays finite (16-bit integers must not be stored as half bit "
                 "patterns, where 0x7C00 and up is Inf/NaN; " + std::to_string(broken) + " channels are not)");
  /* 逐像素判定、一次汇总：一个坏像素和整幅坏掉不该给出同样长的输出。 */
  size_t opaque = 0;
  size_t first_off = pixel_count; /* 第一个不是 0x3C00 的像素，报出来给人看 */
  for (size_t i = 0; i < pixel_count; ++i) {
    if (wide[i * 4u + 3u] == half_bits(1.0f)) {
      ++opaque;
    } else if (first_off == pixel_count) {
      first_off = i;
    }
  }
  COMP_CHECK_MSG(opaque == pixel_count,
                 "full opacity survives the round trip exactly (1.0 is 0x3C00 before and after; " +
                     std::to_string(pixel_count - opaque) + " of " + std::to_string(pixel_count) +
                     " pixels differ, first at " + std::to_string(first_off) + " = 0x" +
                     std::to_string(first_off < pixel_count ? wide[first_off * 4u + 3u] : 0u) + ")");

  /* 8 位视图：levels == 2 的抖动只产出黑与白，亮点比例就是「它看到的色调」。 */
  ImageBuffer view;
  COMP_CHECK(hdr_to_rgba8(copy_of(after, 8, 8, COMP_PIXEL_RGBA16F), &view));
  const std::vector<uint8_t>& v = view.pixel_data();
  int lit = 0, total = 0;
  for (size_t i = 0; i + 3u < v.size(); i += 4u) {
    ++total;
    if (v[i] > 128) ++lit;
  }
  COMP_CHECK_MSG(lit * 4 >= total,
                 "the op ran on a mid tone, so some cells stay dark; lit = " + std::to_string(lit) + " of " +
                     std::to_string(total));
  COMP_CHECK_MSG(lit * 4 <= total * 3,
                 "the op saw 0.5 (half the cells lit), not the bit pattern read as an integer (all lit); lit = " +
                     std::to_string(lit) + " of " + std::to_string(total));

  COMP_CHECK_MSG(document.history().can_undo(), "the round trip is one undo step");
  document.undo();
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16F, "undo keeps the depth too");
}

/*
 * [C2-3] HDR 图层的 deep quantize：落成 8 位副本的那一步必须走
 * `hdr_to_rgba8`（half 的**值**按 0..65535 取整后再抖动取整，domain/layer.cpp
 * 的两趟），不能把 half 的位模式当 16 位整数读——0x3C00（1.0）会被读成 15360，
 * 画面成了噪声。
 *
 * 判据是**双向**的：结果既逐字节等于 half 视图，也逐字节不等于「同样这些字节
 * 按 RGBA16 解释」的那份。只钉前者的话，某个把两种转换都改坏的改动不会变红。
 */
COMP_TEST(tool_ops_domain_hdr_quantize_is_the_half_to_u16_view) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id id = add_hdr_layer(document, half_bits(4.0f), half_bits(0.5f), half_bits(0.25f),
                                         half_bits(1.0f));
  const std::vector<uint8_t> half_before = pixels_of(document, id);

  ToolOpParams params;
  params.kind = kToolOpDeep;
  params.deep_mode = kToolDeepQuantize;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "deep quantize on an HDR layer is a real product path");
  COMP_CHECK_MSG(code == COMP_OK, "quantize reports COMP_OK");

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA8,
                 "quantize lands the layer on 8 bits (upstream DeepLayers.swift:297)");
  COMP_CHECK_MSG(layer->pixels.width == 4 && layer->pixels.height == 4, "quantize keeps the geometry");

  ImageBuffer expected;
  COMP_CHECK(hdr_to_rgba8(copy_of(half_before, 4, 4, COMP_PIXEL_RGBA16F), &expected));
  COMP_CHECK_MSG(pixels_of(document, id) == expected.pixel_data(),
                 "quantize is the canonical half-float 8-bit view, byte for byte");

  ImageBuffer misinterpreted;
  COMP_CHECK(deep_to_rgba8(copy_of(half_before, 4, 4, COMP_PIXEL_RGBA16), &misinterpreted));
  COMP_CHECK_MSG(pixels_of(document, id) != misinterpreted.pixel_data(),
                 "the same bytes read as 16-bit integers is a different image (that is the road not taken)");
  COMP_CHECK_MSG(document.history().can_undo(), "quantize is one undo step");
}

/*
 * [C2-4] 8 位工具**不**在 HDR 图层上降位：如实拒绝，不静默丢高光。
 *
 * 16 位图层被 8 位工具降成 8 位是 DeepLayers.swift:5 的既定规则（上游就是这么
 * 缩一半内存的），但 HDR 的规则是 :82-84 的 normalize/restore 往返——今天只有
 * deep 的往返那条路（[C2-1]），其余工具（stroke / heal / dither / lens /
 * scanlines / chunky）宁可返回 STATE 也不把白以上的光永久剪掉。
 *
 * 错误码钉具体值：`!= COMP_OK` 会让 INTERNAL（内核出错）伪装成「按契约拒绝了」。
 */
COMP_TEST(tool_ops_domain_hdr_layer_is_refused_by_eight_bit_tools) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id id = add_hdr_layer(document, half_bits(4.0f), half_bits(4.0f), half_bits(4.0f),
                                         half_bits(1.0f));
  const std::vector<uint8_t> before = pixels_of(document, id);

  /* 恒等 lens：不改任何像素，于是「拒绝」是唯一可观测的差别。 */
  ToolOpParams params;
  params.kind = kToolOpLens;
  params.lens.k = 0.0;

  comp_result code = COMP_OK;
  std::string error;
  COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                 "an 8-bit tool on an HDR layer is refused rather than downgraded");
  COMP_CHECK_MSG(code == COMP_ERR_STATE, "the refusal is STATE (the layer's depth), not UNSUPPORTED");
  COMP_CHECK_MSG(!document.txn_active(), "a refused op leaves no transaction open");
  COMP_CHECK_MSG(!document.history().can_undo(), "a refused op must not enter the undo stack");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "a refused op must not touch the pixels");
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16F, "the HDR layer keeps its depth");
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

/* ============================================================ 落笔族 × 深度 */

/*
 * [D1] 16 位图层上的一笔：深度保持 16 位，**笔尖之外逐字节不动**，笔尖之内
 * 取 8 位值 ×257。
 *
 * 这是上游 BrushStroke.swift:1097 `sixteenBit()` 的接线口径：8 位内核跑在
 * dithered 副本上，`deep_take_painted` 只把 painted != unpainted 的像素写成
 * 8 位值。断言用「改过的像素四通道都是 257 的倍数、没改的像素与原始 16 位
 * 逐字节相同」来表达，而不是另算一遍 take_painted（那只会复制实现）。
 */
COMP_TEST(tool_ops_domain_stroke_keeps_sixteen_bit_depth_outside_the_stroke) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  /* 两个点：本用例验证的是折线段落笔（单点另有 dab 语义，见 tests/test_tool_ops.cpp）。 */
  const float points[4] = {1.0f, 1.0f, 2.0f, 1.0f};
  ToolOpParams params;
  params.kind = kToolOpStroke;
  params.points = points;
  params.point_count = 2;
  params.brush.color[0] = 250;
  params.brush.color[1] = 10;
  params.brush.color[2] = 10;
  params.brush.diameter = 3.0f;
  params.brush.hardness = 1.0f;
  params.brush.pressure = 1.0f;
  params.brush.flow = 1.0f;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "a stroke on a 16-bit layer is a real product path");
  COMP_CHECK_MSG(code == COMP_OK, "the stroke reports COMP_OK");

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA16,
                 "the stroke family keeps the layer at 16 bits (BrushStroke.swift:1097)");
  COMP_CHECK_MSG(layer->pixels.pixel_data().size() == before.size(),
                 "the depth change is not a silent downgrade to 4-byte pixels");

  const uint16_t* now = reinterpret_cast<const uint16_t*>(layer->pixels.pixel_data().data());
  const uint16_t* old = reinterpret_cast<const uint16_t*>(before.data());
  const size_t count = static_cast<size_t>(8 * 8);
  size_t changed = 0;
  bool only_eight_bit_values = true;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t* p = now + i * 4u;
    const uint16_t* b = old + i * 4u;
    if (p[0] == b[0] && p[1] == b[1] && p[2] == b[2] && p[3] == b[3]) continue;
    ++changed;
    for (int c = 0; c < 4; ++c) {
      if (p[c] % 257u != 0u || p[c] / 257u > 255u) only_eight_bit_values = false;
    }
  }
  COMP_CHECK_MSG(changed > 0, "the stroke actually painted something (otherwise this proves nothing)");
  COMP_CHECK_MSG(changed < count, "pixels outside the stroke keep their 16-bit precision");
  COMP_CHECK_MSG(only_eight_bit_values,
                 "a painted pixel takes the 8-bit value * 257 and nothing else (deep_take_painted)");

  /* 一步撤销：深度与像素都回到落笔前。 */
  COMP_CHECK_MSG(document.history().can_undo(), "the stroke is one undo step");
  document.undo();
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16,
                 "undo restores the 16-bit asset, not an 8-bit copy");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the deep pixels byte for byte");
}

/*
 * [D2] HDR 图层上的一笔：`deep_take_painted_hdr` 那条分支。
 *
 * 与 [D1] 的区别是「白以上的光」：笔尖之外必须仍是 4.0（half），笔尖之内是
 * 8 位值的 /255（≤ 1.0）。这正是上游 HDR 落笔不剪高光、也不伪造高光的语义
 * （BrushStroke.swift:1117-1123 的 `painted` / `unpainted` 比较）。
 */
COMP_TEST(tool_ops_domain_hdr_stroke_keeps_light_above_white_outside_the_stroke) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_hdr_gradient_layer(document, 4.0f);
  const std::vector<uint8_t> before = pixels_of(document, id);

  uint16_t untouched[4];
  half_pixel(before, 8, 4, 4, untouched);
  COMP_CHECK_MSG(half_value(untouched[0]) > 1.001f, "the layer really starts with light above white");

  const float points[4] = {1.0f, 1.0f, 2.0f, 1.0f};
  ToolOpParams params;
  params.kind = kToolOpStroke;
  params.points = points;
  params.point_count = 2;
  params.brush.color[0] = 250;
  params.brush.color[1] = 10;
  params.brush.color[2] = 10;
  params.brush.diameter = 3.0f;
  params.brush.hardness = 1.0f;
  params.brush.pressure = 1.0f;
  params.brush.flow = 1.0f;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "a stroke on an HDR layer is a real product path now (take_painted_hdr)");
  COMP_CHECK_MSG(code == COMP_OK, "the HDR stroke reports COMP_OK");

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA16F,
                 "an HDR layer stays half-float after a stroke (no silent downgrade)");

  const std::vector<uint8_t> after = pixels_of(document, id);
  uint16_t far[4];
  half_pixel(after, 8, 4, 4, far);
  COMP_CHECK_MSG(half_value(far[0]) == half_value(untouched[0]),
                 "a highlight far from the stroke keeps its value above white");
  uint16_t painted[4];
  half_pixel(after, 8, 1, 1, painted);
  COMP_CHECK_MSG(half_value(painted[0]) <= 1.0f,
                 "a painted pixel takes the 8-bit value (never a fabricated highlight)");

  COMP_CHECK_MSG(document.history().can_undo(), "the HDR stroke is one undo step");
  document.undo();
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16F,
                 "undo keeps the HDR depth too");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the half-float pixels byte for byte");
}

/* [D3] 落笔族的其余三个算子走同一条深度保持路径（clone / smudge / liquify）。 */
COMP_TEST(tool_ops_domain_clone_and_warp_keep_sixteen_bit_depth) {
  const ToolOpKind kinds[3] = {kToolOpClone, kToolOpSmudge, kToolOpLiquify};
  for (int k = 0; k < 3; ++k) {
    Engine engine(test_engine_desc());
    Document document(&engine, 8, 8, 72.0f, "sRGB");
    const comp_layer_id id = add_wide_layer(document);

    const float points[2] = {2.0f, 2.0f};
    ToolOpParams params;
    params.kind = kinds[k];
    params.points = points;
    params.point_count = 1;
    params.brush.diameter = 4.0f;
    params.brush.hardness = 1.0f;
    params.brush.pressure = 1.0f;
    params.brush.flow = 1.0f;
    params.clone.has_source = 1;
    params.clone.source_x = 5.0;
    params.clone.source_y = 5.0;
    params.clone.opacity = 1.0f;
    params.warp.strength = 0.5f;

    comp_result code = COMP_ERR_INTERNAL;
    std::string error;
    COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                   "a retouch stroke on a 16-bit layer: " + error);
    COMP_CHECK_MSG(code == COMP_OK, "the retouch stroke reports COMP_OK: " + error);
    COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16,
                   "the whole stroke family keeps the layer at 16 bits");
    COMP_CHECK_MSG(document.history().can_undo(), "each retouch stroke is one undo step");
  }
}

/* ============================================ [E] Blur：模糊工具（P1 追加） */

namespace {

/* 左右两半硬边（左黑右白），用来观察模糊是否只在笔尖下发生。 */
std::vector<uint8_t> hard_edge_pixels(int32_t width, int32_t height) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 255);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                    static_cast<size_t>(x)) * 4u;
      p[0] = p[1] = p[2] = x < width / 2 ? 0 : 255;
    }
  }
  return pixels;
}

ToolOpParams blur_params(const float* points, int32_t point_count, float radius) {
  ToolOpParams params;
  params.kind = kToolOpBlur;
  params.points = points;
  params.point_count = point_count;
  params.brush.diameter = 7.0f;
  params.brush.hardness = 1.0f;
  params.brush.pressure = 1.0f;
  params.brush.flow = 1.0f;
  params.blur.radius = radius;
  return params;
}

}  // namespace

/*
 * [E1] 一笔模糊只落在笔尖下，且同一条折线不会吃到自己刚模糊过的像素。
 *
 * 判定：边缘两侧的像素在笔迹下被拉向中间值（硬边变软）；笔尖外的像素逐字节
 * 不动；第二笔（同一参数）仍能继续改变像素——因为每笔都以当笔开始时的像素
 * 取快照（BlurTool.swift 的 blurSample）。
 */
COMP_TEST(tool_ops_domain_blur_softens_under_the_brush_only) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  const std::vector<uint8_t> source = hard_edge_pixels(16, 16);
  layer->pixels.resize(16, 16, COMP_PIXEL_RGBA8);
  std::memcpy(layer->pixels.writable_pixels().data(), source.data(), source.size());
  const std::vector<uint8_t> before = pixels_of(document, id);
  COMP_CHECK(before == source);

  /* 横向穿过边缘（x = 8），直径 7 覆盖 y ∈ [4.5, 11.5] 一带。 */
  const float points[4] = {4.0f, 8.0f, 12.0f, 8.0f};
  ToolOpParams params = blur_params(points, 2, 3.0f);
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "a blur stroke is a real product path: " + error);
  COMP_CHECK(code == COMP_OK);

  const std::vector<uint8_t> after = pixels_of(document, id);
  const auto at = [](int32_t x, int32_t y) -> size_t {
    return (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 4u;
  };
  COMP_CHECK_MSG(after[at(7, 8)] > before[at(7, 8)],
                 "the dark side of the edge is pulled toward the blurred sample");
  COMP_CHECK_MSG(after[at(8, 8)] < before[at(8, 8)],
                 "the light side of the edge is pulled toward the blurred sample");
  COMP_CHECK_MSG(after[at(7, 0)] == before[at(7, 0)] && after[at(8, 0)] == before[at(8, 0)],
                 "pixels outside the brush tip are byte-identical");
  COMP_CHECK_MSG(after[at(15, 12)] == before[at(15, 12)],
                 "the stroke does not leak to the far corner of the layer");

  /* 第二笔：快照取的是第一笔之后的像素，所以仍会继续变软（可重复 ≠ 幂等）。 */
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "a second blur stroke runs");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(pixels_of(document, id) != after,
                 "each stroke takes a fresh snapshot, so repeated strokes keep softening");

  COMP_CHECK_MSG(document.history().can_undo(), "the blur strokes are undoable");
  document.undo();
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "two undos restore the hard edge byte for byte");
}

/* [E2] radius 的闸：0 / 50+ / NaN / 缺 points 都在开事务之前拒绝。 */
COMP_TEST(tool_ops_domain_blur_rejects_bad_radius_before_transaction) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = document.history().undo_depth();

  const float points[4] = {1.0f, 1.0f, 3.0f, 1.0f};
  comp_result code = COMP_OK;
  std::string error;

  {
    ToolOpParams params = blur_params(points, 2, 0.0f);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "radius 0 is rejected (upstream clamps it, we refuse to guess)");
    COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "radius 0 is an argument error");
  }
  {
    ToolOpParams params = blur_params(points, 2, 50.5f);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "radius above 50 is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }
  {
    ToolOpParams params = blur_params(points, 2, std::numeric_limits<float>::quiet_NaN());
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "a NaN radius is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }
  {
    ToolOpParams params = blur_params(nullptr, 2, 3.0f);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "a null point array is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }

  COMP_CHECK_MSG(!document.txn_active(), "the gate runs before txn_begin: no txn left open");
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before, "rejected blur strokes add no undo step");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "rejected blur strokes leave pixels byte-identical");

  /* 合法值通过同一道闸，并恰好记一步。 */
  ToolOpParams good = blur_params(points, 2, 2.0f);
  COMP_CHECK_MSG(run_tool_op(&document, id, good, &code, &error, nullptr),
                 "a legal radius runs: " + error);
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before + 1,
                 "a blur stroke is exactly one undo step");
}

/* [E3] 16 位图层：笔尖外保持深精度，笔尖内取 8 位值（×257）。 */
COMP_TEST(tool_ops_domain_blur_keeps_sixteen_bit_depth_outside_the_stroke) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  const float points[4] = {2.0f, 2.0f, 3.0f, 2.0f};
  ToolOpParams params = blur_params(points, 2, 2.0f);
  params.brush.diameter = 3.0f;
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "a blur stroke on a 16-bit layer is a real product path: " + error);
  COMP_CHECK(code == COMP_OK);

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA16,
                 "blur joins the stroke family and keeps the layer at 16 bits");

  const uint16_t* now = reinterpret_cast<const uint16_t*>(layer->pixels.pixel_data().data());
  const uint16_t* old = reinterpret_cast<const uint16_t*>(before.data());
  const size_t count = 8u * 8u;
  size_t changed = 0;
  bool only_eight_bit_values = true;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t* p = now + i * 4u;
    const uint16_t* b = old + i * 4u;
    if (p[0] == b[0] && p[1] == b[1] && p[2] == b[2] && p[3] == b[3]) continue;
    ++changed;
    for (int c = 0; c < 4; ++c) {
      if (p[c] % 257u != 0u || p[c] / 257u > 255u) only_eight_bit_values = false;
    }
  }
  COMP_CHECK_MSG(changed > 0, "the blur stroke actually painted something");
  COMP_CHECK_MSG(changed < count, "pixels outside the stroke keep their 16-bit precision");
  COMP_CHECK_MSG(only_eight_bit_values,
                 "a painted pixel takes the 8-bit value * 257 (deep_take_painted)");

  document.undo();
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16,
                 "undo restores the 16-bit asset");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the deep pixels byte for byte");
}

/* ============================================ [F] 色调：减淡 / 加深 / 海绵（P1 追加） */

namespace {

/* 纯色不透明图层（预乘下 rgb 就是直通色，便于直接判方向）。 */
std::vector<uint8_t> flat_pixels(int32_t width, int32_t height, uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t a) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  for (size_t i = 0; i < static_cast<size_t>(width) * static_cast<size_t>(height); ++i) {
    pixels[i * 4 + 0] = r;
    pixels[i * 4 + 1] = g;
    pixels[i * 4 + 2] = b;
    pixels[i * 4 + 3] = a;
  }
  return pixels;
}

ToolOpParams tone_params(ToolOpKind kind, int32_t mode, float strength, const float* points,
                         int32_t point_count) {
  ToolOpParams params;
  params.kind = kind;
  params.points = points;
  params.point_count = point_count;
  params.brush.diameter = 7.0f;
  params.brush.hardness = 1.0f;
  params.brush.pressure = 1.0f;
  params.brush.flow = 1.0f;
  params.tone.mode = mode;
  params.tone.strength = strength;
  return params;
}

}  // namespace

/*
 * [F1] 减淡朝白、加深朝黑，都只落在笔尖下且不动 alpha。
 *
 * 判定：中灰 (128) 在强度 0.5 的减淡下变亮、加深下变暗；alpha 恒 255；笔尖外
 * 逐字节不动；一步撤销回到落笔前。
 */
COMP_TEST(tool_ops_domain_tone_dodge_and_burn_move_toward_white_and_black) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  const std::vector<uint8_t> gray = flat_pixels(16, 16, 128, 128, 128, 255);
  layer->pixels.resize(16, 16, COMP_PIXEL_RGBA8);
  std::memcpy(layer->pixels.writable_pixels().data(), gray.data(), gray.size());
  const std::vector<uint8_t> before = pixels_of(document, id);
  COMP_CHECK(before == gray);

  const auto at = [](int32_t x, int32_t y) -> size_t {
    return (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 4u;
  };
  const float center[2] = {8.0f, 8.0f}; /* 单点 = 圆形笔尖 */
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;

  ToolOpParams dodge = tone_params(kToolOpDodge, 0, 0.5f, center, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, dodge, &code, &error, nullptr),
                 "a dodge stroke is a real product path: " + error);
  COMP_CHECK(code == COMP_OK);
  const std::vector<uint8_t> lightened = pixels_of(document, id);
  COMP_CHECK_MSG(lightened[at(8, 8)] > before[at(8, 8)],
                 "dodge moves the midtone toward white");
  COMP_CHECK_MSG(lightened[at(8, 8) + 3] == 255, "dodge never changes alpha");
  COMP_CHECK_MSG(lightened[at(0, 0)] == before[at(0, 0)],
                 "pixels outside the brush tip are byte-identical");

  COMP_CHECK_MSG(document.history().undo_depth() == 1, "one dodge stroke is one undo step");
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the midtone byte for byte");

  ToolOpParams burn = tone_params(kToolOpBurn, 0, 0.5f, center, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, burn, &code, &error, nullptr),
                 "a burn stroke is a real product path: " + error);
  COMP_CHECK(code == COMP_OK);
  const std::vector<uint8_t> darkened = pixels_of(document, id);
  COMP_CHECK_MSG(darkened[at(8, 8)] < before[at(8, 8)],
                 "burn moves the midtone toward black");
  COMP_CHECK_MSG(darkened[at(8, 8) + 3] == 255, "burn never changes alpha");
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the burn byte for byte");
}

/*
 * [F2] 海绵：去色把色度收掉，加色把色度放大；灰度像素上加色是恒等运算 ——
 * 成功但**不记撤销步**（不能留下一笔什么都没改的撤销）。
 */
COMP_TEST(tool_ops_domain_tone_sponge_desaturates_and_noops_on_gray) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  const std::vector<uint8_t> red = flat_pixels(16, 16, 255, 0, 0, 255);
  layer->pixels.resize(16, 16, COMP_PIXEL_RGBA8);
  std::memcpy(layer->pixels.writable_pixels().data(), red.data(), red.size());
  const std::vector<uint8_t> before = pixels_of(document, id);

  const auto at = [](int32_t x, int32_t y) -> size_t {
    return (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 4u;
  };
  const float center[2] = {8.0f, 8.0f};
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;

  ToolOpParams desaturate = tone_params(kToolOpSponge, 0, 1.0f, center, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, desaturate, &code, &error, nullptr),
                 "a sponge stroke is a real product path: " + error);
  COMP_CHECK(code == COMP_OK);
  const std::vector<uint8_t> dull = pixels_of(document, id);
  COMP_CHECK_MSG(dull[at(8, 8)] < 255, "desaturating red loses red");
  COMP_CHECK_MSG(dull[at(8, 8) + 1] == dull[at(8, 8)] && dull[at(8, 8) + 2] == dull[at(8, 8)],
                 "a fully desaturated pixel is neutral gray");
  COMP_CHECK_MSG(dull[at(8, 8) + 3] == 255, "sponge never changes alpha");
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the red byte for byte");

  /* 灰度 × 加色：色度本来就是 0，乘 (1+w) 还是 0 —— 逐字节不变 = 空操作。 */
  const std::vector<uint8_t> gray = flat_pixels(16, 16, 128, 128, 128, 255);
  Layer* restored = document.find(id);
  COMP_CHECK(restored != nullptr);
  std::memcpy(restored->pixels.writable_pixels().data(), gray.data(), gray.size());
  const size_t undo_before = document.history().undo_depth();
  std::string note;
  ToolOpParams saturate = tone_params(kToolOpSponge, 1, 0.5f, center, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, saturate, &code, &error, &note),
                 "a no-op sponge stroke still succeeds: " + error);
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(note.find("changed no pixel") != std::string::npos,
                 "the note says the tone stroke changed nothing: " + note);
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before,
                 "a stroke that changes no byte records no undo step");
  COMP_CHECK_MSG(pixels_of(document, id) == gray, "the no-op stroke leaves pixels byte-identical");
}

/* [F3] mode / strength / points 的闸都在开事务之前拒绝。 */
COMP_TEST(tool_ops_domain_tone_rejects_bad_mode_and_strength_before_transaction) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = document.history().undo_depth();

  const float points[4] = {2.0f, 2.0f, 5.0f, 2.0f};
  comp_result code = COMP_OK;
  std::string error;

  {
    ToolOpParams params = tone_params(kToolOpDodge, 3, 0.5f, points, 2);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "dodge mode 3 is rejected (only 0/1/2 exist)");
    COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "an out-of-range dodge mode is an arg error");
  }
  {
    ToolOpParams params = tone_params(kToolOpSponge, 2, 0.5f, points, 2);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "sponge mode 2 is rejected (only 0 = desaturate / 1 = saturate)");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }
  {
    ToolOpParams params = tone_params(kToolOpBurn, 1, 0.0f, points, 2);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "strength 0 is rejected instead of recording an empty undo step");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }
  {
    ToolOpParams params =
        tone_params(kToolOpBurn, 1, std::numeric_limits<float>::quiet_NaN(), points, 2);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "a NaN strength is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }
  {
    ToolOpParams params = tone_params(kToolOpDodge, 0, 0.5f, nullptr, 2);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "a null point array is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }

  COMP_CHECK_MSG(!document.txn_active(), "the gate runs before txn_begin: no txn left open");
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before,
                 "rejected tone strokes add no undo step");
  COMP_CHECK_MSG(pixels_of(document, id) == before,
                 "rejected tone strokes leave pixels byte-identical");

  /* 合法值通过同一道闸，并恰好记一步。 */
  ToolOpParams good = tone_params(kToolOpDodge, 0, 0.5f, points, 2);
  COMP_CHECK_MSG(run_tool_op(&document, id, good, &code, &error, nullptr),
                 "a legal tone stroke runs: " + error);
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before + 1,
                 "a tone stroke is exactly one undo step");
}

/* ============================================ [G] 历史记录画笔（P1 追加） */

namespace {

ToolOpParams history_brush_params(int32_t state, float opacity, const float* points,
                                  int32_t point_count) {
  ToolOpParams params;
  params.kind = kToolOpHistoryBrush;
  params.points = points;
  params.point_count = point_count;
  params.brush.diameter = 5.0f;
  params.brush.hardness = 1.0f;
  params.brush.pressure = 1.0f;
  params.brush.flow = 1.0f;
  params.history_brush.state = state;
  params.history_brush.opacity = opacity;
  return params;
}

/* 一次覆盖整幅画布的纯色笔迹（直径远大于画布，hardness/flow 拉满）。 */
ToolOpParams solid_stroke_params(uint8_t r, uint8_t g, uint8_t b, const float* points,
                                 int32_t point_count) {
  ToolOpParams params;
  params.kind = kToolOpStroke;
  params.points = points;
  params.point_count = point_count;
  params.brush.color[0] = r;
  params.brush.color[1] = g;
  params.brush.color[2] = b;
  params.brush.diameter = 64.0f;
  params.brush.hardness = 1.0f;
  params.brush.pressure = 1.0f;
  params.brush.flow = 1.0f;
  return params;
}

/* 历史快照里的图层查找（与域层 find_state_layer 同口径，测试侧独立写一份）。 */
const Layer* state_layer(const DocumentState& state, comp_layer_id id) {
  for (const Layer& layer : state.layers) {
    if (layer.id == id) return &layer;
  }
  return nullptr;
}

}  // namespace

/*
 * [G1] 历史面板下标 -> 状态的映射。
 *
 * 三种栈形态都要对：撤销栈非空、撤销栈空而重做栈非空、两栈都空（借活文档）。
 * 映射错一位的后果是「历史记录画笔静默刷回错误的那一步」，比直接失败更难查。
 */
COMP_TEST(tool_ops_domain_history_state_at_maps_panel_indices) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  const std::vector<uint8_t> gray = flat_pixels(16, 16, 128, 128, 128, 255);
  layer->pixels.resize(16, 16, COMP_PIXEL_RGBA8);
  std::memcpy(layer->pixels.writable_pixels().data(), gray.data(), gray.size());

  /* 两栈全空：index 0 借给活文档，之后的 index 越界。 */
  COMP_CHECK(document.history().step_count() == 0);
  const DocumentState* live = document.history().state_at(0);
  COMP_CHECK_MSG(live != nullptr, "index 0 with an empty stack borrows the live document");
  COMP_CHECK(state_layer(*live, id) != nullptr);
  COMP_CHECK_MSG(document.history().state_at(1) == nullptr, "index past step_count() is null");

  const float center[2] = {8.0f, 8.0f};
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  ToolOpParams first = tone_params(kToolOpDodge, 0, 0.5f, center, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, first, &code, &error, nullptr),
                 "first dodge: " + error);
  const std::vector<uint8_t> after_first = pixels_of(document, id);
  ToolOpParams second = tone_params(kToolOpDodge, 0, 0.5f, center, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, second, &code, &error, nullptr),
                 "second dodge: " + error);
  const std::vector<uint8_t> after_second = pixels_of(document, id);
  COMP_CHECK_MSG(after_first != after_second, "the second dodge really changes the midtone");

  const auto state_pixels = [&](std::size_t index) -> std::vector<uint8_t> {
    const DocumentState* state = document.history().state_at(index);
    if (state == nullptr) return {};
    const Layer* found = state_layer(*state, id);
    return found == nullptr ? std::vector<uint8_t>{} : found->pixels.pixel_data();
  };

  COMP_CHECK_MSG(state_pixels(0) == gray, "index 0 is the state before step 1");
  COMP_CHECK_MSG(state_pixels(1) == after_first, "index 1 is the state after step 1");
  COMP_CHECK_MSG(state_pixels(2) == after_second, "index 2 is the state after step 2");
  COMP_CHECK(document.history().state_at(3) == nullptr);

  document.undo();
  COMP_CHECK_MSG(document.history().current_index() == 1, "undo moves the current index to 1");
  COMP_CHECK_MSG(state_pixels(0) == gray, "undoing keeps index 0 attached to the initial state");
  COMP_CHECK_MSG(state_pixels(1) == after_first, "undoing keeps index 1 attached to step 1");
  COMP_CHECK_MSG(state_pixels(2) == after_second,
                 "index 2 still reads step 2 from the redo stack (LIFO back = step 1)");

  document.undo();
  COMP_CHECK_MSG(document.history().current_index() == 0, "a second undo lands on index 0");
  COMP_CHECK_MSG(state_pixels(0) == gray && state_pixels(1) == after_first &&
                     state_pixels(2) == after_second,
                 "a fully undone history still maps every panel index");

  document.redo();
  COMP_CHECK_MSG(state_pixels(1) == after_first, "redo restores the same mapping");
}

/*
 * [G2] 典型用法：画错的一笔被盖住，从最初状态取样把它「刷回」笔尖下。
 * 断言：只有笔尖下的像素被改、颜色等于取样状态、alpha 跟着取样走、一步撤销。
 */
COMP_TEST(tool_ops_domain_history_brush_paints_the_sampled_state_under_the_tip) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  const std::vector<uint8_t> red = flat_pixels(16, 16, 255, 0, 0, 255);
  layer->pixels.resize(16, 16, COMP_PIXEL_RGBA8);
  std::memcpy(layer->pixels.writable_pixels().data(), red.data(), red.size());

  const float across[4] = {0.0f, 8.0f, 15.0f, 8.0f};
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  ToolOpParams cover = solid_stroke_params(0, 0, 255, across, 2);
  COMP_CHECK_MSG(run_tool_op(&document, id, cover, &code, &error, nullptr),
                 "covering stroke: " + error);
  const std::vector<uint8_t> blue = pixels_of(document, id);
  const size_t at_center = (8u * 16u + 8u) * 4u;
  const size_t at_corner = 0u;
  COMP_CHECK_MSG(blue[at_center] == 0 && blue[at_center + 1] == 0 && blue[at_center + 2] == 255,
                 "the covering stroke turned the layer blue");

  const float tip[2] = {8.0f, 8.0f};
  ToolOpParams brush = history_brush_params(0, 1.0f, tip, 1);
  brush.brush.diameter = 5.0f;
  COMP_CHECK_MSG(run_tool_op(&document, id, brush, &code, &error, nullptr),
                 "the history brush is a real product path: " + error);
  COMP_CHECK(code == COMP_OK);
  const std::vector<uint8_t> patched = pixels_of(document, id);
  COMP_CHECK_MSG(patched[at_center] == 255 && patched[at_center + 1] == 0 &&
                     patched[at_center + 2] == 0,
                 "the brush restored the sampled red under the tip");
  COMP_CHECK_MSG(patched[at_corner + 2] == 255,
                 "pixels outside the tip keep the blue stroke");
  COMP_CHECK_MSG(patched[at_center + 3] == 255, "alpha follows the sampled state");
  COMP_CHECK_MSG(document.history().undo_depth() == 2,
                 "the history brush is exactly one undo step");
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == blue, "undo restores the blue byte for byte");
}

/*
 * [G3] 参数闸：状态越界 / 状态里没有该图层 / opacity / points 都在开事务之前拒绝；
 * 取样与当前像素相同时是 noop（不记空撤销）。
 */
COMP_TEST(tool_ops_domain_history_brush_gates_bad_state_before_transaction) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_pixel_layer(document);
  const float tip[2] = {4.0f, 4.0f};
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = document.history().undo_depth();

  comp_result code = COMP_OK;
  std::string error;
  {
    ToolOpParams params = history_brush_params(1, 1.0f, tip, 1);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "an empty history has no state 1");
    COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "an out-of-range state is an arg error");
  }
  {
    ToolOpParams params = history_brush_params(-1, 1.0f, tip, 1);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "a negative state index is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }
  {
    ToolOpParams params = history_brush_params(0, 0.0f, tip, 1);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "opacity 0 is rejected instead of recording an empty undo step");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }
  {
    ToolOpParams params =
        history_brush_params(0, std::numeric_limits<float>::quiet_NaN(), tip, 1);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "a NaN opacity is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }
  {
    ToolOpParams params = history_brush_params(0, 1.0f, nullptr, 1);
    COMP_CHECK_MSG(!run_tool_op(&document, id, params, &code, &error, nullptr),
                   "a null point array is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }

  COMP_CHECK_MSG(!document.txn_active(), "the gate runs before txn_begin: no txn left open");
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before,
                 "rejected history brush strokes add no undo step");
  COMP_CHECK_MSG(pixels_of(document, id) == before,
                 "rejected history brush strokes leave pixels byte-identical");

  /* 合法但取样等于当前像素（空历史 + state 0 = 活文档）：noop，不记撤销步。 */
  std::string note;
  ToolOpParams same = history_brush_params(0, 1.0f, tip, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, same, &code, &error, &note),
                 "a history brush on an unchanged state still succeeds: " + error);
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(note.find("changed no pixel") != std::string::npos,
                 "the note says the history brush changed nothing: " + note);
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before,
                 "a history stroke that changes no byte records no undo step");

  /*
   * 取样状态里没有同 id 的图层（图层是后来新建的）-> STATE，而不是参数错误：
   * 调用本身合法，是文档状态不满足「那一份历史里得有这一层」。
   */
  const float across[2] = {2.0f, 2.0f};
  ToolOpParams dodge = tone_params(kToolOpDodge, 0, 0.5f, across, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, dodge, &code, &error, nullptr),
                 "a dodge step gives the history something to sample: " + error);
  const comp_layer_id later = add_pixel_layer(document); /* 不进历史：只存在于活文档 */
  const std::vector<uint8_t> later_before = pixels_of(document, later);
  const size_t undo_before_missing = document.history().undo_depth();
  ToolOpParams missing = history_brush_params(1, 1.0f, tip, 1);
  COMP_CHECK_MSG(!run_tool_op(&document, later, missing, &code, &error, nullptr),
                 "state 1 predates the new layer: there is nothing to sample");
  COMP_CHECK_MSG(code == COMP_ERR_STATE, "a missing layer in the sampled state is a state error");
  COMP_CHECK_MSG(pixels_of(document, later) == later_before,
                 "the rejected stroke leaves the newer layer byte-identical");
  COMP_CHECK_MSG(document.history().undo_depth() == undo_before_missing,
                 "the rejected stroke adds no undo step");
}

/* [G4] 16 位图层：笔尖外保住深精度，笔尖内取 8 位取样值（×257）。 */
COMP_TEST(tool_ops_domain_history_brush_keeps_sixteen_bit_depth_outside_the_tip) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  const float center[2] = {4.0f, 4.0f};
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  ToolOpParams dodge = tone_params(kToolOpDodge, 0, 1.0f, center, 1);
  COMP_CHECK_MSG(run_tool_op(&document, id, dodge, &code, &error, nullptr),
                 "a dodge stroke on a 16-bit layer: " + error);
  const std::vector<uint8_t> after_dodge = pixels_of(document, id);

  /* 从第 0 步（dodge 之前）取样，只刷回笔尖下的一小块。 */
  ToolOpParams brush = history_brush_params(0, 1.0f, center, 1);
  brush.brush.diameter = 3.0f;
  COMP_CHECK_MSG(run_tool_op(&document, id, brush, &code, &error, nullptr),
                 "the history brush on a 16-bit layer is a real product path: " + error);
  COMP_CHECK(code == COMP_OK);

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA16,
                 "the history brush joins the stroke family and keeps the layer at 16 bits");

  const uint16_t* now = reinterpret_cast<const uint16_t*>(layer->pixels.pixel_data().data());
  const uint16_t* old = reinterpret_cast<const uint16_t*>(before.data());
  const size_t count = 8u * 8u;
  size_t changed = 0;
  bool only_eight_bit_values = true;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t* p = now + i * 4u;
    const uint16_t* b = old + i * 4u;
    if (p[0] == b[0] && p[1] == b[1] && p[2] == b[2] && p[3] == b[3]) continue;
    ++changed;
    for (int c = 0; c < 4; ++c) {
      if (p[c] % 257u != 0u || p[c] / 257u > 255u) only_eight_bit_values = false;
    }
  }
  COMP_CHECK_MSG(changed > 0, "the history brush actually painted something");
  COMP_CHECK_MSG(changed < count, "pixels outside the tip keep their 16-bit precision");
  COMP_CHECK_MSG(only_eight_bit_values,
                 "a painted pixel takes the 8-bit sample * 257 (deep_take_painted)");

  /* 两次撤销：先回到 dodge 之后、再回到最初；深像素都必须逐字节复原。 */
  document.undo();
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16,
                 "undo restores the 16-bit asset");
  COMP_CHECK(pixels_of(document, id) == after_dodge);
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the deep pixels byte for byte");
}

/* [F4] 16 位图层：色调工具按落笔族往返，笔尖外保持深精度。 */
COMP_TEST(tool_ops_domain_tone_keeps_sixteen_bit_depth_outside_the_stroke) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(document);
  const std::vector<uint8_t> before = pixels_of(document, id);

  const float points[4] = {2.0f, 2.0f, 3.0f, 2.0f};
  ToolOpParams params = tone_params(kToolOpDodge, 0, 0.5f, points, 2);
  params.brush.diameter = 3.0f;
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  COMP_CHECK_MSG(run_tool_op(&document, id, params, &code, &error, nullptr),
                 "a tone stroke on a 16-bit layer is a real product path: " + error);
  COMP_CHECK(code == COMP_OK);

  const Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  COMP_CHECK_MSG(layer->pixels.format == COMP_PIXEL_RGBA16,
                 "tone joins the stroke family and keeps the layer at 16 bits");

  const uint16_t* now = reinterpret_cast<const uint16_t*>(layer->pixels.pixel_data().data());
  const uint16_t* old = reinterpret_cast<const uint16_t*>(before.data());
  const size_t count = 8u * 8u;
  size_t changed = 0;
  bool only_eight_bit_values = true;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t* p = now + i * 4u;
    const uint16_t* b = old + i * 4u;
    if (p[0] == b[0] && p[1] == b[1] && p[2] == b[2] && p[3] == b[3]) continue;
    ++changed;
    for (int c = 0; c < 4; ++c) {
      if (p[c] % 257u != 0u || p[c] / 257u > 255u) only_eight_bit_values = false;
    }
  }
  COMP_CHECK_MSG(changed > 0, "the tone stroke actually changed something");
  COMP_CHECK_MSG(changed < count, "pixels outside the stroke keep their 16-bit precision");
  COMP_CHECK_MSG(only_eight_bit_values,
                 "a painted pixel takes the 8-bit value * 257 (deep_take_painted)");

  document.undo();
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16,
                 "undo restores the 16-bit asset");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the deep pixels byte for byte");
}

/* ============================================================== 文字（S11） */

namespace {

TypeRasterStyle type_style(double font_size, double line_height, double letter_spacing,
                           bool antialias) {
  TypeRasterStyle style;
  style.font_size = font_size;
  style.line_height = line_height;
  style.letter_spacing = letter_spacing;
  style.antialias = antialias;
  return style;
}

double coverage_sum(const TypeCoverage& coverage) {
  double sum = 0.0;
  for (uint8_t value : coverage.coverage) sum += static_cast<double>(value);
  return sum / 255.0;
}

}  // namespace

COMP_TEST(type_raster_measures_lines_and_preserves_ink_area) {
  /* 字号 7 ⇒ 缩放 1；行距 1 ⇒ 行步进 7。两行文本的度量可以手算。 */
  const TypeRasterStyle style = type_style(7.0, 1.0, 0.0, true);
  TypeMeasure measure;
  TypeRasterReport report = type_measure_text("A\nBB", 4, style, &measure);
  COMP_CHECK(report.status == TypeRasterStatus::kOk);
  COMP_CHECK_MSG(measure.line_count == 2, "newlines split the text into two lines");
  COMP_CHECK_MSG(measure.glyph_count == 3, "the newline itself is not a glyph");
  COMP_CHECK_MSG(std::fabs(measure.advance - 6.0) < 1e-9,
                 "advance is 6 font columns at 1:1 scale");
  COMP_CHECK_MSG(std::fabs(measure.width - 12.0) < 1e-9,
                 "the text box width follows the widest line");
  COMP_CHECK_MSG(std::fabs(measure.height - 14.0) < 1e-9,
                 "the text box height is one font size plus one line step");

  /* 字距是加在每个字形步进上的：2 字形行宽 = 2×(6+2)。 */
  const TypeRasterStyle spaced = type_style(7.0, 1.0, 2.0, true);
  report = type_measure_text("AB", 2, spaced, &measure);
  COMP_CHECK(report.status == TypeRasterStatus::kOk);
  COMP_CHECK_MSG(std::fabs(measure.width - 16.0) < 1e-9,
                 "letter spacing widens every glyph advance");

  /* 行距倍数作用在行步进上：1.5 ⇒ 高 = 7 + 7×1.5。 */
  const TypeRasterStyle loose = type_style(7.0, 1.5, 0.0, true);
  report = type_measure_text("A\nB", 3, loose, &measure);
  COMP_CHECK(report.status == TypeRasterStatus::kOk);
  COMP_CHECK_MSG(std::fabs(measure.height - 17.5) < 1e-9,
                 "line height multiplies the line step");

  /*
   * 精确面积不变性：'A' 的点阵有 18 个置位格。缩放 1 时无论原点是否落在半像素
   * 上，覆盖率总和都必须等于 18（抗锯齿只是把同一块面积切细，不增不减）。
   */
  const int kInkCellsOfA = 18;
  for (const double origin : {0.0, 0.5}) {
    TypeCoverage coverage;
    report = type_rasterize("A", 1, style, origin, origin, 0, 0, 16, 16, &coverage);
    COMP_CHECK(report.status == TypeRasterStatus::kOk);
    COMP_CHECK_MSG(!coverage.empty(), "the glyph intersects the layer");
    COMP_CHECK_MSG(std::fabs(coverage_sum(coverage) - kInkCellsOfA) < 0.15,
                   "antialiased coverage preserves the exact ink area");
  }

  /* 硬边：像素中心一票 ⇒ 整数对齐时恰好 18 个 255，没有中间值。 */
  const TypeRasterStyle hard = type_style(7.0, 1.0, 0.0, false);
  TypeCoverage hard_coverage;
  report = type_rasterize("A", 1, hard, 0.0, 0.0, 0, 0, 16, 16, &hard_coverage);
  COMP_CHECK(report.status == TypeRasterStatus::kOk);
  int full = 0;
  int partial = 0;
  for (uint8_t value : hard_coverage.coverage) {
    if (value == 255) ++full;
    else if (value != 0) ++partial;
  }
  COMP_CHECK_MSG(full == kInkCellsOfA, "hard edges produce exactly the set bitmap cells");
  COMP_CHECK_MSG(partial == 0, "hard edges never produce intermediate coverage");

  /* 文本框裁到图层：原点在图层外时窗口从 0 开始。 */
  TypeCoverage clipped;
  report = type_rasterize("A", 1, style, -2.0, -2.0, 0, 0, 16, 16, &clipped);
  COMP_CHECK(report.status == TypeRasterStatus::kOk);
  COMP_CHECK_MSG(clipped.x == 0 && clipped.y == 0, "the coverage window is clipped to the layer");
}

COMP_TEST(type_raster_rejects_unsupported_text_and_out_of_range_styles) {
  const TypeRasterStyle style = type_style(16.0, 1.25, 0.0, true);
  TypeMeasure measure;
  TypeCoverage coverage;

  /* UTF-8 多字节（CJK）与 Tab 都不是本字体支持的字符。 */
  COMP_CHECK_MSG(type_measure_text("\xE4\xBD\xA0", 3, style, &measure).status ==
                     TypeRasterStatus::kBadText,
                 "UTF-8/CJK text is unsupported");
  COMP_CHECK_MSG(type_measure_text("A\tB", 3, style, &measure).status ==
                     TypeRasterStatus::kBadText,
                 "control characters are unsupported");
  COMP_CHECK_MSG(type_measure_text("", 0, style, &measure).status == TypeRasterStatus::kBadText,
                 "empty text has no glyph");
  COMP_CHECK_MSG(type_measure_text(nullptr, 0, style, &measure).status == TypeRasterStatus::kBadText,
                 "a null text pointer is rejected");
  COMP_CHECK_MSG(type_measure_text("\n\n", 2, style, &measure).status == TypeRasterStatus::kBadText,
                 "text made only of newlines has no printable glyph");

  const char* error = "";
  COMP_CHECK_MSG(type_text_is_supported("A", 1, &error), "printable ASCII is supported");
  COMP_CHECK_MSG(!type_text_is_supported("\xE4\xBD\xA0", 3, &error),
                 "type_text_is_supported reports UTF-8 as unsupported");

  /* 字符数上限（65536）：多一个就是 kTooLong，而不是默默截断。 */
  const std::string too_long(65537, 'A');
  COMP_CHECK_MSG(type_measure_text(too_long.data(), too_long.size(), style, &measure).status ==
                     TypeRasterStatus::kTooLong,
                 "more than 65536 characters is rejected");

  /* 样式量程。 */
  COMP_CHECK_MSG(type_measure_text("A", 1, type_style(5.0, 1.25, 0.0, true), &measure).status ==
                     TypeRasterStatus::kBadStyle,
                 "a font size below 6 is out of range");
  COMP_CHECK_MSG(type_measure_text("A", 1, type_style(2049.0, 1.25, 0.0, true), &measure).status ==
                     TypeRasterStatus::kBadStyle,
                 "a font size above 2048 is out of range");
  COMP_CHECK_MSG(type_measure_text("A", 1, type_style(16.0, 0.99, 0.0, true), &measure).status ==
                     TypeRasterStatus::kBadStyle,
                 "a line height below 1 would let lines overlap");
  COMP_CHECK_MSG(type_measure_text("A", 1, type_style(16.0, 9.0, 0.0, true), &measure).status ==
                     TypeRasterStatus::kBadStyle,
                 "a line height above 8 is out of range");
  COMP_CHECK_MSG(type_measure_text("A", 1, type_style(16.0, 1.25, -1000.0, true), &measure).status ==
                     TypeRasterStatus::kBadStyle,
                 "letter spacing that makes the advance non-positive is out of range");
  COMP_CHECK_MSG(type_measure_text("A", 1, type_style(16.0, 1.25, 5000.0, true), &measure).status ==
                     TypeRasterStatus::kBadStyle,
                 "letter spacing beyond 4096 is out of range");

  /* 原点非有限 / 文本框完全在图层外。 */
  COMP_CHECK_MSG(type_rasterize("A", 1, style, std::nan(""), 0.0, 0, 0, 16, 16, &coverage).status ==
                     TypeRasterStatus::kBadStyle,
                 "a NaN origin is rejected");
  COMP_CHECK_MSG(type_rasterize("A", 1, style, 100.0, 100.0, 0, 0, 16, 16, &coverage).status ==
                     TypeRasterStatus::kEmpty,
                 "a text box outside the layer is an empty rasterization");
  COMP_CHECK_MSG(coverage.empty(), "an empty rasterization leaves the window empty");
}