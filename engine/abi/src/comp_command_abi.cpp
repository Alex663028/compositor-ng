// AI生成
/*
 * comp_command_abi.cpp — comp_command.h 的 C ABI 实现。
 *
 * M1 提供命令枚举骨架与「可序列化命令描述符」的落地示例：
 * 命令表是静态数据（可被三端外壳直接读取以生成菜单与快捷键），
 * 执行入口按 id 分派。完整命令集合随功能迁移逐步补齐（见实施路线 M3/M4）。
 */
#include "compositor/comp_command.h"

#include <cstring>

#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_undo.h"
#include "comp_abi_internal.h"

namespace {

struct CommandEntry {
  const char* id;
  const char* path;
  const char* title;
  uint32_t key_code;
  uint32_t modifiers;
  uint8_t is_toggle;
};

/* 键位采用各平台通用的「字母键 ASCII」，由外壳映射到本地虚拟键码。 */
const CommandEntry kCommands[] = {
    {"edit.undo", "Edit/Undo", "Undo", 'Z', COMP_MOD_COMMAND, 0},
    {"edit.redo", "Edit/Redo", "Redo", 'Z', COMP_MOD_COMMAND | COMP_MOD_SHIFT, 0},
    {"layer.add", "Layer/New Layer", "New Layer", 'N',
     COMP_MOD_COMMAND | COMP_MOD_SHIFT, 0},
    {"layer.remove", "Layer/Delete Layer", "Delete Layer", 0, COMP_MOD_NONE, 0},
    {"layer.merge_down", "Layer/Merge Down", "Merge Down", 'E', COMP_MOD_COMMAND, 0},
    {"layer.flatten", "Layer/Flatten Image", "Flatten Image", 0, COMP_MOD_NONE, 0},
    {"file.save", "File/Save", "Save", 'S', COMP_MOD_COMMAND, 0},
};

constexpr size_t kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

const CommandEntry* find_command(const char* id) {
  if (id == nullptr) return nullptr;
  for (size_t i = 0; i < kCommandCount; ++i) {
    if (std::strcmp(kCommands[i].id, id) == 0) return &kCommands[i];
  }
  return nullptr;
}

}  // namespace

extern "C" {

comp_result comp_command_list(comp_engine engine, comp_command_desc* desc, size_t capacity,
                             size_t* out_count) {
  if (out_count == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  compositor::Engine* e = compositor::abi::as_engine(engine);
  COMP_ABI_TRY(e)
  if (e == nullptr) return COMP_ERR_INVALID_ARGUMENT;

  *out_count = kCommandCount;
  if (desc == nullptr) return compositor::abi::ok(e);

  const size_t count = capacity < kCommandCount ? capacity : kCommandCount;
  for (size_t i = 0; i < count; ++i) {
    desc[i].id = kCommands[i].id;
    desc[i].path = kCommands[i].path;
    desc[i].title = kCommands[i].title;
    desc[i].key_code = kCommands[i].key_code;
    desc[i].modifiers = kCommands[i].modifiers;
    desc[i].is_toggle = kCommands[i].is_toggle;
    desc[i].enabled = 1;
    desc[i]._reserved[0] = 0;
    desc[i]._reserved[1] = 0;
  }
  return compositor::abi::ok(e);
  COMP_ABI_CATCH(e)
}

comp_result comp_command_state(comp_document document, const char* id, int32_t* out_enabled,
                              int32_t* out_is_on) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_enabled == nullptr || out_is_on == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out params are null");
  }

  const CommandEntry* entry = find_command(id);
  if (entry == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "unknown command id");
  }

  int32_t enabled = 1;
  const std::string command_id = entry->id;
  if (command_id == "edit.undo") {
    enabled = doc->history().can_undo() ? 1 : 0;
  } else if (command_id == "edit.redo") {
    enabled = doc->history().can_redo() ? 1 : 0;
  } else if (command_id == "layer.remove") {
    enabled = doc->state().layers.size() > 1 ? 1 : 0;
  } else if (command_id == "layer.merge_down") {
    const compositor::Layer* active = doc->find(doc->state().active_layer);
    enabled = (active != nullptr && doc->can_merge_down(active->id)) ? 1 : 0;
  } else if (command_id == "layer.flatten") {
    enabled = doc->state().layers.empty() ? 0 : 1;
  } else if (command_id == "file.save") {
    enabled = 1;
  }

  *out_enabled = enabled;
  *out_is_on = 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_command_invoke(comp_document document, const char* id,
                               const char* args_json_utf8) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  const CommandEntry* entry = find_command(id);
  if (entry == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "unknown command id");
  }
  (void)args_json_utf8;

  const std::string command_id = entry->id;
  if (command_id == "edit.undo") return comp_document_undo(document);
  if (command_id == "edit.redo") return comp_document_redo(document);

  if (command_id == "layer.add") {
    comp_layer_desc desc{};
    desc.name = "Layer";
    desc.blend = COMP_BLEND_NORMAL;
    desc.opacity = 1.0f;
    desc.transform = comp_rect{0, 0, doc->state().width, doc->state().height};
    comp_layer_id new_id = 0;
    return comp_layer_add(document, &desc, &new_id);
  }

  if (command_id == "layer.remove") {
    if (doc->state().layers.size() <= 1) {
      return compositor::abi::fail(engine, COMP_ERR_STATE, "cannot remove the last layer");
    }
    const compositor::Layer* active = doc->find(doc->state().active_layer);
    const comp_layer_id target =
        active != nullptr ? active->id : doc->state().layers.back().id;
    return comp_layer_remove(document, target);
  }

  if (command_id == "layer.merge_down") {
    const compositor::Layer* active = doc->find(doc->state().active_layer);
    if (active == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_STATE, "no active layer to merge");
    }
    return comp_layer_merge_down(document, active->id);
  }

  if (command_id == "layer.flatten") {
    return comp_document_flatten(document);
  }

  /* file.save 需要宿主提供路径，由外壳直接调用 comp_document_save。 */
  return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                               "command requires host-side handling: " + command_id);
  COMP_ABI_CATCH(engine)
}

} /* extern "C" */
