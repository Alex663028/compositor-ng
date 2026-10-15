// AI生成
/*
 * test_abi.cpp — C ABI 端到端：引擎/文档/图层/合成/撤销/命令/IO。
 *
 * 这一组测试即「壳的视角」：只使用 comp_*.h 中的 C 符号，不接触任何 C++ 内部类型。
 * 三端外壳（Windows / HarmonyOS / macOS）的集成路径与此完全一致。
 */
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
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

  size_t command_count = 0;
  COMP_CHECK(comp_command_list(engine, nullptr, 0, &command_count) == COMP_OK);
  COMP_CHECK(command_count >= 5);

  std::vector<comp_command_desc> commands(command_count);
  size_t written = 0;
  COMP_CHECK(comp_command_list(engine, commands.data(), commands.size(), &written) == COMP_OK);
  COMP_CHECK(written == command_count);

  bool found_undo = false;
  for (const comp_command_desc& command : commands) {
    COMP_CHECK(command.id != nullptr && command.title != nullptr);
    if (std::string(command.id) == "edit.undo") found_undo = true;
  }
  COMP_CHECK(found_undo);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

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
