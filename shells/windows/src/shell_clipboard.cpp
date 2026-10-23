// AI生成
/*
 * shell_clipboard.cpp — 图层像素读写与剪贴板
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {


UINT clipboard_layer_format() {
  static UINT format = 0;
  if (format == 0) format = RegisterClipboardFormatW(L"Compositor Layer Pixels");
  return format;
}

/* 100 MP 上限：防止一个坏 DIB 头把壳拖进几 GB 的分配。 */
bool clipboard_size_ok(int64_t width, int64_t height) {
  if (width <= 0 || height <= 0) return false;
  const int64_t count = width * height;
  return count > 0 && count <= 100000000;
}

uint8_t clipboard_straight(uint8_t value, uint8_t alpha) {
  if (alpha == 0) return 0;
  const unsigned scaled =
      (static_cast<unsigned>(value) * 255u + static_cast<unsigned>(alpha) / 2u) /
      static_cast<unsigned>(alpha);
  return static_cast<uint8_t>(scaled > 255u ? 255u : scaled);
}

uint8_t clipboard_premultiply(uint8_t value, uint8_t alpha) {
  return static_cast<uint8_t>((static_cast<unsigned>(value) * alpha + 127u) / 255u);
}

bool clipboard_put_block(UINT format, const void* data, size_t bytes) {
  HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (global == nullptr) return false;
  void* destination = GlobalLock(global);
  if (destination == nullptr) {
    GlobalFree(global);
    return false;
  }
  std::memcpy(destination, data, bytes);
  GlobalUnlock(global);
  if (SetClipboardData(format, global) == nullptr) {
    GlobalFree(global);
    return false;
  }
  return true;
}

bool clipboard_write_image(HWND window, const ClipboardImage& image, std::wstring* error) {
  if (!clipboard_size_ok(image.width, image.height)) {
    if (error != nullptr) *error = L"图像尺寸非法";
    return false;
  }
  const size_t pixel_bytes = static_cast<size_t>(image.width) *
                             static_cast<size_t>(image.height) * 4u;
  if (image.pixels.size() != pixel_bytes) {
    if (error != nullptr) *error = L"像素缓冲尺寸不符";
    return false;
  }

  ClipboardHeader header{};
  header.magic = kClipboardMagic;
  header.width = image.width;
  header.height = image.height;
  header.origin_x = image.origin_x;
  header.origin_y = image.origin_y;
  header.premultiplied = 1;
  std::vector<uint8_t> custom(sizeof(ClipboardHeader) + pixel_bytes);
  std::memcpy(custom.data(), &header, sizeof(header));
  std::memcpy(custom.data() + sizeof(header), image.pixels.data(), pixel_bytes);

  /* DIBV5：32 位 BGRA、自下而上、BI_BITFIELDS + 标准 BGRA 掩码。 */
  const size_t dib_bytes = sizeof(BITMAPV5HEADER) + pixel_bytes;
  std::vector<uint8_t> dib(dib_bytes);
  BITMAPV5HEADER* info = reinterpret_cast<BITMAPV5HEADER*>(dib.data());
  info->bV5Size = sizeof(BITMAPV5HEADER);
  info->bV5Width = image.width;
  info->bV5Height = image.height; /* 正值 = 自下而上 */
  info->bV5Planes = 1;
  info->bV5BitCount = 32;
  info->bV5Compression = BI_BITFIELDS;
  info->bV5RedMask = 0x00FF0000u;
  info->bV5GreenMask = 0x0000FF00u;
  info->bV5BlueMask = 0x000000FFu;
  info->bV5AlphaMask = 0xFF000000u;
  info->bV5CSType = LCS_WINDOWS_COLOR_SPACE;
  info->bV5Intent = LCS_GM_IMAGES;
  uint8_t* dib_pixels = dib.data() + sizeof(BITMAPV5HEADER);
  for (int32_t y = 0; y < image.height; ++y) {
    const uint8_t* source =
        image.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(image.width) * 4u;
    uint8_t* row = dib_pixels + static_cast<size_t>(image.height - 1 - y) *
                                    static_cast<size_t>(image.width) * 4u;
    for (int32_t x = 0; x < image.width; ++x) {
      const uint8_t alpha = source[x * 4 + 3];
      row[x * 4 + 0] = clipboard_straight(source[x * 4 + 2], alpha); /* B */
      row[x * 4 + 1] = clipboard_straight(source[x * 4 + 1], alpha); /* G */
      row[x * 4 + 2] = clipboard_straight(source[x * 4 + 0], alpha); /* R */
      row[x * 4 + 3] = alpha;
    }
  }

  if (!OpenClipboard(window)) {
    if (error != nullptr) *error = L"剪贴板被其它程序占用";
    return false;
  }
  if (!EmptyClipboard()) {
    CloseClipboard();
    if (error != nullptr) *error = L"无法清空剪贴板";
    return false;
  }
  /*
   * 私有格式是应用内往返的唯一凭据：它写成功，粘贴就一定可用。CF_DIBV5 只服务
   * 跨应用，写失败（OOM）不该让用户以为「拷贝失败」——返回 true 并在 error 里
   * 留一句提示，由调用方拼进状态栏。
   */
  const bool custom_ok =
      clipboard_put_block(clipboard_layer_format(), custom.data(), custom.size());
  const bool dib_ok = custom_ok && clipboard_put_block(CF_DIBV5, dib.data(), dib.size());
  CloseClipboard();
  if (!custom_ok) {
    if (error != nullptr) *error = L"写入剪贴板失败";
    return false;
  }
  if (!dib_ok && error != nullptr) *error = L"跨应用格式写入失败（应用内粘贴仍可用）";
  return true;
}

bool clipboard_read_custom(HANDLE handle, ClipboardImage* out) {
  const size_t bytes = GlobalSize(handle);
  if (bytes < sizeof(ClipboardHeader)) return false;
  const void* source = GlobalLock(handle);
  if (source == nullptr) return false;
  ClipboardHeader header{};
  std::memcpy(&header, source, sizeof(header));
  const bool valid = header.magic == kClipboardMagic && header.premultiplied == 1 &&
                     clipboard_size_ok(header.width, header.height);
  const size_t pixel_bytes =
      valid ? static_cast<size_t>(header.width) * static_cast<size_t>(header.height) * 4u : 0;
  if (!valid || bytes < sizeof(header) + pixel_bytes) {
    GlobalUnlock(handle);
    return false;
  }
  out->width = header.width;
  out->height = header.height;
  out->origin_x = header.origin_x;
  out->origin_y = header.origin_y;
  out->pixels.resize(pixel_bytes);
  std::memcpy(out->pixels.data(), static_cast<const uint8_t*>(source) + sizeof(header),
              pixel_bytes);
  GlobalUnlock(handle);
  return true;
}

bool clipboard_read_dib(HANDLE handle, ClipboardImage* out) {
  const size_t bytes = GlobalSize(handle);
  if (bytes < sizeof(BITMAPINFOHEADER)) return false;
  const void* source = GlobalLock(handle);
  if (source == nullptr) return false;
  const uint8_t* base = static_cast<const uint8_t*>(source);

  BITMAPINFOHEADER header{};
  std::memcpy(&header, base, sizeof(header));
  if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > bytes ||
      header.biPlanes != 1 || header.biBitCount != 32) {
    GlobalUnlock(handle);
    return false;
  }
  const bool top_down = header.biHeight < 0;
  const int32_t width = header.biWidth;
  const int32_t height = top_down ? -header.biHeight : header.biHeight;
  if (!clipboard_size_ok(width, height)) {
    GlobalUnlock(handle);
    return false;
  }
  size_t pixel_offset = header.biSize;
  if (header.biCompression == BI_BITFIELDS && header.biSize == sizeof(BITMAPINFOHEADER)) {
    /* 40 字节头 + 12 字节掩码（Win9x 风格）。掩码按标准 BGRA 解释。 */
    if (bytes < pixel_offset + 12u) {
      GlobalUnlock(handle);
      return false;
    }
    const uint32_t* masks = reinterpret_cast<const uint32_t*>(base + pixel_offset);
    if (masks[0] != 0x00FF0000u || masks[1] != 0x0000FF00u || masks[2] != 0x000000FFu) {
      GlobalUnlock(handle);
      return false;
    }
    pixel_offset += 12u;
  } else if (header.biCompression == BI_BITFIELDS) {
    const BITMAPV5HEADER* v5 = reinterpret_cast<const BITMAPV5HEADER*>(base);
    if (header.biSize < sizeof(BITMAPV4HEADER) ||
        v5->bV5RedMask != 0x00FF0000u || v5->bV5GreenMask != 0x0000FF00u ||
        v5->bV5BlueMask != 0x000000FFu) {
      GlobalUnlock(handle);
      return false;
    }
  } else if (header.biCompression != BI_RGB) {
    GlobalUnlock(handle);
    return false;
  }

  const size_t pixel_bytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  if (bytes < pixel_offset + pixel_bytes) {
    GlobalUnlock(handle);
    return false;
  }
  const uint8_t* pixels = base + pixel_offset;

  /* 不少 Windows 程序写 32 位 DIB 时 alpha 全 0（当作 RGB 用）；此时按不透明处理，
   * 否则粘贴出来会是一张「全透明」的图。 */
  bool has_alpha = false;
  for (size_t i = 3; i < pixel_bytes; i += 4) {
    if (pixels[i] != 0) {
      has_alpha = true;
      break;
    }
  }

  out->width = width;
  out->height = height;
  out->origin_x = 0;
  out->origin_y = 0;
  out->pixels.resize(pixel_bytes);
  for (int32_t y = 0; y < height; ++y) {
    const int32_t source_y = top_down ? y : (height - 1 - y);
    const uint8_t* row = pixels + static_cast<size_t>(source_y) * static_cast<size_t>(width) * 4u;
    uint8_t* destination =
        out->pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t alpha = has_alpha ? row[x * 4 + 3] : 255;
      destination[x * 4 + 0] = clipboard_premultiply(row[x * 4 + 2], alpha); /* R */
      destination[x * 4 + 1] = clipboard_premultiply(row[x * 4 + 1], alpha); /* G */
      destination[x * 4 + 2] = clipboard_premultiply(row[x * 4 + 0], alpha); /* B */
      destination[x * 4 + 3] = alpha;
    }
  }
  GlobalUnlock(handle);
  return true;
}

bool clipboard_read_image(HWND window, ClipboardImage* out, std::wstring* error) {
  if (!OpenClipboard(window)) {
    if (error != nullptr) *error = L"剪贴板被其它程序占用";
    return false;
  }
  bool ok = false;
  const UINT custom = clipboard_layer_format();
  if (custom != 0) {
    HANDLE handle = GetClipboardData(custom);
    if (handle != nullptr) ok = clipboard_read_custom(handle, out);
  }
  if (!ok) {
    HANDLE handle = GetClipboardData(CF_DIBV5);
    if (handle == nullptr) handle = GetClipboardData(CF_DIB);
    if (handle != nullptr) ok = clipboard_read_dib(handle, out);
  }
  CloseClipboard();
  if (!ok && error != nullptr) {
    *error = L"剪贴板里没有可用图像（需要本应用的图层数据或 32 位 DIB）";
  }
  return ok;
}

/*
 * 图层是否可按「原始像素」处理：旋转为 0、无翻转、变换尺寸 == 像素尺寸、
 * 格式 RGBA8。只有这种图层能按下标把文档选区映射回图层像素——剪切/清除
 * （要写回原图层）必须满足它，否则宁可不做也不写错位置。
 */
bool layer_is_axis_aligned_8bit(comp_document document, comp_layer_id id, comp_rect* out_rect) {
  comp_rect rect{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(document, id, &rect, &rotation) != COMP_OK) return false;
  if (std::fabs(rotation) > 1e-4f) return false;
  int32_t flip_x = 0;
  int32_t flip_y = 0;
  if (comp_layer_get_flip(document, id, &flip_x, &flip_y) != COMP_OK) return false;
  if (flip_x != 0 || flip_y != 0) return false;
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  if (comp_layer_get_pixels(document, id, nullptr, 0, &width, &height, &format) != COMP_OK) {
    return false;
  }
  if (format != COMP_PIXEL_RGBA8 || width <= 0 || height <= 0) return false;
  if (rect.w != width || rect.h != height) return false;
  if (out_rect != nullptr) *out_rect = rect;
  return true;
}

bool read_layer_pixels(comp_document document, comp_layer_id id, ClipboardImage* out) {
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  if (comp_layer_get_pixels(document, id, nullptr, 0, &width, &height, &format) != COMP_OK) {
    return false;
  }
  if (width <= 0 || height <= 0) return false;
  out->width = width;
  out->height = height;
  out->pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  return comp_layer_get_pixels(document, id, out->pixels.data(),
                               static_cast<size_t>(width) * 4u, &width, &height, &format) ==
         COMP_OK;
}

/* 选区内的拷贝：按 coverage 逐像素乘一遍（硬边选区 255 = 原字节不变）。 */
void crop_clipboard_to_selection(ShellState& shell, ClipboardImage* image) {
  if (!shell.doc.selection_active || shell.doc.selection_mask.empty() || image->pixels.empty()) return;
  comp_rect bounds{};
  if (comp_selection_get_bounds(shell.doc.document, &bounds) != COMP_OK) return;
  const int32_t x0 = std::max(image->origin_x, bounds.x);
  const int32_t y0 = std::max(image->origin_y, bounds.y);
  const int32_t x1 = std::min(image->origin_x + image->width, bounds.x + bounds.w);
  const int32_t y1 = std::min(image->origin_y + image->height, bounds.y + bounds.h);
  if (x1 <= x0 || y1 <= y0) {
    image->width = 0;
    image->height = 0;
    image->pixels.clear();
    return;
  }
  const int32_t width = x1 - x0;
  const int32_t height = y1 - y0;
  std::vector<uint8_t> cropped(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  for (int32_t y = 0; y < height; ++y) {
    const int32_t doc_y = y0 + y;
    for (int32_t x = 0; x < width; ++x) {
      const int32_t doc_x = x0 + x;
      uint32_t coverage = 255;
      if (doc_x >= 0 && doc_x < shell.doc.width && doc_y >= 0 && doc_y < shell.doc.height) {
        coverage = shell.doc.selection_mask[static_cast<size_t>(doc_y) *
                                            static_cast<size_t>(shell.doc.width) +
                                        static_cast<size_t>(doc_x)];
      }
      if (coverage == 0) continue;
      const uint8_t* source =
          image->pixels.data() +
          (static_cast<size_t>(doc_y - image->origin_y) * static_cast<size_t>(image->width) +
           static_cast<size_t>(doc_x - image->origin_x)) *
              4u;
      uint8_t* destination =
          cropped.data() +
          (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
      for (int channel = 0; channel < 4; ++channel) {
        destination[channel] =
            coverage == 255
                ? source[channel]
                : static_cast<uint8_t>((static_cast<unsigned>(source[channel]) * coverage +
                                        127u) /
                                       255u);
      }
    }
  }
  image->origin_x = x0;
  image->origin_y = y0;
  image->width = width;
  image->height = height;
  image->pixels.swap(cropped);
}

/*
 * 取单图层内容：
 *   - 轴对齐 8 位图层 → 原始像素（逐字节往返，落点 = transform 原点）；
 *   - 其它（旋转/翻转/缩放/16 位/16F）→ 画布尺寸的图层合成图（transform、
 *     蒙版、效果、不透明度都烘进去）。它不再是「图层资产」，但**所见即所拷**，
 *     比拷一份落点对不上的原始像素有用。
 */
bool build_layer_clipboard(ShellState& shell, comp_layer_id id, ClipboardImage* out,
                           bool* out_raw, std::wstring* error) {
  *out_raw = false;
  comp_rect rect{};
  if (layer_is_axis_aligned_8bit(shell.doc.document, id, &rect)) {
    if (!read_layer_pixels(shell.doc.document, id, out)) {
      if (error != nullptr) *error = L"读取图层像素失败";
      return false;
    }
    out->origin_x = rect.x;
    out->origin_y = rect.y;
    *out_raw = true;
    return true;
  }
  const int32_t width = shell.doc.width;
  const int32_t height = shell.doc.height;
  if (!clipboard_size_ok(width, height)) {
    if (error != nullptr) *error = L"画布尺寸非法";
    return false;
  }
  out->width = width;
  out->height = height;
  out->origin_x = 0;
  out->origin_y = 0;
  out->pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  comp_size size{};
  const int32_t max_size = std::max(width, height);
  if (comp_render_layer_thumbnail(shell.doc.document, id, max_size, out->pixels.data(),
                                  static_cast<size_t>(width) * 4u, &size) != COMP_OK) {
    if (error != nullptr) *error = L"图层合成失败";
    return false;
  }
  /*
   * max_size = 画布长边，因此输出应当是 1:1 的画布尺寸；引擎若给了别的尺寸，
   * 说明它按 max_size 缩放了——那种像素不能当落点 (0,0) 的画布图用，宁可失败
   * 也不要拷一份位置对不上的东西。
   */
  if (size.width != width || size.height != height) {
    if (error != nullptr) *error = L"图层合成尺寸与画布不一致";
    return false;
  }
  return true;
}

void edit_copy(HWND window, ShellState& shell, bool merged) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"拷贝：还没有文档");
    return;
  }
  ClipboardImage image;
  bool raw = false;
  std::wstring error;
  if (merged) {
    if (!clipboard_size_ok(shell.doc.width, shell.doc.height)) {
      set_status(shell, L"合并拷贝：画布尺寸非法");
      return;
    }
    image.width = shell.doc.width;
    image.height = shell.doc.height;
    image.origin_x = 0;
    image.origin_y = 0;
    image.pixels.assign(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4u,
                        0);
    if (comp_render_flatten(shell.doc.document, image.pixels.data(), COMP_PIXEL_RGBA8,
                            static_cast<size_t>(image.width) * 4u) != COMP_OK) {
      set_status(shell, L"合并拷贝：合成失败");
      return;
    }
  } else {
    const comp_layer_id id = selected_layer_id(shell);
    if (id == 0) {
      set_status(shell, L"拷贝：请先选择一个图层");
      return;
    }
    if (!build_layer_clipboard(shell, id, &image, &raw, &error)) {
      set_status(shell, error.empty() ? L"拷贝失败" : error.c_str());
      return;
    }
  }
  crop_clipboard_to_selection(shell, &image);
  if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
    set_status(shell, L"拷贝：选区内没有像素");
    return;
  }
  if (!clipboard_write_image(window, image, &error)) {
    set_status(shell, error.empty() ? L"写入剪贴板失败" : error.c_str());
    return;
  }
  wchar_t buffer[224];
  const wchar_t* source = merged ? L"合并" : (raw ? L"图层原始像素" : L"图层外观");
  wsprintfW(buffer, L"已拷贝 %s %d×%d @ (%d, %d)", source, image.width, image.height,
            image.origin_x, image.origin_y);
  /* 跨应用格式失败时 error 里有一句提示（应用内粘贴仍可用），一并报出去。 */
  if (!error.empty()) {
    std::wstring combined = buffer;
    combined += L"；";
    combined += error;
    set_status(shell, combined.c_str());
    return;
  }
  set_status(shell, buffer);
}

/*
 * 清除当前图层像素。无选区 = 整层清空；有选区 = 只清 coverage 覆盖到的像素
 * （预乘值乘以 1 - coverage，硬边选区直接归零），一次 set_pixels = 一步撤销。
 */
bool clear_selected_pixels(ShellState& shell, const wchar_t* done_label) {
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"清除：请先选择一个图层");
    return false;
  }
  if (!shell.doc.selection_active) {
    if (comp_layer_set_pixels(shell.doc.document, id, nullptr) != COMP_OK) {
      set_status(shell, L"清除失败（未改动）");
      return false;
    }
    shell.doc.needs_render = true;
    refresh_history_status(shell, done_label);
    return true;
  }
  comp_rect rect{};
  if (!layer_is_axis_aligned_8bit(shell.doc.document, id, &rect)) {
    set_status(shell,
               L"清除：仅支持未旋转/未翻转且尺寸一致的 8 位图层（未改动；可先栅格化）");
    return false;
  }
  ClipboardImage image;
  if (!read_layer_pixels(shell.doc.document, id, &image)) {
    set_status(shell, L"清除：读取图层像素失败（未改动）");
    return false;
  }
  bool changed = false;
  for (int32_t y = 0; y < image.height; ++y) {
    const int32_t doc_y = rect.y + y;
    for (int32_t x = 0; x < image.width; ++x) {
      const int32_t doc_x = rect.x + x;
      if (doc_x < 0 || doc_x >= shell.doc.width || doc_y < 0 || doc_y >= shell.doc.height) continue;
      const uint32_t coverage = shell.doc.selection_mask[static_cast<size_t>(doc_y) *
                                                         static_cast<size_t>(shell.doc.width) +
                                                     static_cast<size_t>(doc_x)];
      if (coverage == 0) continue;
      uint8_t* pixel =
          image.pixels.data() +
          (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) *
              4u;
      const unsigned keep = 255u - coverage;
      for (int channel = 0; channel < 4; ++channel) {
        pixel[channel] = static_cast<uint8_t>(
            (static_cast<unsigned>(pixel[channel]) * keep + 127u) / 255u);
      }
      changed = true;
    }
  }
  if (!changed) {
    set_status(shell, L"清除：选区内没有像素");
    return false;
  }
  comp_image source{};
  source.width = image.width;
  source.height = image.height;
  source.format = COMP_PIXEL_RGBA8;
  source.stride = static_cast<size_t>(image.width) * 4u;
  source.pixels = image.pixels.data();
  if (comp_layer_set_pixels(shell.doc.document, id, &source) != COMP_OK) {
    set_status(shell, L"清除失败（未改动）");
    return false;
  }
  shell.doc.needs_render = true;
  refresh_history_status(shell, done_label);
  return true;
}

void edit_cut(HWND window, ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"剪切：还没有文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"剪切：请先选择一个图层");
    return;
  }
  comp_rect rect{};
  if (!layer_is_axis_aligned_8bit(shell.doc.document, id, &rect)) {
    set_status(shell,
               L"剪切：仅支持未旋转/未翻转且尺寸一致的 8 位图层（未改动；可先栅格化）");
    return;
  }
  ClipboardImage image;
  if (!read_layer_pixels(shell.doc.document, id, &image)) {
    set_status(shell, L"剪切：读取图层像素失败（未改动）");
    return;
  }
  image.origin_x = rect.x;
  image.origin_y = rect.y;
  crop_clipboard_to_selection(shell, &image);
  if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
    set_status(shell, L"剪切：选区内没有像素");
    return;
  }
  std::wstring error;
  if (!clipboard_write_image(window, image, &error)) {
    set_status(shell, error.empty() ? L"写入剪贴板失败（未改动）" : error.c_str());
    return;
  }
  if (!clear_selected_pixels(shell, L"已剪切图层像素")) return;
  wchar_t buffer[224];
  wsprintfW(buffer, L"已剪切 %d×%d @ (%d, %d)", image.width, image.height, image.origin_x,
            image.origin_y);
  if (!error.empty()) {
    std::wstring combined = buffer;
    combined += L"；";
    combined += error;
    set_status(shell, combined.c_str());
    return;
  }
  set_status(shell, buffer);
}

/*
 * 粘贴：读剪贴板 → 新图层 + 像素（+ 贴入时的选区蒙版）全在一个显式事务里，
 * 一次撤销。落点规则：
 *   - 有活动选区：落在选区左上角（「贴入时落点按选区 bounds」，P0-4 验收）；
 *   - 无选区：用私有格式里记下的原文档落点；外部 DIB 没有落点 → (0, 0)。
 */
void edit_paste(HWND window, ShellState& shell, bool into_selection) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"粘贴：还没有文档");
    return;
  }
  ClipboardImage image;
  std::wstring error;
  if (!clipboard_read_image(window, &image, &error)) {
    set_status(shell, error.c_str());
    return;
  }
  comp_rect selection_bounds{};
  const bool has_selection =
      shell.doc.selection_active &&
      comp_selection_get_bounds(shell.doc.document, &selection_bounds) == COMP_OK;
  if (into_selection && !has_selection) {
    set_status(shell, L"贴入：需要活动选区（先用选框工具选一块区域）");
    return;
  }
  int32_t origin_x = image.origin_x;
  int32_t origin_y = image.origin_y;
  if (has_selection) {
    origin_x = selection_bounds.x;
    origin_y = selection_bounds.y;
  }

  comp_layer_desc desc{};
  desc.name = "粘贴的图层";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{origin_x, origin_y, image.width, image.height};
  desc.rotation = 0.0f;
  desc.flip_x = 0;
  desc.flip_y = 0;
  desc.is_group = 0;
  desc.parent = 0;
  desc.sampling = static_cast<comp_sampling>(0);

  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, into_selection ? "Paste Into" : "Paste Layer", &txn) !=
      COMP_OK) {
    set_status(shell, L"粘贴：无法开始事务");
    return;
  }
  comp_layer_id id = 0;
  if (comp_layer_add(shell.doc.document, &desc, &id) != COMP_OK) {
    comp_txn_abort(txn);
    set_status(shell, L"粘贴失败（已回滚）");
    return;
  }
  comp_image source{};
  source.width = image.width;
  source.height = image.height;
  source.format = COMP_PIXEL_RGBA8;
  source.stride = static_cast<size_t>(image.width) * 4u;
  source.pixels = image.pixels.data();
  if (comp_layer_set_pixels(shell.doc.document, id, &source) != COMP_OK) {
    comp_txn_abort(txn);
    set_status(shell, L"粘贴失败（已回滚）");
    return;
  }
  if (into_selection) {
    std::vector<uint8_t> mask(static_cast<size_t>(image.width) *
                                  static_cast<size_t>(image.height),
                              0);
    for (int32_t y = 0; y < image.height; ++y) {
      const int32_t doc_y = origin_y + y;
      for (int32_t x = 0; x < image.width; ++x) {
        const int32_t doc_x = origin_x + x;
        if (doc_x < 0 || doc_x >= shell.doc.width || doc_y < 0 || doc_y >= shell.doc.height) continue;
        mask[static_cast<size_t>(y) * static_cast<size_t>(image.width) +
             static_cast<size_t>(x)] =
            shell.doc.selection_mask[static_cast<size_t>(doc_y) * static_cast<size_t>(shell.doc.width) +
                                 static_cast<size_t>(doc_x)];
      }
    }
    comp_mask layer_mask{};
    layer_mask.width = image.width;
    layer_mask.height = image.height;
    layer_mask.stride = static_cast<size_t>(image.width);
    layer_mask.pixels = mask.data();
    if (comp_layer_set_mask(shell.doc.document, id, &layer_mask) != COMP_OK) {
      comp_txn_abort(txn);
      set_status(shell, L"贴入失败（已回滚）");
      return;
    }
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"粘贴：提交失败");
    return;
  }
  set_visible_cache(shell, id, static_cast<uint8_t>(1));
  refresh_layers(shell);
  panel_select_layer_id(shell, id);
  shell.doc.needs_render = true;
  wchar_t buffer[224];
  wsprintfW(buffer, into_selection ? L"已贴入选区 %d×%d @ (%d, %d)"
                                   : L"已粘贴 %d×%d @ (%d, %d)",
            image.width, image.height, origin_x, origin_y);
  refresh_history_status(shell, buffer);
}

}  // namespace comp_shell
