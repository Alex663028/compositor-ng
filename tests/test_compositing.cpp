// AI生成
/*
 * test_compositing.cpp — ABI 冻结护栏：混合模式与上游 LayerBlendMode 的一致性，
 * 以及「预乘修正」的逐字节回归断言。
 *
 * 冻结依据：robbietilton/Compositor 的 LayerBlendMode 共 24 个成员，其声明顺序即
 * Photoshop 分组顺序（正常 / 变暗组 / 变亮组 / 对比组 / 差值组 / 分量组）。
 * 本仓 comp_blend_mode 的数值顺序与 .comp 清单字符串都必须与之逐字一致，
 * 否则既有 .comp 工程文件会在两端被解读成不同模式。
 *
 * 预乘断言依据：W3C Compositing and Blending Level 1 的合成公式产出的 out_rgb
 * **本身已是预乘值**，落盘时不得再乘一次 alpha（否则 alpha<1 时结果被压暗）。
 * 本文件把该语义钉死在 compositor_blend_pixel_scalar（公共入口）上。
 */
#include <cstdio>
#include <cstring>
#include <vector>

#include "compositing/blend.h"
#include "compositing/composite_ops.h"
#include "render/gpu_kernel.h"
#include "test_framework.h"

using namespace compositor;

namespace {

/* 上游 LayerBlendMode 的 raw value，按枚举声明顺序逐字抄录。 */
const char* const kUpstreamNames[] = {
    "Normal",
    "Darken", "Multiply", "Color Burn", "Linear Burn",
    "Lighten", "Screen", "Color Dodge", "Linear Dodge (Add)",
    "Overlay", "Soft Light", "Hard Light", "Vivid Light", "Linear Light", "Pin Light",
    "Hard Mix",
    "Difference", "Exclusion", "Subtract", "Divide",
    "Hue", "Saturation", "Color", "Luminosity",
};

constexpr size_t kUpstreamCount = sizeof(kUpstreamNames) / sizeof(kUpstreamNames[0]);

/* 预乘 RGBA8 的合法性判据：任一颜色分量不得大于 alpha。 */
bool is_premultiplied(const uint8_t px[4]) {
  return px[0] <= px[3] && px[1] <= px[3] && px[2] <= px[3];
}

void describe(char* out, size_t size, const char* label, const uint8_t got[4], const uint8_t want[4]) {
  std::snprintf(out, size, "%s: got (%u,%u,%u,%u), want (%u,%u,%u,%u)", label, got[0], got[1],
                got[2], got[3], want[0], want[1], want[2], want[3]);
}

}  // namespace

COMP_TEST(blend_mode_matches_upstream_enum) {
  COMP_CHECK(kUpstreamCount == 24);
  COMP_CHECK(COMP_BLEND_NORMAL == 0);
  COMP_CHECK(static_cast<size_t>(COMP_BLEND_LUMINOSITY) + 1 == kUpstreamCount);

  for (size_t i = 0; i < kUpstreamCount; ++i) {
    const comp_blend_mode mode = static_cast<comp_blend_mode>(i);
    const char* name = blend_mode_manifest_name(mode);
    COMP_CHECK(name != nullptr);
    COMP_CHECK_MSG(name != nullptr && std::strcmp(name, kUpstreamNames[i]) == 0,
                   kUpstreamNames[i]);
  }
  /*
   * A50：24..26 是本仓超出上游的三种（PS 有、上游刻意未实现），纯尾部追加，
   * 规范名固定且不得与上游 0..23 撞名。
   */
  COMP_CHECK(COMP_BLEND_DISSOLVE == 24);
  COMP_CHECK(COMP_BLEND_DARKER_COLOR == 25);
  COMP_CHECK(COMP_BLEND_LIGHTER_COLOR == 26);
  {
    const char* dn = blend_mode_manifest_name(COMP_BLEND_DISSOLVE);
    COMP_CHECK_MSG(dn != nullptr && std::strcmp(dn, "Dissolve") == 0, "Dissolve");
    const char* dk = blend_mode_manifest_name(COMP_BLEND_DARKER_COLOR);
    COMP_CHECK_MSG(dk != nullptr && std::strcmp(dk, "Darker Color") == 0, "Darker Color");
    const char* lt = blend_mode_manifest_name(COMP_BLEND_LIGHTER_COLOR);
    COMP_CHECK_MSG(lt != nullptr && std::strcmp(lt, "Lighter Color") == 0, "Lighter Color");
  }
  /* 越界（27 及以后）必须是 nullptr，不得回落到 Normal。 */
  COMP_CHECK(blend_mode_manifest_name(static_cast<comp_blend_mode>(27)) == nullptr);
}

COMP_TEST(blend_mode_manifest_round_trip) {
  for (size_t i = 0; i < kUpstreamCount; ++i) {
    const char* name = kUpstreamNames[i];
    comp_blend_mode parsed = COMP_BLEND_NORMAL;
    COMP_CHECK(blend_mode_from_manifest_name(name, std::strlen(name), &parsed));
    COMP_CHECK(static_cast<size_t>(parsed) == i);
  }

  /* A50：扩展三种的规范名同样可解析回数值。 */
  {
    comp_blend_mode ext = COMP_BLEND_NORMAL;
    COMP_CHECK(blend_mode_from_manifest_name("Dissolve", 8, &ext) && ext == COMP_BLEND_DISSOLVE);
    COMP_CHECK(blend_mode_from_manifest_name("Darker Color", 12, &ext) &&
               ext == COMP_BLEND_DARKER_COLOR);
    COMP_CHECK(blend_mode_from_manifest_name("Lighter Color", 13, &ext) &&
               ext == COMP_BLEND_LIGHTER_COLOR);
  }

  comp_blend_mode parsed = COMP_BLEND_NORMAL;
  /* 前缀相似但不等长（"Normalized"）必须被拒，避免前缀匹配误判。 */
  COMP_CHECK(!blend_mode_from_manifest_name("Normalized", 10, &parsed));
  /* 大小写敏感；空名与空指针必须被拒。 */
  COMP_CHECK(!blend_mode_from_manifest_name("normal", 6, &parsed));
  COMP_CHECK(!blend_mode_from_manifest_name("", 0, &parsed));
  COMP_CHECK(!blend_mode_from_manifest_name(nullptr, 0, &parsed));
}

/*
 * 预乘修正回归（防「二次乘 alpha」缺陷复活）。
 *
 * 缺陷形态：blend_pixels 算出的 out_rgb 已是预乘值，落盘时若再走「乘 alpha」的写回
 * 路径，alpha<1 时整幅图被压暗——例如叠到全透明底时 (64,128,0,192) 会退化成
 * (48,96,0,192)（= 64/128 再乘 192/255 后截断）。
 */
COMP_TEST(premultiplied_blend_is_not_double_alpha_scaled) {
  char detail[192];

  /* 用例 1：预乘源叠到全透明底 —— 结果必须逐字节等于源（叠到「无」上等于自身）。 */
  {
    uint8_t dst[4] = {0, 0, 0, 0};
    const uint8_t src[4] = {64, 128, 0, 192};
    const uint8_t want[4] = {64, 128, 0, 192};
    const double src_alpha = 192.0 / 255.0;
    compositor_blend_pixel_scalar(dst, src, COMP_BLEND_NORMAL, src_alpha);
    describe(detail, sizeof(detail), "transparent base, premultiplied source", dst, want);
    COMP_CHECK_MSG(std::memcmp(dst, want, 4) == 0, detail);
  }

  /* 用例 2：不透明源叠到全透明底 —— 同样必须逐字节等于源。 */
  {
    uint8_t dst[4] = {0, 0, 0, 0};
    const uint8_t src[4] = {64, 128, 0, 255};
    const uint8_t want[4] = {64, 128, 0, 255};
    compositor_blend_pixel_scalar(dst, src, COMP_BLEND_NORMAL, 1.0);
    describe(detail, sizeof(detail), "opaque source onto transparent base", dst, want);
    COMP_CHECK_MSG(std::memcmp(dst, want, 4) == 0, detail);
  }

  /* 用例 3：全透明源（src_alpha = 0）不得改动不透明底。 */
  {
    uint8_t dst[4] = {64, 128, 0, 255};
    const uint8_t src[4] = {0, 0, 0, 0};
    const uint8_t want[4] = {64, 128, 0, 255};
    compositor_blend_pixel_scalar(dst, src, COMP_BLEND_NORMAL, 0.0);
    describe(detail, sizeof(detail), "transparent source must not touch base", dst, want);
    COMP_CHECK_MSG(std::memcmp(dst, want, 4) == 0, detail);
  }

  /* 用例 4：预乘不变式 —— 多模式 × 多不透明度扫描，输出必须仍是合法预乘 RGBA8。 */
  {
    const comp_blend_mode modes[] = {COMP_BLEND_NORMAL, COMP_BLEND_MULTIPLY, COMP_BLEND_SCREEN,
                                     COMP_BLEND_OVERLAY, COMP_BLEND_DIFFERENCE};
    const double alphas[] = {0.0, 0.25, 0.5, 0.75, 1.0};
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
      for (size_t a = 0; a < sizeof(alphas) / sizeof(alphas[0]); ++a) {
        uint8_t dst[4] = {32, 64, 0, 128};
        const uint8_t src[4] = {64, 128, 0, 192};
        compositor_blend_pixel_scalar(dst, src, modes[m], alphas[a]);
        std::snprintf(detail, sizeof(detail), "not premultiplied: mode=%d alpha=%.2f got (%u,%u,%u,%u)",
                      static_cast<int>(modes[m]), alphas[a], dst[0], dst[1], dst[2], dst[3]);
        COMP_CHECK_MSG(is_premultiplied(dst), detail);
      }
    }
  }
}

/*
 * GPU 内核语义钉在 CPU 基准上：着色器 main() 的 C++ 镜像（gpu_kernel_blend_pixel）
 * 与 CPU 合成基准（compositor_blend_pixel_scalar）必须在 **<= 1 LSB** 内一致。
 *
 * 口径说明：镜像内部按 float 演算（与 GPU 一致），CPU 标量路径按 double 演算；
 * 中间值落在 0.5 取整边界时两侧会差 1 个量化级，这正是本项目对 GPU 后端的验收口径
 * （GPU<->CPU <= 1 LSB）。真正要挡住的是「公式分叉」（会差几十个 LSB），而非末位舍入。
 * 没有 GPU 的机器上，这是防止「三端三套公式」的唯一可靠护栏。
 */
COMP_TEST(gpu_kernel_mirror_matches_cpu_scalar_blend) {
  const comp_blend_mode modes[] = {COMP_BLEND_NORMAL,  COMP_BLEND_MULTIPLY, COMP_BLEND_SCREEN,
                                   COMP_BLEND_OVERLAY, COMP_BLEND_DIFFERENCE};
  const uint8_t bases[][4] = {{0, 0, 0, 0}, {32, 64, 0, 128}, {200, 100, 50, 255}};
  const uint8_t sources[][4] = {{64, 128, 0, 192}, {0, 0, 0, 0}, {255, 255, 255, 128}};
  /* 两侧参数口径不同，见 gpu_kernel.h：
   *   CPU 标量入口 src_alpha = 有效 alpha（已含像素自身 alpha）；
   *   GPU 镜像 layer_opacity = 图层不透明度（内部再乘像素 alpha）。 */
  const double layer_opacities[] = {0.0, 0.5, 1.0};
  char detail[192];

  for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b) {
      for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); ++s) {
        for (size_t a = 0; a < sizeof(layer_opacities) / sizeof(layer_opacities[0]); ++a) {
          uint8_t cpu[4];
          uint8_t gpu[4];
          std::memcpy(cpu, bases[b], 4);
          std::memcpy(gpu, bases[b], 4);
          const double pixel_alpha = static_cast<double>(sources[s][3]) / 255.0;
          compositor_blend_pixel_scalar(cpu, sources[s], modes[m], pixel_alpha * layer_opacities[a]);
          gpu_kernel_blend_pixel(gpu, sources[s], modes[m], layer_opacities[a]);
          int max_delta = 0;
          for (int i = 0; i < 4; ++i) {
            const int delta = static_cast<int>(cpu[i]) - static_cast<int>(gpu[i]);
            const int magnitude = delta < 0 ? -delta : delta;
            if (magnitude > max_delta) max_delta = magnitude;
          }
          std::snprintf(detail, sizeof(detail),
                        "gpu mirror deviates > 1 LSB: mode=%d alpha=%.2f cpu (%u,%u,%u,%u) "
                        "gpu (%u,%u,%u,%u)",
                        static_cast<int>(modes[m]), layer_opacities[a], cpu[0], cpu[1], cpu[2], cpu[3],
                        gpu[0], gpu[1], gpu[2], gpu[3]);
          COMP_CHECK_MSG(max_delta <= 1, detail);
        }
      }
    }
  }
}
