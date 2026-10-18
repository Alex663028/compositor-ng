// AI生成
/*
 * test_c5_half_surface.cpp — C5 验收：合成表面按文档深度携带 half。
 *
 * C5 的目标（报告 C3-hdr-float-decoder §11）：
 *   - 有 HDR 图层的文档在 **half 表面**上合成，白点以上的光在合成途中不被截断；
 *   - `blend_pixels_half` 与 8 位 `blend_pixels` 共用同一套 W3C 公式；
 *   - 调整图层在 half 表面上走 `apply_adjustments_hdr`（曝光直落 half，
 *     跨通道调整 normalize → 16 位往返 → restore）；
 *   - 剪贴栈的 coverage 在 half 表面上用 float（`extract/restore_stack_alpha` 的
 *     half 重载）；
 *   - 渲染目标接受 RGBA16F，并与 8 位目标互转；
 *   - **8 位文档的 24 种混合模式必须逐字节不变**——这是 C5 的验收底线。
 *
 * 本文件刻意同时钉「half 行为」与「8 位不变」：后者用「FilterGraph 的结果 ==
 * 直接顺序 blend_layer_into 的结果」逐字节比对，任何让 8 位路径改走 half 的
 * 改动都会立刻挂掉。
 */
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "compositing/blend.h"
#include "compositing/composite_ops.h"
#include "compositing/compositor.h"
#include "compositor/comp_layer.h"
#include "core/comp_geometry.h"
#include "domain/document.h"
#include "domain/layer.h"
#include "domain/layer_merge.h"
#include "engine.h"
#include "imaging/DeepPixels.h"
#include "render/cpu_backend.h"
#include "test_framework.h"

using namespace compositor;

namespace {

comp_engine_desc c5_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-c5-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

/* 建一个铺满画布的图层；像素缓冲按 fmt 分配（内容由调用方填）。 */
comp_layer_id add_layer(Document& doc, const char* name, comp_pixel_format fmt,
                        comp_blend_mode blend = COMP_BLEND_NORMAL, float opacity = 1.0f) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = blend;
  desc.opacity = opacity;
  desc.transform = comp_rect{0, 0, doc.state().width, doc.state().height};
  comp_layer_id id = 0;
  Layer* layer = doc.create_layer(desc, &id);
  if (layer == nullptr) return 0;
  layer->pixels.resize(doc.state().width, doc.state().height, fmt);
  return id;
}

/* 把整层填成一个预乘 half 值。 */
void set_solid_half(Document& doc, comp_layer_id id, float r, float g, float b, float a) {
  Layer* layer = doc.find(id);
  if (layer == nullptr) return;
  const uint16_t px[4] = {deep_half_bits(r), deep_half_bits(g), deep_half_bits(b),
                          deep_half_bits(a)};
  for (int32_t y = 0; y < layer->pixels.height; ++y) {
    uint8_t* row = layer->pixels.writable_row(y);
    for (int32_t x = 0; x < layer->pixels.width; ++x) {
      std::memcpy(row + static_cast<size_t>(x) * 8u, px, sizeof(px));
    }
  }
}

float half_channel(const ImageBuffer& img, int32_t x, int32_t y, int32_t channel) {
  const uint16_t* px = reinterpret_cast<const uint16_t*>(img.row(y));
  return deep_half_value(px[static_cast<size_t>(x) * 4u + static_cast<size_t>(channel)]);
}

/* 不经过 FilterGraph 的顺序合成（8 位参考实现，与 C5 之前的路径逐行同构）。 */
bool direct_composite_8bit(const Document& doc, ImageBuffer* out, std::string* error) {
  out->resize(doc.state().width, doc.state().height, COMP_PIXEL_RGBA8);
  out->clear_transparent();
  for (const Layer& layer : doc.state().layers) {
    if (!blend_layer_into(out, doc, layer, 1.0, error)) return false;
  }
  return true;
}

}  // namespace

/* 1. 表面深度由文档深度决定；16 位整数图层维持 8 位表面（C5 之前的既有口径）。 */
COMP_TEST(c5_surface_format_follows_document_depth) {
  Engine engine(c5_engine_desc());

  Document eight(&engine, 2, 2, 72.0f, "sRGB");
  comp_layer_id id8 = add_layer(eight, "rgba8", COMP_PIXEL_RGBA8);
  COMP_CHECK(id8 != 0);
  COMP_CHECK_MSG(composite_surface_format(eight) == COMP_PIXEL_RGBA8,
                 "an 8-bit document composites on an RGBA8 surface");

  Document wide(&engine, 2, 2, 72.0f, "sRGB");
  comp_layer_id id16 = add_layer(wide, "rgba16", COMP_PIXEL_RGBA16);
  COMP_CHECK(id16 != 0);
  COMP_CHECK_MSG(composite_surface_format(wide) == COMP_PIXEL_RGBA8,
                 "RGBA16 keeps the recorded 8-bit surface behavior");

  Document hdr(&engine, 2, 2, 72.0f, "sRGB");
  comp_layer_id idh = add_layer(hdr, "rgba16f", COMP_PIXEL_RGBA16F);
  COMP_CHECK(idh != 0);
  COMP_CHECK_MSG(composite_surface_format(hdr) == COMP_PIXEL_RGBA16F,
                 "any HDR layer lifts the surface to RGBA16F");
}

/* 2. half 表面保留白点以上的光；8 位显示副本截到白而不是回绕。 */
COMP_TEST(c5_half_surface_keeps_highlights_above_white) {
  Engine engine(c5_engine_desc());
  Document doc(&engine, 2, 1, 72.0f, "sRGB");
  const comp_layer_id id = add_layer(doc, "hdr", COMP_PIXEL_RGBA16F);
  COMP_CHECK(id != 0);

  Layer* layer = doc.find(id);
  COMP_CHECK(layer != nullptr);
  const uint16_t px0[4] = {deep_half_bits(2.0f), deep_half_bits(2.0f), deep_half_bits(2.0f),
                           deep_half_bits(1.0f)};
  const uint16_t px1[4] = {0x7bff, 0x7bff, 0x7bff, deep_half_bits(1.0f)}; /* half 65504 */
  std::memcpy(layer->pixels.writable_row(0), px0, sizeof(px0));
  std::memcpy(layer->pixels.writable_row(0) + 8u, px1, sizeof(px1));

  ImageBuffer canvas;
  std::string error;
  COMP_CHECK(composite_document(doc, &canvas, &error));
  COMP_CHECK_MSG(canvas.format == COMP_PIXEL_RGBA16F,
                 "an HDR document composites into a half surface");
  if (canvas.format == COMP_PIXEL_RGBA16F) {
    COMP_CHECK_MSG(std::fabs(half_channel(canvas, 0, 0, 0) - 2.0f) < 1e-3f,
                   "a premultiplied 2.0 survives compositing instead of clamping at white");
    COMP_CHECK_MSG(std::fabs(half_channel(canvas, 1, 0, 0) - 65504.0f) < 1.0f,
                   "half 65504 survives compositing intact");
  }

  ImageBuffer display;
  COMP_CHECK(hdr_to_rgba8(canvas, &display));
  if (display.width == 2 && display.height == 1) {
    const uint8_t* d0 = display.row(0);
    const uint8_t* d1 = display.row(0) + 4u;
    /* 抖动噪声可能把 255 拉低一档；关键是「饱和到白」，不是精确常数。 */
    for (int c = 0; c < 4; ++c) {
      COMP_CHECK_MSG(std::abs(static_cast<int>(d0[c]) - 255) <= 1,
                     "the 8-bit display copy clips 2.0 to white");
      COMP_CHECK_MSG(std::abs(static_cast<int>(d1[c]) - 255) <= 1,
                     "the 8-bit display copy clips 65504 to white too (no wraparound)");
    }
  }
}

/* 3. C5 验收底线：8 位文档在全部 24 种混合模式下与顺序合成逐字节一致。 */
COMP_TEST(c5_eight_bit_composition_is_byte_identical_in_all_blend_modes) {
  for (int mode = COMP_BLEND_NORMAL; mode <= COMP_BLEND_LUMINOSITY; ++mode) {
    Engine engine(c5_engine_desc());
    Document doc(&engine, 4, 4, 72.0f, "sRGB");

    const comp_layer_id base = add_layer(doc, "base", COMP_PIXEL_RGBA8);
    doc.find(base)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 100, 150, 200, 180);
    const comp_layer_id top = add_layer(doc, "top", COMP_PIXEL_RGBA8,
                                        static_cast<comp_blend_mode>(mode), 0.75f);
    doc.find(top)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 200, 100, 50, 128);

    std::string error;
    ImageBuffer graph_result;
    COMP_CHECK_MSG(composite_document(doc, &graph_result, &error),
                   "composite_document must succeed for every blend mode");
    COMP_CHECK(graph_result.format == COMP_PIXEL_RGBA8);

    ImageBuffer direct;
    COMP_CHECK(direct_composite_8bit(doc, &direct, &error));
    COMP_CHECK_MSG(graph_result.byte_size() == direct.byte_size(),
                   "8-bit graph and direct composition size differ");
    const bool same = graph_result.byte_size() == direct.byte_size() &&
                      std::memcmp(graph_result.pixel_data().data(),
                                  direct.pixel_data().data(),
                                  graph_result.byte_size()) == 0;
    COMP_CHECK_MSG(same, std::string("8-bit blend mode ") +
                             blend_mode_manifest_name(static_cast<comp_blend_mode>(mode)) +
                             " drifted from the direct 8-bit path");
  }
}

/* 4. half 混合与 8 位混合是同一套公式（全覆盖 alpha 时逐通道差 <= 2）。 */
COMP_TEST(c5_half_blend_matches_the_eight_bit_formula) {
  const uint8_t base[4] = {64, 128, 192, 255};
  const uint8_t src[4] = {200, 100, 50, 255};
  const double src_alpha = static_cast<double>(src[3]) / 255.0;

  for (int mode = COMP_BLEND_NORMAL; mode <= COMP_BLEND_LUMINOSITY; ++mode) {
    uint8_t want[4];
    std::memcpy(want, base, sizeof(want));
    blend_pixels(want, src, static_cast<comp_blend_mode>(mode), src_alpha);

    uint16_t dst_half[4];
    uint16_t src_half[4];
    for (int c = 0; c < 4; ++c) {
      dst_half[c] = deep_half_bits(static_cast<float>(base[c]) / 255.0f);
      src_half[c] = deep_half_bits(static_cast<float>(src[c]) / 255.0f);
    }
    blend_pixels_half(dst_half, src_half, static_cast<comp_blend_mode>(mode), src_alpha);

    for (int c = 0; c < 4; ++c) {
      const int got = static_cast<int>(
          std::lround(static_cast<double>(deep_half_value(dst_half[c])) * 255.0));
      COMP_CHECK_MSG(std::abs(got - static_cast<int>(want[c])) <= 2,
                     std::string("half blend drifted from the 8-bit formula in mode ") +
                         blend_mode_manifest_name(static_cast<comp_blend_mode>(mode)));
    }
  }
}

/*
 * 5. half 的颜色**不夹到 alpha**：预乘的 HDR 颜色可以比覆盖率亮。
 *    这是 HDR 与 8 位的本质差别；若 store_half_premultiplied 顺手夹一次
 *    (v = min(v, alpha))，下面的 2.0 > 0.5 会立刻变成 0.5。
 */
COMP_TEST(c5_half_premultiplied_color_may_exceed_alpha) {
  uint16_t dst[4] = {0, 0, 0, 0};
  const uint16_t src[4] = {deep_half_bits(2.0f), deep_half_bits(2.0f), deep_half_bits(2.0f),
                           deep_half_bits(0.5f)};
  blend_pixels_half(dst, src, COMP_BLEND_NORMAL, 0.5);

  const float color = deep_half_value(dst[0]);
  const float alpha = deep_half_value(dst[3]);
  COMP_CHECK_MSG(std::fabs(color - 2.0f) < 1e-3f,
                 "half blending keeps the premultiplied color (2.0) even above alpha");
  COMP_CHECK_MSG(color > alpha, "a half surface must not clamp color to alpha");
  COMP_CHECK_MSG(std::fabs(alpha - 0.5f) < 1e-3f, "alpha stays the coverage (0.5)");
}

/*
 * 6. 调整图层在 half 表面上的像素求值：
 *    - 曝光走 deep_hdr_exposure，结果与直接调用同一内核逐位一致；
 *    - 白点以上的光仍然高于 1（不被截断）；
 *    - HSV 走 normalize → 16 位往返 → restore，高光幅度保留。
 */
COMP_TEST(c5_half_adjustments_preserve_highlights) {
  {
    Engine engine(c5_engine_desc());
    Document doc(&engine, 2, 1, 72.0f, "sRGB");
    const comp_layer_id photo = add_layer(doc, "photo", COMP_PIXEL_RGBA16F);
    Layer* layer = doc.find(photo);
    COMP_CHECK(layer != nullptr);
    const uint16_t px0[4] = {deep_half_bits(0.5f), deep_half_bits(0.5f), deep_half_bits(0.5f),
                             deep_half_bits(1.0f)};
    const uint16_t px1[4] = {deep_half_bits(2.0f), deep_half_bits(1.0f), deep_half_bits(0.5f),
                             deep_half_bits(1.0f)};
    std::memcpy(layer->pixels.writable_row(0), px0, sizeof(px0));
    std::memcpy(layer->pixels.writable_row(0) + 8u, px1, sizeof(px1));

    const comp_layer_id adjust = add_layer(doc, "exposure", COMP_PIXEL_RGBA8);
    doc.find(adjust)->pixels = ImageBuffer{};
    Adjustment exposure;
    exposure.kind = 3;
    exposure.settings_json =
        R"({"exposureSettings":{"exposure":1.0,"offset":0.0,"gamma":1.0}})";
    doc.find(adjust)->adjustments.push_back(exposure);

    ImageBuffer canvas;
    std::string error;
    COMP_CHECK(composite_document(doc, &canvas, &error));
    COMP_CHECK(canvas.format == COMP_PIXEL_RGBA16F);

    ImageBuffer expected = doc.find(photo)->pixels;
    deep_hdr_exposure(reinterpret_cast<uint16_t*>(expected.writable_pixels().data()),
                      expected.stride(), 2, 1, 2.0f, 0.0f, 1.0f);
    /* 表面不是 half 时不做 half 读取：注入验证应以断言失败收场，而不是越界读。 */
    if (canvas.format == COMP_PIXEL_RGBA16F) {
      for (int32_t x = 0; x < 2; ++x) {
        for (int c = 0; c < 3; ++c) {
          COMP_CHECK_MSG(
              std::fabs(half_channel(canvas, x, 0, c) - half_channel(expected, x, 0, c)) < 1e-3f,
              "half exposure must be the deep_hdr_exposure kernel, applied on the surface");
        }
      }
      COMP_CHECK_MSG(half_channel(canvas, 1, 0, 0) > 1.0f,
                     "+1 EV on an HDR highlight stays above white");
    }
  }

  {
    Engine engine(c5_engine_desc());
    Document doc(&engine, 2, 1, 72.0f, "sRGB");
    const comp_layer_id photo = add_layer(doc, "photo", COMP_PIXEL_RGBA16F);
    Layer* layer = doc.find(photo);
    COMP_CHECK(layer != nullptr);
    const uint16_t px[4] = {deep_half_bits(1.0f), deep_half_bits(0.5f), deep_half_bits(2.0f),
                            deep_half_bits(1.0f)};
    std::memcpy(layer->pixels.writable_row(0), px, sizeof(px));
    std::memcpy(layer->pixels.writable_row(0) + 8u, px, sizeof(px));

    const comp_layer_id adjust = add_layer(doc, "hsv", COMP_PIXEL_RGBA8);
    doc.find(adjust)->pixels = ImageBuffer{};
    Adjustment hsv;
    hsv.kind = 0;
    /*
     * hue/saturation/lightness 是**绝对**的 HSL 参数（lightness 0.5 = 中灰），
     * 不是「在原值上加 20%」。取 lightness 50 让归一化后的峰值停在 0.75，
     * restore 乘回 scale=2 后应得 1.5——仍高于白点，证明高光幅度没在往返中丢失。
     */
    hsv.settings_json = R"({"hue":90,"saturation":50,"lightness":50})";
    doc.find(adjust)->adjustments.push_back(hsv);

    ImageBuffer canvas;
    std::string error;
    COMP_CHECK(composite_document(doc, &canvas, &error));
    COMP_CHECK(canvas.format == COMP_PIXEL_RGBA16F);
    if (canvas.format == COMP_PIXEL_RGBA16F) {
      /* 颜色确实被改写（否则「保留高光」可能只是「什么都没做」）。 */
      const bool changed =
          std::fabs(half_channel(canvas, 0, 0, 0) - deep_half_value(px[0])) > 1e-3f ||
          std::fabs(half_channel(canvas, 0, 0, 1) - deep_half_value(px[1])) > 1e-3f;
      COMP_CHECK_MSG(changed, "an HSV adjustment on a half surface must change pixels");
      const float peak =
          std::max({half_channel(canvas, 0, 0, 0), half_channel(canvas, 0, 0, 1),
                    half_channel(canvas, 0, 0, 2)});
      COMP_CHECK_MSG(peak > 1.0f,
                     "normalize/restore must keep the highlight above white through HSV");
    }
  }
}

/* 7. 剪贴栈在 half 表面上用 float coverage，仍按栈语义合成。 */
COMP_TEST(c5_clipping_stack_works_on_a_half_surface) {
  Engine engine(c5_engine_desc());
  Document doc(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(doc, "base", COMP_PIXEL_RGBA16F);
  set_solid_half(doc, base, 0.0f, 0.0f, 1.0f, 1.0f); /* 不透明蓝（预乘） */
  const comp_layer_id child = add_layer(doc, "child", COMP_PIXEL_RGBA16F);
  set_solid_half(doc, child, 1.0f, 0.0f, 0.0f, 1.0f); /* 不透明红（预乘） */
  doc.find(child)->clipping_source = base;

  ImageBuffer canvas;
  std::string error;
  COMP_CHECK_MSG(composite_document(doc, &canvas, &error),
                 "a clipping stack on a half surface must evaluate");
  COMP_CHECK(canvas.format == COMP_PIXEL_RGBA16F);
  if (canvas.format == COMP_PIXEL_RGBA16F) {
    COMP_CHECK_MSG(std::fabs(half_channel(canvas, 2, 2, 0) - 1.0f) < 1e-3f &&
                       std::fabs(half_channel(canvas, 2, 2, 2)) < 1e-3f &&
                       std::fabs(half_channel(canvas, 2, 2, 3) - 1.0f) < 1e-3f,
                   "the clipped member replaces the base's color inside the stack");
  }
}

/* 8. CPU 后端的渲染目标：接受 RGBA16F、拒绝 RGBA16，同深度零拷贝。 */
COMP_TEST(c5_cpu_backend_accepts_half_targets) {
  Engine engine(c5_engine_desc());
  Document doc(&engine, 2, 1, 72.0f, "sRGB");
  const comp_layer_id id = add_layer(doc, "hdr", COMP_PIXEL_RGBA16F);
  set_solid_half(doc, id, 0.5f, 0.25f, 0.125f, 1.0f);

  ImageBuffer canvas;
  std::string error;
  COMP_CHECK(composite_document(doc, &canvas, &error));
  COMP_CHECK(canvas.format == COMP_PIXEL_RGBA16F);

  std::unique_ptr<RenderBackend> backend = create_cpu_backend(&engine);
  COMP_CHECK(backend != nullptr);

  std::vector<uint8_t> half_target(2u * 8u, 0);
  comp_render_target target{};
  target.size = comp_size{2, 1};
  target.format = COMP_PIXEL_RGBA16F;
  target.kind = COMP_SURFACE_CPU_BUFFER;
  target.stride = 16;
  target.pixels = half_target.data();
  COMP_CHECK_MSG(backend->render_document(doc, target, &error),
                 "the cpu backend must accept an RGBA16F render target");
  if (canvas.format == COMP_PIXEL_RGBA16F) {
    COMP_CHECK_MSG(
        std::memcmp(half_target.data(), canvas.pixel_data().data(), half_target.size()) == 0,
        "same-depth rendering must be a byte-exact copy of the half composite");
  }

  std::vector<uint8_t> eight_target(2u * 4u, 0);
  target.format = COMP_PIXEL_RGBA8;
  target.stride = 8;
  target.pixels = eight_target.data();
  COMP_CHECK(backend->render_document(doc, target, &error));
  ImageBuffer display;
  COMP_CHECK(hdr_to_rgba8(canvas, &display));
  if (display.width == 2 && display.height == 1) {
    COMP_CHECK_MSG(std::memcmp(eight_target.data(), display.pixel_data().data(),
                               eight_target.size()) == 0,
                   "an RGBA8 target is the hdr_to_rgba8 display view of the half composite");
  }

  target.format = COMP_PIXEL_RGBA16;
  target.pixels = half_target.data();
  target.stride = 16;
  error.clear();
  COMP_CHECK_MSG(!backend->render_document(doc, target, &error),
                 "the cpu backend must reject RGBA16 targets instead of misreading half rows");
  COMP_CHECK_MSG(error.find("RGBA8 and RGBA16F") != std::string::npos,
                 "the rejection must name the supported formats");
}

/* 9. 缩略图契约不变：half 图层内部按 half 求值，返回 RGBA8 显示副本。 */
COMP_TEST(c5_layer_view_returns_an_eight_bit_display_copy) {
  Engine engine(c5_engine_desc());
  Document doc(&engine, 2, 1, 72.0f, "sRGB");
  const comp_layer_id id = add_layer(doc, "hdr", COMP_PIXEL_RGBA16F);
  set_solid_half(doc, id, 2.0f, 2.0f, 2.0f, 0.5f);

  ImageBuffer view;
  std::string error;
  COMP_CHECK(composite_layer(doc, id, &view, &error));
  COMP_CHECK_MSG(view.format == COMP_PIXEL_RGBA8,
                 "layer thumbnails keep outputting RGBA8 even for HDR layers");
  /*
   * 缩略图的输出是**预乘** RGBA8：预乘 2.0 / alpha 0.5 是直通 4.0，
   * 显示时先截到白、再乘回覆盖率 0.5，得到预乘 0.5 -> 128（叠在黑底上就是
   * 50% 白）。dither_quantize16 明确把颜色夹到 alpha 以内，所以这里不该是 255：
   * 255 会表示「不透明的白」，与本像素 50% 的覆盖率矛盾。
   */
  if (view.format == COMP_PIXEL_RGBA8 && view.width == 2 && view.height == 1) {
    const uint8_t* px = view.row(0);
    COMP_CHECK_MSG(std::abs(static_cast<int>(px[0]) - 128) <= 2 &&
                       std::abs(static_cast<int>(px[3]) - 128) <= 2,
                   "premultiplied 2.0 at alpha 0.5 displays as 50% white (128, not 255)");
  }
}

/*
 * 10. 裁边按格式解码 alpha：half 的小 alpha（0x00FF，高位字节为 0）与
 *     16 位整数 alpha 0x0001 都必须算作「有内容」，不能被当成透明裁掉。
 */
COMP_TEST(c5_trim_to_content_decodes_half_and_wide_alpha) {
  {
    ImageBuffer src;
    src.resize(4, 4, COMP_PIXEL_RGBA16F);
    src.clear_transparent();
    const uint16_t px[4] = {0x3c00, 0x3c00, 0x3c00, 0x00ff}; /* 色 1.0，alpha ≈ 1.5e-5 */
    std::memcpy(src.writable_row(1) + 8u, px, sizeof(px));

    ImageBuffer out;
    comp_rect placement{};
    const bool trimmed = trim_to_content(src, &out, &placement);
    COMP_CHECK_MSG(trimmed, "a half pixel with a tiny alpha still counts as content");
    COMP_CHECK_MSG(placement.x == 1 && placement.y == 1 && placement.w == 1 && placement.h == 1,
                   "the half trim window must be the content pixel");
    COMP_CHECK(out.format == COMP_PIXEL_RGBA16F);
    COMP_CHECK(out.width == 1 && out.height == 1);
    /* 裁失败时 out 为空，不能解引用——否则注入验证会以崩溃代替用例失败。 */
    if (trimmed && out.width == 1 && out.height == 1) {
      const uint16_t* got = reinterpret_cast<const uint16_t*>(out.row(0));
      COMP_CHECK(got[0] == 0x3c00 && got[3] == 0x00ff);
    }
  }

  {
    ImageBuffer src;
    src.resize(4, 4, COMP_PIXEL_RGBA16);
    src.clear_transparent();
    const uint16_t px[4] = {0x4000, 0x0000, 0x0000, 0x0001}; /* alpha 1/65535 */
    std::memcpy(src.writable_row(2) + 2u * 8u, px, sizeof(px));

    ImageBuffer out;
    comp_rect placement{};
    const bool trimmed = trim_to_content(src, &out, &placement);
    COMP_CHECK_MSG(trimmed, "a 16-bit integer pixel with a tiny alpha still counts as content");
    COMP_CHECK_MSG(placement.x == 2 && placement.y == 2 && placement.w == 1 && placement.h == 1,
                   "the 16-bit trim window must be the content pixel");
    if (trimmed && out.width == 1 && out.height == 1) {
      const uint16_t* got = reinterpret_cast<const uint16_t*>(out.row(0));
      COMP_CHECK(got[0] == 0x4000 && got[3] == 0x0001);
    }
  }
}
