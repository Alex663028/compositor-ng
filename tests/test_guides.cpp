// AI生成
/*
 * test_guides.cpp — 画布参考线（标尺拖出的对齐线）的回归护栏。
 *
 * 为什么需要这组用例：参考线是**文档状态**（进 .comp、随撤销回退），不是
 * 外壳设置。它一旦读写出错，症状是「存过的参考线开回来没了」或「撤销一步
 * 把参考线连带删了」——界面上都不报错。这里把三条契约钉死：
 *   1. 轴/位置非法在开事务之前拒绝，不留半成品、不进撤销栈；
 *   2. 每次真实变更恰好一步撤销，值未变（changed = 0）不产生空步；
 *   3. `.comp` 往返逐值一致；空表不写键（旧 manifest 逐字节不变），
 *      坏条目如实拒绝而不是静默丢弃。
 *
 * 吸附（snap）是纯几何，外壳与测试共用 domain/guide.h 的 guide_snap_to。
 */
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_guide.h"
#include "compositor/comp_io.h"
#include "compositor/comp_undo.h"
#include "domain/guide.h"
#include "format/comp_json.h"
#include "test_framework.h"

namespace {

comp_engine guide_engine() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-guide-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  return engine;
}

comp_document guide_document(comp_engine engine, int32_t width = 64, int32_t height = 48) {
  comp_document_desc desc{};
  desc.width = width;
  desc.height = height;
  desc.dpi = 72.0f;
  desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &desc, &document) == COMP_OK);
  return document;
}

std::string read_text(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::string();
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

size_t undo_depth(comp_document document) {
  size_t depth = 0;
  COMP_CHECK(comp_document_history_stats(document, &depth, nullptr) == COMP_OK);
  return depth;
}

size_t guide_count(comp_document document) {
  size_t count = 99;
  COMP_CHECK(comp_guide_count(document, &count) == COMP_OK);
  return count;
}

}  // namespace

COMP_TEST(guide_domain_validation_and_snap) {
  using compositor::guide_is_valid;
  using compositor::guide_snap_to;
  using compositor::kGuideHorizontal;
  using compositor::kGuideVertical;

  COMP_CHECK(guide_is_valid(kGuideHorizontal, 0.0));
  COMP_CHECK(guide_is_valid(kGuideVertical, -20.5));
  COMP_CHECK(guide_is_valid(kGuideVertical, 1.0e6));
  COMP_CHECK(!guide_is_valid(2u, 0.0));
  COMP_CHECK(!guide_is_valid(kGuideHorizontal, std::numeric_limits<double>::quiet_NaN()));
  COMP_CHECK(!guide_is_valid(kGuideHorizontal, std::numeric_limits<double>::infinity()));
  COMP_CHECK(!guide_is_valid(kGuideHorizontal, 1.0e6 + 1.0));

  /* 最近者胜；tolerance 外不吸附。 */
  const double candidates[] = {10.0, 12.5, 30.0};
  double out = 0.0;
  COMP_CHECK(guide_snap_to(11.0, candidates, 3, 2.0, &out));
  COMP_CHECK(out == 10.0);
  COMP_CHECK(guide_snap_to(13.0, candidates, 3, 2.0, &out));
  COMP_CHECK(out == 12.5);
  COMP_CHECK(!guide_snap_to(20.0, candidates, 3, 2.0, &out));
  /* tolerance = 0 时正好命中仍吸附（<= 而不是 <）。 */
  COMP_CHECK(guide_snap_to(12.5, candidates, 3, 0.0, &out));
  COMP_CHECK(out == 12.5);
  /* NaN 候选被跳过，不会把结果带坏。 */
  const double with_nan[] = {std::numeric_limits<double>::quiet_NaN(), 40.0};
  COMP_CHECK(guide_snap_to(41.0, with_nan, 2, 2.0, &out));
  COMP_CHECK(out == 40.0);
  /* 空候选表 / 非法 tolerance / 空输出指针。 */
  COMP_CHECK(!guide_snap_to(1.0, nullptr, 0, 2.0, &out));
  COMP_CHECK(!guide_snap_to(1.0, candidates, 3, -1.0, &out));
  COMP_CHECK(!guide_snap_to(1.0, candidates, 3, 2.0, nullptr));
}

COMP_TEST(guide_abi_crud_and_undo) {
  comp_engine engine = guide_engine();
  comp_document document = guide_document(engine);

  COMP_CHECK(guide_count(document) == 0);

  size_t index = 99;
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_VERTICAL, 12.5, &index) == COMP_OK);
  COMP_CHECK(index == 0);
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_HORIZONTAL, -8.0, &index) == COMP_OK);
  COMP_CHECK(index == 1);
  COMP_CHECK(guide_count(document) == 2);
  COMP_CHECK_MSG(undo_depth(document) == 2, "每条参考线各占一步撤销");

  comp_guide guide{};
  COMP_CHECK(comp_guide_get(document, 0, &guide) == COMP_OK);
  COMP_CHECK(guide.axis == COMP_GUIDE_VERTICAL);
  COMP_CHECK(guide.position == 12.5);
  COMP_CHECK(comp_guide_get(document, 1, &guide) == COMP_OK);
  COMP_CHECK(guide.axis == COMP_GUIDE_HORIZONTAL);
  COMP_CHECK(guide.position == -8.0);

  /* 移动到相同位置：changed = 0，不产生撤销步。 */
  const size_t depth_before = undo_depth(document);
  int32_t changed = 1;
  COMP_CHECK(comp_guide_move(document, 0, 12.5, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth(document) == depth_before);

  COMP_CHECK(comp_guide_move(document, 0, 20.0, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(comp_guide_get(document, 0, &guide) == COMP_OK);
  COMP_CHECK(guide.position == 20.0);

  COMP_CHECK(comp_guide_remove(document, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(guide_count(document) == 1);
  COMP_CHECK_MSG(undo_depth(document) == depth_before + 2, "move 与 remove 各一步");

  /* 撤销一条条回退（含参考线本身，说明它确实在 DocumentState 里）。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(guide_count(document) == 2);
  COMP_CHECK(comp_guide_get(document, 0, &guide) == COMP_OK);
  COMP_CHECK(guide.position == 20.0);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_guide_get(document, 0, &guide) == COMP_OK);
  COMP_CHECK(guide.position == 12.5);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(guide_count(document) == 1);
  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(guide_count(document) == 2);

  COMP_CHECK(comp_guide_clear(document, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(guide_count(document) == 0);
  /* 已清空再清空：changed = 0，不产生空撤销步。 */
  const size_t empty_depth = undo_depth(document);
  COMP_CHECK(comp_guide_clear(document, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth(document) == empty_depth);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(guide_count(document) == 2);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(guide_abi_rejects_invalid) {
  comp_engine engine = guide_engine();
  comp_document document = guide_document(engine);

  size_t index = 0;
  COMP_CHECK(comp_guide_add(document, 2u, 0.0, &index) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_VERTICAL,
                            std::numeric_limits<double>::quiet_NaN(),
                            &index) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_VERTICAL,
                            std::numeric_limits<double>::infinity(),
                            &index) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_VERTICAL, 1.0e7,
                            &index) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK_MSG(guide_count(document) == 0, "非法添加不留半成品");
  COMP_CHECK_MSG(undo_depth(document) == 0, "非法添加不进撤销栈");

  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_VERTICAL, 5.0, &index) == COMP_OK);
  int32_t changed = 0;
  comp_guide guide{};
  COMP_CHECK(comp_guide_get(document, 1, &guide) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_guide_get(document, 0, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_guide_move(document, 1, 6.0, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_guide_move(document, 0, std::numeric_limits<double>::quiet_NaN(),
                             &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_guide_remove(document, 1, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_guide_count(document, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  /* 无效文档句柄一律挡成错误码，不解引用。 */
  COMP_CHECK(comp_guide_count(nullptr, &index) == COMP_ERR_INVALID_ARGUMENT);
  comp_document destroyed = guide_document(engine);
  comp_document_destroy(destroyed);
  COMP_CHECK(comp_guide_count(destroyed, &index) == COMP_ERR_INVALID_ARGUMENT);

  /* 容量上限：第 129 条返回 STATE（容量是状态，不是参数）。 */
  size_t accepted = 0;
  for (size_t i = 1; i < compositor::kMaxGuides; ++i) {
    COMP_CHECK(comp_guide_add(document, COMP_GUIDE_HORIZONTAL, static_cast<double>(i), &index) ==
               COMP_OK);
    ++accepted;
  }
  COMP_CHECK(accepted == compositor::kMaxGuides - 1);
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_HORIZONTAL, 999.0, &index) == COMP_ERR_STATE);
  COMP_CHECK(guide_count(document) == compositor::kMaxGuides);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(guide_comp_round_trip) {
  comp_engine engine = guide_engine();
  const std::string path = "build/guide_round_trip.comp";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  comp_document document = guide_document(engine, 100, 80);
  size_t index = 0;
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_VERTICAL, 12.5, &index) == COMP_OK);
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_HORIZONTAL, -20.0, &index) == COMP_OK);
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_VERTICAL, 99.0, &index) == COMP_OK);
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);

  const std::string manifest = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("\"engineGuides\"") != std::string::npos,
                 "非空参考线写出 engineGuides");

  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(loaded != nullptr);
  COMP_CHECK(guide_count(loaded) == 3);
  comp_guide guide{};
  COMP_CHECK(comp_guide_get(loaded, 0, &guide) == COMP_OK);
  COMP_CHECK(guide.axis == COMP_GUIDE_VERTICAL && guide.position == 12.5);
  COMP_CHECK(comp_guide_get(loaded, 1, &guide) == COMP_OK);
  COMP_CHECK(guide.axis == COMP_GUIDE_HORIZONTAL && guide.position == -20.0);
  COMP_CHECK(comp_guide_get(loaded, 2, &guide) == COMP_OK);
  COMP_CHECK(guide.axis == COMP_GUIDE_VERTICAL && guide.position == 99.0);
  COMP_CHECK_MSG(undo_depth(loaded) == 0, "加载后参考线不占用撤销栈");

  /* 再存一次：读回的值原样写回（往返不漂移）。 */
  const std::string second = "build/guide_round_trip_2.comp";
  std::filesystem::remove_all(second, ec);
  COMP_CHECK(comp_document_save(loaded, second.c_str()) == COMP_OK);
  comp_document reloaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, second.c_str(), &reloaded) == COMP_OK);
  COMP_CHECK(guide_count(reloaded) == 3);
  COMP_CHECK(comp_guide_get(reloaded, 0, &guide) == COMP_OK);
  COMP_CHECK(guide.position == 12.5);
  comp_document_destroy(reloaded);

  /* 空表不写键：旧 manifest 的字节布局不被无谓改写。 */
  comp_document empty = guide_document(engine, 16, 16);
  const std::string empty_path = "build/guide_empty.comp";
  std::filesystem::remove_all(empty_path, ec);
  COMP_CHECK(comp_document_save(empty, empty_path.c_str()) == COMP_OK);
  COMP_CHECK_MSG(read_text(empty_path + "/manifest.json").find("engineGuides") ==
                     std::string::npos,
                 "空参考线不写键");
  comp_document_destroy(empty);

  comp_document_destroy(loaded);
  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(guide_comp_rejects_bad_entries) {
  using compositor::json_dump;
  using compositor::json_parse;
  using compositor::JsonValue;

  comp_engine engine = guide_engine();
  const std::string path = "build/guide_bad.comp";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  comp_document document = guide_document(engine, 32, 32);
  size_t index = 0;
  COMP_CHECK(comp_guide_add(document, COMP_GUIDE_VERTICAL, 4.0, &index) == COMP_OK);
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  comp_document_destroy(document);

  const std::string manifest_path = path + "/manifest.json";
  const std::string original = read_text(manifest_path);
  std::string parse_error;

  /* 1. 非数组。 */
  {
    JsonValue manifest = json_parse(original, &parse_error);
    COMP_CHECK(manifest.is_object());
    manifest.set("engineGuides", JsonValue::string("nope"));
    write_text(manifest_path, json_dump(manifest, 2));
    comp_document loaded = nullptr;
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  /* 2. 轴越界。 */
  {
    JsonValue manifest = json_parse(original, &parse_error);
    JsonValue entry = JsonValue::object({});
    entry.set("axis", JsonValue::integer(2));
    entry.set("position", JsonValue::number(4.0));
    JsonValue guides = JsonValue::array({});
    guides.push(std::move(entry));
    manifest.set("engineGuides", std::move(guides));
    write_text(manifest_path, json_dump(manifest, 2));
    comp_document loaded = nullptr;
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  /* 3. 位置非有限（JSON 里写 null 不是数值）。 */
  {
    JsonValue manifest = json_parse(original, &parse_error);
    JsonValue entry = JsonValue::object({});
    entry.set("axis", JsonValue::integer(1));
    entry.set("position", JsonValue()); /* null */
    JsonValue guides = JsonValue::array({});
    guides.push(std::move(entry));
    manifest.set("engineGuides", std::move(guides));
    write_text(manifest_path, json_dump(manifest, 2));
    comp_document loaded = nullptr;
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  /* 4. 原样恢复后可正常读回（上面的失败不污染文件）。 */
  write_text(manifest_path, original);
  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(loaded != nullptr);
  COMP_CHECK(guide_count(loaded) == 1);
  comp_document_destroy(loaded);

  comp_engine_destroy(engine);
}
