// AI生成
/*
 * test_abi.cpp — C ABI 端到端：引擎/文档/图层/合成/撤销/命令/IO。
 *
 * 这一组测试即「壳的视角」：只使用 comp_*.h 中的 C 符号，不接触任何 C++ 内部类型。
 * 三端外壳（Windows / HarmonyOS / macOS）的集成路径与此完全一致。
 */
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "compositor/comp_command.h"
#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-abi-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 2;
  return desc;
}

/* 生存期与 comp_image.pixels 绑定的实心图像。 */
struct SolidImage {
  std::vector<uint8_t> bytes;
  comp_image image{};

  SolidImage(int32_t width, int32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
      : bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
    for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
      bytes[i] = r;
      bytes[i + 1] = g;
      bytes[i + 2] = b;
      bytes[i + 3] = a;
    }
    image.width = width;
    image.height = height;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = static_cast<size_t>(width) * 4u;
    image.pixels = bytes.data();
  }
};

int abs_diff(int a, int b) { return a > b ? a - b : b - a; }

bool matches(const std::vector<uint8_t>& canvas, int32_t width, int32_t x, int32_t y, uint8_t r,
             uint8_t g, uint8_t b, uint8_t a, int tolerance) {
  const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                        static_cast<size_t>(x)) * 4u;
  if (index + 3 >= canvas.size()) return false;
  return abs_diff(canvas[index + 0], r) <= tolerance &&
         abs_diff(canvas[index + 1], g) <= tolerance &&
         abs_diff(canvas[index + 2], b) <= tolerance &&
         abs_diff(canvas[index + 3], a) <= tolerance;
}

std::vector<uint8_t> flatten(comp_document document, int32_t width, int32_t height) {
  std::vector<uint8_t> canvas(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  const comp_result result =
      comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8, static_cast<size_t>(width) * 4u);
  COMP_CHECK_MSG(result == COMP_OK, "comp_render_flatten");
  return canvas;
}

/* 取文档的唯一背景层（新建文档协议：单背景层）。 */
comp_layer_id background_layer(comp_document document) {
  comp_layer_id ids[8] = {0};
  size_t count = 0;
  comp_layer_list(document, ids, 8, &count);
  return count > 0 ? ids[0] : 0;
}

/* 计数分配器：验证引擎/文档对象真的流经调用方分配器（架构修复验证用）。 */
struct Counting {
  int allocations = 0;
  int releases = 0;
};

void* counting_alloc(size_t bytes, void* user) {
  ++static_cast<Counting*>(user)->allocations;
  return std::malloc(bytes);
}

void counting_free(void* pointer, void* user) {
  if (pointer != nullptr) ++static_cast<Counting*>(user)->releases;
  std::free(pointer);
}

}  // namespace

COMP_TEST(abi_version_and_names) {
  const char* version = comp_version();
  COMP_CHECK(version != nullptr && version[0] != '\0');
  COMP_CHECK(std::string(version).find("1.0") != std::string::npos);

  COMP_CHECK(std::string(comp_result_name(COMP_OK)) == "COMP_OK");
  COMP_CHECK(comp_result_name(static_cast<comp_result>(999)) != nullptr);
  COMP_CHECK(std::string(comp_blend_mode_name(COMP_BLEND_NORMAL)) == "NORMAL");
  COMP_CHECK(std::string(comp_blend_mode_name(COMP_BLEND_LUMINOSITY)) == "LUMINOSITY");
}

COMP_TEST(abi_engine_lifecycle) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK(engine != nullptr);

  comp_backend backend = COMP_BACKEND_AUTO;
  COMP_CHECK(comp_engine_backend(engine, &backend) == COMP_OK);
  COMP_CHECK(backend == COMP_BACKEND_CPU);

  uint32_t threads = 0;
  COMP_CHECK(comp_engine_threads(engine, &threads) == COMP_OK);
  COMP_CHECK(threads >= 1);

  COMP_CHECK(comp_engine_last_error(engine) != nullptr);

  /* 参数校验：空输出指针必须被拒绝而不是崩溃。 */
  COMP_CHECK(comp_engine_create(&desc, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  comp_engine_destroy(engine);
}

COMP_TEST(abi_document_basics) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  document_desc.dpi = 72.0f;
  document_desc.color_space = "sRGB";

  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(document != nullptr);

  comp_size size{0, 0};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 8 && size.height == 8);

  size_t layer_count = 0;
  COMP_CHECK(comp_layer_count(document, &layer_count) == COMP_OK);
  COMP_CHECK_MSG(layer_count == 1, "new document has a single background layer");

  int32_t dirty = -1;
  COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);

  comp_engine owner = nullptr;
  COMP_CHECK(comp_document_engine(document, &owner) == COMP_OK);
  COMP_CHECK(owner == engine);

  /* 画布上限校验：超限尺寸必须返回 INVALID_ARGUMENT。 */
  comp_document_desc huge = document_desc;
  huge.width = 40000;
  comp_document invalid = nullptr;
  COMP_CHECK(comp_document_create(engine, &huge, &invalid) == COMP_ERR_INVALID_ARGUMENT);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_layer_pixels_and_flatten) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);

  const SolidImage red(8, 8, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  std::vector<uint8_t> canvas = flatten(document, 8, 8);
  COMP_CHECK(matches(canvas, 8, 0, 0, 255, 0, 0, 255, 0));
  COMP_CHECK(matches(canvas, 8, 7, 7, 255, 0, 0, 255, 0));

  /* 叠一层不透明绿：Normal 下顶层完全覆盖。 */
  comp_layer_desc layer_desc{};
  layer_desc.name = "Green";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, 8, 8};
  comp_layer_id green = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &green) == COMP_OK);
  COMP_CHECK(green != 0 && green != background);

  const SolidImage green_pixels(8, 8, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, green, &green_pixels.image) == COMP_OK);

  canvas = flatten(document, 8, 8);
  COMP_CHECK(matches(canvas, 8, 4, 4, 0, 255, 0, 255, 0));

  /* 导出的图层顺序：背景在下、绿在上。 */
  comp_layer_id ids[8] = {0};
  size_t count = 0;
  COMP_CHECK(comp_layer_list(document, ids, 8, &count) == COMP_OK);
  COMP_CHECK(count == 2);
  COMP_CHECK(ids[0] == background && ids[1] == green);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_blend_multiply_and_opacity) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  comp_layer_desc layer_desc{};
  layer_desc.name = "Green";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id green = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &green) == COMP_OK);
  const SolidImage green_pixels(4, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, green, &green_pixels.image) == COMP_OK);

  /* Multiply(红基底, 绿上层) = 黑，且结果不透明。 */
  COMP_CHECK(comp_layer_set_blend(document, green, COMP_BLEND_MULTIPLY) == COMP_OK);
  comp_blend_mode read_blend = COMP_BLEND_NORMAL;
  COMP_CHECK(comp_layer_get_blend(document, green, &read_blend) == COMP_OK);
  COMP_CHECK(read_blend == COMP_BLEND_MULTIPLY);

  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK(matches(canvas, 4, 1, 1, 0, 0, 0, 255, 0));

  /* 回到 Normal，opacity 0.5：预乘结果约为 (128, 128, 0, 255)。 */
  COMP_CHECK(comp_layer_set_blend(document, green, COMP_BLEND_NORMAL) == COMP_OK);
  COMP_CHECK(comp_layer_set_opacity(document, green, 0.5f) == COMP_OK);

  float read_opacity = 0.0f;
  COMP_CHECK(comp_layer_get_opacity(document, green, &read_opacity) == COMP_OK);
  COMP_CHECK(read_opacity > 0.49f && read_opacity < 0.51f);

  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 128, 128, 0, 255, 3), "50% green over red");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * 组隔离（comp_layer_set_isolated / comp_layer_get_isolated）。
 *
 * 隔离语义此前只有引擎内部的一条测试能触发（直接改 Layer::isolated），ABI 既没有
 * 开入口也没有回读口，外壳无法开启隔离组、也无法把该状态展示或落盘。这里以「壳的
 * 视角」验证这一对入口接通到了真实合成路径：默认穿透时子层的 Multiply 会打到组外
 * 背景（黑），开启隔离后子树先自合成成单元再以 Normal 叠加（绿）。
 */
COMP_TEST(abi_group_isolation_round_trip_and_composite) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id base = background_layer(document);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, base, &red.image) == COMP_OK);

  comp_layer_desc group_desc{};
  group_desc.name = "Group";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  comp_layer_desc child_desc{};
  child_desc.name = "Child";
  child_desc.blend = COMP_BLEND_MULTIPLY;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id child = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &child) == COMP_OK);
  const SolidImage green(4, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, child, &green.image) == COMP_OK);

  /* 默认穿透：子的 Multiply 打到组外的红 -> 黑。 */
  int32_t isolated = -1;
  COMP_CHECK(comp_layer_get_isolated(document, group, &isolated) == COMP_OK);
  COMP_CHECK_MSG(isolated == 0, "groups default to pass-through");
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 0, 255, 0), "pass-through multiply hits backdrop");

  /* 开启隔离：子树先自合成到透明底（Multiply 对透明仍是绿），再以 Normal 叠红 -> 绿。 */
  COMP_CHECK(comp_layer_set_isolated(document, group, 1) == COMP_OK);
  COMP_CHECK(comp_layer_get_isolated(document, group, &isolated) == COMP_OK);
  COMP_CHECK_MSG(isolated == 1, "isolated flag reads back");
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 255, 0, 255, 0), "isolated group composites as a unit");

  /* 关回去：必须回到穿透（写读都走统一事务，故可撤销、可来回切）。 */
  COMP_CHECK(comp_layer_set_isolated(document, group, 0) == COMP_OK);
  COMP_CHECK(comp_layer_get_isolated(document, group, &isolated) == COMP_OK);
  COMP_CHECK_MSG(isolated == 0, "isolation can be turned back off");
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 0, 255, 0), "pass-through restored");

  /* 非组图层：写入被原样保存（与 isVisible 等属性一致），但不参与合成。 */
  COMP_CHECK(comp_layer_set_isolated(document, base, 1) == COMP_OK);
  COMP_CHECK(comp_layer_get_isolated(document, base, &isolated) == COMP_OK);
  COMP_CHECK_MSG(isolated == 1, "non-group write is stored verbatim");
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 0, 255, 0), "non-group isolation does not isolate");

  /* 参数与 id 校验。 */
  COMP_CHECK(comp_layer_get_isolated(document, group, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_isolated(document, 999999, &isolated) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_set_isolated(document, 999999, 1) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_undo_redo_and_history_stats) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  size_t undo_depth = 0;
  size_t redo_depth = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, &redo_depth) == COMP_OK);
  COMP_CHECK_MSG(undo_depth >= 1, "layer edit is undoable");

  int32_t can_undo = 0;
  COMP_CHECK(comp_document_can_undo(document, &can_undo) == COMP_OK);
  COMP_CHECK(can_undo == 1);

  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK(matches(canvas, 4, 0, 0, 0, 0, 0, 0, 0));

  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK(matches(canvas, 4, 0, 0, 255, 0, 0, 255, 0));

  /* 撤销栈上限：设为 1 后深度不得超过 1。 */
  COMP_CHECK(comp_document_set_history_limit(document, 1) == COMP_OK);
  const SolidImage blue(4, 4, 0, 0, 255, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &blue.image) == COMP_OK);
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, &redo_depth) == COMP_OK);
  COMP_CHECK(undo_depth <= 1);

  /* 撤销栈内存预算：查询当前占用，并把预算压到 1 字节。 */
  size_t history_bytes = 0;
  COMP_CHECK(comp_document_history_bytes(document, &history_bytes) == COMP_OK);
  COMP_CHECK_MSG(history_bytes > 0, "undo entries report estimated bytes");

  /* out_bytes 可为 NULL。 */
  COMP_CHECK(comp_document_history_bytes(document, nullptr) == COMP_OK);

  COMP_CHECK(comp_document_set_history_budget(document, 1) == COMP_OK);
  COMP_CHECK(comp_document_can_undo(document, &can_undo) == COMP_OK);
  COMP_CHECK_MSG(can_undo == 1, "budget eviction keeps the newest undo entry");
  COMP_CHECK_MSG(comp_document_undo(document) == COMP_OK, "undo still works under a tiny budget");

  /* 0 恢复默认预算。 */
  COMP_CHECK(comp_document_set_history_budget(document, 0) == COMP_OK);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * N5：P1-3 新增的「撤销历史已截断」标记（`comp_document_history_truncated`）此前零单测——
 * `abi_undo_redo_and_history_stats` 只断言了 limit / budget / history_bytes，从未验证截断标记。
 * 本用例覆盖三条：新文档为 0；步数上限淘汰后锁定为 1；锁定后不因撤销栈走空而复位。
 * 加上内存上限这条同类淘汰路径，确认两条淘汰分支都点亮同一标记。
 */
COMP_TEST(abi_history_truncated_flag) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  const SolidImage blue(4, 4, 0, 0, 255, 255);
  const SolidImage green(4, 4, 0, 255, 0, 255);

  /* 新文档：从未淘汰过记录。 */
  int32_t truncated = -1;
  COMP_CHECK(comp_document_history_truncated(document, &truncated) == COMP_OK);
  COMP_CHECK_MSG(truncated == 0, "fresh document reports no truncation");

  /* out_truncated 为 NULL：报参数错误而不是崩溃。 */
  COMP_CHECK(comp_document_history_truncated(document, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  /* 步数上限压到 1，连做两次编辑：第二次 push 必然淘汰最旧的一条。 */
  COMP_CHECK(comp_document_set_history_limit(document, 1) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, background, &blue.image) == COMP_OK);
  COMP_CHECK(comp_document_history_truncated(document, &truncated) == COMP_OK);
  COMP_CHECK_MSG(truncated == 1, "step-limit eviction latches the truncated flag");

  /* 标记是锁定的：把撤销栈走空也不复位（只有 History::clear() 复位）。 */
  size_t undo_depth = 0;
  size_t redo_depth = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, &redo_depth) == COMP_OK);
  for (size_t i = 0; i < undo_depth; ++i) {
    COMP_CHECK(comp_document_undo(document) == COMP_OK);
  }
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, &redo_depth) == COMP_OK);
  COMP_CHECK_MSG(undo_depth == 0, "undo stack drained");
  COMP_CHECK(comp_document_history_truncated(document, &truncated) == COMP_OK);
  COMP_CHECK_MSG(truncated == 1, "truncation flag stays latched until clear()");

  comp_document_destroy(document);

  /* 内存上限是同一标记的另一条淘汰路径：预算压到 1 字节后再连做两次编辑。 */
  document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  const comp_layer_id background2 = background_layer(document);
  COMP_CHECK(comp_document_set_history_budget(document, 1) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, background2, &red.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, background2, &green.image) == COMP_OK);
  COMP_CHECK(comp_document_history_truncated(document, &truncated) == COMP_OK);
  COMP_CHECK_MSG(truncated == 1, "budget eviction latches the same truncated flag");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * P1 历史面板：步数 / 逐步标签 / 按步回跳。三者都只是「在既有撤销栈上取信息
 * 或移动下标」，不新增历史记录——所以回跳后 Ctrl+Z/Ctrl+Y 的语义与逐次撤销
 * 完全一致（这里用 flatten 像素验证落点，用 history_stats 验证下标）。
 */
COMP_TEST(abi_history_panel_steps_labels_and_jump) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  const SolidImage blue(4, 4, 0, 0, 255, 255);

  /* 基线步数：新建文档通常为 0，但按引擎实际值取，避免绑定实现细节。 */
  size_t base_steps = 12345;
  COMP_CHECK(comp_document_history_step_count(document, &base_steps) == COMP_OK);

  comp_txn txn = nullptr;
  COMP_CHECK(comp_txn_begin(document, "Alpha", &txn) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);
  COMP_CHECK(comp_txn_commit(txn) == COMP_OK);
  COMP_CHECK(comp_txn_begin(document, "Beta", &txn) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, background, &blue.image) == COMP_OK);
  COMP_CHECK(comp_txn_commit(txn) == COMP_OK);

  size_t steps = 0;
  COMP_CHECK(comp_document_history_step_count(document, &steps) == COMP_OK);
  COMP_CHECK_MSG(steps == base_steps + 2, "each committed txn adds one step");
  /* out_steps 可为 NULL（与 history_stats 同口径）。 */
  COMP_CHECK(comp_document_history_step_count(document, nullptr) == COMP_OK);

  /* 标签：查询模式回所需长度（含 NUL），并原样写入缓冲区。 */
  size_t required = 0;
  COMP_CHECK(comp_document_history_step_label(document, steps, nullptr, 0, &required) ==
             COMP_OK);
  COMP_CHECK_MSG(required == 5, "label length includes the trailing NUL");
  char label[16] = {};
  COMP_CHECK(comp_document_history_step_label(document, steps, label, sizeof(label),
                                              &required) == COMP_OK);
  COMP_CHECK(std::strcmp(label, "Beta") == 0);
  COMP_CHECK(comp_document_history_step_label(document, base_steps + 1, label, sizeof(label),
                                              &required) == COMP_OK);
  COMP_CHECK(std::strcmp(label, "Alpha") == 0);

  /* 边界：index 0 = 最初状态（没有标签）；越界、缓冲区不够、缺 out 参数都拒绝。 */
  COMP_CHECK(comp_document_history_step_label(document, 0, label, sizeof(label), &required) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_document_history_step_label(document, steps + 1, label, sizeof(label),
                                              &required) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_document_history_step_label(document, steps, label, 2, &required) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_document_history_step_label(document, steps, label, sizeof(label), nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);

  /* 回跳到最初：像素回到空文档，下标归零，标签仍在（记录进了重做栈）。 */
  COMP_CHECK(comp_document_history_jump(document, base_steps) == COMP_OK);
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK(matches(canvas, 4, 0, 0, 0, 0, 0, 0, 0));
  size_t undo_depth = 999;
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_depth == base_steps, "jump backwards moves the current index");
  COMP_CHECK(comp_document_history_step_label(document, steps, label, sizeof(label),
                                              &required) == COMP_OK);
  COMP_CHECK(std::strcmp(label, "Beta") == 0);

  /* 跳回最新：像素回来；同一下标再跳是空操作（不多出撤销步）。 */
  COMP_CHECK(comp_document_history_jump(document, steps) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK(matches(canvas, 4, 0, 0, 0, 0, 255, 255, 0));
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, nullptr) == COMP_OK);
  COMP_CHECK(undo_depth == steps);
  COMP_CHECK(comp_document_history_jump(document, steps) == COMP_OK);
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, nullptr) == COMP_OK);
  COMP_CHECK(undo_depth == steps);

  /* 越界拒绝；活动事务内拒绝（否则 commit 会把事务快照与回跳状态撕开）。 */
  COMP_CHECK(comp_document_history_jump(document, steps + 1) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_txn_begin(document, "Gamma", &txn) == COMP_OK);
  COMP_CHECK(comp_document_history_jump(document, base_steps) == COMP_ERR_STATE);
  COMP_CHECK(comp_txn_abort(txn) == COMP_OK);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_explicit_transaction) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);

  int32_t active = -1;
  COMP_CHECK(comp_txn_active(document, &active) == COMP_OK);
  COMP_CHECK(active == 0);

  comp_txn txn = nullptr;
  COMP_CHECK(comp_txn_begin(document, "Paint", &txn) == COMP_OK);
  COMP_CHECK(txn != nullptr);
  COMP_CHECK(comp_txn_active(document, &active) == COMP_OK);
  COMP_CHECK(active == 1);

  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);
  COMP_CHECK(comp_txn_commit(txn) == COMP_OK);

  size_t undo_depth = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_depth == 1, "one explicit transaction = one undo step");

  /* 中止事务：状态回滚，且不产生撤销步。 */
  comp_txn abandoned = nullptr;
  COMP_CHECK(comp_txn_begin(document, "Abandoned", &abandoned) == COMP_OK);
  const SolidImage green(4, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &green.image) == COMP_OK);
  COMP_CHECK(comp_txn_abort(abandoned) == COMP_OK);

  size_t after_abort = 0;
  COMP_CHECK(comp_document_history_stats(document, &after_abort, nullptr) == COMP_OK);
  COMP_CHECK(after_abort == undo_depth);

  const std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 0, 0, 255, 0, 0, 255, 0), "abort rolls back pixels");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_command_model) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* count-only 枚举不要求文档；带 desc 的枚举按文档状态求值 enabled。 */
  size_t command_count = 0;
  COMP_CHECK(comp_command_list(nullptr, nullptr, 0, &command_count) == COMP_OK);
  COMP_CHECK(command_count >= 5);

  std::vector<comp_command_desc> commands(command_count);
  size_t written = 0;
  COMP_CHECK(comp_command_list(document, commands.data(), commands.size(), &written) == COMP_OK);
  COMP_CHECK(written == command_count);

  bool found_undo = false;
  bool all_disabled_undo = true;
  for (const comp_command_desc& command : commands) {
    COMP_CHECK(command.id != nullptr && command.title != nullptr);
    if (std::string(command.id) == "edit.undo") {
      found_undo = true;
      /* 新文档没有可撤销内容：列表与逐条查询必须给出一致的答案。 */
      if (command.enabled != 0) all_disabled_undo = false;
    }
  }
  COMP_CHECK(found_undo);
  COMP_CHECK_MSG(all_disabled_undo,
                 "command_list enabled matches command_state on a fresh document");

  /* 新文档：无可撤销内容。 */
  int32_t enabled = -1;
  int32_t is_on = -1;
  COMP_CHECK(comp_command_state(document, "edit.undo", &enabled, &is_on) == COMP_OK);
  COMP_CHECK(enabled == 0);

  COMP_CHECK(comp_command_state(document, "no.such.command", &enabled, &is_on) ==
             COMP_ERR_NOT_FOUND);

  /* 执行 layer.add：图层数 +1，撤销变为可用。 */
  size_t layers_before = 0;
  COMP_CHECK(comp_layer_count(document, &layers_before) == COMP_OK);
  COMP_CHECK(comp_command_invoke(document, "layer.add", nullptr) == COMP_OK);

  size_t layers_after = 0;
  COMP_CHECK(comp_layer_count(document, &layers_after) == COMP_OK);
  COMP_CHECK(layers_after == layers_before + 1);

  COMP_CHECK(comp_command_state(document, "edit.undo", &enabled, &is_on) == COMP_OK);
  COMP_CHECK(enabled == 1);

  COMP_CHECK(comp_command_invoke(document, "edit.undo", nullptr) == COMP_OK);
  size_t layers_after_undo = 0;
  COMP_CHECK(comp_layer_count(document, &layers_after_undo) == COMP_OK);
  COMP_CHECK(layers_after_undo == layers_before);

  /* file.save 需要宿主路径，引擎侧返回 UNSUPPORTED 而非静默成功。 */
  COMP_CHECK(comp_command_invoke(document, "file.save", nullptr) == COMP_ERR_UNSUPPORTED);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_io_format_detection_and_codec_scope) {
  COMP_CHECK(comp_io_format_from_path("a/b/c.png") == COMP_FORMAT_PNG);
  COMP_CHECK(comp_io_format_from_path("C:\\tmp\\shot.JPEG") == COMP_FORMAT_JPEG);
  COMP_CHECK(comp_io_format_from_path("image.tiff") == COMP_FORMAT_TIFF);
  COMP_CHECK(comp_io_format_from_path("image.heic") == COMP_FORMAT_HEIF);
  COMP_CHECK(comp_io_format_from_path("image.webp") == COMP_FORMAT_WEBP);
  COMP_CHECK(comp_io_format_from_path("image.exr") == COMP_FORMAT_EXR);
  COMP_CHECK(comp_io_format_from_path("image.psd") == COMP_FORMAT_PSD);
  COMP_CHECK(comp_io_format_from_path("project.comp") == COMP_FORMAT_UNKNOWN);
  COMP_CHECK(comp_io_format_from_path("no-extension") == COMP_FORMAT_UNKNOWN);

  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* M5：PNG 与 JPEG 已有真实编解码；仍未落地的格式必须显式 UNSUPPORTED 且给出原因。 */
  comp_codec_state state = COMP_CODEC_UNAVAILABLE;
  size_t outside_note_size = 0;
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_PNG, &state, nullptr, 0, &outside_note_size) == COMP_OK);
  COMP_CHECK(state == COMP_CODEC_VENDORED || state == COMP_CODEC_BUILTIN);
  COMP_CHECK(outside_note_size > 1);

  state = COMP_CODEC_UNAVAILABLE;
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_JPEG, &state, nullptr, 0, &outside_note_size) == COMP_OK);
  COMP_CHECK(state == COMP_CODEC_VENDORED);
  std::vector<char> note(outside_note_size + 1, '\0');
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_JPEG, nullptr, note.data(), note.size(), &outside_note_size) ==
             COMP_OK);
  COMP_CHECK(std::strstr(note.data(), "stb_image") != nullptr);

  /* 未登记格式：NOT_FOUND，但也如实报告不可用。 */
  state = COMP_CODEC_BUILTIN;
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_UNKNOWN, &state, nullptr, 0, &outside_note_size) ==
             COMP_ERR_NOT_FOUND);
  COMP_CHECK(state == COMP_CODEC_UNAVAILABLE);

  /* 仍未落地的格式（TIFF 等）：UNSUPPORTED；JPEG 已可用但文件缺失属 IO。 */
  comp_document imported = nullptr;
  COMP_CHECK(comp_io_import(engine, "photo.jpg", &imported) == COMP_ERR_IO);
  COMP_CHECK(imported == nullptr);

  comp_layer_id imported_layer = 0;
  COMP_CHECK(comp_io_import_layer(document, "photo.jpg", &imported_layer) == COMP_ERR_IO);

  COMP_CHECK(comp_io_export(document, "out.tiff", COMP_FORMAT_TIFF, nullptr) == COMP_ERR_UNSUPPORTED);

  /* PNG 已内建：文件不存在时是 IO 错误，而不是「不支持」。 */
  COMP_CHECK(comp_io_import(engine, "build/__no_such_file__.png", &imported) == COMP_ERR_IO);
  COMP_CHECK(imported == nullptr);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_project_round_trip) {
  std::error_code ec;
  std::filesystem::create_directories("build", ec);
  const std::string project_path = "build/abi_roundtrip.comp";

  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 6;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const SolidImage red(6, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  comp_layer_desc layer_desc{};
  layer_desc.name = "Green";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, 6, 4};
  comp_layer_id green = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &green) == COMP_OK);
  const SolidImage green_pixels(6, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, green, &green_pixels.image) == COMP_OK);

  COMP_CHECK(comp_document_save(document, project_path.c_str()) == COMP_OK);

  comp_project_info info{};
  COMP_CHECK(comp_io_probe_project(project_path.c_str(), &info) == COMP_OK);
  COMP_CHECK(info.width == 6 && info.height == 4);
  COMP_CHECK(info.layer_count == 2);
  COMP_CHECK(info.version >= 1);
  COMP_CHECK(std::string(info.format) == "com.compositor.project");

  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, project_path.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(loaded != nullptr);

  comp_size size{0, 0};
  COMP_CHECK(comp_document_size(loaded, &size) == COMP_OK);
  COMP_CHECK(size.width == 6 && size.height == 4);

  int32_t dirty = 1;
  COMP_CHECK(comp_document_is_dirty(loaded, &dirty) == COMP_OK);
  COMP_CHECK_MSG(dirty == 0, "freshly loaded project is not dirty");

  const std::vector<uint8_t> canvas = flatten(loaded, 6, 4);
  COMP_CHECK_MSG(matches(canvas, 6, 3, 2, 0, 255, 0, 255, 0), "pixels survive save/load");

  comp_document_destroy(loaded);
  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_layer_thumbnail) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 32;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const SolidImage red(32, 16, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  std::vector<uint8_t> thumbnail(8 * 4 * 4, 0);
  comp_size thumbnail_size{0, 0};
  const comp_result result = comp_render_layer_thumbnail(document, background, /*max_size=*/8,
                                                         thumbnail.data(), 8 * 4,
                                                         &thumbnail_size);
  COMP_CHECK(result == COMP_OK);
  COMP_CHECK(thumbnail_size.width <= 8 && thumbnail_size.height <= 8);
  COMP_CHECK(thumbnail_size.width > 0 && thumbnail_size.height > 0);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ==================================================================
 * 第三轮：架构风险收口的回归护栏。
 * 每条对应一处已修复的架构级风险（详见报告第六节）。
 * ================================================================== */

/* 句柄有效性：销毁后的文档句柄继续使用必须得到错误码，而不是静默解引用
 * （对象 magic 在析构时擦除）。 */
COMP_TEST(destroyed_document_handle_is_rejected) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  comp_document_destroy(document);

  /* 销毁后：所有入口一律拒绝，不再有 use-after-free。 */
  comp_size size{};
  COMP_CHECK_MSG(comp_document_size(document, &size) == COMP_ERR_INVALID_ARGUMENT,
                 "destroyed document handle is rejected, not dereferenced");
  int32_t dirty = -1;
  COMP_CHECK_MSG(comp_document_is_dirty(document, &dirty) == COMP_ERR_INVALID_ARGUMENT,
                 "destroyed document handle rejected by a second entry point too");

  /* 引擎本身不受影响（文档销毁不波及引擎）。 */
  uint32_t threads = 0;
  COMP_CHECK(comp_engine_threads(engine, &threads) == COMP_OK);

  /* 重复 destroy 为无操作。 */
  comp_document_destroy(document);

  comp_engine_destroy(engine);
}

/* 反序生命周期：宿主先销毁引擎时，在册文档必须被一并回收，
 * 且「分配/释放」必须成对（经同一分配器）。 */
COMP_TEST(engine_destroy_cleans_up_documents_in_order) {
  Counting counting;

  comp_engine_desc desc = test_engine_desc();
  desc.alloc.alloc = counting_alloc;
  desc.alloc.free = counting_free;
  desc.alloc.user = &counting;

  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  /* 引擎对象本身经定制分配器（此前 desc.alloc 收下从不使用）。 */
  COMP_CHECK_MSG(counting.allocations >= 1, "engine object flows through the custom allocator");

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK_MSG(counting.allocations >= 2, "document object flows through the custom allocator");

  /* 反序：先销毁引擎。~Engine 兜底销毁在册文档。 */
  comp_engine_destroy(engine);

  COMP_CHECK_MSG(counting.allocations == counting.releases,
                 "engine destroy releases every object it allocated (documents included)");

  /* 引擎已销毁：其句柄同样失效。 */
  uint32_t threads = 0;
  COMP_CHECK(comp_engine_threads(engine, &threads) == COMP_ERR_INVALID_ARGUMENT);
}

/* 分配器契约：alloc 与 free 必须成对，只填一半在引擎创建时即被拒绝
 * （块无法归还 / 来源不明指针都是调用方错误，早失败好过带进运行期）。 */
COMP_TEST(engine_create_rejects_unpaired_allocator) {
  comp_engine_desc alloc_only = test_engine_desc();
  alloc_only.alloc.alloc = counting_alloc;
  comp_engine engine = nullptr;
  COMP_CHECK_MSG(comp_engine_create(&alloc_only, &engine) == COMP_ERR_INVALID_ARGUMENT,
                 "alloc without free is rejected at engine creation");

  comp_engine_desc free_only = test_engine_desc();
  free_only.alloc.free = counting_free;
  COMP_CHECK_MSG(comp_engine_create(&free_only, &engine) == COMP_ERR_INVALID_ARGUMENT,
                 "free without alloc is rejected at engine creation");

  /* 成对提供仍然放行（默认分配器路径不受影响）。 */
  comp_engine_desc plain{};
  COMP_CHECK(comp_engine_create(&plain, &engine) == COMP_OK);
  comp_engine_destroy(engine);
}

/* 类型混淆：把引擎句柄当文档句柄传，必须被拒绝而不是把 Engine 当 Document 用。 */
COMP_TEST(handle_type_confusion_is_rejected) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document fake = reinterpret_cast<comp_document>(engine);
  comp_size size{};
  COMP_CHECK_MSG(comp_document_size(fake, &size) == COMP_ERR_INVALID_ARGUMENT,
                 "engine handle used as document handle is rejected before any dereference");
  comp_document_destroy(fake); /* 无操作，不能真的删掉引擎 */

  /* 反向：文档句柄当引擎句柄传，同样必须拒绝。 */
  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  document_desc.dpi = 72.0f;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  if (document != nullptr) {
    comp_engine fake_engine = reinterpret_cast<comp_engine>(document);
    uint32_t fake_threads = 0;
    COMP_CHECK_MSG(comp_engine_threads(fake_engine, &fake_threads) == COMP_ERR_INVALID_ARGUMENT,
                   "document handle used as engine handle is rejected");
    comp_engine_destroy(fake_engine); /* 无操作，不能真的删掉文档 */

    comp_size still_valid{};
    COMP_CHECK(comp_document_size(document, &still_valid) == COMP_OK);
    COMP_CHECK(still_valid.width == 8 && still_valid.height == 8);

    comp_document_destroy(document);
  }

  /* 引擎仍然活着且可用。 */
  uint32_t threads = 0;
  COMP_CHECK(comp_engine_threads(engine, &threads) == COMP_OK);

  comp_engine_destroy(engine);
  /* 重复 destroy 引擎同样无操作。 */
  comp_engine_destroy(engine);
}

/*
 * 野句柄：完全未映射的地址传给引擎 / 文档入口不得段错误。
 *
 * as_engine / as_document 解引用前先查「存活对象登记表」（core/comp_live_handles.h），
 * 查不到就判 nullptr。刻意用 2^48——所有受支持平台的用户态地址空间之外，保证
 * 未映射：一旦退回「直接解引用读 magic」的老路径，本测试会段错误而不是断言失败，
 * 这正是要钉死的行为（与 font 侧 font_abi_wild_and_released_handles_never_crash
 * 同构）。
 */
COMP_TEST(wild_engine_document_handles_never_crash) {
  const comp_engine kWildEngine = reinterpret_cast<comp_engine>(0x1000000000000ULL);
  const comp_document kWildDocument = reinterpret_cast<comp_document>(0x1000000000000ULL);

  comp_backend backend = COMP_BACKEND_CPU;
  COMP_CHECK(comp_engine_backend(kWildEngine, &backend) == COMP_ERR_INVALID_ARGUMENT);
  uint32_t threads = 0;
  COMP_CHECK(comp_engine_threads(kWildEngine, &threads) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_engine_last_error(kWildEngine) != nullptr); /* 返回空串，不是崩溃 */
  comp_engine_destroy(kWildEngine); /* 野句柄释放必须是空操作，不崩 */

  comp_size size{};
  COMP_CHECK(comp_document_size(kWildDocument, &size) == COMP_ERR_INVALID_ARGUMENT);
  comp_document_destroy(kWildDocument);

  /* 真实句柄仍然好用：本测试通过本身也证明登记表没有误伤正常路径。 */
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(comp_engine_threads(engine, &threads) == COMP_OK);

  /* 引擎句柄当文档句柄用：登记表命中（Engine 活着）但 magic 挡下——
   * 同样是 INVALID_ARGUMENT，不是崩溃。 */
  comp_document fake = reinterpret_cast<comp_document>(engine);
  COMP_CHECK(comp_document_size(fake, &size) == COMP_ERR_INVALID_ARGUMENT);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 命令列表：desc 模式必须提供文档（enabled 按文档状态求值）；
 * count-only 模式不要求文档（外壳可在打开文档前枚举命令表）。 */
COMP_TEST(command_list_requires_document_for_desc_mode) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  size_t count = 0;
  COMP_CHECK(comp_command_list(nullptr, nullptr, 0, &count) == COMP_OK);
  COMP_CHECK(count >= 5);

  std::vector<comp_command_desc> commands(count);
  COMP_CHECK_MSG(comp_command_list(nullptr, commands.data(), count, &count) ==
                     COMP_ERR_INVALID_ARGUMENT,
                 "desc mode without a document is rejected (enabled is document-scoped)");

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 新文档：undo 不可用。列表与逐条查询给出一致答案（两套标准已合并）。 */
  size_t written = 0;
  COMP_CHECK(comp_command_list(document, commands.data(), count, &written) == COMP_OK);
  COMP_CHECK(written == count);
  for (const comp_command_desc& command : commands) {
    if (std::string(command.id) == "edit.undo") {
      COMP_CHECK_MSG(command.enabled == 0, "undo disabled on a fresh document");
    }
    if (std::string(command.id) == "layer.add") {
      COMP_CHECK_MSG(command.enabled == 1, "add layer enabled on a fresh document");
    }
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------- AUDIT3：分配器失败路径 */

namespace {

/* 恒失败分配器：验证「分配器返回 null」被如实转成 OOM，且不吐句柄。 */
struct FailingAlloc {
  int calls = 0;
};

void* failing_alloc(size_t bytes, void* user) {
  (void)bytes;
  ++static_cast<FailingAlloc*>(user)->calls;
  return nullptr;
}

void failing_free(void* pointer, void* user) {
  (void)pointer;
  (void)user;
}

/* 可关断分配器：引擎创建期间放行，之后一律失败（document_create 的 OOM 路径）。
 * 开关由测试在两次 create 之间翻转，不依赖引擎内部分配次数。 */
struct SwitchableAlloc {
  int allocations = 0;
  int releases = 0;
  bool allow = true;
};

void* switchable_alloc(size_t bytes, void* user) {
  SwitchableAlloc* state = static_cast<SwitchableAlloc*>(user);
  if (!state->allow) return nullptr;
  ++state->allocations;
  return std::malloc(bytes);
}

void switchable_free(void* pointer, void* user) {
  if (pointer == nullptr) return;
  ++static_cast<SwitchableAlloc*>(user)->releases;
  std::free(pointer);
}

}  // namespace

COMP_TEST(engine_create_fails_cleanly_when_allocator_returns_null) {
  comp_engine_desc desc = test_engine_desc();
  FailingAlloc state;
  desc.alloc.alloc = failing_alloc;
  desc.alloc.free = failing_free;
  desc.alloc.user = &state;

  comp_engine engine = nullptr;
  const comp_result result = comp_engine_create(&desc, &engine);
  COMP_CHECK_MSG(result == COMP_ERR_OUT_OF_MEMORY,
                 "allocator returning null surfaces as OOM, not a crash or a bogus handle");
  COMP_CHECK_MSG(engine == nullptr, "no engine handle escapes on allocation failure");
  COMP_CHECK_MSG(state.calls >= 1, "engine object really went through the injected allocator");
}

COMP_TEST(document_create_fails_cleanly_when_allocator_exhausted) {
  comp_engine_desc desc = test_engine_desc();
  SwitchableAlloc state;
  desc.alloc.alloc = switchable_alloc;
  desc.alloc.free = switchable_free;
  desc.alloc.user = &state;

  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK(engine != nullptr);

  /* 关断分配器：文档对象分配必须失败并如实报 OOM。 */
  state.allow = false;
  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK_MSG(comp_document_create(engine, &document_desc, &document) == COMP_ERR_OUT_OF_MEMORY,
                 "document object allocation failure surfaces as OOM");
  COMP_CHECK(document == nullptr);

  /* 失败路径不损坏引擎：销毁时分配与释放仍然成对（ASAN 下亦可兜住泄漏）。 */
  comp_engine_destroy(engine);
  COMP_CHECK_MSG(state.allocations == state.releases,
                 "engine destroy releases every object it allocated, failure path included");
}

/* C 方案 C1：RGBA16F（半精度 / HDR）图层的真通路。
 *
 * 这条用例改写自旧版的 `layer_set_pixels_rejects_16bit_float_format`——旧版断言
 * 「半精度必须拒绝」，理由是「域层与合成侧没有半精度像素路径」。C1 把那条路径
 * 补上��，断言随之反过来：
 *
 *   1. `comp_layer_set_pixels` 接受 COMP_PIXEL_RGBA16F（8 字节/像素、预乘、
 *      native-endian，每通道一个 IEEE 754 half）。
 *   2. 合成走 `hdr_to_rgba8`：先 `deep_half_to_u16` 按 0..65535 取整
 *      （对齐上游 `DeepLayer.image(_:)`，white 以上的光截断），再 dither 取整。
 *
 * 关键陷阱（也是旧版拒绝理由真正成立的地方，现由本用例钉住）：
 * **半精度的位模式不能当 16 位整数读。** half 的 1.0 是 0x3C00（15360），
 * 若原样取整成 8 位就是 15360>>8 = 60 而非 255。因此本用例铺的 half 数据里
 * 故意包含 0x3C00（1.0）这个值，并断言画布是接近白色的高值（>= 200）；
 * 若哪天有人把 hdr_to_rgba8 换成直接按 16 位读，这个断言会立刻挂。 */
COMP_TEST(layer_set_pixels_accepts_halffloat_format) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);

  /*
   * 不透明纯白（half 1.0 = 0x3C00 的位模式）铺满：
   * `deep_half_to_u16` 应把每个分量算成 65535，dither 后落成 255 左右。
   * 若半精度被当成 16 位整数（0x3C00 = 15360），8 位视图会是 60 左右的深灰。
   */
  std::vector<uint16_t> hdr(4u * 4u * 4u, 0);
  for (size_t i = 0; i < hdr.size(); ++i) {
    hdr[i] = 0x3c00; /* half 1.0 */
  }
  comp_image image{};
  image.width = 4;
  image.height = 4;
  image.format = COMP_PIXEL_RGBA16F;
  image.stride = 4u * 8u;
  image.pixels = hdr.data();
  COMP_CHECK_MSG(comp_layer_set_pixels(document, background, &image) == COMP_OK,
                 "half-float layer pixels are accepted once the HDR pixel path exists");

  const std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 255, 255, 255, 0),
                 "half 1.0 shows as white: the bit pattern is not read as a 16-bit integer");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* HDR 图层的 8 位视图：预乘、高光截断、half 不当整数读。
 *
 * 铺的是背景**黑**（不是白：白底上 alpha=0.5 的层合成出来仍是不透明白，
 * 任何算错都可能被掩盖）。HDR 层每个像素 alpha = half 0.5、三个色分量
 * = half 2.0；其中 (1,1) 换成 half 的最大值 0x7BFF（65504）。
 *
 * 三件事被同时钉住：
 *   1. **half 不能当 16 位整数读。** 若按位模式当整数，2.0（0x4000 = 16384）
 *      会得 8 位的 64、alpha 0.5（0x3800 = 14336）会得 56，黑底上得 ~(64,64,64,255)
 *      的中灰；与正确值「白」一眼可分。
 *   2. **预乘。** 色 2.0 配 alpha 0.5 是直通 4.0（白点以上）；不透明黑底上合成
 *      得预乘 2.0、alpha 1，显示时截到纯白。若把颜色当直通值再用 alpha 压一次，
 *      白会被算成 128——这正是「先夹白再乘 alpha」的 8 位中间缓冲才会有的结果。
 *   3. **高光截断而非回绕。** (1,1) 的 half 是 65504（比 2.0 亮三万多倍），
 *      正确做法是同样截到白；不加夹紧则 65504×65535 取整后回绕成乱色。
 *      断言 (1,1) 与 (0,0) 完全相同即证明截断生效。
 */
COMP_TEST(hdr_highlights_clamp_to_white_in_eight_bit_view) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const SolidImage black(4, 4, 0, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &black.image) == COMP_OK);

  comp_layer_desc hdr_desc{};
  hdr_desc.name = "HDR";
  hdr_desc.blend = COMP_BLEND_NORMAL;
  hdr_desc.opacity = 1.0f;
  hdr_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id hdr_layer = 0;
  COMP_CHECK(comp_layer_add(document, &hdr_desc, &hdr_layer) == COMP_OK);

  std::vector<uint16_t> hdr(4u * 4u * 4u, 0);
  for (size_t p = 0; p < 4u * 4u; ++p) {
    /* 0x4000 = half 2.0，0x7BFF = half 65504（可表示的最大值）。 */
    const uint16_t color = (p == 5u) ? 0x7bff : 0x4000;
    hdr[p * 4u + 0u] = color;
    hdr[p * 4u + 1u] = color;
    hdr[p * 4u + 2u] = color;
    hdr[p * 4u + 3u] = 0x3800; /* half 0.5 */
  }
  comp_image image{};
  image.width = 4;
  image.height = 4;
  image.format = COMP_PIXEL_RGBA16F;
  image.stride = 4u * 8u;
  image.pixels = hdr.data();
  COMP_CHECK_MSG(comp_layer_set_pixels(document, hdr_layer, &image) == COMP_OK,
                 "half-float layer pixels are accepted on a new layer too");

  const std::vector<uint8_t> canvas = flatten(document, 4, 4);
  /*
   * 期望是纯白。half 像素是**预乘**的：色 2.0 配 alpha 0.5 意味着直通分量
   * 4.0（白点以上），压在**不透明**黑底上按 W3C 公式得预乘 0.5×4.0 = 2.0、
   * alpha 1 —— 仍是高光。C5 起合成表面跟文档深度走（有 HDR 图层就是 half），
   * 这个 2.0 在合成途中不被截断；只有落到 RGBA8 渲染目标时，显示副本才按
   * 上游 `image(_:)` 的口径「白点以上显示为白」截成 255。
   *
   * 注意 alpha 断言是 255：背景是**不透明**黑，它把整幅的 alpha 拉满；
   * HDR 层那 0.5 的 alpha 体现在预乘颜色上（2.0×0.5 的直通 4.0），不是把
   * 白压成 128——128 只有在 8 位中间缓冲里先夹白再乘 alpha 才会出现。
   */
  COMP_CHECK_MSG(matches(canvas, 4, 0, 0, 255, 255, 255, 255, 2),
                 "an HDR layer reads as half floats, premultiplied, over black");
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 255, 255, 255, 2),
                 "half 65504 clamps to white exactly like half 2.0 instead of wrapping");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 16 位图层的真通路（本轮收口）：
 *
 *   1. `comp_layer_set_pixels` 接受 COMP_PIXEL_RGBA16（预乘、native-endian）；
 *   2. 合成按该图层的 8 位副本进行（加约一阶噪声后取整，上游 DeepLayer.dithered
 *      同款内核）——所以画面是「接近原值的不透明灰」，而不是按错步长读出来的
 *      垃圾（旧行为下每像素读到像素中间的字节，alpha 时有时无）；
 *   3. `COMP_TOOL_DEEP`（ABI 只暴露 quantize）打在同一张 16 位图层上，得到的 8 位
 *      图层与第 2 步用的是同一份副本：两条路径的整幅合成逐字节相同。
 *
 * 这条等价关系就是 ABI 文档里对 16 位格式的承诺（comp_layer.h 的格式契约），
 * 也是 AUDIT7 §6「Deep 降位路径不可达」的收口证据。 */
COMP_TEST(sixteen_bit_layer_composites_as_its_eight_bit_copy) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  /* 16 位资产：预乘、半透明、颜色呈梯度（纯色会让「按错步长读」看起来也对）。 */
  std::vector<uint16_t> wide(4u * 4u * 4u, 0);
  for (int32_t y = 0; y < 4; ++y) {
    for (int32_t x = 0; x < 4; ++x) {
      uint16_t* px = wide.data() + (static_cast<size_t>(y) * 4u + static_cast<size_t>(x)) * 4u;
      const uint16_t alpha = static_cast<uint16_t>(0x4000 + 0x1000 * (x + y));
      px[3] = alpha;
      px[0] = static_cast<uint16_t>(alpha / 2u);            /* 预乘：rgb <= alpha */
      px[1] = static_cast<uint16_t>(alpha / 3u);
      px[2] = static_cast<uint16_t>(alpha / 4u);
    }
  }
  comp_image image{};
  image.width = 4;
  image.height = 4;
  image.format = COMP_PIXEL_RGBA16;
  image.stride = 4u * 8u;
  image.pixels = wide.data();

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;

  comp_document as_sixteen = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &as_sixteen) == COMP_OK);
  const comp_layer_id wide_layer = background_layer(as_sixteen);
  COMP_CHECK_MSG(comp_layer_set_pixels(as_sixteen, wide_layer, &image) == COMP_OK,
                 "COMP_PIXEL_RGBA16 layer pixels are accepted");

  comp_document as_eight = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &as_eight) == COMP_OK);
  const comp_layer_id copied_layer = background_layer(as_eight);
  COMP_CHECK(comp_layer_set_pixels(as_eight, copied_layer, &image) == COMP_OK);
  comp_tool_op_desc quantize{};
  quantize.kind = COMP_TOOL_DEEP;
  COMP_CHECK_MSG(comp_tool_apply(as_eight, copied_layer, &quantize, nullptr, 0, nullptr) == COMP_OK,
                 "COMP_TOOL_DEEP quantizes a 16-bit layer to 8 bits");

  const std::vector<uint8_t> sixteen_canvas = flatten(as_sixteen, 4, 4);
  const std::vector<uint8_t> eight_canvas = flatten(as_eight, 4, 4);
  COMP_CHECK_MSG(sixteen_canvas == eight_canvas,
                 "a 16-bit layer composites exactly as its 8-bit working copy");

  /* 逐像素断言，保证上一条不是因为两边都空/都透明而「相等」。
   * 半透明灰：alpha 介于 64..191，rgb 与其成预乘比例（容差覆盖一阶噪声）。 */
  for (int32_t y = 0; y < 4; ++y) {
    for (int32_t x = 0; x < 4; ++x) {
      const size_t index = (static_cast<size_t>(y) * 4u + static_cast<size_t>(x)) * 4u;
      const uint16_t alpha16 = static_cast<uint16_t>(0x4000 + 0x1000 * (x + y));
      const uint8_t expected_alpha =
          static_cast<uint8_t>((static_cast<int32_t>(alpha16) + 128) / 257);
      COMP_CHECK_MSG(matches(sixteen_canvas, 4, x, y, expected_alpha / 2, expected_alpha / 3,
                             expected_alpha / 4, expected_alpha, 3),
                     "the 16-bit layer is composited from its rounded 8-bit copy");
      COMP_CHECK_MSG(sixteen_canvas[index + 3] != 0,
                     "the composite is not transparent (a wrong-stride read would show up here)");
    }
  }

  /*
   * 裁剪基底：再挂一层剪贴子层。合成分支会把基底的 alpha 当裁剪因子逐像素读——
   * 若按 4 字节/像素去读 8 字节/像素的基底，因子就是随机数。两个文档仍须逐字节
   * 相同，且子层必须真的改变了画面（否则这条断言是空的）。
   */
  auto add_clipped_child = [](comp_document doc, comp_layer_id base) {
    comp_layer_desc child_desc{};
    child_desc.name = "clipped";
    child_desc.blend = COMP_BLEND_NORMAL;
    child_desc.opacity = 1.0f;
    child_desc.transform = comp_rect{0, 0, 4, 4};
    child_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
    comp_layer_id child = 0;
    COMP_CHECK(comp_layer_add(doc, &child_desc, &child) == COMP_OK);
    COMP_CHECK(comp_layer_set_clipping_source(doc, child, base) == COMP_OK);
    const SolidImage green(4, 4, 0, 255, 0, 255);
    COMP_CHECK(comp_layer_set_pixels(doc, child, &green.image) == COMP_OK);
  };
  add_clipped_child(as_sixteen, wide_layer);
  add_clipped_child(as_eight, copied_layer);

  const std::vector<uint8_t> clipped_sixteen = flatten(as_sixteen, 4, 4);
  const std::vector<uint8_t> clipped_eight = flatten(as_eight, 4, 4);
  COMP_CHECK_MSG(clipped_sixteen != sixteen_canvas,
                 "the clipped child really contributes (otherwise the next check is vacuous)");
  COMP_CHECK_MSG(clipped_sixteen == clipped_eight,
                 "clipping against a 16-bit base reads the same alpha as against its 8-bit copy");

  comp_document_destroy(as_sixteen);
  comp_document_destroy(as_eight);
  comp_engine_destroy(engine);
}

/* 错误槽线程隔离：一个线程的错误不得覆盖另一个线程的错误
 * （此前 last_error_ 是进程级共享 string，多线程共用引擎即数据竞争）。 */
COMP_TEST(last_error_slot_is_thread_local) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 主线程制造一条错误。 */
  int32_t enabled = -1;
  int32_t is_on = -1;
  COMP_CHECK(comp_command_state(document, "no.such.command", &enabled, &is_on) ==
             COMP_ERR_NOT_FOUND);
  const std::string main_error = comp_engine_last_error(engine);
  COMP_CHECK(!main_error.empty());

  /* 工作线程只做成功调用：它的错误槽必须为空，主线程的错误原样保留。 */
  std::string worker_error = "unset";
  std::thread worker([&]() {
    comp_document_desc inner{};
    inner.width = 2;
    inner.height = 2;
    comp_document inner_document = nullptr;
    if (comp_document_create(engine, &inner, &inner_document) == COMP_OK) {
      comp_document_destroy(inner_document);
    }
    worker_error = comp_engine_last_error(engine);
  });
  worker.join();
  COMP_CHECK_MSG(worker_error.empty(), "worker thread owns a separate error slot");
  COMP_CHECK(std::string(comp_engine_last_error(engine)) == main_error);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * 图层变换 round-trip（comp_layer_get_transform）。
 *
 * 入口的意义是「壳能做相对步进」：缩放要以当前中心为锚、旋转要在既有角度上累加，
 * 两端都必须先读到现值。此前 ABI 只有 set_transform 没有对应 getter，壳侧只能
 * 凭记忆维护一份 transform——那必然与文档漂移（撤销、合并、.comp 读回都会改它）。
 *
 * 断言三件事：回读等于写入值；回读值可直接再写入（往返保真）；非法 id 与空出参
 * 被拒，而不是写坏调用方内存。
 */
COMP_TEST(abi_layer_transform_get_is_the_write_mirror) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK_MSG(engine != nullptr, "engine");
  if (engine == nullptr) return;

  comp_document_desc doc_desc{};
  doc_desc.width = 16;
  doc_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &document) == COMP_OK);

  comp_layer_desc layer_desc{};
  layer_desc.name = "transform-probe";
  const comp_rect placed{3, 5, 8, 6};
  layer_desc.transform = placed;
  layer_desc.rotation = 12.0f;
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &layer) == COMP_OK);
  COMP_CHECK(layer != 0);

  /* 1. 写入的值必须原样读回（add 时给的 transform / rotation）。 */
  comp_rect read_rect{0, 0, 0, 0};
  float read_rotation = 0.0f;
  COMP_CHECK(comp_layer_get_transform(document, layer, &read_rect, &read_rotation) == COMP_OK);
  COMP_CHECK(read_rect.x == placed.x && read_rect.y == placed.y && read_rect.w == placed.w &&
             read_rect.h == placed.h);
  COMP_CHECK(read_rotation > 11.9f && read_rotation < 12.1f);

  /* 2. 回读值可直接再写回（不发一次 ABI 也能证明结构无损）。 */
  const comp_rect moved{40, 12, 20, 10};
  const float turned = 200.0f;
  COMP_CHECK(comp_layer_set_transform(document, layer, moved, turned) == COMP_OK);
  COMP_CHECK(comp_layer_get_transform(document, layer, &read_rect, &read_rotation) == COMP_OK);
  COMP_CHECK(read_rect.x == moved.x && read_rect.y == moved.y && read_rect.w == moved.w &&
             read_rect.h == moved.h);
  COMP_CHECK(read_rotation > 199.9f && read_rotation < 200.1f);

  /* 3. 空出参必须先拒，不能写坏调用方内存。 */
  COMP_CHECK(comp_layer_get_transform(document, layer, nullptr, &read_rotation) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_transform(document, layer, &read_rect, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_transform(document, 0xdeadbeef, &read_rect, &read_rotation) ==
             COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * comp_layer_get_mask（S22，ABI 尾部追加 113 -> 114）的读侧契约。
 *
 * 为什么值得单独钉：蒙版是**已参与像素求值**的能力（filter_graph.cpp 的
 * coverage.add(&layer->mask) 真的把蒙版合成进画面），壳却长期没有回读口，
 * 于是图层面板既不知道这一层受不受蒙版约束、也无法把既有蒙版变成可见选区。
 * 这里只钉「存在性 + 尺寸」两个问题，不钉像素——像素的读出口是
 * comp_selection_load_layer(COMP_SELECTION_LOAD_MASK)，两处不得漂移。
 */
COMP_TEST(abi_layer_get_mask_reports_presence_and_size) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK_MSG(engine != nullptr, "engine");
  if (engine == nullptr) return;

  comp_document_desc doc_desc{};
  doc_desc.width = 8;
  doc_desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &document) == COMP_OK);

  comp_layer_desc layer_desc{};
  layer_desc.name = "mask-probe";
  layer_desc.transform = comp_rect{0, 0, 8, 8};
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &layer) == COMP_OK);
  COMP_CHECK(layer != 0);

  /* 1. 新建图层没有蒙版：has=0 且尺寸必须归零，不能让调用方看到上一层的残值。 */
  int32_t has = -1;
  int32_t width = -1;
  int32_t height = -1;
  COMP_CHECK(comp_layer_get_mask(document, layer, &has, &width, &height) == COMP_OK);
  COMP_CHECK(has == 0);
  COMP_CHECK(width == 0 && height == 0);

  /* 2. 写入一张 5x3 的实心蒙版：has=1 且尺寸如实回读。 */
  std::vector<uint8_t> mask_bytes(5 * 3, 128u);
  comp_mask mask{};
  mask.width = 5;
  mask.height = 3;
  mask.stride = 5;
  mask.pixels = mask_bytes.data();
  COMP_CHECK(comp_layer_set_mask(document, layer, &mask) == COMP_OK);
  COMP_CHECK(comp_layer_get_mask(document, layer, &has, &width, &height) == COMP_OK);
  COMP_CHECK(has == 1);
  COMP_CHECK(width == 5 && height == 3);

  /* 3. set_mask(nullptr) = 移除：回到 has=0 + 尺寸归零（与域层空 MaskBuffer 同口径）。 */
  COMP_CHECK(comp_layer_set_mask(document, layer, nullptr) == COMP_OK);
  COMP_CHECK(comp_layer_get_mask(document, layer, &has, &width, &height) == COMP_OK);
  COMP_CHECK(has == 0);
  COMP_CHECK(width == 0 && height == 0);

  /* 4. 出参必填：缺一个就必须先拒，不能写坏调用方栈。 */
  COMP_CHECK(comp_layer_get_mask(document, layer, nullptr, &width, &height) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_mask(document, layer, &has, nullptr, &height) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_mask(document, layer, &has, &width, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_mask(document, 0xdeadbeef, &has, &width, &height) ==
             COMP_ERR_NOT_FOUND);

  /*
   * 5. 只读：查询蒙版信息不得把文档从干净改成脏（面板轮询会反复调它）。
   *    做法是先 set_mask（如实置脏）→ 存盘（mark_dirty(false)）→ 反复查询 →
   *    脏标记必须仍是干净的。只查「一直脏所以没变坏」是空断言，骗得过自己。
   */
  {
    const std::string path = (std::filesystem::temp_directory_path() /
                              "compositor_mask_query_clean.comp")
                                 .string();
    COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
    int32_t dirty = 0;
    COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
    COMP_CHECK_MSG(dirty == 0, "saving must clear the dirty flag");
    for (int32_t i = 0; i < 4; ++i) {
      COMP_CHECK(comp_layer_get_mask(document, layer, &has, &width, &height) == COMP_OK);
    }
    COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
    COMP_CHECK_MSG(dirty == 0, "read-only mask query must not dirty the document");
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * comp_layer_get_pixels（P0-4，ABI 尾部追加 145 -> 146）的读侧契约。
 *
 * 为什么需要它：剪贴板复制/剪切、删除选区内像素都要先**看到**图层当前像素，
 * 再做「读—改—写」。`comp_render_layer_thumbnail` 看起来能读，但那条路输出的是
 * 画布坐标下的单层合成（transform/蒙版/效果/透明度全部烘进去），拿它回写等于
 * 把这些东西烤死进像素。本入口读的是图层**原始资产**，与之严格分工。
 *
 * 这里钉四件事：逐字节往返、步长/出参校验、空图层与组不是错误、只读不置脏；
 * 16 位与 HDR 的降位口径由 deep_to_rgba8 / hdr_to_rgba8 的既有测试覆盖，
 * 本处只钉「out_format 回报原始格式」——宿主靠它决定能不能无损写回。
 */
COMP_TEST(abi_layer_get_pixels_reads_raw_layer_asset) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK_MSG(engine != nullptr, "engine");
  if (engine == nullptr) return;

  comp_document_desc doc_desc{};
  doc_desc.width = 8;
  doc_desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &document) == COMP_OK);

  comp_layer_desc layer_desc{};
  layer_desc.name = "pixels-probe";
  layer_desc.transform = comp_rect{1, 2, 5, 3};
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &layer) == COMP_OK);
  COMP_CHECK(layer != 0);

  /* 预乘、半透明、各像素不同的 5x3 资产：纯色会让「行序/步长读错」看起来也对。 */
  std::vector<uint8_t> source(static_cast<size_t>(5) * 3u * 4u);
  for (size_t i = 0; i < source.size(); i += 4) {
    const uint8_t alpha = static_cast<uint8_t>(40u + (i / 4u) * 9u);
    source[i + 3] = alpha;
    source[i + 0] = static_cast<uint8_t>(alpha / 2u);
    source[i + 1] = static_cast<uint8_t>(alpha / 3u);
    source[i + 2] = static_cast<uint8_t>(alpha / 4u);
  }
  comp_image image{};
  image.width = 5;
  image.height = 3;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = 5u * 4u;
  image.pixels = source.data();
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image) == COMP_OK);

  /* 1. 逐字节回读：尺寸是**图层像素矩形**（5x3），不是画布（8x8）。 */
  std::vector<uint8_t> read_back(source.size(), 0xEEu);
  int32_t width = -1;
  int32_t height = -1;
  comp_pixel_format format = COMP_PIXEL_RGBA16F;
  COMP_CHECK(comp_layer_get_pixels(document, layer, read_back.data(), 5u * 4u, &width, &height,
                                   &format) == COMP_OK);
  COMP_CHECK(width == 5 && height == 3);
  COMP_CHECK(format == COMP_PIXEL_RGBA8);
  COMP_CHECK_MSG(read_back == source, "get_pixels must return the layer asset byte-identically");

  /* 2. 读—改—写闭环：把读到的缓冲原样写回，再读一次必须仍逐字节相等。
         （宿主剪贴板剪切正是这条路径；读侧多一个字节、写侧就会偏一行。） */
  {
    comp_image same{};
    same.width = width;
    same.height = height;
    same.format = COMP_PIXEL_RGBA8;
    same.stride = 5u * 4u;
    same.pixels = read_back.data();
    COMP_CHECK(comp_layer_set_pixels(document, layer, &same) == COMP_OK);
    std::vector<uint8_t> again(source.size(), 0);
    COMP_CHECK(comp_layer_get_pixels(document, layer, again.data(), 5u * 4u, &width, &height,
                                     &format) == COMP_OK);
    COMP_CHECK(again == source);
  }

  /* 3. 查询模式：pixels == nullptr（或 stride == 0）只回尺寸/格式，不写缓冲。
        宿主必须先问尺寸再分配——按画布尺寸分配、图层更大时会越界写。 */
  width = -1;
  height = -1;
  format = COMP_PIXEL_RGBA16F;
  COMP_CHECK(comp_layer_get_pixels(document, layer, nullptr, 0, &width, &height, &format) ==
             COMP_OK);
  COMP_CHECK(width == 5 && height == 3);
  COMP_CHECK(format == COMP_PIXEL_RGBA8);

  /* 4. 步长不足整行必须拒绝（引擎不替调用方猜 stride，与 flatten 同一把尺子）。 */
  COMP_CHECK(comp_layer_get_pixels(document, layer, read_back.data(), 5u * 4u - 1u, &width,
                                   &height, &format) == COMP_ERR_INVALID_ARGUMENT);
  /* 出参必填：缺一个都先拒，不能写坏调用方栈。 */
  COMP_CHECK(comp_layer_get_pixels(document, layer, read_back.data(), 5u * 4u, nullptr, &height,
                                   &format) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_pixels(document, layer, read_back.data(), 5u * 4u, &width, nullptr,
                                   &format) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_pixels(document, layer, read_back.data(), 5u * 4u, &width, &height,
                                   nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_pixels(document, 0xdeadbeef, read_back.data(), 5u * 4u, &width,
                                   &height, &format) == COMP_ERR_NOT_FOUND);

  /* 5. 新建像素层铺满画布（全透明）：报画布尺寸，不是 0；组没有像素资产：报 0x0。
        两者都是合法状态，不是错误。 */
  comp_layer_desc fresh_desc{};
  fresh_desc.name = "fresh";
  fresh_desc.transform = comp_rect{0, 0, 8, 8};
  comp_layer_id fresh_layer = 0;
  COMP_CHECK(comp_layer_add(document, &fresh_desc, &fresh_layer) == COMP_OK);
  width = 7;
  height = 7;
  COMP_CHECK(comp_layer_get_pixels(document, fresh_layer, nullptr, 0, &width, &height, &format) ==
             COMP_OK);
  COMP_CHECK(width == 8 && height == 8);
  COMP_CHECK(format == COMP_PIXEL_RGBA8);

  comp_layer_desc group_desc{};
  group_desc.name = "group";
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);
  width = 7;
  height = 7;
  COMP_CHECK(comp_layer_get_pixels(document, group, nullptr, 0, &width, &height, &format) ==
             COMP_OK);
  COMP_CHECK(width == 0 && height == 0);

  /*
   * 5. 只读：复制/剪切会反复调用读取，不能把文档从干净改成脏。
   *    先写像素（如实置脏）→ 存盘清脏 → 反复读 → 脏标记必须仍是干净。
   */
  {
    const std::string path =
        (std::filesystem::temp_directory_path() / "compositor_get_pixels_clean.comp").string();
    COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
    int32_t dirty = 1;
    COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
    COMP_CHECK_MSG(dirty == 0, "saving must clear the dirty flag");
    for (int32_t i = 0; i < 4; ++i) {
      COMP_CHECK(comp_layer_get_pixels(document, layer, read_back.data(), 5u * 4u, &width, &height,
                                       &format) == COMP_OK);
    }
    COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
    COMP_CHECK_MSG(dirty == 0, "read-only pixel query must not dirty the document");
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  /* 6. 16 位资产：默认输出 8 位工作副本，但 out_format 必须回报 RGBA16——
        宿主据此拒绝把 8 位副本写回 16 位图层（否则静默降位）。 */
  {
    comp_document_desc wide_desc{};
    wide_desc.width = 4;
    wide_desc.height = 4;
    comp_document wide_doc = nullptr;
    COMP_CHECK(comp_document_create(engine, &wide_desc, &wide_doc) == COMP_OK);
    const comp_layer_id wide_layer = background_layer(wide_doc);
    std::vector<uint16_t> wide(4u * 4u * 4u, 0);
    for (size_t i = 0; i < wide.size(); i += 4) {
      wide[i + 3] = 0x8000;
      wide[i + 0] = 0x4000;
    }
    comp_image wide_image{};
    wide_image.width = 4;
    wide_image.height = 4;
    wide_image.format = COMP_PIXEL_RGBA16;
    wide_image.stride = 4u * 8u;
    wide_image.pixels = wide.data();
    COMP_CHECK(comp_layer_set_pixels(wide_doc, wide_layer, &wide_image) == COMP_OK);

    std::vector<uint8_t> wide_read(4u * 4u * 4u, 0);
    int32_t wide_width = 0;
    int32_t wide_height = 0;
    comp_pixel_format wide_format = COMP_PIXEL_RGBA8;
    COMP_CHECK(comp_layer_get_pixels(wide_doc, wide_layer, wide_read.data(), 4u * 4u, &wide_width,
                                     &wide_height, &wide_format) == COMP_OK);
    COMP_CHECK(wide_width == 4 && wide_height == 4);
    COMP_CHECK_MSG(wide_format == COMP_PIXEL_RGBA16,
                   "out_format must report the stored format, not the working copy's");
    COMP_CHECK_MSG(wide_read[3] >= 120 && wide_read[3] <= 136,
                   "16-bit working copy is the dithered 8-bit view (alpha 0x8000 ~= 128)");
    comp_document_destroy(wide_doc);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * comp_layer_get_group_info（S22，ABI 尾部追加 113 -> 114 -> 115）。
 *
 * 钉三件事：根级图层的树关系全零；显式建组后 is_group/parent 如实回读；
 * 剪贴来源写什么回什么。缺任何一件，图层面板就只能靠猜画「是不是组」「是否剪贴」
 * ——`.comp` 读回带组/带剪贴的文档时会静默失真，且没有任何测试能发现。
 */
COMP_TEST(abi_layer_get_group_info_reports_tree_shape) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK_MSG(engine != nullptr, "engine");
  if (engine == nullptr) return;

  comp_document_desc doc_desc{};
  doc_desc.width = 8;
  doc_desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &document) == COMP_OK);

  /* 1. 背景层是根级普通图层：三项必须全零。 */
  comp_layer_id background = 0;
  size_t count = 0;
  COMP_CHECK(comp_layer_list(document, &background, 1u, &count) == COMP_OK);
  COMP_CHECK(count == 1u && background != 0);

  int32_t is_group = -1;
  comp_layer_id parent = 0xdeadbeef;
  comp_layer_id clipping = 0xdeadbeef;
  COMP_CHECK(comp_layer_get_group_info(document, background, &is_group, &parent,
                                       &clipping) == COMP_OK);
  COMP_CHECK(is_group == 0 && parent == 0 && clipping == 0);

  /* 2. 建一个组 + 组内一层：组的 is_group=1、子的 parent 指向组。 */
  comp_layer_desc group_desc{};
  group_desc.name = "group";
  group_desc.is_group = 1;
  group_desc.transform = comp_rect{0, 0, 8, 8};
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);
  COMP_CHECK(group != 0);

  comp_layer_desc child_desc{};
  child_desc.name = "child";
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id child = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &child) == COMP_OK);
  COMP_CHECK(child != 0);

  COMP_CHECK(comp_layer_get_group_info(document, group, &is_group, &parent,
                                       &clipping) == COMP_OK);
  COMP_CHECK(is_group == 1);
  COMP_CHECK(parent == 0); /* 组自身是根级 */

  COMP_CHECK(comp_layer_get_group_info(document, child, &is_group, &parent,
                                       &clipping) == COMP_OK);
  COMP_CHECK(is_group == 0);
  COMP_CHECK(parent == group);

  /* 3. 剪贴来源：写什么回什么，取消后回 0。 */
  COMP_CHECK(comp_layer_set_clipping_source(document, child, background) == COMP_OK);
  COMP_CHECK(comp_layer_get_group_info(document, child, &is_group, &parent,
                                       &clipping) == COMP_OK);
  COMP_CHECK(clipping == background);
  COMP_CHECK(comp_layer_set_clipping_source(document, child, 0) == COMP_OK);
  COMP_CHECK(comp_layer_get_group_info(document, child, &is_group, &parent,
                                       &clipping) == COMP_OK);
  COMP_CHECK(clipping == 0);

  /* 4. 出参必填 + 未知 id 必须被拒（不能写坏调用方栈）。 */
  COMP_CHECK(comp_layer_get_group_info(document, child, nullptr, &parent, &clipping) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_group_info(document, child, &is_group, nullptr, &clipping) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_group_info(document, child, &is_group, &parent, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_group_info(document, 0xdeadbeef, &is_group, &parent,
                                       &clipping) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * comp_layer_get_flip（S24，ABI 尾部追加 115 -> 116）+ set_flip 的镜像契约。
 *
 * 为什么必须补读口：引擎的 Layer::flip_x / flip_y 一直参与像素求值
 * （resample.cpp:328 把符号折进采样坐标），但 ABI 只有 setter。壳读不到现值就只能
 * 「盲目取反」——撤销、合并、.comp 读回之后本地记账即失真，取反会翻错方向。
 *
 * 本用例钉四件事，缺一条这个读口就等于没接：
 *   1. 默认不翻转（0/0），且翻转真的改变合成像素（不是只改标志位）；
 *   2. 写入规范化：-1 / 2 一律归 1，回读只会是 0 或 1；
 *   3. 同值写入不产生撤销步，真切换恰好一步且可撤销回原态；
 *   4. 出参必填、未知 id 被拒、纯查询不置脏（面板会反复轮询）。
 */
COMP_TEST(abi_layer_flip_round_trip_and_undo) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK_MSG(engine != nullptr, "engine");
  if (engine == nullptr) return;

  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  comp_layer_id background = 0;
  size_t layer_count = 0;
  COMP_CHECK(comp_layer_list(document, &background, 1u, &layer_count) == COMP_OK);
  COMP_CHECK(layer_count == 1u && background != 0);

  /* 左半红、右半透明：水平镜像后红块必须换到右半，像素断言才有意义。 */
  std::vector<uint8_t> half_pixels(8u * 8u * 4u, 0u);
  for (int32_t y = 0; y < 8; ++y) {
    for (int32_t x = 0; x < 4; ++x) {
      const size_t index = (static_cast<size_t>(y) * 8u + static_cast<size_t>(x)) * 4u;
      half_pixels[index + 0] = 255;
      half_pixels[index + 3] = 255;
    }
  }
  comp_image half_image{};
  half_image.width = 8;
  half_image.height = 8;
  half_image.format = COMP_PIXEL_RGBA8;
  half_image.stride = 8u * 4u;
  half_image.pixels = half_pixels.data();
  COMP_CHECK(comp_layer_set_pixels(document, background, &half_image) == COMP_OK);

  /* 1. 默认不翻转。 */
  int32_t flip_x = -1;
  int32_t flip_y = -1;
  COMP_CHECK(comp_layer_get_flip(document, background, &flip_x, &flip_y) == COMP_OK);
  COMP_CHECK(flip_x == 0 && flip_y == 0);

  std::vector<uint8_t> canvas = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(canvas, 8, 1, 1, 255, 0, 0, 255, 3), "untouched: red on the left");
  COMP_CHECK_MSG(matches(canvas, 8, 6, 1, 0, 0, 0, 0, 3), "untouched: transparent on the right");

  /* 2. flip_x 必须真的改像素（以下断言在 set_flip 变成纯元数据时会失败）。 */
  COMP_CHECK(comp_layer_set_flip(document, background, 1, 0) == COMP_OK);
  COMP_CHECK(comp_layer_get_flip(document, background, &flip_x, &flip_y) == COMP_OK);
  COMP_CHECK(flip_x == 1 && flip_y == 0);
  canvas = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(canvas, 8, 6, 1, 255, 0, 0, 255, 3), "flip_x: red moved to the right");
  COMP_CHECK_MSG(matches(canvas, 8, 1, 1, 0, 0, 0, 0, 3), "flip_x: left is now transparent");

  /* 3. 规范化：非 0 即翻转，回读只会是 0/1。 */
  COMP_CHECK(comp_layer_set_flip(document, background, -1, 2) == COMP_OK);
  COMP_CHECK(comp_layer_get_flip(document, background, &flip_x, &flip_y) == COMP_OK);
  COMP_CHECK(flip_x == 1 && flip_y == 1);
  /* 垂直翻转对「左右分块」的图没有可见影响，只钉标志位即可（上面已证 flip_x 落像素）。 */
  canvas = flatten(document, 8, 8);
  COMP_CHECK(matches(canvas, 8, 6, 1, 255, 0, 0, 255, 3));

  /* 4. 撤销语义：同值不产生步；真切换恰好一步，且能撤销回原态。 */
  COMP_CHECK(comp_layer_set_flip(document, background, 1, 1) == COMP_OK); /* 已是 1/1 */
  size_t undo_depth = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, nullptr) == COMP_OK);
  const size_t baseline_depth = undo_depth;
  COMP_CHECK_MSG(undo_depth > 0u, "前序 set_pixels / set_flip 已留下撤销步");

  COMP_CHECK(comp_layer_set_flip(document, background, 1, 1) == COMP_OK);
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_depth == baseline_depth, "同值 set_flip 不得压入撤销步（否则撤销栈被空步塞满）");

  COMP_CHECK(comp_layer_set_flip(document, background, 0, 1) == COMP_OK);
  COMP_CHECK(comp_document_history_stats(document, &undo_depth, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_depth == baseline_depth + 1u, "一次切换恰好一步撤销");

  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_layer_get_flip(document, background, &flip_x, &flip_y) == COMP_OK);
  COMP_CHECK(flip_x == 1 && flip_y == 1);

  /* 5. 出参必填 + 未知 id 必须被拒，不能写坏调用方栈。 */
  COMP_CHECK(comp_layer_get_flip(document, background, nullptr, &flip_y) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_flip(document, background, &flip_x, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_flip(document, 0xdeadbeef, &flip_x, &flip_y) == COMP_ERR_NOT_FOUND);

  /* 6. 纯只读：查询不得把干净文档改脏（面板每帧都在轮询这一对）。 */
  {
    const std::string path =
        (std::filesystem::temp_directory_path() / "compositor_flip_query_clean.comp").string();
    COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
    int32_t dirty = 0;
    COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
    COMP_CHECK_MSG(dirty == 0, "保存后必须是干净状态");
    for (int32_t i = 0; i < 4; ++i) {
      COMP_CHECK(comp_layer_get_flip(document, background, &flip_x, &flip_y) == COMP_OK);
    }
    COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
    COMP_CHECK_MSG(dirty == 0, "只读查询不得置脏");
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * comp_layer_effect_count / comp_layer_effect_at（S24，ABI 尾部追加 117 -> 119）。
 *
 * 这一对是「写得出、读不回」的最后一块：comp_layer_set_effect 早在 ABI 里，效果
 * 也早已参与像素求值（effect_eval.cpp 的 6 个 kind），但壳无法枚举——画不出
 * 「这层有几个效果」，菜单也无法标出已启用项；.comp 读回后本地记账即失真。
 *
 * 钉五件事：空层 count=0 且 at(0) 越界拒绝；写入顺序即回读顺序；settings 串
 * **原样**回传（内核不解析不改写，含未知键）；两段式长度契约（capacity-1 + NUL、
 * out_size 报全长的字节数）；通过 ABI 写的效果真的进入合成像素 + 纯查询不置脏。
 */
COMP_TEST(abi_layer_effect_enumeration_round_trip) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK_MSG(engine != nullptr, "engine");
  if (engine == nullptr) return;

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  comp_layer_id background = 0;
  size_t layer_count = 0;
  COMP_CHECK(comp_layer_list(document, &background, 1u, &layer_count) == COMP_OK);
  COMP_CHECK(layer_count == 1u && background != 0);

  const SolidImage green(4, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &green.image) == COMP_OK);

  /* 1. 空层：count=0，at(0) 必须按越界拒绝（不能读到上一层/越界的残留）。 */
  size_t effect_count = 123u;
  COMP_CHECK(comp_layer_effect_count(document, background, &effect_count) == COMP_OK);
  COMP_CHECK(effect_count == 0u);

  uint32_t kind = 4242u;
  size_t needed = 0;
  COMP_CHECK(comp_layer_effect_at(document, background, 0, &kind, nullptr, 0, &needed) ==
             COMP_ERR_INVALID_ARGUMENT);

  /*
   * 2. 通过 ABI 挂一个颜色叠加并把绿色整层染红：这是「效果真的参与像素求值」的
   *    端到端证据，也顺带钉住 settings 的键名（red/green/blue，0..1）。
   *    同时验证 count/at 立刻能枚举出它。
   */
  COMP_CHECK(comp_layer_set_effect(document, background, 2u,
                                   "{\"red\":1,\"green\":0,\"blue\":0}") == COMP_OK);
  COMP_CHECK(comp_layer_effect_count(document, background, &effect_count) == COMP_OK);
  COMP_CHECK(effect_count == 1u);

  const char kOverlaySettings[] = "{\"red\":1,\"green\":0,\"blue\":0}";
  COMP_CHECK(comp_layer_effect_count(document, background, &effect_count) == COMP_OK);
  kind = 4242u;
  needed = 0;
  COMP_CHECK(comp_layer_effect_at(document, background, 0, &kind, nullptr, 0, &needed) == COMP_OK);
  COMP_CHECK(kind == 2u);
  COMP_CHECK_MSG(needed == sizeof(kOverlaySettings) - 1u, "out_size 报字节长度（不含 NUL）");

  std::vector<char> text(needed + 1u, '\0');
  size_t written = 0;
  COMP_CHECK(comp_layer_effect_at(document, background, 0, &kind, text.data(), text.size(),
                                  &written) == COMP_OK);
  COMP_CHECK(written == needed);
  COMP_CHECK_MSG(std::strcmp(text.data(), kOverlaySettings) == 0,
                 "settings 必须原样回传（内核不解析、不重排键序）");

  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 0, 0, 255, 3), "颜色叠加经 ABI 写入后真的改像素");

  /* 3. 第二个效果 + 两段式容量契约（capacity=8 写 7 字节 + NUL）。 */
  const char kStrokeSettings[] = "{\"size\":9,\"inside\":true,\"unknown\":[1,2]}";
  COMP_CHECK(comp_layer_set_effect(document, background, 0u, kStrokeSettings) == COMP_OK);
  COMP_CHECK(comp_layer_effect_count(document, background, &effect_count) == COMP_OK);
  COMP_CHECK(effect_count == 2u);

  kind = 4242u;
  needed = 0;
  COMP_CHECK(comp_layer_effect_at(document, background, 1, &kind, nullptr, 0, &needed) == COMP_OK);
  COMP_CHECK(kind == 0u);
  COMP_CHECK(needed == sizeof(kStrokeSettings) - 1u);

  char truncated[8];
  std::memset(truncated, 0x7f, sizeof(truncated));
  size_t truncated_size = 0;
  COMP_CHECK(comp_layer_effect_at(document, background, 1, &kind, truncated, sizeof(truncated),
                                  &truncated_size) == COMP_OK);
  COMP_CHECK_MSG(truncated_size == needed, "容量不足时 out_size 仍报完整长度（先量后取）");
  COMP_CHECK(truncated[sizeof(truncated) - 1] == '\0');
  COMP_CHECK(std::strncmp(truncated, kStrokeSettings, sizeof(truncated) - 1) == 0);

  /* 顺序 = 写入序，不是按 kind 排序。 */
  kind = 4242u;
  COMP_CHECK(comp_layer_effect_at(document, background, 0, &kind, nullptr, 0, &needed) == COMP_OK);
  COMP_CHECK(kind == 2u);

  /* 4. 同 kind 再写 = 改设置而不是追加；移除只删该 kind，其余顺序不变。 */
  COMP_CHECK(comp_layer_set_effect(document, background, 2u, "{\"red\":0,\"green\":0,\"blue\":1}") ==
             COMP_OK);
  COMP_CHECK(comp_layer_effect_count(document, background, &effect_count) == COMP_OK);
  COMP_CHECK(effect_count == 2u);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255, 3), "改设置后像素跟着变（蓝）");

  COMP_CHECK(comp_layer_remove_effect(document, background, 2u) == COMP_OK);
  COMP_CHECK(comp_layer_effect_count(document, background, &effect_count) == COMP_OK);
  COMP_CHECK(effect_count == 1u);
  kind = 4242u;
  COMP_CHECK(comp_layer_effect_at(document, background, 0, &kind, nullptr, 0, &needed) == COMP_OK);
  COMP_CHECK(kind == 0u);

  /* 5. 越界 / 空出参 / 未知图层：一律先拒，且不写坏调用方内存。 */
  kind = 4242u;
  needed = 0;
  COMP_CHECK(comp_layer_effect_at(document, background, 1, &kind, nullptr, 0, &needed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_effect_count(document, background, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  /*
   * 三个出参都是可选的（与 comp_layer_adjustment_at 同口径）：全给 nullptr 等于
   * 「只想知道这一项存在」，必须 OK 而不是拒绝；拒绝会让两段式查询的探针写法失效。
   */
  COMP_CHECK(comp_layer_effect_at(document, background, 0, nullptr, nullptr, 0, nullptr) ==
             COMP_OK);
  COMP_CHECK(comp_layer_effect_count(document, 0xdeadbeef, &effect_count) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_effect_at(document, 0xdeadbeef, 0, &kind, nullptr, 0, &needed) ==
             COMP_ERR_NOT_FOUND);

  /* 6. 纯只读：枚举不得把干净文档改脏。 */
  {
    const std::string path =
        (std::filesystem::temp_directory_path() / "compositor_effect_query_clean.comp").string();
    COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
    int32_t dirty = 0;
    COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
    COMP_CHECK_MSG(dirty == 0, "保存后必须是干净状态");
    for (int32_t i = 0; i < 4; ++i) {
      COMP_CHECK(comp_layer_effect_count(document, background, &effect_count) == COMP_OK);
      COMP_CHECK(comp_layer_effect_at(document, background, 0, &kind, nullptr, 0, &needed) ==
                 COMP_OK);
    }
    COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
    COMP_CHECK_MSG(dirty == 0, "只读枚举不得置脏");
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
