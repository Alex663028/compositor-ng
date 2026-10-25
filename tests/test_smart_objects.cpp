// AI生成
/*
 * test_smart_objects.cpp — P1-2 收尾 ⑦：智能对象 / 智能滤镜的端到端测试。
 *
 * 判据是可观测事实，不是「返回 COMP_OK」：
 *   1. 创建是**幂等**的：第二次调用 out_changed = 0、不落撤销步；画面逐字节不变；
 *      source 与 pixels 共享同一份缓冲（转换本身不复制像素）；
 *   2. 创建拒绝：组层 / 无像素 / HDR / 锁定的层（都在开事务之前，不留撤销步）；
 *   3. 滤镜栈：add / set / remove 各一步撤销；同参数重设不落撤销步；恒等参数也
 *      入栈（画面不变但栈变了）；
 *   4. 停用条目**留在栈里但不参与求值**：set(enabled = 0) 后画面回到 source，
 *      再启用回到逐字节相同的渲染；
 *   5. 求值在**对象像素空间**：图层移动后重算同一份栈，结果逐字节不变；
 *   6. 智能图层的像素写入被闸住：comp_layer_set_pixels / comp_filter_apply /
 *      comp_tool_apply 一律 COMP_ERR_STATE；
 *   7. 栅格化丢掉真源、留下当前渲染，且不可幂等重复（第二次调用不落撤销步）；
 *   8. `.comp` 往返：engineSmart + images/<uuid>.smart.png；普通图层不写该键；
 *   9. 栈序重排（P1-2 收尾 ⑩）：新序重求值、一步撤销、from == to 与越界不落步、
 *      像素锁闸、停用标记跟条目走；
 *  10. 展示名：空名回退、UTF-8/单行/长度闸在开事务之前、同值不落步、可撤销、
 *      过 `.comp` 往返，且空名不写键。
 *
 * 只用 comp_*.h 里的 C 符号，与三端外壳的集成路径一致。
 */
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_filter.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

constexpr int32_t kW = 32;
constexpr int32_t kH = 32;

comp_engine_desc engine_desc_make() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-smart-object-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

comp_document make_document(comp_engine engine, int32_t width = kW, int32_t height = kH) {
  comp_document_desc desc{};
  desc.width = width;
  desc.height = height;
  comp_document document = nullptr;
  if (comp_document_create(engine, &desc, &document) != COMP_OK) return nullptr;
  return document;
}

comp_layer_id add_layer(comp_document document, const char* name, bool group = false) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  desc.is_group = group ? 1 : 0;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  return id;
}

comp_layer_id bottom_layer(comp_document document) {
  comp_layer_id id = 0;
  size_t count = 0;
  if (comp_layer_list(document, &id, 1u, &count) != COMP_OK || count < 1u) return 0;
  return id;
}

size_t history_steps(comp_document document) {
  size_t steps = 0;
  if (comp_document_history_step_count(document, &steps) != COMP_OK) return 0xFFFFFFFFu;
  return steps;
}

/* 一块硬的白色方块压在一张渐变上：高斯模糊/噪声都能让它明显变化，便于断言
 * 「滤镜确实动过像素」与「撤销确实还原」。 */
std::vector<uint8_t> pattern_pixels(int32_t width, int32_t height) {
  std::vector<uint8_t> px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                        static_cast<size_t>(x)) * 4u;
      const bool block = x >= width / 4 && x < width / 2 && y >= height / 4 && y < height / 2;
      px[i + 0] = block ? 255u : static_cast<uint8_t>(x * 6);
      px[i + 1] = block ? 0u : static_cast<uint8_t>(y * 6);
      px[i + 2] = block ? 32u : 96u;
      px[i + 3] = 255u;
    }
  }
  return px;
}

bool set_pixels_rgba8(comp_document document, comp_layer_id id, const std::vector<uint8_t>& px,
                     int32_t width, int32_t height) {
  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(width) * 4u;
  image.pixels = const_cast<uint8_t*>(px.data());
  return comp_layer_set_pixels(document, id, &image) == COMP_OK;
}

struct Pixels {
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  std::vector<uint8_t> bytes;

  bool empty() const { return bytes.empty(); }
};

Pixels read_pixels(comp_document document, comp_layer_id id) {
  Pixels out;
  if (comp_layer_get_pixels(document, id, nullptr, 0, &out.width, &out.height, &out.format) !=
      COMP_OK) {
    return out;
  }
  if (out.width <= 0 || out.height <= 0) return out;
  const size_t stride = static_cast<size_t>(out.width) * 4u;
  out.bytes.resize(stride * static_cast<size_t>(out.height));
  if (comp_layer_get_pixels(document, id, out.bytes.data(), stride, &out.width, &out.height,
                            &out.format) != COMP_OK) {
    out.bytes.clear();
  }
  return out;
}

comp_filter_desc gaussian(double radius) {
  comp_filter_desc desc{};
  desc.kind = COMP_FILTER_GAUSSIAN_BLUR;
  desc.gaussian_blur.radius = radius;
  return desc;
}

comp_filter_desc add_noise(double amount, uint32_t seed) {
  comp_filter_desc desc{};
  desc.kind = COMP_FILTER_ADD_NOISE;
  desc.add_noise.amount = amount;
  desc.add_noise.seed = seed;
  return desc;
}

bool is_smart(comp_document document, comp_layer_id id) {
  int32_t value = -1;
  if (comp_layer_smart_object_is_smart(document, id, &value) != COMP_OK) return false;
  return value == 1;
}

size_t filter_count(comp_document document, comp_layer_id id) {
  size_t count = 0xFFFFFFFFu;
  if (comp_layer_smart_filter_count(document, id, &count) != COMP_OK) return 0xFFFFFFFFu;
  return count;
}

/* 第 index 条的类型（越界/失败返回 -1，便于一条断言判序）。 */
int filter_kind_at(comp_document document, comp_layer_id id, size_t index) {
  comp_filter_desc desc{};
  if (comp_layer_smart_filter_at(document, id, index, &desc, nullptr) != COMP_OK) return -1;
  return static_cast<int>(desc.kind);
}


/* 建一个已转换的智能对象图层（像素 = pattern）。 */
struct SmartFixture {
  comp_engine engine = nullptr;
  comp_document document = nullptr;
  comp_layer_id layer = 0;
  std::vector<uint8_t> source;

  bool build() {
    comp_engine_desc desc = engine_desc_make();
    if (comp_engine_create(&desc, &engine) != COMP_OK) return false;
    document = make_document(engine);
    if (document == nullptr) return false;
    layer = bottom_layer(document);
    if (layer == 0) return false;
    source = pattern_pixels(kW, kH);
    if (!set_pixels_rgba8(document, layer, source, kW, kH)) return false;
    int32_t changed = 0;
    if (comp_layer_smart_object_create(document, layer, &changed) != COMP_OK) return false;
    return changed == 1;
  }

  ~SmartFixture() {
    if (document != nullptr) comp_document_destroy(document);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
};

std::string work_path(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/smart_objects", ec);
  return std::string("build/smart_objects/") + name;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

/* ------------------------------------------------------ 1. 创建 / 幂等 / 撤销 */

COMP_TEST(smart_object_create_is_idempotent_and_undoable) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  const comp_layer_id layer = bottom_layer(document);
  COMP_CHECK(layer != 0);
  const std::vector<uint8_t> source = pattern_pixels(kW, kH);
  COMP_CHECK(set_pixels_rgba8(document, layer, source, kW, kH));

  int32_t smart = -1;
  COMP_CHECK(comp_layer_smart_object_is_smart(document, layer, &smart) == COMP_OK);
  COMP_CHECK_MSG(smart == 0, "a fresh pixel layer is not a smart object");
  /* 非智能图层的滤镜栈查询是明确的 STATE，不是「0 条」。 */
  size_t count = 0;
  COMP_CHECK(comp_layer_smart_filter_count(document, layer, &count) == COMP_ERR_STATE);

  const size_t before = history_steps(document);
  const Pixels picture = read_pixels(document, layer);
  int32_t changed = -1;
  COMP_CHECK(comp_layer_smart_object_create(document, layer, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "the first conversion changes the layer kind");
  COMP_CHECK_MSG(history_steps(document) == before + 1, "conversion is one undo step");
  COMP_CHECK(is_smart(document, layer));
  COMP_CHECK_MSG(filter_count(document, layer) == 0, "a new smart object has an empty stack");
  COMP_CHECK_MSG(read_pixels(document, layer).bytes == picture.bytes,
                 "conversion must not change the picture (source == pixels)");

  /* 幂等：第二次调用什么也不做。 */
  changed = -1;
  COMP_CHECK(comp_layer_smart_object_create(document, layer, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "re-converting an existing smart object is a no-op");
  COMP_CHECK_MSG(history_steps(document) == before + 1, "the no-op does not add an undo step");

  /* 撤销把整层状态一起带回：真源与滤镜栈都不在了。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK_MSG(!is_smart(document, layer), "undo restores the plain pixel layer");
  COMP_CHECK(read_pixels(document, layer).bytes == picture.bytes);
  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(is_smart(document, layer));

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(smart_object_create_rejects_unsupported_layers) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  /* 组层：没有自己的像素可以当 source。 */
  const comp_layer_id group = add_layer(document, "group", true);
  COMP_CHECK(group != 0);
  size_t before = history_steps(document);
  COMP_CHECK(comp_layer_smart_object_create(document, group, nullptr) == COMP_ERR_STATE);
  COMP_CHECK_MSG(history_steps(document) == before, "a group layer is rejected before any txn");

  /* 锁定的层：闸在开事务之前，失败不留撤销步。 */
  const comp_layer_id locked = bottom_layer(document);
  COMP_CHECK(locked != 0 && locked != group);
  COMP_CHECK(set_pixels_rgba8(document, locked, pattern_pixels(kW, kH), kW, kH));
  COMP_CHECK(comp_layer_set_lock(document, locked, COMP_LAYER_LOCK_ALL) == COMP_OK);
  before = history_steps(document);
  COMP_CHECK(comp_layer_smart_object_create(document, locked, nullptr) == COMP_ERR_STATE);
  COMP_CHECK_MSG(history_steps(document) == before, "a rejected conversion leaves no undo step");
  COMP_CHECK(comp_layer_set_lock(document, locked, 0) == COMP_OK);

  /* HDR：8 位滤镜内核没有它的通路。 */
  const comp_layer_id hdr = bottom_layer(document);
  std::vector<uint8_t> half(static_cast<size_t>(kW) * kH * 8u, 0u);
  for (size_t i = 3; i < static_cast<size_t>(kW) * kH * 4u; i += 4) {
    uint16_t one = 0x3C00; /* half 1.0 */
    std::memcpy(half.data() + i * 2u, &one, sizeof(one));
  }
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA16F;
  image.stride = static_cast<size_t>(kW) * 8u;
  image.pixels = half.data();
  COMP_CHECK(comp_layer_set_pixels(document, hdr, &image) == COMP_OK);
  COMP_CHECK(comp_layer_smart_object_create(document, hdr, nullptr) == COMP_ERR_STATE);

  /* 不存在的图层。 */
  COMP_CHECK(comp_layer_smart_object_create(document, 0xDEADBEEFu, nullptr) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 16 位图层先落 8 位工作副本（与滤镜同一条约定），之后该层就是 RGBA8。 */
COMP_TEST(smart_object_create_downgrades_sixteen_bit) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);

  const comp_layer_id layer = bottom_layer(document);
  COMP_CHECK(layer != 0);
  std::vector<uint8_t> deep(static_cast<size_t>(kW) * kH * 8u, 0u);
  for (size_t pixel = 0; pixel < static_cast<size_t>(kW) * kH; ++pixel) {
    uint16_t channels[4] = {60000u, 30000u, 1000u, 65535u};
    std::memcpy(deep.data() + pixel * 8u, channels, sizeof(channels));
  }
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA16;
  image.stride = static_cast<size_t>(kW) * 8u;
  image.pixels = deep.data();
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image) == COMP_OK);

  int32_t changed = 0;
  COMP_CHECK(comp_layer_smart_object_create(document, layer, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(read_pixels(document, layer).format == COMP_PIXEL_RGBA8);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------- 2. 滤镜栈：add / set / remove */

COMP_TEST(smart_filter_add_set_remove_is_one_step_each) {
  SmartFixture fx;
  COMP_CHECK(fx.build());
  const Pixels source = read_pixels(fx.document, fx.layer);

  /* 恒等参数也照常入栈：画面不变，但栈变了（宿主之后可以改参数）。 */
  const comp_filter_desc identity = gaussian(0.0);
  size_t before = history_steps(fx.document);
  COMP_CHECK(comp_layer_smart_filter_add(fx.document, fx.layer, &identity) == COMP_OK);
  COMP_CHECK_MSG(filter_count(fx.document, fx.layer) == 1, "an identity filter still enters the stack");
  COMP_CHECK_MSG(history_steps(fx.document) == before + 1, "add is one undo step");
  COMP_CHECK_MSG(read_pixels(fx.document, fx.layer).bytes == source.bytes,
                 "an identity filter does not change the picture");
  COMP_CHECK(comp_layer_smart_filter_remove(fx.document, fx.layer, 0) == COMP_OK);
  COMP_CHECK(filter_count(fx.document, fx.layer) == 0);

  /* 真滤镜：像素变了、栈里有它、回读参数一致。 */
  const comp_filter_desc blur = gaussian(3.0);
  COMP_CHECK(comp_layer_smart_filter_add(fx.document, fx.layer, &blur) == COMP_OK);
  const Pixels blurred = read_pixels(fx.document, fx.layer);
  COMP_CHECK_MSG(blurred.bytes != source.bytes, "a real filter changes the picture");
  comp_filter_desc readback{};
  int32_t enabled = -1;
  COMP_CHECK(comp_layer_smart_filter_at(fx.document, fx.layer, 0, &readback, &enabled) == COMP_OK);
  COMP_CHECK(readback.kind == COMP_FILTER_GAUSSIAN_BLUR);
  COMP_CHECK(readback.gaussian_blur.radius == 3.0);
  COMP_CHECK_MSG(enabled == 1, "new entries are enabled");

  /* 同值重设：不重算、不落撤销步。 */
  before = history_steps(fx.document);
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &blur, 1) == COMP_OK);
  COMP_CHECK_MSG(history_steps(fx.document) == before, "setting the same value adds no undo step");

  /* 改参数：一步撤销、像素变了；撤销回到上一次渲染。 */
  const comp_filter_desc wider_desc = gaussian(6.0);
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &wider_desc, 1) == COMP_OK);
  COMP_CHECK_MSG(history_steps(fx.document) == before + 1, "a real update is one undo step");
  const Pixels wider = read_pixels(fx.document, fx.layer);
  COMP_CHECK_MSG(wider.bytes != blurred.bytes, "a larger radius blurs further");
  COMP_CHECK(comp_document_undo(fx.document) == COMP_OK);
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == blurred.bytes);

  /* 删到最后一条：画面逐字节回到 source。 */
  COMP_CHECK(comp_layer_smart_filter_remove(fx.document, fx.layer, 0) == COMP_OK);
  COMP_CHECK(filter_count(fx.document, fx.layer) == 0);
  COMP_CHECK_MSG(read_pixels(fx.document, fx.layer).bytes == source.bytes,
                 "an empty stack re-renders the source");
  COMP_CHECK(comp_document_undo(fx.document) == COMP_OK);
  COMP_CHECK(filter_count(fx.document, fx.layer) == 1);

  /* 越界 index / 非法参数都在开事务之前失败。 */
  before = history_steps(fx.document);
  const comp_filter_desc out_of_range = gaussian(9999.0);
  const comp_filter_desc negative = gaussian(-1.0);
  COMP_CHECK(comp_layer_smart_filter_remove(fx.document, fx.layer, 9) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &out_of_range, 1) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_smart_filter_add(fx.document, fx.layer, &negative) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK_MSG(history_steps(fx.document) == before, "rejected calls leave no undo step");
}

COMP_TEST(smart_filter_disabled_keeps_parameters_and_stops_evaluating) {
  SmartFixture fx;
  COMP_CHECK(fx.build());
  const Pixels source = read_pixels(fx.document, fx.layer);

  const comp_filter_desc noise = add_noise(25.0, 7u);
  COMP_CHECK(comp_layer_smart_filter_add(fx.document, fx.layer, &noise) == COMP_OK);
  const Pixels noisy = read_pixels(fx.document, fx.layer);
  COMP_CHECK(noisy.bytes != source.bytes);

  /* 停用：条目还在，画面回到 source。 */
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &noise, 0) == COMP_OK);
  COMP_CHECK_MSG(filter_count(fx.document, fx.layer) == 1, "disabling keeps the entry");
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == source.bytes);
  comp_filter_desc readback{};
  int32_t enabled = -1;
  COMP_CHECK(comp_layer_smart_filter_at(fx.document, fx.layer, 0, &readback, &enabled) == COMP_OK);
  COMP_CHECK_MSG(enabled == 0, "the disabled flag round-trips");
  COMP_CHECK_MSG(readback.add_noise.amount == 25.0 && readback.add_noise.seed == 7u,
                 "disabling does not drop the parameters");

  /* 重新启用：逐字节回到同一次渲染（求值是确定性的）。 */
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &noise, 1) == COMP_OK);
  COMP_CHECK_MSG(read_pixels(fx.document, fx.layer).bytes == noisy.bytes,
                 "re-enabling reproduces the same render");

  /* 再停用一次后删掉条目：仍是 source。 */
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &noise, 0) == COMP_OK);
  COMP_CHECK(comp_layer_smart_filter_remove(fx.document, fx.layer, 0) == COMP_OK);
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == source.bytes);
}

/* --------------------------------------- 3. 对象像素空间（不随图层几何漂移） */

COMP_TEST(smart_filter_render_is_object_space) {
  SmartFixture fx;
  COMP_CHECK(fx.build());

  const comp_filter_desc noise = add_noise(25.0, 7u);
  const comp_filter_desc stronger = add_noise(30.0, 7u);
  COMP_CHECK(comp_layer_smart_filter_add(fx.document, fx.layer, &noise) == COMP_OK);
  const Pixels before = read_pixels(fx.document, fx.layer);

  /* 移动图层（平移，不改像素尺寸）：再求值一次必须逐字节相同，说明噪声场锚在
   * 对象像素空间 (0,0)，而不是文档坐标。 */
  COMP_CHECK(comp_layer_set_transform(fx.document, fx.layer, comp_rect{7, 3, kW, kH}, 0.0f) ==
             COMP_OK);
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == before.bytes);
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &stronger, 1) == COMP_OK);
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &noise, 1) == COMP_OK);
  COMP_CHECK_MSG(read_pixels(fx.document, fx.layer).bytes == before.bytes,
                 "re-evaluating after a move reproduces the same bytes");

  /* 旋转同理（求值不读几何）。 */
  COMP_CHECK(comp_layer_set_transform(fx.document, fx.layer, comp_rect{7, 3, kW, kH}, 37.0f) ==
             COMP_OK);
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &stronger, 1) == COMP_OK);
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 0, &noise, 1) == COMP_OK);
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == before.bytes);
}

/* --------------------------------------------- 4. 像素写入被闸住（须先栅格化） */

COMP_TEST(smart_layer_rejects_direct_pixel_writes) {
  SmartFixture fx;
  COMP_CHECK(fx.build());
  const Pixels picture = read_pixels(fx.document, fx.layer);
  const size_t before = history_steps(fx.document);

  std::vector<uint8_t> replacement = pattern_pixels(kW, kH);
  for (size_t i = 0; i < replacement.size(); ++i) replacement[i] = 0u;
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(kW) * 4u;
  image.pixels = replacement.data();
  COMP_CHECK_MSG(comp_layer_set_pixels(fx.document, fx.layer, &image) == COMP_ERR_STATE,
                 "set_pixels on a smart layer asks the host to rasterize first");

  comp_filter_desc blur = gaussian(2.0);
  int32_t changed = -1;
  COMP_CHECK_MSG(comp_filter_apply(fx.document, fx.layer, &blur, 1.0, &changed, nullptr, 0,
                                   nullptr) == COMP_ERR_STATE,
                 "the whole-layer filter path refuses smart layers");

  comp_tool_op_desc tool{};
  tool.kind = COMP_TOOL_DITHER;
  COMP_CHECK_MSG(comp_tool_apply(fx.document, fx.layer, &tool, nullptr, 0, nullptr) ==
                     COMP_ERR_STATE,
                 "pixel tools refuse smart layers");

  COMP_CHECK_MSG(history_steps(fx.document) == before, "all rejections leave no undo step");
  COMP_CHECK(read_pixels(fx.document, fx.layer).bytes == picture.bytes);
}

/* ------------------------------------------------------- 5. 栅格化（保留画面） */

COMP_TEST(smart_object_rasterize_keeps_the_render) {
  SmartFixture fx;
  const comp_filter_desc blur = gaussian(3.0);
  COMP_CHECK(fx.build());
  COMP_CHECK(comp_layer_smart_filter_add(fx.document, fx.layer, &blur) == COMP_OK);
  const Pixels rendered = read_pixels(fx.document, fx.layer);

  size_t before = history_steps(fx.document);
  COMP_CHECK(comp_layer_smart_object_rasterize(fx.document, fx.layer) == COMP_OK);
  COMP_CHECK_MSG(history_steps(fx.document) == before + 1, "rasterize is one undo step");
  COMP_CHECK_MSG(!is_smart(fx.document, fx.layer), "rasterize drops the smart object");
  COMP_CHECK_MSG(read_pixels(fx.document, fx.layer).bytes == rendered.bytes,
                 "rasterize keeps the current render, not the source");
  COMP_CHECK(comp_layer_smart_filter_count(fx.document, fx.layer, &before) == COMP_ERR_STATE);

  /* 幂等：普通图层再栅格化不落撤销步。 */
  before = history_steps(fx.document);
  COMP_CHECK(comp_layer_smart_object_rasterize(fx.document, fx.layer) == COMP_OK);
  COMP_CHECK_MSG(history_steps(fx.document) == before, "re-rasterizing a plain layer is a no-op");

  COMP_CHECK(comp_document_undo(fx.document) == COMP_OK);
  COMP_CHECK_MSG(is_smart(fx.document, fx.layer), "undo brings the smart object back");
}

/* ------------------------------------------------------ 6. `.comp` 往返保真 */

COMP_TEST(smart_object_comp_round_trip) {
  SmartFixture fx;
  const comp_filter_desc blur = gaussian(3.0);
  const comp_filter_desc noise = add_noise(25.0, 7u);
  COMP_CHECK(fx.build());
  COMP_CHECK(comp_layer_smart_filter_add(fx.document, fx.layer, &blur) == COMP_OK);
  COMP_CHECK(comp_layer_smart_filter_add(fx.document, fx.layer, &noise) == COMP_OK);
  COMP_CHECK(comp_layer_smart_filter_set(fx.document, fx.layer, 1, &noise, 0) == COMP_OK);
  const Pixels expected = read_pixels(fx.document, fx.layer);

  const std::string path = work_path("round_trip.comp");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  COMP_CHECK(comp_io_save_project(fx.document, path.c_str()) == COMP_OK);

  const std::string manifest = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("engineSmart") != std::string::npos,
                 "the manifest records smart objects");
  COMP_CHECK_MSG(manifest.find(".smart.png") != std::string::npos,
                 "the original pixels get their own asset");
  size_t smart_assets = 0;
  for (const auto& entry : std::filesystem::directory_iterator(path + "/images", ec)) {
    if (entry.path().filename().u8string().find(".smart.png") != std::string::npos) ++smart_assets;
  }
  COMP_CHECK_MSG(smart_assets == 1u, "exactly one smart source asset is written");

  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(fx.engine, path.c_str(), &loaded) == COMP_OK);
  const comp_layer_id loaded_layer = bottom_layer(loaded);
  COMP_CHECK(loaded_layer != 0);
  COMP_CHECK_MSG(is_smart(loaded, loaded_layer), "the smart object survives a save/load");
  COMP_CHECK(filter_count(loaded, loaded_layer) == 2);

  comp_filter_desc first{};
  int32_t enabled = -1;
  COMP_CHECK(comp_layer_smart_filter_at(loaded, loaded_layer, 0, &first, &enabled) == COMP_OK);
  COMP_CHECK(first.kind == COMP_FILTER_GAUSSIAN_BLUR && first.gaussian_blur.radius == 3.0);
  COMP_CHECK(enabled == 1);
  comp_filter_desc second{};
  COMP_CHECK(comp_layer_smart_filter_at(loaded, loaded_layer, 1, &second, &enabled) == COMP_OK);
  COMP_CHECK(second.kind == COMP_FILTER_ADD_NOISE && second.add_noise.seed == 7u);
  COMP_CHECK_MSG(enabled == 0, "the disabled flag survives a save/load");
  COMP_CHECK_MSG(second.add_noise.amount == 25.0,
                 "a disabled entry keeps its parameters across save/load");

  /* imageFile 是渲染缓存：读回来就是同一份画面，不需要也不应该重新求值。 */
  COMP_CHECK(read_pixels(loaded, loaded_layer).bytes == expected.bytes);

  /* 原始像素确实存下来了：删掉第一条滤镜会重新求值，结果必须等于「只留噪声
   * 且停用」= source。这证明 source 不是从渲染结果反推的。 */
  COMP_CHECK(comp_layer_smart_filter_remove(loaded, loaded_layer, 1) == COMP_OK);
  COMP_CHECK(comp_layer_smart_filter_remove(loaded, loaded_layer, 0) == COMP_OK);
  COMP_CHECK_MSG(read_pixels(loaded, loaded_layer).bytes == fx.source,
                 "the saved source asset re-renders the original pixels");

  comp_document_destroy(loaded);
}

/* 没有智能对象的工程不写 engineSmart（缺键 = 普通图层），落盘结果与旧版一致。 */
COMP_TEST(plain_project_has_no_smart_keys) {
  comp_engine_desc desc = engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document document = make_document(engine);
  COMP_CHECK(document != nullptr);
  const comp_layer_id layer = bottom_layer(document);
  COMP_CHECK(layer != 0);
  COMP_CHECK(set_pixels_rgba8(document, layer, pattern_pixels(kW, kH), kW, kH));

  const std::string path = work_path("plain.comp");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  COMP_CHECK(comp_io_save_project(document, path.c_str()) == COMP_OK);
  const std::string manifest = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("engineSmart") == std::string::npos,
                 "an ordinary project must not grow a smart key");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------ 7. 栈序重排（P1-2 收尾 ⑩） */

/*
 * 栈序就是求值顺序：两条不可交换的滤镜换个位置，画面必须变；撤销必须把
 * 「顺序 + 像素」一起带回。停用条目的 enabled 跟着条目走，不按位置留。
 */
