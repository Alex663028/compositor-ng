// AI生成
/*
 * test_crop.cpp — Crop 工具（F2，对齐上游 Crop.swift + CanvasResizer.swift）。
 *
 * 覆盖：取整、拖框（方向 / 比例 / Option 对称）、切换比例、合法性、
 *       提交（图层平移 + 换画布 + 丢弃选区 + 一步撤销）、负原点扩画布、
 *       无操作短路、非法输入无副作用、显式事务分组、Canvas Size 丢选区。
 *
 * 判据是端到端事实：合成像素、文档尺寸、撤销深度、选区状态——不是
 * 「API 返回 COMP_OK」。像素探针用带 x/y 编码的 NEAREST 图层，任何位移都用
 * 逐字节比较钉死。
 */
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_crop.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_selection.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

comp_engine_desc crop_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-crop-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

/* 把 x/y 编进颜色的探针图：任何平移/翻转/缩放都会改变字节。alpha = 255，
 * 预乘后与源值相同，合成结果是精确值而非近似值。 */
struct RampImage {
  std::vector<uint8_t> bytes;
  comp_image image{};

  RampImage(int32_t width, int32_t height)
      : bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                          static_cast<size_t>(x)) * 4u;
        bytes[i] = static_cast<uint8_t>((x * 7 + 3) & 0xFF);
        bytes[i + 1] = static_cast<uint8_t>((y * 11 + 5) & 0xFF);
        bytes[i + 2] = static_cast<uint8_t>((x * 5 + y * 3) & 0xFF);
        bytes[i + 3] = 255;
      }
    }
    image.width = width;
    image.height = height;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = static_cast<size_t>(width) * 4u;
    image.pixels = bytes.data();
  }
};

/* 加一个 NEAREST 探针图层：像素与 transform 一一对应，比较可以逐字节做。 */
comp_layer_id add_ramp_layer(comp_document document, comp_rect transform) {
  comp_layer_desc desc{};
  desc.name = "ramp";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = transform;
  desc.sampling = COMP_SAMPLING_NEAREST;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  const RampImage image(transform.w, transform.h);
  if (comp_layer_set_pixels(document, id, &image.image) != COMP_OK) return 0;
  return id;
}

std::vector<uint8_t> render_rgba(comp_document document) {
  comp_size size{};
  if (comp_document_size(document, &size) != COMP_OK) return {};
  std::vector<uint8_t> pixels(static_cast<size_t>(size.width) *
                                  static_cast<size_t>(size.height) * 4u,
                              0u);
  if (comp_render_flatten(document, pixels.data(), COMP_PIXEL_RGBA8,
                          static_cast<size_t>(size.width) * 4u) != COMP_OK) {
    return {};
  }
  return pixels;
}

size_t undo_depth(comp_document document) {
  size_t depth = 0;
  if (comp_document_history_stats(document, &depth, nullptr) != COMP_OK) return 0;
  return depth;
}

/* part 是否等于 full 在 (offset_x, offset_y) 处的同尺寸窗口。 */
bool sub_rect_matches(const std::vector<uint8_t>& full, int32_t full_width, int32_t full_height,
                      const std::vector<uint8_t>& part, int32_t part_width, int32_t part_height,
                      int32_t offset_x, int32_t offset_y) {
  if (offset_x < 0 || offset_y < 0) return false;
  if (offset_x + part_width > full_width || offset_y + part_height > full_height) return false;
  if (full.size() != static_cast<size_t>(full_width) * static_cast<size_t>(full_height) * 4u) {
    return false;
  }
  if (part.size() != static_cast<size_t>(part_width) * static_cast<size_t>(part_height) * 4u) {
    return false;
  }
  for (int32_t y = 0; y < part_height; ++y) {
    const uint8_t* source =
        full.data() + (static_cast<size_t>(y + offset_y) * static_cast<size_t>(full_width) +
                       static_cast<size_t>(offset_x)) * 4u;
    const uint8_t* target = part.data() + static_cast<size_t>(y) *
                                              static_cast<size_t>(part_width) * 4u;
    if (std::memcmp(source, target, static_cast<size_t>(part_width) * 4u) != 0) return false;
  }
  return true;
}

}  // namespace

/* ------------------------------------------------------------ 取整 */

COMP_TEST(crop_snap_rounds_each_edge_and_enforces_minimum_size) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_crop_rect out{};
  /* 四条边各自取整：0.4 -> 0，10.6 -> 11，0.6 -> 1，6.2 -> 6。 */
  COMP_CHECK(comp_crop_snap_rect(engine, comp_crop_rect{0.4, 0.6, 10.2, 5.6}, &out) == COMP_OK);
  COMP_CHECK(out.x == 0.0 && out.y == 1.0 && out.width == 11.0 && out.height == 5.0);

  /* .rounded() 是「半边远离零」：2.5 -> 3，-2.5 -> -3。 */
  COMP_CHECK(comp_crop_snap_rect(engine, comp_crop_rect{2.5, -2.5, 1.0, 1.0}, &out) == COMP_OK);
  COMP_CHECK(out.x == 3.0 && out.y == -3.0 && out.width == 1.0 && out.height == 1.0);

  /* 反向拖拽（负宽高）先标准化。 */
  COMP_CHECK(comp_crop_snap_rect(engine, comp_crop_rect{10.0, 10.0, -4.0, -3.0}, &out) == COMP_OK);
  COMP_CHECK(out.x == 6.0 && out.y == 7.0 && out.width == 4.0 && out.height == 3.0);

  /* 零尺寸至少 1 像素（上游 max(1, ...)）。 */
  COMP_CHECK(comp_crop_snap_rect(engine, comp_crop_rect{5.0, 5.0, 0.0, 0.0}, &out) == COMP_OK);
  COMP_CHECK(out.x == 5.0 && out.y == 5.0 && out.width == 1.0 && out.height == 1.0);

  /* 非有限输入被拒绝，输出不被写。 */
  comp_crop_rect untouched{1.0, 2.0, 3.0, 4.0};
  const double nan = std::numeric_limits<double>::quiet_NaN();
  COMP_CHECK(comp_crop_snap_rect(engine, comp_crop_rect{nan, 0.0, 4.0, 4.0}, &untouched) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(untouched.x == 1.0 && untouched.y == 2.0 && untouched.width == 3.0 &&
             untouched.height == 4.0);
  COMP_CHECK(comp_crop_snap_rect(engine, comp_crop_rect{1.0e8, 0.0, 4.0, 4.0}, &untouched) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_crop_snap_rect(engine, comp_crop_rect{0.0, 0.0, 4.0, 4.0}, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);

  comp_engine_destroy(engine);
}

/* ------------------------------------------------------------ 拖框 */

COMP_TEST(crop_create_frame_follows_drag_ratio_and_symmetry) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_crop_rect out{};
  /* 自由拖拽：(10,20) -> (30,50) = 20 x 30。 */
  COMP_CHECK(comp_crop_create_frame(engine, 10.0, 20.0, 30.0, 50.0, 0.0, 0, &out) == COMP_OK);
  COMP_CHECK(out.x == 10.0 && out.y == 20.0 && out.width == 20.0 && out.height == 30.0);

  /* 反向拖拽得到同一个框。 */
  COMP_CHECK(comp_crop_create_frame(engine, 30.0, 50.0, 10.0, 20.0, 0.0, 0, &out) == COMP_OK);
  COMP_CHECK(out.x == 10.0 && out.y == 20.0 && out.width == 20.0 && out.height == 30.0);

  /* 比例 2:1，宽度占优：dx = 40、dy = 10 -> 高度拉到 20。 */
  COMP_CHECK(comp_crop_create_frame(engine, 0.0, 0.0, 40.0, 10.0, 2.0, 0, &out) == COMP_OK);
  COMP_CHECK(out.x == 0.0 && out.y == 0.0 && out.width == 40.0 && out.height == 20.0);

  /* 比例 2:1，高度占优：dy = 10 -> 宽度拉到 20。 */
  COMP_CHECK(comp_crop_create_frame(engine, 0.0, 0.0, 4.0, 10.0, 2.0, 0, &out) == COMP_OK);
  COMP_CHECK(out.x == 0.0 && out.y == 0.0 && out.width == 20.0 && out.height == 10.0);

  /* Option 对称：以起点为中心展开。 */
  COMP_CHECK(comp_crop_create_frame(engine, 50.0, 50.0, 60.0, 53.0, 0.0, 1, &out) == COMP_OK);
  COMP_CHECK(out.x == 40.0 && out.y == 47.0 && out.width == 20.0 && out.height == 6.0);

  /* 非有限坐标/比例被拒绝。 */
  const double nan = std::numeric_limits<double>::quiet_NaN();
  COMP_CHECK(comp_crop_create_frame(engine, nan, 0.0, 10.0, 10.0, 0.0, 0, &out) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_crop_create_frame(engine, 0.0, 0.0, 10.0, 10.0, nan, 0, &out) ==
             COMP_ERR_INVALID_ARGUMENT);

  comp_engine_destroy(engine);
}

/* ------------------------------------------------------------ 比例 */

COMP_TEST(crop_change_ratio_keeps_width_and_centers_vertically) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  const comp_crop_rect current{10.0, 10.0, 40.0, 20.0};
  comp_crop_rect out{};
  int32_t changed = -1;

  /* 2:1 恰好是当前形状：不变。 */
  COMP_CHECK(comp_crop_change_ratio(engine, current, 2.0, &out, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(out.x == 10.0 && out.y == 10.0 && out.width == 40.0 && out.height == 20.0);

  /* 4:1：高度 10，垂直居中 -> y = 20 - 5 = 15。 */
  COMP_CHECK(comp_crop_change_ratio(engine, current, 4.0, &out, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(out.x == 10.0 && out.y == 15.0 && out.width == 40.0 && out.height == 10.0);

  /* 1:1：高度 40，居中 -> y = 0。 */
  COMP_CHECK(comp_crop_change_ratio(engine, current, 1.0, &out, &changed) == COMP_OK);
  COMP_CHECK(out.x == 10.0 && out.y == 0.0 && out.width == 40.0 && out.height == 40.0);

  /* 5:1：高度 8，居中 -> y = 16。 */
  COMP_CHECK(comp_crop_change_ratio(engine, current, 5.0, &out, &changed) == COMP_OK);
  COMP_CHECK(out.x == 10.0 && out.y == 16.0 && out.width == 40.0 && out.height == 8.0);

  /* 非正 / 非有限比例被拒绝（上游比例来自 UI，恒为正）。 */
  COMP_CHECK(comp_crop_change_ratio(engine, current, 0.0, &out, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_crop_change_ratio(engine, current, -2.0, &out, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_crop_change_ratio(engine, current,
                                    std::numeric_limits<double>::quiet_NaN(), &out, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_crop_change_ratio(engine, current, 2.0, nullptr, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);

  comp_engine_destroy(engine);
}

/* ------------------------------------------------------------ 合法性 */

COMP_TEST(crop_is_valid_matches_commit_rules) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  int32_t valid = -1;
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{0.0, 0.0, 100.0, 100.0}, &valid) == COMP_OK);
  COMP_CHECK(valid == 1);

  /* 单边上限 30000。 */
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{0.0, 0.0, 30000.0, 3000.0}, &valid) ==
             COMP_OK);
  COMP_CHECK(valid == 1);
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{0.0, 0.0, 30001.0, 10.0}, &valid) == COMP_OK);
  COMP_CHECK(valid == 0);

  /* 原点上限 1e6。 */
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{-1000000.0, 0.0, 10.0, 10.0}, &valid) ==
             COMP_OK);
  COMP_CHECK(valid == 1);
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{1000001.0, 0.0, 10.0, 10.0}, &valid) ==
             COMP_OK);
  COMP_CHECK(valid == 0);

  /* 像素预算（本仓 1 亿，上游 2 亿）：10000^2 恰好在上限，30000^2 超限。 */
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{0.0, 0.0, 10000.0, 10000.0}, &valid) ==
             COMP_OK);
  COMP_CHECK(valid == 1);
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{0.0, 0.0, 30000.0, 30000.0}, &valid) ==
             COMP_OK);
  COMP_CHECK(valid == 0);
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{0.0, 0.0, 20000.0, 6000.0}, &valid) ==
             COMP_OK);
  COMP_CHECK(valid == 0);

  /* 非有限输入 = 不可提交，但这是查询，不是错误。 */
  const double nan = std::numeric_limits<double>::quiet_NaN();
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{nan, 0.0, 10.0, 10.0}, &valid) == COMP_OK);
  COMP_CHECK(valid == 0);
  COMP_CHECK(comp_crop_is_valid(engine, comp_crop_rect{0.0, 0.0, 10.0, 10.0}, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);

  comp_engine_destroy(engine);
}

/* ------------------------------------------------------------ 提交 */

COMP_TEST(crop_translates_layers_and_resizes_canvas) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_ramp_layer(document, comp_rect{0, 0, 16, 16}) != 0);

  const std::vector<uint8_t> before = render_rgba(document);
  COMP_CHECK(before.size() == 16u * 16u * 4u);

  const size_t depth_before = undo_depth(document);
  int32_t changed = -1;
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{4.0, 6.0, 8.0, 8.0}, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth(document) == depth_before + 1);

  comp_size size{};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 8 && size.height == 8);

  /* 裁剪不重采样：新画布逐字节等于原图 (4,6) 处的 8x8 窗口。 */
  const std::vector<uint8_t> cropped = render_rgba(document);
  COMP_CHECK_MSG(sub_rect_matches(before, 16, 16, cropped, 8, 8, 4, 6),
                 "cropped render equals the same sub-rect of the original");

  /* 一步撤销：尺寸与像素一起回来；重做再裁剪一次。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 16 && size.height == 16);
  COMP_CHECK(render_rgba(document) == before);

  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 8 && size.height == 8);
  COMP_CHECK(render_rgba(document) == cropped);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(crop_to_negative_origin_extends_canvas_with_transparent_border) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_ramp_layer(document, comp_rect{0, 0, 16, 16}) != 0);

  const std::vector<uint8_t> before = render_rgba(document);
  int32_t changed = 0;
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{-2.0, -2.0, 20.0, 20.0}, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);

  comp_size size{};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 20 && size.height == 20);

  const std::vector<uint8_t> after = render_rgba(document);
  COMP_CHECK(after.size() == 20u * 20u * 4u);
  /* 内容右下平移 2 像素，新增的左上/右下边缘是透明画布。
   * sub_rect_matches 的第一个参数是「被裁窗口的大图」，所以这里把裁剪后的
   * 20x20 画布当容器、原来的 16x16 内容当偏移 (2,2) 处的窗口。 */
  COMP_CHECK_MSG(sub_rect_matches(after, 20, 20, before, 16, 16, 2, 2),
                 "existing pixels keep their content, shifted by the crop origin");
  COMP_CHECK(after[0] == 0 && after[1] == 0 && after[2] == 0 && after[3] == 0);
  const size_t last = (19u * 20u + 19u) * 4u;
  COMP_CHECK(after[last] == 0 && after[last + 3] == 0);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(crop_is_non_destructive_and_restores_by_expanding_back) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_ramp_layer(document, comp_rect{0, 0, 16, 16}) != 0);

  const std::vector<uint8_t> before = render_rgba(document);

  int32_t changed = 0;
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{4.0, 6.0, 8.0, 8.0}, &changed) == COMP_OK);
  /* 第二次向负方向扩回来：裁掉的像素从未被销毁，应逐字节还原。 */
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{-4.0, -6.0, 16.0, 16.0}, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);

  comp_size size{};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 16 && size.height == 16);
  COMP_CHECK_MSG(render_rgba(document) == before,
                 "crop keeps layer pixels; expanding back restores the original image");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(crop_noop_does_not_add_history) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_ramp_layer(document, comp_rect{0, 0, 16, 16}) != 0);

  const std::vector<uint8_t> before = render_rgba(document);
  const size_t depth_before = undo_depth(document);
  int32_t changed = -1;
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{0.0, 0.0, 16.0, 16.0}, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK_MSG(undo_depth(document) == depth_before,
                 "cropping to the current canvas adds no undo step");
  COMP_CHECK(render_rgba(document) == before);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(crop_drops_selection_in_the_same_undo_step) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const float points[4] = {4.0f, 4.0f, 12.0f, 12.0f};
  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_shape(document, COMP_SELECTION_RECT, points, 2,
                                        COMP_SELECTION_REPLACE, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  int32_t active = 0;
  COMP_CHECK(comp_selection_get_active(document, &active) == COMP_OK);
  COMP_CHECK(active == 1);

  const size_t depth_before = undo_depth(document);
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{4.0, 4.0, 8.0, 8.0}, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth(document) == depth_before + 1);
  COMP_CHECK_MSG(comp_selection_get_active(document, &active) == COMP_OK && active == 0,
                 "crop drops the selection (new canvas = new coordinate system)");

  /* 撤销一步：画布与选区一起回来。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  comp_size size{};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 16 && size.height == 16);
  COMP_CHECK(comp_selection_get_active(document, &active) == COMP_OK && active == 1);
  comp_rect bounds{};
  COMP_CHECK(comp_selection_get_bounds(document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.x == 4 && bounds.y == 4 && bounds.w == 8 && bounds.h == 8);

  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(comp_selection_get_active(document, &active) == COMP_OK && active == 0);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(crop_rejects_invalid_inputs_without_side_effects) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_ramp_layer(document, comp_rect{0, 0, 16, 16}) != 0);

  const std::vector<uint8_t> before = render_rgba(document);
  const size_t depth_before = undo_depth(document);
  int32_t changed = 7;

  /* 单边超限、像素预算超限、原点超限、非有限坐标。 */
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{0.0, 0.0, 30001.0, 10.0}, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{0.0, 0.0, 20000.0, 6000.0}, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{1000001.0, 0.0, 16.0, 16.0}, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_crop_apply(document,
                             comp_crop_rect{std::numeric_limits<double>::quiet_NaN(), 0.0, 16.0,
                                            16.0},
                             &changed) == COMP_ERR_INVALID_ARGUMENT);

  comp_size size{};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 16 && size.height == 16);
  COMP_CHECK(undo_depth(document) == depth_before);
  COMP_CHECK(render_rgba(document) == before);

  /* 图层 transform 平移后越界：整次裁剪被拒绝，文档一个字段都不动。 */
  COMP_CHECK(add_ramp_layer(document, comp_rect{1000000, 0, 4, 4}) != 0);
  const size_t depth_with_far_layer = undo_depth(document);
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{-1000000.0, 0.0, 16.0, 16.0}, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 16 && size.height == 16);
  COMP_CHECK(undo_depth(document) == depth_with_far_layer);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(crop_transactions_group_and_roll_back) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_ramp_layer(document, comp_rect{0, 0, 16, 16}) != 0);

  const std::vector<uint8_t> before = render_rgba(document);
  const size_t depth_before = undo_depth(document);
  int32_t changed = 0;

  /* 显式事务内的两次裁剪合成一步。 */
  comp_txn txn = nullptr;
  COMP_CHECK(comp_txn_begin(document, "Crop batch", &txn) == COMP_OK);
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{2.0, 2.0, 12.0, 12.0}, &changed) == COMP_OK);
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{0.0, 0.0, 8.0, 8.0}, &changed) == COMP_OK);
  COMP_CHECK_MSG(undo_depth(document) == depth_before,
                 "commits inside an explicit transaction are suppressed");
  COMP_CHECK(comp_txn_commit(txn) == COMP_OK);
  COMP_CHECK(undo_depth(document) == depth_before + 1);

  comp_size size{};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 8 && size.height == 8);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 16 && size.height == 16);
  COMP_CHECK(render_rgba(document) == before);

  /* abort 回滚：尺寸复原，且不产生历史。 */
  const size_t depth_after_undo = undo_depth(document);
  COMP_CHECK(comp_txn_begin(document, "Crop abort", &txn) == COMP_OK);
  COMP_CHECK(comp_crop_apply(document, comp_crop_rect{0.0, 0.0, 8.0, 8.0}, &changed) == COMP_OK);
  COMP_CHECK(comp_txn_abort(txn) == COMP_OK);
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 16 && size.height == 16);
  COMP_CHECK(undo_depth(document) == depth_after_undo);
  COMP_CHECK(render_rgba(document) == before);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* -------------------------------------------------- Canvas Size 同口径 */

COMP_TEST(canvas_size_change_drops_selection) {
  comp_engine_desc desc = crop_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const float points[4] = {2.0f, 2.0f, 10.0f, 10.0f};
  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_shape(document, COMP_SELECTION_RECT, points, 2,
                                        COMP_SELECTION_REPLACE, 1, &changed) == COMP_OK);

  int32_t active = 0;
  COMP_CHECK(comp_document_set_size(document, comp_size{8, 8}) == COMP_OK);
  COMP_CHECK_MSG(comp_selection_get_active(document, &active) == COMP_OK && active == 0,
                 "canvas size change drops the selection");

  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_selection_get_active(document, &active) == COMP_OK && active == 1);

  /* 尺寸没变时不动选区。 */
  COMP_CHECK(comp_document_set_size(document, comp_size{16, 16}) == COMP_OK);
  COMP_CHECK(comp_selection_get_active(document, &active) == COMP_OK && active == 1);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
