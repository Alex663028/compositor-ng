// AI生成
/*
 * test_layer_style.cpp — P1-2 ④：图层样式导出/导入（拷贝/粘贴）的端到端测试。
 *
 * 判据不是「API 返回 COMP_OK」，而是可观测量：
 *   1. 导出/导入是**逐字节往返**：kind 顺序与 settings 字符串都不变形，未知键也带着；
 *   2. 导入是**整份替换**，不是按 kind 合并；空数组能把目标层样式清空；
 *   3. 幂等：同一样式再导入一次不落撤销步；有变化则恰好一步撤销；
 *   4. 显式事务里的多次粘贴由**一步**撤销整体回退（壳层多选粘贴的底座）；
 *   5. 样式真的改变渲染（Color Overlay 把渐变层刷成红色），撤销后逐字节复原；
 *   6. 拒绝面明确：组、求值不了的 kind、坏报文、锁定全部——都不留下半份状态。
 *
 * 只使用 comp_*.h 里的 C 符号，与三端外壳的集成路径一致。
 */
#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

constexpr int32_t kW = 8;
constexpr int32_t kH = 8;
constexpr size_t kStride = static_cast<size_t>(kW) * 4u;

/* Color Overlay 全不透明黑色：样式是否真的参与渲染的最佳探针（纯色层会被误读）。 */
const char* const kOverlayBlack = "{\"opacity\":1,\"red\":0,\"green\":0,\"blue\":0}";
const char* const kOverlayRed = "{\"opacity\":1,\"red\":1,\"green\":0,\"blue\":0}";
const char* const kShadow = "{\"opacity\":0.5,\"angle\":90,\"distance\":4,\"blur\":2}";

struct Fixture {
  comp_engine engine = nullptr;
  comp_document doc = nullptr;

  Fixture() {
    comp_engine_desc desc{};
    desc.app_name = "compositor-layer-style-tests";
    desc.version = "1.0.0";
    desc.backend = COMP_BACKEND_CPU;
    desc.worker_threads = 1;
    comp_document_desc document_desc{};
    document_desc.width = kW;
    document_desc.height = kH;
    document_desc.dpi = 72.0f;
    COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
    COMP_CHECK(comp_document_create(engine, &document_desc, &doc) == COMP_OK);
  }
  ~Fixture() {
    if (doc != nullptr) comp_document_destroy(doc);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
};

comp_layer_id add_layer(comp_document doc, const char* name, int32_t is_group = 0) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, kW, kH};
  desc.parent = 0;
  desc.is_group = is_group;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  if (comp_layer_add(doc, &desc, &id) != COMP_OK) return 0;
  return id;
}

/* 每像素 (x*16, y*16, 128, 255) 的渐变。 */
std::vector<uint8_t> gradient_pixels() {
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4u);
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4u;
      px[0] = static_cast<uint8_t>(x * 16);
      px[1] = static_cast<uint8_t>(y * 16);
      px[2] = 128;
      px[3] = 255;
    }
  }
  return pixels;
}

void fill_gradient(comp_document doc, comp_layer_id id) {
  std::vector<uint8_t> pixels = gradient_pixels();
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(doc, id, &image) == COMP_OK);
}

std::vector<uint8_t> flatten(comp_document doc) {
  std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4u);
  COMP_CHECK(comp_render_flatten(doc, out.data(), COMP_PIXEL_RGBA8, kStride) == COMP_OK);
  return out;
}

/* 两段式导出（查询长度 -> 分配 -> 取报文），与宿主同一条调用路径。 */
std::string export_style(comp_document doc, comp_layer_id id, comp_result* out_code) {
  size_t need = 0;
  comp_result rc = comp_layer_export_style(doc, id, nullptr, 0, &need);
  if (out_code != nullptr) *out_code = rc;
  if (rc != COMP_OK) return std::string();
  std::vector<char> buffer(need + 1, '\0');
  rc = comp_layer_export_style(doc, id, buffer.data(), buffer.size(), &need);
  if (out_code != nullptr) *out_code = rc;
  if (rc != COMP_OK) return std::string();
  return std::string(buffer.data(), need);
}

/* 效果栈的规范化指纹：kind 顺序 + settings 原文。 */
std::string effects_signature(comp_document doc, comp_layer_id id) {
  size_t count = 0;
  if (comp_layer_effect_count(doc, id, &count) != COMP_OK) return "<error>";
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    uint32_t kind = 0;
    size_t need = 0;
    if (comp_layer_effect_at(doc, id, i, &kind, nullptr, 0, &need) != COMP_OK) return "<error>";
    std::vector<char> text(need + 1, '\0');
    if (comp_layer_effect_at(doc, id, i, &kind, text.data(), text.size(), &need) != COMP_OK) {
      return "<error>";
    }
    out += std::to_string(kind) + ":" + std::string(text.data(), need) + ";";
  }
  return out;
}

bool is_reddish(const std::vector<uint8_t>& canvas, size_t pixel) {
  const size_t at = pixel * 4u;
  return canvas[at + 0] >= 200u && canvas[at + 1] <= 40u && canvas[at + 2] <= 40u;
}

}  // namespace

/* ------------------------------------------- 往返 / 幂等 / 一步撤销 */

COMP_TEST(layer_style_export_import_roundtrip) {
  Fixture f;
  const comp_layer_id a = add_layer(f.doc, "a");
  const comp_layer_id b = add_layer(f.doc, "b");
  COMP_CHECK(a != 0 && b != 0);

  COMP_CHECK(comp_layer_set_effect(f.doc, a, COMP_EFFECT_DROP_SHADOW, kShadow) == COMP_OK);
  COMP_CHECK(comp_layer_set_effect(f.doc, a, COMP_EFFECT_COLOR_OVERLAY, kOverlayBlack) == COMP_OK);

  comp_result code = COMP_ERR_INTERNAL;
  const std::string blob = export_style(f.doc, a, &code);
  COMP_CHECK_MSG(code == COMP_OK, "exporting a styled layer succeeds");
  COMP_CHECK_MSG(blob.size() >= 2 && blob.front() == '[' && blob.back() == ']',
                 "the style blob is a JSON array");
  const std::string source = effects_signature(f.doc, a);
  COMP_CHECK_MSG(effects_signature(f.doc, b).empty(), "the target starts without styles");

  int32_t changed = -1;
  COMP_CHECK(comp_layer_import_style(f.doc, b, blob.c_str(), &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "pasting onto a bare layer changes it");
  COMP_CHECK_MSG(effects_signature(f.doc, b) == source,
                 "paste copies kind order and settings verbatim");
  /* 双向逐字节：目标层再导出必须与源报文一字不差。 */
  COMP_CHECK(export_style(f.doc, b, &code) == blob);

  /* 幂等：同一样式再粘一次不落撤销步，一次撤销必须回到「没有样式」。 */
  changed = -1;
  COMP_CHECK(comp_layer_import_style(f.doc, b, blob.c_str(), &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "re-pasting the same style is a no-op");
  COMP_CHECK(comp_document_undo(f.doc) == COMP_OK);
  COMP_CHECK_MSG(effects_signature(f.doc, b).empty(), "one undo removes the pasted style");
  COMP_CHECK_MSG(effects_signature(f.doc, a) == source, "the source layer is untouched");
  COMP_CHECK(comp_document_redo(f.doc) == COMP_OK);
  COMP_CHECK_MSG(effects_signature(f.doc, b) == source, "redo brings the style back");

}

/* ------------------------------------------- 导出缓冲契约 */

COMP_TEST(layer_style_export_buffer_contract) {
  Fixture f;
  const comp_layer_id a = add_layer(f.doc, "a");
  COMP_CHECK(a != 0);

  size_t need = 12345;
  COMP_CHECK(comp_layer_export_style(f.doc, a, nullptr, 0, &need) == COMP_OK);
  COMP_CHECK_MSG(need == 2u, "an empty style exports as the two-byte '[]'");

  char small[2] = {'x', 'x'};
  size_t size_small = 0;
  COMP_CHECK_MSG(comp_layer_export_style(f.doc, a, small, sizeof(small), &size_small) ==
                     COMP_ERR_INVALID_ARGUMENT,
                 "a buffer without room for the NUL is rejected, never truncated");
  COMP_CHECK_MSG(small[0] == 'x' && small[1] == 'x', "the rejected call leaves the buffer alone");

  char exact[3] = {'x', 'x', 'x'};
  COMP_CHECK(comp_layer_export_style(f.doc, a, exact, sizeof(exact), &need) == COMP_OK);
  COMP_CHECK_MSG(std::string(exact, 2) == "[]", "an exactly sized buffer receives '[]'");
  COMP_CHECK_MSG(exact[2] == '\0', "the payload is NUL-terminated");

  COMP_CHECK(comp_layer_export_style(f.doc, a, nullptr, 0, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  size_t missing_size = 0;
  COMP_CHECK(comp_layer_export_style(f.doc, 999999, nullptr, 0, &missing_size) ==
             COMP_ERR_NOT_FOUND);

}

/* ------------------------------------------- 导出拒绝面：组 / 求值不了的 kind */

COMP_TEST(layer_style_export_rejections) {
  Fixture f;
  const comp_layer_id a = add_layer(f.doc, "a");
  const comp_layer_id group = add_layer(f.doc, "group", 1);
  COMP_CHECK(a != 0 && group != 0);

  size_t need = 0;
  COMP_CHECK_MSG(comp_layer_export_style(f.doc, group, nullptr, 0, &need) == COMP_ERR_UNSUPPORTED,
                 "groups never evaluate their own effects");
  int32_t changed = -1;
  COMP_CHECK(comp_layer_import_style(f.doc, group, "[]", &changed) == COMP_ERR_UNSUPPORTED);
  COMP_CHECK_MSG(changed == 0, "a rejected paste reports no change");

  /* 更高版本 .comp 才认识的 kind：导出也必须明确拒绝，不产出粘不回来的剪贴板。 */
  COMP_CHECK(comp_layer_set_effect(f.doc, a, 99u, "{}") == COMP_OK);
  COMP_CHECK_MSG(comp_layer_export_style(f.doc, a, nullptr, 0, &need) == COMP_ERR_UNSUPPORTED,
                 "an unevaluated effect kind blocks the export");

}

/* ------------------------------------------- 导入报文校验 */

COMP_TEST(layer_style_import_payload_validation) {
  Fixture f;
  const comp_layer_id a = add_layer(f.doc, "a");
  COMP_CHECK(a != 0);
  COMP_CHECK(comp_layer_set_effect(f.doc, a, COMP_EFFECT_DROP_SHADOW, kShadow) == COMP_OK);
  const std::string before = effects_signature(f.doc, a);

  const char* const bad_payloads[] = {
      nullptr,
      "",
      "{}",
      "[1]",
      "[{\"effect\":2}]",
      "[{\"effect\":\"2\",\"settings\":\"{}\"}]",
      "[{\"effect\":2.5,\"settings\":\"{}\"}]",
      "[{\"effect\":-1,\"settings\":\"{}\"}]",
      "[{\"effect\":2,\"settings\":{}}]",
      "[{\"effect\":2,\"settings\":\"{}\"},{\"effect\":2,\"settings\":\"{}\"}]",
  };
  for (const char* payload : bad_payloads) {
    int32_t changed = -1;
    COMP_CHECK_MSG(comp_layer_import_style(f.doc, a, payload, &changed) ==
                       COMP_ERR_INVALID_ARGUMENT,
                   "malformed style payloads are rejected");
    COMP_CHECK_MSG(changed == 0, "a rejected payload reports no change");
    COMP_CHECK_MSG(effects_signature(f.doc, a) == before,
                   "a rejected payload leaves the stack untouched");
  }

  /* 未知 kind 单独给 UNSUPPORTED（宿主能区分「传错了」与「做不到」）。 */
  int32_t changed = -1;
  COMP_CHECK(comp_layer_import_style(f.doc, a, "[{\"effect\":99,\"settings\":\"{}\"}]", &changed) ==
             COMP_ERR_UNSUPPORTED);
  COMP_CHECK_MSG(effects_signature(f.doc, a) == before, "an unknown kind changes nothing");

  /* 额外键忽略（前向兼容），settings 原文一字不差地存下来。 */
  const char* const forward = "[{\"effect\":2,\"settings\":\"{\\\"opacity\\\":1}\",\"note\":\"x\"}]";
  COMP_CHECK(comp_layer_import_style(f.doc, a, forward, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(effects_signature(f.doc, a) == "2:{\"opacity\":1};",
                 "unknown entry keys are ignored and settings stay verbatim");

  /* 空数组是合法输入：清空样式；已经是空的再清一次是空操作。 */
  COMP_CHECK(comp_layer_import_style(f.doc, a, "[]", &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(effects_signature(f.doc, a).empty(), "an empty array clears the style");
  COMP_CHECK(comp_layer_import_style(f.doc, a, "[]", &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "clearing an already-clear style is a no-op");

  COMP_CHECK(comp_layer_import_style(f.doc, 999999, "[]", &changed) == COMP_ERR_NOT_FOUND);

}

/* ------------------------------------------- 锁闸 / 组闸 */

COMP_TEST(layer_style_import_locks) {
  Fixture f;
  const comp_layer_id a = add_layer(f.doc, "a");
  COMP_CHECK(a != 0);

  const char* const blob = "[{\"effect\":2,\"settings\":\"{\\\"opacity\\\":1}\"}]";
  int32_t changed = -1;

  COMP_CHECK(comp_layer_set_lock(f.doc, a, COMP_LAYER_LOCK_ALL) == COMP_OK);
  COMP_CHECK_MSG(comp_layer_import_style(f.doc, a, blob, &changed) == COMP_ERR_STATE,
                 "lock all blocks a style change");
  COMP_CHECK_MSG(changed == 0, "a locked paste reports no change");
  size_t need = 0;
  COMP_CHECK_MSG(comp_layer_export_style(f.doc, a, nullptr, 0, &need) == COMP_OK,
                 "reading the style is still allowed under the lock");

  /* 锁定图像像素不管样式（与 comp_layer_set_effect 同口径）。 */
  COMP_CHECK(comp_layer_set_lock(f.doc, a, COMP_LAYER_LOCK_PIXELS) == COMP_OK);
  COMP_CHECK(comp_layer_import_style(f.doc, a, blob, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "a pixel lock does not block style-only edits");

}

/* ------------------------------------------- 样式真的改变渲染 */

COMP_TEST(layer_style_paste_changes_rendering) {
  Fixture f;
  const comp_layer_id base = add_layer(f.doc, "base");
  const comp_layer_id source = add_layer(f.doc, "source");
  COMP_CHECK(base != 0 && source != 0);
  fill_gradient(f.doc, base);
  fill_gradient(f.doc, source);
  COMP_CHECK(comp_layer_set_effect(f.doc, source, COMP_EFFECT_COLOR_OVERLAY, kOverlayRed) ==
             COMP_OK);
  comp_result code = COMP_ERR_INTERNAL;
  const std::string blob = export_style(f.doc, source, &code);
  COMP_CHECK(code == COMP_OK);

  /* 供体层隐藏后再取基线：撤销只回退粘贴，供体自己的样式仍在（也不该污染画布）。 */
  COMP_CHECK(comp_layer_set_visible(f.doc, source, 0) == COMP_OK);
  const std::vector<uint8_t> pristine = flatten(f.doc);

  int32_t changed = -1;
  COMP_CHECK(comp_layer_import_style(f.doc, base, blob.c_str(), &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> styled = flatten(f.doc);
  COMP_CHECK_MSG(is_reddish(styled, 0) && is_reddish(styled, kW * kH - 1),
                 "the pasted Color Overlay recolors the whole layer");

  COMP_CHECK(comp_document_undo(f.doc) == COMP_OK);
  const std::vector<uint8_t> restored = flatten(f.doc);
  COMP_CHECK_MSG(restored == pristine, "one undo restores the exact pre-paste pixels");

}

/* ------------------------------------------- 多选粘贴：一次事务 = 一步撤销 */

COMP_TEST(layer_style_multi_paste_single_undo) {
  Fixture f;
  const comp_layer_id one = add_layer(f.doc, "one");
  const comp_layer_id two = add_layer(f.doc, "two");
  const comp_layer_id source = add_layer(f.doc, "source");
  COMP_CHECK(one != 0 && two != 0 && source != 0);
  fill_gradient(f.doc, one);
  fill_gradient(f.doc, two);

  COMP_CHECK(comp_layer_set_effect(f.doc, source, COMP_EFFECT_COLOR_OVERLAY, kOverlayRed) ==
             COMP_OK);
  comp_result code = COMP_ERR_INTERNAL;
  const std::string blob = export_style(f.doc, source, &code);
  COMP_CHECK(code == COMP_OK);
  const std::vector<uint8_t> pristine = flatten(f.doc);

  comp_txn txn = nullptr;
  COMP_CHECK(comp_txn_begin(f.doc, "Paste Layer Style", &txn) == COMP_OK);
  COMP_CHECK(comp_layer_import_style(f.doc, one, blob.c_str(), nullptr) == COMP_OK);
  COMP_CHECK(comp_layer_import_style(f.doc, two, blob.c_str(), nullptr) == COMP_OK);
  COMP_CHECK(comp_txn_commit(txn) == COMP_OK);
  COMP_CHECK_MSG(!effects_signature(f.doc, one).empty() &&
                     !effects_signature(f.doc, two).empty(),
                 "both selected layers receive the style");

  COMP_CHECK(comp_document_undo(f.doc) == COMP_OK);
  COMP_CHECK_MSG(effects_signature(f.doc, one).empty() &&
                     effects_signature(f.doc, two).empty(),
                 "one undo reverts the whole multi-layer paste");
  COMP_CHECK_MSG(flatten(f.doc) == pristine, "the canvas returns to its pre-paste bytes");

}
