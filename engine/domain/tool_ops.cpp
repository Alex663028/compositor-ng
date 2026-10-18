// AI生成
/*
 * tool_ops.cpp — 工具操作领域层实现（像素内核的产品通路）。
 *
 * 本文件是 engine/imaging 下 7 组「此前只有测试直调」的内核进入产品路径的唯一入口：
 *   WandPixels / HealPixels / PatchPixels / DitherPixels / LensPixels /
 *   BrushPixels / DeepPixels
 *
 * 装配原则（见 tool_ops.h 文件头）：
 *   - 内核签名按上游原样保留（它们是纯 C、不是本仓设计的），本文件负责把
 *     「领域参数 + 图层几何 + 坐标空间」翻译成内核要的形态；
 *   - 坐标空间：内核全部工作在**图层像素空间**（图层自己的像素网格），
 *     上游的魔棒种子、修补 coverage、透镜中心也都在图层像素空间。宿主的
 *     指针在文档像素空间，故本文件显式做文档→图层的换算；
 *   - 参数闸在开事务之前；像素写入一律走 writable_pixels()（COW）。
 */
#include "domain/tool_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "core/comp_geometry.h"
#include "compositing/adjustment_eval.h"
#include "compositing/compositor.h"
#include "domain/document.h"
#include "domain/retouch.h"
#include "imaging/comp_imaging.h"
#include "imaging/deep_round_trip.h"

namespace compositor {
namespace {

/* 校验上限：色彩范围吸管列表。8 色已远超上游面板容量，取整为闸。 */
constexpr int32_t kMaxRangeColors = 8;

/* 半径/直径/点数上限：防止宿主传进来一个把缓冲写爆的值。 */
constexpr int32_t kMaxSelectRadius = 512;
constexpr float kMaxBrushDiameter = 8192.0f;
/*
 * Smudge/Liquify 的直径上限**低于**画笔：它们的 carried/scratch 缓冲是
 * O(diameter²) 个 float。8192 直径时单是 carried 就约 1 GB（side ≈ 8193，
 * 8193² × 4 × 4 B），宿主一个误传就能把进程打死。上游没有显式上限，但上游
 * 的 UI 滑块也不会给到 8192；本仓取 1024（carried ≈ 16 MB）作为可验收的上界。
 */
constexpr float kMaxWarpDiameter = 1024.0f;
constexpr int32_t kMaxStrokePoints = 1 << 16;
/* 工具坐标（文档像素）的绝对值上限：与 transform 原点上限同口径，防止取整溢出。 */
constexpr double kMaxToolCoordinate = 1e6;
/*
 * 颗粒百分比上限。上游 Grain 面板最大 100（Photoshop 的 Noise 面板也是 0..100），
 * 但内核把 amount 当“半程百分比”放大到整程，>100 会让噪声幅度超过信号本身；
 * 取 100 作为闸，与上游面板量程一致。
 */
constexpr float kMaxGrainAmount = 100.0f;

bool finite01(float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; }

std::string label(uint32_t kind) {
  switch (kind) {
    case kToolOpSelect: return "select";
    case kToolOpStroke: return "stroke";
    case kToolOpHeal: return "heal";
    case kToolOpDither: return "dither";
    case kToolOpLens: return "lens";
    case kToolOpDeep: return "deep";
    case kToolOpScanlines: return "scanlines";
    case kToolOpChunky: return "chunky";
    case kToolOpClone: return "clone";
    case kToolOpSmudge: return "smudge";
    case kToolOpLiquify: return "liquify";
    case kToolOpBlur: return "blur";
    default: return "none";
  }
}

/* ------------------------------------------------------------ 选区 */

/*
 * 宿主指针在文档像素坐标，内核工作在图层像素坐标。返回 false 表示落在图层外。
 * 用 effective_bounds() 的原点做平移（对齐合成路径，见 composite_ops.cpp）：
 * transform 的 x/y 就是图层像素 (0,0) 在文档里的位置。
 */
bool seed_to_layer(const Layer& layer, int32_t doc_x, int32_t doc_y, int32_t* out_x, int32_t* out_y) {
  const comp_rect b = layer.effective_bounds();
  const int32_t x = doc_x - b.x;
  const int32_t y = doc_y - b.y;
  if (x < 0 || y < 0 || x >= layer.pixels.width || y >= layer.pixels.height) return false;
  *out_x = x;
  *out_y = y;
  return true;
}

/* 累积蒙版：新 coverage 与既有蒙版取最大（反复点选是"加选"，对齐上游）。 */
void accumulate_mask(Layer* layer, const std::vector<uint8_t>& coverage) {
  MaskBuffer& mask = layer->mask;
  if (mask.width != layer->pixels.width || mask.height != layer->pixels.height ||
      mask.empty()) {
    mask.resize(layer->pixels.width, layer->pixels.height, 0);
  }
  for (int32_t y = 0; y < layer->pixels.height; ++y) {
    uint8_t* row = mask.writable_row(y);
    const uint8_t* src = coverage.data() + static_cast<size_t>(y) * static_cast<size_t>(layer->pixels.width);
    const size_t w = static_cast<size_t>(layer->pixels.width);
    for (size_t x = 0; x < w; ++x) {
      if (src[x] > row[x]) row[x] = src[x];
    }
  }
}

/* ------------------------------------------------------------ 笔刷 */

/*
 * 圆头笔的几何：折线 + 直径/硬度/峰值 -> coverage。
 *
 * Stroke 与 Blur 共用同一份：两者在上游是同一套笔刷遮罩（`BrushStroke` 的
 * mask），只是把「笔色」还是「模糊样本」画进去不同。分叉会让两支笔在同一组
 * 参数下画出不同形状——那是最难解释的一类差异。
 *
 * 形状 = 到线段的距离 <= 半径；hardness 决定 [hardness*半径, 半径] 这段的
 * 线性羽化带（hardness=1 带宽 0 ⇒ 全实；hardness=0 从中心就开始羽化）。
 * pressure × flow 乘进峰值 coverage：flow 是「单次落笔把像素涂死」的总量上限，
 * 与 pressure（笔压）区分开，两者相乘是上游画笔的合成口径。
 */
class StrokeShape {
 public:
  StrokeShape(const float* points, int32_t point_count, float diameter, float hardness,
              float pressure_times_flow)
      : points_(points), point_count_(point_count) {
    const float d = diameter > 0.0f ? diameter : 1.0f;
    radius_ = d * 0.5f;
    const float hard = std::min(std::max(hardness, 0.0f), 1.0f);
    peak_ = std::min(std::max(pressure_times_flow, 0.0f), 1.0f);
    inner_ = radius_ * hard;     /* 全实区半径 */
    feather_ = radius_ - inner_; /* 羽化带宽 */
  }

  /* peak 为 0（pressure 或 flow 为 0）：整笔不落任何像素。 */
  bool empty() const { return peak_ <= 0.0f; }

  /* coverage 在 (px,py) 上取到最近线段的距离。 */
  float coverage_at(float px, float py) const {
    float best = 1e30f;
    if (point_count_ == 1) {
      /*
       * 单点 = 一记圆形 dab：鼠标只在原地按一下松手（没有 MOUSEMOVE）也必须留下
       * 笔尖。参数闸允许 1..65536 个折点，这里不接受「1 个点静默不画」。
       */
      const float ex = px - points_[0];
      const float ey = py - points_[1];
      best = ex * ex + ey * ey;
    }
    for (int32_t i = 0; i + 1 < point_count_; ++i) {
      const float ax = points_[i * 2], ay = points_[i * 2 + 1];
      const float bx = points_[(i + 1) * 2], by = points_[(i + 1) * 2 + 1];
      const float dx = bx - ax, dy = by - ay;
      const float len2 = dx * dx + dy * dy;
      float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
      t = std::min(std::max(t, 0.0f), 1.0f);
      const float ex = px - (ax + dx * t), ey = py - (ay + dy * t);
      const float d2 = ex * ex + ey * ey;
      if (d2 < best) best = d2;
    }
    if (best > radius_ * radius_) return 0.0f;
    const float d = std::sqrt(best);
    if (d <= inner_ || feather_ <= 0.0f) return peak_;
    const float t = 1.0f - (d - inner_) / feather_; /* 0..1：羽化外缘 → 实心 */
    const float e = t * t * (3.0f - 2.0f * t);      /* smoothstep，两端导数为 0 */
    return peak_ * e;
  }

  /* 落笔包围盒：按半径外扩，避免整幅扫描。完全落在图层外时返回 false。 */
  bool bounds(int32_t width, int32_t height, int32_t* x0, int32_t* y0, int32_t* x1,
              int32_t* y1) const {
    float min_x = points_[0], max_x = points_[0], min_y = points_[1], max_y = points_[1];
    for (int32_t i = 1; i < point_count_; ++i) {
      min_x = std::min(min_x, points_[i * 2]);
      max_x = std::max(max_x, points_[i * 2]);
      min_y = std::min(min_y, points_[i * 2 + 1]);
      max_y = std::max(max_y, points_[i * 2 + 1]);
    }
    int32_t left = static_cast<int32_t>(std::floor(min_x - radius_));
    int32_t right = static_cast<int32_t>(std::ceil(max_x + radius_));
    int32_t top = static_cast<int32_t>(std::floor(min_y - radius_));
    int32_t bottom = static_cast<int32_t>(std::ceil(max_y + radius_));
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, width - 1);
    bottom = std::min(bottom, height - 1);
    if (left > right || top > bottom) return false;
    *x0 = left;
    *y0 = top;
    *x1 = right;
    *y1 = bottom;
    return true;
  }

 private:
  const float* points_;
  int32_t point_count_;
  float radius_ = 0.0f;
  float inner_ = 0.0f;
  float feather_ = 0.0f;
  float peak_ = 0.0f;
};

/* 笔刷落笔：按 coverage 把笔色（直通 sRGB）盖到图层像素上。 */
void stamp_polyline(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                    const float* points, int32_t point_count, const BrushStrokeParams& brush) {
  const StrokeShape shape(points, point_count, brush.diameter, brush.hardness,
                          brush.pressure * brush.flow);
  if (shape.empty()) return;
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!shape.bounds(width, height, &x0, &y0, &x1, &y1)) return;

  /*
   * 合成口径：笔画按 coverage 盖在图层上，**不叠加**——同 alpha 源覆盖。这与
   * paint 工具一致，也让「同一参数两次调用逐字节相同」成立（若累加，第二次会
   * 把像素推得更深，可重复性就没了）。alpha 取 source-over 结果。
   */
  for (int32_t y = y0; y <= y1; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int32_t x = x0; x <= x1; ++x) {
      const float cov =
          shape.coverage_at(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
      if (cov <= 0.0f) continue;
      uint8_t* px = row + static_cast<size_t>(x) * 4;
      if (brush.erase != 0) {
        /* 橡皮擦 = destination-out：预乘 RGBA 一起乘 (1 - coverage)。 */
        const double keep = 1.0 - static_cast<double>(cov);
        for (int c = 0; c < 4; ++c) {
          px[c] = static_cast<uint8_t>(
              std::lround(std::min(std::max(static_cast<double>(px[c]) * keep, 0.0), 255.0)));
        }
        continue;
      }
      /* 图层是预乘的：笔色直通 sRGB 先乘 coverage 得预乘值，再 source-over。 */
      const double sa = cov;
      const double ba = px[3] / 255.0;
      const double oa = sa + ba * (1.0 - sa);
      for (int c = 0; c < 3; ++c) {
        const double sc = (brush.color[c] / 255.0) * sa;
        const double bc = px[c] / 255.0;
        const double oc = sc + bc * (1.0 - sa);
        px[c] = static_cast<uint8_t>(std::lround(std::min(std::max(oc, 0.0), 1.0) * 255.0));
      }
      px[3] = static_cast<uint8_t>(std::lround(std::min(oa, 1.0) * 255.0));
    }
  }
}

/* ------------------------------------------------------------ 模糊 */

/*
 * 模糊工具：把「当笔开始时的整层模糊副本」按笔迹 coverage 盖回当前像素。
 *
 * 与 stamp_polyline 的区别只在「画什么」：这里是预乘空间的两点插值
 * `p + (blurred - p) * cov`（等价于把模糊副本以 coverage 为 alpha 做
 * source-over），四个通道一起插值。**alpha 也参与**：上游对整幅 CIImage
 * 做高斯模糊，边缘会随涂抹变软；不模糊 alpha 会在透明边界上留下硬边。
 *
 * 副本在第 1 笔开始前就取好（见 run_tool_op），所以同一笔内的折线不会
 * 吃到「刚被自己模糊过的像素」再糊一次；重复落笔才会继续变软。
 */
void blur_stamp_paint(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                      const std::vector<uint8_t>& blurred, const float* points,
                      int32_t point_count, const BrushStrokeParams& brush) {
  const StrokeShape shape(points, point_count, brush.diameter, brush.hardness,
                          brush.pressure * brush.flow);
  if (shape.empty()) return;
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!shape.bounds(width, height, &x0, &y0, &x1, &y1)) return;
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  for (int32_t y = y0; y <= y1; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const uint8_t* src = blurred.data() + static_cast<size_t>(y) * row_bytes;
    for (int32_t x = x0; x <= x1; ++x) {
      const float cov =
          shape.coverage_at(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
      if (cov <= 0.0f) continue;
      uint8_t* px = row + static_cast<size_t>(x) * 4;
      const uint8_t* sp = src + static_cast<size_t>(x) * 4;
      for (int c = 0; c < 4; ++c) {
        const double value = static_cast<double>(px[c]) +
                             (static_cast<double>(sp[c]) - static_cast<double>(px[c])) * cov;
        px[c] = static_cast<uint8_t>(std::lround(std::min(std::max(value, 0.0), 255.0)));
      }
    }
  }
}

/* ------------------------------------------------------------ 透镜 */

/*
 * 以图层像素中心为畸变中心做径向重采样。
 *
 * 上游 Lens 是整篇镜头（中心 = 画布中心）；工具入口把作用面收窄到图层，
 * 中心取该图层像素的中心，与上游工具栏「Lens 作用于当前图层」一致，
 * 也避免「图层在画布一角时畸变轴心跑偏」这种直觉上错误的结果。
 * k = 0 时内核本身即恒等，这里再短路一次以保证逐字节不动。
 */
void apply_lens(ImageBuffer* pixels, double k) {
  if (k == 0.0) return;
  const size_t width = static_cast<size_t>(pixels->width);
  const size_t height = static_cast<size_t>(pixels->height);
  std::vector<uint8_t> source(pixels->pixel_data());
  lens_distort(source.data(), pixels->writable_pixels().data(), width, height, pixels->stride(), k);
}

/* ------------------------------------------------------------ 16 位深度 */

/*
 * 16 位 → 8 位的工作副本统一在 domain/layer.h 的 `deep_to_rgba8`（唯一生产者，
 * 与合成/缩略图/裁剪取 alpha 用的是同一份）。本层只用它，不再自带一份实现——
 * 两份实现一旦漂移，同一张 16 位图层在「合成出来」和「quantize 出来」就会不同。
 */

/*
 * 16 位图层上的 8 位工具：上游的做法是让操作落在图层**那份 dithered 8 位副本**
 * 上（DeepLayers.swift:4「a dithered 8-bit copy that … every tool keep using
 * unchanged」），图层自此不再是 16 位（:5「any other operation makes a new
 * ImportedImage without it, and the layer is simply 8-bit from then on」）。
 * 本函数照做：把 16 位像素换成它的 8 位副本，让后面的内核按 4 字节/像素工作。
 */
bool use_rgba8_working_copy(Layer* layer) {
  if (layer->pixels.format == COMP_PIXEL_RGBA8) return true;
  if (layer->pixels.format != COMP_PIXEL_RGBA16) return false;
  return deep_to_rgba8(layer->pixels, &layer->pixels);
}

/*
 * 落笔族（stroke / clone / smudge / liquify / blur）在 16 位与 HDR 上的往返。
 *
 * 上游 BrushStroke.swift:1097 `sixteenBit()` 的口径，不是「全 16 位落笔」：
 *   1. 把图层的**原始** 16 位像素当宽缓冲 `wide`（工具全程不碰它）；
 *   2. 8 位内核只在图层那份 dithered 8 位副本上跑，得到 `painted`；
 *      落笔前同一份副本是 `unpainted`；
 *   3. `deep_take_painted(_hdr)` 逐像素比较两者，只把**真正变过**的像素写成
 *      8 位值（16 位：×257；HDR：/255 转 half）。
 *
 * 于是「笔尖盖到的地方」是 8 位内核的结果，「没盖到的地方」保住 16 位/HDR
 * 精度——上游本来就这样（SmudgeLiquify.swift:236-240 的 `deepClone` 走同一条路）。
 *
 * 只有落笔族能这么往返：heal / dither / lens 之类的整幅算子在上游会让图层
 * 直接变成 8 位（DeepLayers.swift:5），本仓照做（use_rgba8_working_copy）。
 */
bool is_stroke_family(uint32_t kind) {
  return kind == static_cast<uint32_t>(kToolOpStroke) ||
         kind == static_cast<uint32_t>(kToolOpClone) ||
         kind == static_cast<uint32_t>(kToolOpSmudge) ||
         kind == static_cast<uint32_t>(kToolOpLiquify) ||
         /* 模糊只改笔尖盖到的像素，笔尖之外保住深像素——与落笔族同一口径。 */
         kind == static_cast<uint32_t>(kToolOpBlur);
}

struct DeepPaintCopy {
  ImageBuffer deep; /* 原始 16 位 / half 像素，工具全程不碰 */
  ImageBuffer base; /* 落笔前的 8 位副本 = deep_take_painted 的 unpainted */
  bool hdr = false;
  bool active() const { return deep.width > 0 && deep.height > 0 && base.width > 0; }
};

/* 深像素留在 deep，图层换成 base 的 8 位副本；两张缓冲共享同一条像素存储。 */
bool build_deep_paint_copy(const ImageBuffer& src, DeepPaintCopy* out) {
  if (out == nullptr) return false;
  out->hdr = src.format == COMP_PIXEL_RGBA16F;
  ImageBuffer base;
  const bool made = out->hdr ? hdr_to_rgba8(src, &base) : deep_to_rgba8(src, &base);
  if (!made) return false;
  out->deep = src; /* 共享 + COW：工具写的是副本，不污染深像素 */
  out->base = base;
  return true;
}

/*
 * 把 8 位落笔结果并回深像素（只在写成过的像素上取值）。
 * 返回 false 表示缓冲形态对不上（尺寸/步长），调用方按内部错误处理。
 */
bool merge_deep_paint(const ImageBuffer& painted, DeepPaintCopy* copy) {
  if (copy == nullptr || !copy->active()) return false;
  if (painted.format != COMP_PIXEL_RGBA8 || copy->base.format != COMP_PIXEL_RGBA8) return false;
  if (painted.width != copy->base.width || painted.height != copy->base.height) return false;
  if (copy->deep.width != copy->base.width || copy->deep.height != copy->base.height) return false;
  ImageBuffer& deep = copy->deep;
  uint16_t* wide = reinterpret_cast<uint16_t*>(deep.writable_pixels().data());
  if (wide == nullptr) return false;
  if (copy->hdr) {
    deep_take_painted_hdr(wide, deep.stride(), painted.row(0), copy->base.row(0),
                          copy->base.stride(), static_cast<size_t>(painted.width),
                          static_cast<size_t>(painted.height));
  } else {
    deep_take_painted(wide, deep.stride(), painted.row(0), copy->base.row(0),
                      copy->base.stride(), static_cast<size_t>(painted.width),
                      static_cast<size_t>(painted.height));
  }
  return true;
}

/*
 * 16 位深度往返在文件下方实现（`deep_round_trip`）：deep_split 把 16 位像素拆成
 * low / +1 / +2 / +4 四档的 8 位副本，在每档副本上跑**同一个** 8 位操作，再由
 * deep_merge 按「颜色落在 low 之上多少」读回 16 位。这是往返唯一能成立的形状：
 * 四趟都跑同一个内核操作，宿主不能自己造结果，否则就变成伪造像素。
 */

}  // namespace

/*
 * 16 位 / HDR 的 8 位操作往返已抽到 imaging/deep_round_trip.{h,cpp}：C5 之后
 * 调整求值（compositing/adjustment_eval.cpp）也要走同一条算法，两份实现一旦
 * 漂移，同一个调整在工具路径与合成路径上会给出不同像素。本文件只调用。
 */

/* ============================================================ 选区轮廓 */

/*
 * 图层蒙版的轮廓折线（上游 MagicWand.outline，`wand_trace`，WandPixels.h）。
 *
 * 为什么要有它：上游魔棒不止产出蒙版，还把蒙版的非零区域沿像素边缘描成闭合
 * 环（外边界顺时针、洞逆时针，于是缠绕规则正好填出那些像素），宿主拿它做
 * 「选区转可编辑路径」。本仓此前只做了「选中 -> 累积蒙版」那半。唯一的落点
 * 决定：轮廓必须给在**文档像素坐标**上，因为宿主要把它接到画布路径上，而
 * 蒙版缓冲区走的是图层像素网格，两者差一个 `effective_bounds()` 的原点。
 *
 * 语义（不改内核口径）：
 *   - 只读：不写图层像素、不写蒙版、不开事务、不进撤销栈；
 *   - 蒙版为空 → 成功且 loops 为空（对齐上游 `guard loopCount > 0` 返回 nil）；
 *   - 内核 -2（轮廓过于碎，不值得描）→ COMP_ERR_STATE，与上游 tooDetailed 同义；
 *   - 内核 -1（内存不足）→ COMP_ERR_OUT_OF_MEMORY。
 */
bool selection_outline(const Layer& layer, SelectionOutline* out, comp_result* out_code,
                       std::string* error) {
  auto fail = [&](comp_result code, const std::string& message) {
    if (out_code != nullptr) *out_code = code;
    if (error != nullptr) *error = message;
    return false;
  };
  if (out == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "outline out is null");

  out->loops.clear();
  out->points.clear();
  if (layer.mask.empty()) return true;   /* 无蒙版 = 无选中 = 无轮廓 */

  const size_t w = static_cast<size_t>(layer.mask.width);
  const size_t h = static_cast<size_t>(layer.mask.height);
  int32_t* points = nullptr;
  int32_t* loops = nullptr;
  size_t point_count = 0;
  size_t loop_count = 0;
  const int rc = wand_trace(layer.mask.row(0), w, h, &points, &point_count, &loops, &loop_count);
  /*
   * 无论走哪条分支，两段 malloc 都只在这里释放一次（内核失败时也会置空/不填，
   * free(nullptr) 安全）。搬到宿主 vector 后裸指针不越过本函数，避免把
   * malloc 契约泄漏给 ABI 边界。
   */
  if (rc == -2) {
    free(points);
    free(loops);
    return fail(COMP_ERR_STATE, "selection outline is too detailed to draw; raise the tolerance or "
                "turn off contiguous");
  }
  if (rc != 0) {
    free(points);
    free(loops);
    return fail(COMP_ERR_OUT_OF_MEMORY, "selection outline ran out of memory");
  }
  out->points.assign(points, points + point_count * 2);
  out->loops.assign(loops, loops + loop_count);
  free(points);
  free(loops);

  /* 平移到文档像素空间：宿主要把轮廓接到画布路径上。 */
  const comp_rect b = layer.effective_bounds();
  if (b.x != 0 || b.y != 0) {
    for (size_t i = 0; i < out->points.size(); i += 2) {
      out->points[i] += b.x;
      out->points[i + 1] += b.y;
    }
  }
  return true;
}

/* ============================================================ 直方图 */

/*
 * 图层像素的 256 桶直方图（`levels_histogram`，LevelsPixels.h）。
 *
 * 为什么要有它：Levels 面板要显示直方图，而上游 `Levels.swift` 的直方图是
 * **带 coverage 的加权直方图**——coverage 是该像素被蒙版/不透明度覆盖的比例，
 * 权重按 alpha 走，而不是「每个像素各算一票」。宿主此前只能自己扫像素，
 * 于是不得不自己决定加权口径；一旦与内核差一档，面板上的曲线就和实际
 * levels 调整脱节。
 *
 * `bins` 必须是 `double[256]`。coverage 可空：空 = 权重 1（整幅按不透明计），
 * 非空时尺寸须等于图层像素的 `height × stride` 布局，按 `coverage_stride` 取行。
 * 16 位图层显式拒绝（COMP_ERR_UNSUPPORTED）——直方图的桶是 8 位的，
 * 量化口径归 deep_to_rgba8 管，本函数只做 8 位的直读，不在这里偷偷降位。
 */
bool layer_histogram(const Layer& layer, const uint8_t* coverage, size_t coverage_stride,
                     double* bins, comp_result* out_code, std::string* error) {
  auto fail = [&](comp_result code, const std::string& message) {
    if (out_code != nullptr) *out_code = code;
    if (error != nullptr) *error = message;
    return false;
  };
  if (bins == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "histogram bins are null");
  if (layer.pixels.empty()) {
    return fail(COMP_ERR_STATE, "layer has no pixels");
  }
  if (layer.pixels.format == COMP_PIXEL_RGBA16) {
    return fail(COMP_ERR_UNSUPPORTED,
                "histogram is 8-bit only; quantize the layer first (COMP_TOOL_DEEP / quantize)");
  }
  for (int i = 0; i < 256; ++i) bins[i] = 0.0;

  const size_t w = layer.pixels.width;
  const size_t h = layer.pixels.height;
  const size_t stride = layer.pixels.stride();
  for (size_t y = 0; y < h; ++y) {
    const uint8_t* px = layer.pixels.row(y);
    const uint8_t* cov = coverage != nullptr ? coverage + y * coverage_stride : nullptr;
    for (size_t x = 0; x < w; ++x) {
      /*
       * 内核逐像素取三个颜色通道，桶是「亮度」（0.299R+0.587G+0.114B）。
       * 预乘像素直接取 R/G/B 就是取亮度被 alpha 压过——半透明区域会被
       * 系统性低估。因此这里先把颜色**反预乘回直通**，让面板显示的是
       * 用户实际看到的颜色分布，而不是 alpha 的分布。
       */
      const uint8_t a = px[x * 4 + 3];
      float r, g, b;
      if (a == 0) {
        r = g = b = 0.0f;
      } else if (a == 255) {
        r = px[x * 4 + 0];
        g = px[x * 4 + 1];
        b = px[x * 4 + 2];
      } else {
        const float inv = 255.0f / static_cast<float>(a);
        r = std::min(255.0f, px[x * 4 + 0] * inv);
        g = std::min(255.0f, px[x * 4 + 1] * inv);
        b = std::min(255.0f, px[x * 4 + 2] * inv);
      }
      const uint8_t luma = static_cast<uint8_t>(
          std::lround(0.299f * r + 0.587f * g + 0.114f * b));
      bins[luma] += cov != nullptr ? (cov[x] / 255.0) : 1.0;
    }
  }
  if (out_code != nullptr) *out_code = COMP_OK;
  return true;
}

/* ============================================================ 修补范围 */

/*
 * 修补 coverage 的非零包围盒（`heal_coverage_bounds`，HealPixels.h）。
 *
 * 为什么要有它：Spot Heal 的实现要先把待修补区域裁到包围盒再做 PatchMatch，
 * 否则一张大图上点一个小洞也要全图求解。上游 `Heal.swift` 自己算这个包围盒；
 * 本仓的 `spot_heal` 内部已经在算，宿主却算不出自己手上的 coverage 覆盖了
 * 哪里——只能自己写一遍扫描，多一处与内核漂移的机会。
 *
 * 只读：不改图层、不开事务、不进撤销栈。coverage 空或全 0 时 bounds 全 0
 * （半开区间 [x0,x1)×[y0,y1)），与内核一致。coverage 的 stride 由调用方给，
 * 因为 coverage 不存在「图层像素缓冲」那个自动 stride 的东西。
 */
bool heal_coverage_bounds_of(const uint8_t* coverage, size_t coverage_stride,
                             int32_t width, int32_t height, int32_t bounds[4],
                             comp_result* out_code, std::string* error) {
  auto fail = [&](comp_result code, const std::string& message) {
    if (out_code != nullptr) *out_code = code;
    if (error != nullptr) *error = message;
    return false;
  };
  if (bounds == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "bounds out is null");
  if (width <= 0 || height <= 0) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "coverage size must be positive");
  }
  if (coverage == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "heal coverage is null");

  long raw[4] = {0, 0, 0, 0};
  heal_coverage_bounds(coverage, static_cast<size_t>(width), static_cast<size_t>(height),
                       coverage_stride, raw);
  bounds[0] = static_cast<int32_t>(raw[0]);
  bounds[1] = static_cast<int32_t>(raw[1]);
  bounds[2] = static_cast<int32_t>(raw[2]);
  bounds[3] = static_cast<int32_t>(raw[3]);
  if (out_code != nullptr) *out_code = COMP_OK;
  return true;
}

/* ============================================================ 入口 */

bool run_tool_op(Document* document, comp_layer_id target, const ToolOpParams& params,
                 comp_result* out_code, std::string* error, std::string* note) {
  /*
   * 错误码由本层直接回报，不让调用方从文案反推（见 tool_ops.h 末尾）。
   * 每一处失败都自带码；兜底为 INTERNAL，只在 fail() 被漏标的路径上出现。
   */
  auto fail = [&](comp_result code, const std::string& message) {
    if (out_code != nullptr) *out_code = code;
    if (error != nullptr) *error = label(static_cast<uint32_t>(params.kind)) + ": " + message;
    return false;
  };
  auto bad_arg = [&](const std::string& message) { return fail(COMP_ERR_INVALID_ARGUMENT, message); };
  auto bad_state = [&](const std::string& message) { return fail(COMP_ERR_STATE, message); };
  auto oom = [&](const std::string& message) { return fail(COMP_ERR_OUT_OF_MEMORY, message); };

  if (document == nullptr) return bad_arg("document is null");
  Layer* layer = document->find(target);
  if (layer == nullptr) return fail(COMP_ERR_NOT_FOUND, "layer not found");
  if (layer->is_group) return bad_state("group layer has no pixels to operate on");
  if (!layer->has_pixels()) return bad_state("layer has no pixels");

  const size_t width = static_cast<size_t>(layer->pixels.width);
  const size_t height = static_cast<size_t>(layer->pixels.height);

  /* ---------------- 参数闸（全部在开事务之前，见 tool_ops.h 设计原则 3） */

  switch (params.kind) {
    case kToolOpNone:
      return bad_arg("kind is none");

    case kToolOpSelect: {
      const bool use_range = params.use_color_range != 0;
      if (use_range) {
        if (params.color_range.fuzziness < 0 || params.color_range.fuzziness > 255) {
          return bad_arg("color range fuzziness out of 0..255");
        }
        if (params.include_count < 0 || params.include_count > kMaxRangeColors) {
          return bad_arg("include color count out of range");
        }
        if (params.exclude_count < 0 || params.exclude_count > kMaxRangeColors) {
          return bad_arg("exclude color count out of range");
        }
        if (params.include_count > 0 && params.include == nullptr) {
          return bad_arg("include colors are null");
        }
        if (params.exclude_count > 0 && params.exclude == nullptr) {
          return bad_arg("exclude colors are null");
        }
      } else {
        if (params.select.tolerance < 0 || params.select.tolerance > 255) {
          return bad_arg("select tolerance out of 0..255");
        }
        if (params.select.radius < 0 || params.select.radius > kMaxSelectRadius) {
          return bad_arg("select radius out of range");
        }
        int32_t seed_x = 0, seed_y = 0;
        if (!seed_to_layer(*layer, params.seed_x, params.seed_y, &seed_x, &seed_y)) {
          return bad_arg("wand seed lies outside the layer");
        }
      }
      break;
    }

    case kToolOpStroke: {
      if (params.point_count <= 0 || params.point_count > kMaxStrokePoints) {
        return bad_arg("stroke needs 1..65536 points");
      }
      if (params.points == nullptr) return bad_arg("stroke points are null");
      if (!std::isfinite(params.brush.diameter) || params.brush.diameter <= 0.0f ||
          params.brush.diameter > kMaxBrushDiameter) {
        return bad_arg("brush diameter out of range");
      }
      if (!finite01(params.brush.pressure)) return bad_arg("brush pressure out of 0..1");
      if (!finite01(params.brush.hardness)) return bad_arg("brush hardness out of 0..1");
      if (!finite01(params.brush.flow)) return bad_arg("brush flow out of 0..1");
      /* 颗粒是可选附加项，但一旦给出就得在合法区间：负数会当反相噪声，比拒绝更坏。 */
      if (!std::isfinite(params.brush.grain_amount) || params.brush.grain_amount < 0.0f ||
          params.brush.grain_amount > kMaxGrainAmount) {
        return bad_arg("brush grain amount out of range");
      }
      for (int32_t i = 0; i < params.point_count * 2; ++i) {
        if (!std::isfinite(params.points[i])) return bad_arg("stroke point is not finite");
      }
      break;
    }

    case kToolOpClone:
    case kToolOpSmudge:
    case kToolOpLiquify: {
      const bool is_clone = params.kind == kToolOpClone;
      if (params.point_count <= 0 || params.point_count > kMaxStrokePoints) {
        return bad_arg("retouch stroke needs 1..65536 points");
      }
      if (params.points == nullptr) return bad_arg("retouch points are null");
      const float diameter = params.brush.diameter;
      const float max_diameter = is_clone ? kMaxBrushDiameter : kMaxWarpDiameter;
      if (!std::isfinite(diameter) || diameter <= 0.0f || diameter > max_diameter) {
        return bad_arg(is_clone ? "clone brush diameter out of range"
                                : "smudge/liquify diameter out of range (max 1024)");
      }
      if (!finite01(params.brush.hardness)) return bad_arg("brush hardness out of 0..1");
      /*
       * 坐标上界不只是「防呆」：落笔包围盒、dab 中心与 Liquify 的位移余量都会
       * 走 int32 取整，1e30 这类有限但离谱的值在取整处是 UB（既有 Stroke 闸只
       * 查了 isfinite）。新算子按 transform 原点同款上限（1e6）拒绝。
       */
      for (int32_t i = 0; i < params.point_count * 2; ++i) {
        if (!std::isfinite(params.points[i]) ||
            std::fabs(static_cast<double>(params.points[i])) > kMaxToolCoordinate) {
          return bad_arg("retouch point is not finite or out of range");
        }
      }
      if (is_clone) {
        if (!finite01(params.brush.pressure)) return bad_arg("brush pressure out of 0..1");
        if (!finite01(params.brush.flow)) return bad_arg("brush flow out of 0..1");
        if (!finite01(params.clone.opacity)) return bad_arg("clone opacity out of 0..1");
        const bool carried = params.clone.aligned != 0 && params.clone.has_offset != 0;
        if (!params.clone.has_source && !carried) {
          return bad_state("clone has no source point and no carried offset");
        }
        if (params.clone.has_source) {
          if (!std::isfinite(params.clone.source_x) ||
              !std::isfinite(params.clone.source_y) ||
              std::fabs(params.clone.source_x) > kMaxToolCoordinate ||
              std::fabs(params.clone.source_y) > kMaxToolCoordinate) {
            return bad_arg("clone source point is not finite or out of range");
          }
        }
        if (carried) {
          if (!std::isfinite(params.clone.offset_x) ||
              !std::isfinite(params.clone.offset_y) ||
              std::fabs(params.clone.offset_x) > kMaxToolCoordinate ||
              std::fabs(params.clone.offset_y) > kMaxToolCoordinate) {
            return bad_arg("clone carried offset is not finite or out of range");
          }
        }
      } else {
        if (!finite01(params.warp.strength)) return bad_arg("warp strength out of 0..1");
      }
      break;
    }

    case kToolOpBlur: {
      if (params.point_count <= 0 || params.point_count > kMaxStrokePoints) {
        return bad_arg("blur stroke needs 1..65536 points");
      }
      if (params.points == nullptr) return bad_arg("blur points are null");
      if (!std::isfinite(params.brush.diameter) || params.brush.diameter <= 0.0f ||
          params.brush.diameter > kMaxBrushDiameter) {
        return bad_arg("blur brush diameter out of range");
      }
      if (!finite01(params.brush.hardness)) return bad_arg("brush hardness out of 0..1");
      if (!finite01(params.brush.pressure)) return bad_arg("brush pressure out of 0..1");
      if (!finite01(params.brush.flow)) return bad_arg("brush flow out of 0..1");
      /* 上游把 blurRadius 夹到 0.5..50：0 不是「不模糊」，而是一个会被静默抬高的值，
       * 这里直接拒绝，免得宿主以为「radius 0 = 恒等」。 */
      if (!std::isfinite(params.blur.radius) || params.blur.radius < 0.5f ||
          params.blur.radius > 50.0f) {
        return bad_arg("blur radius must be 0.5..50");
      }
      for (int32_t i = 0; i < params.point_count * 2; ++i) {
        if (!std::isfinite(params.points[i]) ||
            std::fabs(static_cast<double>(params.points[i])) > kMaxToolCoordinate) {
          return bad_arg("blur point is not finite or out of range");
        }
      }
      break;
    }

    case kToolOpHeal: {
      if (params.heal.mode < 0 || params.heal.mode > 2) return bad_arg("heal mode must be 0..2");
      if (!finite01(params.heal.opacity)) return bad_arg("heal opacity out of 0..1");
      if (params.coverage == nullptr) return bad_arg("heal coverage is null");
      const size_t need = width > 0 ? width : 1u;
      if (params.coverage_stride < need) return bad_arg("heal coverage stride too small");
      break;
    }

    case kToolOpDither: {
      if (params.dither.style < 0 || params.dither.style > DITHER_GLYPHS) {
        return bad_arg("dither style out of range");
      }
      if (params.dither.levels < 2 || params.dither.levels > 16) {
        return bad_arg("dither levels must be 2..16");
      }
      if (!std::isfinite(params.dither.diffusion) || params.dither.diffusion < 0.0f ||
          params.dither.diffusion > 1.0f) {
        return bad_arg("dither diffusion out of 0..1");
      }
      if (params.dither.cell < 2 || params.dither.cell > 512) {
        return bad_arg("dither cell must be 2..512");
      }
      if (!std::isfinite(params.dither.angle)) return bad_arg("dither angle is not finite");
      if (!std::isfinite(params.dither.density) || !std::isfinite(params.dither.contrast)) {
        return bad_arg("dither density/contrast is not finite");
      }
      break;
    }

    case kToolOpLens: {
      if (!std::isfinite(params.lens.k)) return bad_arg("lens k is not finite");
      if (std::fabs(params.lens.k) > 1.0) return bad_arg("lens k must be within -1..1");
      break;
    }

    case kToolOpScanlines: {
      const ScanlinesParams& s = params.scanlines;
      if (s.spacing < 2 || s.spacing > 32) return bad_arg("scanlines spacing must be 2..32");
      if (!finite01(s.thickness) || s.thickness < 0.05f) return bad_arg("scanlines thickness must be 0.05..1");
      if (!finite01(s.glow) || !finite01(s.dots) || !finite01(s.smoothness) ||
          !finite01(s.threshold) || !finite01(s.black_level)) {
        return bad_arg("scanlines glow/dots/smoothness/threshold/black_level must be 0..1");
      }
      if (!std::isfinite(s.wobble) || s.wobble < 0.0f || s.wobble > 64.0f) {
        return bad_arg("scanlines wobble must be 0..64");
      }
      if (!std::isfinite(s.displace) || s.displace < -100.0f || s.displace > 100.0f) {
        return bad_arg("scanlines displace must be -100..100");
      }
      if (!std::isfinite(s.split) || s.split < 0.0f || s.split > 16.0f) {
        return bad_arg("scanlines split must be 0..16");
      }
      if (!finite01(s.density + 0.5f) || !finite01(s.contrast + 0.5f)) {
        return bad_arg("scanlines density/contrast must be -1..1");
      }
      break;
    }

    case kToolOpChunky: {
      if (params.chunky.block < 2 || params.chunky.block > 64) {
        return bad_arg("chunky block must be 2..64");
      }
      if (params.chunky.dot != 0 && params.chunky.two_colors == 0) {
        return bad_arg("chunky dot gaps take the dark color only when two_colors is set");
      }
      break;
    }

    case kToolOpDeep: {
      if (params.deep_mode != kToolDeepQuantize && params.deep_mode != kToolDeepRoundTrip) {
        return bad_arg("deep mode must be quantize or round trip");
      }
      /*
       * 16 位与 HDR 两种深度都收：前者的往返在 16 位整数布局上跑，后者先经
       * deep_hdr_normalize 把高光除回白内（见下面的 deep_hdr_round_trip）。
       * 8 位图层仍然拒绝——它没有 16 位以上的精度可保，降级成「对 8 位像素再跑
       * 一遍」就是伪造。HDR 不是 8 位：它带着白以上的光，深度的往返对它有实义
       * （DeepLayers.swift:82-84）。
       */
      if (layer->pixels.format != COMP_PIXEL_RGBA16 && layer->pixels.format != COMP_PIXEL_RGBA16F) {
        return bad_state("deep needs a COMP_PIXEL_RGBA16 or COMP_PIXEL_RGBA16F layer (this layer "
                    "holds 8-bit pixels already); import a 16-bit or HDR asset, or set those "
                    "pixels via comp_layer_set_pixels");
      }
      if (params.deep_mode == kToolDeepRoundTrip) {
        /*
         * 往返的内层操作只允许**不改几何、不写蒙版**的像素算子（目前是 lens /
         * dither / none）。stroke、heal 需要坐标与 coverage，select 写蒙版，
         * scanlines/chunky 改了就是另一回事——降级成「对副本的像素变换」等于
         * 伪造它们在 16 位上的语义，所以在参数闸拒绝，不留给执行期。
         */
        if (params.inner != kToolOpNone && params.inner != kToolOpLens && params.inner != kToolOpDither) {
          return bad_arg("deep round trip inner op must be none, lens or dither (operations that "
                    "change geometry or write the mask have no 16-bit meaning here)");
        }
        if (params.inner == kToolOpLens) {
          if (!std::isfinite(params.lens.k) || std::fabs(params.lens.k) > 1.0) {
            return bad_arg("deep round trip inner lens k must be finite and within -1..1");
          }
        }
        if (params.inner == kToolOpDither) {
          if (params.dither.style < 0 || params.dither.style > DITHER_GLYPHS) {
            return bad_arg("deep round trip inner dither style out of range");
          }
          if (params.dither.levels < 2 || params.dither.levels > 16) {
            return bad_arg("deep round trip inner dither levels must be 2..16");
          }
          if (!std::isfinite(params.dither.diffusion) || params.dither.diffusion < 0.0f ||
              params.dither.diffusion > 1.0f) {
            return bad_arg("deep round trip inner dither diffusion out of 0..1");
          }
          if (params.dither.cell < 2 || params.dither.cell > 512) {
            return bad_arg("deep round trip inner dither cell must be 2..512");
          }
          if (!std::isfinite(params.dither.angle)) return bad_arg("deep round trip inner dither angle is not finite");
          if (!std::isfinite(params.dither.density) || !std::isfinite(params.dither.contrast)) {
            return bad_arg("deep round trip inner dither density/contrast is not finite");
          }
        }
      }
      break;
    }

    default:
      return bad_arg("unknown tool op kind");
  }

  /* ---------------- 执行（一步撤销） */

  const bool owns_txn = !document->txn_active();
  std::string txn_error;
  if (owns_txn && document->txn_begin("Tool: " + label(static_cast<uint32_t>(params.kind)),
                                      &txn_error) != COMP_OK) {
    if (error != nullptr) *error = txn_error;
    if (out_code != nullptr) *out_code = COMP_ERR_STATE;
    return false;
  }
  Layer* target_layer = document->find(target);
  if (target_layer == nullptr) {
    if (owns_txn) document->txn_abort(&txn_error);
    return fail(COMP_ERR_NOT_FOUND, "layer vanished after the parameter gate");
  }

  /*
   * 落笔族在 16 位 / HDR 上保留深度（DeepPaintCopy）：先把原始深像素收好，
   * 图层换成它的 8 位副本；内核跑完后只把**写成过**的像素并回去（见
   * merge_deep_paint）。这条路径对齐上游 BrushStroke.swift:1097 的 sixteenBit()，
   * HDR 走同一函数的 `deep_take_painted_hdr` 分支（:1119），不再需要 tile+coverage
   * 模型——上游也是整幅比较 painted/unpainted，不是按 tile 记账。
   */
  DeepPaintCopy deep_paint;
  if (params.kind != kToolOpDeep && is_stroke_family(static_cast<uint32_t>(params.kind)) &&
      (target_layer->pixels.format == COMP_PIXEL_RGBA16 ||
       target_layer->pixels.format == COMP_PIXEL_RGBA16F)) {
    if (!build_deep_paint_copy(target_layer->pixels, &deep_paint)) {
      if (owns_txn) document->txn_abort(&txn_error);
      return bad_state("layer pixels could not be brought to their 8-bit working copy");
    }
    target_layer->pixels = deep_paint.base;
  }

  /*
   * 其余工具是整幅 8 位算子：先把 16 位像素落到它的 8 位副本（见
   * use_rgba8_working_copy），图层自此是 8 位（DeepLayers.swift:5）。
   * deep 自己不在这里降位——quantize 分支做的就是这件事，round trip 分支在参数闸
   * 已经拒绝，在这里降位会掩盖那条拒绝。
   *
   * HDR（RGBA16F）**不**降位：降成 8 位等于把白以上的光永久丢掉，而这层规则
   * 属于 16 位图层（:5），不属于 HDR（:82-84）。除落笔族外，HDR 上可用的仍是
   * deep 的往返；其余工具如实拒绝，不静默降级。
   */
  if (params.kind != kToolOpDeep && !use_rgba8_working_copy(target_layer)) {
    if (owns_txn) document->txn_abort(&txn_error);
    return bad_state("layer pixels are neither RGBA8 nor RGBA16 (an HDR layer takes only the deep "
                     "tool's round trip today)");
  }

  bool ok = true;
  switch (params.kind) {
    case kToolOpSelect: {
      std::vector<uint8_t> mask(width * height, 0);
      long selected = 0;
      if (params.use_color_range != 0) {
        selected = color_range_mask(target_layer->pixels.writable_pixels().data(), width, height,
                                    target_layer->pixels.stride(), params.include,
                                    params.include_count, params.exclude, params.exclude_count,
                                    params.color_range.fuzziness, params.color_range.invert,
                                    mask.data());
      } else {
        int32_t seed_x = 0, seed_y = 0;
        seed_to_layer(*target_layer, params.seed_x, params.seed_y, &seed_x, &seed_y);
        selected = wand_mask(target_layer->pixels.writable_pixels().data(), width, height,
                             target_layer->pixels.stride(),
                             static_cast<size_t>(seed_x), static_cast<size_t>(seed_y),
                             static_cast<size_t>(params.select.radius), params.select.tolerance,
                             params.select.contiguous, mask.data());
      }
      if (selected < 0) {
        ok = oom("select ran out of memory");
        break;
      }
      /* 选中数不为 0 时才写蒙版：否则「点了个空处」会留下一张全 0 的空蒙版，
       * 把整层蒙成透明——这与上游「无选中即不改变」一致。 */
      if (selected > 0) accumulate_mask(target_layer, mask);
      if (note != nullptr) *note += "selected pixels are accumulated into the layer mask (add)";
      break;
    }

    case kToolOpStroke: {
      /* 文档 → 图层像素坐标平移。 */
      const comp_rect b = target_layer->effective_bounds();
      std::vector<float> local(static_cast<size_t>(params.point_count) * 2u);
      for (int32_t i = 0; i < params.point_count * 2; ++i) {
        const int32_t coordinate = static_cast<int32_t>(params.points[i]);
        local[i] = static_cast<float>(coordinate - ((i & 1) ? b.y : b.x));
      }
      stamp_polyline(target_layer->pixels.writable_pixels().data(),
                     target_layer->pixels.stride(), target_layer->pixels.width,
                     target_layer->pixels.height, local.data(), params.point_count, params.brush);
      /*
       * 颗粒叠加在落笔之后（全图层范围，不是只在笔画上），口径对齐 NoisePixels.h：
       * amount 是 Photoshop 百分比（均匀 ±amount% 半程、高斯标准差 2/3），
       * alpha 不动、全透明像素不动。origin 取 effective_bounds 的原点，使噪声
       * 钉在**文档坐标系**上——同一笔在图层平移后得到同样的颗粒。
       */
      if (params.brush.grain_amount > 0.0f && params.brush.erase == 0) {
        noise_add_at(target_layer->pixels.writable_pixels().data(), target_layer->pixels.width,
                     target_layer->pixels.height, target_layer->pixels.stride(),
                     params.brush.grain_amount, params.brush.grain_gaussian,
                     params.brush.grain_monochromatic, params.brush.seed,
                     static_cast<int64_t>(b.x), static_cast<int64_t>(b.y));
      }
      break;
    }

    case kToolOpClone: {
      /* 文档 → 图层像素坐标平移（与 Stroke 同口径：只认 effective_bounds 原点）。 */
      const comp_rect b = target_layer->effective_bounds();
      std::vector<float> local(static_cast<size_t>(params.point_count) * 2u);
      for (int32_t i = 0; i < params.point_count * 2; ++i) {
        local[i] = static_cast<float>(params.points[i] - ((i & 1) ? b.y : b.x));
      }

      /*
       * 偏移口径（上游 cloneStrokeOffset）：
       *   aligned && carried -> 沿用宿主带回的偏移；否则 round(source - 首笔落点)。
       * 整段折线共用这一个偏移：上游的 clone 是「整笔跟着同一个源走」。
       */
      int32_t offset_x = 0;
      int32_t offset_y = 0;
      if (params.clone.aligned != 0 && params.clone.has_offset != 0) {
        offset_x = static_cast<int32_t>(std::llround(params.clone.offset_x));
        offset_y = static_cast<int32_t>(std::llround(params.clone.offset_y));
      } else {
        offset_x = static_cast<int32_t>(
            std::llround(params.clone.source_x - static_cast<double>(params.points[0])));
        offset_y = static_cast<int32_t>(
            std::llround(params.clone.source_y - static_cast<double>(params.points[1])));
      }

      /*
       * 源快照：**一笔开始前**取好，整笔只读它。
       *   - 只取当前层：直接复制图层原始像素，不经过 blend/opacity；
       *   - 整篇合成：composite_document 给的是画布尺寸的表面，这里按目标图层
       *     的包围盒原点逐像素搬进图层空间。HDR 文档的表面是 RGBA16F，先走
       *     hdr_to_rgba8（与显示口径一致，高光按白截断）。
       */
      std::vector<uint8_t> snapshot(static_cast<size_t>(target_layer->pixels.width) *
                                        static_cast<size_t>(target_layer->pixels.height) * 4u,
                                    0u);
      if (params.clone.sample_all_layers == 0) {
        for (int32_t y = 0; y < target_layer->pixels.height; ++y) {
          std::memcpy(snapshot.data() +
                          static_cast<size_t>(y) * static_cast<size_t>(target_layer->pixels.width) * 4u,
                      target_layer->pixels.row(y),
                      static_cast<size_t>(target_layer->pixels.width) * 4u);
        }
      } else {
        ImageBuffer composite;
        std::string comp_error;
        if (!composite_document(*document, &composite, &comp_error)) {
          ok = fail(COMP_ERR_INTERNAL, "clone could not compose the document: " + comp_error);
          break;
        }
        ImageBuffer converted;
        const ImageBuffer* display = &composite;
        if (composite.format == COMP_PIXEL_RGBA16F) {
          if (!hdr_to_rgba8(composite, &converted)) {
            ok = fail(COMP_ERR_INTERNAL, "clone could not flatten the HDR composite");
            break;
          }
          display = &converted;
        }
        for (int32_t y = 0; y < target_layer->pixels.height; ++y) {
          const int32_t doc_y = b.y + y;
          if (doc_y < 0 || doc_y >= display->height) continue;
          const uint8_t* src_row = display->row(doc_y);
          uint8_t* dst_row = snapshot.data() +
                             static_cast<size_t>(y) *
                                 static_cast<size_t>(target_layer->pixels.width) * 4u;
          for (int32_t x = 0; x < target_layer->pixels.width; ++x) {
            const int32_t doc_x = b.x + x;
            if (doc_x < 0 || doc_x >= display->width) continue;
            std::memcpy(dst_row + static_cast<size_t>(x) * 4u,
                        src_row + static_cast<size_t>(doc_x) * 4u, 4u);
          }
        }
      }

      /* 上游 CloneSettings.opacity 与画笔 pressure × flow 一起构成覆盖率峰值。 */
      const float peak = std::min(std::max(params.brush.pressure * params.brush.flow *
                                               params.clone.opacity,
                                           0.0f),
                                  1.0f);
      if (!clone_stamp_paint(target_layer->pixels.writable_pixels().data(),
                             target_layer->pixels.stride(), target_layer->pixels.width,
                             target_layer->pixels.height, snapshot.data(), offset_x, offset_y,
                             local.data(), params.point_count, params.brush.diameter,
                             params.brush.hardness, peak)) {
        ok = oom("clone ran out of memory");
      }
      break;
    }

    case kToolOpSmudge:
    case kToolOpLiquify: {
      const comp_rect b = target_layer->effective_bounds();
      std::vector<float> local(static_cast<size_t>(params.point_count) * 2u);
      for (int32_t i = 0; i < params.point_count * 2; ++i) {
        local[i] = static_cast<float>(params.points[i] - ((i & 1) ? b.y : b.x));
      }
      uint8_t* pixels = target_layer->pixels.writable_pixels().data();
      const bool ok_paint =
          params.kind == kToolOpSmudge
              ? smudge_paint(pixels, target_layer->pixels.stride(), target_layer->pixels.width,
                             target_layer->pixels.height, local.data(), params.point_count,
                             params.brush.diameter, params.brush.hardness, params.warp.strength)
              : liquify_push_paint(pixels, target_layer->pixels.stride(),
                                   target_layer->pixels.width, target_layer->pixels.height,
                                   local.data(), params.point_count, params.brush.diameter,
                                   params.brush.hardness, params.warp.strength);
      if (!ok_paint) ok = oom(params.kind == kToolOpSmudge ? "smudge ran out of memory"
                                                           : "liquify ran out of memory");
      break;
    }

    case kToolOpBlur: {
      /* 文档 → 图层像素坐标平移（与 Stroke / Clone 同口径）。 */
      const comp_rect b = target_layer->effective_bounds();
      std::vector<float> local(static_cast<size_t>(params.point_count) * 2u);
      for (int32_t i = 0; i < params.point_count * 2; ++i) {
        local[i] = static_cast<float>(params.points[i] - ((i & 1) ? b.y : b.x));
      }
      /*
       * 上游 blurSample：sigma = min(blurRadius / perPixel, sidePixels / 2)。
       * 本仓的笔刷几何本来就在**图层像素**空间（相当于上游已除过 perPixel），
       * 故只做较长边一半的上夹。样本在盖回之前取：同一笔的折线不会吃到自己
       * 刚模糊过的像素再糊一遍，重复落笔才会继续变软。
       */
      const float side = static_cast<float>(std::max(width, height));
      const float sigma = std::min(params.blur.radius, side * 0.5f);
      std::vector<uint8_t> blurred;
      if (!blur_rgba8_copy(target_layer->pixels.row(0), target_layer->pixels.width,
                           target_layer->pixels.height, target_layer->pixels.stride(), sigma,
                           &blurred)) {
        ok = fail(COMP_ERR_INTERNAL, "blur could not build the blurred sample");
        break;
      }
      blur_stamp_paint(target_layer->pixels.writable_pixels().data(),
                       target_layer->pixels.stride(), target_layer->pixels.width,
                       target_layer->pixels.height, blurred, local.data(), params.point_count,
                       params.brush);
      break;
    }

    case kToolOpHeal: {
      /* coverage 按图层像素空间解释，逐行按宿主 stride 取。 */
      std::vector<uint8_t> tight(width * height);
      for (size_t y = 0; y < height; ++y) {
        std::memcpy(tight.data() + y * width,
                    params.coverage + y * params.coverage_stride, width);
      }
      const int rc = spot_heal(target_layer->pixels.writable_pixels().data(), tight.data(), width,
                               height, target_layer->pixels.stride(), params.heal.opacity,
                               params.heal.mode, params.heal.seed);
      if (rc < 0) ok = oom("heal ran out of memory");
      break;
    }

    case kToolOpDither: {
      /* 域层 DitherParams -> 内核 DitherParams：字段名逐字对应，缺省项显式置 0。 */
      ::DitherParams p{};
      p.style = params.dither.style;
      p.levels = params.dither.levels;
      p.diffusion = params.dither.diffusion;
      p.density = params.dither.density;
      p.contrast = params.dither.contrast;
      p.cell = params.dither.cell;
      p.angle = params.dither.angle;
      p.lightOnDark = params.dither.light_on_dark;
      p.originalColors = params.dither.original_colors;
      for (int c = 0; c < 3; ++c) {
        p.dark[c] = params.dither.dark[c];
        p.light[c] = params.dither.light[c];
      }
      /*
       * 字形图集刻意不由 ABI 灌入（那是宿主资源，见 tool_ops.h）。GLYPHS 缺图集时
       * 内核走 spot() 的 default 分支 = 菱形网点：如实回报，不静默按方块画，
       * 因为把内核的菱形说成方块就是在撒谎。
       */
      p.glyphs = nullptr;
      p.glyphCoverage = nullptr;
      p.glyphCount = 0;
      p.glyphWidth = 0;
      p.glyphHeight = 0;
      if (p.style == DITHER_GLYPHS && note != nullptr) {
        *note += "glyph atlas not carried by the ABI; GLYPHS fell back to the diamond screen "
                 "(DitherPixels spot() default branch); ";
      }
      if (dither_apply(target_layer->pixels.writable_pixels().data(), width, height,
                       target_layer->pixels.stride(), &p) == 0) {
        ok = oom("dither ran out of memory");
      }
      break;
    }

    case kToolOpLens: {
      apply_lens(&target_layer->pixels, params.lens.k);
      break;
    }

    case kToolOpDeep: {
      if (params.deep_mode == kToolDeepQuantize) {
        /*
         * HDR 的 8 位副本走 hdr_to_rgba8：先 deep_half_to_u16 把 half 的**值**落成
         * 16 位整数，再 dither_quantize16 取整成 8 位（domain/layer.cpp 的两趟）。
         * 直接送进 deep_to_rgba8 会按 16 位整数读 half 的位模式——0x3C00（1.0）
         * 读成 15360，画面全是噪声。两条路都是「白点以上显示为白」。
         */
        const bool quantized = target_layer->pixels.format == COMP_PIXEL_RGBA16F
                                   ? hdr_to_rgba8(target_layer->pixels, &target_layer->pixels)
                                   : deep_to_rgba8(target_layer->pixels, &target_layer->pixels);
        if (!quantized) {
          ok = bad_state("deep quantize could not build the 8-bit working copy");
        }
        break;
      }
      /*
       * 往返：图层保持它的深度（RGBA16 仍是 16 位、RGBA16F 仍是 half），把 pixels
       * 穿过 `params.inner` 那个 8 位操作四遍（deep_split/deep_merge）。inner 为
       * none 时不做任何操作，直接把 low 档结果合回——这条路径等价于「8 位量化后
       * 又读回 16 位」，会丢掉 8 位以上的精度，如实写进 note 而不是当作无损往返。
       * HDR 走 deep_hdr_round_trip（上游 normalizing 那一层）。
       */
      {
        const bool is_hdr = target_layer->pixels.format == COMP_PIXEL_RGBA16F;
        const uint16_t* deep = reinterpret_cast<const uint16_t*>(target_layer->pixels.row(0));
        const size_t deep_stride = target_layer->pixels.stride();
        std::vector<uint16_t> merged(width * height * 4u, 0);

        /* 把 inner 那个 8 位操作包成 std::function：四趟各跑一遍同一个内核。 */
        auto apply_inner = [&](uint8_t* buf, size_t w, size_t h, size_t stride) -> bool {
          switch (params.inner) {
            case kToolOpNone:
              return true;   /* 不做操作：见上面的 note 说明 */
            case kToolOpLens: {
              if (params.lens.k == 0.0) return true;
              std::vector<uint8_t> src(buf, buf + stride * h);
              lens_distort(src.data(), buf, w, h, stride, params.lens.k);
              return true;
            }
            case kToolOpDither: {
              ::DitherParams p{};
              p.style = params.dither.style;
              p.levels = params.dither.levels;
              p.diffusion = params.dither.diffusion;
              p.density = params.dither.density;
              p.contrast = params.dither.contrast;
              p.cell = params.dither.cell;
              p.angle = params.dither.angle;
              p.lightOnDark = params.dither.light_on_dark;
              p.originalColors = params.dither.original_colors;
              for (int c = 0; c < 3; ++c) {
                p.dark[c] = params.dither.dark[c];
                p.light[c] = params.dither.light[c];
              }
              p.glyphs = nullptr;
              p.glyphCoverage = nullptr;
              p.glyphCount = 0;
              p.glyphWidth = 0;
              p.glyphHeight = 0;
              return dither_apply(buf, w, h, stride, &p) != 0;
            }
            default:
              /* 其余算子要么改几何（stroke/heal）、要么写蒙版（select），都不可
               * 降级成「对副本的像素变换」，故不列入；参数闸已拒绝。 */
              return false;
          }
        };

        std::string rt_error;
        const bool ran = is_hdr
                             ? deep_hdr_round_trip(deep, deep_stride, merged.data(), width * 8u, width,
                                                   height, apply_inner, /*mixes=*/0, &rt_error)
                             : deep_round_trip(deep, deep_stride, merged.data(), width * 8u, width,
                                               height, apply_inner, /*mixes=*/0, &rt_error);
        if (!ran) {
          ok = fail(COMP_ERR_INTERNAL, rt_error);
          break;
        }
        /* 结果写回（COW）：深度不变（RGBA16 仍 16 位、RGBA16F 仍 half），像素换成合回的结果。 */
        std::memcpy(target_layer->pixels.writable_pixels().data(), merged.data(), width * height * 8u);
        if (params.inner == kToolOpNone && note != nullptr) {
          *note += "deep round trip without an inner operation is a quantize-and-read-back: "
                   "it keeps the layer at 16 bits but loses precision above 8 bits; ";
        }
      }
      break;
    }

    case kToolOpScanlines: {
      /* 域层参数 -> 内核 ScanlinesParams（字段逐字对应，缺省项显式置 0）。 */
      ::ScanlinesParams p{};
      p.spacing = params.scanlines.spacing;
      p.thickness = params.scanlines.thickness;
      p.dots = params.scanlines.dots;
      p.wobble = params.scanlines.wobble;
      p.displace = params.scanlines.displace;
      p.threshold = params.scanlines.threshold;
      p.split = params.scanlines.split;
      p.density = params.scanlines.density;
      p.contrast = params.scanlines.contrast;
      p.blackLevel = params.scanlines.black_level;
      p.smoothness = params.scanlines.smoothness;
      p.originalColors = params.scanlines.original_colors;
      for (int c = 0; c < 3; ++c) {
        p.dark[c] = params.scanlines.dark[c];
        p.light[c] = params.scanlines.light[c];
      }
      if (scanlines_apply(target_layer->pixels.writable_pixels().data(), width, height,
                          target_layer->pixels.stride(), &p) == 0) {
        ok = oom("scanlines ran out of memory");
        break;
      }
      /*
       * glow > 0 时上游再加一遍「线外的光晕」：把线本身模糊后加回（磷光晕开）。
       * 上游用 Core Image 的高斯模糊；本仓没有该路径，用调整求值里已有的
       * 三趟盒式近似（adjustment_eval.cpp 的 blur）——同一份代码、同一个调用，
       * 免得再抄一份模糊实现出来。模糊在图层像素空间做，与 scanlines 同一坐标系。
       */
      if (params.scanlines.glow > 0.0f) {
        const ImageBuffer& src = target_layer->pixels;   /* 只读：row() 绝不触发复制 */
        const size_t sigma_px = static_cast<size_t>(params.scanlines.spacing) * 3u + 3u;
        std::vector<uint8_t> bloom;
        if (blur_rgba8_copy(src.row(0), src.width, src.height, src.stride(),
                            static_cast<float>(sigma_px), &bloom)) {
          /*
           * 上游 amount = glow/100 * 2.5（glow 已归一到 0..1，故 ×2.5）。
           * 内核自己反预乘/再预乘，此处不得动 alpha。
           */
          dither_glow(target_layer->pixels.writable_pixels().data(), bloom.data(), width, height,
                      target_layer->pixels.stride(),
                      std::min(params.scanlines.glow * 2.5f, 2.5f));
        } else {
          ok = oom("scanlines glow ran out of memory");
        }
      }
      break;
    }

    case kToolOpChunky: {
      /*
       * 上游口径（0bit/Dither.swift:98-127）：先把图降采样到 1/block，在**副本**
       * 上抖动（避免半调参数去改原图），再用无平滑的最近邻放大回去；pixelShape
       * 为 dot 时用 dither_dots 把块间缝隙填成 dark 色。ASCII 风格按像素尺寸走，
       * 与本算子无关，故不区分。
       */
      const int32_t block = params.chunky.block;
      const int32_t small_w = static_cast<int32_t>((width + static_cast<size_t>(block) - 1u) / static_cast<size_t>(block));
      const int32_t small_h = static_cast<int32_t>((height + static_cast<size_t>(block) - 1u) / static_cast<size_t>(block));
      if (small_w <= 0 || small_h <= 0) { ok = bad_arg("chunky block is larger than the layer"); break; }

      /* 降采样取小块内的直通 sRGB 均值（预乘均值的 alpha 会偏暗，故按直通道算）。 */
      std::vector<uint8_t> small(static_cast<size_t>(small_w) * static_cast<size_t>(small_h) * 4u, 0);
      for (int32_t y = 0; y < small_h; ++y) {
        for (int32_t x = 0; x < small_w; ++x) {
          uint32_t acc[4] = {0, 0, 0, 0};
          uint32_t n = 0;
          const int32_t x1 = std::min(static_cast<int32_t>(width), (x + 1) * block);
          const int32_t y1 = std::min(static_cast<int32_t>(height), (y + 1) * block);
          for (int32_t sy = y * block; sy < y1; ++sy) {
            const uint8_t* row = target_layer->pixels.row(sy);
            for (int32_t sx = x * block; sx < x1; ++sx) {
              const uint8_t* px = row + static_cast<size_t>(sx) * 4u;
              const uint32_t a = px[3];
                  uint32_t straight[3];
                  for (int c = 0; c < 3; ++c) straight[c] = a ? (px[c] * 255u + a / 2u) / a : 0u;
                  for (int c = 0; c < 3; ++c) acc[c] += straight[c];
                  acc[3] += a;
                  ++n;
                }
          }
          if (n == 0) continue;
          const uint32_t sa = acc[3] / n;
          uint8_t* out = small.data() + (static_cast<size_t>(y) * static_cast<size_t>(small_w) + static_cast<size_t>(x)) * 4u;
          for (int c = 0; c < 3; ++c) {
            const uint32_t sc = acc[c] / n;
            out[c] = static_cast<uint8_t>(sc > sa ? sa : sc);
          }
          out[3] = static_cast<uint8_t>(sa);
        }
      }

      /* 抖动落在副本上（同 DitherParams，字形图集仍不由 ABI 灌入）。 */
      ::DitherParams p{};
      p.style = params.dither.style;
      p.levels = params.dither.levels;
      p.diffusion = params.dither.diffusion;
      p.density = params.dither.density;
      p.contrast = params.dither.contrast;
      p.cell = params.dither.cell;
      p.angle = params.dither.angle;
      p.lightOnDark = params.dither.light_on_dark;
      p.originalColors = params.dither.original_colors;
      for (int c = 0; c < 3; ++c) {
        p.dark[c] = params.dither.dark[c];
        p.light[c] = params.dither.light[c];
      }
      if (p.style == DITHER_GLYPHS && note != nullptr) {
        *note += "glyph atlas not carried by the ABI; GLYPHS fell back to the diamond screen "
                 "(DitherPixels spot() default branch); ";
      }
      if (dither_apply(small.data(), static_cast<size_t>(small_w), static_cast<size_t>(small_h),
                       static_cast<size_t>(small_w) * 4u, &p) == 0) {
        ok = oom("chunky dither ran out of memory");
        break;
      }

      /* 最近邻放大回原尺寸（上游放大时不设插值质量，即不平滑）。 */
      std::vector<uint8_t>& full = target_layer->pixels.writable_pixels();
      for (int32_t y = 0; y < target_layer->pixels.height; ++y) {
        uint8_t* row = full.data() + static_cast<size_t>(y) * target_layer->pixels.stride();
        const size_t sy = std::min(static_cast<size_t>(y) / static_cast<size_t>(block),
                                   static_cast<size_t>(small_h) - 1u);
        const uint8_t* src = small.data() + sy * static_cast<size_t>(small_w) * 4u;
        for (int32_t x = 0; x < target_layer->pixels.width; ++x) {
          const size_t sx = std::min(static_cast<size_t>(x) / static_cast<size_t>(block),
                                     static_cast<size_t>(small_w) - 1u);
          std::memcpy(row + static_cast<size_t>(x) * 4u, src + sx * 4u, 4u);
        }
      }
      if (params.chunky.dot != 0) {
        uint8_t gap[3] = {0, 0, 0};
        for (int c = 0; c < 3; ++c) gap[c] = params.chunky.two_colors ? params.chunky.gap[c] : 0u;
        dither_dots(full.data(), width, height, target_layer->pixels.stride(), block, gap);
      }
      break;
    }

    default:
      ok = bad_arg("unknown tool op kind");
      break;
  }

  if (!ok) {
    /* 落笔族失败：把原始深像素放回去。调用方可能持有外层事务（owns_txn 为假），
     * 不能把半途的 8 位工作副本留在图层里。 */
    if (deep_paint.active()) target_layer->pixels = std::move(deep_paint.deep);
    if (owns_txn) document->txn_abort(&txn_error);
    return false;
  }
  if (deep_paint.active()) {
    if (!merge_deep_paint(target_layer->pixels, &deep_paint)) {
      target_layer->pixels = std::move(deep_paint.deep);
      if (owns_txn) document->txn_abort(&txn_error);
      if (error != nullptr) *error = "deep paint merge: buffer shape mismatch";
      if (out_code != nullptr) *out_code = COMP_ERR_INTERNAL;
      return false;
    }
    target_layer->pixels = std::move(deep_paint.deep);
  }
  if (owns_txn) {
    std::string commit_error;
    if (document->txn_commit(&commit_error) != COMP_OK) {
      if (error != nullptr) *error = commit_error;
      if (out_code != nullptr) *out_code = COMP_ERR_STATE;
      return false;
    }
  }
  document->mark_dirty(true);
  /*
   * 成功也回写 out_code = COMP_OK：调用方常把一个初值为 COMP_ERR_INTERNAL 的
   * 变量传进来当作「有没有出错」的标志，只在失败时写就等于让上一次失败的残值
   * 留在那里，把成功读成失败（域层测试里真踩到了这个）。
   */
  if (out_code != nullptr) *out_code = COMP_OK;
  return true;
}

}  // namespace compositor
