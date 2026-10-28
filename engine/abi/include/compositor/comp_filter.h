// AI生成
/*
 * comp_filter.h — Filter 菜单的 C ABI（P1 滤镜批次）。
 *
 * 语义唯一真源见 `domain/filters.h` 的文件头：十五个算子（Vignette / Tonal
 * Contrast / Bloom / Lens Correction / Gaussian Blur / Add Noise / Grain /
 * Motion Blur / Progressive Blur / Sharpen / Reduce Noise / High Pass /
 * Minimum / Maximum / Offset）的像素口径、选区混合与能力边界都写在那里。
 *
 * 本头文件只暴露一件事：一次「整层」滤镜调用。
 *
 *   - 参数是面板设置（量程对齐上游 `FilterSettings.normalized`）；
 *   - **零初始化 = 恒等**：各子结构的 amount / distortion 缺省为 0，此时不动
 *     像素、不开事务（out_changed = 0）。宿主需要面板缺省值时自己填（上游缺省
 *     见 docs/filters.md），ABI 不替宿主决定「用户想要什么效果」；
 *   - 越界参数拒绝而不是静默 clamp（与 camera_raw 同规矩；上游是 clamp，
 *     这里刻意更严格，避免宿主传错量程却静默生效半个效果）；
 *   - 一步撤销：加入调用方已开启的显式事务，或自成一个隐式步；
 *   - 有选区时结果按 coverage 与选区外的原像素混合（文档坐标）。
 *
 * 能力边界（与 domain/filters.h 一致）：
 *   16 位图层先落 8 位工作副本（note 写明），HDR 图层 -> COMP_ERR_STATE；
 *   组图层 / 无像素图层 -> COMP_ERR_STATE。
 */
#ifndef COMPOSITOR_COMP_FILTER_H
#define COMPOSITOR_COMP_FILTER_H

#include <stddef.h>
#include <stdint.h>

#include "comp_core.h"
#include "comp_layer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 滤镜种类。数值与域层 `FilterKind` 一一对应（尾部追加，不改既有值）。 */
typedef enum comp_filter_kind {
  COMP_FILTER_VIGNETTE        = 0,
  COMP_FILTER_TONAL_CONTRAST  = 1,
  COMP_FILTER_BLOOM           = 2,
  COMP_FILTER_LENS_CORRECTION = 3,
  COMP_FILTER_GAUSSIAN_BLUR   = 4,
  COMP_FILTER_ADD_NOISE       = 5,
  COMP_FILTER_GRAIN           = 6,
  COMP_FILTER_MOTION_BLUR     = 7,
  COMP_FILTER_PROGRESSIVE_BLUR = 8,
  COMP_FILTER_SHARPEN          = 9,
  COMP_FILTER_REDUCE_NOISE     = 10,
  COMP_FILTER_HIGH_PASS        = 11,
  COMP_FILTER_MINIMUM          = 12,
  COMP_FILTER_MAXIMUM          = 13,
  COMP_FILTER_OFFSET           = 14,
  COMP_FILTER_CUSTOM           = 15,
  /* A45（2026-10-27）尾部追加：去噪族的两项。 */
  COMP_FILTER_MEDIAN           = 16, /* 中间值：窗口取中位色 */
  COMP_FILTER_DUST_SCRATCHES   = 17, /* 蒙尘与划痕：超阈值才替换为中位数 */
  /* A46（2026-10-28）尾部追加：扭曲 / 像素化 / 风格化三组。 */
  COMP_FILTER_SPHERIZE         = 18, /* 球面化：amount -100..100，0 = 恒等 */
  COMP_FILTER_PINCH            = 19, /* 挤压：amount -100..100，0 = 恒等 */
  COMP_FILTER_TWIRL            = 20, /* 旋转扭曲：angle -999..999，0 = 恒等 */
  COMP_FILTER_MOSAIC           = 21, /* 马赛克：cell_size 0 = 恒等，否则 2..200 */
  COMP_FILTER_EMBOSS           = 22, /* 浮雕：amount 0 = 恒等，否则 1..500 */
  /* A47（2026-10-28）尾部追加：渲染（Render）组三项。 */
  COMP_FILTER_CLOUDS           = 23, /* 云彩：amount 0 = 恒等，否则 1..100 */
  COMP_FILTER_DIFFERENCE_CLOUDS = 24, /* 分层云彩：amount 0 = 恒等，否则 1..100 */
  COMP_FILTER_PICTURE_FRAME    = 25  /* 画框：width 0 = 恒等，否则 1..500 */
} comp_filter_kind;

/*
 * Vignette（上游 FilterSettings.vignette*）。
 *   amount      0..100，0 = 恒等
 *   midpoint    0..100（羽化起点）
 *   roundness   -100..100（负数 = 方角）
 *   feather     0..100
 *   highlights  0..100（高光保护）
 *   color       直通 sRGB 0..1 的边缘色
 */
typedef struct comp_filter_vignette {
  double amount;
  double midpoint;
  double roundness;
  double feather;
  double highlights;
  double color[3];
} comp_filter_vignette;

/*
 * Tonal Contrast（上游 FilterSettings.tonal*）。
 *   amount      0..100（= 内核 strength * 50）
 *   radius      1..100（高斯 detail 半径，图层像素；预览再乘 scale）
 *   shadows / midtones / highlights  -100..100
 */
typedef struct comp_filter_tonal_contrast {
  double amount;
  double radius;
  double shadows;
  double midtones;
  double highlights;
} comp_filter_tonal_contrast;

/* Bloom / Glow（上游 FilterSettings.bloom*）。amount 0..100，radius 1..150。 */
typedef struct comp_filter_bloom {
  double amount;
  double radius;
} comp_filter_bloom;

/* Lens Correction（上游 FilterSettings.distortion）。-100..100，0 = 恒等。 */
typedef struct comp_filter_lens_correction {
  double distortion;
} comp_filter_lens_correction;

/*
 * Gaussian Blur（上游 FilterSettings.radius / preserveTransparency）。
 *   radius                0 = 恒等；否则 0.1..250（图层像素，预览再乘 scale）
 *   preserve_transparency 1 = 只模糊颜色，轮廓与原 alpha 不动
 */
typedef struct comp_filter_gaussian_blur {
  double radius;
  int32_t preserve_transparency;
} comp_filter_gaussian_blur;

/*
 * Add Noise（上游 FilterSettings.amount / gaussian / monochromatic / seed）。
 *   amount          0 = 恒等；否则 0.1..400（Photoshop 百分比）
 *   gaussian        1 = 高斯分布，0 = 均匀分布
 *   monochromatic   1 = 三通道加同一个值（只改亮度）
 *   seed            噪声场种子；同 seed + 同图层原点逐字节可复现
 */
typedef struct comp_filter_add_noise {
  double amount;
  int32_t gaussian;
  int32_t monochromatic;
  uint32_t seed;
} comp_filter_add_noise;

/*
 * Grain（上游 FilterSettings.grain / GrainSettings）。
 *   amount     0..100，0 = 恒等
 *   size       0.5..20（颗粒尺度，文档像素）
 *   roughness  0..100
 *   seed       图案种子
 */
typedef struct comp_filter_grain {
  double amount;
  double size;
  double roughness;
  uint32_t seed;
} comp_filter_grain;

/*
 * Motion Blur（上游 FilterSettings.angle / distance，CIMotionBlur）。
 *   angle     -90..90（度；水平为 0，逆时针为正，Photoshop 口径）
 *   distance  0 = 恒等；否则 1..2000（条纹长度，图层像素；预览再乘 scale）
 */
typedef struct comp_filter_motion_blur {
  double angle;
  double distance;
} comp_filter_motion_blur;

/*
 * Progressive Blur（上游 FilterSettings.progressive*，CIMaskedVariableBlur）。
 *   from / to   0..250（渐变轴两端的模糊半径，图层像素；预览再乘 scale）
 *   start_x/y   轴起点的文档坐标（连续；auto_axis = 1 时忽略）
 *   end_x/y     轴终点的文档坐标（同上）
 *   radial        1 = 以 start 为心、|end - start| 为半径的径向渐变
 *   inverted      1 = 交换 from/to（等价于把轴掉头）
 *   explicit_axis 0 = 用缺省轴（图层自身像素矩形中线、自顶向下）；
 *                 1 = 用 start/end（文档坐标）
 *
 * 零初始化（from = to = 0，explicit_axis = 0）= 恒等 + 缺省轴：C 侧与域层
 * （`auto_axis = true`）的零初始化口径一致，不把 (0,0)-(0,0) 误读成退化轴。
 */
typedef struct comp_filter_progressive_blur {
  double from;
  double to;
  double start_x;
  double start_y;
  double end_x;
  double end_y;
  int32_t radial;
  int32_t inverted;
  int32_t explicit_axis;
} comp_filter_progressive_blur;

/*
 * Sharpen（Unsharp Mask，上游 Filter > Sharpen > Unsharp Mask）。
 *   amount     0 = 恒等；否则 0.1..500（百分比，上游量程）
 *   radius     0.1..250（高斯 detail 半径，图层像素；预览再乘 scale）
 *   threshold  0..255（通道差低于它不动，避免放大噪点）
 *
 * 口径 = 经典 USM：`src + amount/100 * (src - blur(src))`，alpha 不变。
 */
typedef struct comp_filter_sharpen {
  double amount;
  double radius;
  double threshold;
} comp_filter_sharpen;

/*
 * Reduce Noise（上游 Filter > Noise > Reduce Noise 的常用子集）。
 *   strength             0 = 恒等；否则 0.1..10（上游量程 0..10）
 *   preserve_details     0..100（越高越保留细节、平滑越弱）
 *   reduce_color_noise   0..100（只平滑色度，亮度不动）
 *
 * 未暴露 `sharpen_details`（上游对话框第四行）：锐化已有独立滤镜，
 * 不把两条锐化通路塞进同一个算子；边界记在 docs/filters.md §3.11。
 */
typedef struct comp_filter_reduce_noise {
  double strength;
  double preserve_details;
  double reduce_color_noise;
} comp_filter_reduce_noise;

/*
 * High Pass（上游 Filter > Other > High Pass）。
 *   radius  0 = 恒等；否则 0.1..250（图层像素，预览再乘 scale）
 *
 * 口径 = `128 + (src - blur(src))`（直通色），alpha 不变：平坦区域变中灰，
 * 细节以中灰为轴上下偏移。模糊与 Sharpen / Gaussian Blur 同一实现。
 */
typedef struct comp_filter_high_pass {
  double radius;
} comp_filter_high_pass;

/*
 * Minimum / Maximum（上游 Filter > Other > Minimum / Maximum）。
 *   radius  0 = 恒等；否则 1..100（方形窗口半径，整数像素；预览再乘 scale）
 *
 * 四个通道一起做（alpha 参与），半透明轮廓会跟着形状收缩 / 扩张；
 * 窗口在图层边界处裁剪，不复制边缘像素。
 */
typedef struct comp_filter_minimum {
  double radius;
} comp_filter_minimum;

typedef struct comp_filter_maximum {
  double radius;
} comp_filter_maximum;

/*
 * Median / Dust & Scratches 共用（A45，2026-10-27 尾部追加）。
 *   radius     0 = 恒等（不开事务）；1..50 像素
 *   threshold  仅 Dust & Scratches 使用：0 = 等同 Median；
 *              1..255 = 「中心与中位数的差超过它才替换」
 */
typedef struct comp_filter_denoise {
  double radius;
  int32_t threshold;
} comp_filter_denoise;

/*
 * Spherize（上游 Filter > Distort > Spherize）。
 *   amount  -100..100，0 = 恒等；正值 = 球面（中心放大），负值 = 凹面。
 */
typedef struct comp_filter_spherize {
  double amount;
} comp_filter_spherize;

/*
 * Pinch（上游 Filter > Distort > Pinch）。
 *   amount  -100..100，0 = 恒等；正值 = 向内挤压，负值 = 向外扩张。
 */
typedef struct comp_filter_pinch {
  double amount;
} comp_filter_pinch;

/*
 * Twirl（上游 Filter > Distort > Twirl）。
 *   angle  -999..999（度），0 = 恒等；边缘（r >= 1）不转。
 */
typedef struct comp_filter_twirl {
  double angle;
} comp_filter_twirl;

/*
 * Mosaic（上游 Filter > Pixelate > Mosaic）。
 *   cell_size  0 = 恒等；否则 2..200（方格边长，图层像素；预览再乘 scale）。
 */
typedef struct comp_filter_mosaic {
  double cell_size;
} comp_filter_mosaic;

/*
 * Emboss（上游 Filter > Stylize > Emboss）。
 *   amount  0 = 恒等；否则 1..500（百分比）
 *   angle   -360..360（光照方向，度）
 *   height  0..10（采样间距，图层像素；预览再乘 scale 后至少 1）
 */
typedef struct comp_filter_emboss {
  double amount;
  double angle;
  double height;
} comp_filter_emboss;

/*
 * Clouds（上游 Filter > Render > Clouds）。
 *   amount      0 = 恒等；否则 1..100（云彩覆盖强度百分比）
 *   size        2..1000（噪声尺度，文档像素；越大云团越大）
 *   seed        噪声场种子；同 seed + 同图层原点逐字节可复现
 *   color_low   直通 sRGB 0..1 的暗端色（上游缺省黑）
 *   color_high  直通 sRGB 0..1 的亮端色（上游缺省白）
 *
 * 口径 = 5 八度值噪声 fBm，按文档坐标锚定；输出逐字节保留原 alpha（透明区
 * 不上色）。上游会铺满整张画布（透明底也照铺），本仓只改图层已有像素，差异
 * 记入 note。`amount` 作云彩与原像素的混合权重。
 */
typedef struct comp_filter_clouds {
  double amount;
  double size;
  uint32_t seed;
  double color_low[3];
  double color_high[3];
} comp_filter_clouds;

/*
 * Difference Clouds（上游 Filter > Render > Difference Clouds）。
 *   amount  0 = 恒等；否则 1..100
 *   size    2..1000（噪声尺度，文档像素）
 *   seed    噪声场种子
 *
 * 口径 = 灰度云纹与原像素做差分（`|cloud - src|`），再按 amount 混合。
 */
typedef struct comp_filter_difference_clouds {
  double amount;
  double size;
  uint32_t seed;
} comp_filter_difference_clouds;

/*
 * Picture Frame（上游 Filter > Render > Picture Frame 的扁平色带子集）。
 *   width  0 = 恒等；否则 1..500（四边色带厚度，图层像素；预览再乘 scale）
 *   color  直通 sRGB 0..1 的边框色
 *
 * 口径 = 四边等厚纯色环（像素到最近图层边界的距离 < width*scale 即着色），
 * alpha 逐字节不变。上游是带浮雕的立体相框，本仓只做扁平色带，差异记入 note。
 */
typedef struct comp_filter_picture_frame {
  double width;
  double color[3];
} comp_filter_picture_frame;

/* Offset 的露出区域处理方式（数值即域层 `OffsetSettings.wrap`）。 */
typedef enum comp_offset_wrap {
  COMP_OFFSET_TRANSPARENT = 0, /* 露出部分留空（不环绕） */
  COMP_OFFSET_EDGE        = 1, /* 用最外一圈像素填满露出区域 */
  COMP_OFFSET_WRAP        = 2  /* 移出去的内容从对侧卷回来 */
} comp_offset_wrap;

/*
 * Offset（上游 Filter > Other > Offset）。
 *   horizontal / vertical  -10000..10000（图层像素，四舍五入到整像素）
 *   wrap                   comp_offset_wrap；取整后位移都为 0 = 恒等
 *
 * 整像素平移、不重采样；位移为整数时逐字节可逆。
 */
typedef struct comp_filter_offset {
  double horizontal;
  double vertical;
  int32_t wrap;
} comp_filter_offset;

/*
 * Custom（上游 Filter > Other > Custom）。
 *   kernel[25]  5×5 卷积核，行优先（第 0 行是最上面一行）
 *   scale       除数；0 按 1 处理（面板可以把「Scale」清成 0 而不除零）
 *   offset      每通道在除法之后加的常数
 *
 * 量程：核项 / scale / offset 都在 -999..999，越界拒绝（不静默夹取）。
 * 卷积在直通色上做、alpha 逐字节不变；邻居坐标夹到图层内（边缘延展）。
 *
 * **零初始化 = 恒等**：kernel 全 0 且 scale 0 且 offset 0（即调用方根本没填这个
 * 子结构）按恒等核处理，与 desc 其余字段「零值 = 恒等」的口径一致。显式想要
 * 全 0 输出（黑）请把 scale 置 1 或 offset 置非 0。
 */
typedef struct comp_filter_custom {
  double kernel[25];
  double scale;
  double offset;
} comp_filter_custom;

/*
 * 一次滤镜调用。只有 `kind` 对应的那个子结构被读取；其余忽略。
 * 零初始化 = Vignette + amount 0 = 恒等。
 *
 * `sharpen` / `reduce_noise` 是尾部追加（kind 9 / 10），`high_pass` /
 * `minimum` / `maximum` / `offset` 是再一批尾部追加（kind 11..14），`custom`
 * 是第四批尾部追加（kind 15）：旧宿主按旧头文件编译时描述符更小，**新宿主必须
 * 用同一份头文件**——本仓两端同源构建，不提供跨版本结构体协商（导出符号表
 * 仍只做尾部追加）。
 */
typedef struct comp_filter_desc {
  comp_filter_kind             kind;
  comp_filter_vignette         vignette;
  comp_filter_tonal_contrast   tonal_contrast;
  comp_filter_bloom            bloom;
  comp_filter_lens_correction  lens_correction;
  comp_filter_gaussian_blur    gaussian_blur;
  comp_filter_add_noise        add_noise;
  comp_filter_grain            grain;
  comp_filter_motion_blur      motion_blur;
  comp_filter_progressive_blur progressive_blur;
  comp_filter_sharpen          sharpen;
  comp_filter_reduce_noise     reduce_noise;
  comp_filter_high_pass        high_pass;
  comp_filter_minimum          minimum;
  comp_filter_maximum          maximum;
  comp_filter_offset           offset;
  comp_filter_custom           custom;
  comp_filter_denoise          median;         /* A45：kind == MEDIAN */
  comp_filter_denoise          dust_scratches; /* A45：kind == DUST_SCRATCHES */
  /* A46 尾部追加：扭曲 / 像素化 / 风格化三组。旧宿主按旧头文件编译时描述符更小，
   * 新宿主必须用同一份头文件（两端同源构建，不提供跨版本结构体协商）。 */
  comp_filter_spherize         spherize;       /* kind == SPHERIZE */
  comp_filter_pinch            pinch;          /* kind == PINCH */
  comp_filter_twirl            twirl;          /* kind == TWIRL */
  comp_filter_mosaic           mosaic;         /* kind == MOSAIC */
  comp_filter_emboss           emboss;         /* kind == EMBOSS */
  /* A47 尾部追加：渲染组三项。旧宿主按旧头文件编译时描述符更小，新宿主必须用
   * 同一份头文件（两端同源构建，不提供跨版本结构体协商）。 */
  comp_filter_clouds           clouds;            /* kind == CLOUDS */
  comp_filter_difference_clouds difference_clouds; /* kind == DIFFERENCE_CLOUDS */
  comp_filter_picture_frame    picture_frame;     /* kind == PICTURE_FRAME */
} comp_filter_desc;

/*
 * 对目标图层跑一次滤镜。成功返回 COMP_OK。
 *
 * `scale` 是预览像素 / 图层像素（<= 0 视为 1）：影响以像素为单位的半径与位移
 * （Tonal Contrast / Bloom / Gaussian Blur / Motion Blur / Progressive Blur /
 * Sharpen / High Pass / Minimum / Maximum / Offset），让降采样预览看起来与
 * 全尺寸一致。Custom 的卷积核不随 scale 缩放（核没有连续的半径语义）。
 *
 * 错误码：
 *   参数越界 / 非有限 / desc 为 NULL -> COMP_ERR_INVALID_ARGUMENT
 *   图层不存在                       -> COMP_ERR_NOT_FOUND
 *   组图层 / 无像素 / HDR 图层        -> COMP_ERR_STATE
 *   分配失败                         -> COMP_ERR_OUT_OF_MEMORY
 *
 * note 可为 NULL；非 NULL 时写入 UTF-8 非致命说明（16 位降位、选区裁切、
 * Bloom / Gaussian Blur / Motion Blur / Progressive Blur / Custom 的近似口径），两段式：
 * note_capacity 为 0 时只回报所需长度，out_note_size 写入含 '\0' 的长度。
 * out_changed 可为 NULL。
 */
COMP_API comp_result comp_filter_apply(comp_document document, comp_layer_id target,
                                       const comp_filter_desc* desc, double scale,
                                       int32_t* out_changed, char* note, size_t note_capacity,
                                       size_t* out_note_size);

/* ========================================= 智能对象 / 智能滤镜（P1-2 收尾 ⑦）
 *
 * 语义见 domain/layer.h 的 `SmartObject`：`source`（原始像素，不可变）+ `filters`
 * （有序滤镜栈）是真源，图层像素是它俩的**求值缓存**。
 *
 * 通用口径：
 *   - 每一步写操作一步撤销（加入调用方已开的事务，或自成一个隐式步）；
 *   - 参数闸 / 求值失败都在开事务之前：失败不留撤销步、不动像素；
 *   - 求值在**对象像素空间**做（原点 0,0、不读选区）：图层移动/缩放/旋转到哪，
 *     同一条滤镜都渲出逐字节相同的结果；
 *   - 智能图层的像素写入走本组入口；comp_layer_set_pixels、工具算子与
 *     comp_filter_apply 一律 COMP_ERR_STATE（要求先栅格化）。
 */

/*
 * 把当前像素层转换为智能对象：此刻的像素成为不可变的 source，滤镜栈为空
 * （画面逐字节不变）。已经是智能对象 -> COMP_OK、*out_changed = 0（幂等，
 * 不落撤销步）。文字图层 -> COMP_ERR_STATE（先 rasterize 文字）。
 * 组图层 / 无像素 -> COMP_ERR_STATE。16 位图层先落 8 位工作副本（与滤镜
 * 同一约定，之后该层就是 RGBA8）；HDR 图层 -> COMP_ERR_STATE。
 * `out_changed` 可为 NULL。一步撤销。
 */
COMP_API comp_result comp_layer_smart_object_create(comp_document document, comp_layer_id id,
                                                    int32_t* out_changed);

/* 1 = 该层是智能对象（像素是求值缓存），0 = 普通图层。只读，不开事务。 */
COMP_API comp_result comp_layer_smart_object_is_smart(comp_document document, comp_layer_id id,
                                                      int32_t* out_is_smart);

/*
 * 栅格化：丢掉 source 与整条滤镜栈，**保留当前渲染出的像素**（不可逆的可编辑性
 * 损失）。普通图层 -> COMP_OK（幂等，不落撤销步）。一步撤销。
 */
COMP_API comp_result comp_layer_smart_object_rasterize(comp_document document, comp_layer_id id);

/* 滤镜栈条数（含被停用的）。只读。图层不是智能对象 -> COMP_ERR_STATE。 */
COMP_API comp_result comp_layer_smart_filter_count(comp_document document, comp_layer_id id,
                                                   size_t* out_count);

/*
 * 取第 index 条：整份设置写回 `out_desc`（其 `kind` 与条目一致），启用状态写回
 * `out_enabled`（0/1）。两个出参均可为 NULL。只读。
 * index 越界 -> COMP_ERR_INVALID_ARGUMENT；不是智能对象 -> COMP_ERR_STATE。
 */
COMP_API comp_result comp_layer_smart_filter_at(comp_document document, comp_layer_id id,
                                                size_t index, comp_filter_desc* out_desc,
                                                int32_t* out_enabled);

/*
 * 追加一条滤镜并立即重新求值（source 从头跑整条栈）。新条目 enabled = 1。
 * 恒等参数也照常入栈（画面不变，但滤镜栈变了——宿主之后可改参数）。
 * 参数越界 -> COMP_ERR_INVALID_ARGUMENT；不是智能对象 / 无 source -> COMP_ERR_STATE。
 * 一步撤销。
 */
COMP_API comp_result comp_layer_smart_filter_add(comp_document document, comp_layer_id id,
                                                 const comp_filter_desc* desc);

/*
 * 替换第 index 条的设置与启用状态，然后重新求值。
 * (settings, enabled) 与当前条目完全相同 -> COMP_OK、不落撤销步。
 * 参数越界 / index 越界 -> COMP_ERR_INVALID_ARGUMENT；不是智能对象 -> COMP_ERR_STATE。
 * 一步撤销。
 */
COMP_API comp_result comp_layer_smart_filter_set(comp_document document, comp_layer_id id,
                                                 size_t index, const comp_filter_desc* desc,
                                                 int32_t enabled);

/* 删除第 index 条并重新求值（删到空栈时画面回到 source）。一步撤销。 */
COMP_API comp_result comp_layer_smart_filter_remove(comp_document document, comp_layer_id id,
                                                    size_t index);

/* ========================================= 智能滤镜栈顺序与命名（P1-2 收尾 ⑩）
 *
 * 尾部追加（P1-2 第九批之后）：栈序就是求值顺序（自下而上，索引 0 先跑），
 * 改名只是面板展示名、不参与求值。两者都随快照进撤销栈与 `.comp`。
 */

/* 展示名字节上限（UTF-8，不含 '\0'），与 domain/layer.h 的
 * kMaxSmartFilterNameBytes 必须一致。 */
#define COMP_SMART_FILTER_NAME_MAX_BYTES 256

/*
 * 把第 `from` 条移到第 `to` 位（两者都是移动**前**的索引；语义同
 * std::vector 的「取出再插入」），然后按新顺序重新求值。
 *
 *   - from == to：COMP_OK、不重新求值、不落撤销步（宿主可无脑调用）；
 *   - 移动后栈序不变（例如把唯一一条移动 0→0 之外的空栈）不可能发生——越界即错误；
 *   - 索引越界 -> COMP_ERR_INVALID_ARGUMENT；不是智能对象 -> COMP_ERR_STATE；
 *   - 与其它写入口同一把锁闸（COMP_LAYER_LOCK_PIXELS）：顺序改变会改画面；
 *   - 求值在开事务之前：失败不留撤销步、不动像素。一步撤销。
 *
 * 注意 `to` 可以是 `count - 1`（移到栈顶）与 0（移到栈底），不要求 `to < count`
 * 之外的其它约定；`to == count` 视为越界（栈长度不变）。
 */
COMP_API comp_result comp_layer_smart_filter_move(comp_document document, comp_layer_id id,
                                                  size_t from, size_t to);

/*
 * 设置第 index 条的展示名（`name_utf8` 可为 NULL 或空串 = 清除名字，回退到
 * 滤镜类型名）。UTF-8、单行、≤ COMP_SMART_FILTER_NAME_MAX_BYTES 字节（不含
 * '\0'）；含控制字符、非法 UTF-8 或超长 -> COMP_ERR_INVALID_ARGUMENT。
 *
 * 名字**不参与像素求值**：与当前名字相同 -> COMP_OK、不落撤销步；否则只改
 * 元数据、不重新求值（画面逐字节不变），但仍进快照——一次撤销。不触发
 * 像素锁闸（与色标/链接同一口径：它不改画面）。不是智能对象 -> COMP_ERR_STATE。
 */
COMP_API comp_result comp_layer_smart_filter_set_name(comp_document document, comp_layer_id id,
                                                      size_t index, const char* name_utf8);

/*
 * 取第 index 条的展示名（两段式，口径同 comp_layer_comp_name）：`out_required`
 * 写含 '\0' 的所需长度；`buffer == NULL` 或 `capacity == 0` 为查询模式。
 * 名字为空（未命名）时长度为 1（只有 '\0'）。缓冲区不足 ->
 * COMP_ERR_INVALID_ARGUMENT，不截断。index 越界 -> COMP_ERR_INVALID_ARGUMENT；
 * 不是智能对象 -> COMP_ERR_STATE。只读，不开事务。
 */
COMP_API comp_result comp_layer_smart_filter_name(comp_document document, comp_layer_id id,
                                                  size_t index, char* buffer, size_t capacity,
                                                  size_t* out_required);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_FILTER_H */
