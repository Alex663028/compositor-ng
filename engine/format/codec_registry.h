// AI生成
/*
 * codec_registry.h — 栅格编解码能力登记表（M5「编解码替换」的接缝）。
 *
 * 目的：把「哪些格式能用、由谁实现、为什么不能用」收敛到唯一真源，使
 * `comp_io_import` / `comp_io_export` 的失败原因可解释、可测试。
 *
 * 状态由**编译期实证**决定：vendor 在 engine/third_party 下的真实库（libpng+zlib /
 * stb_image+stb_image_write）经 `__has_include` 探测存在时记为「已引入」；缺失或
 * 从未实现者一律如实标记不可用并写明原因。绝不返回「假装成功」，也不写占位实现。
 */
#ifndef COMPOSITOR_FORMAT_CODEC_REGISTRY_H
#define COMPOSITOR_FORMAT_CODEC_REGISTRY_H

#include <cstddef>
#include <cstdint>

namespace compositor {

/* 编解码状态（数值与 ABI `comp_codec_state` 一致）。 */
enum CodecState : uint32_t {
  kCodecBuiltIn = 0,     /* 本仓自研零依赖实现 */
  kCodecUnavailable = 1, /* 未实现，且本机未链接第三方实现 */
  kCodecVendored = 2,    /* 由 vendor 在 engine/third_party 的真实库提供 */
};

struct CodecEntry {
  uint32_t format;     /* comp_image_format 数值 */
  const char* name;    /* 规范短名，如 "PNG" */
  uint32_t state;      /* CodecState */
  const char* backend; /* "built-in" / 真实库标识 / 候选第三方库名 */
  const char* note;    /* 边界说明（ASCII，避免编译与终端编码风险） */
  /*
   * can_read / can_write 描述的是**栅格通道**（comp_io_import / comp_io_export：
   * 一次解码成一张图 / 一次合成成一张图）。像 PSD 这种「分层另有一条入口」的格式，
   * 两条通道能力不同构：拍平读写走 stb（只解不封），分层读写走树内 psd_codec
   * （comp_io_import_document / comp_io_export_document）。
   * 因此此处保持「栅格通道」口径，分层能力写在 note 里——宿主只能看到 note
   * （comp_io_codec_support 不导出这两个 bool），note 说全才算说清。
   */
  bool can_read;       /* 栅格通道（comp_io_import）能否读 */
  bool can_write;      /* 栅格通道（comp_io_export，拍平）能否写 */
};

/* 返回登记表首地址与条目数（表内顺序 = comp_image_format 枚举顺序）。 */
const CodecEntry* codec_entries();
size_t codec_entry_count();

/* 未登记的格式（含 COMP_FORMAT_UNKNOWN）返回 nullptr。 */
const CodecEntry* codec_entry_for(uint32_t format);

}  // namespace compositor

#endif /* COMPOSITOR_FORMAT_CODEC_REGISTRY_H */
