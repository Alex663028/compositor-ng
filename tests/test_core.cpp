// AI生成
/*
 * test_core.cpp — core 层：几何、分配器、并行原语、JSON。
 */
#include <string>
#include <vector>

#include "core/comp_alloc.h"
#include "core/comp_geometry.h"
#include "core/comp_parallel.h"
#include "format/comp_json.h"
#include "test_framework.h"

using namespace compositor;

COMP_TEST(geometry_intersection_and_clamp) {
  const comp_rect a{0, 0, 10, 10};
  const comp_rect b{5, 5, 10, 10};
  const comp_rect hit = intersection(a, b);
  COMP_CHECK(hit.x == 5 && hit.y == 5 && hit.w == 5 && hit.h == 5);
  COMP_CHECK(is_empty(intersection(a, comp_rect{20, 20, 4, 4})));
  COMP_CHECK(area(a) == 100);
  COMP_CHECK(contains(a, comp_rect{2, 2, 3, 3}));

  const comp_rect clamped = clamp_to_canvas(comp_rect{-4, -4, 8, 8}, comp_size{6, 6});
  COMP_CHECK(clamped.x == 0 && clamped.y == 0 && clamped.w == 4 && clamped.h == 4);
}

COMP_TEST(geometry_canvas_limits) {
  COMP_CHECK(is_valid_canvas(comp_size{1920, 1080}));
  COMP_CHECK(!is_valid_canvas(comp_size{0, 100}));
  COMP_CHECK(!is_valid_canvas(comp_size{-1, 100}));
  COMP_CHECK(!is_valid_canvas(comp_size{30001, 1}));
  /* 边在限内但像素数超 1 亿 → 非法 */
  COMP_CHECK(!is_valid_canvas(comp_size{30000, 30000}));

  COMP_CHECK(bytes_per_pixel(COMP_PIXEL_RGBA8) == 4);
  COMP_CHECK(bytes_per_pixel(COMP_PIXEL_RGBA16) == 8);
  COMP_CHECK(bytes_per_pixel(COMP_PIXEL_RGBA16F) == 8);
  COMP_CHECK(bytes_per_pixel(static_cast<comp_pixel_format>(99)) == 0);
}

namespace {

struct CountingAllocator {
  int allocations = 0;
  int releases = 0;
};

void* counting_alloc(size_t bytes, void* user) {
  ++static_cast<CountingAllocator*>(user)->allocations;
  return std::malloc(bytes);
}

void counting_free(void* pointer, void* user) {
  ++static_cast<CountingAllocator*>(user)->releases;
  std::free(pointer);
}

}  // namespace

COMP_TEST(allocator_default_and_custom) {
  Allocator default_allocator;
  COMP_CHECK(!default_allocator.is_custom());

  void* block = default_allocator.allocate_zeroed(64);
  COMP_CHECK(block != nullptr);
  const unsigned char* bytes = static_cast<const unsigned char*>(block);
  bool zeroed = true;
  for (int i = 0; i < 64; ++i) {
    if (bytes[i] != 0) zeroed = false;
  }
  COMP_CHECK(zeroed);
  default_allocator.release(block);

  CountingAllocator counting;
  comp_allocator desc{};
  desc.alloc = counting_alloc;
  desc.free = counting_free;
  desc.user = &counting;

  Allocator custom(desc);
  COMP_CHECK(custom.is_custom());
  void* second = custom.allocate(32);
  custom.release(second);
  COMP_CHECK(counting.allocations == 1);
  COMP_CHECK(counting.releases == 1);
}

namespace {

struct RangeProbe {
  std::vector<int> hits;
};

void mark_range(void* ctx, size_t begin, size_t end) {
  RangeProbe* probe = static_cast<RangeProbe*>(ctx);
  for (size_t i = begin; i < end; ++i) probe->hits[i] += 1;
}

}  // namespace

COMP_TEST(parallel_ranges_cover_exactly_once) {
  const size_t count = 257;

  const size_t serial = comp_parallel_set_threads(1);
  COMP_CHECK(serial == 1);

  RangeProbe serial_probe;
  serial_probe.hits.assign(count, 0);
  comp_parallel_ranges(count, mark_range, &serial_probe);
  for (size_t i = 0; i < count; ++i) COMP_CHECK(serial_probe.hits[i] == 1);

  comp_parallel_set_threads(4);
  RangeProbe parallel_probe;
  parallel_probe.hits.assign(count, 0);
  comp_parallel_ranges(count, mark_range, &parallel_probe);
  /* 区间必须互不重叠且并集恰好覆盖 [0, count)：每个元素命中一次。 */
  for (size_t i = 0; i < count; ++i) COMP_CHECK(parallel_probe.hits[i] == 1);

  /* 空区间不得触发回调。 */
  RangeProbe empty_probe;
  empty_probe.hits.assign(4, 0);
  comp_parallel_ranges(0, mark_range, &empty_probe);
  for (int i = 0; i < 4; ++i) COMP_CHECK(empty_probe.hits[i] == 0);

  comp_parallel_set_threads(0);
  COMP_CHECK(comp_parallel_threads() >= 1);
}

COMP_TEST(json_round_trip) {
  JsonValue root = JsonValue::object({});
  root.set("format", JsonValue::string("com.compositor.project"));
  root.set("version", JsonValue::integer(13));
  root.set("width", JsonValue::integer(1920));
  root.set("opacity", JsonValue::number(0.75));
  root.set("visible", JsonValue::boolean(true));
  root.set("name", JsonValue::string("图层 1"));

  JsonValue items = JsonValue::array({});
  items.push(JsonValue::integer(1));
  items.push(JsonValue::integer(2));
  root.set("ids", std::move(items));

  const std::string text = json_dump(root, 2);
  std::string error;
  const JsonValue parsed = json_parse(text, &error);
  COMP_CHECK_MSG(error.empty(), "parse error");
  COMP_CHECK(parsed.is_object());
  COMP_CHECK(parsed.string_or("format", "") == "com.compositor.project");
  COMP_CHECK(parsed.integer_or("version", 0) == 13);
  COMP_CHECK(parsed.integer_or("width", 0) == 1920);
  COMP_CHECK(parsed.number_or("opacity", 0.0) > 0.74 && parsed.number_or("opacity", 0.0) < 0.76);
  COMP_CHECK(parsed.bool_or("visible", false));
  COMP_CHECK(parsed.string_or("name", "") == "图层 1");
  const JsonValue* ids = parsed.find("ids");
  COMP_CHECK(ids != nullptr && ids->is_array() && ids->as_array().size() == 2);
}

COMP_TEST(json_unicode_escape_and_errors) {
  /* 手工构造的 \u 转义（BMP 内的中文字符）应被解析为 UTF-8。 */
  const JsonValue value = json_parse("{\"name\":\"\\u56fe\\u5c42\"}", nullptr);
  COMP_CHECK(value.is_object());
  COMP_CHECK(value.string_or("name", "") == "图层");

  std::string error;
  const JsonValue broken = json_parse("{\"a\": }", &error);
  COMP_CHECK(broken.is_null());
  COMP_CHECK(!error.empty());
}
