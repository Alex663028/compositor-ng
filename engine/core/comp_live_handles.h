// AI生成
/*
 * comp_live_handles.h — 存活对象登记表（ABI 句柄安全的第二道闸门）。
 *
 * 背景：comp_engine / comp_document / comp_font / comp_txn 句柄都是内部 C++ 对象指针的
 * reinterpret_cast，靠对象内置 magic 校验。但**完全野的**句柄（宿主传了个
 * 0xDEADBEEF 之类的未映射地址）在解引用读 magic 那一刻就段错误了，根本走不到
 * 比较。本表记录**当前存活**的对象指针（构造成功时登记、析构时注销），句柄转换
 * 先查表命中才允许读对象里的 magic。
 *
 * 表项带**类型标签**（LiveHandleKind），这不只是记账方便：
 *   magic 是各对象自己的**尾部成员**，偏移随类型变化（Engine 在偏移 156、
 *   160 字节的对象里；Document 在偏移约 472）。此前表只存"活没活"，于是
 *   as_document(engine_handle) 先查到"活着"，再按 Document 的布局去读 magic，
 *   实际读的是 Engine 分配块之后再 472 字节——Windows 上多半还落在堆里，
 *   断言只是看到垃圾值返回 INVALID_ARGUMENT；鸿蒙上那块地址可能未映射，
 *   直接 SIGSEGV（ASan 报 heap-buffer-overflow，READ 到 160 字节区域右侧
 *   472 字节处）。类型标签让"类型不对"在**任何解引用之前**就被否掉。
 *
 * 表带一把全局互斥锁；一次哈希查找的代价可以忽略。查询只哈希指针值本身、
 * 不解引用，所以野指针查询本身也是安全的。表是「存活 + 类型」判据，magic 是
 * 第二道「未被 poison」判据——两者都过才是一个可用句柄（见 comp_abi_internal.cpp）。
 *
 * 函数内 static 实现，避开全局构造顺序问题；登记 / 注销 / 查询均 noexcept。
 */
#ifndef COMPOSITOR_CORE_COMP_LIVE_HANDLES_H
#define COMPOSITOR_CORE_COMP_LIVE_HANDLES_H

#include <cstdint>

namespace compositor {

/* 句柄种类：与引擎内部可被宿主持有的对象类型一一对应。comp_txn 指向
 * ABI 内部的 comp_txn_handle（comp_abi_internal.h），不是独立 C++ 类，
 * 但走同一张表、同一套双闸门。 */
enum class LiveHandleKind : uint8_t {
  kEngine = 1,
  kDocument = 2,
  kFont = 3,
  kTxn = 4,
};

/* 构造成功后调用（对象已可用）。nullptr 安全空操作。 */
void register_live_handle(const void* object, LiveHandleKind kind) noexcept;
/* 析构开头调用（此后该指针被宿主误传进来也查不到，不会被解引用）。nullptr 安全空操作。 */
void unregister_live_handle(const void* object, LiveHandleKind kind) noexcept;
/* 该指针是否是当前存活的**该类型**对象。只哈希指针值，不解引用。 */
bool is_live_handle(const void* object, LiveHandleKind kind) noexcept;

}  // namespace compositor

#endif /* COMPOSITOR_CORE_COMP_LIVE_HANDLES_H */
