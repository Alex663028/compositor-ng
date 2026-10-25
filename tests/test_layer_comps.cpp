// AI生成
/*
 * test_layer_comps.cpp — P1-2 收尾 ⑤：图层复合（Layer Comp）的端到端测试。
 *
 * 判据是可观测事实而不是「返回 COMP_OK」：
 *   1. 捕获记的是**当时所有存在图层**的 visible + opacity 快照；
 *   2. 应用把它原样写回，且是**一步撤销**；再次应用是空操作，不落撤销步、
 *      不推修订号；
 *   3. 捕获之后才建的层不在复合里（应用不去动它），应用时已删掉的层跳过；
 *   4. 名字校验严格（空 / 换行 / 非法 UTF-8 / 超长都拒绝），重名允许；
 *   5. id 单调递增、删除后空洞不复用；
 *   6. `.comp` 往返保真（键 engineLayerComps，空表不写键）。
 *
 * 只用 comp_*.h 里的 C 符号，与三端外壳的集成路径一致。
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

comp_engine_desc comp_engine_desc_make() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-layer-comp-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

comp_layer_id bottom_layer(comp_document document) {
  comp_layer_id id = 0;
  size_t count = 0;
  if (comp_layer_list(document, &id, 1u, &count) != COMP_OK || count < 1u) return 0;
  return id;
}

comp_layer_id add_layer(comp_document document, const char* name) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, 8, 8};
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  return id;
}

size_t history_steps(comp_document document) {
  size_t steps = 0;
  if (comp_document_history_step_count(document, &steps) != COMP_OK) return 0xFFFFFFFFu;
  return steps;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string work_path(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/layer_comps", ec);
  return std::string("build/layer_comps/") + name;
}

struct LayerLook {
  bool found = false;
  uint8_t visible = 0;
  float opacity = 0.0f;
};

LayerLook look(comp_document document, comp_layer_id id) {
  LayerLook out;
  int32_t visible = 0;
  if (comp_layer_get_visible(document, id, &visible) != COMP_OK) return out;
  float opacity = -1.0f;
  if (comp_layer_get_opacity(document, id, &opacity) != COMP_OK) return out;
  out.found = true;
  out.visible = visible ? 1u : 0u;
  out.opacity = opacity;
  return out;
}

}  // namespace

/* --------------------------------- 捕获 / 读取 / 校验 / id 分配 */

COMP_TEST(layer_comp_capture_read_and_validation) {
  comp_engine_desc desc = comp_engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 32;
  document_desc.height = 32;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = bottom_layer(document);
  const comp_layer_id b = add_layer(document, "b");
  const comp_layer_id c = add_layer(document, "c");
  COMP_CHECK(a != 0 && b != 0 && c != 0);

  size_t count = 99;
  COMP_CHECK(comp_layer_comp_count(document, &count) == COMP_OK);
  COMP_CHECK_MSG(count == 0u, "a new document has no layer comps");

  /* 参数闸。 */
  COMP_CHECK(comp_layer_comp_count(document, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  comp_layer_comp_id unused_id = 7;
  COMP_CHECK(comp_layer_comp_capture(document, nullptr, &unused_id) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK_MSG(unused_id == 0u, "a rejected capture does not hand out an id");
  COMP_CHECK(comp_layer_comp_capture(document, "", &unused_id) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_comp_capture(document, "line\nbreak", &unused_id) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_comp_capture(document, "\xC3\x28", &unused_id) ==
             COMP_ERR_INVALID_ARGUMENT); /* 非法 UTF-8 */
  {
    /* 超长（> 1024 字节）名字必须拒绝——上界就是 ABI 契约里的那个。 */
    std::string too_long(COMP_LAYER_COMP_NAME_MAX_BYTES + 1u, 'x');
    COMP_CHECK(comp_layer_comp_capture(document, too_long.c_str(), &unused_id) ==
               COMP_ERR_INVALID_ARGUMENT);
    std::string at_limit(COMP_LAYER_COMP_NAME_MAX_BYTES, 'x');
    comp_layer_comp_id limit_id = 0;
    COMP_CHECK(comp_layer_comp_capture(document, at_limit.c_str(), &limit_id) == COMP_OK);
    COMP_CHECK(limit_id != 0u);
    COMP_CHECK(comp_layer_comp_delete(document, limit_id) == COMP_OK);
  }
  COMP_CHECK(comp_layer_comp_count(document, &count) == COMP_OK);
  COMP_CHECK_MSG(count == 0u, "every rejected capture left the comp list alone");

  /* 正常捕获：先记下 c 被隐藏、b 半透明的那一刻。 */
  COMP_CHECK(comp_layer_set_visible(document, c, 0) == COMP_OK);
  COMP_CHECK(comp_layer_set_opacity(document, b, 0.25f) == COMP_OK);
  comp_layer_comp_id first = 0;
  COMP_CHECK(comp_layer_comp_capture(document, "半透明", &first) == COMP_OK);
  COMP_CHECK_MSG(first > 0u, "capture returns a non-zero comp id");

  size_t entries = 0;
  COMP_CHECK(comp_layer_comp_entry_count(document, first, &entries) == COMP_OK);
  COMP_CHECK_MSG(entries == 3u, "the snapshot covers every existing layer");

  /* 条目内容 = 捕获时的 visible / opacity，顺序与堆叠序一致。 */
  bool saw_b = false;
  bool saw_c = false;
  for (size_t i = 0; i < entries; ++i) {
    comp_layer_id layer = 0;
    uint8_t visible = 2;
    float opacity = -1.0f;
    COMP_CHECK(comp_layer_comp_entry(document, first, i, &layer, &visible, &opacity) == COMP_OK);
    COMP_CHECK(layer != 0u);
    if (layer == b) {
      saw_b = true;
      COMP_CHECK(visible == 1u);
      COMP_CHECK(std::fabs(opacity - 0.25f) <= 0.001f);
    } else if (layer == c) {
      saw_c = true;
      COMP_CHECK_MSG(visible == 0u, "hidden layers are recorded as hidden");
      COMP_CHECK(std::fabs(opacity - 1.0f) <= 0.001f);
    }
  }
  COMP_CHECK(saw_b && saw_c);

  /* 名字读取走两段式：先问长度，再取内容。 */
  size_t required = 0;
  COMP_CHECK(comp_layer_comp_name(document, first, nullptr, 0, &required) == COMP_OK);
  COMP_CHECK(required >= 2u);
  std::vector<char> name(required);
  COMP_CHECK(comp_layer_comp_name(document, first, name.data(), name.size(), &required) ==
             COMP_OK);
  COMP_CHECK_MSG(std::string(name.data()) == "半透明", "the comp name round-trips as UTF-8");
  std::vector<char> small(1);
  size_t small_required = 0;
  COMP_CHECK(comp_layer_comp_name(document, first, small.data(), small.size(), &small_required) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK_MSG(small_required == required, "a short buffer still reports the required size");

  /* 重名允许（面板用 id 区分，名字只是显示文本）。 */
  comp_layer_comp_id duplicate_name = 0;
  COMP_CHECK(comp_layer_comp_capture(document, "半透明", &duplicate_name) == COMP_OK);
  COMP_CHECK(duplicate_name != first);
  COMP_CHECK(comp_layer_comp_delete(document, duplicate_name) == COMP_OK);

  /* 未知 id / 越界下标。 */
  COMP_CHECK(comp_layer_comp_apply(document, 424242, nullptr) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_comp_entry_count(document, 424242, &entries) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_comp_entry(document, first, 999, nullptr, nullptr, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_comp_id_at(document, 99, &unused_id) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_comp_rename(document, 424242, "x") == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_comp_delete(document, 424242) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_comp_update(document, 424242) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------ 应用：还原 / 幂等 / 一步撤销 */

COMP_TEST(layer_comp_apply_restores_and_is_idempotent) {
  comp_engine_desc desc = comp_engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 32;
  document_desc.height = 32;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = bottom_layer(document);
  const comp_layer_id b = add_layer(document, "b");
  COMP_CHECK(a != 0 && b != 0);

  COMP_CHECK(comp_layer_set_opacity(document, b, 0.5f) == COMP_OK);
  comp_layer_comp_id comp = 0;
  COMP_CHECK(comp_layer_comp_capture(document, "snapshot", &comp) == COMP_OK);

  /* 打乱：a 隐藏、b 不透明又可见。 */
  COMP_CHECK(comp_layer_set_visible(document, a, 0) == COMP_OK);
  COMP_CHECK(comp_layer_set_visible(document, b, 1) == COMP_OK);
  COMP_CHECK(comp_layer_set_opacity(document, b, 1.0f) == COMP_OK);
  const size_t steps_before = history_steps(document);

  int32_t changed = -1;
  COMP_CHECK(comp_layer_comp_apply(document, comp, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "apply reports the change");
  LayerLook la = look(document, a);
  LayerLook lb = look(document, b);
  COMP_CHECK(la.found && lb.found);
  COMP_CHECK_MSG(la.visible == 1u, "apply restores visibility");
  COMP_CHECK_MSG(std::fabs(lb.opacity - 0.5f) <= 0.001f, "apply restores opacity");
  COMP_CHECK_MSG(history_steps(document) == steps_before + 1u,
                 "applying a comp is exactly one undo step");

  /* 再应用一次：什么都没变，不落撤销步。 */
  changed = -1;
  COMP_CHECK(comp_layer_comp_apply(document, comp, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "a second apply is a no-op");
  COMP_CHECK_MSG(history_steps(document) == steps_before + 1u,
                 "the no-op apply does not push a history step");
  COMP_CHECK(comp_document_can_undo(document, &changed) == COMP_OK);

  /* 一次撤销把两层的两个属性一起退回。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  la = look(document, a);
  lb = look(document, b);
  COMP_CHECK_MSG(la.visible == 0u, "one undo restores the pre-apply visibility");
  COMP_CHECK_MSG(std::fabs(lb.opacity - 1.0f) <= 0.001f, "and the pre-apply opacity");
  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  la = look(document, a);
  lb = look(document, b);
  COMP_CHECK(la.visible == 1u && std::fabs(lb.opacity - 0.5f) <= 0.001f);

  /* 捕获之后才建的层、以及应用前被删掉的层。 */
  comp_layer_id late = add_layer(document, "late");
  COMP_CHECK(late != 0);
  COMP_CHECK(comp_layer_set_visible(document, late, 0) == COMP_OK);
  COMP_CHECK(comp_layer_set_opacity(document, b, 0.75f) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_layer_comp_apply(document, comp, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  LayerLook ll = look(document, late);
  COMP_CHECK_MSG(ll.found && ll.visible == 0u,
                 "layers created after the capture are left alone");
  COMP_CHECK_MSG(std::fabs(look(document, b).opacity - 0.5f) <= 0.001f,
                 "tracked layers are still restored");

  /* 删掉落进复合里的层：应用跳过它，其余照常。 */
  COMP_CHECK(comp_layer_remove(document, b) == COMP_OK);
  COMP_CHECK(comp_layer_set_visible(document, a, 0) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_layer_comp_apply(document, comp, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(look(document, a).visible == 1u,
                 "a deleted entry is skipped, the rest still applies");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ---------------------------------- update / rename / delete 与 id 不复用 */

COMP_TEST(layer_comp_update_rename_delete_and_id_reuse) {
  comp_engine_desc desc = comp_engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 32;
  document_desc.height = 32;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = bottom_layer(document);
  COMP_CHECK(a != 0);

  comp_layer_comp_id comp = 0;
  COMP_CHECK(comp_layer_comp_capture(document, "v1", &comp) == COMP_OK);

  /* update：把当前状态刷进已有复合（捕获之后再改的两项都要进快照）。 */
  COMP_CHECK(comp_layer_set_visible(document, a, 0) == COMP_OK);
  COMP_CHECK(comp_layer_set_opacity(document, a, 0.3f) == COMP_OK);
  const size_t before_update = history_steps(document);
  COMP_CHECK(comp_layer_comp_update(document, comp) == COMP_OK);
  COMP_CHECK_MSG(history_steps(document) == before_update + 1u, "update is one undo step");
  uint8_t visible = 2;
  float opacity = -1.0f;
  COMP_CHECK(comp_layer_comp_entry(document, comp, 0, nullptr, &visible, &opacity) == COMP_OK);
  COMP_CHECK(visible == 0u && std::fabs(opacity - 0.3f) <= 0.001f);

  /* update 幂等：状态没变就不落撤销步。 */
  const size_t after_update = history_steps(document);
  COMP_CHECK(comp_layer_comp_update(document, comp) == COMP_OK);
  COMP_CHECK_MSG(history_steps(document) == after_update, "a redundant update is a no-op");

  /* update 是随撤销回退的（回到捕获时的 visible/opacity）。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_layer_comp_entry(document, comp, 0, nullptr, &visible, &opacity) == COMP_OK);
  COMP_CHECK_MSG(visible == 1u && std::fabs(opacity - 1.0f) <= 0.001f,
                 "undo rolls the comp snapshot back");
  COMP_CHECK(comp_document_redo(document) == COMP_OK);

  /* rename：校验 + 幂等 + 撤销。 */
  COMP_CHECK(comp_layer_comp_rename(document, comp, "") == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_comp_rename(document, comp, "a\tb") == COMP_ERR_INVALID_ARGUMENT);
  const size_t before_rename = history_steps(document);
  COMP_CHECK(comp_layer_comp_rename(document, comp, "v2") == COMP_OK);
  COMP_CHECK(history_steps(document) == before_rename + 1u);
  COMP_CHECK(comp_layer_comp_rename(document, comp, "v2") == COMP_OK);
  COMP_CHECK_MSG(history_steps(document) == before_rename + 1u,
                 "renaming to the same name is a no-op");
  size_t required = 0;
  std::vector<char> name(64);
  COMP_CHECK(comp_layer_comp_name(document, comp, name.data(), name.size(), &required) == COMP_OK);
  COMP_CHECK(std::string(name.data()) == "v2");
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_layer_comp_name(document, comp, name.data(), name.size(), &required) == COMP_OK);
  COMP_CHECK_MSG(std::string(name.data()) == "v1", "undo restores the old name");
  COMP_CHECK(comp_document_redo(document) == COMP_OK);

  /* delete 是**非**幂等的（删掉不存在的 id 明确报 NOT_FOUND），且撤销能救回。 */
  COMP_CHECK(comp_layer_comp_delete(document, comp) == COMP_OK);
  size_t count = 99;
  COMP_CHECK(comp_layer_comp_count(document, &count) == COMP_OK);
  COMP_CHECK(count == 0u);
  COMP_CHECK(comp_layer_comp_delete(document, comp) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_layer_comp_count(document, &count) == COMP_OK);
  COMP_CHECK_MSG(count == 1u, "undo brings the deleted comp back");
  COMP_CHECK(comp_document_redo(document) == COMP_OK);

  /* id 单调递增、空洞不复用：复合已被删掉（上面的 redo 删的），新建拿到更大的 id。 */
  COMP_CHECK(comp_layer_comp_count(document, &count) == COMP_OK);
  COMP_CHECK_MSG(count == 0u, "the comp is gone before allocating the next id");
  comp_layer_comp_id next = 0;
  COMP_CHECK(comp_layer_comp_capture(document, "v3", &next) == COMP_OK);
  COMP_CHECK_MSG(next > comp, "comp ids are monotonic and never reused");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* --------------------------------------------------- 数量上界 */

COMP_TEST(layer_comp_count_is_bounded) {
  comp_engine_desc desc = comp_engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  size_t created = 0;
  for (;;) {
    comp_layer_comp_id id = 0;
    const comp_result result = comp_layer_comp_capture(document, "c", &id);
    if (result != COMP_OK) {
      COMP_CHECK_MSG(result == COMP_ERR_STATE, "the cap surfaces as COMP_ERR_STATE");
      COMP_CHECK_MSG(id == 0u, "a refused capture does not hand out an id");
      break;
    }
    COMP_CHECK(id != 0u);
    ++created;
    if (created > 4096u) break; /* 上界失效时的护栏，避免死循环 */
  }
  size_t count = 0;
  COMP_CHECK(comp_layer_comp_count(document, &count) == COMP_OK);
  COMP_CHECK_MSG(created == count, "count matches the number of accepted captures");
  COMP_CHECK_MSG(created > 0u, "the cap is not zero");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------- `.comp` 往返 */

COMP_TEST(layer_comp_comp_round_trip) {
  comp_engine_desc desc = comp_engine_desc_make();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 32;
  document_desc.height = 32;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id a = bottom_layer(document);
  const comp_layer_id b = add_layer(document, "b");
  COMP_CHECK(a != 0 && b != 0);

  /* 没有复合时不写键（老 manifest 逐字节不变）。 */
  const std::string empty_path = work_path("comps_empty.comp");
  COMP_CHECK(comp_document_save(document, empty_path.c_str()) == COMP_OK);
  const std::string empty_manifest = read_text(empty_path + "/manifest.json");
  COMP_CHECK_MSG(empty_manifest.find("engineLayerComps") == std::string::npos,
                 "an empty comp list does not write the key");

  COMP_CHECK(comp_layer_set_visible(document, b, 0) == COMP_OK);
  COMP_CHECK(comp_layer_set_opacity(document, a, 0.4f) == COMP_OK);
  comp_layer_comp_id first = 0;
  comp_layer_comp_id second = 0;
  COMP_CHECK(comp_layer_comp_capture(document, "第一", &first) == COMP_OK);
  COMP_CHECK(comp_layer_comp_capture(document, "第二", &second) == COMP_OK);

  const std::string path = work_path("comps_round_trip.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  const std::string manifest = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("\"engineLayerComps\"") != std::string::npos,
                 "engineLayerComps is written when comps exist");
  COMP_CHECK(manifest.find("\"第一\"") != std::string::npos);
  COMP_CHECK(manifest.find("\"第二\"") != std::string::npos);

  /* 读回：id / 名字 / 条目一律保真，id 分配器接着往下走。 */
  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, path.c_str(), &loaded) == COMP_OK);
  if (loaded != nullptr) {
    size_t count = 0;
    COMP_CHECK(comp_layer_comp_count(loaded, &count) == COMP_OK);
    COMP_CHECK_MSG(count == 2u, "both comps survive the round trip");

    comp_layer_comp_id restored = 0;
    COMP_CHECK(comp_layer_comp_id_at(loaded, 0, &restored) == COMP_OK);
    COMP_CHECK_MSG(restored == first, "comp ids are preserved (the host's handles stay valid)");

    size_t required = 0;
    std::vector<char> name(64);
    COMP_CHECK(comp_layer_comp_name(loaded, restored, name.data(), name.size(), &required) ==
               COMP_OK);
    COMP_CHECK_MSG(std::string(name.data()) == "第一", "the comp name survives as UTF-8");

    size_t entries = 0;
    COMP_CHECK(comp_layer_comp_entry_count(loaded, restored, &entries) == COMP_OK);
    COMP_CHECK_MSG(entries == 2u, "entries are restored");
    bool saw_a = false;
    for (size_t i = 0; i < entries; ++i) {
      comp_layer_id layer = 0;
      uint8_t visible = 2;
      float opacity = -1.0f;
      COMP_CHECK(comp_layer_comp_entry(loaded, restored, i, &layer, &visible, &opacity) ==
                 COMP_OK);
      if (layer == a) {
        saw_a = true;
        COMP_CHECK(std::fabs(opacity - 0.4f) <= 0.001f);
        COMP_CHECK(visible == 1u);
      } else if (layer == b) {
        COMP_CHECK(visible == 0u);
      }
    }
    COMP_CHECK_MSG(saw_a, "the entry layer ids resolve back to the loaded layers");

    /* 载入后的复合照样能应用，且新捕获的 id 继续单调。 */
    COMP_CHECK(comp_layer_set_visible(loaded, a, 0) == COMP_OK);
    int32_t changed = -1;
    COMP_CHECK(comp_layer_comp_apply(loaded, restored, &changed) == COMP_OK);
    COMP_CHECK_MSG(changed == 1, "a loaded comp still applies");
    COMP_CHECK_MSG(look(loaded, a).visible == 1u, "and restores the loaded layer");

    comp_layer_comp_id after_load = 0;
    COMP_CHECK(comp_layer_comp_capture(loaded, "第三", &after_load) == COMP_OK);
    COMP_CHECK_MSG(after_load > second, "the id allocator continues past the loaded ids");

    comp_document_destroy(loaded);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
