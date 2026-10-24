// AI生成
/*
 * comp_abi_internal.h — ABI 实现层内部辅助（不对外暴露）。
 *
 * 设计：ABI 是不透明句柄 + 纯 C 符号；本头文件集中承载句柄转换、参数校验、
 * 错误上报与「隐式事务」的统一封装，避免在每个导出函数里重复样板代码。
 */
#ifndef COMPOSITOR_ABI_SRC_COMP_ABI_INTERNAL_H
#define COMPOSITOR_ABI_SRC_COMP_ABI_INTERNAL_H

#include <exception>
#include <new>
#include <string>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "domain/document.h"
#include "engine.h"

namespace compositor {

class FontFace;

namespace abi {

/* 句柄 <-> 内部类型转换。
 * 三者同一套双闸门：先查「存活对象登记表」（core/comp_live_handles.h，
 * 构造登记 / 析构注销 / 查询只哈希指针值 + 类型标签），命中且类型匹配才允许
 * 解引用读 magic——完全野的句柄（未映射地址）与类型混淆（引擎句柄当文档传）
 * 都被挡在解引用之前，不会段错误，只能变成错误码。类型标签不可省：magic 是
 * 尾部成员，按错误类型的布局读会越界（F2）。 */
Engine* as_engine(comp_engine handle);
Document* as_document(comp_document handle);
FontFace* as_font(comp_font handle);

/* 校验句柄；失败返回 COMP_ERR_INVALID_ARGUMENT。 */
comp_result guard_document(comp_document handle, Document** out_document, Engine** out_engine);

/* 把错误写入引擎并在无引擎时静默返回错误码。 */
comp_result fail(Engine* engine, comp_result code, const std::string& message);

/* 清空引擎最近错误并返回成功。 */
comp_result ok(Engine* engine);

/*
 * ABI 入口异常护栏（P1-1）。
 *
 * 背景：ABI 是纯 C 边界（Swift / ArkTS / C# 调用方无法接住 C++ 异常）。
 * 引擎内部会 resize 出画布级缓冲，std::bad_alloc 一旦越过 extern "C" 会直接
 * 终止宿进程。用法：在「引擎指针已解析」之后放 COMP_ABI_TRY(engine)，
 * 在函数体末尾放 COMP_ABI_CATCH(engine)，异常即被转成错误码 + last_error。
 *
 * 例外：若函数体内会调用 document->txn_begin()，则不能直接用这对宏——异常发生
 * 时事务仍是 active 状态，必须自行 catch 并 txn_abort（见 comp_io_import_layer）。
 */
#define COMP_ABI_TRY(engine_ptr) try {
#define COMP_ABI_CATCH(engine_ptr)                                            \
  }                                                                           \
  catch (const std::bad_alloc&) {                                             \
    return compositor::abi::fail((engine_ptr), COMP_ERR_OUT_OF_MEMORY,        \
                                 "out of memory");                           \
  }                                                                           \
  catch (const std::exception& ex) {                                          \
    return compositor::abi::fail((engine_ptr), COMP_ERR_INTERNAL, ex.what()); \
  }                                                                           \
  catch (...) {                                                               \
    return compositor::abi::fail((engine_ptr), COMP_ERR_INTERNAL,             \
                                 "unexpected exception");                     \
  }

/* 把 ABI 的 comp_image 内容复制进内部 ImageBuffer（按 stride 逐行拷贝）。 */
bool copy_image_into(const comp_image& source, ImageBuffer* destination, std::string* error);

/* 把 ABI 的 comp_mask 内容复制进内部 MaskBuffer。 */
bool copy_mask_into(const comp_mask& source, MaskBuffer* destination, std::string* error);

/*
 * 隐式事务：未处于显式事务的写操作需要自成一个撤销步。
 * 用法：构造后执行修改，成功后调用 commit()；失败路径不 commit。
 */
class ImplicitTxn {
 public:
  ImplicitTxn(Document* document, std::string label)
      : document_(document), owns_(document != nullptr && !document->txn_active()),
        label_(std::move(label)) {
    if (owns_) before_ = document_->state();
  }

  void commit() {
    if (owns_) document_->commit_implicit(label_, before_);
  }

  bool owns() const { return owns_; }

 private:
  Document* document_ = nullptr;
  bool owns_ = false;
  std::string label_;
  DocumentState before_{};
};

}  // namespace abi
}  // namespace compositor

/* 事务句柄的内部定义（C 侧只看到不透明 comp_txn）。 */
struct comp_txn_handle {
  compositor::Document* document = nullptr;
  std::string label;
  bool active = false;
};

#endif /* COMPOSITOR_ABI_SRC_COMP_ABI_INTERNAL_H */
