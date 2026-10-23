// AI生成
/*
 * utf8_text.h — 字符串落盘前的两个公共判定：严格 UTF-8 与「单行」。
 *
 * 为什么在领域层挡而不是交给宿主：名字（图层名、通道名）会被 `.comp` 原样
 * 写进 JSON 字符串。坏字节要么让落盘结构损坏，要么在宿主侧显示成替换字符后
 * **再也无法还原**；内嵌 NUL/换行则会让面板与 `.comp` 各自截断出两个不同的
 * 名字。两条判定必须全仓只有一份实现，否则「这边收、那边不收」迟早分叉。
 *
 * 这里是头文件内联：判定只有几十行，不引入新的链接单元。
 */
#ifndef COMPOSITOR_DOMAIN_UTF8_TEXT_H
#define COMPOSITOR_DOMAIN_UTF8_TEXT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace compositor {

/*
 * 严格 UTF-8 校验（拒绝超长编码、代理区 U+D800..U+DFFF、> U+10FFFF 与截断）。
 */
inline bool utf8_is_valid(const std::string& text) {
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t extra = 0;
    uint32_t code = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      code = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      code = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      code = c & 0x07u;
    } else {
      return false; /* 0x80..0xBF 的孤立续字节、或 0xF8..0xFF */
    }
    if (i + extra >= n) return false; /* 截断：续字节不够 */
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      code = (code << 6) | (cc & 0x3Fu);
    }
    /* 超长编码与代理区/越界码点：一律拒绝（RFC 3629 的合法性判定）。 */
    if (extra == 1 && code < 0x80) return false;
    if (extra == 2 && code < 0x800) return false;
    if (extra == 3 && code < 0x10000) return false;
    if (code >= 0xD800 && code <= 0xDFFF) return false;
    if (code > 0x10FFFF) return false;
    i += extra + 1;
  }
  return true;
}

/*
 * 可单行显示的文本：没有 C0 控制字符（含内嵌 NUL、换行、Tab）与 DEL。
 * 多字节 UTF-8 的每个字节都 >= 0x80，因此逐字节判定不会误伤 CJK。
 */
inline bool utf8_is_single_line(const std::string& text) {
  for (unsigned char c : text) {
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_UTF8_TEXT_H */
