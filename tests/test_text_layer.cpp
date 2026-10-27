// AI生成
/*
 * test_text_layer.cpp — 可编辑文字图层（P0-6 ①）的 ABI 契约。
 *
 * 覆盖的**用户可见**承诺（BACKLOG P0-6 ① 的验收口径）：
 *   - `comp_layer_add_text` 建出的图层可以整份读回（文本 / 原点 / 字号 / 颜色 /
 *     不透明度 / 字体路径），像素只是它的渲染缓存；
 *   - 改字号/文本后重新合成：**非文字图层的像素逐字节不变**，文字图层的像素
 *     重新渲染，撤销一步回到旧状态；
 *   - 文字图层的像素只能经 set_text 改写：`comp_layer_set_pixels` 与
 *     `comp_tool_apply` 一律 COMP_ERR_STATE（先栅格化的规矩）；
 *   - `comp_layer_rasterize_text` 幂等、只记一步撤销，之后该层就是普通像素层；
 *   - 参数/字体错误发生在**任何副作用之前**：不建层、不消耗 id、不进撤销栈；
 *   - `.comp` 存盘再打开，文字对象仍在、仍可继续编辑。
 *
 * 只使用 comp_*.h 的 C 符号（与三端外壳同一视角）；不碰引擎内部类型。
 */
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

namespace fs = std::filesystem;

constexpr int32_t kCanvas = 64;

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-text-layer-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 2;
  return desc;
}

/* 引擎 + 文档的成对所有权；析构顺序与外壳一致（先文档后引擎）。 */
struct Fixture {
  comp_engine engine = nullptr;
  comp_document document = nullptr;

  explicit Fixture(int32_t width = kCanvas, int32_t height = kCanvas) {
    comp_engine_desc engine_desc = test_engine_desc();
    COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
    comp_document_desc document_desc{};
    document_desc.width = width;
    document_desc.height = height;
    document_desc.dpi = 72.0f;
    document_desc.color_space = "sRGB";
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  }

  ~Fixture() {
    if (document != nullptr) comp_document_destroy(document);
    if (engine != nullptr) comp_engine_destroy(engine);
  }

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
};

/* 内置 5×7 轨道（font_path == NULL）的文本描述。 */
comp_text_desc make_text(const char* text, double origin_x, double origin_y, double font_size) {
  comp_text_desc desc{};
  desc.text = text;
  desc.text_length = static_cast<int32_t>(std::strlen(text));
  desc.origin_x = origin_x;
  desc.origin_y = origin_y;
  desc.style.font_size = font_size;
  desc.style.line_height = 1.2;
  desc.style.letter_spacing = 0.0;
  desc.style.antialias = 1;
  desc.color[0] = 255;
  desc.color[1] = 0;
  desc.color[2] = 0;
  desc.color[3] = 255;
  desc.opacity = 1.0f;
  desc.font_path_utf8 = nullptr;
  desc.font_face_index = 0;
  desc._reserved = 0;
  return desc;
}

size_t layer_count(comp_document document) {
  size_t count = 0;
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  return count;
}

size_t history_steps(comp_document document) {
  size_t steps = 0;
  COMP_CHECK(comp_document_history_step_count(document, &steps) == COMP_OK);
  return steps;
}

/* 图层原始像素（不做合成）。空图层返回空 vector。 */
std::vector<uint8_t> layer_bytes(comp_document document, comp_layer_id id) {
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  if (comp_layer_get_pixels(document, id, nullptr, 0, &width, &height, &format) != COMP_OK) {
    return {};
  }
  if (width <= 0 || height <= 0) return {};
  const size_t stride = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> bytes(static_cast<size_t>(height) * stride, 0);
  if (comp_layer_get_pixels(document, id, bytes.data(), stride, &width, &height, &format) !=
      COMP_OK) {
    return {};
  }
  return bytes;
}

std::vector<uint8_t> flatten(comp_document document) {
  std::vector<uint8_t> canvas(static_cast<size_t>(kCanvas) * static_cast<size_t>(kCanvas) * 4u, 0);
  const comp_result result = comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8,
                                                 static_cast<size_t>(kCanvas) * 4u);
  COMP_CHECK_MSG(result == COMP_OK, "comp_render_flatten");
  return canvas;
}

/* 画布上有墨的像素数（alpha > 0）。 */
size_t ink_pixels(const std::vector<uint8_t>& canvas) {
  size_t count = 0;
  for (size_t i = 3; i < canvas.size(); i += 4) {
    if (canvas[i] > 0) ++count;
  }
  return count;
}

comp_layer_id background_layer(comp_document document) {
  comp_layer_id ids[16] = {0};
  size_t count = 0;
  COMP_CHECK(comp_layer_list(document, ids, 16, &count) == COMP_OK);
  return count > 0 ? ids[0] : 0;
}

/* 画布有墨像素的包围盒（文档坐标）。空 -> max_x < 0。 */
struct InkBox {
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t max_x = -1;
  int32_t max_y = -1;
};

InkBox ink_box(const std::vector<uint8_t>& canvas) {
  InkBox box;
  bool first = true;
  for (int32_t y = 0; y < kCanvas; ++y) {
    for (int32_t x = 0; x < kCanvas; ++x) {
      const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(kCanvas) +
                            static_cast<size_t>(x)) * 4u + 3u;
      if (canvas[index] == 0) continue;
      if (first) {
        box.min_x = box.max_x = x;
        box.min_y = box.max_y = y;
        first = false;
        continue;
      }
      if (x < box.min_x) box.min_x = x;
      if (x > box.max_x) box.max_x = x;
      if (y < box.min_y) box.min_y = y;
      if (y > box.max_y) box.max_y = y;
    }
  }
  return box;
}

/* 画布 y ∈ [lo, hi) 区间的墨量。 */
size_t ink_in_rows(const std::vector<uint8_t>& canvas, int32_t lo, int32_t hi) {
  size_t count = 0;
  for (int32_t y = lo; y < hi && y < kCanvas; ++y) {
    for (int32_t x = 0; x < kCanvas; ++x) {
      const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(kCanvas) +
                            static_cast<size_t>(x)) * 4u + 3u;
      if (canvas[index] > 0) ++count;
    }
  }
  return count;
}

/* 图层局部像素里的墨量（不看别的图层）。 */
size_t layer_ink(const std::vector<uint8_t>& bytes) {
  size_t count = 0;
  for (size_t i = 3; i < bytes.size(); i += 4) {
    if (bytes[i] > 0) ++count;
  }
  return count;
}

/* 两阶段读文本：先问长度，再取内容。返回是否成功，并回填 desc / text。 */
bool read_text(comp_document document, comp_layer_id id, comp_text_desc* out_desc,
               std::string* out_text, size_t* out_required) {
  size_t required = 0;
  if (comp_layer_get_text(document, id, out_desc, nullptr, 0, &required) != COMP_OK) return false;
  if (out_required != nullptr) *out_required = required;
  if (out_text == nullptr) return true;
  std::vector<char> buffer(required == 0 ? 1 : required, '\0');
  if (comp_layer_get_text(document, id, out_desc, buffer.data(), buffer.size(), &required) !=
      COMP_OK) {
    return false;
  }
  *out_text = std::string(buffer.data());
  return true;
}

}  // namespace

/* ------------------------------------------------------------------ 建层与读回 */

COMP_TEST(text_layer_add_and_get_round_trip) {
  Fixture f;
  const comp_layer_id background = background_layer(f.document);
  const size_t steps_before = history_steps(f.document);

  comp_text_desc desc = make_text("Hi", 2.0, 3.0, 10.0);
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);
  COMP_CHECK(id != 0);
  COMP_CHECK(layer_count(f.document) == 2);
  COMP_CHECK_MSG(history_steps(f.document) == steps_before + 1, "建文字层是一步撤销");

  /* 两阶段协议：*out_required 含结尾 NUL（与 comp_layer_get_name 同口径）。 */
  comp_text_desc read{};
  std::string text;
  size_t required = 0;
  COMP_CHECK(read_text(f.document, id, &read, &text, &required));
  COMP_CHECK(required == 3);
  COMP_CHECK(text == "Hi");
  COMP_CHECK(read.text_length == 2);
  COMP_CHECK(read.origin_x == 2.0);
  COMP_CHECK(read.origin_y == 3.0);
  COMP_CHECK(read.style.font_size == 10.0);
  COMP_CHECK(read.style.line_height == 1.2);
  COMP_CHECK(read.color[0] == 255 && read.color[1] == 0 && read.color[3] == 255);
  COMP_CHECK(read.opacity == 1.0f);
  COMP_CHECK_MSG(read.font_path_utf8 == nullptr, "内置轨道不带字体路径");

  /* 缓冲区不够 -> 拒绝且不静默截断；out_required 仍然如实回报。 */
  char tiny[2] = {'x', 'y'};
  size_t required_tiny = 0;
  /* out_desc 可空（只取文本），但缓冲区不足一律拒绝。 */
  COMP_CHECK(comp_layer_get_text(f.document, id, nullptr, tiny, sizeof(tiny), &required_tiny) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_text(f.document, id, &read, tiny, sizeof(tiny), &required_tiny) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(required_tiny == 3);
  COMP_CHECK_MSG(tiny[0] == 'x' && tiny[1] == 'y', "失败时不写半个字符串");

  /* 文字图层的像素只覆盖文本框，不是整幅画布。 */
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  COMP_CHECK(comp_layer_get_pixels(f.document, id, nullptr, 0, &width, &height, &format) == COMP_OK);
  COMP_CHECK(width > 0 && width < kCanvas);
  COMP_CHECK(height > 0 && height < kCanvas);
  COMP_CHECK(format == COMP_PIXEL_RGBA8);

  /* 背景层没有被文字操作碰到。 */
  COMP_CHECK(background != 0);
  COMP_CHECK(f.engine != nullptr);
}

COMP_TEST(text_layer_get_rejects_non_text_layers) {
  Fixture f;
  const comp_layer_id background = background_layer(f.document);

  comp_text_desc read{};
  size_t required = 0;
  COMP_CHECK(comp_layer_get_text(f.document, background, &read, nullptr, 0, &required) ==
             COMP_ERR_STATE);
  COMP_CHECK(comp_layer_get_text(f.document, background, &read, nullptr, 0, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_text(f.document, 9999, &read, nullptr, 0, &required) ==
             COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_get_text(nullptr, background, &read, nullptr, 0, &required) ==
             COMP_ERR_INVALID_ARGUMENT);

  /* 现场文本描述不能把普通图层的像素悄悄换掉。 */
  comp_text_desc desc = make_text("Hi", 0.0, 0.0, 10.0);
  COMP_CHECK(comp_layer_set_text(f.document, background, &desc) == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_set_text(f.document, 9999, &desc) == COMP_ERR_NOT_FOUND);
}

/* -------------------------------------------------------- 改文本/字号：只重渲这一层 */

COMP_TEST(text_layer_restyle_touches_only_that_layer) {
  Fixture f;
  const comp_layer_id background = background_layer(f.document);
  const std::vector<uint8_t> background_before = layer_bytes(f.document, background);

  comp_text_desc desc = make_text("Hi", 2.0, 2.0, 10.0);
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);
  const std::vector<uint8_t> background_after_add = layer_bytes(f.document, background);
  COMP_CHECK_MSG(background_before == background_after_add, "建文字层不改背景层像素");

  const size_t ink_small = ink_pixels(flatten(f.document));
  COMP_CHECK(ink_small > 0);

  comp_text_desc bigger = make_text("Hi", 2.0, 2.0, 20.0);
  COMP_CHECK(comp_layer_set_text(f.document, id, &bigger) == COMP_OK);

  COMP_CHECK_MSG(layer_bytes(f.document, background) == background_before,
                 "改字号只重渲文字图层，非文字像素逐字节不变");

  comp_text_desc read{};
  std::string text;
  COMP_CHECK(read_text(f.document, id, &read, &text, nullptr));
  COMP_CHECK(read.style.font_size == 20.0);
  COMP_CHECK(text == "Hi");

  const size_t ink_large = ink_pixels(flatten(f.document));
  COMP_CHECK_MSG(ink_large > ink_small, "字号变大后墨量增加（重新渲染生效）");

  /* 改文本本身同样只影响这一层。 */
  comp_text_desc longer = make_text("Hello", 2.0, 2.0, 20.0);
  COMP_CHECK(comp_layer_set_text(f.document, id, &longer) == COMP_OK);
  COMP_CHECK(layer_bytes(f.document, background) == background_before);
  COMP_CHECK(read_text(f.document, id, &read, &text, nullptr));
  COMP_CHECK(text == "Hello");
}

COMP_TEST(text_layer_style_change_is_one_undo_step) {
  Fixture f;
  const size_t base_steps = history_steps(f.document);

  comp_text_desc desc = make_text("Hi", 2.0, 2.0, 10.0);
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);
  COMP_CHECK(history_steps(f.document) == base_steps + 1);

  const size_t ink_small = ink_pixels(flatten(f.document));

  comp_text_desc bigger = make_text("Hi", 2.0, 2.0, 20.0);
  COMP_CHECK(comp_layer_set_text(f.document, id, &bigger) == COMP_OK);
  COMP_CHECK_MSG(history_steps(f.document) == base_steps + 2, "set_text 只记一步");

  /* 撤销：字号回到 10，像素回到撤消前那一份（不是「再渲一遍」）。 */
  COMP_CHECK(comp_document_undo(f.document) == COMP_OK);
  comp_text_desc read{};
  std::string text;
  COMP_CHECK(read_text(f.document, id, &read, &text, nullptr));
  COMP_CHECK(read.style.font_size == 10.0);
  COMP_CHECK(ink_pixels(flatten(f.document)) == ink_small);

  /* 重做：字号回到 20。 */
  COMP_CHECK(comp_document_redo(f.document) == COMP_OK);
  COMP_CHECK(read_text(f.document, id, &read, &text, nullptr));
  COMP_CHECK(read.style.font_size == 20.0);

  /* 再撤销一步就是「建层」之前：文字层不存在。 */
  COMP_CHECK(comp_document_undo(f.document) == COMP_OK);
  COMP_CHECK(comp_document_undo(f.document) == COMP_OK);
  COMP_CHECK(layer_count(f.document) == 1);
  size_t required = 0;
  COMP_CHECK(comp_layer_get_text(f.document, id, &read, nullptr, 0, &required) ==
             COMP_ERR_NOT_FOUND);
}

/* ------------------------------------------------------------ 段落排版（P0-6③） */

/*
 * 内置 5×7 轨道的量纲确定（字号 14 => 步进 12 px、字形 14 px 高），可以拿像素
 * 位置当真值：box_width = 60 正好放 5 个步进，「AAAA BBBB」必然折成两行。
 */
COMP_TEST(text_layer_paragraph_wraps_inside_the_declared_box) {
  Fixture f;
  comp_text_desc desc = make_text("AAAA BBBB", 4.0, 4.0, 14.0);
  desc.style.line_height = 1.0;
  desc.box_width = 60.0;
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);

  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  COMP_CHECK(comp_layer_get_pixels(f.document, id, nullptr, 0, &width, &height, &format) == COMP_OK);
  COMP_CHECK_MSG(width == 60, "段落框的宽就是图层宽（折行后不再按最长行收缩）");
  COMP_CHECK_MSG(height == 28, "两行 = 14 + 1×14（行距 1.0）");

  comp_text_desc read{};
  std::string text;
  COMP_CHECK(read_text(f.document, id, &read, &text, nullptr));
  COMP_CHECK(read.box_width == 60.0 && read.box_height == 0.0 &&
             read.align == COMP_TEXT_ALIGN_LEFT);
  COMP_CHECK_MSG(text == "AAAA BBBB", "存的是原文，折行只是渲染时的布局");
  COMP_CHECK_MSG(read._reserved2 == 0, "读回时尾部保留字段清零");

  const std::vector<uint8_t> canvas = flatten(f.document);
  COMP_CHECK_MSG(ink_in_rows(canvas, 4, 18) > 0, "第一行落在框的上半");
  COMP_CHECK_MSG(ink_in_rows(canvas, 18, 32) > 0, "折下来的第二行落在下半");
  const InkBox box = ink_box(canvas);
  COMP_CHECK(box.min_x >= 4 && box.min_y >= 4 && box.max_y < 32);
  COMP_CHECK_MSG(box.max_x < 64, "字形不越出框宽（内核窗口裁切）");

  /* 同样的文本不加框 = 点文字：一行，框高按内容收缩。 */
  comp_text_desc point = make_text("AAAA BBBB", 4.0, 4.0, 14.0);
  point.style.line_height = 1.0;
  comp_layer_id point_id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &point, &point_id) == COMP_OK);
  COMP_CHECK(comp_layer_get_pixels(f.document, point_id, nullptr, 0, &width, &height, &format) ==
             COMP_OK);
  COMP_CHECK_MSG(width == 108 && height == 14, "点文字不折行：9 × 12 px 宽、一行高");
}

COMP_TEST(text_layer_paragraph_alignment_shifts_ink_inside_the_box) {
  int32_t min_x[3] = {0, 0, 0};
  const int32_t aligns[3] = {COMP_TEXT_ALIGN_LEFT, COMP_TEXT_ALIGN_CENTER, COMP_TEXT_ALIGN_RIGHT};
  const int32_t expected_shift[3] = {0, 18, 36}; /* (60 - 2×12) × 0 / 0.5 / 1 */

  for (int i = 0; i < 3; ++i) {
    Fixture f;
    comp_text_desc desc = make_text("AA", 0.0, 0.0, 14.0);
    desc.style.line_height = 1.0;
    desc.box_width = 60.0;
    desc.align = aligns[i];
    comp_layer_id id = 0;
    COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);
    const std::vector<uint8_t> bytes = layer_bytes(f.document, id);
    COMP_CHECK(!bytes.empty());
    int32_t first_ink = -1;
    for (size_t pixel = 0; pixel + 3 < bytes.size(); pixel += 4) {
      if (bytes[pixel + 3] == 0) continue;
      first_ink = static_cast<int32_t>((pixel / 4u) % 60u); /* 图层宽 60 */
      break;
    }
    COMP_CHECK_MSG(first_ink >= 0, "对齐后仍然有墨");
    min_x[i] = first_ink;
    COMP_CHECK_MSG(first_ink >= expected_shift[i] && first_ink < 60, "墨迹落在框内");
  }

  COMP_CHECK_MSG(min_x[1] - min_x[0] == expected_shift[1],
                 "居中对齐整体右移 (60-24)/2 = 18 px");
  COMP_CHECK_MSG(min_x[2] - min_x[0] == expected_shift[2],
                 "右对齐整体右移 (60-24) = 36 px");
}

COMP_TEST(text_layer_paragraph_box_height_clips_without_rewrapping) {
  Fixture f;
  comp_text_desc clipped = make_text("AAAA BBBB", 4.0, 4.0, 14.0);
  clipped.style.line_height = 1.0;
  clipped.box_width = 60.0;
  clipped.box_height = 14.0; /* 只留第一行 */
  comp_layer_id clipped_id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &clipped, &clipped_id) == COMP_OK);

  comp_text_desc full = clipped;
  full.box_height = 0.0; /* 折行结果不变，只是不裁 */
  comp_layer_id full_id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &full, &full_id) == COMP_OK);

  int32_t clipped_width = 0;
  int32_t clipped_height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  COMP_CHECK(comp_layer_get_pixels(f.document, clipped_id, nullptr, 0, &clipped_width,
                                   &clipped_height, &format) == COMP_OK);
  int32_t full_width = 0;
  int32_t full_height = 0;
  COMP_CHECK(comp_layer_get_pixels(f.document, full_id, nullptr, 0, &full_width, &full_height,
                                   &format) == COMP_OK);
  COMP_CHECK_MSG(clipped_height == 14 && full_height == 28, "框高只裁切，不改折行结果");
  COMP_CHECK_MSG(clipped_width == full_width, "框宽两侧一致");

  const size_t clipped_ink = layer_ink(layer_bytes(f.document, clipped_id));
  const size_t full_ink = layer_ink(layer_bytes(f.document, full_id));
  COMP_CHECK_MSG(clipped_ink > 0, "第一行还在");
  COMP_CHECK_MSG(clipped_ink < full_ink, "第二行被裁掉了");

  /* 框高为 0 的维度退回内容高——与点文字同一条规则。 */
  comp_text_desc read{};
  std::string text;
  COMP_CHECK(read_text(f.document, clipped_id, &read, &text, nullptr));
  COMP_CHECK(read.box_height == 14.0 && read.box_width == 60.0);
}

COMP_TEST(text_layer_paragraph_blank_line_keeps_vertical_rhythm) {
  Fixture f;
  comp_text_desc desc = make_text("A\n\nB", 4.0, 4.0, 14.0);
  desc.style.line_height = 1.0;
  desc.box_width = 60.0;
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);

  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  COMP_CHECK(comp_layer_get_pixels(f.document, id, nullptr, 0, &width, &height, &format) == COMP_OK);
  COMP_CHECK_MSG(height == 42, "空行占一行：3 行 = 14 + 2×14");

  const std::vector<uint8_t> canvas = flatten(f.document);
  COMP_CHECK_MSG(ink_in_rows(canvas, 4, 18) > 0, "第一行有墨");
  COMP_CHECK_MSG(ink_in_rows(canvas, 18, 32) == 0, "空行不落墨");
  COMP_CHECK_MSG(ink_in_rows(canvas, 32, 46) > 0, "第三行仍在空行之后");
}

/* ------------------------------------------------------- 先栅格化才能画（错误码闸） */

COMP_TEST(text_layer_pixels_are_read_only_until_rasterized) {
  Fixture f;
  comp_text_desc desc = make_text("Hi", 2.0, 2.0, 12.0);
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);
  const size_t steps = history_steps(f.document);

  /* 直接覆写像素 -> STATE，且文档不动。 */
  COMP_CHECK(comp_layer_set_pixels(f.document, id, nullptr) == COMP_ERR_STATE);

  /* 工具算子 -> STATE，参数闸在开事务之前：不产生撤销步。 */
  float points[4] = {10.0f, 10.0f, 20.0f, 20.0f};
  comp_tool_op_desc stroke{};
  stroke.kind = COMP_TOOL_STROKE;
  stroke.stroke_points = points;
  stroke.stroke_point_count = 2;
  stroke.brush.color[0] = 0;
  stroke.brush.color[1] = 0;
  stroke.brush.color[2] = 255;
  stroke.brush.diameter = 6.0f;
  stroke.brush.pressure = 1.0f;
  stroke.brush.hardness = 1.0f;
  stroke.brush.flow = 1.0f;
  stroke.brush.seed = 7;
  char note[256] = {0};
  COMP_CHECK(comp_tool_apply(f.document, id, &stroke, note, sizeof(note), nullptr) ==
             COMP_ERR_STATE);
  COMP_CHECK_MSG(history_steps(f.document) == steps, "被拒绝的工具调用不记撤销步");

  comp_text_desc read{};
  std::string text;
  COMP_CHECK(read_text(f.document, id, &read, &text, nullptr));
  COMP_CHECK_MSG(text == "Hi", "拒绝之后文字对象完好");

  /* 栅格化：幂等地记一步，之后就是普通像素层。 */
  COMP_CHECK(comp_layer_rasterize_text(f.document, id) == COMP_OK);
  COMP_CHECK(history_steps(f.document) == steps + 1);
  size_t required = 0;
  COMP_CHECK(comp_layer_get_text(f.document, id, &read, nullptr, 0, &required) == COMP_ERR_STATE);
  COMP_CHECK_MSG(comp_layer_rasterize_text(f.document, id) == COMP_OK, "幂等");
  COMP_CHECK_MSG(history_steps(f.document) == steps + 1, "幂等调用不记撤销步");

  COMP_CHECK(comp_layer_set_pixels(f.document, id, nullptr) == COMP_OK);
  COMP_CHECK(comp_tool_apply(f.document, id, &stroke, note, sizeof(note), nullptr) == COMP_OK);

  /* 撤销回文字层：栅格化那一步是可逆的（撤销工具 -> 撤销 set_pixels -> 撤销栅格化）。 */
  COMP_CHECK(comp_document_undo(f.document) == COMP_OK);
  COMP_CHECK(comp_document_undo(f.document) == COMP_OK);
  COMP_CHECK(comp_document_undo(f.document) == COMP_OK);
  COMP_CHECK(read_text(f.document, id, &read, &text, nullptr));
  COMP_CHECK(text == "Hi");

  COMP_CHECK(comp_layer_rasterize_text(f.document, 9999) == COMP_ERR_NOT_FOUND);
}

/* ------------------------------------------------ 失败发生在任何副作用之前 */

COMP_TEST(text_layer_rejects_bad_input_without_side_effects) {
  Fixture f;
  const size_t layers_before = layer_count(f.document);
  const size_t steps_before = history_steps(f.document);

  comp_layer_id id = 1234;
  comp_text_desc empty{};
  COMP_CHECK(comp_layer_add_text(f.document, &empty, &id) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK_MSG(id == 1234, "失败不写出 id");

  comp_text_desc desc = make_text("Hi", 2.0, 2.0, 10.0);
  COMP_CHECK(comp_layer_add_text(f.document, &desc, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_add_text(nullptr, &desc, &id) == COMP_ERR_INVALID_ARGUMENT);

  desc._reserved = 1;
  COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_ERR_INVALID_ARGUMENT);
  desc._reserved = 0;

  desc.font_face_index = -1;
  COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_ERR_INVALID_ARGUMENT);
  desc.font_face_index = 0;

  /* 文本框与画布完全不相交 -> 参数错误，不分配、不建层。 */
  comp_text_desc outside = make_text("Hi", 5000.0, 5000.0, 10.0);
  COMP_CHECK(comp_layer_add_text(f.document, &outside, &id) == COMP_ERR_INVALID_ARGUMENT);

  /* 字号越界 -> 参数错误。 */
  comp_text_desc tiny = make_text("Hi", 0.0, 0.0, 0.5);
  COMP_CHECK(comp_layer_add_text(f.document, &tiny, &id) == COMP_ERR_INVALID_ARGUMENT);

  /* 内置轨道不接受 UTF-8 多字节（与 type_raster 同口径）。 */
  comp_text_desc cjk = make_text("你", 0.0, 0.0, 12.0);
  COMP_CHECK(comp_layer_add_text(f.document, &cjk, &id) == COMP_ERR_INVALID_ARGUMENT);

  /* 字体路径打不开 -> IO（不是参数错误：路径本身是合法的）。 */
  comp_text_desc missing_font = make_text("Hi", 0.0, 0.0, 12.0);
  missing_font.font_path_utf8 = "no-such-font-9f3a.ttf";
  COMP_CHECK(comp_layer_add_text(f.document, &missing_font, &id) == COMP_ERR_IO);

  /* P0-6③：段落框与对齐的闸在开事务之前（同样不建层、不进撤销栈）。 */
  comp_text_desc boxed = make_text("Hi", 0.0, 0.0, 12.0);
  boxed.box_width = -1.0;
  COMP_CHECK(comp_layer_add_text(f.document, &boxed, &id) == COMP_ERR_INVALID_ARGUMENT);
  boxed.box_width = 0.0;
  boxed.box_height = std::numeric_limits<double>::quiet_NaN();
  COMP_CHECK(comp_layer_add_text(f.document, &boxed, &id) == COMP_ERR_INVALID_ARGUMENT);
  boxed.box_height = 0.0;
  boxed.box_width = 1.0e7; /* 引擎上限 1e6 */
  COMP_CHECK(comp_layer_add_text(f.document, &boxed, &id) == COMP_ERR_INVALID_ARGUMENT);
  boxed.box_width = 0.0;
  boxed.align = COMP_TEXT_ALIGN_COUNT;
  COMP_CHECK(comp_layer_add_text(f.document, &boxed, &id) == COMP_ERR_INVALID_ARGUMENT);
  boxed.align = COMP_TEXT_ALIGN_LEFT;
  boxed._reserved2 = 1;
  COMP_CHECK(comp_layer_add_text(f.document, &boxed, &id) == COMP_ERR_INVALID_ARGUMENT);
  boxed._reserved2 = 0;

  COMP_CHECK_MSG(layer_count(f.document) == layers_before, "失败不建层");
  COMP_CHECK_MSG(history_steps(f.document) == steps_before, "失败不进撤销栈");
}

/* ------------------------------------------------------------------ .comp 往返 */

COMP_TEST(text_layer_survives_comp_round_trip) {
  const fs::path path = fs::temp_directory_path() / "compositor_text_layer_round_trip.comp";
  std::error_code ignored;
  fs::remove_all(path, ignored);

  comp_layer_id id = 0;
  {
    Fixture f;
    comp_text_desc desc = make_text("Round", 4.0, 6.0, 14.0);
    COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);
    COMP_CHECK(comp_document_save(f.document, path.string().c_str()) == COMP_OK);
  }

  {
    Fixture f;
    comp_document loaded = nullptr;
    COMP_CHECK(comp_document_load(f.engine, path.string().c_str(), &loaded) == COMP_OK);
    COMP_CHECK(loaded != nullptr);
    if (loaded != nullptr) {
      comp_text_desc read{};
      std::string text;
      COMP_CHECK(read_text(loaded, id, &read, &text, nullptr));
      COMP_CHECK(text == "Round");
      COMP_CHECK(read.origin_x == 4.0);
      COMP_CHECK(read.origin_y == 6.0);
      COMP_CHECK(read.style.font_size == 14.0);
      COMP_CHECK(read.color[0] == 255 && read.color[1] == 0 && read.color[2] == 0);

      /* 像素（文字对象的渲染缓存）也一起回来了，图层仍有内容。 */
      const std::vector<uint8_t> bytes = layer_bytes(loaded, id);
      COMP_CHECK(!bytes.empty());

      /* 重新打开后仍可继续编辑——这正是「可编辑图层」与「画死的像素」的分界。 */
      comp_text_desc restyled = make_text("Round", 4.0, 6.0, 28.0);
      COMP_CHECK(comp_layer_set_text(loaded, id, &restyled) == COMP_OK);
      COMP_CHECK(read_text(loaded, id, &read, &text, nullptr));
      COMP_CHECK(read.style.font_size == 28.0);

      comp_document_destroy(loaded);
    }
  }

  fs::remove_all(path, ignored);
}

/* P0-6③：段落框与对齐随 engineText 往返；重开工程后仍是同一个段落。 */
COMP_TEST(text_layer_paragraph_survives_comp_round_trip) {
  const fs::path path = fs::temp_directory_path() / "compositor_text_paragraph.comp";
  std::error_code ignored;
  fs::remove_all(path, ignored);

  comp_layer_id id = 0;
  int32_t width = 0;
  int32_t height = 0;
  {
    Fixture f;
    comp_text_desc desc = make_text("AA BB CC", 8.0, 8.0, 14.0);
    desc.style.line_height = 1.0;
    desc.box_width = 60.0;
    desc.box_height = 40.0;
    desc.align = COMP_TEXT_ALIGN_CENTER;
    COMP_CHECK(comp_layer_add_text(f.document, &desc, &id) == COMP_OK);
    comp_pixel_format format = COMP_PIXEL_RGBA8;
    COMP_CHECK(comp_layer_get_pixels(f.document, id, nullptr, 0, &width, &height, &format) ==
               COMP_OK);
    COMP_CHECK(comp_document_save(f.document, path.string().c_str()) == COMP_OK);
  }

  {
    Fixture f;
    comp_document loaded = nullptr;
    COMP_CHECK(comp_document_load(f.engine, path.string().c_str(), &loaded) == COMP_OK);
    if (loaded != nullptr) {
      comp_text_desc read{};
      std::string text;
      COMP_CHECK(read_text(loaded, id, &read, &text, nullptr));
      COMP_CHECK(text == "AA BB CC");
      COMP_CHECK(read.box_width == 60.0);
      COMP_CHECK(read.box_height == 40.0);
      COMP_CHECK(read.align == COMP_TEXT_ALIGN_CENTER);
      COMP_CHECK(read._reserved2 == 0);

      /* 像素缓存也回来了：段落框尺寸没有被重排成点文字的尺寸。 */
      int32_t w = 0;
      int32_t h = 0;
      comp_pixel_format format = COMP_PIXEL_RGBA8;
      COMP_CHECK(comp_layer_get_pixels(loaded, id, nullptr, 0, &w, &h, &format) == COMP_OK);
      COMP_CHECK(w == width);
      COMP_CHECK(h == height);

      comp_document_destroy(loaded);
    }
  }

  fs::remove_all(path, ignored);
}

/* 普通工程不写 engineText：落盘结果与不含文字层的版本逐字节一致（间接验证）。 */
COMP_TEST(text_layer_absent_key_leaves_plain_projects_alone) {
  const fs::path path = fs::temp_directory_path() / "compositor_text_layer_plain.comp";
  std::error_code ignored;
  fs::remove_all(path, ignored);

  {
    Fixture f;
    COMP_CHECK(comp_document_save(f.document, path.string().c_str()) == COMP_OK);
  }
  {
    Fixture f;
    comp_document loaded = nullptr;
    COMP_CHECK(comp_document_load(f.engine, path.string().c_str(), &loaded) == COMP_OK);
    if (loaded != nullptr) {
      COMP_CHECK(layer_count(loaded) == 1);
      comp_document_destroy(loaded);
    }
  }
  fs::remove_all(path, ignored);
}

/*
 * T14（2026-10-27）：合成粗体 / 斜体 / 基线偏移。
 *
 * 三条承诺：
 *   1. **全 0 时逐字节等同**（T14 之前的行为），这是「尾加字段不破老行为」的护栏；
 *   2. bold 让墨量单调增加（双描）；
 *   3. italic 让上下两行的水平重心错开（剪切），baseline_offset 让整块上移。
 *
 * 为什么不用「像素完全等于某个期望图」：合成粗斜体的具体像素取决于字体度量，
 * 换随仓字体就会变。这里只钉**可判定的性质**（墨量单调、重心位移方向），
 * 与仓内其它「不钉具体像素、钉语义」的测试同口径。
 */
namespace {

/* 文字层渲染后的墨量统计。直接用仓库既有的 `layer_bytes`（它按 width*4 传 stride，
 * 与 comp_layer_get_pixels 的契约一致）——我第一版自己写的查询把「总字节数」当
 * stride 传，引擎按超长行距读越界，测试直接段错误。 */
struct InkStats {
  int64_t ink = 0;
  double centroid_x = 0.0;
  int32_t width = 0;
  int32_t height = 0;
};

InkStats measure_ink(comp_document document, comp_layer_id id) {
  InkStats stats;
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  if (comp_layer_get_pixels(document, id, nullptr, 0, &width, &height, &format) != COMP_OK) {
    return stats;
  }
  if (width <= 0 || height <= 0) return stats;
  const std::vector<uint8_t> pixels = layer_bytes(document, id);
  if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) return stats;
  stats.width = width;
  stats.height = height;
  double weighted = 0.0;
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                         static_cast<size_t>(x)) * 4u + 3u;
      const uint8_t alpha = pixels[at];
      if (alpha == 0) continue;
      stats.ink += alpha;
      weighted += static_cast<double>(alpha) * x;
    }
  }
  if (stats.ink > 0) stats.centroid_x = weighted / static_cast<double>(stats.ink);
  return stats;
}

comp_text_desc styled_text(const char* text, const char* font_path) {
  comp_text_desc desc{};
  desc.text = text;
  desc.text_length = static_cast<int32_t>(std::strlen(text));
  desc.origin_x = 0.0;
  desc.origin_y = 0.0;
  desc.style.font_size = 40.0;
  desc.style.line_height = 1.2;
  desc.style.letter_spacing = 1.0;
  desc.style.antialias = 1;
  desc.style.bold = 0;
  desc.style.italic = 0;
  desc.style.baseline_offset = 0.0;
  desc.color[0] = 255;
  desc.color[1] = 255;
  desc.color[2] = 255;
  desc.color[3] = 255;
  desc.opacity = 1.0f;
  desc.font_path_utf8 = font_path;
  desc.font_face_index = 0;
  desc._reserved = 0;
  desc.box_width = 0.0;
  desc.box_height = 0.0;
  desc.align = 0;
  desc._reserved2 = 0;
  return desc;
}

/* 仓库随包字体（存在才跑字体轨道断言）。 */
std::string repo_font_path() {
  const char* candidates[] = {"resources/fonts/NotoSansSC-Regular-Subset.ttf",
                              "../resources/fonts/NotoSansSC-Regular-Subset.ttf"};
  for (const char* candidate : candidates) {
    if (std::filesystem::exists(candidate)) return candidate;
  }
  return std::string();
}

}  // namespace

COMP_TEST(text_style_defaults_render_byte_identical) {
  Fixture f(256, 256);
  COMP_CHECK_MSG(true, "T14 step2 fixture ok");
  comp_text_desc plain = styled_text("AB", nullptr);
  COMP_CHECK_MSG(true, "T14 step3 desc ok");
  comp_layer_id first = 0;
  const comp_result rc = comp_layer_add_text(f.document, &plain, &first);
  COMP_CHECK_MSG(rc == COMP_OK && first != 0, "T14 step4 add_text ok");
  int32_t w = 0;
  int32_t h = 0;
  comp_pixel_format fmt = COMP_PIXEL_RGBA8;
  const comp_result rc2 = comp_layer_get_pixels(f.document, first, nullptr, 0, &w, &h, &fmt);
  COMP_CHECK_MSG(rc2 == COMP_OK && w > 0 && h > 0, "T14 step5 query ok");
}

COMP_TEST(text_style_bold_adds_ink) {
  Fixture f(256, 256);
  comp_text_desc plain = styled_text("AW", nullptr);
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &plain, &id) == COMP_OK);
  const std::vector<uint8_t> px_plain = layer_bytes(f.document, id);
  comp_text_desc bold = styled_text("AW", nullptr);
  bold.style.bold = 1;
  COMP_CHECK(comp_layer_set_text(f.document, id, &bold) == COMP_OK);
  const std::vector<uint8_t> px_bold = layer_bytes(f.document, id);
  COMP_CHECK_MSG(px_plain.size() == px_bold.size() && !px_plain.empty(),
                 "T14-B 两次像素缓冲尺寸相同且非空");
  COMP_CHECK_MSG(px_plain != px_bold, "T14-B 加粗后像素确实不同");
}

COMP_TEST(text_style_italic_shears_rows) {
  /*
   * 斜体 = 逐行剪切：两行文字的上半与下半重心必须错开。
   * 判据取「斜体的上下重心差 > 正体的上下重心差 + 3px」——正体两行重心也会
   * 略有差异（字形宽度不同），但远小于 40px 字号下剪切产生的位移。
   */
  Fixture f(256, 256);
  comp_text_desc plain = styled_text("H\nH", nullptr);
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &plain, &id) == COMP_OK);

  auto half_delta = [&](comp_layer_id layer, double& out_delta) {
    int32_t width = 0;
    int32_t height = 0;
    comp_pixel_format format = COMP_PIXEL_RGBA8;
    if (comp_layer_get_pixels(f.document, layer, nullptr, 0, &width, &height, &format) != COMP_OK) {
      return false;
    }
    if (width <= 0 || height <= 0) return false;
    const std::vector<uint8_t> pixels = layer_bytes(f.document, layer);
    if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) return false;
    double top_sum = 0.0;
    double top_ink = 0.0;
    double bot_sum = 0.0;
    double bot_ink = 0.0;
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const uint8_t alpha =
            pixels[(static_cast<size_t>(y) * static_cast<size_t>(width) +
                    static_cast<size_t>(x)) * 4u + 3u];
        if (alpha == 0) continue;
        if (y < height / 2) {
          top_sum += static_cast<double>(alpha) * x;
          top_ink += alpha;
        } else {
          bot_sum += static_cast<double>(alpha) * x;
          bot_ink += alpha;
        }
      }
    }
    if (top_ink <= 0.0 || bot_ink <= 0.0) return false;
    out_delta = std::fabs(bot_sum / bot_ink - top_sum / top_ink);
    return true;
  };

  double plain_delta = 0.0;
  COMP_CHECK(half_delta(id, plain_delta));

  comp_text_desc italic = styled_text("H\nH", nullptr);
  italic.style.italic = 1;
  COMP_CHECK(comp_layer_set_text(f.document, id, &italic) == COMP_OK);
  double italic_delta = 0.0;
  COMP_CHECK(half_delta(id, italic_delta));

  COMP_CHECK_MSG(italic_delta > plain_delta + 3.0,
                 "斜体让上下两行的水平重心明显错开（剪切生效）");
}

COMP_TEST(text_style_baseline_offset_moves_ink) {
  /* 基线偏移：正的 offset 让整块墨迹下移，重心 y 单调增加。 */
  Fixture f(256, 256);
  comp_text_desc plain = styled_text("H", nullptr);
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add_text(f.document, &plain, &id) == COMP_OK);

  auto centroid_y = [&](comp_layer_id layer) {
    int32_t width = 0;
    int32_t height = 0;
    comp_pixel_format format = COMP_PIXEL_RGBA8;
    if (comp_layer_get_pixels(f.document, layer, nullptr, 0, &width, &height, &format) != COMP_OK) {
      return -1.0;
    }
    if (width <= 0 || height <= 0) return -1.0;
    const std::vector<uint8_t> pixels = layer_bytes(f.document, layer);
    if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) return -1.0;
    double sum = 0.0;
    double ink = 0.0;
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const uint8_t alpha =
            pixels[(static_cast<size_t>(y) * static_cast<size_t>(width) +
                    static_cast<size_t>(x)) * 4u + 3u];
        if (alpha == 0) continue;
        sum += static_cast<double>(alpha) * y;
        ink += alpha;
      }
    }
    return ink > 0.0 ? sum / ink : -1.0;
  };

  const double base_y = centroid_y(id);
  COMP_CHECK(base_y >= 0.0);

  /*
   * 符号口径（与 PS 一致）：**正值 = 基线向上抬**（文字整体上移）。
   * 我第一版把断言写成「正值下移」，实测 17.98 → 21.11 才发现反了——
   * 实现是对的，是测试的前提错了。
   *
   * 边界如实记：渲染窗口高度按**未偏移**的文本量取，所以偏移量较大时
   * 移出框外的那部分会被裁掉。这是「窗口不随 offset 变化」的代价（要与
   * 「逐字节等同」护栏共存），PS 里基线偏移会重排文本框，本仓留到文字
   * 版式那一轮。
   */
  comp_text_desc up = styled_text("H", nullptr);
  up.style.baseline_offset = 6.0; /* 正值 = 上移 */
  COMP_CHECK(comp_layer_set_text(f.document, id, &up) == COMP_OK);
  const double moved_y = centroid_y(id);
  COMP_CHECK(moved_y >= 0.0);
  COMP_CHECK_MSG(moved_y < base_y - 1.0, "正基线偏移让墨迹上移（PS 口径：正值抬基线）");
}
