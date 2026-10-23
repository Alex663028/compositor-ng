// AI生成
/*
 * comp_guide_abi.cpp — comp_guide.h 的 C ABI 实现。
 *
 * 与其它 ABI 文件同一套约定：先校验句柄与参数，再开隐式事务；
 * changed = 0 的调用不 commit（不产生空撤销步）。
 */
#include "compositor/comp_guide.h"

#include "comp_abi_internal.h"
#include "domain/guide.h"

namespace {

using compositor::Document;
using compositor::DocumentState;
using compositor::Engine;

}  // namespace

comp_result comp_guide_count(comp_document document, size_t* out_count) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_count == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_count is null");
  }
  *out_count = doc->state().guides.size();
  return compositor::abi::ok(engine);
}

comp_result comp_guide_get(comp_document document, size_t index, comp_guide* out_guide) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_guide == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_guide is null");
  }
  const DocumentState& state = doc->state();
  if (index >= state.guides.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "guide index out of range");
  }
  out_guide->axis = state.guides[index].axis;
  out_guide->position = state.guides[index].position;
  return compositor::abi::ok(engine);
}

comp_result comp_guide_add(comp_document document, uint32_t axis, double position,
                           size_t* out_index) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (!compositor::guide_is_valid(axis, position)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "guide axis or position is invalid");
  }
  if (doc->state().guides.size() >= compositor::kMaxGuides) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "guide limit reached");
  }

  compositor::abi::ImplicitTxn txn(doc, "Add Guide");
  size_t index = 0;
  if (!compositor::guide_add(&doc->state(), axis, position, &index)) {
    /* 上面两道闸已经排除了全部失败条件；走到这里说明状态在调用过程中变了。 */
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "guide add failed");
  }
  txn.commit();
  if (out_index != nullptr) *out_index = index;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_guide_move(comp_document document, size_t index, double position,
                            int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (index >= doc->state().guides.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "guide index out of range");
  }
  if (!compositor::guide_is_valid(doc->state().guides[index].axis, position)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "guide position is invalid");
  }

  compositor::abi::ImplicitTxn txn(doc, "Move Guide");
  bool changed = false;
  if (!compositor::guide_move(&doc->state(), index, position, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "guide move failed");
  }
  if (changed) txn.commit();
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_guide_remove(comp_document document, size_t index, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (index >= doc->state().guides.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "guide index out of range");
  }

  compositor::abi::ImplicitTxn txn(doc, "Remove Guide");
  bool changed = false;
  if (!compositor::guide_remove(&doc->state(), index, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "guide remove failed");
  }
  if (changed) txn.commit();
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_guide_clear(comp_document document, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  compositor::abi::ImplicitTxn txn(doc, "Clear Guides");
  bool changed = false;
  if (!compositor::guide_clear(&doc->state(), &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "guide clear failed");
  }
  if (changed) txn.commit();
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}
