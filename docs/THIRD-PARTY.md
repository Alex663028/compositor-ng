# 第三方组件与许可证义务

`engine/third_party/` 下只有源码，没有预编译二进制；每个组件的许可证原文随源码入仓。
字体资源放在 `resources/fonts/`（不在 `engine/third_party/` 下），但同属第三方资产，
许可义务一视同仁。本文件是它们在本仓中的**用途与许可证义务**的唯一索引，随组件增删同步更新。

| 组件 | 版本 | 许可证 | 许可证原文 | 本仓用途 |
|------|------|--------|-----------|----------|
| FreeType | 2.13.3（裁剪） | FreeType License (FTL) | `engine/third_party/freetype/LICENSE.TXT`（另有 `FTL.TXT` 为 GPLv2 选项，本仓不用） | 字体栅格化：`.ttf` / `.otf` / `.ttc` 的轮廓解析与灰度 coverage 位图 |
| zlib | 见 `README.zlib` | zlib License | `engine/third_party/zlib/README.zlib` | DEFLATE：PNG 解码、部分 EXR 的 ZIP/ZIPS 压缩 |
| libpng | 1.6.x | PNG Reference Library License v2 | `engine/third_party/libpng/LICENSE` | PNG 编解码（`codec_lib.cpp` 的 `__has_include("png.h")` 探测路径） |
| stb_image / stb_image_write | 2.30 | Public Domain / MIT 双许可 | 各头文件末尾的 LICENSE 段 | JPEG 等无独立库格式的回落解码；PNG 写入的备用路径 |
| Noto Sans SC（裁剪子集） | 见 `resources/fonts/README.md` | SIL Open Font License 1.1 | `resources/fonts/OFL.txt` | 文字工具（P5 起）随仓的可加载字体；`scripts/make_font_subset.py` 固定到 Regular 并裁剪字符集 |

## 义务摘要

上表五个组件都是**宽松/开源许可**，共同要求是「保留版权声明与许可证原文，并在文档/关于信息里署名」。

- 再分发时必须同时带上上表「许可证原文」列指向的文件；本仓已把它们逐字入仓，
  打包时不要裁剪 `engine/third_party/*/` 下的许可证文件。
- FreeType 用 FTL 而非 GPLv2：**不要**把它当作 GPL 组件处理，也不要在发行说明里写
  「本产品包含 GPL 代码」。
- 字体按 OFL 1.1 分发：**不得单独售卖字体本身**；本仓的 subset 是修改版，必须继续使用
  OFL，且不得使用源字体的保留名——产物族名已标为 `Noto Sans SC Subset`，不冒充完整字体。
  打包时 `resources/fonts/OFL.txt` 必须与 `.ttf` 一同分发。
- 第三方源码不参与本仓 `-Wall -Wextra` 基线（告警只应来自我们自己的代码），
  构建脚本对它们整体降级告警，见 `CMakeLists.txt` 与 `scripts/build.py` 的注释。

## 本地裁剪与改动

- FreeType 做过裁剪（删模块、改 `ftmodule.h`、关 `FT_CONFIG_OPTION_USE_ZLIB`），
  改动清单、编译清单与线程安全注意事项见 `engine/third_party/freetype/VENDOR-NOTES.md`。
  裁剪**不影响**许可证义务：被删掉的只是未使用的驱动源码，署名要求照旧。
- zlib / libpng / stb 为原样 vendored，未做本地修改。

- `third_party/vulkan-headers/`：Khronos 官方 Vulkan 头（Apache-2.0，自 OHOS NDK sysroot 拷贝）。仅用于 `tools/vulkan_decl_check.cpp` 的声明漂移检查，不参与运行时构建（见其 VENDOR-NOTES.md）。
