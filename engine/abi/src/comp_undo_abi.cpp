// AI生成
/*
 * comp_undo_abi.cpp — comp_undo.h 的 C ABI 实现（事务与撤销/重做）。
 */
#include "compositor/comp_undo.h"

#include <cstring>
#include <new>
#include <string>

#include "comp_abi_internal.h"

using compositor::Document;
using compositor::Engine;

extern "C" {

comp_result comp_txn_begin(comp_document document, const char* label_utf8, comp_txn* out_txn) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_txn == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_txn is null");
  }
  if (doc->txn_active()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "a transaction is already active");
  }

  const std::string label = label_utf8 != nullptr ? label_utf8 : "Edit";
  std::string error;
  const comp_result result = doc->txn_begin(label, &error);
  if (result != COMP_OK) return compositor::abi::fail(engine, result, error);

  comp_txn txn = new (std::nothrow) comp_txn_handle();
  if (txn == nullptr) {
    std::string abort_error;
    doc->txn_abort(&abort_error);
    return compositor::abi::fail(engine, COMP_ERR_OUT_OF_MEMORY, "cannot allocate txn handle");
  }
  txn->document = doc;
  txn->label = label;
  txn->active = true;
  /* 登记槽位：文档先于句柄销毁时，~Document 会把 document 置空使其失效。 */
  doc->register_txn_slot(&txn->document);
  *out_txn = txn;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_txn_commit(comp_txn txn) {
  if (txn == nullptr || !txn->active || txn->document == nullptr) {
    /* document 为空 = 宿主已销毁文档（~Document 作废），此时不得解引用。 */
    return COMP_ERR_INVALID_ARGUMENT;
  }
  Document* doc = txn->document;
  Engine* engine = doc->engine();
  std::string error;
  comp_result result = COMP_OK;
  /* P1-1：txn_commit 会 push 一份整篇快照（画布级分配），可能抛 bad_alloc。
     异常必须在此拦下，同时保证 txn 句柄被回收（document 侧 txn_active_ 已置 false）。 */
  try {
    result = doc->txn_commit(&error);
  } catch (const std::bad_alloc&) {
    doc->unregister_txn_slot(&txn->document);
    txn->active = false;
    delete txn;
    return compositor::abi::fail(engine, COMP_ERR_OUT_OF_MEMORY, "out of memory");
  } catch (const std::exception& ex) {
    doc->unregister_txn_slot(&txn->document);
    txn->active = false;
    delete txn;
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, ex.what());
  } catch (...) {
    doc->unregister_txn_slot(&txn->document);
    txn->active = false;
    delete txn;
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "unexpected exception");
  }
  doc->unregister_txn_slot(&txn->document);
  txn->active = false;
  delete txn;
  if (result != COMP_OK) return compositor::abi::fail(engine, result, error);
  return compositor::abi::ok(engine);
}

comp_result comp_txn_abort(comp_txn txn) {
  if (txn == nullptr || !txn->active || txn->document == nullptr) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  Document* doc = txn->document;
  Engine* engine = doc->engine();
  std::string error;
  comp_result result = COMP_OK;
  /* P1-1：回滚要整篇拷回快照，同样可能抛 bad_alloc；一并拦下并回收句柄。 */
  try {
    result = doc->txn_abort(&error);
  } catch (const std::bad_alloc&) {
    doc->unregister_txn_slot(&txn->document);
    txn->active = false;
    delete txn;
    return compositor::abi::fail(engine, COMP_ERR_OUT_OF_MEMORY, "out of memory");
  } catch (const std::exception& ex) {
    doc->unregister_txn_slot(&txn->document);
    txn->active = false;
    delete txn;
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, ex.what());
  } catch (...) {
    doc->unregister_txn_slot(&txn->document);
    txn->active = false;
    delete txn;
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "unexpected exception");
  }
  doc->unregister_txn_slot(&txn->document);
  txn->active = false;
  delete txn;
  if (result != COMP_OK) return compositor::abi::fail(engine, result, error);
  return compositor::abi::ok(engine);
}

comp_result comp_txn_active(comp_document document, int32_t* out_active) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_active == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_active is null");
  }
  *out_active = doc->txn_active() ? 1 : 0;
  return compositor::abi::ok(engine);
}

comp_result comp_document_undo(comp_document document) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  /* 活动事务内必须拒绝：否则 commit 会把「事务前快照 / 已回退的状态」
     压成一条历史，撤销链错位。要放弃整笔事务请用 comp_txn_abort。 */
  if (doc->txn_active()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "cannot undo while a transaction is active");
  }
  if (!doc->history().can_undo()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "nothing to undo");
  }
  if (!doc->undo()) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "undo failed");
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_document_redo(comp_document document) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (doc->txn_active()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "cannot redo while a transaction is active");
  }
  if (!doc->history().can_redo()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "nothing to redo");
  }
  if (!doc->redo()) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "redo failed");
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_document_can_undo(comp_document document, int32_t* out_can) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_can == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_can is null");
  }
  *out_can = doc->history().can_undo() ? 1 : 0;
  return compositor::abi::ok(engine);
}

comp_result comp_document_can_redo(comp_document document, int32_t* out_can) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_can == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_can is null");
  }
  *out_can = doc->history().can_redo() ? 1 : 0;
  return compositor::abi::ok(engine);
}

comp_result comp_document_history_stats(comp_document document, size_t* out_undo_depth,
                                       size_t* out_redo_depth) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_undo_depth != nullptr) *out_undo_depth = doc->history().undo_depth();
  if (out_redo_depth != nullptr) *out_redo_depth = doc->history().redo_depth();
  return compositor::abi::ok(engine);
}

comp_result comp_document_set_history_limit(comp_document document, size_t limit) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  doc->history().set_limit(limit);
  return compositor::abi::ok(engine);
}

comp_result comp_document_set_history_budget(comp_document document, size_t budget_bytes) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  doc->history().set_budget_bytes(budget_bytes);
  return compositor::abi::ok(engine);
}

comp_result comp_document_history_bytes(comp_document document, size_t* out_bytes) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_bytes != nullptr) *out_bytes = doc->history().bytes();
  return compositor::abi::ok(engine);
}

comp_result comp_document_history_truncated(comp_document document, int32_t* out_truncated) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_truncated == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_truncated is null");
  }
  *out_truncated = doc->history().truncated() ? 1 : 0;
  return compositor::abi::ok(engine);
}

/* ------------------------------------------------- 历史面板（P1，尾部追加） */

comp_result comp_document_history_step_count(comp_document document, size_t* out_steps) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_steps != nullptr) *out_steps = doc->history().step_count();
  return compositor::abi::ok(engine);
}

comp_result comp_document_history_step_label(comp_document document, size_t step,
                                             char* buffer, size_t capacity,
                                             size_t* out_required) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_required == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_required is null");
  }
  const std::string* label = doc->history().step_label(step);
  if (label == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "step out of range");
  }
  const size_t required = label->size() + 1; /* 含结尾 NUL */
  *out_required = required;
  /* 查询模式：只问长度（capacity 为 0 也算），不碰 buffer。 */
  if (buffer == nullptr || capacity == 0) return compositor::abi::ok(engine);
  if (capacity < required) {
    /* 不静默截断：与 comp_layer_get_name 同口径，截断可能切坏一个多字节字符。 */
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "label buffer is too small (see out_required)");
  }
  std::memcpy(buffer, label->c_str(), required);
  return compositor::abi::ok(engine);
}

comp_result comp_document_history_jump(comp_document document, size_t step) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  /* 活动事务内拒绝：理由同 undo/redo——事务快照会与回跳后的状态撕开。 */
  if (doc->txn_active()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "cannot jump while a transaction is active");
  }
  const std::size_t total = doc->history().step_count();
  if (step > total) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "step out of range (see history_step_count)");
  }
  while (doc->history().current_index() > step) {
    if (!doc->undo()) {
      return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "undo failed during jump");
    }
  }
  /*
   * 重做会调用 History::trim()：极端预算下可能在途中淘汰最旧记录，下标整体
   * 前移、目标从此不可达。用「原步数 + 1」封顶，宁可报错也不无限循环。
   */
  const std::size_t cap = total + 1;
  for (std::size_t i = 0; i < cap && doc->history().current_index() < step; ++i) {
    if (!doc->redo()) {
      return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "redo failed during jump");
    }
  }
  if (doc->history().current_index() < step) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "history was trimmed during jump; target unreachable");
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

} /* extern "C" */
