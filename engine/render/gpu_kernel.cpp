// AI生成
#include "render/gpu_kernel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "compositing/blend.h"

namespace compositor {

namespace {

/* ------------------------------------------------------------------ HLSL */

const char* const kHlsl = R"HLSL(
// Compositor 合成计算着色器 —— D3D12 / cs_5_0
// 与 engine/compositing/blend.cpp 使用同一套 W3C Compositing and Blending Level 1 公式。
// 每个 case 后的注释即 comp_blend_mode 的上游规范名（ABI 冻结表，顺序与枚举数值一致）。
// 资源：u0 = 目标（预乘 RGBA8，读写），u1 = 源（预乘 RGBA8）。
#define COMP_BLEND_COUNT 27

RWTexture2D<float4> Dst : register(u0);
RWTexture2D<float4> Src : register(u1);

cbuffer Params : register(b0) {
  uint4 g_head;  // x = comp_blend_mode，y = 画布宽，z = 画布高
  uint4 g_tail;  // x = asuint(有效不透明度)
};

float clamp01(float v) { return clamp(v, 0.0, 1.0); }

float f_burn(float cb, float cs) { return cs <= 0.0 ? 0.0 : clamp01(1.0 - (1.0 - cb) / cs); }
float f_dodge(float cb, float cs) { return cs >= 1.0 ? 1.0 : clamp01(cb / (1.0 - cs)); }
float f_hardlight(float cb, float cs) {
  return cs <= 0.5 ? clamp01(2.0 * cb * cs) : clamp01(1.0 - 2.0 * (1.0 - cb) * (1.0 - cs));
}
float f_softlight(float cb, float cs) {
  if (cs <= 0.5) return clamp01(cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb));
  float d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
  return clamp01(cb + (2.0 * cs - 1.0) * (d - cb));
}
float f_vivid(float cb, float cs) {
  return cs <= 0.5 ? f_burn(cb, clamp01(2.0 * cs)) : f_dodge(cb, clamp01(2.0 * cs - 1.0));
}

float blend_channel(uint m, float cb, float cs) {
  switch (m) {
    case 0:  /* Normal */             return cs;
    case 1:  /* Darken */             return min(cb, cs);
    case 2:  /* Multiply */           return cb * cs;
    case 3:  /* Color Burn */         return f_burn(cb, cs);
    case 4:  /* Linear Burn */        return clamp01(cb + cs - 1.0);
    case 5:  /* Lighten */            return max(cb, cs);
    case 6:  /* Screen */             return cb + cs - cb * cs;
    case 7:  /* Color Dodge */        return f_dodge(cb, cs);
    case 8:  /* Linear Dodge (Add) */ return clamp01(cb + cs);
    case 9:  /* Overlay */            return f_hardlight(cs, cb);
    case 10: /* Soft Light */         return f_softlight(cb, cs);
    case 11: /* Hard Light */         return f_hardlight(cb, cs);
    case 12: /* Vivid Light */        return f_vivid(cb, cs);
    case 13: /* Linear Light */       return clamp01(cb + 2.0 * cs - 1.0);
    case 14: /* Pin Light */          return cs <= 0.5 ? min(cb, clamp01(2.0 * cs)) : max(cb, clamp01(2.0 * cs - 1.0));
    case 15: /* Hard Mix */           return f_vivid(cb, cs) < 0.5 ? 0.0 : 1.0;
    case 16: /* Difference */         return abs(cb - cs);
    case 17: /* Exclusion */          return clamp01(cb + cs - 2.0 * cb * cs);
    case 18: /* Subtract */           return clamp01(cb - cs);
    case 19: /* Divide */             return cs <= 0.0 ? 0.0 : clamp01(cb / cs);
    default:                          return cs;
  }
}

float lum3(float3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }
float sat3(float3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }

void clip_color(inout float3 c) {
  float l = lum3(c);
  float n = min(c.r, min(c.g, c.b));
  float x = max(c.r, max(c.g, c.b));
  if (n < 0.0) c = l + ((c - l) * l) / (l - n);
  if (x > 1.0) c = l + ((c - l) * (1.0 - l)) / (x - l);
}

void set_lum(inout float3 c, float l) { c += (l - lum3(c)); clip_color(c); }

void set_sat(inout float3 c, float s) {
  int i0 = 0, i1 = 1, i2 = 2;
  float a[3] = {c.r, c.g, c.b};
  if (a[i0] > a[i1]) { int t = i0; i0 = i1; i1 = t; }
  if (a[i1] > a[i2]) { int t = i1; i1 = i2; i2 = t; }
  if (a[i0] > a[i1]) { int t = i0; i0 = i1; i1 = t; }
  float mn = a[i0], mx = a[i2], md = a[i1];
  if (mx > mn) { md = ((md - mn) * s) / (mx - mn); mx = s; } else { md = 0.0; mx = 0.0; }
  float r[3];
  r[i0] = 0.0; r[i1] = md; r[i2] = mx;
  c = float3(r[0], r[1], r[2]);
}

float3 blend_rgb3(uint m, float3 cb, float3 cs) {
  switch (m) {
    case 20: /* Hue */        { float3 t = cs; set_sat(t, sat3(cb)); set_lum(t, lum3(cb)); return t; }
    case 21: /* Saturation */ { float3 t = cb; set_sat(t, sat3(cs)); set_lum(t, lum3(cb)); return t; }
    case 22: /* Color */      { float3 t = cs; set_lum(t, lum3(cb)); return t; }
    case 23: /* Luminosity */ { float3 t = cb; set_lum(t, lum3(cs)); return t; }
    /* A50：本仓超出上游的三种（PS 有、上游刻意未实现）。Dissolve 在 main() 里按坐标把
     * 有效 alpha 二值化，这里返回 cs 占位；Darker/Lighter Color 是整像素亮度择色。 */
    case 24: /* Dissolve */      { return cs; }
    case 25: /* Darker Color */  { return lum3(cb) <= lum3(cs) ? cb : cs; }
    case 26: /* Lighter Color */ { return lum3(cb) >= lum3(cs) ? cb : cs; }
    default: break;
  }
  return float3(blend_channel(m, cb.r, cs.r), blend_channel(m, cb.g, cs.g), blend_channel(m, cb.b, cs.b));
}

float unquantize(uint v) { return (float)v / 255.0; }
uint quantize(float v) { return (uint)clamp(v * 255.0 + 0.5, 0.0, 255.0); }

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= g_head.y || tid.y >= g_head.z) return;

  float4 d = Dst[tid.xy];
  float4 s = Src[tid.xy];

  // 直通化（与 CPU 的 to_straight 一致：a == 0 时颜色按 0 处理）。
  float3 Cb = d.a > 0.0 ? d.rgb / d.a : float3(0.0, 0.0, 0.0);
  float3 Cs = s.a > 0.0 ? s.rgb / s.a : float3(0.0, 0.0, 0.0);

  // 有效源 alpha = 像素 alpha × 图层有效不透明度（含组穿透/蒙版/剪贴）。
  float alpha = s.a * asfloat(g_tail.x);
  float ab = d.a;

  // A50 Dissolve：按文档坐标的确定性哈希把有效 alpha 二值化（与 CPU
  // blend_dissolve_coverage 同一套公式；跨后端必须逐式一致）。
  if (g_head.x == 24u) {
    uint h = (tid.x * 0x9E3779B1u) ^ (tid.y * 0x85EBCA6Bu);
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    float t = (float)h / 4294967296.0;
    alpha = (alpha >= 1.0) ? 1.0 : ((alpha > 0.0 && t < alpha) ? 1.0 : 0.0);
  }

  float3 B = blend_rgb3(g_head.x, Cb, Cs);

  // W3C 合成（结果即预乘颜色）；预乘分量直接量化写入，不得再乘 out_alpha（否则二次乘 alpha）。
  float3 co;
  co.x = alpha * (1.0 - ab) * Cs.x + alpha * ab * B.x + (1.0 - alpha) * ab * Cb.x;
  co.y = alpha * (1.0 - ab) * Cs.y + alpha * ab * B.y + (1.0 - alpha) * ab * Cb.y;
  co.z = alpha * (1.0 - ab) * Cs.z + alpha * ab * B.z + (1.0 - alpha) * ab * Cb.z;
  float ao = alpha + ab * (1.0 - alpha);

  float4 out_px;
  out_px.r = (float)quantize(clamp(co.x, 0.0, 1.0)) / 255.0;
  out_px.g = (float)quantize(clamp(co.y, 0.0, 1.0)) / 255.0;
  out_px.b = (float)quantize(clamp(co.z, 0.0, 1.0)) / 255.0;
  out_px.a = (float)quantize(clamp(ao, 0.0, 1.0)) / 255.0;
  Dst[tid.xy] = out_px;
}
)HLSL";

/* ------------------------------------------------------------------ GLSL */

const char* const kGlsl = R"GLSL(
// Compositor 合成计算着色器 —— Vulkan / GLSL 450
// 与 D3D12 版（render/gpu_kernel.cpp 的 HLSL）及 CPU 基准 blend.cpp 语义一一对应。
// 每个 case 后的注释即 comp_blend_mode 的上游规范名（ABI 冻结表）。
#version 450
#define COMP_BLEND_COUNT 27

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba8, set = 0, binding = 0) uniform image2D g_dst;
layout(rgba8, set = 0, binding = 1) uniform image2D g_src;

layout(push_constant) uniform Params {
  uint  mode;
  uint  width;
  uint  height;
  uint  pad;
  float alpha;  // 图层有效不透明度
} pc;

float clamp01(float v) { return clamp(v, 0.0, 1.0); }

float f_burn(float cb, float cs) { return cs <= 0.0 ? 0.0 : clamp01(1.0 - (1.0 - cb) / cs); }
float f_dodge(float cb, float cs) { return cs >= 1.0 ? 1.0 : clamp01(cb / (1.0 - cs)); }
float f_hardlight(float cb, float cs) {
  return cs <= 0.5 ? clamp01(2.0 * cb * cs) : clamp01(1.0 - 2.0 * (1.0 - cb) * (1.0 - cs));
}
float f_softlight(float cb, float cs) {
  if (cs <= 0.5) return clamp01(cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb));
  float d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
  return clamp01(cb + (2.0 * cs - 1.0) * (d - cb));
}
float f_vivid(float cb, float cs) {
  return cs <= 0.5 ? f_burn(cb, clamp01(2.0 * cs)) : f_dodge(cb, clamp01(2.0 * cs - 1.0));
}

float blend_channel(uint m, float cb, float cs) {
  switch (m) {
    case 0:  /* Normal */             return cs;
    case 1:  /* Darken */             return min(cb, cs);
    case 2:  /* Multiply */           return cb * cs;
    case 3:  /* Color Burn */         return f_burn(cb, cs);
    case 4:  /* Linear Burn */        return clamp01(cb + cs - 1.0);
    case 5:  /* Lighten */            return max(cb, cs);
    case 6:  /* Screen */             return cb + cs - cb * cs;
    case 7:  /* Color Dodge */        return f_dodge(cb, cs);
    case 8:  /* Linear Dodge (Add) */ return clamp01(cb + cs);
    case 9:  /* Overlay */            return f_hardlight(cs, cb);
    case 10: /* Soft Light */         return f_softlight(cb, cs);
    case 11: /* Hard Light */         return f_hardlight(cb, cs);
    case 12: /* Vivid Light */        return f_vivid(cb, cs);
    case 13: /* Linear Light */       return clamp01(cb + 2.0 * cs - 1.0);
    case 14: /* Pin Light */          return cs <= 0.5 ? min(cb, clamp01(2.0 * cs)) : max(cb, clamp01(2.0 * cs - 1.0));
    case 15: /* Hard Mix */           return f_vivid(cb, cs) < 0.5 ? 0.0 : 1.0;
    case 16: /* Difference */         return abs(cb - cs);
    case 17: /* Exclusion */          return clamp01(cb + cs - 2.0 * cb * cs);
    case 18: /* Subtract */           return clamp01(cb - cs);
    case 19: /* Divide */             return cs <= 0.0 ? 0.0 : clamp01(cb / cs);
    default:                          return cs;
  }
}

float lum3(vec3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }
float sat3(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }

void clip_color(inout vec3 c) {
  float l = lum3(c);
  float n = min(c.r, min(c.g, c.b));
  float x = max(c.r, max(c.g, c.b));
  if (n < 0.0) c = l + ((c - l) * l) / (l - n);
  if (x > 1.0) c = l + ((c - l) * (1.0 - l)) / (x - l);
}

void set_lum(inout vec3 c, float l) { c += (l - lum3(c)); clip_color(c); }

void set_sat(inout vec3 c, float s) {
  int i0 = 0, i1 = 1, i2 = 2;
  float a[3] = float[3](c.r, c.g, c.b);
  if (a[i0] > a[i1]) { int t = i0; i0 = i1; i1 = t; }
  if (a[i1] > a[i2]) { int t = i1; i1 = i2; i2 = t; }
  if (a[i0] > a[i1]) { int t = i0; i0 = i1; i1 = t; }
  float mn = a[i0], mx = a[i2], md = a[i1];
  if (mx > mn) { md = ((md - mn) * s) / (mx - mn); mx = s; } else { md = 0.0; mx = 0.0; }
  float r[3];
  r[i0] = 0.0; r[i1] = md; r[i2] = mx;
  c = vec3(r[0], r[1], r[2]);
}

vec3 blend_rgb3(uint m, vec3 cb, vec3 cs) {
  switch (m) {
    case 20: /* Hue */        { vec3 t = cs; set_sat(t, sat3(cb)); set_lum(t, lum3(cb)); return t; }
    case 21: /* Saturation */ { vec3 t = cb; set_sat(t, sat3(cs)); set_lum(t, lum3(cb)); return t; }
    case 22: /* Color */      { vec3 t = cs; set_lum(t, lum3(cb)); return t; }
    case 23: /* Luminosity */ { vec3 t = cb; set_lum(t, lum3(cs)); return t; }
    /* A50：本仓超出上游的三种（PS 有、上游刻意未实现）。Dissolve 在 main() 里按坐标把
     * 有效 alpha 二值化，这里返回 cs 占位；Darker/Lighter Color 是整像素亮度择色。 */
    case 24: /* Dissolve */      { return cs; }
    case 25: /* Darker Color */  { return lum3(cb) <= lum3(cs) ? cb : cs; }
    case 26: /* Lighter Color */ { return lum3(cb) >= lum3(cs) ? cb : cs; }
    default: break;
  }
  return vec3(blend_channel(m, cb.r, cs.r), blend_channel(m, cb.g, cs.g), blend_channel(m, cb.b, cs.b));
}

void main() {
  uvec2 tid = gl_GlobalInvocationID.xy;
  if (tid.x >= pc.width || tid.y >= pc.height) return;

  vec4 d = imageLoad(g_dst, ivec2(tid));
  vec4 s = imageLoad(g_src, ivec2(tid));

  vec3 Cb = d.a > 0.0 ? d.rgb / d.a : vec3(0.0);
  vec3 Cs = s.a > 0.0 ? s.rgb / s.a : vec3(0.0);

  float alpha = s.a * pc.alpha;
  float ab = d.a;

  // A50 Dissolve：按文档坐标的确定性哈希把有效 alpha 二值化（与 CPU
  // blend_dissolve_coverage 同一套公式；跨后端必须逐式一致）。
  if (pc.mode == 24u) {
    uint h = (tid.x * 0x9E3779B1u) ^ (tid.y * 0x85EBCA6Bu);
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    float t = float(h) / 4294967296.0;
    alpha = (alpha >= 1.0) ? 1.0 : ((alpha > 0.0 && t < alpha) ? 1.0 : 0.0);
  }

  vec3 B = blend_rgb3(pc.mode, Cb, Cs);
  vec3 co = alpha * (1.0 - ab) * Cs + alpha * ab * B + (1.0 - alpha) * ab * Cb;
  float ao = alpha + ab * (1.0 - alpha);

  vec3 q = floor(clamp(co, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
  float qa = floor(clamp(ao, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
  imageStore(g_dst, ivec2(tid), vec4(q, qa));
}
)GLSL";

uint8_t quantize_u8(double v) {
  if (v <= 0.0) return 0;
  if (v >= 1.0) return 255;
  return static_cast<uint8_t>(v * 255.0 + 0.5);
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/* 从着色器源码里抽取 `case N:` 分支及其后的块注释规范名，返回 (N, Name) 列表。 */
struct CaseEntry {
  int index;
  std::string name;
};

std::vector<CaseEntry> parse_cases(const char* source) {
  std::vector<CaseEntry> out;
  if (source == nullptr) return out;
  std::string text(source);
  size_t pos = 0;
  while (true) {
    const size_t at = text.find("case ", pos);
    if (at == std::string::npos) break;
    const size_t after = at + 5;
    size_t num_end = after;
    while (num_end < text.size() && text[num_end] >= '0' && text[num_end] <= '9') ++num_end;
    if (num_end == after) { pos = after; continue; }
    size_t colon = num_end;
    while (colon < text.size() && text[colon] != ':') {
      if (text[colon] == '\n') break;
      ++colon;
    }
    if (colon >= text.size() || text[colon] != ':') { pos = after; continue; }
    const size_t open = text.find("/*", colon);
    if (open == std::string::npos) { pos = colon; continue; }
    const size_t close = text.find("*/", open);
    if (close == std::string::npos) { pos = open; continue; }
    CaseEntry entry;
    entry.index = std::stoi(text.substr(after, num_end - after));
    std::string name = text.substr(open + 2, close - open - 2);
    const size_t first = name.find_first_not_of(" \t");
    const size_t last = name.find_last_not_of(" \t");
    if (first != std::string::npos) name = name.substr(first, last - first + 1);
    entry.name = name;
    out.push_back(entry);
    pos = close + 2;
  }
  return out;
}

}  // namespace

const char* gpu_kernel_hlsl() { return kHlsl; }
const char* gpu_kernel_glsl() { return kGlsl; }

int gpu_kernel_blend_case_count() { return 27; }

bool gpu_kernel_validate(std::string* error) {
  const int expected = gpu_kernel_blend_case_count();
  const char* sources[2] = {kHlsl, kGlsl};
  const char* labels[2] = {"hlsl", "glsl"};
  for (int s = 0; s < 2; ++s) {
    const std::vector<CaseEntry> cases = parse_cases(sources[s]);
    if (static_cast<int>(cases.size()) != expected) {
      if (error != nullptr) {
        *error = std::string(labels[s]) + " shader has " + std::to_string(cases.size()) +
                 " blend cases, expected " + std::to_string(expected);
      }
      return false;
    }
    for (int i = 0; i < expected; ++i) {
      const char* manifest = blend_mode_manifest_name(static_cast<comp_blend_mode>(i));
      if (cases[static_cast<size_t>(i)].index != i) {
        if (error != nullptr) {
          *error = std::string(labels[s]) + " shader case order mismatch at position " +
                   std::to_string(i);
        }
        return false;
      }
      if (manifest == nullptr || cases[static_cast<size_t>(i)].name != manifest) {
        if (error != nullptr) {
          *error = std::string(labels[s]) + " shader case " + std::to_string(i) + " is \"" +
                   cases[static_cast<size_t>(i)].name + "\", ABI table says \"" +
                   (manifest != nullptr ? manifest : "(null)") + "\"";
        }
        return false;
      }
    }
  }
  return true;
}

void gpu_kernel_blend_pixel(uint8_t* dst_premultiplied, const uint8_t* src_premultiplied,
                            comp_blend_mode mode, double layer_opacity, int32_t px, int32_t py) {
  if (dst_premultiplied == nullptr || src_premultiplied == nullptr) return;

  /* 直通化（镜像 to_straight）。 */
  const double sa = static_cast<double>(src_premultiplied[3]) / 255.0;
  const double ab = static_cast<double>(dst_premultiplied[3]) / 255.0;
  double Cs[3];
  double Cb[3];
  for (int i = 0; i < 3; ++i) {
    Cs[i] = sa > 0.0 ? (static_cast<double>(src_premultiplied[i]) / 255.0) / sa : 0.0;
    Cb[i] = ab > 0.0 ? (static_cast<double>(dst_premultiplied[i]) / 255.0) / ab : 0.0;
  }

  /* 有效源 alpha：像素 alpha × 图层不透明度（镜像着色器 s.a * g_alpha）。 */
  double alpha = sa * layer_opacity;

  /*
   * A50 Dissolve：按文档坐标的确定性哈希把有效 alpha 二值化（镜像着色器 main()）。
   * 边界：CPU 端坐标走 static_cast<uint32_t>，负坐标与 GPU 的 tid.xy（恒非负）不一致，
   * 故镜像仅在**非负画布坐标**下与 CPU 逐式一致（<=1 LSB）。
   */
  if (mode == COMP_BLEND_DISSOLVE) {
    if (alpha >= 1.0) {
      alpha = 1.0;
    } else if (alpha <= 0.0) {
      alpha = 0.0;
    } else {
      uint32_t h =
          static_cast<uint32_t>(px) * 0x9E3779B1u ^ static_cast<uint32_t>(py) * 0x85EBCA6Bu;
      h ^= h >> 13;
      h *= 0xC2B2AE35u;
      h ^= h >> 16;
      const double t = static_cast<double>(h) / 4294967296.0;
      alpha = (t < alpha) ? 1.0 : 0.0;
    }
  }

  double blended[3];
  blend_rgb(mode == COMP_BLEND_DISSOLVE ? COMP_BLEND_NORMAL : mode, Cb, Cs, blended);

  const double out_alpha = alpha + ab * (1.0 - alpha);
  for (int i = 0; i < 3; ++i) {
    const double co = alpha * (1.0 - ab) * Cs[i] + alpha * ab * blended[i] +
                      (1.0 - alpha) * ab * Cb[i];
    /* 结果已是预乘分量，直接量化写入；不再乘 out_alpha。 */
    dst_premultiplied[i] = quantize_u8(clamp01(co));
  }
  dst_premultiplied[3] = quantize_u8(clamp01(out_alpha));
}

}  // namespace compositor
