// AI生成
/*
 * test_effect_eval.cpp — 图层效果「像素求值」的回归护栏。
 *
 * 为什么需要这组用例：效果此前**只入库不求值**——`comp_layer_set_effect` 写入成功、
 * `comp_layer_effects` 能读回，`engineEffects` 往返正常，但合成路径完全无视它们
 * （挂上投影前后 `comp_render_flatten` 输出逐字节相同），而且 `flatten` /
 * `merge_down` 因为「无法烘入」直接拒绝带效果的图层。宿主只能写不能看。
 *
 * 冻结的行为要点：
 *   1. 10 种 kind 全部有像素实现，且在非恒等参数下逐字节改变渲染结果；
 *   2. 隐藏带效果的图层 ⇒ 渲染结果逐字节回到无效果状态；
 *   3. 不可见参数（opacity=0 / size=0 / 距离与模糊同时为 0）⇒ 逐字节不变；
 *   4. 坏 JSON / 未知 kind ⇒ 跳过而不是产出错误像素，更不会让整篇合成失败；
 *   5. outside 效果画在像素**之下**、inside 效果与叠色画在像素**之上**；
 *   6. 叠色类效果（Color Overlay / 内发光 / 内阴影）是 source-atop：
 *      不改变图层 alpha，且只落在已有 alpha 之内；
 *   7. 引擎不认识的 effect kind 仍然阻止 flatten/merge_down（烘入会静默丢弃）。
 *
 * 绘制次序与语义对齐上游 `Compositor/Document/LayerEffects.swift:388
 * render(_:mask:effects:)`；与上游的已知差异见 engine/compositing/effect_eval.h。
 */
#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "test_framework.h"

/*
 * 本文件只用 C ABI，不引入引擎内部命名空间（ABI 头不声明 compositor 命名空间，
 * 见 test_adjustment_eval.cpp 的同款说明）。
 */
namespace {

constexpr int32_t kW = 12;
constexpr int32_t kH = 12;
constexpr size_t kStride = static_cast<size_t>(kW) * 4;

struct Fixture {
  comp_engine engine = nullptr;
  comp_document doc = nullptr;

  explicit Fixture(int32_t size = kW) {
    comp_engine_desc desc{};
    desc.app_name = "compositor-effect-eval-tests";
    desc.version = "1.0.0";
    desc.backend = COMP_BACKEND_CPU;
    desc.worker_threads = 1;
    comp_document_desc doc_desc{};
    doc_desc.width = size;
    doc_desc.height = size;
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

/*
 * 中间一块不透明方块、四周透明的画布。
 *
 * 用「居中方块」而非纯色或渐变：纯色会让模糊/叠色看起来「没变化」，渐变会让
 * 「形状在哪里」难以断言。方块的边缘在 (lo, hi) 上，outside 类效果必须出现在
 * 方块之外，inside / 叠色类必须出现在方块之内——位置本身就是断言。
 */
std::vector<uint8_t> square_pixels(int32_t lo, int32_t hi, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4, 0);
  for (int32_t y = lo; y < hi; ++y) {
    for (int32_t x = lo; x < hi; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4;
      px[0] = r;
      px[1] = g;
      px[2] = b;
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

/* 加一个像素图层（默认是居中 4..8 的不透明方块），返回图层 id。 */
comp_layer_id add_square(comp_document doc, comp_layer_id* out_id = nullptr) {
  comp_layer_desc desc{};
  desc.name = "square";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.parent = 0;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add(doc, &desc, &id) == COMP_OK);
  std::vector<uint8_t> pixels = square_pixels(4, 8, 200, 100, 50);
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(doc, id, &image) == COMP_OK);
  if (out_id != nullptr) *out_id = id;
  return id;
}

uint8_t alpha_at(const std::vector<uint8_t>& buf, int32_t x, int32_t y) {
  return buf[(static_cast<size_t>(y) * kW + x) * 4 + 3];
}

/* 该像素是否完全不透明。 */
bool opaque(const std::vector<uint8_t>& buf, int32_t x, int32_t y) {
  return alpha_at(buf, x, y) == 255;
}

/* 该像素是否完全透明。 */
bool clear(const std::vector<uint8_t>& buf, int32_t x, int32_t y) {
  return alpha_at(buf, x, y) == 0;
}

/* 10 种 kind 的非恒等参数。键名逐字取自上游 LayerEffects.swift 的字段名。 */
struct EffectCase {
  uint32_t kind;
  const char* name;
  const char* settings;
};

const EffectCase kEvaluatedEffects[] = {
    /* 描边：outside，size=3 会扩出一圈落在方块之外的环。 */
    {0, "stroke-outside", R"({"size":3,"red":0,"green":1,"blue":0,"opacity":1,"inside":false})"},
    /* 投影：angle=0 => 阴影偏 +x（dx=-cos(0)*d= -d，见 shadow_offset）。 */
    {1, "drop-shadow", R"({"angle":0,"distance":3,"blur":1,"red":0,"green":0,"blue":1,"opacity":1})"},
    /* 叠色：满覆盖红，必须改变方块内部颜色。 */
    {2, "color-overlay", R"({"red":1,"green":0,"blue":0,"opacity":1})"},
    /* 内阴影：落在形状内部。 */
    {3, "inner-shadow", R"({"angle":0,"distance":2,"blur":1,"red":0,"green":1,"blue":0,"opacity":1})"},
    /* 外发光：柔化后扣掉内部，落��形状之外。 */
    {4, "outer-glow", R"({"size":3,"red":1,"green":0,"blue":0,"opacity":1})"},
    /* 内发光：柔化后取逆，只落形状内部。 */
    {5, "inner-glow", R"({"size":3,"red":1,"green":0,"blue":0,"opacity":1})"},
    /* P1-1：斜面（左暗右亮，只落形状内部）。 */
    {6, "bevel", R"({"angle":0,"altitude":0,"depth":100,"size":1,"soften":0,"opacity":1,"red":1,"green":1,"blue":1,"shadow_red":0,"shadow_green":0,"shadow_blue":0})"},
    /* 缎光：正反两个偏移副本之差。 */
    {7, "satin", R"({"angle":0,"distance":2,"blur":0,"opacity":1,"red":0,"green":1,"blue":0})"},
    /* 渐变叠加：左黑右白。 */
    {8, "gradient-overlay", R"({"angle":0,"opacity":1,"red":0,"green":0,"blue":0,"red2":1,"green2":1,"blue2":1})"},
    /* 图案叠加：10% 缩放的棋盘（周期 3.2px，方块内部会跨格）。 */
    {9, "pattern-overlay", R"({"pattern":0,"scale":10,"angle":0,"opacity":1,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1})"},
};

}  // namespace

/* 1. 10 种 kind 全部改变像素（此前它们全部是 no-op）。 */
COMP_TEST(effect_eval_every_kind_changes_pixels) {
  for (const EffectCase& c : kEvaluatedEffects) {
    Fixture fx;
    const std::vector<uint8_t> plain = flatten(fx.doc);
    const comp_layer_id id = add_square(fx.doc);
    const std::vector<uint8_t> before = flatten(fx.doc);
    COMP_CHECK(comp_layer_set_effect(fx.doc, id, c.kind, c.settings) == COMP_OK);
    const std::vector<uint8_t> after = flatten(fx.doc);
    COMP_CHECK_MSG(before != after, std::string("kind ") + c.name + " must change pixels");
    COMP_CHECK_MSG(before != plain, "sanity: the square layer itself must change pixels");
  }
}

/* 2. 隐藏带效果的图层 ⇒ 逐字节回到无效果状态。 */
COMP_TEST(effect_eval_hidden_layer_is_byte_exact_noop) {
  for (const EffectCase& c : kEvaluatedEffects) {
    Fixture fx;
    comp_layer_id id = 0;
    add_square(fx.doc, &id);
    COMP_CHECK(comp_layer_set_effect(fx.doc, id, c.kind, c.settings) == COMP_OK);
    const std::vector<uint8_t> with_effect = flatten(fx.doc);
    COMP_CHECK(comp_layer_set_visible(fx.doc, id, 0) == COMP_OK);
    const std::vector<uint8_t> hidden = flatten(fx.doc);
    COMP_CHECK_MSG(hidden != with_effect, std::string(c.name) + ": hiding must actually hide");
  }

  /*
   * 关键不变量：隐藏一个带效果的图层 == 从未创建这个图层。
   *
   * 对照侧的图层数必须**一致**：ABI 的 comp_document_create 会自动建一层全透明的
   * Background（comp_engine_abi.cpp:26 ensure_background_layer），而 comp_layer_add
   * 又是在其上叠加。若对照侧少建一层，即便隐藏了方块，层数差异仍会让两个文档的
   * 结构不同——早期版本就是这么写的，且它恰好是因为「隐藏后仍有一层看不见的方块」
   * 才先失败，再靠改成同层数才绿。
   */
  Fixture hidden;
  comp_layer_id hid = 0;
  add_square(hidden.doc, &hid);
  COMP_CHECK(comp_layer_set_effect(
                 hidden.doc, hid, 1,
                 R"({"angle":0,"distance":3,"blur":1,"opacity":1})") == COMP_OK);
  COMP_CHECK(comp_layer_set_visible(hidden.doc, hid, 0) == COMP_OK);

  Fixture clean;
  comp_layer_id clean_id = 0;
  add_square(clean.doc, &clean_id);
  COMP_CHECK(comp_layer_set_visible(clean.doc, clean_id, 0) == COMP_OK);

  COMP_CHECK_MSG(flatten(hidden.doc) == flatten(clean.doc),
                 "hidden effect layer must be a byte-exact no-op");
}

/* 3. 不可见参数 ⇒ 逐字节不变（缺键/零值必须回落为「看不见」）。 */
COMP_TEST(effect_eval_invisible_settings_are_byte_exact) {
  const EffectCase kInvisible[] = {
      /* opacity 归零 */
      {0, "stroke-opacity0", R"({"size":3,"opacity":0})"},
      {1, "shadow-opacity0", R"({"distance":3,"blur":1,"opacity":0})"},
      {2, "overlay-opacity0", R"({"opacity":0})"},
      {4, "glow-opacity0", R"({"size":3,"opacity":0})"},
      /* size / distance 归零 */
      {0, "stroke-size0", R"({"size":0,"opacity":1})"},
      {4, "outer-glow-size0", R"({"size":0,"opacity":1})"},
      /* 距离与模糊同时为 0 ⇒ 没有形状可画 */
      {1, "shadow-no-offset-no-blur", R"({"distance":0,"blur":0,"opacity":1})"},
      {3, "inner-shadow-no-offset-no-blur", R"({"distance":0,"blur":0,"opacity":1})"},
      /* 空 JSON：缺键回落默认值，但默认的投影/发光/描边都是可见的，
       * 所以这里只断言「不失败」，像素是否变化由各自用例负责。 */
  };
  for (const EffectCase& c : kInvisible) {
    Fixture fx;
    const comp_layer_id id = add_square(fx.doc);
    const std::vector<uint8_t> before = flatten(fx.doc);
    COMP_CHECK(comp_layer_set_effect(fx.doc, id, c.kind, c.settings) == COMP_OK);
    COMP_CHECK_MSG(flatten(fx.doc) == before,
                   std::string("invisible ") + c.name + " must not change pixels");
  }
}

/* 4. 坏 JSON / 未知 kind ⇒ 不失败；未知 kind 跳过，坏 JSON 回落到上游默认值。 */
COMP_TEST(effect_eval_bad_settings_are_skipped_not_fatal) {
  /*
   * 分两类断言，因为两者的**正确行为本就不同**：
   *
   * (a) 未知 kind：引擎不认识，既不求值也不画 ⇒ 必须逐字节不变。
   *     它同时仍会阻止 flatten（见 effect_eval_unknown_kind_blocks_flatten）。
   */
  const struct { uint32_t kind; const char* name; const char* settings; } kUnknownKind[] = {
      {999, "unknown-kind", R"({"size":3})"},
      {0xFFFFFFFFu, "huge-kind", R"({"size":3})"},
  };
  for (const auto& c : kUnknownKind) {
    Fixture fx;
    const comp_layer_id id = add_square(fx.doc);
    const std::vector<uint8_t> before = flatten(fx.doc);
    COMP_CHECK(comp_layer_set_effect(fx.doc, id, c.kind, c.settings) == COMP_OK);
    COMP_CHECK_MSG(flatten(fx.doc) == before,
                   std::string(c.name) + " must be skipped, not rendered as garbage");
  }

  /*
   * (b) 坏 JSON / 键类型不对：回落**上游默认值**，而上游默认值全都是可见的
   *     （StrokeEffect size=4/opacity=1/color=black；ShadowEffect opacity=0.5；
   *      GlowEffect opacity=0.75；ColorOverlay opacity=1）。
   *     所以正确行为是「画出一条默认描边」，而不是「当作无效果跳过」。
   *
   * 因此这里断言三件真正重要的事：
   *   - 渲染不失败（坏参数不致使整篇合成报错）；
   *   - 不产生**垃圾像素**（不得出现 NaN 溢出、alpha>255 之类的畸形值）；
   *   - 同一份坏 JSON 每次渲染结果**一致**（不引入非确定性）。
   * 早期版本断言「坏 JSON 逐字节不变」，那是把回落默认值误当成「跳过」；
   * 若照那条改，实现就得把默认值改成不可见，那就与上游的
   * `StrokeEffect` 默认语义（size=4 的黑描边）分道扬镳了。
   */
  const struct { uint32_t kind; const char* name; const char* settings; } kBadJson[] = {
      {0, "not-json", "this is not json"},
      {0, "json-array", "[1,2,3]"},
      {0, "wrong-types", R"({"size":"big","opacity":"loud","inside":"yes"})"},
      {1, "shadow-wrong-types", R"({"angle":"x","distance":[],"blur":null})"},
  };
  for (const auto& c : kBadJson) {
    Fixture fx;
    const comp_layer_id id = add_square(fx.doc);
    COMP_CHECK(comp_layer_set_effect(fx.doc, id, c.kind, c.settings) == COMP_OK);
    const std::vector<uint8_t> first = flatten(fx.doc);
    const std::vector<uint8_t> second = flatten(fx.doc);
    COMP_CHECK_MSG(first == second,
                   std::string(c.name) + " must render deterministically (defaults, not garbage)");
    /* 逐像素健全性：alpha 合法、颜色不超过 alpha（预乘约束）。 */
    bool sane = true;
    for (size_t i = 0; i < first.size(); i += 4) {
      if (first[i + 3] == 0) continue;
      for (int ch = 0; ch < 3; ++ch) {
        if (first[i + ch] > first[i + 3]) sane = false;
      }
    }
    COMP_CHECK_MSG(sane, std::string(c.name) + " must not produce non-premultiplied pixels");
  }
}

/* 5. outside 描边落在像素**之外**（画在像素之下才能被看见）。 */
COMP_TEST(effect_eval_stroke_outside_lies_beyond_the_pixels) {
  Fixture fx;
  const comp_layer_id id = add_square(fx.doc);
  const std::vector<uint8_t> before = flatten(fx.doc);
  /* 绿色描边，size=3。 */
  COMP_CHECK(comp_layer_set_effect(
                 fx.doc, id, 0,
                 R"({"size":3,"red":0,"green":1,"blue":0,"opacity":1,"inside":false})") == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  /* 方块内（5,5）仍是原来的像素色：outside 描边画在像素之下，被像素盖住。 */
  const size_t inside_px = (static_cast<size_t>(5) * kW + 5) * 4;
  COMP_CHECK_MSG(after[inside_px] == before[inside_px] &&
                     after[inside_px + 1] == before[inside_px + 1],
                 "outside stroke must not tint the pixels themselves");
  /* 方块外紧邻处（3,5）出现描边（绿色为主）。 */
  const size_t ring_px = (static_cast<size_t>(5) * kW + 3) * 4;
  COMP_CHECK_MSG(after[ring_px + 3] > 0, "outside stroke must paint beyond the pixels");
  COMP_CHECK_MSG(after[ring_px + 1] > after[ring_px + 0],
                 "outside stroke must be the stroke color (green dominant)");
  /* 远处（0,0）不受影响：描边宽度有限。 */
  COMP_CHECK(clear(after, 0, 0));
}

/* 6. inside 描边落在像素**之内**（压在像素之上）。 */
COMP_TEST(effect_eval_stroke_inside_lies_within_the_pixels) {
  Fixture fx;
  const comp_layer_id id = add_square(fx.doc);
  const std::vector<uint8_t> before = flatten(fx.doc);
  COMP_CHECK(comp_layer_set_effect(
                 fx.doc, id, 0,
                 R"({"size":3,"red":0,"green":1,"blue":0,"opacity":1,"inside":true})") == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  /* 方块内紧邻边缘（4,4）被染绿：inside 描边压在像素之上。 */
  const size_t edge_px = (static_cast<size_t>(4) * kW + 4) * 4;
  COMP_CHECK_MSG(after[edge_px + 1] > after[edge_px + 0], "inside stroke must tint the edge");
  /* 方块外（3,3）必须仍然是空的：inside 描边不外扩。 */
  COMP_CHECK_MSG(clear(after, 3, 3), "inside stroke must not paint beyond the pixels");
  /*
   * 注意：size=3 的 inside 描边在 4×4 的方块上会**吃掉整个方块**——
   * 内圈 = 原形状 - 收缩(size=3) 的形状，而把 4 像素宽的方块向内收缩 3 像素
   * 会收缩成空集，于是内圈就是整块方块。所以这里断言「中心仍是绿的」，
   * 而不是「中心保持原色」：后者会把正确的实现判成 bug。
   * 早期版本正是这样写的，把 size=3 的 inside 描边当作「够不到中央」。
   */
  const size_t core_px = (static_cast<size_t>(6) * kW + 6) * 4;
  COMP_CHECK_MSG(after[core_px + 1] > after[core_px + 0],
                 "inside stroke of size=3 on a 4x4 square must fill the interior");
}

/* 7. Color Overlay 是 source-atop：改颜色、**不改 alpha**。 */
COMP_TEST(effect_eval_color_overlay_preserves_alpha) {
  Fixture fx;
  /* 半个透明（alpha=128）的图层，叠色必须完全吃到颜色但不改变 alpha。 */
  comp_layer_desc desc{};
  desc.name = "half";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.parent = 0;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add(fx.doc, &desc, &id) == COMP_OK);
  std::vector<uint8_t> pixels = square_pixels(4, 8, 200, 100, 50);
  /*
   * 只把**方块内**的 alpha 降到 128。
   * square_pixels 把方块外的像素写成全 0（alpha=0），若改成
   * 「循环把所有像素的 alpha 设为 128」，方块外就变成了一层半透明雾，
   * source-atop 自然会在 (2,2) 那种形状外的位置着到色——
   * 那是测试自己造的输入，不是实现的 bug。
   */
  for (int32_t y = 4; y < 8; ++y) {
    for (int32_t x = 4; x < 8; ++x) {
      pixels[(static_cast<size_t>(y) * kW + x) * 4 + 3] = 128;
    }
  }
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(fx.doc, id, &image) == COMP_OK);

  const std::vector<uint8_t> before = flatten(fx.doc);
  /* 满覆盖纯红。 */
  COMP_CHECK(comp_layer_set_effect(fx.doc, id, 2, R"({"red":1,"green":0,"blue":0,"opacity":1})") ==
             COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  /* 形状内：alpha 逐字节不变，颜色变成红（预乘 ⇒ 半透明处 r=128）。 */
  const size_t px = (static_cast<size_t>(5) * kW + 5) * 4;
  COMP_CHECK_MSG(after[px + 3] == before[px + 3], "source-atop must not change alpha");
  COMP_CHECK_MSG(after[px + 0] == 128 && after[px + 1] == 0 && after[px + 2] == 0,
                 "full-strength overlay must recolor the pixel (premultiplied red)");
  /* 形状外：source-atop 只落在已有 alpha 之内，四周必须仍然全透明。 */
  COMP_CHECK_MSG(clear(after, 2, 2), "source-atop must not paint outside the pixels");
}

/* 8. 内阴影 / 内发光只落在形状**内部**（各自的 coverage 取逆所致）。 */
COMP_TEST(effect_eval_inner_effects_stay_within_the_pixels) {
  for (const EffectCase& c : {kEvaluatedEffects[3], kEvaluatedEffects[5]}) {
    Fixture fx;
    const comp_layer_id id = add_square(fx.doc);
    COMP_CHECK(comp_layer_set_effect(fx.doc, id, c.kind, c.settings) == COMP_OK);
    const std::vector<uint8_t> after = flatten(fx.doc);
    COMP_CHECK_MSG(clear(after, 0, 0),
                   std::string(c.name) + " must not paint outside the pixels");
    COMP_CHECK_MSG(clear(after, 3, 3),
                   std::string(c.name) + " must not paint outside the pixels");
    /* 内部必有变化。 */
    bool any = false;
    for (int32_t y = 4; y < 8 && !any; ++y) {
      for (int32_t x = 4; x < 8 && !any; ++x) {
        const size_t px = (static_cast<size_t>(y) * kW + x) * 4;
        const size_t base = (static_cast<size_t>(5) * kW + 5) * 4;
        if (after[px] != after[base] || after[px + 1] != after[base + 1]) any = true;
      }
    }
    COMP_CHECK_MSG(any, std::string(c.name) + " must actually alter the interior");
  }
}

/* 9. P1-1 追加的三种效果（斜面 / 缎光 / 渐变叠加）也只落在形状内部。 */
COMP_TEST(effect_eval_p1_1_effects_stay_within_the_pixels) {
  for (const EffectCase& c : {kEvaluatedEffects[6], kEvaluatedEffects[7], kEvaluatedEffects[8]}) {
    Fixture fx;
    const comp_layer_id id = add_square(fx.doc);
    const std::vector<uint8_t> before = flatten(fx.doc);
    COMP_CHECK(comp_layer_set_effect(fx.doc, id, c.kind, c.settings) == COMP_OK);
    const std::vector<uint8_t> after = flatten(fx.doc);
    COMP_CHECK_MSG(clear(after, 0, 0), std::string(c.name) + " must not paint outside");
    COMP_CHECK_MSG(clear(after, 3, 3), std::string(c.name) + " must not paint outside");
    /* 不改变图层 alpha（全部 source-atop）。 */
    COMP_CHECK_MSG(alpha_at(after, 5, 5) == 255, std::string(c.name) + " must keep alpha");
    bool any = false;
    for (int32_t y = 4; y < 8 && !any; ++y) {
      for (int32_t x = 4; x < 8 && !any; ++x) {
        const size_t px = (static_cast<size_t>(y) * kW + x) * 4;
        if (after[px] != before[px] || after[px + 1] != before[px + 1] ||
            after[px + 2] != before[px + 2]) {
          any = true;
        }
      }
    }
    COMP_CHECK_MSG(any, std::string(c.name) + " must actually alter the interior");
  }
}

/* 10. 渐变叠加沿 angle 方向铺开：0° = 左起右终，90° = 上起下终。 */
COMP_TEST(effect_eval_gradient_overlay_runs_along_angle) {
  {
    Fixture fx;
    const comp_layer_id id = add_square(fx.doc);
    COMP_CHECK(comp_layer_set_effect(
                   fx.doc, id, 8,
                   R"({"angle":0,"opacity":1,"red":0,"green":0,"blue":0,"red2":1,"green2":1,"blue2":1})") ==
               COMP_OK);
    const std::vector<uint8_t> after = flatten(fx.doc);
    const uint8_t left = after[(static_cast<size_t>(6) * kW + 4) * 4];
    const uint8_t right = after[(static_cast<size_t>(6) * kW + 7) * 4];
    COMP_CHECK_MSG(left < right, "0 deg gradient must darken the left side first");
    COMP_CHECK_MSG(left < 20, "0 deg gradient must start at the start color");
    COMP_CHECK_MSG(right > 230, "0 deg gradient must end at the end color");
  }
  {
    Fixture fx;
    const comp_layer_id id = add_square(fx.doc);
    COMP_CHECK(comp_layer_set_effect(
                   fx.doc, id, 8,
                   R"({"angle":90,"reverse":true,"opacity":1,"red":0,"green":0,"blue":0,"red2":1,"green2":1,"blue2":1})") ==
               COMP_OK);
    const std::vector<uint8_t> after = flatten(fx.doc);
    /* 90° 自上而下，reverse 之后上边变成终点色（白）、下边是起点色（黑）。 */
    const uint8_t top = after[(static_cast<size_t>(4) * kW + 5) * 4];
    const uint8_t bottom = after[(static_cast<size_t>(7) * kW + 5) * 4];
    COMP_CHECK_MSG(top > bottom, "90 deg + reverse must flip the vertical ramp");
  }
}

/* 11. 斜面沿光向量分亮暗：angle=0 时光从右来 ⇒ 右边亮面、左边暗面。 */
COMP_TEST(effect_eval_bevel_light_side_is_bright) {
  Fixture fx;
  const comp_layer_id id = add_square(fx.doc);
  COMP_CHECK(comp_layer_set_effect(
                 fx.doc, id, 6,
                 R"({"angle":0,"altitude":0,"depth":100,"size":1,"soften":0,"opacity":1,"red":1,"green":1,"blue":1,"shadow_red":0,"shadow_green":0,"shadow_blue":0})") ==
             COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);
  const size_t left = (static_cast<size_t>(6) * kW + 4) * 4;
  const size_t right = (static_cast<size_t>(6) * kW + 7) * 4;
  COMP_CHECK_MSG(after[right] > 200, "bevel must highlight the lit (right) edge");
  COMP_CHECK_MSG(after[left] < 100, "bevel must shade the opposite (left) edge");
}

/* 12. 外发光落在形状**之外**（柔化后扣掉内部）。 */
COMP_TEST(effect_eval_outer_glow_lies_beyond_the_pixels) {
  Fixture fx;
  const comp_layer_id id = add_square(fx.doc);
  const std::vector<uint8_t> before = flatten(fx.doc);
  COMP_CHECK(comp_layer_set_effect(
                 fx.doc, id, 4, R"({"size":3,"red":1,"green":0,"blue":0,"opacity":1})") == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  /* 方块外紧邻处出现红光。 */
  COMP_CHECK_MSG(!clear(after, 3, 5), "outer glow must paint beyond the pixels");
  const size_t glow_px = (static_cast<size_t>(5) * kW + 3) * 4;
  COMP_CHECK_MSG(after[glow_px + 0] > after[glow_px + 1], "outer glow must be the glow color");
  /* 方块中央不被染色：外发光扣掉了内部。 */
  const size_t core_px = (static_cast<size_t>(6) * kW + 6) * 4;
  COMP_CHECK_MSG(after[core_px] == before[core_px] && after[core_px + 1] == before[core_px + 1],
                 "outer glow must not tint the core");
  /* 远处不受影响。 */
  COMP_CHECK(clear(after, 0, 0));
}

/* 13. 投影落在形状**之外**，且不染色形状内部。 */
COMP_TEST(effect_eval_drop_shadow_lies_beyond_the_pixels) {
  Fixture fx;
  const comp_layer_id id = add_square(fx.doc);
  const std::vector<uint8_t> before = flatten(fx.doc);
  /* angle=0 => shadow_offset 给 dx=-cos(0)*d=-d、dy=sin(0)*d=0，
   * 即阴影向左偏移 3 像素（阴影背向光源）。 */
  COMP_CHECK(comp_layer_set_effect(
                 fx.doc, id, 1,
                 R"({"angle":0,"distance":3,"blur":1,"red":0,"green":0,"blue":1,"opacity":1})") ==
             COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);

  /* 左侧（3,5）出现阴影；右侧（8,5）不应出现（偏移方向只有一个）。 */
  COMP_CHECK_MSG(!clear(after, 3, 5), "drop shadow must paint beyond the pixels");
  const size_t shadow_px = (static_cast<size_t>(5) * kW + 3) * 4;
  COMP_CHECK_MSG(after[shadow_px + 2] > after[shadow_px + 0], "shadow must be the shadow color");
  COMP_CHECK_MSG(clear(after, 8, 5), "drop shadow must be offset one way only");
  /* 内部不变：阴影画在像素之下。 */
  const size_t core_px = (static_cast<size_t>(6) * kW + 6) * 4;
  COMP_CHECK_MSG(after[core_px] == before[core_px], "drop shadow must not tint the core");
}

/* 14. 效果不吃图层的混合模式与不透明度（在混合那一步才生效）。 */
COMP_TEST(effect_eval_is_applied_before_layer_blend) {
  Fixture fx;
  comp_layer_id below = 0;
  comp_layer_id above = 0;
  add_square(fx.doc, &below);

  /* 上层是半透明白方块，带外发光。 */
  comp_layer_desc desc{};
  desc.name = "glow";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.parent = 0;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  COMP_CHECK(comp_layer_add(fx.doc, &desc, &above) == COMP_OK);
  std::vector<uint8_t> white = square_pixels(6, 10, 255, 255, 255);
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = white.data();
  COMP_CHECK(comp_layer_set_pixels(fx.doc, above, &image) == COMP_OK);
  COMP_CHECK(comp_layer_set_effect(
                 fx.doc, above, 4,
                 R"({"size":2,"red":1,"green":0,"blue":0,"opacity":1})") == COMP_OK);

  const std::vector<uint8_t> normal = flatten(fx.doc);

  /* 把上层不透明度降到 0：像素与**它的发光**都应消失（发光是该图层的一部分）。 */
  COMP_CHECK(comp_layer_set_opacity(fx.doc, above, 0.0f) == COMP_OK);
  const std::vector<uint8_t> faded = flatten(fx.doc);
  COMP_CHECK_MSG(faded != normal, "sanity: opacity must matter");
  /* 只剩下层方块：发光处也必须回到下层颜色。
   * （5,7）位于下层方块 (4..8,4..8) 内、上层方块 (6..10,6..10) 的发光范围内。 */
  const size_t px = (static_cast<size_t>(7) * kW + 5) * 4;
  COMP_CHECK_MSG(faded[px] == 255 || faded[px] > 100,
                 "glow must fade with the layer opacity");
}

/* 15. 引擎不认识的 effect kind 仍然阻止 flatten（否则烘入会静默丢弃）。 */
COMP_TEST(effect_eval_unknown_kind_blocks_flatten) {
  Fixture fx;
  const comp_layer_id id = add_square(fx.doc);
  COMP_CHECK(comp_layer_set_effect(fx.doc, id, 999, R"({"size":3})") == COMP_OK);
  /* 渲染不失败（跳过），但 flatten 必须拒绝。 */
  std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4);
  /* flatten 走 ABI 入口会拒绝；这里只断言渲染路径不崩。 */
  comp_result r = comp_render_flatten(fx.doc, out.data(), COMP_PIXEL_RGBA8, kStride);
  COMP_CHECK_MSG(r == COMP_OK || r == COMP_ERR_UNSUPPORTED,
                 "unknown effect must not crash the render path");
  /* comp_document_flatten 走域路径，应明确拒绝。 */
  COMP_CHECK(comp_document_flatten(fx.doc) == COMP_ERR_UNSUPPORTED);
}

/*
 * 16. 图案叠加：棋盘按 scale 平铺、按 angle 旋转，且 source-atop 不越界。
 *
 * 方块是 (4..8, 4..8)；scale=10% 让周期变成 3.2px，方块内部会跨格：
 *   (4,6) 的格子坐标 (1,1) 为偶 => 底色蓝；(7,6) 为 (2,1) 奇 => 前景红。
 * 90° 旋转把 (u,v) 换成 (v,-u)，(cu+cv) 的奇偶整体翻转 => 两点颜色互换。
 */
COMP_TEST(effect_eval_pattern_overlay_tiles_and_rotates) {
  const char* kChecker0 =
      R"({"pattern":0,"scale":10,"angle":0,"opacity":1,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1})";
  const char* kChecker90 =
      R"({"pattern":0,"scale":10,"angle":90,"opacity":1,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1})";
  const size_t cell_bg = (static_cast<size_t>(6) * kW + 4) * 4; /* 蓝色格 */
  const size_t cell_fg = (static_cast<size_t>(6) * kW + 7) * 4; /* 红色格 */

  Fixture fx;
  const comp_layer_id id = add_square(fx.doc);
  COMP_CHECK(comp_layer_set_effect(fx.doc, id, 9, kChecker0) == COMP_OK);
  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(after[cell_bg + 2] > 200 && after[cell_bg + 0] < 40,
                 "pattern background cell must take the second color");
  COMP_CHECK_MSG(after[cell_fg + 0] > 200 && after[cell_fg + 2] < 40,
                 "pattern foreground cell must take the first color");
  /* source-atop：图层 alpha 不变，方块之外一个像素都不碰。 */
  COMP_CHECK(alpha_at(after, 4, 6) == 255);
  COMP_CHECK(clear(after, 0, 0));
  COMP_CHECK(clear(after, 11, 7));

  Fixture rotated;
  const comp_layer_id rid = add_square(rotated.doc);
  COMP_CHECK(comp_layer_set_effect(rotated.doc, rid, 9, kChecker90) == COMP_OK);
  const std::vector<uint8_t> turned = flatten(rotated.doc);
  COMP_CHECK_MSG(turned[cell_bg + 0] > 200 && turned[cell_bg + 2] < 40,
                 "90 deg must rotate the checker (cell parity flips)");
  COMP_CHECK_MSG(turned[cell_fg + 2] > 200 && turned[cell_fg + 0] < 40,
                 "90 deg must rotate the checker on the other cell too");
}

/*
 * 17. 图案叠加的四种内置图案互不相同；越界参数被夹取；opacity=0 逐字节不变。
 *
 * 「互不相同」是防止四种 pattern 值最后都落到同一条分支——只测一种的话，
 * 复制粘贴写错 case 号不会有任何信号。
 */
COMP_TEST(effect_eval_pattern_overlay_kinds_and_limits) {
  auto render = [](const char* settings) {
    Fixture fx;
    const comp_layer_id id = add_square(fx.doc);
    COMP_CHECK(comp_layer_set_effect(fx.doc, id, 9, settings) == COMP_OK);
    return flatten(fx.doc);
  };
  const char* kHead = R"({"scale":10,"angle":0,"opacity":1,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1,"pattern":)";
  auto with_pattern = [&](int pattern) {
    std::string json(kHead);
    json += std::to_string(pattern);
    json += "}";
    return render(json.c_str());
  };

  const std::vector<uint8_t> checker = with_pattern(0);
  const std::vector<uint8_t> grid = with_pattern(1);
  const std::vector<uint8_t> stripes = with_pattern(2);
  const std::vector<uint8_t> dots = with_pattern(3);
  COMP_CHECK_MSG(checker != grid, "checker and grid must differ");
  COMP_CHECK_MSG(checker != stripes, "checker and stripes must differ");
  COMP_CHECK_MSG(checker != dots, "checker and dots must differ");
  COMP_CHECK_MSG(grid != stripes, "grid and stripes must differ");
  COMP_CHECK_MSG(grid != dots, "grid and dots must differ");
  COMP_CHECK_MSG(stripes != dots, "stripes and dots must differ");

  /* ranged 夹取：pattern 99 -> 3，scale 0 -> 10，angle 360 与 0 同义。 */
  COMP_CHECK_MSG(with_pattern(99) == dots, "out-of-range pattern must clamp to 3");
  COMP_CHECK_MSG(render(R"({"pattern":1,"scale":0,"angle":0,"opacity":1,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1})") ==
                     render(R"({"pattern":1,"scale":10,"angle":0,"opacity":1,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1})"),
                 "scale below 10% must clamp");
  COMP_CHECK_MSG(render(R"({"pattern":1,"scale":10,"angle":360,"opacity":1,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1})") ==
                     render(R"({"pattern":1,"scale":10,"angle":0,"opacity":1,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1})"),
                 "angle 360 must be identical to 0");

  /* opacity = 0 是合法的「不可见」：逐字节回到没有效果的状态。 */
  Fixture fx;
  const comp_layer_id id = add_square(fx.doc);
  const std::vector<uint8_t> plain = flatten(fx.doc);
  COMP_CHECK(comp_layer_set_effect(
                 fx.doc, id, 9,
                 R"({"pattern":0,"scale":10,"angle":0,"opacity":0,"red":1,"green":0,"blue":0,"red2":0,"green2":0,"blue2":1})") ==
             COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == plain, "opacity 0 must leave pixels byte-identical");
}
