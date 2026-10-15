// AI生成
/*
 * comp_command.h — 声明式命令模型（三端共享）。
 *
 * 背景：原 macOS 工程的命令调色板基于 NSMenu 反射、条目携带闭包，无法序列化，
 * 也无法在 Windows / HarmonyOS 复用。此处改为“可序列化的命令描述符 + 按 id 调用”，
 * 使三端外壳共享同一套命令定义与快捷键模型。
 *
 * 说明：M1 只提供命令枚举与调用骨架，具体命令集合随功能迁移逐步补齐。
 */
#ifndef COMPOSITOR_COMP_COMMAND_H
#define COMPOSITOR_COMP_COMMAND_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 修饰键位掩码 */
enum {
  COMP_MOD_NONE    = 0,
  COMP_MOD_COMMAND = 1 << 0, /* macOS Cmd / Windows Ctrl */
  COMP_MOD_OPTION  = 1 << 1, /* macOS Option / Windows Alt */
  COMP_MOD_CONTROL = 1 << 2,
  COMP_MOD_SHIFT   = 1 << 3
};

typedef struct comp_command_desc {
  const char* id;        /* 稳定标识，如 "layer.merge_down" */
  const char* path;      /* 菜单路径，如 "Layer/Merge Down" */
  const char* title;     /* 显示名（宿主可本地化覆盖） */
  uint32_t    key_code;  /* 默认键位（0 = 未绑定） */
  uint32_t    modifiers; /* COMP_MOD_* 位掩码 */
  uint8_t     is_toggle; /* 1 = 复选类命令 */
  uint8_t     enabled;   /* 由引擎按当前文档状态求值 */
  uint8_t     _reserved[2];
} comp_command_desc;

/*
 * 两段式枚举：desc 为 NULL 时仅写 out_count；否则最多写入 capacity 条并写 out_count
 * （out_count 始终为该引擎的完整命令数）。
 */
COMP_API comp_result comp_command_list(comp_engine engine,
                                       comp_command_desc* desc,
                                       size_t capacity,
                                       size_t* out_count);

/* 查询指定命令在当前文档下的可用性与选中态。 */
COMP_API comp_result comp_command_state(comp_document document,
                                        const char* id,
                                        int32_t* out_enabled,
                                        int32_t* out_is_on);

/*
 * 执行命令。args_json_utf8 为可选参数（如选区坐标、画笔参数），可为 NULL。
 * 命令内部自动开启事务，撤销粒度 = 一次 invoke。
 */
COMP_API comp_result comp_command_invoke(comp_document document,
                                         const char* id,
                                         const char* args_json_utf8);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_COMMAND_H */
