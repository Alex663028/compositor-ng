// AI生成
/*
 * comp_live_handles.h — 存活对象登记表（ABI 句柄安全的第二道闸门）。
 *
 * 背景：comp_engine / comp_document / comp_font 句柄都是内部 C++ 对象指针的
 * reinterpret_cast，靠对象内置 magic 校验。但**完全野的**句柄（宿主传了个
 * 0xDEADBEEF 之类的未映射地址）在解引用读 magic 那一刻就段错误了，根本走不到
 * 比较。本表记录**当前存活**的对象指针（构造成功时登记、析构时注销），句柄转换
 * 先查表命中才允许读对象里的 magic。
 *
 * 表带一把全局互斥锁；一次哈希查找的代价可以忽略。查询只哈希指针值本身、
 * 不解引用，所以野指针查询本身也是安全的。表是「存活」判据，magic 是「类型」
 * 判据——两者都过才是一个可用句柄（见 comp_abi_internal.cpp 的 as_* 实现）。
 *
 * 函数内 static 实现，避开全局构造顺序问题；登记 / 注销 / 查询均 noexcept。
 */
#ifndef COMPOSITOR_CORE_COMP_LIVE_HANDLES_H
#define COMPOSITOR_CORE_COMP_LIVE_HANDLES_H

namespace compositor {

/* 构造成功后调用（对象已可用）。nullptr 安全空操作。 */
void register_live_handle(const void* object) noexcept;
/* 析构开头调用（此后该指针被宿主误传进来也查不到，不会被解引用）。nullptr 安全空操作。 */
void unregister_live_handle(const void* object) noexcept;
/* 该指针是否是当前存活的对象。只哈希指针值，不解引用。 */
bool is_live_handle(const void* object) noexcept;

}  // namespace compositor

#endif /* COMPOSITOR_CORE_COMP_LIVE_HANDLES_H */
