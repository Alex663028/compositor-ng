// AI生成
/*
 * comp_handle_magic.h — ABI 句柄有效性魔数（Engine / Document 共用）。
 *
 * 轻量方案，不引入全局句柄表：对象内置一枚构造期写入、析构期擦除的魔数，
 * abi 层 reinterpret_cast 之后核对它，即可把「销毁后使用 / 类型混淆 /
 * 野指针」挡成错误码。代价是一次整数比较，ABI 类型与二进制布局不变。
 */
#ifndef COMPOSITOR_CORE_COMP_HANDLE_MAGIC_H
#define COMPOSITOR_CORE_COMP_HANDLE_MAGIC_H

#include <cstdint>

namespace compositor {

constexpr uint32_t kEngineMagic = 0x454E4749u;   /* "ENGI" */
constexpr uint32_t kDocumentMagic = 0x444F4355u; /* "DOCU" */
constexpr uint32_t kHandlePoisoned = 0xDEADBEEFu;

}  // namespace compositor

#endif /* COMPOSITOR_CORE_COMP_HANDLE_MAGIC_H */
