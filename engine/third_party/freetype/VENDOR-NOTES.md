# FreeType 2.13.3（裁剪版）

- 上游：<https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz>
- 许可证：FreeType License（见 `LICENSE.TXT`）与 FTL（见 `FTL.TXT`）；两者都要求在
  文档/关于信息里署名，见 `docs/THIRD-PARTY.md`。
- 本地改动**只有四处**：删掉未使用的 `src/` 模块目录、按裁剪结果重写
  `include/freetype/config/ftmodule.h`、在 `ftoption.h` 关闭 `FT_CONFIG_OPTION_USE_ZLIB`、新增本说明。
  其余库代码一行未改，升级时可直接替换整棵树再重放这四处。
- 不支持 gzip 压缩字体与 WOFF/WOFF2 压缩表（`src/gzip` 未 vendored，OHOS 侧也没有 zlib 包含路径）；
  普通 `.ttf` / `.otf` / `.ttc` 不受影响。这条在三条构建路径上一致，不制造宿主/鸿蒙分叉。

## 保留的模块

| 目录 | 作用 |
|------|------|
| `src/base` | 内存/流/对象/轮廓/位图/字形（`ftbase.c` 聚合） |
| `src/sfnt` | SFNT 表（TrueType 与 OpenType 共用） |
| `src/truetype` | TrueType 字体驱动（`.ttf` / `.ttc`） |
| `src/cff` | CFF/OpenType 字体驱动（Noto Sans SC 就是 CFF） |
| `src/psaux` | CFF 需要的 PostScript 解析 |
| `src/pshinter` | CFF 需要的 PS hinting（`FT_LOAD_NO_HINTING` 下不启用，但模块必须链接） |
| `src/psnames` | 字形名 |
| `src/smooth` | 灰度抗锯齿光栅器（唯一需要的 rasterizer） |

删掉：`autofit`、`type1`、`cid`、`pfr`、`type42`、`winfonts`、`pcf`、`bdf`、`raster`、`sdf`、
`svg`、`cache`、`gzip`、`lzw`、`bzip2`、`gxvalid`、`otvalid`、`tools`。

## 编译口径（重要）

**不能 glob 全部 `.c`。** FreeType 每个模块只有一个聚合翻译单元（如 `src/truetype/truetype.c`），
它用 `#include "ttobjs.c"` 这种方式把同目录其余 `.c` 包含进来；若这些文件再被单独编译，
就会出现重复符号。三条构建路径都使用下面的显式清单，并且把 `third_party/freetype`
从 `engine/**/*.c` 的 glob 里排除：

```
src/base/ftsystem.c  src/base/ftinit.c   src/base/ftdebug.c
src/base/ftbase.c    src/base/ftbbox.c   src/base/ftglyph.c   src/base/ftbitmap.c
src/base/ftmm.c
src/sfnt/sfnt.c      src/truetype/truetype.c
src/cff/cff.c        src/psaux/psaux.c   src/pshinter/pshinter.c
src/psnames/psnames.c src/smooth/smooth.c
```

包含路径只加 `engine/third_party/freetype/include`，与上游 `docs/INSTALL.ANY` 一致。

## 本仓渲染口径

- 只用灰度 coverage：`FT_RENDER_MODE_NORMAL`，禁用 LCD/子像素与 SDF。
- 固定 `FT_LOAD_NO_HINTING`，关闭 autohinter：同一字体同一字号在 Windows / OHOS
  产出逐字节相同的位图，视觉回归才有可比性。
- FreeType 的 `FT_Library` / `FT_Face` 不是线程安全的。渲染并行度见
  `docs/shell-parity.md` §5.45（P5-2）；不要把同一个 `FT_Face` 交给两个线程。
