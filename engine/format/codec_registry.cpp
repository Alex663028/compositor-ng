// AI生成
/*
 * codec_registry.cpp — 编解码能力登记表（唯一真源）。
 *
 * 表中每一条都由**编译期实证**决定，而不是写死的文案：
 *   - vendored 真实库（engine/third_party）经 codec_lib 的 __has_include 探测；
 *   - 缺失或从未实现者如实标记不可用并写明原因（ASCII）。
 * 因此同一份源码在「有 third_party」与「没有 third_party」两种机器上都能如实自述能力。
 */
#include "format/codec_registry.h"

#include <string>

#include "format/codec_lib.h"

namespace compositor {
namespace {

constexpr size_t kEntryCount = 7u;

struct CodecTable {
  std::string png_note;
  std::string jpeg_note;
  std::string psd_note;
  CodecEntry entries[kEntryCount];
  CodecTable();
};

CodecTable::CodecTable() {
  const bool has_png = codec_lib_has_png();
  const bool has_stb = codec_lib_has_stb();

  png_note = has_png
                 ? std::string("vendored real library (") + codec_lib_png_backend() +
                       "); .comp internal layer pixels still use the self-contained comp_png path"
                 : "third_party/libpng missing: falling back to the self-contained comp_png codec";

  jpeg_note = has_stb
                  ? std::string("vendored real library (") + codec_lib_stb_backend() +
                        "); baseline JPEG read/write; lossy, honours options {\"quality\":N}"
                  : "host has no libjpeg-turbo headers/libs and no C/C++ package manager; "
                    "Python-side PIL jpg is a cross-check reference only, not linkable";

  psd_note = has_stb
                 ? std::string("vendored real library (") + codec_lib_stb_backend() +
                       "); READ-ONLY: import supported, export unsupported"
                 : "no PSD reader available on host";

  entries[0] = CodecEntry{1u, "PNG", has_png ? kCodecVendored : kCodecBuiltIn,
                          has_png ? codec_lib_png_backend() : "built-in", png_note.c_str(), true,
                          true};
  entries[1] = CodecEntry{2u, "JPEG", has_stb ? kCodecVendored : kCodecUnavailable,
                          has_stb ? codec_lib_stb_backend() : "libjpeg-turbo", jpeg_note.c_str(),
                          has_stb, has_stb};
  entries[2] = CodecEntry{3u, "TIFF", kCodecUnavailable, "libtiff",
                          "host has no libtiff headers/libs and it is not vendored in this repo",
                          false, false};
  entries[3] = CodecEntry{4u, "HEIF", kCodecUnavailable, "libheif",
                          "host has no libheif; encoder is a large external dependency not present",
                          false, false};
  entries[4] = CodecEntry{5u, "WebP", kCodecUnavailable, "libwebp",
                          "host has no libwebp headers/libs; only a non-linkable bundled copy "
                          "inside the DevEco plugin directory was found",
                          false, false};
  entries[5] = CodecEntry{6u, "EXR", kCodecUnavailable, "OpenEXR",
                          "host has no OpenEXR headers/libs (ImfHeader.h not found)", false, false};
  entries[6] = CodecEntry{7u, "PSD", has_stb ? kCodecVendored : kCodecUnavailable,
                          has_stb ? codec_lib_stb_backend() : "custom/libpsd", psd_note.c_str(),
                          has_stb, false};
}

const CodecTable& codec_table() {
  /* 函数局部 static：C++11 起初始化线程安全，且生命周期覆盖全部调用方。 */
  static const CodecTable table;
  return table;
}

}  // namespace

const CodecEntry* codec_entries() { return codec_table().entries; }

size_t codec_entry_count() { return kEntryCount; }

const CodecEntry* codec_entry_for(uint32_t format) {
  const CodecEntry* table = codec_entries();
  for (size_t i = 0; i < kEntryCount; ++i) {
    if (table[i].format == format) return table + i;
  }
  return nullptr;
}

}  // namespace compositor
