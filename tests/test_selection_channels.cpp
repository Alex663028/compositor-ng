// AI生成
/*
 * test_selection_channels.cpp — 存储选区通道（PS 的 Alpha 通道）的回归护栏。
 *
 * 为什么需要这组用例：通道是**文档状态**（进 .comp、随撤销回退），保存的是
 * 「硬边蒙版 + 羽化半径」而不是烘焙过羽化的 coverage。这里钉死四条契约：
 *   1. 存/载往返逐字节相等，羽化参数一起恢复；等值载回不产生空撤销步；
 *   2. add / subtract 与形状叠加同一口径，且把羽化归 0；
 *   3. 容量 16、非法名字/索引/枚举在开事务之前拒绝，不留半成品；
 *   4. `.comp` 往返保真，空表不写键；换画布丢通道、旋转画布转置通道。
 */
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "compositor/comp_canvas.h"
#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_selection.h"
#include "compositor/comp_undo.h"
#include "domain/domain_state.h"
#include "domain/selection.h"
#include "domain/selection_channel.h"
#include "format/comp_json.h"
#include "format/comp_png.h"
#include "test_framework.h"

namespace {

comp_engine channel_engine() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-channel-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  return engine;
}

comp_document channel_document(comp_engine engine, int32_t width = 16, int32_t height = 12) {
  comp_document_desc desc{};
  desc.width = width;
  desc.height = height;
  desc.dpi = 72.0f;
  desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &desc, &document) == COMP_OK);
  return document;
}

size_t undo_depth(comp_document document) {
  size_t depth = 0;
  COMP_CHECK(comp_document_history_stats(document, &depth, nullptr) == COMP_OK);
  return depth;
}

size_t channel_count(comp_document document) {
  size_t count = 99;
  COMP_CHECK(comp_selection_channel_count(document, &count) == COMP_OK);
  return count;
}

std::string channel_name(comp_document document, size_t index) {
  size_t required = 0;
  if (comp_selection_channel_get_name(document, index, nullptr, 0, &required) != COMP_OK) {
    return std::string();
  }
  std::vector<char> buffer(required);
  if (comp_selection_channel_get_name(document, index, buffer.data(), buffer.size(), &required) !=
      COMP_OK) {
    return std::string();
  }
  return std::string(buffer.data());
}

void select_rect(comp_document document, float x0, float y0, float x1, float y1,
                 comp_selection_combine combine) {
  const float points[4] = {x0, y0, x1, y1};
  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_shape(document, COMP_SELECTION_RECT, points, 2, combine, 0,
                                        &changed) == COMP_OK);
}

std::vector<uint8_t> coverage(comp_document document, int32_t width, int32_t height) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  comp_mask mask{width, height, static_cast<size_t>(width), pixels.data()};
  if (comp_selection_get_coverage(document, &mask) != COMP_OK) pixels.clear();
  return pixels;
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

}  // namespace

COMP_TEST(selection_channel_domain_contract) {
  using compositor::DocumentState;
  using compositor::kMaxSelectionChannelNameBytes;
  using compositor::kSelectionAdd;
  using compositor::kSelectionReplace;
  using compositor::kSelectionSubtract;
  using compositor::selection_channel_clear;
  using compositor::selection_channel_load;
  using compositor::selection_channel_name_is_valid;
  using compositor::selection_channel_remove;
  using compositor::selection_channel_save;

  /* 名字：非空、≤255 字节、严格 UTF-8、无控制字符。 */
  COMP_CHECK(selection_channel_name_is_valid("Alpha 1"));
  COMP_CHECK(selection_channel_name_is_valid("选区"));
  COMP_CHECK(selection_channel_name_is_valid(std::string(kMaxSelectionChannelNameBytes, 'a')));
  COMP_CHECK(!selection_channel_name_is_valid(""));
  COMP_CHECK(!selection_channel_name_is_valid(std::string(kMaxSelectionChannelNameBytes + 1, 'a')));
  COMP_CHECK(!selection_channel_name_is_valid(std::string("a\0b", 3)));
  COMP_CHECK(!selection_channel_name_is_valid("a\nb"));
  COMP_CHECK(!selection_channel_name_is_valid("\xC3"));        /* 截断 */
  COMP_CHECK(!selection_channel_name_is_valid("\xC0\x80"));    /* 超长编码 */
  COMP_CHECK(!selection_channel_name_is_valid("\xED\xA0\x80")); /* 代理区 */

  DocumentState state;
  state.width = 4;
  state.height = 3;
  size_t index = 7;
  COMP_CHECK(!selection_channel_save(&state, "A", &index)); /* 没有选区 */
  COMP_CHECK(state.channels.empty());

  state.selection.active = true;
  state.selection.base.resize(4, 3, 0);
  for (int32_t y = 0; y < 3; ++y) {
    uint8_t* row = state.selection.base.writable_row(y);
    row[0] = 255;
    row[1] = 128;
  }
  state.selection.feather = 2.0f;
  COMP_CHECK(selection_channel_save(&state, "A", &index));
  COMP_CHECK(index == 0);
  COMP_CHECK(state.channels.size() == 1);
  COMP_CHECK_MSG(state.channels[0].mask.pixels == state.selection.base.pixels,
                 "保存按 COW 共享蒙版，不复制像素");
  COMP_CHECK(state.channels[0].feather == 2.0f);

  /* 改选区不能改坏通道：writable 路径必须自己分离。 */
  state.selection.base.writable_row(0)[0] = 0;
  COMP_CHECK(state.channels[0].mask.row(0)[0] == 255);

  state.selection = compositor::SelectionState{};
  bool changed = false;
  COMP_CHECK(selection_channel_load(&state, 0, kSelectionReplace, &changed));
  COMP_CHECK(changed);
  COMP_CHECK(state.selection.active);
  COMP_CHECK(state.selection.feather == 2.0f);
  COMP_CHECK(state.selection.base.row(0)[0] == 255);
  COMP_CHECK(state.selection.base.row(0)[1] == 128);

  /* 结果逐字节相同：不产生空撤销步。 */
  changed = true;
  COMP_CHECK(selection_channel_load(&state, 0, kSelectionReplace, &changed));
  COMP_CHECK(!changed);

  COMP_CHECK(!selection_channel_load(&state, 1, kSelectionReplace, &changed));
  COMP_CHECK(!selection_channel_load(&state, 0, 7, &changed));

  /* add / subtract 归零羽化；add 从空选区开始时蒙版等价于 replace。 */
  state.selection.feather = 4.0f;
  state.selection.base.fill(0);
  COMP_CHECK(selection_channel_load(&state, 0, kSelectionAdd, &changed));
  COMP_CHECK(changed);
  COMP_CHECK(state.selection.feather == 0.0f);
  COMP_CHECK(state.selection.base.row(0)[1] == 128);

  /* subtract 没有基础选区：无操作，羽化也不动。 */
  state.selection = compositor::SelectionState{};
  changed = true;
  COMP_CHECK(selection_channel_load(&state, 0, kSelectionSubtract, &changed));
  COMP_CHECK(!changed);
  COMP_CHECK(!state.selection.active);

  changed = false;
  COMP_CHECK(selection_channel_remove(&state, 0, &changed));
  COMP_CHECK(changed);
  COMP_CHECK(state.channels.empty());
  COMP_CHECK(!selection_channel_remove(&state, 0, &changed));

  changed = true;
  COMP_CHECK(selection_channel_clear(&state, &changed));
  COMP_CHECK(!changed);
}

COMP_TEST(selection_channel_abi_round_trip_and_undo) {
  comp_engine engine = channel_engine();
  comp_document document = channel_document(engine);

  COMP_CHECK(channel_count(document) == 0);
  size_t index = 0;
  COMP_CHECK_MSG(comp_selection_channel_save(document, "Alpha 1", &index) == COMP_ERR_STATE,
                 "没有选区不能存通道");
  COMP_CHECK(channel_count(document) == 0);
  COMP_CHECK(undo_depth(document) == 0);

  select_rect(document, 2.0f, 2.0f, 8.0f, 7.0f, COMP_SELECTION_REPLACE);
  int32_t changed = 0;
  COMP_CHECK(comp_selection_set_feather(document, 3, &changed) == COMP_OK);
  const std::vector<uint8_t> expected = coverage(document, 12, 10);
  const size_t depth = undo_depth(document);

  COMP_CHECK(comp_selection_channel_save(document, "Alpha 1", &index) == COMP_OK);
  COMP_CHECK(index == 0);
  COMP_CHECK(channel_count(document) == 1);
  COMP_CHECK_MSG(undo_depth(document) == depth + 1, "保存通道是一步撤销");

  /* 两段式读名：查询模式、容量不足拒绝、容量够才写。 */
  size_t required = 0;
  COMP_CHECK(comp_selection_channel_get_name(document, 0, nullptr, 0, &required) == COMP_OK);
  COMP_CHECK(required == 8); /* "Alpha 1" + NUL */
  char small[4] = {};
  COMP_CHECK(comp_selection_channel_get_name(document, 0, small, sizeof(small), &required) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(required == 8);
  char buffer[8] = {};
  COMP_CHECK(comp_selection_channel_get_name(document, 0, buffer, sizeof(buffer), &required) ==
             COMP_OK);
  COMP_CHECK(std::string(buffer) == "Alpha 1");

  /* 立即 replace 载回：逐字节相同 → changed = 0，不加撤销步。 */
  changed = 1;
  COMP_CHECK(comp_selection_channel_load(document, 0, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth(document) == depth + 1);

  COMP_CHECK(comp_selection_deselect(document, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  changed = 0;
  COMP_CHECK(comp_selection_channel_load(document, 0, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(coverage(document, 12, 10) == expected);
  float feather = 0.0f;
  COMP_CHECK(comp_selection_get_feather(document, &feather) == COMP_OK);
  COMP_CHECK(feather == 3.0f);

  /* 撤销载回 = 回到「无选区」；通道本身还在（它也在 DocumentState 里）。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  int32_t active = 1;
  COMP_CHECK(comp_selection_get_active(document, &active) == COMP_OK);
  COMP_CHECK(active == 0);
  COMP_CHECK(channel_count(document) == 1);
  COMP_CHECK(comp_document_redo(document) == COMP_OK);

  /* 非法名字：都在开事务之前拒绝。 */
  COMP_CHECK(comp_selection_channel_save(document, "", &index) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_save(document, nullptr, &index) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_save(document, "\xFF", &index) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_save(document, "a\nb", &index) == COMP_ERR_INVALID_ARGUMENT);
  const std::string too_long(256, 'a');
  COMP_CHECK(comp_selection_channel_save(document, too_long.c_str(), &index) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(channel_count(document) == 1);

  COMP_CHECK(comp_selection_channel_get_name(document, 1, buffer, 8, &required) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_get_name(document, 0, buffer, 8, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_load(document, 1, COMP_SELECTION_REPLACE, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_load(document, 0, static_cast<comp_selection_combine>(9),
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_remove(document, 1, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_count(document, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_count(nullptr, &index) == COMP_ERR_INVALID_ARGUMENT);
  comp_document destroyed = channel_document(engine);
  comp_document_destroy(destroyed);
  COMP_CHECK(comp_selection_channel_count(destroyed, &index) == COMP_ERR_INVALID_ARGUMENT);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(selection_channel_combine_semantics) {
  comp_engine engine = channel_engine();

  /* 用同一套形状叠加算参考值，避免把「矩形栅格化」再写一遍。 */
  comp_document reference = channel_document(engine);
  select_rect(reference, 2.0f, 2.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE);
  select_rect(reference, 5.0f, 5.0f, 12.0f, 12.0f, COMP_SELECTION_ADD);
  const std::vector<uint8_t> expected_add = coverage(reference, 16, 12);

  comp_document reference_sub = channel_document(engine);
  select_rect(reference_sub, 2.0f, 2.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE);
  select_rect(reference_sub, 5.0f, 5.0f, 12.0f, 12.0f, COMP_SELECTION_SUBTRACT);
  const std::vector<uint8_t> expected_subtract = coverage(reference_sub, 16, 12);

  comp_document document = channel_document(engine);
  size_t index = 0;
  select_rect(document, 2.0f, 2.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE);
  COMP_CHECK(comp_selection_channel_save(document, "A", &index) == COMP_OK);

  /* B 带羽化保存：载入 add 时羽化必须归 0，蒙版仍是硬边。 */
  select_rect(document, 5.0f, 5.0f, 12.0f, 12.0f, COMP_SELECTION_REPLACE);
  int32_t changed = 0;
  COMP_CHECK(comp_selection_set_feather(document, 5, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_channel_save(document, "B", &index) == COMP_OK);
  COMP_CHECK(index == 1);
  const std::vector<uint8_t> hard_b = [&] {
    select_rect(document, 5.0f, 5.0f, 12.0f, 12.0f, COMP_SELECTION_REPLACE);
    return coverage(document, 16, 12);
  }();

  select_rect(document, 2.0f, 2.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE);
  changed = 0;
  COMP_CHECK(comp_selection_channel_load(document, 1, COMP_SELECTION_ADD, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(coverage(document, 16, 12) == expected_add, "add 与形状叠加同口径");
  float feather = 9.0f;
  COMP_CHECK(comp_selection_get_feather(document, &feather) == COMP_OK);
  COMP_CHECK(feather == 0.0f);

  select_rect(document, 2.0f, 2.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE);
  changed = 0;
  COMP_CHECK(comp_selection_channel_load(document, 1, COMP_SELECTION_SUBTRACT, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(coverage(document, 16, 12) == expected_subtract, "subtract 与形状叠加同口径");

  /* 没有选区时：add 得到通道蒙版（羽化 0）；subtract 无操作。 */
  COMP_CHECK(comp_selection_deselect(document, &changed) == COMP_OK);
  changed = 0;
  COMP_CHECK(comp_selection_channel_load(document, 1, COMP_SELECTION_ADD, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(coverage(document, 16, 12) == hard_b);
  COMP_CHECK(comp_selection_get_feather(document, &feather) == COMP_OK);
  COMP_CHECK(feather == 0.0f);

  COMP_CHECK(comp_selection_deselect(document, &changed) == COMP_OK);
  changed = 1;
  COMP_CHECK(comp_selection_channel_load(document, 1, COMP_SELECTION_SUBTRACT, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 0);
  int32_t active = 1;
  COMP_CHECK(comp_selection_get_active(document, &active) == COMP_OK);
  COMP_CHECK(active == 0);

  comp_document_destroy(document);
  comp_document_destroy(reference);
  comp_document_destroy(reference_sub);
  comp_engine_destroy(engine);
}

COMP_TEST(selection_channel_capacity_and_undo) {
  comp_engine engine = channel_engine();
  comp_document document = channel_document(engine, 8, 8);

  int32_t changed = 0;
  COMP_CHECK(comp_selection_select_all(document, &changed) == COMP_OK);

  size_t index = 99;
  for (size_t i = 0; i < compositor::kMaxSelectionChannels; ++i) {
    const std::string name = "C" + std::to_string(i);
    COMP_CHECK(comp_selection_channel_save(document, name.c_str(), &index) == COMP_OK);
    COMP_CHECK(index == i);
  }
  COMP_CHECK(channel_count(document) == compositor::kMaxSelectionChannels);
  COMP_CHECK_MSG(comp_selection_channel_save(document, "overflow", &index) == COMP_ERR_STATE,
                 "容量满回报 STATE");

  const size_t full_depth = undo_depth(document);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(channel_count(document) == compositor::kMaxSelectionChannels - 1);
  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(channel_count(document) == compositor::kMaxSelectionChannels);
  COMP_CHECK(undo_depth(document) == full_depth);

  COMP_CHECK(comp_selection_channel_remove(document, 0, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(channel_count(document) == compositor::kMaxSelectionChannels - 1);
  COMP_CHECK(channel_name(document, 0) == "C1");
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(channel_count(document) == compositor::kMaxSelectionChannels);
  COMP_CHECK_MSG(channel_name(document, 0) == "C0", "撤销删除后下标与名字原样回来");

  COMP_CHECK(comp_selection_channel_remove(document, 99, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_channel_load(document, 99, COMP_SELECTION_REPLACE, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(selection_channel_comp_round_trip) {
  comp_engine engine = channel_engine();
  const std::string path = "build/channel_round_trip.comp";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  comp_document document = channel_document(engine);
  size_t index = 0;
  int32_t changed = 0;
  select_rect(document, 1.0f, 2.0f, 9.0f, 10.0f, COMP_SELECTION_REPLACE);
  COMP_CHECK(comp_selection_set_feather(document, 5, &changed) == COMP_OK);
  const std::vector<uint8_t> expected = coverage(document, 16, 12);
  COMP_CHECK(comp_selection_channel_save(document, "Chan One", &index) == COMP_OK);

  select_rect(document, 0.0f, 0.0f, 4.0f, 4.0f, COMP_SELECTION_REPLACE);
  COMP_CHECK(comp_selection_set_feather(document, 1, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_channel_save(document, "选区二", &index) == COMP_OK);
  COMP_CHECK(index == 1);

  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  const std::string manifest_path = path + "/manifest.json";
  const std::string manifest = read_text(manifest_path);
  COMP_CHECK_MSG(manifest.find("\"engineChannels\"") != std::string::npos,
                 "非空通道写出 engineChannels");

  size_t png_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(path + "/images")) {
    if (entry.path().extension() == ".png" &&
        entry.path().filename().u8string().find(".channel") != std::string::npos) {
      ++png_count;
    }
  }
  COMP_CHECK(png_count == 2);

  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(loaded != nullptr);
  COMP_CHECK(channel_count(loaded) == 2);
  COMP_CHECK(channel_name(loaded, 0) == "Chan One");
  COMP_CHECK(channel_name(loaded, 1) == "选区二");
  COMP_CHECK_MSG(undo_depth(loaded) == 0, "加载后通道不占撤销栈");

  COMP_CHECK(comp_selection_channel_load(loaded, 0, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(coverage(loaded, 16, 12) == expected);
  float feather = 0.0f;
  COMP_CHECK(comp_selection_get_feather(loaded, &feather) == COMP_OK);
  COMP_CHECK(feather == 5.0f);

  /* 再存一次：manifest 逐字节一致（通道往返不漂移）。 */
  const std::string second = "build/channel_round_trip_2.comp";
  std::filesystem::remove_all(second, ec);
  COMP_CHECK(comp_document_save(loaded, second.c_str()) == COMP_OK);
  COMP_CHECK_MSG(read_text(second + "/manifest.json") == manifest, "二次保存 manifest 逐字节一致");

  /* 空表不写键：旧 manifest 的字节布局不被无谓改写。 */
  comp_document empty = channel_document(engine, 8, 8);
  const std::string empty_path = "build/channel_empty.comp";
  std::filesystem::remove_all(empty_path, ec);
  COMP_CHECK(comp_document_save(empty, empty_path.c_str()) == COMP_OK);
  COMP_CHECK_MSG(read_text(empty_path + "/manifest.json").find("engineChannels") ==
                     std::string::npos,
                 "空通道不写键");

  comp_document_destroy(empty);
  comp_document_destroy(loaded);
  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(selection_channel_comp_rejects_bad_entries) {
  using compositor::json_dump;
  using compositor::json_parse;
  using compositor::JsonValue;

  comp_engine engine = channel_engine();
  const std::string path = "build/channel_bad.comp";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  comp_document document = channel_document(engine, 16, 12);
  select_rect(document, 2.0f, 2.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE);
  size_t index = 0;
  COMP_CHECK(comp_selection_channel_save(document, "Alpha 1", &index) == COMP_OK);
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  comp_document_destroy(document);

  const std::string manifest_path = path + "/manifest.json";
  const std::string original = read_text(manifest_path);
  std::string parse_error;

  /* 1. 非数组。 */
  {
    JsonValue manifest = json_parse(original, &parse_error);
    COMP_CHECK(manifest.is_object());
    manifest.set("engineChannels", JsonValue::string("nope"));
    write_text(manifest_path, json_dump(manifest, 2));
    comp_document loaded = nullptr;
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  /* 2. 名字非法（控制字符）。 */
  {
    JsonValue manifest = json_parse(original, &parse_error);
    JsonValue entry = JsonValue::object({});
    entry.set("name", JsonValue::string("bad\nname"));
    entry.set("feather", JsonValue::number(0.0));
    JsonValue channels = JsonValue::array({});
    channels.push(std::move(entry));
    manifest.set("engineChannels", std::move(channels));
    write_text(manifest_path, json_dump(manifest, 2));
    comp_document loaded = nullptr;
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  /* 3. feather 越界。 */
  {
    JsonValue manifest = json_parse(original, &parse_error);
    JsonValue entry = JsonValue::object({});
    entry.set("name", JsonValue::string("Alpha 1"));
    entry.set("feather", JsonValue::number(-1.0));
    JsonValue channels = JsonValue::array({});
    channels.push(std::move(entry));
    manifest.set("engineChannels", std::move(channels));
    write_text(manifest_path, json_dump(manifest, 2));
    comp_document loaded = nullptr;
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  /* 4. 条数超上限。 */
  {
    JsonValue manifest = json_parse(original, &parse_error);
    JsonValue channels = JsonValue::array({});
    for (size_t i = 0; i <= compositor::kMaxSelectionChannels; ++i) {
      JsonValue entry = JsonValue::object({});
      entry.set("name", JsonValue::string("C" + std::to_string(i)));
      entry.set("feather", JsonValue::number(0.0));
      channels.push(std::move(entry));
    }
    manifest.set("engineChannels", std::move(channels));
    write_text(manifest_path, json_dump(manifest, 2));
    comp_document loaded = nullptr;
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  /* 恢复 manifest，通道资产仍在。 */
  write_text(manifest_path, original);
  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(channel_count(loaded) == 1);
  comp_document_destroy(loaded);

  /* 5. 蒙版尺寸与文档不符。 */
  {
    std::vector<uint8_t> small(4 * 4, 255);
    std::vector<uint8_t> png;
    COMP_CHECK(compositor::png_encode_gray8(small.data(), 4, 4, &png, nullptr));
    /* 文件名由 (documentID, 下标) 派生：从 manifest 里取 documentID。 */
    JsonValue manifest = json_parse(original, &parse_error);
    const std::string document_id = manifest.string_or("documentID", "");
    COMP_CHECK(!document_id.empty());
    std::ofstream out(path + "/images/" + document_id + ".channel0.png",
                      std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    out.close();

    loaded = nullptr; /* 失败路径不该写 out 参数；先清掉上一个成功加载的句柄 */
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  /* 6. 资产缺失。 */
  {
    JsonValue manifest = json_parse(original, &parse_error);
    const std::string document_id = manifest.string_or("documentID", "");
    std::filesystem::remove(path + "/images/" + document_id + ".channel0.png", ec);
    loaded = nullptr;
    COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) != COMP_OK);
    COMP_CHECK(loaded == nullptr);
  }

  comp_engine_destroy(engine);
}

COMP_TEST(selection_channel_follows_canvas_geometry) {
  comp_engine engine = channel_engine();
  comp_document document = channel_document(engine, 8, 6);

  select_rect(document, 0.0f, 0.0f, 2.0f, 6.0f, COMP_SELECTION_REPLACE);
  size_t index = 0;
  COMP_CHECK(comp_selection_channel_save(document, "Left", &index) == COMP_OK);
  COMP_CHECK(channel_count(document) == 1);

  /* Image Size：换坐标系，通道与选区一起丢弃；撤销回来。 */
  COMP_CHECK(comp_document_resize_image(document, comp_size{16, 16}) == COMP_OK);
  COMP_CHECK(channel_count(document) == 0);
  int32_t active = 1;
  COMP_CHECK(comp_selection_get_active(document, &active) == COMP_OK);
  COMP_CHECK(active == 0);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(channel_count(document) == 1);
  comp_size size{};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 8 && size.height == 6);

  /* 90° 顺时针：8x6 -> 6x8，左侧两列转到顶部两行。 */
  int32_t changed = 0;
  COMP_CHECK(comp_document_rotate_canvas(document, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 6 && size.height == 8);
  COMP_CHECK(channel_count(document) == 1);

  COMP_CHECK(comp_selection_deselect(document, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_channel_load(document, 0, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const std::vector<uint8_t> rotated = coverage(document, 6, 8);
  COMP_CHECK_MSG(rotated[0 * 6 + 3] == 255 && rotated[1 * 6 + 3] == 255,
                 "转置后的通道是顶部两行");
  COMP_CHECK(rotated[2 * 6 + 3] == 0);

  /* Canvas Size：同样丢通道。 */
  COMP_CHECK(comp_document_set_size(document, comp_size{20, 20}) == COMP_OK);
  COMP_CHECK(channel_count(document) == 0);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(channel_count(document) == 1);
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 6 && size.height == 8);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
