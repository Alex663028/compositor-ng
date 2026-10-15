// AI生成
#include "format/comp_uuid.h"

#include <chrono>
#include <random>

namespace compositor {

namespace {

const char kHex[] = "0123456789ABCDEF";

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void stamp_version_bits(uint8_t bytes[16]) {
  /* 版本位 = 4，变体位 = 10xx：与 Foundation UUID 的文本形态一致，上游可无差别解析。 */
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
}

}  // namespace

std::string uuid_to_string(const uint8_t bytes[16]) {
  std::string text;
  text.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    text.push_back(kHex[bytes[i] >> 4]);
    text.push_back(kHex[bytes[i] & 0x0F]);
  }
  return text;
}

bool uuid_from_string(const std::string& text, uint8_t bytes[16]) {
  /* 只接受 Foundation UUID 的规范文本形态：36 字符、连字符固定在 8/13/18/23 位、
   * 其余为十六进制（大小写不敏感）。省略连字符或长度不符一律视为非法——
   * 这样「非规范 id」在载入时会被替换为派生规范 UUID，写出的 manifest 恒规范。 */
  if (text.size() != 36) return false;
  int hex_index = 0;
  for (int i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[static_cast<size_t>(i)] != '-') return false;
      continue;
    }
    const int value = hex_value(text[static_cast<size_t>(i)]);
    if (value < 0) return false;
    uint8_t& byte = bytes[hex_index / 2];
    if (hex_index % 2 == 0) {
      byte = static_cast<uint8_t>(value << 4);
    } else {
      byte = static_cast<uint8_t>(byte | static_cast<uint8_t>(value));
    }
    ++hex_index;
  }
  return hex_index == 32;
}

bool uuid_is_wellformed(const std::string& text) {
  uint8_t bytes[16];
  return uuid_from_string(text, bytes);
}

std::string uuid_v4_new() {
  std::random_device device;
  const uint64_t seed = (static_cast<uint64_t>(device()) << 32) ^
                        static_cast<uint64_t>(device()) ^
                        static_cast<uint64_t>(
                            std::chrono::high_resolution_clock::now().time_since_epoch().count());
  std::mt19937_64 engine(seed);
  uint8_t bytes[16];
  for (int i = 0; i < 16; i += 8) {
    const uint64_t value = engine();
    for (int k = 0; k < 8; ++k) {
      bytes[i + k] = static_cast<uint8_t>((value >> (8 * k)) & 0xFFu);
    }
  }
  stamp_version_bits(bytes);
  return uuid_to_string(bytes);
}

std::string uuid_derive(const std::string& document_uuid, uint64_t key) {
  /* FNV-1a 双通道：一次得到 128 位，足以保证同一 (文档, 图层 id) 恒定。 */
  uint64_t first = 14695981039346656037ULL;
  uint64_t second = 1099511628211ULL;
  for (unsigned char c : document_uuid) {
    first = (first ^ c) * 1099511628211ULL;
    second = (second ^ static_cast<unsigned char>(c + 0x9u)) * 1099511628211ULL;
  }
  for (int i = 0; i < 8; ++i) {
    const uint8_t byte = static_cast<uint8_t>((key >> (8 * i)) & 0xFFu);
    first = (first ^ byte) * 1099511628211ULL;
    second = (second ^ static_cast<unsigned char>(byte + 0x5Du)) * 1099511628211ULL;
  }
  uint8_t bytes[16];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>((first >> (8 * (7 - i))) & 0xFFu);
    bytes[8 + i] = static_cast<uint8_t>((second >> (8 * (7 - i))) & 0xFFu);
  }
  stamp_version_bits(bytes);
  return uuid_to_string(bytes);
}

}  // namespace compositor
