// AI生成
/*
 * comp_handle_magic.h — ABI 句柄有效性魔数（Engine / Document / FontFace 共用）。
 *
 * 第二道闸门（第一道是 core/comp_live_handles.h 的存活对象登记表）：对象内置
 * 一枚构造期写入、析构期擦除的魔数，abi 层在查表命中之后再核对它，即可把
 * 「销毁后使用 / 类型混淆」挡成错误码。代价是一次整数比较，ABI 类型与二进制
 * 布局不变。
 */
#ifndef COMPOSITOR_CORE_COMP_HANDLE_MAGIC_H
#define COMPOSITOR_CORE_COMP_HANDLE_MAGIC_H

#include <cstdint>

namespace compositor {

constexpr uint32_t kEngineMagic = 0x454E4749u;   /* "ENGI" */
constexpr uint32_t kDocumentMagic = 0x444F4355u; /* "DOCU" */
constexpr uint32_t kFontMagic = 0x464F4E54u;     /* "FONT"（P5-1 字体句柄） */
constexpr uint32_t kHandlePoisoned = 0xDEADBEEFu;

}  // namespace compositor

#endif /* COMPOSITOR_CORE_COMP_HANDLE_MAGIC_H */
