// AI生成
/*
 * test_layer_links.cpp — P1-2 第三批：图层链接的端到端测试。
 *
 * 判据同样不是「API 返回 COMP_OK」，而是可观测事实：
 *   1. 链接是**关系元数据**：读写、撤销/重做、`.comp` 往返都保真，且不改像素；
 *   2. 几何联动真的发生：同组的位移/等比缩放/旋转精确作用到其余成员，
 *      不同组的层一格不动；
 *   3. 联动是**一步撤销**，且锁闸能整次拒绝（不留半截写）；
 *   4. 表示不了的情况（非等比缩放 × 非 90° 相对角 = 剪切）明确拒绝；
 *   5. 复制图层不带链接（副本能单独移动）。
 *
 * 只使用 comp_*.h 里的 C 符号，与三端外壳的集成路径一致。
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compositor/comp_canvas.h"
#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

comp_engine_desc link_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-layer-link-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

comp_layer_id background_layer(comp_document document) {
  comp_layer_id id = 0;
  size_t count = 0;
  if (comp_layer_list(document, &id, 1u, &count) != COMP_OK || count < 1u) return 0;
  return id;
}

comp_layer_id add_layer(comp_document document, const char* name, comp_rect rect) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = rect;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  return id;
}

comp_layer_id add_group(comp_document document, const char* name) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.is_group = 1;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  return id;
}

bool rect_is(const comp_rect& r, int32_t x, int32_t y, int32_t w, int32_t h) {
  return r.x == x && r.y == y && r.w == w && r.h == h;
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string work_path(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/layer_links", ec);
  return std::string("build/layer_links/") + name;
}

}  // namespace

/* ---------------------------------------------- 链接组的读写 / 校验 / 撤销 */

COMP_TEST(layer_link_set_get_validation_and_undo) {
  comp_engine_desc desc = link_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 64;
  document_desc.height = 64;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = background_layer(document);
  const comp_layer_id b = add_layer(document, "b", comp_rect{0, 0, 16, 16});
  const comp_layer_id c = add_layer(document, "c", comp_rect{32, 32, 16, 16});
  const comp_layer_id group = add_group(document, "group");
  COMP_CHECK(a != 0 && b != 0 && c != 0 && group != 0);

  uint32_t group_id = 99;
  COMP_CHECK(comp_layer_get_link(document, a, &group_id) == COMP_OK);
  COMP_CHECK_MSG(group_id == 0u, "new layers are unlinked");

  /* 参数闸：少于两个 id / 空指针 / 重复 id 一律拒绝，且不产生链接。 */
  const comp_layer_id only[1] = {a};
  COMP_CHECK(comp_layer_link(document, only, 1u, nullptr, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_link(document, nullptr, 2u, nullptr, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  const comp_layer_id dup[2] = {a, a};
  COMP_CHECK(comp_layer_link(document, dup, 2u, nullptr, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  const comp_layer_id missing[2] = {a, 999999};
  COMP_CHECK(comp_layer_link(document, missing, 2u, nullptr, nullptr) == COMP_ERR_NOT_FOUND);
  /* 组图层明确不支持（组的几何是子树合成的产物，没有可联动的矩形）。 */
  const comp_layer_id with_group[2] = {a, group};
  COMP_CHECK(comp_layer_link(document, with_group, 2u, nullptr, nullptr) ==
             COMP_ERR_UNSUPPORTED);
  COMP_CHECK(comp_layer_get_link(document, a, &group_id) == COMP_OK);
  COMP_CHECK(group_id == 0u);

  /* 正常链接：a + b 同组，c 不在组。 */
  int32_t changed = -1;
  const comp_layer_id pair[2] = {a, b};
  COMP_CHECK(comp_layer_link(document, pair, 2u, &group_id, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(group_id > 0u, "engine allocates a non-zero group id");
  uint32_t ga = 0;
  uint32_t gb = 0;
  uint32_t gc = 0;
  COMP_CHECK(comp_layer_get_link(document, a, &ga) == COMP_OK);
  COMP_CHECK(comp_layer_get_link(document, b, &gb) == COMP_OK);
  COMP_CHECK(comp_layer_get_link(document, c, &gc) == COMP_OK);
  COMP_CHECK(ga == group_id && gb == group_id);
  COMP_CHECK_MSG(gc == 0u, "unselected layers stay unlinked");

  /* 幂等：同组再链一次是空操作，不落撤销步。 */
  changed = -1;
  COMP_CHECK(comp_layer_link(document, pair, 2u, &group_id, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "re-linking the same set is a no-op");
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  uint32_t after_undo = 99;
  COMP_CHECK(comp_layer_get_link(document, a, &after_undo) == COMP_OK);
  COMP_CHECK_MSG(after_undo == 0u, "one undo removes the link step");
  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(comp_layer_get_link(document, a, &after_undo) == COMP_OK);
  COMP_CHECK(after_undo == group_id);

  /* 解除只作用于被列举的层：只解 a，b 仍留在原组。 */
  changed = -1;
  COMP_CHECK(comp_layer_unlink(document, only, 1u, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(comp_layer_get_link(document, a, &ga) == COMP_OK);
  COMP_CHECK(comp_layer_get_link(document, b, &gb) == COMP_OK);
  COMP_CHECK_MSG(ga == 0u, "the listed layer is unlinked");
  COMP_CHECK_MSG(gb == group_id, "unlisted group members keep their group");

  /* 都不在组时空操作（a 已解，c 从未链接）。 */
  const comp_layer_id plain_pair[2] = {a, c};
  changed = -1;
  COMP_CHECK(comp_layer_unlink(document, plain_pair, 2u, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "unlinking unlinked layers is a no-op");

  /* 再次链接会把两个既有组合并成一个（不把旧组拆半）。 */
  const comp_layer_id d = add_layer(document, "d", comp_rect{48, 0, 16, 16});
  COMP_CHECK(d != 0);
  const comp_layer_id c_d[2] = {c, d};
  uint32_t other_group = 0;
  COMP_CHECK(comp_layer_link(document, c_d, 2u, &other_group, nullptr) == COMP_OK);
  COMP_CHECK(other_group > 0u && other_group != group_id);
  const comp_layer_id b_c[2] = {b, c};
  uint32_t merged = 0;
  COMP_CHECK(comp_layer_link(document, b_c, 2u, &merged, nullptr) == COMP_OK);
  uint32_t gd = 0;
  COMP_CHECK(comp_layer_get_link(document, b, &gb) == COMP_OK);
  COMP_CHECK(comp_layer_get_link(document, c, &gc) == COMP_OK);
  COMP_CHECK(comp_layer_get_link(document, d, &gd) == COMP_OK);
  COMP_CHECK_MSG(gb == gc && gc == gd, "merging two groups joins all their members");
  COMP_CHECK_MSG(merged == gb, "the merged group id is the surviving one");

  /* 只读口的目标校验。 */
  COMP_CHECK(comp_layer_get_link(document, a, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_link(document, 999999, &ga) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_unlink(document, nullptr, 1u, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ---------------------------------------- 联动：位移 + 等比缩放（精确口径） */

COMP_TEST(layer_link_transform_translates_and_scales_mates) {
  comp_engine_desc desc = link_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 512;
  document_desc.height = 512;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = background_layer(document);
  COMP_CHECK(a != 0);
  /* a: 20x20 @ (0,0)，中心 (10,10)；b: 20x20 @ (50,50)，中心 (60,60)。 */
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 20, 20}, 0.0f) == COMP_OK);
  const comp_layer_id b = add_layer(document, "b", comp_rect{50, 50, 20, 20});
  const comp_layer_id c = add_layer(document, "c", comp_rect{200, 200, 20, 20});
  COMP_CHECK(b != 0 && c != 0);
  const comp_layer_id pair[2] = {a, b};
  COMP_CHECK(comp_layer_link(document, pair, 2u, nullptr, nullptr) == COMP_OK);

  /*
   * 目标：20x20 -> 40x40（等比 2x），中心 (10,10) -> (120,60)。
   * b 的中心相对目标中心 (50,50) * 2 = (100,100)，平移到 (220,160)，
   * 新原点 = (220-20, 160-20) = (200,140)，尺寸 40x40。
   */
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{100, 40, 40, 40}, 0.0f) == COMP_OK);

  comp_rect rb{};
  float rot_b = 0.0f;
  COMP_CHECK(comp_layer_get_transform(document, b, &rb, &rot_b) == COMP_OK);
  COMP_CHECK_MSG(rect_is(rb, 200, 140, 40, 40),
                 "a linked layer takes the same translation and scale");
  COMP_CHECK(near(rot_b, 0.0f));

  /* 不同组的 c 一格不动。 */
  comp_rect rc{};
  float rot_c = 0.0f;
  COMP_CHECK(comp_layer_get_transform(document, c, &rc, &rot_c) == COMP_OK);
  COMP_CHECK_MSG(rect_is(rc, 200, 200, 20, 20), "unlinked layers are untouched");

  /* 整次联动是一步撤销：撤销后两层都回到原矩形。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  comp_rect ra{};
  float rot_a = 0.0f;
  COMP_CHECK(comp_layer_get_transform(document, a, &ra, &rot_a) == COMP_OK);
  COMP_CHECK(comp_layer_get_transform(document, b, &rb, &rot_b) == COMP_OK);
  COMP_CHECK_MSG(rect_is(ra, 0, 0, 20, 20), "undo restores the target");
  COMP_CHECK_MSG(rect_is(rb, 50, 50, 20, 20), "undo restores the linked mate in the same step");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* -------------------------------------------------- 联动：旋转（s=1 精确） */

COMP_TEST(layer_link_transform_rotates_mates) {
  comp_engine_desc desc = link_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 512;
  document_desc.height = 512;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = background_layer(document);
  COMP_CHECK(a != 0);
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 20, 20}, 0.0f) == COMP_OK);
  const comp_layer_id b = add_layer(document, "b", comp_rect{50, 50, 20, 20});
  COMP_CHECK(b != 0);
  const comp_layer_id pair[2] = {a, b};
  COMP_CHECK(comp_layer_link(document, pair, 2u, nullptr, nullptr) == COMP_OK);

  /*
   * 只转 30°（目标矩形与中心不变）：b 同步转 30°，并绕目标中心公转——
   * 相对位置 (50,50) 旋转 30° 得 (18.3, 68.3)，新中心 (28.3, 78.3)，
   * 原点 (18, 68)。这正是「整组绕目标层中心旋转」的几何。
   */
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 20, 20}, 30.0f) == COMP_OK);
  comp_rect rb{};
  float rot_b = 0.0f;
  COMP_CHECK(comp_layer_get_transform(document, b, &rb, &rot_b) == COMP_OK);
  COMP_CHECK_MSG(rect_is(rb, 18, 68, 20, 20), "the mate orbits the target's center");
  COMP_CHECK_MSG(near(rot_b, 30.0f), "the mate's rotation follows the target");

  /* 再转到 -15°：联动的是**增量**（-45°），b = 30 + (-45) = -15°。 */
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 20, 20}, -15.0f) == COMP_OK);
  COMP_CHECK(comp_layer_get_transform(document, b, &rb, &rot_b) == COMP_OK);
  COMP_CHECK_MSG(rect_is(rb, 61, 35, 20, 20), "the mate keeps orbiting on later rotations");
  COMP_CHECK_MSG(near(rot_b, -15.0f),
                 "rotations are additive deltas, not absolute copies");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------ 表示不了的情况：非等比缩放 × 斜角 */

COMP_TEST(layer_link_rejects_shear_inducing_scale) {
  comp_engine_desc desc = link_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 512;
  document_desc.height = 512;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = background_layer(document);
  COMP_CHECK(a != 0);
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 20, 20}, 0.0f) == COMP_OK);
  const comp_layer_id b = add_layer(document, "b", comp_rect{50, 50, 20, 20});
  COMP_CHECK(b != 0);
  /* b 先转 45°（不与目标轴向对齐），再链接。 */
  COMP_CHECK(comp_layer_set_transform(document, b, comp_rect{50, 50, 20, 20}, 45.0f) == COMP_OK);
  const comp_layer_id pair[2] = {a, b};
  COMP_CHECK(comp_layer_link(document, pair, 2u, nullptr, nullptr) == COMP_OK);

  /* 非等比缩放（2x × 1）会让 b 出现剪切：明确拒绝，而不是静默近似。 */
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 40, 20}, 0.0f) ==
             COMP_ERR_UNSUPPORTED);

  /* 拒绝 = 什么都没发生（目标层自己也保持原状）。 */
  comp_rect ra{};
  comp_rect rb{};
  float rot_a = 0.0f;
  float rot_b = 0.0f;
  COMP_CHECK(comp_layer_get_transform(document, a, &ra, &rot_a) == COMP_OK);
  COMP_CHECK(comp_layer_get_transform(document, b, &rb, &rot_b) == COMP_OK);
  COMP_CHECK_MSG(rect_is(ra, 0, 0, 20, 20), "a rejected linked transform leaves the target");
  COMP_CHECK_MSG(rect_is(rb, 50, 50, 20, 20), "and leaves the mate");
  COMP_CHECK(near(rot_a, 0.0f) && near(rot_b, 45.0f));

  /* 等比缩放同一场景是允许的（无剪切）。 */
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 30, 30}, 0.0f) == COMP_OK);
  COMP_CHECK(comp_layer_get_transform(document, b, &rb, &rot_b) == COMP_OK);
  COMP_CHECK_MSG(rb.w == 30 && rb.h == 30, "uniform scale still works on a rotated mate");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* --------------------------------- 位置锁：整次拒绝，不做半截提交 */

COMP_TEST(layer_link_transform_is_blocked_by_mate_position_lock) {
  comp_engine_desc desc = link_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 128;
  document_desc.height = 128;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = background_layer(document);
  COMP_CHECK(a != 0);
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 20, 20}, 0.0f) == COMP_OK);
  const comp_layer_id b = add_layer(document, "b", comp_rect{50, 50, 20, 20});
  COMP_CHECK(b != 0);
  const comp_layer_id pair[2] = {a, b};
  COMP_CHECK(comp_layer_link(document, pair, 2u, nullptr, nullptr) == COMP_OK);
  COMP_CHECK(comp_layer_set_lock(document, b, COMP_LAYER_LOCK_POSITION) == COMP_OK);

  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{10, 10, 20, 20}, 0.0f) ==
             COMP_ERR_STATE);
  comp_rect ra{};
  comp_rect rb{};
  float rot = 0.0f;
  COMP_CHECK(comp_layer_get_transform(document, a, &ra, &rot) == COMP_OK);
  COMP_CHECK(comp_layer_get_transform(document, b, &rb, &rot) == COMP_OK);
  COMP_CHECK_MSG(rect_is(ra, 0, 0, 20, 20), "the target is not written when a mate is locked");
  COMP_CHECK_MSG(rect_is(rb, 50, 50, 20, 20), "the locked mate is not written either");

  /* 解锁后同一操作通过。 */
  COMP_CHECK(comp_layer_set_lock(document, b, COMP_LAYER_LOCK_NONE) == COMP_OK);
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{10, 10, 20, 20}, 0.0f) == COMP_OK);
  COMP_CHECK(comp_layer_get_transform(document, b, &rb, &rot) == COMP_OK);
  COMP_CHECK_MSG(rect_is(rb, 60, 60, 20, 20), "after unlocking the link moves the mate again");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------- 复制不带链接 + `.comp` 往返 */

COMP_TEST(layer_link_duplicate_clears_and_comp_round_trip) {
  comp_engine_desc desc = link_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 64;
  document_desc.height = 64;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = background_layer(document);
  COMP_CHECK(a != 0);
  COMP_CHECK(comp_layer_set_transform(document, a, comp_rect{0, 0, 16, 16}, 0.0f) == COMP_OK);
  const comp_layer_id b = add_layer(document, "b", comp_rect{32, 32, 16, 16});
  const comp_layer_id plain = add_layer(document, "plain", comp_rect{48, 0, 16, 16});
  COMP_CHECK(b != 0 && plain != 0);
  const comp_layer_id pair[2] = {a, b};
  uint32_t group_id = 0;
  COMP_CHECK(comp_layer_link(document, pair, 2u, &group_id, nullptr) == COMP_OK);

  /* 复制带链的层：副本不带链接（否则一动就带着原件走）。 */
  comp_layer_id copy = 0;
  COMP_CHECK(comp_layer_duplicate(document, a, &copy) == COMP_OK);
  COMP_CHECK(copy != 0);
  uint32_t copy_group = 99;
  COMP_CHECK(comp_layer_get_link(document, copy, &copy_group) == COMP_OK);
  COMP_CHECK_MSG(copy_group == 0u, "duplicates start unlinked");

  /* 落盘：只有链接成员写 engineLink，且值相同。 */
  const std::string path = work_path("link_round_trip.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  const std::string manifest = read_text(path + "/manifest.json");
  const size_t first = manifest.find("\"engineLink\"");
  COMP_CHECK_MSG(first != std::string::npos, "engineLink is written for linked layers");
  COMP_CHECK_MSG(manifest.find("\"engineLink\"", first + 1u) != std::string::npos,
                 "every linked member persists the key");
  COMP_CHECK_MSG(manifest.find(std::string("\"engineLink\": ") + std::to_string(group_id)) !=
                     std::string::npos,
                 "the persisted group id matches the engine's");

  /* 读回：链接关系还在，且联动仍然生效。 */
  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, path.c_str(), &loaded) == COMP_OK);
  if (loaded != nullptr) {
    comp_layer_id ids[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t listed = 0;
    COMP_CHECK(comp_layer_list(loaded, ids, 8u, &listed) == COMP_OK);
    comp_layer_id restored_a = 0;
    comp_layer_id restored_b = 0;
    uint32_t restored_group = 0;
    for (size_t i = 0; i < listed; ++i) {
      uint32_t g = 0;
      if (comp_layer_get_link(loaded, ids[i], &g) != COMP_OK || g == 0u) continue;
      if (restored_group == 0u) restored_group = g;
      if (restored_a == 0) {
        restored_a = ids[i];
      } else if (restored_b == 0) {
        restored_b = ids[i];
      }
    }
    COMP_CHECK_MSG(restored_group == group_id, "the link group survives a .comp round trip");
    COMP_CHECK_MSG(restored_a != 0 && restored_b != 0, "both linked layers are restored");

    /* 读回后的链接同样参与联动：把 a 平移 (5,5)，b 跟同样的量。 */
    comp_rect ra{};
    comp_rect rb{};
    float rot = 0.0f;
    COMP_CHECK(comp_layer_get_transform(loaded, restored_a, &ra, &rot) == COMP_OK);
    COMP_CHECK(comp_layer_get_transform(loaded, restored_b, &rb, &rot) == COMP_OK);
    const comp_rect target{ra.x + 5, ra.y + 5, ra.w, ra.h};
    COMP_CHECK(comp_layer_set_transform(loaded, restored_a, target, 0.0f) == COMP_OK);
    comp_rect rb_after{};
    COMP_CHECK(comp_layer_get_transform(loaded, restored_b, &rb_after, &rot) == COMP_OK);
    COMP_CHECK_MSG(rb_after.x == rb.x + 5 && rb_after.y == rb.y + 5,
                   "link lashes survive loading: the mate moves with the target");

    /* 未链接的层不带键。 */
    comp_document_destroy(loaded);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
