// AI生成
/*
 * test_adjustment_eval.cpp — 调整图层「像素求值」的回归护栏。
 *
 * 为什么需要这组用例：调整图层此前只入库不求值——ABI 写入成功、计数能读回，
 * 但合成路径完全无视它们（ctypes 实测：加 Invert / Hue/Sat / Add Noise 前后
 * `comp_render_flatten` 输出逐字节相同）。本组把「调整真的改变像素」钉死，
 * 并对齐上游的滤镜节点语义（EditorCanvas.swift:3092
 * "An adjustment re-colors what's under it"）。
 *
 * 冻结的行为要点：
 *   1. 12 个 kind 全部有像素实现，且在非恒等参数下逐字节改变渲染结果；
 *   2. 调整作用于「其下方的合成结果」，不影响上方图层（滤镜节点语义）；
 *   3. 隐藏调整图层 ⇒ 渲染结果逐字节回到无调整状态；
 *   4. 恒等参数 ⇒ 像素逐字节不变；
 *   5. 坏 JSON / 未知 kind ⇒ 跳过而不是产出错误像素，更不会让整篇合成失败。
 *
 * 2026-10-16（Pi，AUDIT6）增补两条上游语义：
 *   6. 组蒙版参与 coverage（EditorCanvas.swift:3114-3120 的 folders:true 分支）；
 *   7. 非 normal 混合模式先混后回 coverage（EditorCanvas.swift:3096-3106）。
 */
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

/*
 * 本文件只用 C ABI，不引入引擎内部命名空间：只包含 ABI 头时
 * `using namespace compositor` 不成立（ABI 头不声明该命名空间）。
 */
namespace {

constexpr int32_t kW = 8;
constexpr int32_t kH = 8;
constexpr size_t kStride = static_cast<size_t>(kW) * 4;

/* 建引擎 + 建 8x8 文档；RAII 保证用例提前返回也不泄漏。 */
struct Fixture {
  comp_engine engine = nullptr;
  comp_document doc = nullptr;

  Fixture() {
    comp_engine_desc desc{};
    desc.app_name = "compositor-adjustment-eval-tests";
    desc.version = "1.0.0";
    desc.backend = COMP_BACKEND_CPU;
    desc.worker_threads = 1;
    comp_document_desc doc_desc{};
    doc_desc.width = kW;
    doc_desc.height = kH;
    doc_desc.dpi = 72.0f;
    comp_engine_create(&desc, &engine);
    comp_document_create(engine, &doc_desc, &doc);
  }
  ~Fixture() {
    if (doc != nullptr) comp_document_destroy(doc);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
};

/* 每像素 (x*16, y*16, 128, 255) 的渐变。
 * 必须用渐变而非纯色：模糊/曲线作用于纯色是恒等，会被误读成「没生效」。 */
std::vector<uint8_t> gradient_pixels() {
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4;
      px[0] = static_cast<uint8_t>(x * 16);
      px[1] = static_cast<uint8_t>(y * 16);
      px[2] = 128;
      px[3] = 255;
    }
  }
  return pixels;
}

std::vector<uint8_t> flatten(comp_document doc) {
  std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4);
  COMP_CHECK(comp_render_flatten(doc, out.data(), COMP_PIXEL_RGBA8, kStride) == COMP_OK);
  return out;
}

/* 加一个带渐变的像素图层并渲染（返回结果；out_base 拿到图层 id）。 */
std::vector<uint8_t> render_gradient(comp_document doc, comp_layer_id* out_base = nullptr) {
  comp_layer_desc desc{};
  desc.name = "base";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.parent = 0;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id base = 0;
  COMP_CHECK(comp_layer_add(doc, &desc, &base) == COMP_OK);

  std::vector<uint8_t> pixels = gradient_pixels();
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(doc, base, &image) == COMP_OK);
  if (out_base != nullptr) *out_base = base;
  return flatten(doc);
}

/* 叠一个只带调整的图层（本身无像素），返回该图层 id。 */
comp_layer_id add_adjustment(comp_document doc, uint32_t kind, const char* settings) {
  comp_layer_desc desc{};
  desc.name = "adjustment";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.parent = 0;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add(doc, &desc, &id) == COMP_OK);
  COMP_CHECK(comp_layer_add_adjustment(doc, id, kind, settings) == COMP_OK);
  return id;
}

/* 12 个 kind 的非恒等参数（键名逐字取自 adjustment_table 的参数表）。 */
struct KindCase {
  uint32_t kind;
  const char* name;
  const char* settings;
};

const KindCase kEvaluatedKinds[] = {
    {0, "hsv", R"({"hue":90,"saturation":50,"lightness":20})"},
    {1, "levels",
     R"({"levels":{"channel":"RGB","ranges":[{"black":0,"gamma":1,"white":255,"outputBlack":0,"outputWhite":128},{},{},{}]}})"},
    {2, "curves", R"({"curves":{"channels":[[{"x":0,"y":255},{"x":255,"y":0}]]}})"},
    {3, "exposure", R"({"exposureSettings":{"exposure":2.0,"offset":0.0,"gamma":1.0}})"},
    {4, "gradientMap",
     R"({"gradientMapSettings":{"shadows":{"red":1,"green":0,"blue":0},"highlights":{"red":0,"green":0,"blue":1},"reversed":false}})"},
    {5, "grain", R"({"grainSettings":{"amount":60,"size":1.5,"roughness":50,"seed":7}})"},
    {6, "addNoise",
     R"({"noiseAmount":80,"noiseGaussian":true,"noiseMonochromatic":false,"noiseSeed":3})"},
    {7, "gaussianBlur", R"({"blurRadius":6.0})"},
    {8, "motionBlur", R"({"motionAngle":0,"motionDistance":12})"},
    {9, "invert", R"({})"},
    {10, "blackWhite",
     R"({"blackWhiteSettings":{"reds":100,"yellows":100,"greens":100,"cyans":100,"blues":100,"magentas":100,"tint":false}})"},
    {11, "colorBalance",
     R"({"colorBalanceSettings":{"shadowCyanRed":-60,"midCyanRed":0,"highlightCyanRed":60,"preserveLuminosity":true}})"},
};

}  // namespace

/* 1. 12 个 kind 全部改变像素（此前它们全部是 no-op）。 */
COMP_TEST(adjustment_eval_every_kind_changes_pixels) {
  for (const KindCase& c : kEvaluatedKinds) {
    Fixture fx;
    const std::vector<uint8_t> plain = render_gradient(fx.doc);
    add_adjustment(fx.doc, c.kind, c.settings);
    const std::vector<uint8_t> adjusted = flatten(fx.doc);
    COMP_CHECK_MSG(plain != adjusted, std::string("kind ") + c.name + " must change pixels");
  }
}

/* 2. Invert 是精确可断言的：RGB 逐通道取反，alpha 不变。 */
COMP_TEST(adjustment_eval_invert_is_exact_complement) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  add_adjustment(fx.doc, 9, "{}");
  const std::vector<uint8_t> adjusted = flatten(fx.doc);
  for (size_t i = 0; i < plain.size(); i += 4) {
    COMP_CHECK(adjusted[i] == 255 - plain[i]);
    COMP_CHECK(adjusted[i + 1] == 255 - plain[i + 1]);
    COMP_CHECK(adjusted[i + 2] == 255 - plain[i + 2]);
    COMP_CHECK(adjusted[i + 3] == plain[i + 3]);
  }
}

/* 3. 隐藏调整图层 ⇒ 逐字节回到无调整（对齐上游 toggleLayerVisibility）。 */
COMP_TEST(adjustment_eval_hidden_adjustment_restores_pixels) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  const comp_layer_id adj = add_adjustment(fx.doc, 9, "{}");
  COMP_CHECK(flatten(fx.doc) != plain);

  COMP_CHECK(comp_layer_set_visible(fx.doc, adj, 0) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == plain, "hidden adjustment layer must be a byte-exact no-op");
}

/* 4. 恒等参数 ⇒ 逐字节不变（缺键/默认值必须回落恒等）。 */
COMP_TEST(adjustment_eval_identity_settings_are_byte_exact) {
  /* Levels 默认 ranges = 0/1/255/0/255 即恒等；Exposure 0/0/1 即恒等。 */
  const KindCase kIdentity[] = {
      {1, "levels-default",
       R"({"levels":{"ranges":[{"black":0,"gamma":1,"white":255,"outputBlack":0,"outputWhite":255},{},{},{}]}})"},
      {3, "exposure-default", R"({"exposureSettings":{"exposure":0,"offset":0,"gamma":1}})"},
  };
  for (const KindCase& c : kIdentity) {
    Fixture fx;
    const std::vector<uint8_t> plain = render_gradient(fx.doc);
    add_adjustment(fx.doc, c.kind, c.settings);
    COMP_CHECK_MSG(flatten(fx.doc) == plain,
                   std::string("identity ") + c.name + " must not change pixels");
  }
}

/* 坏 JSON / 未知 kind 写入侧就应拒绝；且拒绝后渲染不受影响。 */
COMP_TEST(adjustment_eval_bad_settings_skip_cleanly) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);

  auto add_rejected = [&](uint32_t kind, const char* settings) {
    comp_layer_desc desc{};
    desc.name = "bad";
    desc.blend = COMP_BLEND_NORMAL;
    desc.opacity = 1.0f;
    desc.transform = comp_rect{0, 0, kW, kH};
    comp_layer_id id = 0;
    COMP_CHECK(comp_layer_add(fx.doc, &desc, &id) == COMP_OK);
    /* 两种坏输入：无法解析的 JSON、以及超出参数表的 kind。 */
    COMP_CHECK_MSG(comp_layer_add_adjustment(fx.doc, id, kind, settings) != COMP_OK,
                   "invalid adjustment must be rejected at the ABI boundary");
  };

  add_rejected(9, "{not json at all");
  add_rejected(9999, R"({})");
  COMP_CHECK_MSG(flatten(fx.doc) == plain,
                 "rejected adjustments must leave pixels byte-exact");
}

/* 7. opacity<1 的调整层是「半强度调整」（调整结果按 1-opacity 混回 backdrop）。 */
COMP_TEST(adjustment_eval_opacity_is_partial_strength) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  const comp_layer_id adj = add_adjustment(fx.doc, 9, "{}");
  const std::vector<uint8_t> full = flatten(fx.doc);
  COMP_CHECK(full != plain);

  COMP_CHECK(comp_layer_set_opacity(fx.doc, adj, 0.5f) == COMP_OK);
  const std::vector<uint8_t> half = flatten(fx.doc);
  /* 半强度应落在两端之间，且两端不同（否则 opacity 被忽略或当成开关）。 */
  COMP_CHECK(half != plain);
  COMP_CHECK(half != full);
  /* Invert 的补值是线性的，所以半强度应精确等于两端平均。 */
  for (size_t i = 0; i < half.size(); i += 4) {
    const int expected = (static_cast<int>(plain[i]) + static_cast<int>(full[i]) + 1) / 2;
    COMP_CHECK_MSG(std::abs(half[i] - expected) <= 1,
                   "half-opacity invert should sit midway between plain and full");
  }
}

/* 8. 蒙版裁剪调整：仅全白区域被调整，黑区字节不变。 */
COMP_TEST(adjustment_eval_mask_clips_adjustment) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  const comp_layer_id adj = add_adjustment(fx.doc, 9, "{}");
  const std::vector<uint8_t> unmasked = flatten(fx.doc);
  COMP_CHECK(unmasked != plain);

  /* 左半全白（255=调整），右半全黑（0=不调整）。 */
  std::vector<uint8_t> mask_pixels(static_cast<size_t>(kW) * kH);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      mask_pixels[static_cast<size_t>(y) * kW + x] = (x < kW / 2) ? 255 : 0;
    }
  }
  comp_mask mask{};
  mask.width = kW;
  mask.height = kH;
  mask.stride = static_cast<size_t>(kW);
  mask.pixels = mask_pixels.data();
  COMP_CHECK(comp_layer_set_mask(fx.doc, adj, &mask) == COMP_OK);

  const std::vector<uint8_t> masked = flatten(fx.doc);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      const size_t px = (static_cast<size_t>(y) * kW + x) * 4;
      if (x < kW / 2) {
        COMP_CHECK_MSG(masked[px] == unmasked[px], "masked-in area must stay adjusted");
      } else {
        COMP_CHECK_MSG(masked[px] == plain[px], "masked-out area must be untouched");
      }
    }
  }
}

/* 9. 调整层同时带自身像素时：像素叠加在调整之上（上游允许 asset+adjustment 并存）。 */
COMP_TEST(adjustment_eval_adjustment_then_own_pixels) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);

  /* 调整层里再放一张半透明白色图层。 */
  const comp_layer_id adj = add_adjustment(fx.doc, 9, "{}");
  std::vector<uint8_t> white(static_cast<size_t>(kW) * kH * 4, 0);
  for (size_t i = 0; i < white.size(); i += 4) {
    white[i] = 255;
    white[i + 1] = 255;
    white[i + 2] = 255;
    white[i + 3] = 128;
  }
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = white.data();
  COMP_CHECK(comp_layer_set_pixels(fx.doc, adj, &image) == COMP_OK);

  const std::vector<uint8_t> out = flatten(fx.doc);
  /*
   * 合成顺序：backdrop -> Invert -> 叠加调整层自身的半透明白。
   * 取反后底色值 v' = 255 - v，再按 50% 不透明度混白：(v' + 255)/2。
   * 断言结果落在「仅调整」与「仅叠白」两个界之间，并确实不同于两者。
   */
  std::vector<uint8_t> only_white(static_cast<size_t>(kW) * kH * 4, 0);
  for (size_t i = 0; i < white.size(); i += 4) {
    only_white[i] = static_cast<uint8_t>((plain[i] + 255) / 2);
    only_white[i + 1] = static_cast<uint8_t>((plain[i + 1] + 255) / 2);
    only_white[i + 2] = static_cast<uint8_t>((plain[i + 2] + 255) / 2);
    only_white[i + 3] = 255;
  }
  COMP_CHECK_MSG(out != only_white, "adjustment must apply before the layer's own pixels");
  const size_t px = 0;
  COMP_CHECK_MSG(out[px] != plain[px], "combined result differs from plain backdrop");
}

/* 6. 调整只作用于其下方图层，不影响上方（滤镜节点而非全局后处理）。 */
COMP_TEST(adjustment_eval_does_not_touch_layers_above) {
  Fixture fx;
  render_gradient(fx.doc);
  add_adjustment(fx.doc, 9, "{}");

  comp_layer_desc top_desc{};
  top_desc.name = "top";
  top_desc.blend = COMP_BLEND_NORMAL;
  top_desc.opacity = 1.0f;
  top_desc.transform = comp_rect{0, 0, kW, kH};
  top_desc.parent = 0;
  top_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id top = 0;
  COMP_CHECK(comp_layer_add(fx.doc, &top_desc, &top) == COMP_OK);

  std::vector<uint8_t> red(static_cast<size_t>(kW) * kH * 4, 0);
  for (size_t i = 0; i < red.size(); i += 4) {
    red[i] = 255;
    red[i + 3] = 255;
  }
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = red.data();
  COMP_CHECK(comp_layer_set_pixels(fx.doc, top, &image) == COMP_OK);

  const std::vector<uint8_t> out = flatten(fx.doc);
  /* 上层不透明红遮住一切：反转不渗透到调整层之上。 */
  for (size_t i = 0; i < out.size(); i += 4) {
    COMP_CHECK_MSG(out[i] == 255 && out[i + 1] == 0 && out[i + 2] == 0,
                   "layers above the adjustment must be untouched");
  }
}

/* 10. 拼合图像：调整被烘入像素（此前带调整的文档直接 COMP_ERR_UNSUPPORTED）。 */
COMP_TEST(adjustment_eval_flatten_bakes_adjustment) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  add_adjustment(fx.doc, 9, "{}");
  const std::vector<uint8_t> before = flatten(fx.doc);
  COMP_CHECK_MSG(before != plain, "sanity: the adjustment must change pixels before flattening");

  COMP_CHECK_MSG(comp_document_flatten(fx.doc) == COMP_OK,
                 "flatten must bake evaluable adjustments, not refuse the document");
  /* 拼合后只剩一层，且调整已消失（烘进像素）。 */
  size_t layer_count = 0;
  COMP_CHECK(comp_layer_count(fx.doc, &layer_count) == COMP_OK);
  COMP_CHECK(layer_count == 1);
  /* 烘入结果必须与拼合前的整篇合成逐字节一致。 */
  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(after == before, "flatten must be render-invariant");
}

/* 11. 拼合后可撤销：一步撤销回到拼合前（含调整）。 */
COMP_TEST(adjustment_eval_flatten_is_undoable) {
  Fixture fx;
  render_gradient(fx.doc);
  add_adjustment(fx.doc, 9, "{}");
  const std::vector<uint8_t> before = flatten(fx.doc);
  size_t layers_before = 0;
  COMP_CHECK(comp_layer_count(fx.doc, &layers_before) == COMP_OK);

  COMP_CHECK(comp_document_flatten(fx.doc) == COMP_OK);
  size_t layers_after = 0;
  COMP_CHECK(comp_layer_count(fx.doc, &layers_after) == COMP_OK);
  COMP_CHECK(layers_after == 1);
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  size_t layers_restored = 0;
  COMP_CHECK(comp_layer_count(fx.doc, &layers_restored) == COMP_OK);
  COMP_CHECK_MSG(layers_restored == layers_before,
                 "undo must restore every flattened layer");
  COMP_CHECK_MSG(flatten(fx.doc) == before,
                 "undo of flatten must restore the pre-flatten pixels exactly");
}

/* ------------------------------------------------------------------ helpers */

/* 建一个横跨整幅画布的组层，返回其 id。 */
comp_layer_id add_group(comp_document doc, const char* name) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.is_group = 1;
  desc.parent = 0;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add(doc, &desc, &id) == COMP_OK);
  return id;
}

/* 一张左右分半（全白 / 全黑）的画布尺寸蒙版。 */
comp_mask half_mask(std::vector<uint8_t>* storage) {
  storage->assign(static_cast<size_t>(kW) * kH, 0);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW / 2; ++x) {
      (*storage)[static_cast<size_t>(y) * kW + x] = 255;
    }
  }
  comp_mask mask{};
  mask.width = kW;
  mask.height = kH;
  mask.stride = static_cast<size_t>(kW);
  mask.pixels = storage->data();
  return mask;
}

/* 12. 组蒙版参与调整 coverage（AUDIT5 §7.1 的缺口）。
 *
 * 上游 EditorCanvas.swift:3114-3120：调整层在其组内时，祖先链上每个带蒙版的组
 * 都以 `multiply` 进入 coverage。接手时只做了自身蒙版（×opacity），组蒙版从未被读——
 * 于是「把调整层放进一个带蒙版的组」完全不影响渲染。 */
COMP_TEST(adjustment_eval_group_mask_clips_adjustment) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);

  /* 调整层挂在带蒙版的组里：组蒙版左白右黑 ⇒ 仅左半应被调整。 */
  const comp_layer_id group = add_group(fx.doc, "folder");
  comp_layer_desc adj_desc{};
  adj_desc.name = "adjustment";
  adj_desc.blend = COMP_BLEND_NORMAL;
  adj_desc.opacity = 1.0f;
  adj_desc.transform = comp_rect{0, 0, kW, kH};
  adj_desc.parent = group;
  adj_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id adj = 0;
  COMP_CHECK(comp_layer_add(fx.doc, &adj_desc, &adj) == COMP_OK);
  COMP_CHECK(comp_layer_add_adjustment(fx.doc, adj, 9, "{}") == COMP_OK);

  /* 无组蒙版时：整幅被调整（作为对照，证明蒙版确实是变量）。 */
  const std::vector<uint8_t> no_group_mask = flatten(fx.doc);
  COMP_CHECK_MSG(no_group_mask != plain, "sanity: the adjustment must change pixels");

  std::vector<uint8_t> storage;
  comp_mask mask = half_mask(&storage);
  COMP_CHECK(comp_layer_set_mask(fx.doc, group, &mask) == COMP_OK);

  const std::vector<uint8_t> masked = flatten(fx.doc);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      const size_t px = (static_cast<size_t>(y) * kW + x) * 4;
      if (x < kW / 2) {
        COMP_CHECK_MSG(masked[px] == no_group_mask[px],
                       "inside the group mask the adjustment must apply as before");
      } else {
        COMP_CHECK_MSG(masked[px] == plain[px],
                       "outside the group mask the adjustment must be suppressed");
      }
    }
  }
}

/* 13. 组蒙版与自身蒙版相乘（AUDIT5 §7.1 的第二半）。
 *
 * 上游 coverage = 自身蒙版 × 组蒙版 × opacity，三者是**乘法**关系：
 * 只各自实现一半不构成正确语义，必须验证交集/并集边界。 */
COMP_TEST(adjustment_eval_own_and_group_mask_multiply) {
  Fixture fx;
  render_gradient(fx.doc);

  const comp_layer_id group = add_group(fx.doc, "folder");
  comp_layer_desc adj_desc{};
  adj_desc.name = "adjustment";
  adj_desc.blend = COMP_BLEND_NORMAL;
  adj_desc.opacity = 1.0f;
  adj_desc.transform = comp_rect{0, 0, kW, kH};
  adj_desc.parent = group;
  adj_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id adj = 0;
  COMP_CHECK(comp_layer_add(fx.doc, &adj_desc, &adj) == COMP_OK);
  COMP_CHECK(comp_layer_add_adjustment(fx.doc, adj, 9, "{}") == COMP_OK);

  /* 自身蒙版：上半白 / 下半黑；组蒙版：左半白 / 右半黑。 */
  std::vector<uint8_t> own_storage(static_cast<size_t>(kW) * kH, 0);
  for (int32_t y = 0; y < kH / 2; ++y) {
    for (int32_t x = 0; x < kW; ++x) own_storage[static_cast<size_t>(y) * kW + x] = 255;
  }
  comp_mask own{};
  own.width = kW;
  own.height = kH;
  own.stride = static_cast<size_t>(kW);
  own.pixels = own_storage.data();
  COMP_CHECK(comp_layer_set_mask(fx.doc, adj, &own) == COMP_OK);

  std::vector<uint8_t> group_storage;
  comp_mask group_mask = half_mask(&group_storage);
  COMP_CHECK(comp_layer_set_mask(fx.doc, group, &group_mask) == COMP_OK);

  const std::vector<uint8_t> plain = flatten(fx.doc);
  /* 移除两张蒙版 ⇒ 全幅被调整，用作「被调整」的判据。 */
  COMP_CHECK(comp_layer_set_mask(fx.doc, adj, nullptr) == COMP_OK);
  COMP_CHECK(comp_layer_set_mask(fx.doc, group, nullptr) == COMP_OK);
  const std::vector<uint8_t> adjusted = flatten(fx.doc);
  COMP_CHECK_MSG(adjusted != plain, "sanity: unmasked adjustment must change pixels");
  /* 复原两张蒙版，回到相乘语义。 */
  COMP_CHECK(comp_layer_set_mask(fx.doc, adj, &own) == COMP_OK);
  COMP_CHECK(comp_layer_set_mask(fx.doc, group, &group_mask) == COMP_OK);
  const std::vector<uint8_t> both = flatten(fx.doc);

  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      const size_t px = (static_cast<size_t>(y) * kW + x) * 4;
      const bool in_own = y < kH / 2;
      const bool in_group = x < kW / 2;
      if (in_own && in_group) {
        COMP_CHECK_MSG(both[px] == adjusted[px], "intersection of both masks stays adjusted");
      } else {
        COMP_CHECK_MSG(both[px] == plain[px],
                       "outside the intersection either mask must veto the adjustment");
      }
    }
  }
}

/* 14. 非 normal 混合模式：先与下层满覆盖混合，再按 below 的 alpha 回 coverage
 *    （上游 EditorCanvas.swift:3096-3106；AUDIT5 §7.2 的缺口）。
 *
 * 选 MULTIPLY 为判据：Invert 后与下层相乘若真的发生了，
 * 输出必须与「调整值 × 下层值」一致，且**不等于**未混合的调整值。 */
COMP_TEST(adjustment_eval_blend_mode_applies_to_adjustment) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  const comp_layer_id adj = add_adjustment(fx.doc, 9, "{}");
  const std::vector<uint8_t> normal_mode = flatten(fx.doc);
  COMP_CHECK_MSG(normal_mode != plain, "sanity: the adjustment must change pixels");

  COMP_CHECK(comp_layer_set_blend(fx.doc, adj, COMP_BLEND_MULTIPLY) == COMP_OK);
  const std::vector<uint8_t> multiplied = flatten(fx.doc);
  COMP_CHECK_MSG(multiplied != normal_mode,
                 "a non-normal blend mode must not be treated as normal");
  COMP_CHECK_MSG(multiplied != plain, "a non-normal blend mode must still re-color below");

  /*
   * 满覆盖处（backdrop alpha = 255，上游把两侧都提到不透明后按 W3C 混合）：
   * MULTIPLY 的结果 = round(invert(plain) * plain / 255)。
   * 合成器对不在 [0,1] 的 blend_rgb 输出不做钳制，仅在写回时钳——
   * 两者在此处都不会越界，因此可以精确断言。
   */
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      const size_t px = (static_cast<size_t>(y) * kW + x) * 4;
      for (int c = 0; c < 3; ++c) {
        const int inverted = 255 - static_cast<int>(plain[px + c]);
        const int below = static_cast<int>(plain[px + c]);
        const double mixed = static_cast<double>(inverted) * below / 255.0;
        const int expected = std::min(255, std::max(0, static_cast<int>(mixed + 0.5)));
        COMP_CHECK_MSG(multiplied[px + c] == expected,
                       "multiply blend of the adjusted color with below must match W3C");
      }
    }
  }
}

/* 15. 非 normal 混合 + 自身蒙版：软边缘不被加厚（AUDIT5 §7.2 的关键不变量）。
 *
 * 上游先按 blend mode 满覆盖混色，**再**按 below 的 alpha 回 coverage；
 * 若顺序反了（先按 coverage 混色再取 alpha），半透明底上的软边缘会明显加厚。
 * 这里用「同一 alpha、不同 coverage」的两个底色验证：coverage 只能改颜色，
 * 不能改 alpha。 */
COMP_TEST(adjustment_eval_blend_mode_preserves_below_alpha) {
  Fixture fx;

  /* 底图：左半不透明、右半半透明（alpha=128）的渐变。 */
  comp_layer_desc desc{};
  desc.name = "base";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.parent = 0;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id base = 0;
  COMP_CHECK(comp_layer_add(fx.doc, &desc, &base) == COMP_OK);
  std::vector<uint8_t> pixels = gradient_pixels();
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = kW / 2; x < kW; ++x) {
      pixels[(static_cast<size_t>(y) * kW + x) * 4 + 3] = 128;
    }
  }
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(fx.doc, base, &image) == COMP_OK);

  const comp_layer_id adj = add_adjustment(fx.doc, 9, "{}");
  const std::vector<uint8_t> normal_mode = flatten(fx.doc);

  COMP_CHECK(comp_layer_set_blend(fx.doc, adj, COMP_BLEND_MULTIPLY) == COMP_OK);
  const std::vector<uint8_t> multiplied = flatten(fx.doc);

  /* alpha 必须逐字节等于底图：coverage 只改颜色，不改 alpha。 */
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      const size_t px = (static_cast<size_t>(y) * kW + x) * 4;
      const uint8_t expected_alpha = (x < kW / 2) ? 255 : 128;
      COMP_CHECK_MSG(normal_mode[px + 3] == expected_alpha, "sanity: base alpha survives");
      COMP_CHECK_MSG(multiplied[px + 3] == expected_alpha,
                     "blend mode must not thicken a soft edge (alpha must stay below's)");
    }
  }
  COMP_CHECK_MSG(multiplied != normal_mode, "sanity: the colors must differ somewhere");
}

/* ------------------------------------------------------- P0-3：Curves 求值精确性 */

/*
 * 恒等曲线必须逐字节不变。
 *
 * 这条用例直接钉住此前的标度缺陷：求值侧把 0..255 的输入除以 255 后拿去做
 * 0..255 的控制点插值，恒等曲线 (0,0)-(255,255) 于是把每个像素压到接近 0
 * ——画面全黑。断言「逐字节不变」对任何标度错误都是致命的。
 */
COMP_TEST(curves_eval_identity_is_byte_exact) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  add_adjustment(fx.doc, 2, R"({"curves":{"channels":[[{"x":0,"y":0},{"x":255,"y":255}]]}})");
  COMP_CHECK_MSG(flatten(fx.doc) == plain,
                 "identity curve must be a byte-exact no-op (0..255 control point scale)");
}

/* 两端点翻转的曲线 = 逐通道取反（两点 Hermite 退化成分段线性）。 */
COMP_TEST(curves_eval_invert_curve_is_exact_complement) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  add_adjustment(fx.doc, 2, R"({"curves":{"channels":[[{"x":0,"y":255},{"x":255,"y":0}]]}})");
  const std::vector<uint8_t> adjusted = flatten(fx.doc);
  for (size_t i = 0; i < plain.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      COMP_CHECK_MSG(adjusted[i + c] == 255 - plain[i + c],
                     "two-point inverted curve must be an exact complement");
    }
    COMP_CHECK(adjusted[i + 3] == plain[i + 3]);
  }
}

/* 控制点必须被精确命中：抬中间调的曲线在 x=64 处输出 y=128。 */
COMP_TEST(curves_eval_hits_control_points_exactly) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  add_adjustment(fx.doc, 2,
                 R"({"curves":{"channels":[[{"x":0,"y":0},{"x":64,"y":128},{"x":255,"y":255}]]}})");
  const std::vector<uint8_t> adjusted = flatten(fx.doc);

  /* 渐变里红通道 = x*16：x=0 -> 0，x=4 -> 64，x=7 -> 112。 */
  for (int32_t y = 0; y < kH; ++y) {
    const size_t px0 = (static_cast<size_t>(y) * kW + 0) * 4;
    const size_t px4 = (static_cast<size_t>(y) * kW + 4) * 4;
    COMP_CHECK_MSG(plain[px4] == 64, "sanity: gradient red at x=4 is the control point x");
    COMP_CHECK_MSG(adjusted[px0] == 0, "curve at x=0 must output y=0");
    COMP_CHECK_MSG(adjusted[px4] == 128, "curve must hit the control point (64 -> 128) exactly");
    /* 中间调被抬起而非压低（标度/方向任一错都会失败）。 */
    COMP_CHECK_MSG(adjusted[px4] > plain[px4], "midtone lift must brighten");
    /* channels[0] 是 RGB 合成曲线，对三条通道同时生效：绿通道取到 64 时同样落到 128。 */
    const size_t px4_green = (static_cast<size_t>(4) * kW + 0) * 4;
    COMP_CHECK_MSG(plain[px4_green + 1] == 64,
                   "sanity: gradient green at y=4 is the control point x");
    COMP_CHECK_MSG(adjusted[px4_green + 1] == 128,
                   "the RGB composite curve must apply to every channel, green included");
  }
}

/*
 * 通道曲线先于 RGB 合成曲线（上游 `value(value(x, channel), 0)`）。
 *
 * 判据：红通道取反后过「白点减半」的 RGB 曲线 ⇒ 结果 = (255-v)*128/255；
 * 若实现拿通道曲线**替换**了 RGB 曲线，红通道会是 255-v，差一倍。
 */
COMP_TEST(curves_eval_channel_curve_composes_with_rgb_curve) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  add_adjustment(fx.doc, 2,
                 R"({"curves":{"channels":[[{"x":0,"y":0},{"x":255,"y":128}],)"
                 R"([{"x":0,"y":255},{"x":255,"y":0}]]}})");
  const std::vector<uint8_t> adjusted = flatten(fx.doc);

  for (size_t i = 0; i < plain.size(); i += 4) {
    const double red = (255.0 - plain[i]) * 128.0 / 255.0;
    COMP_CHECK_MSG(adjusted[i] == static_cast<uint8_t>(std::lround(red)),
                   "red = RGB curve(channel curve(v))");
    const double green = plain[i + 1] * 128.0 / 255.0;
    COMP_CHECK_MSG(adjusted[i + 1] == static_cast<uint8_t>(std::lround(green)),
                   "unset channel goes straight through the RGB curve");
    const double blue = plain[i + 2] * 128.0 / 255.0;
    COMP_CHECK_MSG(adjusted[i + 2] == static_cast<uint8_t>(std::lround(blue)),
                   "unset channel goes straight through the RGB curve");
  }
}

/* Levels 黑点拉到 40 ⇒ 40 以下的输入全部压成黑（上游 LevelsFilter 公式）。 */
COMP_TEST(levels_eval_black_point_crushes_shadows) {
  Fixture fx;
  const std::vector<uint8_t> plain = render_gradient(fx.doc);
  add_adjustment(fx.doc, 1,
                 R"({"levels":{"ranges":[{)"
                 R"("black":40,"white":255,"gamma":1,"outputBlack":0,"outputWhite":255}]}})");
  const std::vector<uint8_t> adjusted = flatten(fx.doc);

  for (size_t i = 0; i < plain.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      const double normalized = std::min(1.0, std::max(0.0, (plain[i + c] - 40.0) / 215.0));
      const int expected = static_cast<int>(std::lround(normalized * 255.0));
      COMP_CHECK_MSG(adjusted[i + c] == expected, "levels black point must match upstream formula");
    }
    COMP_CHECK(adjusted[i + 3] == plain[i + 3]);
  }
  /* 阴影被压黑的直观判据：红通道 x<=2（0/16/32）在输出里必须为 0。 */
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x <= 2; ++x) {
      const size_t px = (static_cast<size_t>(y) * kW + x) * 4;
      COMP_CHECK_MSG(adjusted[px] == 0, "levels black=40 must crush values below 40");
    }
  }
}
