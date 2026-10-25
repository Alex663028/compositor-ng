// AI生成
/*
 * shell_window.cpp — 窗口过程、键盘/鼠标交互、菜单与视图
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

#include <compositor/comp_guide.h> /* ROI #3：菜单里按参考线条数置灰「清除参考线」 */

namespace comp_shell {


/* ------------------------------------------------ 菜单勾选态（S24） */

/*
 * 递归设置勾选。为什么必须递归：图层菜单里的「可见性/剪贴/隔离/直方图」在
 * 顶层子菜单，「混合模式/采样模式」的档位在二级子菜单——CheckMenuItem 只能对
 * 单个 HMENU 按命令 id 生效，不递归就会出现「顶层勾了、子菜单没勾」。
 *
 * 为什么不重建菜单：重建（DestroyMenu + build_menu + SetMenu）会让已打开的菜单
 * 句柄悬空，并让每次 INITMENU 都闪一下；勾选态是纯显示状态，WM_INITMENU 里刷
 * 一遍即可（GetMenu(window) 拿到的就是同一份 HMENU）。
 */
void menu_set_checked_recursive(HMENU menu, UINT id, bool checked) {
  if (menu == nullptr) return;
  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    const UINT state = GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION);
    if (state == static_cast<UINT>(-1)) continue;
    if ((state & MF_POPUP) != 0) {
      menu_set_checked_recursive(GetSubMenu(menu, i), id, checked);
    } else if (GetMenuItemID(menu, i) == id) {
      CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    }
  }
}

/*
 * 单选设置（混合模式 / 采样模式 / 当前工具）：命令 id 落在 [first, last] 区间里
 * 的整段做互斥勾选。只对**包含区间成员的同一个子菜单**调用 CheckMenuRadioItem，
 * 否则 Win32 会把不相邻的项也当成一组。
 */
void menu_radio_check_recursive(HMENU menu, UINT first, UINT last, UINT selected) {
  if (menu == nullptr) return;
  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    const UINT state = GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION);
    if (state == static_cast<UINT>(-1)) continue;
    if ((state & MF_POPUP) != 0) {
      menu_radio_check_recursive(GetSubMenu(menu, i), first, last, selected);
      continue;
    }
    const UINT command = GetMenuItemID(menu, i);
    if (command >= first && command <= last) {
      CheckMenuRadioItem(menu, first, last, selected, MF_BYCOMMAND);
      return; /* 同一子菜单只做一次；继续递归会重复调用 */
    }
  }
}

/*
 * 置灰/恢复（S41）：EnableMenuItem 与 CheckMenuItem 一样按单个 HMENU 生效，
 * 混合模式/采样/效果都在二级子菜单里，所以同样要递归下去找命令 id。
 */
void menu_set_enabled_recursive(HMENU menu, UINT id, bool enabled) {
  if (menu == nullptr) return;
  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    const UINT state = GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION);
    if (state == static_cast<UINT>(-1)) continue;
    if ((state & MF_POPUP) != 0) {
      menu_set_enabled_recursive(GetSubMenu(menu, i), id, enabled);
    } else if (GetMenuItemID(menu, i) == id) {
      EnableMenuItem(menu, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    }
  }
}

/*
 * WM_INITMENU 的唯一入口：把当前状态映射成勾选态。
 *
 * 判定规则在纯函数 comp_ui::layer_menu_checks 里（无选中层不勾、非组的 isolated
 * 不算、采样/混合夹到合法档位），这里只做「状态 → 菜单」的搬运，所以规则部分
 * 可以表驱动单测，不需要起窗口。
 *
 * S41 起同一入口还负责置灰：前置条件（有无选区/蒙版/可撤销/能否合并/重排端头）
 * 先填进 LayerMenuState，再由 comp_ui::layer_menu_enabled 折成逐项可用态。
 * **只影响菜单**：快捷键仍直接进 run_shell_command，各面板函数保留自己的拒绝提示，
 * 键盘路径的口径不做任何改变。
 */
void sync_menu_state(HWND window, ShellState& shell) {
  HMENU menu = GetMenu(window);
  if (menu == nullptr) return;

  comp_ui::LayerMenuState state{};
  const LayerRow* row = selected_row(shell);
  if (row != nullptr) {
    state.has_layer = true;
    state.visible = row->visible != 0;
    state.clipping = row->clipping != 0;
    state.is_group = row->is_group != 0;
    state.isolated = row->isolated != 0;
    state.sampling = row->sampling;
    state.blend = row->blend;
  }
  state.histogram_open = shell.doc.histogram_open;

  /*
   * S41：置灰需要的前置条件。全部读实况，不在壳里另设缓存——
   *   - 撤销/重做问引擎（can_undo/can_redo）；
   *   - 合并问 comp_layer_can_merge_down（ABI 注释里写明「供外壳置灰菜单」）；
   *   - 剪贴/重排复用各自的纯函数计划，与真正执行时同一套判定，
   *     不会出现「菜单说能做、点了说不行」。
   */
  state.has_selection = shell.doc.selection_active && !shell.doc.selection_mask.empty();
  state.layer_count = static_cast<int32_t>(shell.doc.layers.size());
  state.multi_selected = !shell.doc.extra_selection.empty();
  state.effect_count = static_cast<int32_t>(shell.doc.effect_kinds.size());
  if (shell.doc.document != nullptr) {
    int32_t can = 0;
    if (comp_document_can_undo(shell.doc.document, &can) == COMP_OK && can != 0) state.can_undo = true;
    can = 0;
    if (comp_document_can_redo(shell.doc.document, &can) == COMP_OK && can != 0) state.can_redo = true;
  }
  if (row != nullptr) {
    state.has_mask = row->has_mask != 0;
    if (shell.doc.document != nullptr) {
      int32_t can = 0;
      if (comp_layer_can_merge_down(shell.doc.document, row->id, &can) == COMP_OK && can != 0) {
        state.can_merge_down = true;
      }
    }
    const std::vector<comp_ui::LayerStackEntry> entries = layer_stack_entries(shell);
    const int32_t count = static_cast<int32_t>(entries.size());
    const int32_t index = comp_ui::layer_index_at_row(
        shell.doc.layer_display.data(), panel_row_count(shell), shell.doc.selected_layer);
    /* 未剪贴时要能找到同父基底；已在剪贴态时任何一层都能取消。 */
    if (row->clipping != 0) {
      state.clipping_available = true;
    } else if (shell.doc.layers.size() >= 2) {
      state.clipping_available =
          comp_ui::plan_clipping_base_index(entries.data(), count, index).ok();
    }
    /* 端头直接置灰，而不是点完才在状态栏说「已经是顶层」。 */
    state.can_move_up = comp_ui::plan_layer_reorder(entries.data(), count, index, -1).ok;
    state.can_move_down = comp_ui::plan_layer_reorder(entries.data(), count, index, 1).ok;
    /* P0-6 ①：只有可编辑文字图层才让「栅格化文字」可点。查询口径就是 ABI 本身：
     * 文字层返回 COMP_OK，普通像素层返回 COMP_ERR_STATE。 */
    if (shell.doc.document != nullptr) {
      size_t required = 0;
      state.is_text = comp_layer_get_text(shell.doc.document, row->id, nullptr, nullptr, 0,
                                          &required) == COMP_OK;
    }
  }

  const comp_ui::LayerMenuChecks checks = comp_ui::layer_menu_checks(state);

  menu_set_checked_recursive(menu, kCmdLayerToggleVisible, checks.visible);
  menu_set_checked_recursive(menu, kCmdLayerToggleClipping, checks.clipping);
  menu_set_checked_recursive(menu, kCmdLayerToggleIsolated, checks.isolated);
  menu_set_checked_recursive(menu, kCmdHistogramToggle, checks.histogram);
  /* §5.43：勾选 = 已收起（竖条态），与画出来的形态一致。 */
  menu_set_checked_recursive(menu, kCmdPanelLayers, shell.layer_panel_collapsed);
  menu_set_checked_recursive(menu, kCmdPanelAdjust, shell.adjust_panel_collapsed);
  /*
   * ROI #3 视图开关：勾选 = 当前打开。全部按文档状态刷新（默认：参考线与对齐
   * 打开、标尺与网格关闭）。「清除参考线」没有参考线时置灰——空点击不值得进状态栏。
   */
  menu_set_checked_recursive(menu, kCmdViewRulers, shell.doc.show_rulers);
  menu_set_checked_recursive(menu, kCmdViewGuides, shell.doc.show_guides);
  menu_set_checked_recursive(menu, kCmdViewGrid, shell.doc.show_grid);
  menu_set_checked_recursive(menu, kCmdViewSnap, shell.doc.snap_enabled);
  size_t guide_total = 0;
  const bool has_guides = shell.doc.document != nullptr &&
                          comp_guide_count(shell.doc.document, &guide_total) == COMP_OK &&
                          guide_total > 0;
  menu_set_enabled_recursive(menu, kCmdViewClearGuides, has_guides);
  if (checks.sampling >= 0) {
    menu_radio_check_recursive(menu, kCmdSamplingBase,
                               kCmdSamplingBase + static_cast<UINT>(comp_ui::kSamplingCount) - 1,
                               kCmdSamplingBase + static_cast<UINT>(checks.sampling));
  }
  if (checks.blend >= 0) {
    menu_radio_check_recursive(menu, kCmdBlendBase,
                               kCmdBlendBase + static_cast<UINT>(comp_ui::kBlendCount) - 1,
                               kCmdBlendBase + static_cast<UINT>(checks.blend));
  }
  /*
   * P1-2：锁与颜色标签都是单选组。锁的位组合在 ABI 里合法、菜单只有五档，
   * 用 panel_lock_menu_flag 归一到「最先命中的位」，与 panel_set_lock 的
   * 点选口径完全一致（不会出现「勾的不是点下去会生效的那个」）。
   */
  if (row != nullptr) {
    UINT lock_id = kCmdLayerLockNone;
    switch (panel_lock_menu_flag(row->lock_flags)) {
      case COMP_LAYER_LOCK_TRANSPARENT: lock_id = kCmdLayerLockTransparent; break;
      case COMP_LAYER_LOCK_PIXELS: lock_id = kCmdLayerLockPixels; break;
      case COMP_LAYER_LOCK_POSITION: lock_id = kCmdLayerLockPosition; break;
      case COMP_LAYER_LOCK_ALL: lock_id = kCmdLayerLockAll; break;
      default: break;
    }
    menu_radio_check_recursive(menu, kCmdLayerLockNone, kCmdLayerLockAll, lock_id);
    const uint32_t label =
        row->color_label < COMP_LAYER_COLOR_COUNT ? row->color_label : COMP_LAYER_COLOR_NONE;
    menu_radio_check_recursive(menu, kCmdLayerColorBase,
                               kCmdLayerColorBase + COMP_LAYER_COLOR_COUNT - 1,
                               kCmdLayerColorBase + label);
    /* 链接是勾选项（不是单选组）：主选中层在组里就勾上，点一次解/加。 */
    menu_set_checked_recursive(menu, kCmdLayerLinkToggle, row->link_group != 0);
  }
  /*
   * P1-2 收尾：面板过滤是视图态单选组，勾选态直接读 shell 的三个字段——它不随
   * 选中的图层变化，所以放在 row 判空之外。
   */
  menu_radio_check_recursive(menu, kCmdLayerFilterKindBase,
                             kCmdLayerFilterKindBase + 6,
                             kCmdLayerFilterKindBase +
                                 static_cast<UINT>(shell.layer_filter_kind < 0
                                                       ? 0
                                                       : shell.layer_filter_kind));
  {
    UINT color_id = kCmdLayerFilterColorAll;
    if (shell.layer_filter_color == 0) {
      color_id = kCmdLayerFilterColorNone;
    } else if (shell.layer_filter_color > 0 && shell.layer_filter_color <= 7) {
      color_id = kCmdLayerFilterColorBase +
                 static_cast<UINT>(shell.layer_filter_color - 1);
    }
    menu_radio_check_recursive(menu, kCmdLayerFilterColorAll, kCmdLayerFilterColorBase + 6,
                               color_id);
  }
  /* 效果项：kEffectKindCount 个 kind 各自独立勾选（已启用 = 勾），不是互斥组。 */
  for (int kind = 0; kind < comp_ui::kEffectKindCount; ++kind) {
    const bool present =
        comp_ui::effect_list_contains(shell.doc.effect_kinds.empty() ? nullptr : shell.doc.effect_kinds.data(),
                                      static_cast<int32_t>(shell.doc.effect_kinds.size()), kind);
    menu_set_checked_recursive(menu, kCmdEffectBase + static_cast<UINT>(kind), present);
  }
  /* 工具菜单单选：当前工具（tool_index 是 ui_core 的单一真相，不在这里另算）。 */
  const int tool = comp_ui::tool_index(shell.tool);
  if (tool >= 0) {
    menu_radio_check_recursive(menu, kCmdToolBase,
                               kCmdToolBase + static_cast<UINT>(comp_ui::tool_count()) - 1,
                               kCmdToolBase + static_cast<UINT>(tool));
  }
  /* 镜像两项目前是「动作」而非开关：勾选态展示该方向是否已翻转，便于自诊断。 */
  if (row != nullptr) {
    menu_set_checked_recursive(menu, kCmdLayerFlipHorizontal, row->flip_x != 0);
    menu_set_checked_recursive(menu, kCmdLayerFlipVertical, row->flip_y != 0);
  }

  /*
   * S41：置灰。逐组按命令 id 应用『状态 → 可用』的判定（规则在
   * comp_ui::layer_menu_enabled）。置灰只针对菜单；如果同一命令还有快捷键，
   * 按键仍会进 run_shell_command 并得到各面板函数原有的拒绝提示——
   * 那是有意保留的第二条路径：键盘用户不该因为菜单项变灰就失去提示。
   */
  const comp_ui::LayerMenuEnabled enabled = comp_ui::layer_menu_enabled(state);
  menu_set_enabled_recursive(menu, kCmdUndo, enabled.undo);
  menu_set_enabled_recursive(menu, kCmdRedo, enabled.redo);
  /* 需要选中图层的整组项。新增图层/新建组不属于这一组（无层也能建）。 */
  const UINT layer_present_items[] = {
      kCmdLayerDelete,        kCmdLayerDuplicate,     kCmdLayerRename,
      kCmdLayerToggleVisible, kCmdLayerOpacityDown,   kCmdLayerOpacityUp,
      kCmdLayerOpacitySet,    kCmdLayerBlendPrev,     kCmdLayerBlendNext,
      kCmdLayerFlipHorizontal, kCmdLayerFlipVertical, kCmdLayerMoveLeft,
      kCmdLayerMoveRight,     kCmdLayerMoveUp,        kCmdLayerMoveDown,
      kCmdLayerRotateLeft,    kCmdLayerRotateRight,   kCmdLayerScaleDown,
      kCmdLayerScaleUp,
      kCmdLayerTransformNumeric, kCmdLayerTransformAgain,
  };
  for (UINT id : layer_present_items) {
    menu_set_enabled_recursive(menu, id, enabled.layer_present);
  }
  menu_set_enabled_recursive(menu, kCmdLayerMergeDown, enabled.layer_merge_down);
  menu_set_enabled_recursive(menu, kCmdLayerFlatten, enabled.layer_flatten);
  /* 合并可见 / 反向排列：少于两层时都是空操作（引擎返回 changed=0），一律置灰。 */
  menu_set_enabled_recursive(menu, kCmdLayerMergeVisible, enabled.layer_flatten);
  menu_set_enabled_recursive(menu, kCmdLayerReverse, enabled.layer_flatten);
  /* 链接/解除：有选中层即可点（少于两层的拒绝文案由面板函数给）。 */
  menu_set_enabled_recursive(menu, kCmdLayerLinkToggle, enabled.layer_present);
  /* 样式：拷贝要选中层（组由面板函数如实拒绝）；粘贴还要求剪贴板里有报文。 */
  menu_set_enabled_recursive(menu, kCmdLayerCopyStyle, enabled.layer_present);
  menu_set_enabled_recursive(menu, kCmdLayerPasteStyle,
                             enabled.layer_present && !shell.layer_style_clipboard.empty());
  menu_set_enabled_recursive(menu, kCmdLayerRasterizeText, enabled.layer_rasterize_text);
  menu_set_enabled_recursive(menu, kCmdLayerSelectUp, enabled.layer_step);
  menu_set_enabled_recursive(menu, kCmdLayerSelectDown, enabled.layer_step);
  menu_set_enabled_recursive(menu, kCmdLayerBringForward, enabled.move_up);
  menu_set_enabled_recursive(menu, kCmdLayerBringToFront, enabled.move_up);
  menu_set_enabled_recursive(menu, kCmdLayerSendBackward, enabled.move_down);
  menu_set_enabled_recursive(menu, kCmdLayerSendToBack, enabled.move_down);
  for (int kind = 0; kind < comp_ui::kEffectKindCount; ++kind) {
    menu_set_enabled_recursive(menu, kCmdEffectBase + static_cast<UINT>(kind),
                               enabled.effect_toggle);
  }
  menu_set_enabled_recursive(menu, kCmdEffectEdit, enabled.effect_edit);
  menu_set_enabled_recursive(menu, kCmdEffectClear, enabled.effect_clear);
  for (int mode = 0; mode < comp_ui::kBlendCount; ++mode) {
    menu_set_enabled_recursive(menu, kCmdBlendBase + static_cast<UINT>(mode),
                               enabled.layer_present);
  }
  for (int mode = 0; mode < comp_ui::kSamplingCount; ++mode) {
    menu_set_enabled_recursive(menu, kCmdSamplingBase + static_cast<UINT>(mode),
                               enabled.layer_present);
  }
  /* P1-2：锁与颜色标签同属「有选中图层才可点」，与混合/采样同一把闸。 */
  for (UINT id = kCmdLayerLockNone; id <= kCmdLayerLockAll; ++id) {
    menu_set_enabled_recursive(menu, id, enabled.layer_present);
  }
  for (uint32_t label = 0; label < COMP_LAYER_COLOR_COUNT; ++label) {
    menu_set_enabled_recursive(menu, kCmdLayerColorBase + label, enabled.layer_present);
  }
  /*
   * P1-2 收尾：图层复合。捕获只要有文档就能点（空文档的复合也合法）；其余六条
   * 都要求复合表非空——直接问引擎，不用壳侧缓存，撤销到「还没有复合」时菜单
   * 会立刻跟着置灰。
   */
  size_t layer_comp_count = 0;
  if (shell.doc.document == nullptr ||
      comp_layer_comp_count(shell.doc.document, &layer_comp_count) != COMP_OK) {
    layer_comp_count = 0;
  }
  const bool layer_comp_present = layer_comp_count > 0;
  menu_set_enabled_recursive(menu, kCmdLayerCompCapture, shell.doc.document != nullptr);
  const UINT layer_comp_items[] = {kCmdLayerCompApply,  kCmdLayerCompUpdate,
                                   kCmdLayerCompRename, kCmdLayerCompDelete,
                                   kCmdLayerCompPrev,   kCmdLayerCompNext};
  for (UINT id : layer_comp_items) {
    menu_set_enabled_recursive(menu, id, layer_comp_present);
  }
  /*
   * P1-2 收尾 ⑦：智能对象三条。转换只要求有选中图层（组 / 文字 / HDR / 锁定由引擎
   * 判定并在状态栏说明）；栅格化与清除滤镜要求当前层**确实是**智能对象——直接问
   * 引擎，不用壳侧缓存，撤销掉转换后菜单立刻跟着置灰。
   */
  int32_t smart_selected = 0;
  const comp_layer_id smart_target =
      shell.doc.document != nullptr ? selected_layer_id(shell) : 0;
  if (smart_target != 0) {
    comp_layer_smart_object_is_smart(shell.doc.document, smart_target, &smart_selected);
  }
  size_t smart_filter_total = 0;
  if (smart_selected != 0 &&
      comp_layer_smart_filter_count(shell.doc.document, smart_target, &smart_filter_total) !=
          COMP_OK) {
    smart_filter_total = 0;
  }
  menu_set_enabled_recursive(menu, kCmdLayerSmartConvert,
                             enabled.layer_present && smart_selected == 0);
  menu_set_enabled_recursive(menu, kCmdLayerSmartRasterize, smart_selected != 0);
  menu_set_enabled_recursive(menu, kCmdLayerSmartClear, smart_filter_total > 0);
  /*
   * P1-2 收尾 ⑧：填充图层。三种「新建」只要有文档就能点；编辑 / 栅格化 / 状态要求
   * 当前层**确实是**填充层——同样直接问引擎，撤销掉新建后菜单立刻跟着置灰。
   */
  int32_t fill_selected = 0;
  if (smart_target != 0) {
    comp_layer_is_fill(shell.doc.document, smart_target, &fill_selected);
  }
  menu_set_enabled_recursive(menu, kCmdLayerFillSolid, shell.doc.document != nullptr);
  menu_set_enabled_recursive(menu, kCmdLayerFillGradient, shell.doc.document != nullptr);
  menu_set_enabled_recursive(menu, kCmdLayerFillPattern, shell.doc.document != nullptr);
  menu_set_enabled_recursive(menu, kCmdLayerFillEdit, fill_selected != 0);
  menu_set_enabled_recursive(menu, kCmdLayerFillRasterize, fill_selected != 0);
  menu_set_enabled_recursive(menu, kCmdLayerFillStatus, fill_selected != 0);
  menu_set_enabled_recursive(menu, kCmdLayerMaskFromSelection, enabled.mask_from_selection);
  menu_set_enabled_recursive(menu, kCmdLayerMaskToSelection, enabled.mask_to_selection);
  menu_set_enabled_recursive(menu, kCmdLayerMaskRemove, enabled.mask_remove);
  menu_set_enabled_recursive(menu, kCmdLayerToggleClipping, enabled.clipping);
  menu_set_enabled_recursive(menu, kCmdLayerToggleIsolated, enabled.isolated);
  menu_set_enabled_recursive(menu, kCmdSelectNone, enabled.selection_modify);
  menu_set_enabled_recursive(menu, kCmdSelectInvert, enabled.selection_modify);
  menu_set_enabled_recursive(menu, kCmdSelectExpand, enabled.selection_modify);
  menu_set_enabled_recursive(menu, kCmdSelectContract, enabled.selection_modify);
  menu_set_enabled_recursive(menu, kCmdSelectBorder, enabled.selection_modify);
  menu_set_enabled_recursive(menu, kCmdSelectSmooth, enabled.selection_modify);
  menu_set_enabled_recursive(menu, kCmdSelectGrow, enabled.selection_modify);
  menu_set_enabled_recursive(menu, kCmdSelectSimilar, enabled.selection_modify);
  /* 变换选区需要一张可变换的基础蒙版；selection_modify 是同一把闸。 */
  menu_set_enabled_recursive(menu, kCmdSelectTransform, enabled.selection_modify);
  menu_set_enabled_recursive(menu, kCmdSelectLoadPixels, enabled.selection_load_pixels);
  /* ROI #5：存通道需要当前有选区；通道面板本身随时可开（空列表也给提示）。 */
  menu_set_enabled_recursive(menu, kCmdSelectSaveChannel, enabled.selection_save_channel);
}

/* 面板值 -> ABI 结构（唯一映射点，与 ui_core 的行序一一对应）。 */
void build_trim_options(const ShellState& shell, comp_trim_options* out) {
  comp_trim_options options{};
  options.based_on = static_cast<comp_trim_based_on>(shell.doc.trim.based_on);
  options.top = shell.doc.trim.edge[0];
  options.bottom = shell.doc.trim.edge[1];
  options.left = shell.doc.trim.edge[2];
  options.right = shell.doc.trim.edge[3];
  options.tolerance = static_cast<uint8_t>(shell.doc.trim.tolerance);
  *out = options;
}

/* ------------------------------------------------------------ 工具 / 视图 / 鼠标 */

void set_tool(ShellState& shell, comp_ui::ToolId id) {
  /* 离开钢笔工具 = 放弃未提交的路径（与切走套索/marquee 的取消口径一致）。 */
  if (shell.tool == comp_ui::ToolId::Pen && id != comp_ui::ToolId::Pen) {
    pen_cancel(shell);
  }
  /* 离开渐变工具 = 放弃进行中的拖拽（只丢预览，不产生撤销步）。 */
  if (shell.tool == comp_ui::ToolId::Gradient && id != comp_ui::ToolId::Gradient) {
    gradient_cancel(shell);
  }
  /* 离开形状工具同理：只丢预览。 */
  if (shell.tool == comp_ui::ToolId::Shape && id != comp_ui::ToolId::Shape) {
    shape_cancel(shell);
  }
  /*
   * 离开裁剪工具 = 丢掉待提交的框。必须丢：框是文档坐标系里的绝对矩形，
   * 切走工具后它还在屏幕上会让人以为按 Enter 还能提交，但那时 Enter 已经
   * 路由到别的逻辑去了——留着一个不能用的模态比没有模态更难理解。
   */
  if (shell.tool == comp_ui::ToolId::Crop && id != comp_ui::ToolId::Crop) {
    crop_cancel(shell);
  }
  /* 离开文字工具 = 放弃未提交的键入与属性键入（面板本身不是模态，不产生撤销步）。 */
  if (shell.tool == comp_ui::ToolId::Type && id != comp_ui::ToolId::Type) {
    type_cancel(shell);
    shell.doc.type_input.clear();
  }
  /* 切到文字工具时丢掉调整面板的数值缓冲：两个面板共用 Tab/数字键，不能同时有输入态。 */
  if (id == comp_ui::ToolId::Type) shell.doc.adj_input.clear();
  shell.tool = id;
  /*
   * 切到历史记录画笔：顺手把历史面板打开——否则「取样哪一格」既看不见也点不到，
   * 自动取样虽然能画，但用户不知道自己在刷什么。没有文档时静默（面板打开也无意义）。
   */
  if (id == comp_ui::ToolId::HistoryBrush && !shell.doc.history_open && shell.doc.document != nullptr) {
    history_panel_toggle(shell);
  }
  const comp_ui::ToolInfo* info = comp_ui::tool_info(id);
  if (info == nullptr) return;
  wchar_t buffer[192];
  if (id == comp_ui::ToolId::Retouch) {
    wsprintfW(buffer, L"工具：Retouch / %s（再按 R 循环）",
              widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str());
  } else if (id == comp_ui::ToolId::Tone) {
    wsprintfW(buffer, L"工具：%s（再按 O 循环；Alt+[ / ] 调强度，当前 %d%%）",
              widen_ascii(comp_ui::tone_state_name(shell.tone_state)).c_str(),
              static_cast<int>(shell.tone_strength * 100.0 + 0.5));
  } else if (id == comp_ui::ToolId::Marquee) {
    wsprintfW(buffer, L"工具：Marquee / %s（M 切矩形/椭圆，Shift 加选 / Alt 减选）",
              widen_ascii(comp_ui::marquee_shape_name(shell.marquee_shape)).c_str());
  } else if (id == comp_ui::ToolId::Lasso) {
    wsprintfW(buffer, L"工具：Lasso（拖出自由选区；Shift 加选 / Alt 减选；羽化 %d px）",
              shell.selection_feather);
  } else if (id == comp_ui::ToolId::Pen) {
    wsprintfW(buffer,
              L"工具：Pen（点击落锚点，拖动拉控制柄；点首锚点闭合，Enter 完成，Esc 取消；羽化 %d px）",
              shell.selection_feather);
  } else if (id == comp_ui::ToolId::Gradient) {
    wsprintfW(buffer,
              L"工具：Gradient / %s / 色标 %s（拖出方向与长度后松开；G 切五种类型；"
              L"Shift 反向；色标表与抖动在选项栏切换）",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str(),
              widen_ascii(comp_ui::gradient_preset_name(shell.gradient_preset)).c_str());
  } else if (id == comp_ui::ToolId::Shape) {
    wsprintfW(buffer,
              L"工具：Shape / %s（拖出%s；Shift = 正方/正圆，Alt = 从中心；再按 U 切种类；"
              L"Alt+[ / ] 调线宽，Shift+Alt+[ / ] 调圆角）",
              widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
              shell.shape_kind == comp_ui::kShapeLine ? L"两端点" : L"对角点");
  } else if (id == comp_ui::ToolId::Crop) {
    wsprintfW(buffer,
              L"工具：Crop（拖出裁剪框，框外变暗；Enter 提交，Esc 取消；再点画布重新起框）");
  } else if (id == comp_ui::ToolId::Type) {
    wsprintfW(buffer,
              L"工具：Type（画布点击落插入点，键入 ASCII；Shift+Enter 换行，回车提交，Esc 取消；"
              L"字号 %d px，行距 %d%%，字距 %d px；右侧面板 Tab/-=/数字键；"
              L"[ / ] 字号，Shift+[ / ] 字距，Alt+[ / ] 行距）",
              static_cast<int>(shell.doc.type_font_size + 0.5),
              static_cast<int>(shell.doc.type_line_height * 100.0 + 0.5),
              static_cast<int>(std::lround(shell.doc.type_letter_spacing)));
  } else if (id == comp_ui::ToolId::Wand) {
    wsprintfW(buffer,
              L"工具：Magic Wand（容差 %d，窗口 %d×%d，%s取样，%s；[ / ] 调容差，Alt+[ / ] 调窗口，Shift+W 切来源）",
              shell.wand_tolerance, shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1,
              shell.wand_merged ? L"合并" : L"当前图层",
              shell.wand_contiguous ? L"连通" : L"全图");
  } else if (id == comp_ui::ToolId::HistoryBrush) {
    wsprintfW(buffer,
              L"工具：History Brush（Y）· 取样第 %d 个状态｜Shift+点历史面板的行改取样源，"
              L"Alt+[ / ] 调不透明度（当前 %d%%）",
              history_brush_resolved_state(shell),
              static_cast<int>(shell.history_brush_opacity * 100.0 + 0.5));
  } else if (id == comp_ui::ToolId::Clone) {
    if (shell.doc.clone_source_set) {
      wsprintfW(buffer,
                L"工具：Clone Stamp（S）· 源 (%d, %d) · %s取样｜Alt+点击换源点，"
                L"Shift+Alt = 整篇合成（保持），Shift+K 切取样范围",
                static_cast<int>(shell.doc.clone_source_x),
                static_cast<int>(shell.doc.clone_source_y),
                shell.clone_sample_all ? L"整篇合成" : L"当前图层");
    } else {
      wsprintfW(buffer,
                L"工具：Clone Stamp（S）· 先 Alt+点击画布设置源点（Shift+Alt = 整篇合成）；"
                L"Shift+K 切取样范围（%s），Shift+S 切对齐（%s），"
                L"Alt+[ / ] 调不透明度（%d%%）",
                shell.clone_sample_all ? L"整篇合成" : L"当前图层",
                shell.clone_aligned ? L"开" : L"关",
                static_cast<int>(comp_ui::clamp_clone_opacity(shell.clone_opacity) * 100.0 + 0.5));
    }
  } else if (id == comp_ui::ToolId::Heal) {
    wsprintfW(buffer,
              L"工具：Spot Healing（J）· %s｜拖动涂抹要修补的区域，松开应用；"
              L"再按 J 切算法，[ / ] 调直径、Shift+[ / ] 调硬度、Alt+[ / ] 调不透明度（%d%%）",
              widen_ascii(comp_ui::heal_mode_name(shell.heal_mode)).c_str(),
              static_cast<int>(comp_ui::clamp_heal_opacity(shell.heal_opacity) * 100.0 + 0.5));
  } else if (id == comp_ui::ToolId::Move) {
    wsprintfW(buffer,
              L"工具：Move（V）· 无选区 = 拖整层、Alt+拖动 = 复制一份再移动；"
              L"有选区 = 选区内搬像素（Alt = 复制）、选区外拖整层；"
              L"面板多选 = 整批移动（Alt = 整批复制）；方向键微调 1 px（Shift 1 / Ctrl 10）；"
              L"单选图层时拖四角/边中点 = 缩放，拖上方旋钮 = 旋转（Shift = 15° 吸附）");
  } else if (id == comp_ui::ToolId::Zoom) {
    wsprintfW(buffer,
              L"工具：Zoom（点击放大 1.2 倍 / 拖框放大到该区域；Alt+点击缩小；"
              L"双击画布 = 100%；Ctrl+0 适合窗口 / Ctrl+1 实际像素）");
  } else {
    wsprintfW(buffer, L"工具：%s（%c）", widen_ascii(info->name).c_str(),
              info->shortcut != 0 ? static_cast<wchar_t>(info->shortcut - 'a' + 'A') : L' ');
  }
  set_status(shell, buffer);
}

/* R 键循环 Blur / Smudge / Liquify；三者都走 comp_tool_apply。 */
void set_retouch_mode(ShellState& shell, int mode) {
  shell.retouch_mode = mode;
  shell.tool = comp_ui::ToolId::Retouch;
  wchar_t buffer[192];
  if (mode == comp_ui::kRetouchBlur) {
    wsprintfW(buffer, L"工具：Retouch / Blur（Alt+[ / ] 调半径，当前 %d px）",
              static_cast<int>(shell.blur_radius + 0.5));
  } else {
    wsprintfW(buffer, L"工具：Retouch / %s（Alt+[ / ] 调强度，当前 %d%%）",
              widen_ascii(comp_ui::retouch_mode_name(mode)).c_str(),
              static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
  }
  set_status(shell, buffer);
}

/* O 键循环 8 档（Dodge/Burn 各三档 + Sponge 两档）；都走 comp_tool_apply。 */
void set_tone_state(ShellState& shell, int state) {
  shell.tone_state = state;
  shell.tool = comp_ui::ToolId::Tone;
  wchar_t buffer[192];
  wsprintfW(buffer, L"工具：%s（再按 O 循环；Alt+[ / ] 调强度，当前 %d%%）",
            widen_ascii(comp_ui::tone_state_name(state)).c_str(),
            static_cast<int>(shell.tone_strength * 100.0 + 0.5));
  set_status(shell, buffer);
}

void brush_step(ShellState& shell, int direction, bool hardness) {
  wchar_t buffer[96];
  shell.brush_preset = 0; /* P1-4：快捷键手改参数同样脱离预设 */
  if (hardness) {
    shell.brush_hardness = comp_ui::step_brush_hardness(shell.brush_hardness, direction);
    wsprintfW(buffer, L"笔刷硬度 %d%%",
              static_cast<int>(shell.brush_hardness * 100.0 + 0.5));
  } else {
    shell.brush_size = comp_ui::step_brush_size(shell.brush_size, direction);
    wsprintfW(buffer, L"笔刷直径 %d px", static_cast<int>(shell.brush_size + 0.5));
  }
  set_status(shell, buffer);
}

bool client_layout(const ShellState& shell, Layout* out) {
  RECT client{};
  if (shell.window == nullptr || !GetClientRect(shell.window, &client)) return false;
  comp_ui::LayoutPrefs prefs;
  prefs.layer_panel_width = shell.layer_panel_width;
  prefs.adjust_panel_width = shell.adjust_panel_width;
  prefs.layer_panel_collapsed = shell.layer_panel_collapsed;
  prefs.adjust_panel_collapsed = shell.adjust_panel_collapsed;
  /*
   * ROI #7：GetClientRect 给的是**设备像素**，排版常量是逻辑像素——这里换算是
   * 唯一的入口（present 与此函数共用，命中测试与绘制因此永远同一套矩形）。
   */
  const int dpi = shell_dpi(shell);
  *out = comp_ui::compute_layout(dpi_logical(client.right - client.left, dpi),
                                 dpi_logical(client.bottom - client.top, dpi), prefs);
  return true;
}

/*
 * 面板分界线拖动（§6.60）。宽度偏好在 ShellState 里，**不进撤销栈**：布局不是
 * 文档状态，撤销一次不该把面板宽度回退（PS 亦然，`Ctrl+Z` 只动像素/图层）。
 *
 * 拖动只算「按下点 -> 当前点」的总位移，不在上一帧结果上累加——与整层拖动同一
 * 写法：来回拖回原点必然回到原宽度，不会因取整漂移。
 *
 * 画布中心会随面板宽度移动，这里把差额补偿到 pan 上，画面里的文档因此**待在原地**
 * 不动：用户拖的是面板，不该顺带把正在看的图像推走。
 */
void panel_resize_begin(HWND window, ShellState& shell, int target, int mouse_x) {
  if (target != 1 && target != 2) return;
  shell.panel_resize_target = target;
  shell.panel_resize_start_x = mouse_x;
  shell.panel_resize_base_width =
      target == 1 ? shell.layer_panel_width : shell.adjust_panel_width;
  SetCapture(window);
  wchar_t buffer[160];
  wsprintfW(buffer, L"%s：拖动调宽，双击分界线复位（%d px），Esc 取消",
            target == 1 ? L"图层面板" : L"右侧面板",
            target == 1 ? comp_ui::Layout::kLayerPanelWidth : comp_ui::Layout::kAdjustPanelWidth);
  set_status(shell, buffer);
}

void panel_resize_extend(ShellState& shell, int mouse_x) {
  if (shell.panel_resize_target == 0) return;
  Layout before{};
  if (!client_layout(shell, &before)) return;

  const int dx = mouse_x - shell.panel_resize_start_x;
  const int next =
      shell.panel_resize_target == 1
          ? comp_ui::layer_panel_width_from_drag(shell.panel_resize_base_width, dx)
          : comp_ui::adjust_panel_width_from_drag(shell.panel_resize_base_width, dx);
  const int current = shell.panel_resize_target == 1 ? shell.layer_panel_width
                                                     : shell.adjust_panel_width;
  if (next == current) return;
  if (shell.panel_resize_target == 1) {
    shell.layer_panel_width = next;
  } else {
    shell.adjust_panel_width = next;
  }

  Layout after{};
  bool shown = true;
  if (client_layout(shell, &after)) {
    const comp_ui::Point was = comp_ui::CanvasView::canvas_center(before.canvas);
    const comp_ui::Point now = comp_ui::CanvasView::canvas_center(after.canvas);
    shell.doc.view.pan_by(was.x - now.x, was.y - now.y);
    /* 面板被窗口宽度收走时如实说明：拖动改了偏好，但这一尺寸下放不下。 */
    shown = shell.panel_resize_target == 1 ? after.layer_panel.w > 0
                                          : after.adjust_panel.w > 0;
  }

  wchar_t buffer[176];
  wsprintfW(buffer, L"%s宽度 %d px%s", shell.panel_resize_target == 1 ? L"图层面板" : L"右侧面板",
            next, shown ? L"" : L"（当前窗口放不下，已收起；放宽窗口后生效）");
  set_status(shell, buffer);
}

void panel_resize_end(ShellState& shell) {
  if (shell.panel_resize_target == 0) return;
  const int target = shell.panel_resize_target;
  const int width =
      target == 1 ? shell.layer_panel_width : shell.adjust_panel_width;
  /* 与滑杆同一顺序：先清状态再 ReleaseCapture，捕获变更会重入 cancel。 */
  shell.panel_resize_target = 0;
  shell.panel_resize_base_width = 0;
  ReleaseCapture();

  wchar_t buffer[176];
  wsprintfW(buffer, L"%s宽度：%d px（双击分界线复位）",
            target == 1 ? L"图层面板" : L"右侧面板", width);
  set_status(shell, buffer);
}

void panel_resize_cancel(ShellState& shell) {
  if (shell.panel_resize_target == 0) return;
  Layout before{};
  client_layout(shell, &before);
  const int target = shell.panel_resize_target;
  const int restore = shell.panel_resize_base_width;
  if (target == 1) {
    shell.layer_panel_width = restore;
  } else {
    shell.adjust_panel_width = restore;
  }
  shell.panel_resize_target = 0;
  shell.panel_resize_base_width = 0;

  Layout after{};
  if (client_layout(shell, &after)) {
    const comp_ui::Point was = comp_ui::CanvasView::canvas_center(before.canvas);
    const comp_ui::Point now = comp_ui::CanvasView::canvas_center(after.canvas);
    shell.doc.view.pan_by(was.x - now.x, was.y - now.y);
  }
}

/* 双击分界线 = 复位缺省宽度（PS 双击分隔条复位同一手感，方便拖坏了收场）。 */
void panel_resize_reset(ShellState& shell, int target) {
  Layout before{};
  if (!client_layout(shell, &before)) return;
  if (target == 1) {
    shell.layer_panel_width = comp_ui::Layout::kLayerPanelWidth;
  } else if (target == 2) {
    shell.adjust_panel_width = comp_ui::Layout::kAdjustPanelWidth;
  } else {
    return;
  }
  Layout after{};
  if (client_layout(shell, &after)) {
    const comp_ui::Point was = comp_ui::CanvasView::canvas_center(before.canvas);
    const comp_ui::Point now = comp_ui::CanvasView::canvas_center(after.canvas);
    shell.doc.view.pan_by(was.x - now.x, was.y - now.y);
  }
  set_status(shell, target == 1 ? L"图层面板宽度已复位为 248 px"
                                : L"右侧面板宽度已复位为 330 px");
}

/*
 * §5.43 面板整块收起（停靠最小版）。target 1 = 图层面板、2 = 调整面板。
 *
 * 与 §6.60 拖宽同一套口径：只改会话偏好（不进撤销栈），并且把画布中心的位移
 * 补偿进 pan——收起/展开会让画布宽 224px、中心横移 112px，不补偿的话用户按 F7
 * 收面板，正在看的图像会跟着跳一下，像「视图被重置了」。
 *
 * 宽度偏好**不**跟着收起变：展开回来仍是用户拖出来的宽度（宽度是会话状态，
 * 收起只是它的一个显示开关）。
 */
void panel_toggle_collapsed(ShellState& shell, int target) {
  if (target != 1 && target != 2) return;
  Layout before{};
  if (!client_layout(shell, &before)) return;

  if (target == 1) {
    shell.layer_panel_collapsed = !shell.layer_panel_collapsed;
  } else {
    shell.adjust_panel_collapsed = !shell.adjust_panel_collapsed;
  }

  Layout after{};
  bool shown = true;
  if (client_layout(shell, &after)) {
    const comp_ui::Point was = comp_ui::CanvasView::canvas_center(before.canvas);
    const comp_ui::Point now = comp_ui::CanvasView::canvas_center(after.canvas);
    shell.doc.view.pan_by(was.x - now.x, was.y - now.y);
    shown = target == 1 ? after.layer_panel.w > 0 : after.adjust_panel.w > 0;
  }

  const bool collapsed =
      target == 1 ? shell.layer_panel_collapsed : shell.adjust_panel_collapsed;
  const wchar_t* hint = L"";
  if (!shown) {
    hint = L"（当前窗口放不下，放宽窗口后生效）";
  } else if (collapsed) {
    hint = target == 1 ? L"（F7 或点竖条展开）" : L"（F8 或点竖条展开）";
  }
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s：%s%s", target == 1 ? L"图层面板" : L"调整面板",
            collapsed ? L"已收起" : L"已展开", hint);
  set_status(shell, buffer);
}

void view_fit(ShellState& shell) {
  Layout layout{};
  if (!client_layout(shell, &layout)) return;
  shell.doc.view.fit(shell.doc.width, shell.doc.height, layout.canvas);
  shell.doc.needs_fit = false;
  wchar_t buffer[96];
  wsprintfW(buffer, L"适合窗口：%d%%", static_cast<int>(shell.doc.view.zoom * 100.0 + 0.5));
  set_status(shell, buffer);
}

void view_actual(ShellState& shell) {
  shell.doc.view.actual_pixels();
  shell.doc.needs_fit = false;
  set_status(shell, L"实际像素：100%");
}

void view_zoom_step(ShellState& shell, double factor) {
  Layout layout{};
  if (!client_layout(shell, &layout) || layout.canvas.empty()) return;
  const comp_ui::Point center = comp_ui::CanvasView::canvas_center(layout.canvas);
  shell.doc.view.zoom_by(factor, center.x, center.y, layout.canvas);
  wchar_t buffer[96];
  wsprintfW(buffer, L"缩放 %d%%", static_cast<int>(shell.doc.view.zoom * 100.0 + 0.5));
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 缩放工具（Z） */

/*
 * 缩放工具是纯视图操作：只改 view.zoom/pan，不碰文档、不进撤销栈。
 *
 * 锚点用**点击点**而不是画布中心（PS 语义）：连点时目标像素停在光标下，
 * 不会越点越偏。尺度夹取仍在 ui_core（2%..3200%），到顶再点画面不动，
 * 所以状态栏要明确写「已到上限/下限」，否则看起来像没响应。
 */
bool zoom_tool_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Zoom;
}

void zoom_tool_click(ShellState& shell, int screen_x, int screen_y, const Layout& layout) {
  if (layout.canvas.empty()) return;
  const bool zoom_out = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const double before = shell.doc.view.zoom;
  shell.doc.view.zoom_by(zoom_out ? 1.0 / 1.2 : 1.2, static_cast<double>(screen_x),
                     static_cast<double>(screen_y), layout.canvas);
  shell.doc.needs_fit = false;

  const int percent = static_cast<int>(shell.doc.view.zoom * 100.0 + 0.5);
  wchar_t buffer[160];
  if (shell.doc.view.zoom == before) {
    const bool at_floor = shell.doc.view.zoom <= comp_ui::CanvasView::kMinZoom;
    wsprintfW(buffer, L"缩放 %d%%：已到%s，画面不再变化（Ctrl+0 适合窗口）", percent,
              at_floor ? L"下限 2%" : L"上限 3200%");
  } else {
    wsprintfW(buffer, L"缩放 %d%%（%s·以点击点为锚）", percent,
              zoom_out ? L"缩小" : L"放大");
  }
  set_status(shell, buffer);
}

/* ------------------------------------------------------- 缩放光标（自定义） */

/*
 * 系统光标表里没有「放大镜 +/-」，用 32x32 单色光标自绘一个：
 *   - 圆环 [7.2, 8.0] 像素带 = 黑描边，[5.6, 7.2] = 白环，玻璃内透明；
 *   - 手柄从 (18,18) 到 (27,27)，宽 2.2 px 白芯 + 黑边；
 *   - 玻璃中心画横线（缩小）或横竖两线（放大）。
 * 单色光标用 AND/XOR 两个位平面：AND=1,XOR=0 透明；AND=0,XOR=0 黑；
 * AND=0,XOR=1 白（实际是反色，黑白背景都看得见）。32 px 宽正好 4 字节/行，
 * 不需要行尾补齐。进程内各建一次并缓存；建失败时回落到箭头。
 */
HCURSOR make_zoom_cursor(bool zoom_out) {
  constexpr int kSize = 32;
  constexpr int kStride = kSize / 8; /* 32 位 -> 4 字节，天然 WORD 对齐 */
  uint8_t and_bits[kStride * kSize];
  uint8_t xor_bits[kStride * kSize];
  std::memset(and_bits, 0xFF, sizeof(and_bits)); /* 默认全透明 */
  std::memset(xor_bits, 0x00, sizeof(xor_bits));

  const double cx = 12.5;
  const double cy = 12.5;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const double px = static_cast<double>(x) + 0.5;
      const double py = static_cast<double>(y) + 0.5;
      const double dx = px - cx;
      const double dy = py - cy;
      const double dist = std::sqrt(dx * dx + dy * dy);

      bool black = false;
      bool white = false;
      /* 圆环：外圈黑、内圈白，玻璃内部留空。 */
      if (dist <= 8.0 && dist >= 5.6) {
        if (dist >= 7.2) {
          black = true;
        } else {
          white = true;
        }
      }
      /* 手柄：线段 (18,18)->(27,27) 的垂直距离与端点圆头。 */
      {
        const double sx = px - 18.0;
        const double sy = py - 18.0;
        const double t = std::min(std::max((sx + sy) * 0.5, 0.0), 6.364);
        const double hx = sx - t * 0.70710678;
        const double hy = sy - t * 0.70710678;
        const double hd = std::sqrt(hx * hx + hy * hy);
        if (hd <= 2.0) {
          if (hd <= 1.1) {
            white = true;
          } else {
            black = true;
          }
        }
      }
      /* 玻璃内的 +/-：约 1.8 px 粗的白线（反色，压在任何内容上都可见）。 */
      if (std::fabs(dy) <= 0.9 && std::fabs(dx) <= 4.0) white = true;
      if (!zoom_out && std::fabs(dx) <= 0.9 && std::fabs(dy) <= 4.0) white = true;

      if (!black && !white) continue; /* 透明 */
      const int index = y * kStride + x / 8;
      const uint8_t mask = static_cast<uint8_t>(0x80u >> (x % 8));
      and_bits[index] = static_cast<uint8_t>(and_bits[index] & ~mask); /* AND=0 */
      if (white) xor_bits[index] = static_cast<uint8_t>(xor_bits[index] | mask);
    }
  }
  return CreateCursor(GetModuleHandleW(nullptr), 12, 12, kSize, kSize, and_bits, xor_bits);
}

HCURSOR zoom_cursor_handle(bool zoom_out) {
  static HCURSOR cached[2] = {nullptr, nullptr};
  HCURSOR& slot = cached[zoom_out ? 1 : 0];
  if (slot == nullptr) slot = make_zoom_cursor(zoom_out);
  return slot;
}

/* ------------------------------------------------------- 框选缩放（Z 拖框） */

/*
 * 按下先只记录起点并抓鼠标：真正的判断放在松手——位移小于 4 px 视为单击，
 * 沿用原来的「点一下放大 1.2 倍」；大于阈值才换算成文档矩形缩放到该区域。
 * 这样单击/拖框共用一个入口，Alt+单击缩小的语义也不受影响。
 */
void zoom_marquee_begin(HWND window, ShellState& shell, int x, int y) {
  shell.doc.zoom_marquee_active = true;
  shell.doc.zoom_marquee_start.x = x;
  shell.doc.zoom_marquee_start.y = y;
  shell.doc.zoom_marquee_current = shell.doc.zoom_marquee_start;
  SetCapture(window);
}

void zoom_marquee_extend(ShellState& shell, int x, int y) {
  if (!shell.doc.zoom_marquee_active) return;
  shell.doc.zoom_marquee_current.x = x;
  shell.doc.zoom_marquee_current.y = y;
}

void zoom_marquee_end(HWND window, ShellState& shell) {
  if (!shell.doc.zoom_marquee_active) return;
  shell.doc.zoom_marquee_active = false;
  const POINT start = shell.doc.zoom_marquee_start;
  const POINT current = shell.doc.zoom_marquee_current;
  /* 先清状态再放捕获：ReleaseCapture 会同步重入 WM_CAPTURECHANGED。 */
  ReleaseCapture();

  Layout layout{};
  if (shell.doc.document == nullptr || !client_layout(shell, &layout) || layout.canvas.empty()) {
    return;
  }
  const int dx = current.x - start.x;
  const int dy = current.y - start.y;
  if (abs(dx) < 4 && abs(dy) < 4) {
    zoom_tool_click(shell, start.x, start.y, layout);
    return;
  }
  const comp_ui::Point a =
      shell.doc.view.screen_to_doc(start.x, start.y, layout.canvas, shell.doc.width, shell.doc.height);
  const comp_ui::Point b =
      shell.doc.view.screen_to_doc(current.x, current.y, layout.canvas, shell.doc.width, shell.doc.height);
  if (!shell.doc.view.zoom_to_doc_rect(a.x, a.y, b.x, b.y, layout.canvas, shell.doc.width,
                                   shell.doc.height)) {
    zoom_tool_click(shell, start.x, start.y, layout);
    return;
  }
  shell.doc.needs_fit = false;
  const int percent = static_cast<int>(shell.doc.view.zoom * 100.0 + 0.5);
  wchar_t buffer[160];
  wsprintfW(buffer, L"缩放 %d%%（框选放大：拖出的区域铺满画布；Ctrl+0 适合窗口）", percent);
  set_status(shell, buffer);
}

void begin_pan(HWND window, ShellState& shell, int x, int y) {
  shell.panning = true;
  shell.pan_anchor.x = x;
  shell.pan_anchor.y = y;
  SetCapture(window);
  SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZEALL)));
}

void end_pan(HWND window, ShellState& shell) {
  if (!shell.panning) return;
  shell.panning = false;
  ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
}

void canvas_click(ShellState& shell, int x, int y, const Layout& layout) {
  if (shell.doc.document == nullptr) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  wchar_t buffer[128];
  wsprintfW(buffer, L"画布 (%d, %d)", static_cast<int>(std::floor(doc.x)),
            static_cast<int>(std::floor(doc.y)));
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 前景色键入（Shift+X） */

void begin_color_input(ShellState& shell) {
  shell.color_input = L"#";
  set_status(shell, L"前景色：键入 #RRGGBB 或 #RGB，回车确认（Esc 取消）");
}

void refresh_color_input_status(ShellState& shell) {
  wchar_t buffer[96];
  wsprintfW(buffer, L"前景色：%s（回车确认，Esc 取消）", shell.color_input.c_str());
  set_status(shell, buffer);
}

void commit_color_input(ShellState& shell) {
  char ascii[8] = {0};
  for (size_t i = 0; i < shell.color_input.size() && i < 7; ++i) {
    const wchar_t ch = shell.color_input[i];
    ascii[i] = ch < 128 ? static_cast<char>(ch) : '?';
  }
  comp_ui::ColorRGB parsed{};
  const bool ok = comp_ui::color_parse_hex(ascii, &parsed);
  shell.color_input.clear();
  if (!ok) {
    set_status(shell, L"前景色：格式无效（需 #RGB 或 #RRGGBB）");
    return;
  }
  shell.foreground = parsed;
  char hex[8] = {0};
  comp_ui::color_to_hex(shell.foreground, hex);
  wchar_t buffer[96];
  wsprintfW(buffer, L"前景色：%s", widen_ascii(hex).c_str());
  set_status(shell, buffer);
}

void handle_key(HWND window, ShellState& shell, WPARAM key, bool ctrl, bool shift, bool alt) {
  /*
   * P0-1：图层名 / 不透明度键入态是最高的模态。字符本体由 WM_CHAR 送进来
   * （CJK 走 WM_IME_CHAR），这里只处理编辑键；其余按键一律吞掉，理由同其它
   * 键入态——输入到一半按到 B，把混合模式改掉而输入框还在，状态不可解释。
   */
  if (shell.doc.layer_input != LayerInput::None) {
    if (key == VK_ESCAPE) {
      panel_cancel_layer_input(shell);
      set_status(shell, L"已取消图层输入");
      return;
    }
    if (key == VK_BACK) {
      panel_layer_input_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      panel_commit_layer_input(shell);
      return;
    }
    return;
  }

  /*
   * §5.44 选区手柄拖动中：Esc 取消。拖动阶段引擎从未被写入（松手才提交），
   * 所以「取消」只是丢掉预览 + 放掉捕获，不产生撤销步；其余按键吞掉，
   * 免得切工具/撤销在拖动中途把状态撕成两半。
   */
  if (shell.doc.selection_handle_drag >= 0) {
    if (key == VK_ESCAPE) {
      selection_handle_cancel(shell);
      ReleaseCapture();
      InvalidateRect(window, nullptr, FALSE);
      set_status(shell, L"选区手柄：已取消，选区未变");
      return;
    }
    return;
  }

  /* 前景色十六进制键入态最先生效：它是模态输入，不能漏到工具快捷键。 */
  if (!shell.color_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.color_input.clear();
      set_status(shell, L"已取消前景色输入");
      return;
    }
    if (key == VK_BACK) {
      shell.color_input.pop_back();
      if (shell.color_input.empty()) {
        set_status(shell, L"已取消前景色输入");
      } else {
        refresh_color_input_status(shell);
      }
      return;
    }
    if (key == VK_RETURN) {
      commit_color_input(shell);
      return;
    }
    const bool hex_digit = (key >= '0' && key <= '9') || (key >= 'A' && key <= 'F') ||
                           (key >= 'a' && key <= 'f');
    if (hex_digit && shell.color_input.size() < 7) {
      shell.color_input.push_back(static_cast<wchar_t>(key));
      refresh_color_input_status(shell);
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 裁剪框待提交态（S22）：Enter 提交 / Esc 取消。
   * 与颜色/羽化输入态同级——都是模态，任何其中一个激活时其余快捷键一律让路，
   * 否则「框已拖好、手滑按到 B」会把混合模式改掉而裁剪框还在，状态不可解释。
   */
  if (shell.doc.crop_active) {
    if (key == VK_RETURN) {
      crop_commit(shell);
      return;
    }
    if (key == VK_ESCAPE) {
      crop_cancel(shell);
      set_status(shell, L"已取消裁剪");
      return;
    }
  }

  /* 羽化半径键入态（Shift+F）：只吃数字/退格/回车/Esc，其余键一律屏蔽。 */
  if (shell.doc.feather_input_active) {
    if (key == VK_ESCAPE) {
      shell.doc.feather_input.clear();
      shell.doc.feather_input_active = false;
      set_status(shell, L"已取消羽化输入");
      return;
    }
    if (key == VK_BACK) {
      shell.doc.feather_input.pop_back();
      if (shell.doc.feather_input.empty()) {
        shell.doc.feather_input_active = false;
        set_status(shell, L"已取消羽化输入");
      } else {
        refresh_feather_input_status(shell);
      }
      return;
    }
    if (key == VK_RETURN) {
      commit_feather_input(shell);
      return;
    }
    if (key >= '0' && key <= '9' && shell.doc.feather_input.size() < 3) {
      shell.doc.feather_input.push_back(static_cast<wchar_t>(key));
      refresh_feather_input_status(shell);
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 扩展/收缩/边界/平滑半径键入态（P0-4 / P0-5）：同款输入机，回车才落盘。 */
  if (shell.doc.modify_input_active) {
    if (key == VK_ESCAPE) {
      shell.doc.modify_input.clear();
      shell.doc.modify_input_active = false;
      set_status(shell, L"已取消选区修改");
      return;
    }
    if (key == VK_BACK) {
      shell.doc.modify_input.pop_back();
      if (shell.doc.modify_input.empty()) {
        shell.doc.modify_input_active = false;
        set_status(shell, L"已取消选区修改");
      } else {
        refresh_modify_input_status(shell);
      }
      return;
    }
    if (key == VK_RETURN) {
      commit_modify_input(shell);
      return;
    }
    if (key >= '0' && key <= '9' && shell.doc.modify_input.size() < 3) {
      shell.doc.modify_input.push_back(static_cast<wchar_t>(key));
      refresh_modify_input_status(shell);
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 文档级对话框（新建 / 图像大小 / 画布大小 / 历史容量）：模态吃键，Ctrl 组合
   * 仍可穿透（与滤镜/效果/修边面板同一条模态规矩）。键入态也在这里处理——
   * doc_dialog_input 只属于这个对话框，不必另开分支。
   */
  if (shell.doc.doc_dialog != comp_ui::DocDialogKind::None && !ctrl) {
    if (!shell.doc.doc_dialog_input.empty()) {
      switch (key) {
        case VK_ESCAPE:
          shell.doc.doc_dialog_input.clear();
          set_status(shell, L"已取消输入");
          return;
        case VK_BACK:
          shell.doc.doc_dialog_input.pop_back();
          if (shell.doc.doc_dialog_input.empty()) {
            set_status(shell, L"已取消输入");
          } else {
            wchar_t buffer[96];
            wsprintfW(buffer, L"输入中：%s", shell.doc.doc_dialog_input.c_str());
            set_status(shell, buffer);
          }
          return;
        case VK_RETURN:
          /* 键入态下回车 = 落值 + 应用，少按一次键（apply 内部先提交草稿）。 */
          doc_dialog_apply(window, shell);
          return;
        case VK_TAB:
          /* 先落值再切行，免得切走即丢草稿。 */
          doc_dialog_commit_input(shell);
          doc_dialog_focus_row(shell, shift ? -1 : 1);
          return;
        default:
          break;
      }
      if (key == VK_OEM_MINUS || key == VK_SUBTRACT) {
        doc_dialog_begin_input(shell, L'-');
        return;
      }
      /*
       * 小数点（色标表的「位置/中点」两行）：输入框只收整数，.5 这种一位小数
       * 由 doc_dialog_commit_input 拼成 %。其它行 doc_dialog_begin_input 会拒收。
       */
      if (key == VK_OEM_PERIOD || key == VK_DECIMAL) {
        doc_dialog_begin_input(shell, L'.');
        return;
      }
      if (key >= '0' && key <= '9') {
        doc_dialog_begin_input(shell, static_cast<wchar_t>(key));
        return;
      }
      return; /* 输入态下屏蔽其它快捷键，避免误触发 */
    }
    /*
     * 关闭提示的字母快捷键（P0-3）：S = 保存并关闭、D = 不保存直接关闭。
     * 只给 ClosePrompt 开这个口子——其它对话框必须继续吞掉字母键，否则工具
     * 快捷键会从面板底下把当前工具换掉。
     */
    if (shell.doc.doc_dialog == comp_ui::DocDialogKind::ClosePrompt) {
      if (key == 'S' || key == 'D') {
        /*
         * 文字工具还开着时（键入态在下面才处理），TranslateMessage 随后还会送一个
         * 同名字符：不吞掉它，按 S 保存却取消另存为的话，那个 'S' 会被插进待提交
         * 的文字里——用户刚说「保存」，文档反而被改了。
         */
        shell.doc.type_skip_char = true;
        doc_dialog_set_row_value(
            shell, 0, key == 'S' ? comp_ui::kCloseActionSave : comp_ui::kCloseActionDiscard);
        doc_dialog_apply(window, shell);
        return;
      }
    }
    switch (key) {
      case VK_ESCAPE:
        doc_dialog_close(shell, L"已取消");
        return;
      case VK_TAB:
        doc_dialog_focus_row(shell, shift ? -1 : 1);
        return;
      case VK_RETURN:
        doc_dialog_apply(window, shell);
        return;
      case VK_UP:
        doc_dialog_focus_row(shell, -1);
        return;
      case VK_DOWN:
        doc_dialog_focus_row(shell, 1);
        return;
      case VK_LEFT:
        doc_dialog_step(shell, -1);
        return;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: {
        /*
         * 允许负值的行（Camera Raw）：'-' 起输入，而不是步进 -1——否则负值只能
         * 靠长按左箭头从 0 数下去。这些行仍可用 LEFT 步进，减号键被借用不算
         * 丢能力；其它对话框 min >= 0，行为保持原样。
         */
        const comp_ui::DocRowSpec spec =
            comp_ui::doc_dialog_row(shell.doc.doc_dialog, shell.doc.doc_dialog_focus);
        if (spec.kind == comp_ui::DocRowKind::Number && spec.min < 0) {
          doc_dialog_begin_input(shell, L'-');
          return;
        }
        doc_dialog_step(shell, -1);
        return;
      }
      case VK_RIGHT:
      case VK_OEM_PLUS:
        doc_dialog_step(shell, 1);
        return;
      case VK_SPACE:
        /* 空格 = 步进 +1：开/关、背景、锚点都用同一颗键翻。 */
        doc_dialog_step(shell, 1);
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      doc_dialog_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    /* 其余按键吞掉：面板打开时不让工具字母键把底层工具换掉。 */
    return;
  }

  /* 修边面板的容差键入态：只吃数字/退格/回车/Esc。 */
  if (!shell.doc.trim_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.doc.trim_input.clear();
      set_status(shell, L"已取消容差输入");
      return;
    }
    if (key == VK_BACK) {
      shell.doc.trim_input.pop_back();
      if (shell.doc.trim_input.empty()) {
        set_status(shell, L"已取消容差输入");
      } else {
        wchar_t buffer[96];
        wsprintfW(buffer, L"输入中：%s", shell.doc.trim_input.c_str());
        set_status(shell, buffer);
      }
      return;
    }
    if (key == VK_RETURN) {
      trim_commit_input(shell);
      return;
    }
    if (key >= '0' && key <= '9') {
      trim_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 修边面板打开时，键盘归面板：Esc 只关面板（不关窗），Ctrl 组合仍可穿透
   * （与滤镜/效果面板同一条模态规矩）。数字键直接把焦点落到 Tolerance 再键入。
   */
  if (shell.doc.trim_open && !ctrl) {
    switch (key) {
      case VK_ESCAPE:
        trim_close(shell);
        return;
      case VK_TAB:
        trim_focus_row(shell, shift ? -1 : 1);
        return;
      case VK_RETURN:
        trim_apply(shell);
        return;
      case VK_OEM_MINUS:
        trim_step_value(shell, -1);
        return;
      case VK_OEM_PLUS:
        trim_step_value(shell, 1);
        return;
      case VK_BACK:
        shell.doc.trim.reset();
        set_status(shell, L"修边参数已重置为默认值");
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      shell.doc.trim.focused = 5;
      trim_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    /* 其余按键在面板打开时吞掉：避免按工具字母键把面板留在「另一个工具」上。 */
    return;
  }

  /* 滤镜面板输入态：与调整面板同一套输入规矩，但目标值是滤镜参数。 */
  if (!shell.doc.filter_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.doc.filter_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      filter_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      /* 键入 + 回车 = 落值并立刻应用，少一次按键（应用结果会盖掉预览，不必先预览）。 */
      filter_commit_input(shell, false);
      apply_filter(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.doc.filter_input.clear();
      filter_focus_row(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      filter_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      filter_begin_input(shell, L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      filter_begin_input(shell, L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 滤镜面板打开时，键盘归面板：Esc 只关面板（不关窗），Ctrl 组合仍可穿透。 */
  if (shell.doc.filter_open && !ctrl) {
    switch (key) {
      case VK_ESCAPE:
        close_filter_panel(shell);
        return;
      case VK_TAB:
        filter_focus_row(shell, shift ? -1 : 1);
        return;
      case VK_RETURN:
        apply_filter(shell);
        return;
      case VK_OEM_MINUS:
        filter_step_value(shell, -1);
        return;
      case VK_OEM_PLUS:
        filter_step_value(shell, 1);
        return;
      case VK_BACK:
        filter_reset_values(shell);
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      filter_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    /* 其余按键在面板打开时吞掉：避免按工具字母键把面板留在「另一个工具」上。 */
    return;
  }

  /* 效果参数面板输入态：与滤镜面板同一套输入规矩，目标值是效果参数。 */
  if (!shell.doc.effect_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.doc.effect_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      panel_effect_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      /* 键入 + 回车 = 落值并立刻应用，少一次按键（与滤镜面板同款）。 */
      panel_effect_commit_input(shell);
      panel_apply_effect(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.doc.effect_input.clear();
      panel_effect_move_param(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      shell.doc.effect_input.push_back(static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      shell.doc.effect_input.push_back(L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      shell.doc.effect_input.push_back(L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 效果参数面板打开时，键盘归面板：Esc 只关面板（不关窗），Ctrl 组合仍可穿透。 */
  if (shell.doc.effect_open && !ctrl) {
    switch (key) {
      case VK_ESCAPE:
        close_effect_panel(shell);
        return;
      case VK_TAB:
        panel_effect_move_param(shell, shift ? -1 : 1);
        return;
      case VK_RETURN:
        panel_apply_effect(shell);
        return;
      case VK_OEM_MINUS:
        panel_effect_step_value(shell, -1);
        return;
      case VK_OEM_PLUS:
        panel_effect_step_value(shell, 1);
        return;
      case VK_BACK:
        panel_effect_reset_values(shell);
        return;
      case VK_OEM_COMMA:
        panel_effect_cycle_kind(shell, -1);
        return;
      case VK_OEM_PERIOD:
        panel_effect_cycle_kind(shell, 1);
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      panel_effect_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    /* 其余按键在面板打开时吞掉：避免工具字母键把面板留在「另一个工具」上。 */
    return;
  }

  /* 填充参数面板输入态（P1-2 收尾 ⑧）：与效果面板同一套输入规矩。 */
  if (!shell.doc.fill_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.doc.fill_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      fill_panel_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      /* 键入 + 回车 = 落值并立刻应用（新建态则建层），少一次按键。 */
      fill_panel_commit_input(shell);
      if (shell.doc.fill_input.empty()) panel_apply_fill(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.doc.fill_input.clear();
      fill_panel_move_param(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      shell.doc.fill_input.push_back(static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      shell.doc.fill_input.push_back(L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      shell.doc.fill_input.push_back(L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 填充参数面板打开时，键盘归面板：Esc 只关面板（不关窗），Ctrl 组合仍可穿透。 */
  if (shell.doc.fill_open && !ctrl) {
    switch (key) {
      case VK_ESCAPE:
        close_fill_panel(shell);
        return;
      case VK_TAB:
        fill_panel_move_param(shell, shift ? -1 : 1);
        return;
      case VK_RETURN:
        panel_apply_fill(shell);
        return;
      case VK_OEM_MINUS:
        fill_panel_step_value(shell, -1);
        return;
      case VK_OEM_PLUS:
        fill_panel_step_value(shell, 1);
        return;
      case VK_BACK:
        fill_panel_reset_values(shell);
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      fill_panel_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    /* 其余按键在面板打开时吞掉：避免工具字母键把面板留在「另一个工具」上。 */
    return;
  }

    return;
  }

  /* 色彩范围容差键入态（P0-4 第二批）：只吃数字/退格/回车/Esc。 */
  if (!shell.doc.color_range_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.doc.color_range_input.clear();
      set_status(shell, L"已取消容差输入");
      return;
    }
    if (key == VK_BACK) {
      shell.doc.color_range_input.pop_back();
      if (shell.doc.color_range_input.empty()) {
        set_status(shell, L"已取消容差输入");
      } else {
        wchar_t buffer[96];
        wsprintfW(buffer, L"输入中：%s", shell.doc.color_range_input.c_str());
        set_status(shell, buffer);
      }
      return;
    }
    if (key == VK_RETURN) {
      color_range_commit_input(shell);
      return;
    }
    if (key >= '0' && key <= '9') {
      color_range_begin_input(shell, static_cast<wchar_t>(key));
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 色彩范围面板打开时，键盘归面板（P0-4 第二批）。与滤镜/效果面板不同，
   * 这里连 Ctrl 组合也要拦：预览挂在显式事务上，撤销/新建/打开会把事务快照
   * 和当前文档撕开，abort 时就会「复活」一个本来已经不该存在的选区。
   * 其余 Ctrl 组合（缩放等）不影响事务，照常穿透。
   */
  if (shell.doc.color_range_open) {
    if (ctrl) {
      if (key == 'Z' || key == 'Y' || key == 'N' || key == 'O' || key == 'S' || key == 'A' ||
          key == 'D') {
        set_status(shell, L"色彩范围预览中：先回车应用或 Esc 取消，再执行该命令");
        return;
      }
    } else {
      switch (key) {
        case VK_ESCAPE:
          color_range_cancel(shell);
          return;
        case VK_RETURN:
          color_range_commit(shell);
          return;
        case VK_OEM_MINUS:
          color_range_step_fuzziness(shell, -1);
          return;
        case VK_OEM_PLUS:
          color_range_step_fuzziness(shell, 1);
          return;
        case VK_BACK:
          color_range_clear_colors(shell);
          return;
        case 'I':
          color_range_toggle_invert(shell);
          return;
        default:
          break;
      }
      if (key >= '0' && key <= '9') {
        color_range_begin_input(shell, static_cast<wchar_t>(key));
        return;
      }
      /* 其余按键在面板打开时吞掉（含工具字母键）：模态，避免面板与工具打架。 */
      return;
    }
  }

  /* 路径面板打开且可见时键盘归面板（P0-4 可选）。Ctrl 组合仍可穿透：面板不留事务。 */
  if (path_panel_visible(shell) && !ctrl) {
    /*
     * 钢笔正在落点时（面板多半是上一条路径留下的）键归钢笔：回车提交当前路径、
     * Esc 取消当前路径；其余键仍吞掉——若放行，Del 会落到「删除选中图层」上。
     */
    if (shell.tool == comp_ui::ToolId::Pen && !shell.doc.pen_anchors.empty()) {
      if (key == VK_RETURN) {
        commit_pen_path(shell);
      } else if (key == VK_ESCAPE) {
        pen_cancel(shell);
      }
      return;
    }
    switch (key) {
      case VK_ESCAPE:
        shell.doc.path_open = false;
        set_status(shell, L"已关闭路径面板");
        return;
      case VK_UP:
        path_select(shell, -1);
        return;
      case VK_DOWN:
        path_select(shell, 1);
        return;
      case VK_RETURN:
        path_apply_selected(shell);
        return;
      case VK_DELETE:
        path_delete_selected(shell);
        return;
      default:
        break;
    }
    /* 其余按键吞掉：避免工具字母键把面板留在「另一个工具」上。 */
    return;
  }

  /* 历史记录面板打开且可见时键盘归面板（P1）。Ctrl 组合仍可穿透：面板不留事务。 */
  if (history_panel_visible(shell) && !ctrl) {
    /*
     * 钢笔落点中的回车/Esc 归钢笔（与路径面板同一条例外）：否则画布上点了锚点
     * 之后回车会被面板吞掉，路径永远提交不了；Del 也不会落到「删除图层」上。
     */
    if (shell.tool == comp_ui::ToolId::Pen && !shell.doc.pen_anchors.empty()) {
      if (key == VK_RETURN) {
        commit_pen_path(shell);
      } else if (key == VK_ESCAPE) {
        pen_cancel(shell);
      }
      return;
    }
    switch (key) {
      case VK_ESCAPE:
        shell.doc.history_open = false;
        set_status(shell, L"已关闭历史记录面板");
        return;
      case VK_UP:
        history_step(shell, -1);
        return;
      case VK_DOWN:
        history_step(shell, 1);
        return;
      case VK_HOME:
        history_jump_to(shell, 0);
        return;
      case VK_END:
        history_jump_to(shell, history_state_count(shell) - 1);
        return;
      default:
        break;
    }
    return; /* 其余按键吞掉：避免工具字母键把面板留在「另一个工具」上。 */
  }

  /*
   * ROI #5 通道面板：面板打开且可见时键盘归面板。命名态吞掉所有键（字符从
   * WM_CHAR 进来）；非命名态下 Ctrl+回车是唯一的 Ctrl 组合（减选），其余
   * Ctrl 快捷键照常穿透。
   */
  if (channel_panel_visible(shell) && (!ctrl || key == VK_RETURN)) {
    if (shell.doc.channel_name_active) {
      switch (key) {
        case VK_ESCAPE:
          channel_name_cancel(shell);
          return;
        case VK_RETURN:
          channel_name_commit(shell);
          return;
        case VK_BACK:
          channel_name_backspace(shell);
          return;
        default:
          return; /* 字符在 WM_CHAR，其它键在命名态一律吞掉 */
      }
    }
    switch (key) {
      case VK_ESCAPE:
        channel_panel_close(shell);
        set_status(shell, L"已关闭通道面板");
        return;
      case VK_UP:
        channel_select(shell, -1);
        return;
      case VK_DOWN:
        channel_select(shell, 1);
        return;
      case VK_RETURN: {
        const comp_selection_combine combine = ctrl ? COMP_SELECTION_SUBTRACT
                                             : shift ? COMP_SELECTION_ADD
                                                     : COMP_SELECTION_REPLACE;
        channel_load_selected(shell, combine);
        return;
      }
      case 'S':
        channel_save_begin(shell);
        /* 同一个按键的 WM_CHAR('s') 别落进刚打开的名字缓冲。 */
        if (shell.doc.channel_name_active) shell.doc.channel_name_skip_char = true;
        return;
      case VK_DELETE:
        channel_delete_selected(shell);
        return;
      default:
        break;
    }
    return; /* 其余按键吞掉：避免工具字母键把面板留在「另一个工具」上。 */
  }

  /*
   * 字体面板（P0-6②）打开且可见时键盘归面板。顺序与右栏的绘制优先级一致：
   * 对话框 -> 路径 -> 历史 -> 字体。字母与中文走 WM_CHAR（输入法提交的也是
   * WM_CHAR），这里只管导航、应用与退格；其余键一律吞掉，别让工具快捷键
   * 在面板背后换工具。
   */
  if (font_panel_visible(shell) && !ctrl) {
    switch (key) {
      case VK_ESCAPE:
        /* 先退过滤（用户往往只是想回到完整列表），再按一次才关面板。 */
        if (!shell.font_filter.empty()) {
          font_filter_clear(shell);
        } else {
          font_panel_close(shell);
          set_status(shell, L"已关闭字体面板");
        }
        return;
      case VK_UP:
        font_select(shell, -1);
        return;
      case VK_DOWN:
        font_select(shell, 1);
        return;
      case VK_PRIOR:
      case VK_NEXT: {
        int page = 8;
        Layout layout{};
        if (client_layout(shell, &layout)) {
          const int rows = comp_ui::font_visible_rows(layout.adjust_panel);
          if (rows > 1) page = rows - 1;
        }
        font_select(shell, key == VK_PRIOR ? -page : page);
        return;
      }
      case VK_HOME:
        font_select_edge(shell, false);
        return;
      case VK_END:
        font_select_edge(shell, true);
        return;
      case VK_RETURN:
        font_apply_row(shell, shell.font_selected);
        return;
      case VK_BACK:
        font_filter_backspace(shell);
        return;
      default:
        break;
    }
    return; /* 其余按键吞掉：面板是模态的（与应用对话框同口径）。 */
  }

  /* 文字属性面板输入态：与滤镜面板同一套输入规矩，目标值是字号/行距/字距。 */
  if (!shell.doc.type_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.doc.type_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      type_panel_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      type_panel_commit_input(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.doc.type_input.clear();
      type_panel_focus(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      type_panel_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      type_panel_begin_input(shell, L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      type_panel_begin_input(shell, L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* P0-3：拖动中的滑杆/曲线点按 Esc = 取消这次拖动（事务回滚，不留撤销步）。 */
  if (shell.doc.curve_drag_point >= 0 && key == VK_ESCAPE) {
    curve_drag_finish(shell, false);
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  if (shell.doc.adj_drag_param >= 0 && key == VK_ESCAPE) {
    adjust_drag_finish(shell, false);
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  /* Move 工具拖图层：Esc 放弃这次位移（abort 事务 = 位置回到按下时）。 */
  if (shell.doc.layer_move_active && key == VK_ESCAPE) {
    move_cancel(shell);
    set_status(shell, L"已取消移动，图层位置未改变");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  /*
   * P0-3：曲线页按 Delete = 删除焦点控制点。必须在图层删除命令之前判——
   * 那边 Delete 是「删图层」，正在调曲线时按到会整层消失。
   */
  if (key == VK_DELETE && shell.doc.adj_kind == COMP_ADJUSTMENT_CURVES &&
      shell.doc.curve_focus_point >= 0) {
    curve_delete_point(shell, shell.doc.curve_focus_point);
    InvalidateRect(window, nullptr, FALSE);
    return;
  }

  /* 调整面板输入态优先：Esc 取消输入（不关窗）、退格删字符、回车提交、数字/./- 进缓冲。 */
  if (!shell.doc.adj_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.doc.adj_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      panel_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      panel_commit_input(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.doc.adj_input.clear();
      panel_move_param(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      shell.doc.adj_input.push_back(static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      shell.doc.adj_input.push_back(L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      shell.doc.adj_input.push_back(L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 文字键入态：最优先吃掉字符键，避免 'B' 被当成画笔快捷键、空格被当成抓手。
   * Esc 取消、Enter 提交、Shift+Enter 换行、Backspace 删字符；[ / ] 调字号与字距。
   * Ctrl 组合放行（撤销/打开等仍可用），其余未识别键一律吞掉（模态输入规矩）。
   */
  if (shell.doc.type_active) {
    if (key == VK_ESCAPE) {
      type_cancel(shell);
      set_status(shell, L"已取消文字输入（未产生撤销步）");
      return;
    }
    if (key == VK_RETURN) {
      if (shift) {
        shell.doc.type_text.push_back('\n');
        shell.doc.type_pending_high = 0; /* 换行把没拼完的代理对隔断了 */
        type_refresh_status(shell);
      } else {
        type_commit(shell);
      }
      return;
    }
    if (key == VK_BACK) {
      /* 按码点删：UTF-8 下一个汉字 3 字节，按字节删会留下半个码点（引擎会拒收）。 */
      comp_ui::type_utf8_erase_last(shell.doc.type_text);
      shell.doc.type_pending_high = 0;
      type_refresh_status(shell);
      return;
    }
    if (key == VK_OEM_4 || key == VK_OEM_6) {
      type_step_param(shell, key == VK_OEM_4 ? -1 : 1, shift);
      /* 这一键已经是快捷键，TranslateMessage 随后还会送一个 '[' / ']'：吃掉它。 */
      shell.doc.type_skip_char = true;
      return;
    }
    if (!ctrl) {
      /*
       * 字符本体一律走 WM_CHAR（输入法、键盘布局、AltGr 的共同出口）。这里不再
       * 自己按 VK 查表：两条路同时插字符会让每个按键都打两遍。
       */
      return; /* 键入态下屏蔽其它快捷键，避免误触发 */
    }
  }

  /*
   * 文字属性面板（Type 工具、未在键入文字时）：Tab 切行、- / = 调值、数字键输入。
   * [ / ] 仍走下面的 type_step_param（字号/字距），Alt+[ / ] 走 WM_SYSKEYDOWN（行距）。
   */
  if (shell.tool == comp_ui::ToolId::Type && !ctrl) {
    switch (key) {
      case VK_TAB:
        type_panel_focus(shell, shift ? -1 : 1);
        return;
      case VK_OEM_MINUS:
        type_panel_step(shell, -1);
        return;
      case VK_OEM_PLUS:
        type_panel_step(shell, 1);
        return;
      case VK_RETURN:
        /* 字体行回车 = 打开字体面板（选真实字体）；其余行没有「回车落值」的语义。 */
        if (comp_ui::type_panel_row_is_toggle(shell.type_panel_focused)) {
          font_panel_toggle(window, shell);
          return;
        }
        break;
      default:
        break;
    }
    if (key >= '0' && key <= '9' && !comp_ui::type_panel_row_is_toggle(shell.type_panel_focused)) {
      type_panel_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
  }

  /* ROI #8：破坏性调整态下 Esc 只退出该态（不关窗、不改像素，与输入态同一规矩）。 */
  if (key == VK_ESCAPE && shell.doc.adj_destructive) {
    panel_exit_destructive(shell);
    InvalidateRect(window, nullptr, FALSE);
    return;
  }

  if (key == VK_ESCAPE) {
    /* 钢笔有未提交路径时 Esc 只取消路径，不关窗（与输入态的 Esc 同规矩）。 */
    if (shell.tool == comp_ui::ToolId::Pen && !shell.doc.pen_anchors.empty()) {
      pen_cancel(shell);
      return;
    }
    /* 形状拖拽中 Esc 只取消这一笔预览，不关窗。 */
    if (shell.doc.shape_active) {
      shape_cancel(shell);
      set_status(shell, L"已取消形状拖拽（未产生撤销步）");
      return;
    }
    PostMessageW(window, WM_CLOSE, 0, 0);
    return;
  }
  if (ctrl) {
    switch (key) {
      case 'A':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjAdd);
          return;
        }
        run_shell_command(window, shell, kCmdSelectAll);
        return;
      case 'I':
        /* Ctrl+Shift+I 反选（与上游一致）；Ctrl+I = 图像 > 反相（破坏性落墨）。 */
        if (shift) {
          run_shell_command(window, shell, kCmdSelectInvert);
          return;
        }
        run_shell_command(window, shell, kCmdImageInvert);
        return;
      case 'U':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjApply);
          return;
        }
        break;
      case 'K':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjKindNext);
          return;
        }
        break;
      case 'J':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjKindPrev);
          return;
        }
        /* Ctrl+J 复制图层（对齐上游 Duplicate Layer 的 ⌘J）。 */
        run_shell_command(window, shell, kCmdLayerDuplicate);
        return;
      case 'G':
        /* Ctrl+G 建组（对齐上游 Group Layers）；Ctrl+Alt+G 剪贴蒙版走 SYSKEYDOWN。 */
        if (!shift) {
          run_shell_command(window, shell, kCmdLayerNewGroup);
          return;
        }
        break;
      case 'D':
        /* Ctrl+Shift+D 删除选中的调整；Ctrl+D 取消选区。 */
        if (shift) {
          run_shell_command(window, shell, kCmdAdjRemove);
          return;
        }
        run_shell_command(window, shell, kCmdSelectNone);
        return;
      case 'Z':
        /* Ctrl+Shift+Z 与 Ctrl+Y 同为重做，兼容两种习惯。 */
        run_shell_command(window, shell, shift ? kCmdRedo : kCmdUndo);
        return;
      case 'Y':
        run_shell_command(window, shell, kCmdRedo);
        return;
      case 'N':
        /* Ctrl+Shift+N 新建图层（沿用图像编辑器惯例），Ctrl+N 新建文档。 */
        run_shell_command(window, shell, shift ? kCmdLayerAdd : kCmdNew);
        return;
      case 'E':
        /* Ctrl+Shift+E = 效果参数面板；Ctrl+E 仍是向下合并（对齐上游 ⌘E）。 */
        if (shift) {
          run_shell_command(window, shell, kCmdEffectEdit);
          return;
        }
        run_shell_command(window, shell, kCmdLayerMergeDown);
        return;
      case 'O':
        /* Ctrl+Shift+O 打开位图，Ctrl+O 打开 .comp——两条路径语义不同，不能合并。 */
        run_shell_command(window, shell, shift ? kCmdOpenImage : kCmdOpen);
        return;
      /*
       * P0-4 可选：Ctrl+Shift+P = 路径面板。P 本身是钢笔工具键位，带 Ctrl 不冲突；
       * 不用 Ctrl+Shift+Q（无实义）是因为 P 更像 Paths。
       */
      case 'P':
        if (shift) {
          run_shell_command(window, shell, kCmdSelectPaths);
          return;
        }
        break;
      case 'S':
        /* Ctrl+Shift+S 另存为位图，Ctrl+S 保存 .comp。 */
        run_shell_command(window, shell, shift ? kCmdExportImage : kCmdSave);
        return;
      /*
       * P1-3 变换：Ctrl+T 数值变换（上游 ⌘T 的对位），Ctrl+Shift+T 再次变换。
       * 文字工具没有占用 T 之前的 Ctrl 组合，这两个键此前都空着。
       */
      case 'T':
        run_shell_command(window, shell,
                          shift ? kCmdLayerTransformAgain : kCmdLayerTransformNumeric);
        return;
      case 'V':
        /*
         * P0-4：粘贴。Ctrl+V 此前落到 Move 工具键位（V），图层显隐因此改用
         * Ctrl+Shift+V——剪贴板是编辑器的基本动作，工具快捷键让位。
         */
        if (shift) {
          run_shell_command(window, shell, kCmdLayerToggleVisible);
          return;
        }
        run_shell_command(window, shell, kCmdEditPaste);
        return;
      case 'C':
        /* Ctrl+C 拷贝、Ctrl+Shift+C 合并拷贝；C 单独的裁剪工具键位不受影响。 */
        run_shell_command(window, shell, shift ? kCmdEditCopyMerged : kCmdEditCopy);
        return;
      case 'X':
        /* Ctrl+X 剪切（此前没有绑定，裁剪工具是 C）。 */
        run_shell_command(window, shell, kCmdEditCut);
        return;
      /*
       * S24 镜像：Ctrl+Shift+H 水平、Ctrl+Shift+F 垂直（避开已占用的 Ctrl+Shift+V；
       * H/F 取 Flip Horizontal / Flip Vertical 首字母，与上游 ⌘T 系变换分开记）。
       */
      case 'H':
        if (shift) {
          run_shell_command(window, shell, kCmdLayerFlipHorizontal);
          return;
        }
        break;
      case 'F':
        if (shift) {
          run_shell_command(window, shell, kCmdLayerFlipVertical);
          return;
        }
        break;
      case '0':
        run_shell_command(window, shell, kCmdViewFit);
        return;
      case '1':
        run_shell_command(window, shell, kCmdViewActual);
        return;
      /*
       * ROI #3 视图参考：与上游同键位。
       *   Ctrl+R         标尺
       *   Ctrl+;         参考线显示
       *   Ctrl+Shift+;   对齐（吸附）
       *   Ctrl+'         网格
       * 四个键此前都空着；它们只改覆盖层/落点，不改文档像素。
       */
      case 'R':
        run_shell_command(window, shell, kCmdViewRulers);
        return;
      case VK_OEM_1:
        run_shell_command(window, shell, shift ? kCmdViewSnap : kCmdViewGuides);
        return;
      case VK_OEM_7:
        run_shell_command(window, shell, kCmdViewGrid);
        return;
      case VK_OEM_PLUS:
        run_shell_command(window, shell, kCmdViewZoomIn);
        return;
      case VK_OEM_MINUS:
        run_shell_command(window, shell, kCmdViewZoomOut);
        return;
      case VK_OEM_4: /* Ctrl+[ = 图层不透明度 -10% */
        run_shell_command(window, shell, kCmdLayerOpacityDown);
        return;
      case VK_OEM_6: /* Ctrl+] = 图层不透明度 +10% */
        run_shell_command(window, shell, kCmdLayerOpacityUp);
        return;
      default:
        break;
    }
  }
  switch (key) {
    /*
     * 图层变换（S21）：方向键与 Alt+[ ] 在带修饰时归图层几何，不带修饰仍是既有
     * 行为（VK_UP/DOWN 选图层、[ ] 调笔刷/容差/字号）。
     *   方向键             = 选图层（不变）
     *   Shift+方向         = 平移 1 px 微调
     *   Ctrl+方向          = 平移 10 px
     *   Ctrl+Shift+左/右  = 旋转 ∓15°（Alt 被 Move 的「复制」占用，不取）
     *   Alt+[ / ]         = 等比例缩放 ±1%，中心锚定
     * 步长全部由修饰现场决定，直接调用 panel_* 函数并传 dx/dy：命令常量只表达
     * 「动作」，把粗细留给按键，壳侧就不存在第二套步长口径。
     */
    case VK_LEFT:
    case VK_RIGHT: {
      if (ctrl && shift) {
        panel_rotate_layer(shell, key == VK_RIGHT);
        break;
      }
      if (!ctrl && !shift) {
        /*
         * Move 工具下无修饰方向键 = 微调 1 px（上游 V 的语义）。其它工具保持既有行为
         * ——左右方向无动作、上下方向选图层——否则会把「换图层」这条基本操作抢走。
         */
        if (shell.tool == comp_ui::ToolId::Move && shell.doc.document != nullptr) {
          panel_move_layer(shell, key == VK_LEFT ? -1 : 1, 0);
        }
        break;
      }
      const int step = comp_ui::layer_transform_move_step(ctrl);
      panel_move_layer(shell, key == VK_LEFT ? -step : step, 0);
      break;
    }
    case VK_UP:
    case VK_DOWN: {
      /*
       * S22：Ctrl+Shift+↑/↓ = 上移/下移一层（面板序）。
       * 该组合此前未被占用（左右方向的 Ctrl+Shift 是 S21 的旋转），因此不与
       * 任何既有键位冲突。
       */
      if (ctrl && shift) {
        run_shell_command(window, shell,
                          key == VK_UP ? kCmdLayerBringForward : kCmdLayerSendBackward);
        break;
      }
      /* Move 工具 + 无修饰 = 微调 1 px；与左右方向同一口径，见上。 */
      if (!shift && !ctrl && shell.tool == comp_ui::ToolId::Move &&
          shell.doc.document != nullptr) {
        panel_move_layer(shell, 0, key == VK_UP ? -1 : 1);
        break;
      }
      /* 无修饰仍是选图层；Shift/Ctrl 才平移（避免与既有 ↑↓ 选图层抢键）。 */
      if (shift || ctrl) {
        const int step = comp_ui::layer_transform_move_step(ctrl);
        panel_move_layer(shell, 0, key == VK_UP ? -step : step);
        break;
      }
      run_shell_command(window, shell,
                        key == VK_UP ? kCmdLayerSelectUp : kCmdLayerSelectDown);
      break;
    }
    case VK_OEM_4: /* [ = 笔刷变小；Shift+[ = 硬度降低；Wand = 容差 -；Type = 字号/字距 - */
      if (shell.tool == comp_ui::ToolId::Type) {
        type_step_param(shell, -1, shift);
      } else if (shell.tool == comp_ui::ToolId::Wand) {
        shell.wand_tolerance = comp_ui::step_wand_tolerance(shell.wand_tolerance, -1);
        wchar_t buffer[160];
        wsprintfW(buffer, L"魔棒容差 %d（窗口 %d×%d，逐通道，alpha 参与）", shell.wand_tolerance,
                  shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1);
        set_status(shell, buffer);
      } else {
        brush_step(shell, -1, shift);
      }
      break;
    case VK_OEM_6: /* ] = 笔刷变大；Shift+] = 硬度提高；Wand = 容差 +；Type = 字号/字距 + */
      if (shell.tool == comp_ui::ToolId::Type) {
        type_step_param(shell, 1, shift);
      } else if (shell.tool == comp_ui::ToolId::Wand) {
        shell.wand_tolerance = comp_ui::step_wand_tolerance(shell.wand_tolerance, 1);
        wchar_t buffer[160];
        wsprintfW(buffer, L"魔棒容差 %d（窗口 %d×%d，逐通道，alpha 参与）", shell.wand_tolerance,
                  shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1);
        set_status(shell, buffer);
      } else {
        brush_step(shell, 1, shift);
      }
      break;
    case VK_TAB:
      panel_move_param(shell, shift ? -1 : 1);
      break;
    /* P0-1：图层删除 / 重命名（面板焦点不独立，快捷键作用于当前选中图层）。 */
    case VK_DELETE:
      run_shell_command(window, shell, kCmdLayerDelete);
      break;
    /*
     * P0-4：Backspace = 清除选区内的图层像素（Photoshop 里 Del 与 Backspace 等价）。
     * 这里只认 Backspace，Del 保持「删除图层」——改 Del 会动到既有手感与自检脚本；
     * 没有选区时也走清除（整层清空），与上游一致。
     */
    case VK_BACK:
      run_shell_command(window, shell, kCmdEditClear);
      break;
    case VK_F2:
      run_shell_command(window, shell, kCmdLayerRename);
      break;
    case VK_RETURN:
      /* 钢笔：回车 = 提交闭合路径；其余工具里回车仍是调整面板的落值键。 */
      if (shell.tool == comp_ui::ToolId::Pen && !shell.doc.pen_anchors.empty()) {
        commit_pen_path(shell);
      } else {
        panel_commit_input(shell);
      }
      break;
    case VK_OEM_COMMA:
      panel_select_adjustment(shell, -1);
      break;
    case VK_OEM_PERIOD:
      panel_select_adjustment(shell, 1);
      break;
    case VK_OEM_MINUS:
      panel_step_value(shell, -1);
      break;
    case VK_OEM_PLUS:
      panel_step_value(shell, 1);
      break;
    default:
      /* 数字键在调整面板里开始键入数值；其余字母按上游工具键位映射。 */
      if (key >= '0' && key <= '9') {
        panel_begin_input(shell, static_cast<wchar_t>(key));
        break;
      }
      /* Shift+O = 直接键入图层不透明度（对齐上游双击数值的做法，比 ±10% 步进精确）。 */
      if (key == 'O' && shift && !ctrl) {
        run_shell_command(window, shell, kCmdLayerOpacitySet);
        break;
      }
      /* X = 下一项色板；Shift+X = 键入十六进制。前景色是画笔与吸管的公共落点。 */
      if (key == 'X' && !ctrl) {
        if (shift) {
          begin_color_input(shell);
          break;
        }
        shell.palette_index = comp_ui::step_swatch(shell.palette_index, 1);
        shell.foreground = comp_ui::swatch_at(shell.palette_index);
        char hex[8] = {0};
        comp_ui::color_to_hex(shell.foreground, hex);
        wchar_t buffer[160];
        wsprintfW(buffer, L"前景色：%s（色板 %d/%d，Shift+X 键入十六进制）",
                  widen_ascii(hex).c_str(), shell.palette_index + 1,
                  comp_ui::swatch_count());
        set_status(shell, buffer);
        break;
      }
      /* Shift+F = 键入新选区羽化半径；M = 框选形状循环；Shift+W = 魔棒取样来源。 */
      if (key == 'F' && shift) {
        begin_feather_input(shell);
        break;
      }
      if (key == 'M' && !ctrl) {
        if (shell.tool == comp_ui::ToolId::Marquee) {
          shell.marquee_shape = comp_ui::marquee_shape_next(shell.marquee_shape);
          wchar_t buffer[160];
          wsprintfW(buffer, L"框选形状：%s（再按 M 切换）",
                    widen_ascii(comp_ui::marquee_shape_name(shell.marquee_shape)).c_str());
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Marquee);
        }
        break;
      }
      if (key == 'W' && shift && !ctrl) {
        if (shell.tool == comp_ui::ToolId::Wand) {
          shell.wand_merged = !shell.wand_merged;
          set_status(shell, shell.wand_merged
                                ? L"魔棒取样：合并图层（Shift+W 切回当前图层）"
                                : L"魔棒取样：当前图层（Shift+W 切到合并）");
        } else {
          set_tool(shell, comp_ui::ToolId::Wand);
        }
        break;
      }
      /*
       * Shift+B = 混合模式后退一步、Shift+N 同键位前进一步（对齐上游 Shift++ / Shift+−
       * 的菜单序循环；本壳 OEM_PLUS/MINUS 已被调整面板与缩放占用，改取语义键）。
       * 大写 B 不与 Brush 的小写 'b' 键位冲突（工具快捷键只匹配小写）。
       */
      if (key == 'B' && shift && !ctrl) {
        run_shell_command(window, shell, kCmdLayerBlendPrev);
        break;
      }
      if (key == 'N' && shift && !ctrl) {
        run_shell_command(window, shell, kCmdLayerBlendNext);
        break;
      }
      /*
       * S22：Ctrl+Shift+M = 从选区建立图层蒙版；Ctrl+Alt+G = 剪贴蒙版开关。
       * M 在 Ctrl+Shift 下未被占用（单独 M 是 Marquee，Ctrl+M 无定义）；
       * G 单独是渐变工具，故剪贴必须带 Alt 才能与它分开。
       * Alt 由 WM_SYSKEYDOWN 上报，这里用 alt 标志区分（见本函数开头的取键）。
       */
      if (key == 'M' && ctrl && shift) {
        run_shell_command(window, shell, kCmdLayerMaskFromSelection);
        break;
      }
      if (key == 'G' && !ctrl) {
        /* 重复按 G 在同一工具内循环五种渐变类型（对齐上游的同类工具习惯）。 */
        if (shell.tool == comp_ui::ToolId::Gradient) {
          shell.gradient_type = comp_ui::next_gradient_type(shell.gradient_type);
          wchar_t buffer[192];
          wsprintfW(buffer, L"渐变类型：%s（再按 G 切换；Shift = 反向色标）",
                    widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str());
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Gradient);
        }
        break;
      }
      if (key == 'U' && !ctrl) {
        /* 重复按 U 在同一工具内循环 Rectangle -> Ellipse -> Line（与 M/G/R 同款习惯）。 */
        if (shell.tool == comp_ui::ToolId::Shape) {
          shell.shape_kind = comp_ui::next_shell_shape_kind(shell.shape_kind);
          wchar_t buffer[224];
          wsprintfW(buffer, L"形状种类：%s（再按 U 切换；%s）",
                    widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
                    shell.shape_kind == comp_ui::kShapeLine
                        ? L"Alt+[ / ] 调线宽"
                        : L"Shift+Alt+[ / ] 调圆角，Alt+[ / ] 调线宽");
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Shape);
        }
        break;
      }
      if (key < 128) {
        /*
         * Shift+S = 仿制「对齐」开关（只在仿制工具激活时拦截）。PS 把它放在选项栏，
         * 本壳没有选项栏，用同一字母的 Shift 组合最省记忆；不带 Shift 的 S 仍是选工具。
         * 关掉对齐必须清掉已存偏移，否则重新打开时会复用关掉之前的首笔偏移。
         */
        if (key == 'S' && shift && shell.tool == comp_ui::ToolId::Clone) {
          shell.clone_aligned = !shell.clone_aligned;
          if (!shell.clone_aligned) shell.doc.clone_has_offset = false;
          wchar_t buffer[192];
          wsprintfW(buffer,
                    shell.clone_aligned
                        ? L"仿制对齐：开（整段会话复用首笔偏移；Shift+S 切换）"
                        : L"仿制对齐：关（每一笔都以源点重新对位；Shift+S 切换）");
          set_status(shell, buffer);
          break;
        }
        /*
         * Shift+K = 仿制取样范围（当前图层 / 整篇合成）的持久开关。K 不在工具键位表里，
         * 不会抢别的工具的字母；与 Shift+S 同一套「Shift+工具字母 = 工具选项」记法。
         */
        if (key == 'K' && shift && shell.tool == comp_ui::ToolId::Clone) {
          clone_toggle_sample_all(shell);
          break;
        }
        const comp_ui::ToolId id =
            comp_ui::tool_shortcut_target(shell.tool, static_cast<char>(key));
        if (id == comp_ui::ToolId::Retouch) {
          /* 重复按 R 在同一工具内循环 Blur -> Smudge -> Liquify（对齐上游）。 */
          set_retouch_mode(shell, shell.tool == comp_ui::ToolId::Retouch
                                      ? comp_ui::next_retouch_mode(shell.retouch_mode)
                                      : comp_ui::kRetouchBlur);
        } else if (id == comp_ui::ToolId::Tone) {
          /* 重复按 O 在 8 档里循环；从别的工具切进来一律回到 Dodge 中间调。 */
          set_tone_state(shell, shell.tool == comp_ui::ToolId::Tone
                                    ? comp_ui::next_tone_state(shell.tone_state)
                                    : comp_ui::kToneDodgeMidtone);
        } else if (id == comp_ui::ToolId::Heal) {
          /*
           * 重复按 J 在三种修补算法里循环；从别的工具切进来回到 Content-Aware
           * （最慢但最像 PS 的默认档），避免上一次会话的算法静默生效。
           */
          if (shell.tool == comp_ui::ToolId::Heal) {
            shell.heal_mode = comp_ui::next_heal_mode(shell.heal_mode);
            wchar_t buffer[224];
            wsprintfW(buffer,
                      L"修补算法：%s（再按 J 切换；Content-Aware / Create Texture / "
                      L"Proximity Match）",
                      widen_ascii(comp_ui::heal_mode_name(shell.heal_mode)).c_str());
            set_status(shell, buffer);
          } else {
            shell.heal_mode = comp_ui::kHealContentAware;
            set_tool(shell, comp_ui::ToolId::Heal);
          }
        } else if (id != comp_ui::ToolId::Count) {
          set_tool(shell, id);
        }
      }
      break;
  }
}

/* ------------------------------------------------ 笔压采样（P1-4） */

using GetPointerTypeFn = BOOL(WINAPI*)(UINT32, POINTER_INPUT_TYPE*);
using GetPointerPenInfoFn = BOOL(WINAPI*)(UINT32, POINTER_PEN_INFO*);

/*
 * WM_POINTER 只用来**读**笔压，随后一律交回 DefWindowProc：系统会把笔/触摸
 * 消息提升成 WM_MOUSEMOVE / WM_LBUTTONDOWN，绘制仍走原来那条鼠标通路。
 * 所以压感是一个纯增量——没有 WM_POINTER、没有 PEN_MASK_PRESSURE 位、远程
 * 会话里全部回落 1.0，像素与 P1-4 之前逐字节相同。
 *
 * 两个 API 都在 user32 里，但只有 Win8+ 才有；与 shell_dpi.cpp 同一做法，
 * 用 GetProcAddress 解析，老系统上是「没有压感」而不是「起不来」。
 */
void pen_sample_pressure(ShellState& shell, UINT32 pointer_id) {
  static GetPointerTypeFn get_type = nullptr;
  static GetPointerPenInfoFn get_pen_info = nullptr;
  static bool resolved = false;
  if (!resolved) {
    resolved = true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
      get_type = reinterpret_cast<GetPointerTypeFn>(GetProcAddress(user32, "GetPointerType"));
      get_pen_info =
          reinterpret_cast<GetPointerPenInfoFn>(GetProcAddress(user32, "GetPointerPenInfo"));
    }
  }
  if (get_type == nullptr || get_pen_info == nullptr) return;
  POINTER_INPUT_TYPE pointer_type = PT_POINTER;
  /* 触摸与鼠标指针同样会发 WM_POINTER；只有笔带压力。 */
  if (!get_type(pointer_id, &pointer_type) || pointer_type != PT_PEN) return;
  POINTER_PEN_INFO info{};
  if (!get_pen_info(pointer_id, &info)) return;
  const bool has_pressure = (info.penMask & PEN_MASK_PRESSURE) != 0;
  shell.pen_pressure = comp_ui::pen_pressure_to_unit(info.pressure, has_pressure);
  shell.pen_pressure_active = has_pressure;
  /* 一笔只有一个 brush.pressure（ABI 是常量），所以整笔取采样平均。 */
  if (shell.doc.stroke_kind != comp_ui::StrokeKind::None) {
    shell.doc.stroke_pressure_sum += shell.pen_pressure;
    ++shell.doc.stroke_pressure_samples;
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  ShellState* shell = g_shell;
  /*
   * ROI #7：鼠标消息里的坐标恒为**设备像素**，而布局与命中全部在逻辑像素里，
   * 在过程入口统一换算一次。WM_MOUSEWHEEL 的 lParam 是屏幕坐标，它单独走
   * device_* + ScreenToClient 再换算，不直接吃这两个值。
   */
  const int device_x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
  const int device_y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
  const int window_dpi = shell != nullptr ? shell_dpi(*shell) : 96;
  const int mouse_x = dpi_logical(device_x, window_dpi);
  const int mouse_y = dpi_logical(device_y, window_dpi);
  switch (message) {
    case WM_CREATE:
      /* 拖放导入：注册后 Windows 才会把 OLE 拖放转成 WM_DROPFILES。 */
      DragAcceptFiles(window, TRUE);
      return 0;
    case WM_DROPFILES:
      /* 导入要求「没有活动事务」（comp_io.h 的前置条件），先收口连续微调。 */
      if (shell != nullptr) nudge_flush(*shell);
      if (shell != nullptr) handle_drop_files(window, *shell, reinterpret_cast<HDROP>(wparam));
      return 0;
    case WM_PAINT:
      if (shell != nullptr) present(window, *shell);
      break;
    case WM_INITMENU:
      /*
       * S24：菜单勾选态在这里刷新。**不重建菜单**——重建会让已打开的 HMENU 悬空，
       * 且每次弹菜单都闪一下；勾选是纯显示状态，按命令 id 递归改标记即可。
       */
      if (shell != nullptr) {
        /* 菜单打开即收口连续微调：勾选/置灰读到的必须是已提交的状态。 */
        nudge_flush(*shell);
        sync_menu_state(window, *shell);
      }
      return 0;
    case WM_KEYDOWN:
      if (shell != nullptr) {
        /*
         * S22 F6：亮度直方图开关。放在字母键处理之前——它与任何工具快捷键都
         * 不冲突，且面板类快捷键统一走 run_shell_command 分发，菜单与键不分叉。
         */
        if (wparam == VK_F6) {
          run_shell_command(window, *shell, kCmdHistogramToggle);
          return 0;
        }
        /*
         * §5.43 F7 / F8：面板整块收起（与 PS 同键位）。同样走 run_shell_command，
         * 菜单、快捷键、点按钮三条入口不分叉。
         */
        if (wparam == VK_F7) {
          run_shell_command(window, *shell, kCmdPanelLayers);
          return 0;
        }
        if (wparam == VK_F8) {
          run_shell_command(window, *shell, kCmdPanelAdjust);
          return 0;
        }
        /*
         * §6.60 Esc 取消面板拖动：回到按下前的宽度（与滑杆/曲线拖动同一手感）。
         * 必须先回滚再 ReleaseCapture——释放捕获会重入 cancel，那时目标已清空即为空操作。
         */
        if (wparam == VK_ESCAPE && shell->panel_resize_target != 0) {
          panel_resize_cancel(*shell);
          ReleaseCapture();
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        /*
         * 文字键入态下空格是字符，不是临时抓手；文档级对话框打开时空格归面板
         * （开关 / 背景 / 锚点都用空格翻），不然这里的临时抓手会先吞掉它。
         */
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool dialog_owns_space = shell->doc.doc_dialog != comp_ui::DocDialogKind::None;
        if (wparam == VK_SPACE && !dialog_owns_space && !(shell->doc.type_active && !ctrl)) {
          /* 空格 = 临时抓手：按下不重复触发，松开才结束。 */
          if (!shell->space_down) {
            shell->space_down = true;
            set_status(*shell, L"按住空格拖动画布（或中键拖动）");
            InvalidateRect(window, nullptr, FALSE);
          }
          break;
        }
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        handle_key(window, *shell, wparam, ctrl, shift);
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_KEYUP:
      if (shell != nullptr && wparam == VK_SPACE) {
        shell->space_down = false;
        end_pan(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
      }
      /*
       * 方向键松开 = 这段连续微调结束（§5.30 补）：整套位移压成一条撤销记录。
       * 用 KEYUP 而不是定时器收口——按键重复本来就是一串 KEYDOWN + 一个 KEYUP，
       * 语义边界与用户「按住」的动作完全对齐，也不需要在空转时跑计时器。
       */
      if (shell != nullptr &&
          (wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP || wparam == VK_DOWN)) {
        if (nudge_flush(*shell)) {
          refresh_history_status(*shell, shell->status.c_str());
          InvalidateRect(window, nullptr, FALSE);
        }
      }
      break;
    /*
     * P0-1：图层名 / 不透明度键入的字符入口。
     *
     * 用 WM_CHAR 而不是在 WM_KEYDOWN 里按 VK 转字符：前者是 TranslateMessage
     * 与 IME 的共同出口，CJK 才能原样进来（WM_CHAR 的 wParam 就是 UTF-16 码元）。
     * 编辑键（回车/退格/Esc）仍由 WM_KEYDOWN 处理，这里把控制字符挡掉。
     */
    case WM_CHAR:
      if (shell != nullptr && shell->doc.layer_input != LayerInput::None) {
        panel_layer_input_char(*shell, static_cast<wchar_t>(wparam));
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * ROI #5：通道命名。与图层改名同一条出口（WM_CHAR / 输入法），中文通道名
       * 能直接进来。命名态在 KEYDOWN 里已把键盘收归面板，这里只管字符。
       */
      if (shell != nullptr && shell->doc.channel_name_active) {
        channel_name_char(*shell, static_cast<wchar_t>(wparam));
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * 字体面板（P0-6②）：WM_CHAR 是键入与输入法的共同出口，中文字体名也能
       * 直接过滤。排在 type_active 之前——KEYDOWN 已把面板打开时的键归面板，
       * 这里若先喂给文字缓冲，会变成「插入点在闪、过滤词不动」。
       */
      if (shell != nullptr && font_panel_visible(*shell)) {
        font_filter_char(*shell, static_cast<wchar_t>(wparam));
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * 文字工具（P5-3）同一个出口：WM_CHAR / WM_IME_CHAR 送来的码元进 UTF-8
       * 缓冲。落点后键盘是模态的，字符不再兼作工具快捷键（快捷键在 KEYDOWN
       * 里已经处理过的会置 type_skip_char，这里跳过它对应的那个字符）。
       */
      if (shell != nullptr && shell->doc.type_active) {
        if (type_char_input(*shell, static_cast<wchar_t>(wparam))) {
          type_refresh_status(*shell);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    /*
     * P1-4 笔压：只采样，不拦截派发——break 之后统一走 DefWindowProc，由系统
     * 继续把笔消息提升成鼠标消息（绘制仍只有一条通路，压感不会另开一套）。
     */
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
      if (shell != nullptr) pen_sample_pressure(*shell, GET_POINTERID_WPARAM(wparam));
      break;
    case WM_LBUTTONDOWN:
      if (shell != nullptr) {
        /* 鼠标一按就结束连续微调：拖拽/画笔/面板行都会自己开事务，不能撞在活动事务上。 */
        nudge_flush(*shell);
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          /*
           * ROI #5：通道命名态下点到别处 = 放弃这次命名（不保存），点击照常继续。
           * 通道面板内的点击由下面的通道分支处理（它先取消命名再选中行），这里
           * 只负责面板以外的位置——否则用户点工具按钮/画布时名字会一直挂着。
           */
          if (shell->doc.channel_name_active &&
              !(channel_panel_visible(*shell) &&
                layout.adjust_panel.contains(mouse_x, mouse_y))) {
            channel_name_cancel(*shell);
            InvalidateRect(window, nullptr, FALSE);
          }
          const int button =
              comp_ui::tool_button_at(layout.toolbar, comp_ui::tool_count(), mouse_x, mouse_y);
          if (button >= 0) {
            const comp_ui::ToolInfo* info = comp_ui::tool_info_at(button);
            if (info != nullptr) set_tool(*shell, info->id);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /*
           * §5.49 选项栏：排在工具按钮之后、面板开关之前。它在两者之间的一条
           * 独立横栏，命中后必须吃掉这次点击——否则会继续落到画布上落下第一笔。
           */
          if (layout.options.h > 0) {
            int delta = 0;
            const int option =
                comp_ui::option_hit(layout.options, shell->tool, mouse_x, mouse_y, &delta);
            if (option >= 0) {
              const comp_ui::ToolOptionSpec spec = comp_ui::tool_option(shell->tool, option);
              if (apply_tool_option(*shell, spec.id, delta)) {
                wchar_t line[128];
                wsprintfW(line, L"%s：%s", widen_ascii(spec.label).c_str(),
                          option_value_text(*shell, spec).c_str());
                shell->status = line;
              }
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          /*
           * §5.43 收起/展开按钮：先于分界线与面板行。展开态它与分界线留 3px
           * 间隔（互不重叠），收起态整条 24px 都是按钮——顺序必须在这里，否则
           * 点竖条会先落到「面板行」/滑杆上。
           */
          const int toggle = comp_ui::panel_toggle_at(layout, mouse_x, mouse_y);
          if (toggle != 0) {
            panel_toggle_collapsed(*shell, toggle);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /*
           * §6.60：面板分界线必须先于面板行命中——它是面板内侧 5px 的窄带，
           * 落在图层行/滑杆行里，顺序反了永远点不到（只会选行）。
           */
          const int splitter = comp_ui::panel_splitter_at(layout, mouse_x, mouse_y);
          if (splitter != 0) {
            panel_resize_begin(window, *shell, splitter, mouse_x);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /*
           * 命中图层行：row_top 与 draw_layer_panel 共用同一个直方图偏移量。
           * 两处各写一个常量迟早不一致，而症状是「点不中要的那层」——不报错、
           * 只在面板开着直方图时偶现，极难复现。
           */
          /*
           * S24 直方图交互：绘图区按下 = 开始框选（松开超过 4px 才真的缩放）；
           * 标签行按下 = 复位全幅。放在图层行命中之前，两块几何互不重叠，但顺序
           * 固定后「图里拖」与「点图层行」不会互相吃掉点击。
           */
          if (shell->doc.histogram_open && !shell->doc.histogram_bins.empty()) {
            const comp_ui::Rect plot = comp_ui::histogram_plot_rect(layout.layer_panel);
            if (plot.contains(mouse_x, mouse_y)) {
              shell->doc.histogram_drag = true;
              shell->doc.histogram_drag_from_x = mouse_x;
              shell->doc.histogram_drag_to_x = mouse_x;
              SetCapture(window);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
            const comp_ui::Rect histogram_label =
                comp_ui::histogram_label_rect(layout.layer_panel);
            if (histogram_label.contains(mouse_x, mouse_y)) {
              panel_histogram_reset(*shell);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          const int row_top = comp_ui::layer_rows_top(shell->doc.histogram_open);
          /* S44：行数用可见行数（折叠后子层不占行），行号经 display 换算成图层。 */
          const int row = comp_ui::layer_row_at(layout.layer_panel, panel_row_count(*shell),
                                                mouse_x, mouse_y, row_top);
          if (row >= 0) {
            const LayerRow* clicked_layer = layer_row_at(*shell, row);
            if (clicked_layer != nullptr && clicked_layer->is_group != 0) {
              /*
               * S44 组折叠：三角落在行首的固定槽位里，只对组行生效，且必须先于
               * 行选中与双击改名——否则点三角会顺手把该组选上/改起名来。
               */
              const comp_ui::Rect row_rect = comp_ui::layer_row_rect(
                  layout.layer_panel, row, row_top);
              const int depth = row < static_cast<int>(shell->doc.layer_depth.size())
                                    ? shell->doc.layer_depth[static_cast<size_t>(row)]
                                    : 0;
              const comp_ui::Rect triangle = comp_ui::layer_triangle_rect(row_rect, depth);
              if (triangle.contains(mouse_x, mouse_y)) {
                panel_toggle_group_collapsed(*shell, clicked_layer->id);
                refresh_effects(*shell);
                /* 组内层数取全量数组（折叠后子层不在可见行里，数不到）。 */
                int32_t children = 0;
                for (const LayerRow& layer : shell->doc.layers) {
                  if (layer.parent == clicked_layer->id) ++children;
                }
                wchar_t buffer[192];
                wsprintfW(buffer, L"%s组 #%I64u（%d 层）",
                          layer_group_collapsed(*shell, clicked_layer->id) ? L"已折叠 "
                                                                          : L"已展开 ",
                          static_cast<unsigned long long>(clicked_layer->id), children);
                set_status(*shell, buffer);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
              }
            }
            const bool click_ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool click_shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const comp_layer_id clicked_id = clicked_layer != nullptr ? clicked_layer->id : 0;
            /*
             * P0-1 多选（对齐上游图层面板）：
             *   Ctrl+点 = 切换该层是否在多选集合里；点主选中层则把它从选中里摘掉
             *             （若还有其它多选层，用第一个顶上做主选中）。
             *   Shift+点 = 从主选中行到该行整段成为多选。
             * 两种修饰下都不进入拖行 pending：多选拖拽需要「整组一起挪」的语义，
             * 而当前拖行只搬一层，按下就先别埋一个会误触的 pending。
             */
            if (click_ctrl) {
              if (panel_selection_contains(*shell, clicked_id)) {
                if (row == shell->doc.selected_layer) {
                  if (!shell->doc.extra_selection.empty()) {
                    const comp_layer_id promoted = shell->doc.extra_selection.front();
                    shell->doc.extra_selection.erase(shell->doc.extra_selection.begin());
                    /* S44：行号随折叠变化，按 id 反查可见行，而不是数组逆序推算。 */
                    panel_select_layer_id(*shell, promoted);
                  }
                } else {
                  auto& extras = shell->doc.extra_selection;
                  for (size_t i = 0; i < extras.size(); ++i) {
                    if (extras[i] == clicked_id) {
                      extras.erase(extras.begin() + static_cast<std::ptrdiff_t>(i));
                      break;
                    }
                  }
                }
              } else {
                shell->doc.extra_selection.push_back(clicked_id);
              }
            } else if (click_shift && shell->doc.selected_layer >= 0) {
              const int32_t from = shell->doc.selected_layer < row ? shell->doc.selected_layer : row;
              const int32_t to = shell->doc.selected_layer < row ? row : shell->doc.selected_layer;
              shell->doc.extra_selection.clear();
              for (int32_t line = from; line <= to; ++line) {
                const LayerRow* line_layer = layer_row_at(*shell, line);
                if (line_layer != nullptr && line_layer->id != clicked_id) {
                  shell->doc.extra_selection.push_back(line_layer->id);
                }
              }
              shell->doc.selected_layer = row;
            } else {
              shell->doc.extra_selection.clear();
              shell->doc.selected_layer = row;
              /*
               * S24 拖行重排：按下即 pending（记录起点行与 y），移动超过阈值才进入
               * active。单击必须还是「选中」，手抖 1px 不该把层挪走；捕获在
               * WM_LBUTTONUP / WM_CAPTURECHANGED 里释放。
               */
              shell->doc.layer_drag_row = row;
              shell->doc.layer_drag_target = row;
              shell->doc.layer_drag_press_y = mouse_y;
              shell->doc.layer_drag_pending = true;
              shell->doc.layer_drag_active = false;
              SetCapture(window);
            }
            refresh_adjustments(*shell);
            /* S24：效果列表同样跟随选中图层，否则菜单勾选与 fx 标记会停在上一层。 */
            refresh_effects(*shell);
            /* P1-2 收尾 ⑧：填充面板也跟随选中图层（绑定层或退回新建态）。 */
            refresh_fill(*shell);
            const LayerRow* selected = selected_row(*shell);
            if (selected != nullptr) {
              wchar_t buffer[160];
              if (panel_selection_ids(*shell, false).size() > 1) {
                wsprintfW(buffer, L"已选中 %d 个图层（主选中 #%I64u，第 %d 行；Ctrl+J/Del 批量）",
                          static_cast<int>(panel_selection_ids(*shell, false).size()),
                          static_cast<unsigned long long>(selected->id), row + 1);
              } else {
                wsprintfW(buffer, L"已选中图层 #%I64u（第 %d 行）",
                          static_cast<unsigned long long>(selected->id), row + 1);
              }
              set_status(*shell, buffer);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->doc.doc_dialog != comp_ui::DocDialogKind::None && layout.adjust_panel.w > 0) {
            /* 点行 = 聚焦到该行；行几何与绘制共用 doc_dialog_row_rect。 */
            const int row = comp_ui::doc_dialog_row_at(
                layout.adjust_panel, comp_ui::doc_dialog_row_count(shell->doc.doc_dialog),
                mouse_x, mouse_y);
            if (row >= 0) {
              shell->doc.doc_dialog_input.clear();
              doc_dialog_focus_row(*shell, row - shell->doc.doc_dialog_focus);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->doc.effect_open && layout.adjust_panel.w > 0) {
            /* 点参数行 = 聚焦到该行；行几何与绘制共用 effect_param_row_rect。 */
            const int row = comp_ui::effect_param_row_at(
                layout.adjust_panel, static_cast<int>(shell->doc.effect_values.size()), mouse_x,
                mouse_y);
            if (row >= 0) {
              panel_effect_focus_row(*shell, row);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->doc.trim_open && layout.adjust_panel.w > 0) {
            /* 点行 = 聚焦到该行；行几何与绘制共用 trim_row_rect。 */
            const int row = comp_ui::trim_row_at(layout.adjust_panel, shell->doc.trim.row_count(),
                                                 mouse_x, mouse_y);
            if (row >= 0) {
              shell->doc.trim_input.clear();
              shell->doc.trim.focused = row;
              wchar_t buffer[192];
              wsprintfW(buffer, L"修边 %d/%d：%s", row + 1, shell->doc.trim.row_count(),
                        widen_ascii(comp_ui::trim_row_label(row)).c_str());
              set_status(*shell, buffer);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->doc.filter_open && layout.adjust_panel.w > 0) {
            /* 点参数行 = 聚焦到该行；行几何与绘制共用 filter_param_row_rect。 */
            const int row = comp_ui::filter_param_row_at(
                layout.adjust_panel, shell->doc.filter.param_count(), mouse_x, mouse_y);
            if (row >= 0) {
              shell->doc.filter_input.clear();
              filter_focus_row(*shell, row - shell->doc.filter.focused);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->doc.fill_open && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 填充参数行（P1-2 收尾 ⑧）：上半行 = 聚焦，下半行（滑道）= 聚焦 + 拖动。
             * 行/滑道几何与 draw_fill_panel 同源（ui_core::adjust_param_row_rect /
             * adjust_slider_track_rect）。布尔没有滑道，点一下就切换。
             * 面板里点空白也吞掉：面板开着时右栏没有第二个来源。
             */
            const int32_t total = static_cast<int32_t>(shell->doc.fill_values.size());
            const int32_t scroll = comp_ui::adjust_param_clamp_scroll(
                layout.adjust_panel, total, shell->doc.fill_scroll);
            shell->doc.fill_scroll = scroll;
            const int row = comp_ui::adjust_param_row_at(layout.adjust_panel, total, scroll,
                                                        mouse_x, mouse_y);
            if (row >= 0) {
              comp_fill_param param{};
              if (fill_param_query(*shell, row, &param)) {
                shell->doc.fill_input.clear();
                if (param.type == COMP_PARAM_BOOL) {
                  fill_panel_focus_row(*shell, row);
                  fill_panel_step_value(*shell, 1);
                  /* 已绑定则一次写入（一步撤销）；新建态只改缓冲，回车建层。 */
                  if (shell->doc.fill_layer != 0) panel_apply_fill(*shell);
                } else if (comp_ui::adjust_row_track_hit(layout.adjust_panel, row, scroll,
                                                         mouse_x, mouse_y)) {
                  fill_panel_begin_drag(*shell, layout.adjust_panel, row,
                                        static_cast<double>(mouse_x));
                  if (shell->doc.fill_drag_param >= 0) SetCapture(window);
                } else {
                  fill_panel_focus_row(*shell, row);
                }
              }
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->doc.smart_open && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
          if (type_panel_visible(*shell) && layout.adjust_panel.w > 0) {
            /* 点属性行 = 聚焦到该行；行几何与绘制共用 type_param_row_rect。 */
            const int row =
                comp_ui::type_param_row_at(layout.adjust_panel, mouse_x, mouse_y);
            if (row >= 0) {
              shell->doc.type_input.clear();
              type_panel_focus(*shell, row - shell->type_panel_focused);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (font_panel_visible(*shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 字体面板：单击 = 选中并应用（改轨道 + 若是文字图层则重排一次，一步撤销）。
             * 空白处也吞掉，别漏给调整面板——面板开着时右栏没有第二个来源。
             */
            const int count = font_row_count(*shell);
            const int scroll =
                comp_ui::font_clamp_scroll(layout.adjust_panel, count, shell->font_scroll);
            const int row =
                comp_ui::font_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (row >= 0) font_apply_row(*shell, row);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (!shell->doc.effect_open && !shell->doc.filter_open && !shell->doc.trim_open &&
              !shell->doc.fill_open && !shell->doc.smart_open &&
              !type_panel_visible(*shell) &&
              !font_panel_visible(*shell) &&
              shell->doc.doc_dialog == comp_ui::DocDialogKind::None &&
              !shell->doc.color_range_open && !path_panel_visible(*shell) &&
              !history_panel_visible(*shell) && !channel_panel_visible(*shell) &&
              layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * P0-3 调整面板：上半行 = 聚焦，下半行（滑道）= 聚焦 + 开始拖动。
             * 行/滑道几何全部来自 ui_core::adjust_param_row_rect /
             * adjust_slider_track_rect，与 draw_adjust_panel 同源。
             * 分页视图（色阶）里几何用「相对行」（row - base），数据用绝对下标。
             */
            const int32_t shown_total = adjust_shown_count(*shell);
            const int32_t base = adjust_param_base(*shell);
            const int32_t scroll = comp_ui::adjust_param_clamp_scroll(
                layout.adjust_panel, shown_total, shell->doc.adj_scroll);
            shell->doc.adj_scroll = scroll;

            /* 色阶通道条：先于参数行命中（它在标题行，行命中吃不到这里）。 */
            if (adjust_paged_kind(static_cast<uint32_t>(shell->doc.adj_kind))) {
              const int32_t chip = comp_ui::adjust_channel_chip_at(
                  layout.adjust_panel, kLevelsChannelCount, mouse_x, mouse_y);
              if (chip >= 0) {
                adjust_set_channel(*shell, chip);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
              }
            }

            /* 曲线：通道条 -> 绘图区（命中控制点 = 拖动，空白 = 加点后拖动）。 */
            if (shell->doc.adj_kind == COMP_ADJUSTMENT_CURVES) {
              const int32_t chip = comp_ui::curve_channel_chip_at(
                  layout.adjust_panel, kCurveChannels, mouse_x, mouse_y);
              if (chip >= 0) {
                curve_set_channel(*shell, chip);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
              }
              const comp_ui::Rect plot = comp_ui::adjust_curve_plot_rect(layout.adjust_panel);
              if (!plot.empty() && plot.contains(mouse_x, mouse_y)) {
                const int32_t count = static_cast<int32_t>(shell->doc.curve_xy.size() / 2);
                const int32_t hit =
                    count >= 2 ? comp_ui::curve_point_hit(
                                     plot, shell->doc.curve_xy.data(), static_cast<int>(count),
                                     mouse_x, mouse_y, 7.0)
                               : -1;
                if (hit >= 0) {
                  curve_begin_drag(*shell, plot, hit, mouse_x, mouse_y);
                  if (shell->doc.curve_drag_point >= 0) SetCapture(window);
                } else {
                  curve_add_point(*shell, plot, mouse_x, mouse_y);
                  if (shell->doc.curve_drag_point >= 0) SetCapture(window);
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
              }
            }

            const int row = comp_ui::adjust_param_row_at(layout.adjust_panel, shown_total,
                                                         scroll, mouse_x, mouse_y);
            if (row >= 0) {
              const int32_t index = base + row;
              shell->doc.adj_param = index;
              shell->doc.adj_input.clear();
              if (comp_ui::adjust_row_track_hit(layout.adjust_panel, row, scroll, mouse_x,
                                                mouse_y)) {
                comp_adjustment_param param{};
                if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell->doc.adj_kind),
                                                   static_cast<size_t>(index),
                                                   &param) == COMP_OK) {
                  if (param.type == COMP_PARAM_BOOL) {
                    /* 布尔参数没有滑道：点一下就是切换。 */
                    panel_step_value(*shell, 1);
                  } else {
                    adjust_begin_drag(*shell, layout.adjust_panel, index, mouse_x);
                    SetCapture(window);
                  }
                }
              } else {
                wchar_t buffer[220];
                comp_adjustment_param param{};
                if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell->doc.adj_kind),
                                                   static_cast<size_t>(index),
                                                   &param) == COMP_OK) {
                  wsprintfW(buffer, L"%s = %s（范围 [%s, %s]；拖动滑杆实时预览）",
                            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
                            widen_ascii(shell->doc.adj_values[static_cast<size_t>(index)].c_str())
                                .c_str(),
                            widen_ascii(number_text(param.min_value).c_str()).c_str(),
                            widen_ascii(number_text(param.max_value).c_str()).c_str());
                  set_status(*shell, buffer);
                }
              }
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->doc.color_range_open && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 色彩范围面板：点色块 = 删掉该取样色；点「反相」行 = 切换；
             * 点「容差」行 = 提示键入（数字键在任何位置都能起键入态）。
             */
            const int slot = comp_ui::color_range_swatch_at(
                layout.adjust_panel,
                static_cast<int>(shell->doc.color_range_include.size() / 3),
                static_cast<int>(shell->doc.color_range_exclude.size() / 3), mouse_x, mouse_y);
            if (slot >= 0) {
              color_range_remove_swatch(*shell, slot);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
            const int row = comp_ui::color_range_row_at(layout.adjust_panel, mouse_x, mouse_y);
            if (row == 1) {
              color_range_toggle_invert(*shell);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
            if (row == 0) {
              set_status(*shell,
                         L"色彩范围：- / = 步进容差，或直接按数字键入（回车落值）");
              return 0;
            }
          }
          if (path_panel_visible(*shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 路径面板：点行 = 选中该条；转选区走双击（WM_LBUTTONDBLCLK）或回车，
             * 单击不落动作——避免误点把用户已有选区顶掉。空白处也吞掉，别漏给调整面板。
             */
            const int count = static_cast<int>(shell->doc.paths.size());
            const int scroll =
                comp_ui::path_clamp_scroll(layout.adjust_panel, count, shell->doc.path_scroll);
            const int index =
                comp_ui::path_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (index >= 0) {
              shell->doc.path_selected = index;
              path_status(*shell);
              InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
          }
          if (history_panel_visible(*shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 历史记录面板：点行 = 直接回跳到该状态（单击即落动作，与路径面板不同——
             * 回跳本身可再回跳，不产生历史记录，误点没有破坏性）。空白处吞掉。
             * Shift+点行 = 只把它指定给历史记录画笔当取样状态，**不回跳**：
             * 取样源必须能停在「过去某一格」而当前画面保持在最新，否则这支笔
             * 就没有意义了。
             */
            const int count = history_state_count(*shell);
            const int scroll =
                comp_ui::history_clamp_scroll(layout.adjust_panel, count, shell->doc.history_scroll);
            const int index =
                comp_ui::history_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (index >= 0) {
              if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                shell->doc.history_brush_state = index;
                wchar_t buffer[288];
                wsprintfW(buffer, L"历史记录画笔取样：第 %d 个状态（%s）· 当前画面未回跳",
                          index, history_state_label(*shell, index).c_str());
                set_status(*shell, buffer);
              } else {
                history_jump_to(*shell, index);
              }
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (channel_panel_visible(*shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * ROI #5 通道面板：点行 = 选中该条；载入走双击或回车，单击不落动作
             * （与路径面板同规矩——载入会顶掉用户现有选区，误点代价太大）。
             * 命名态下点面板 = 先取消命名（不保存），再按行选中。
             */
            if (shell->doc.channel_name_active) channel_name_cancel(*shell);
            const int count = channel_count(*shell);
            const int scroll = comp_ui::channel_clamp_scroll(layout.adjust_panel, count,
                                                             shell->doc.channel_scroll);
            const int index =
                comp_ui::channel_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (index >= 0) {
              shell->doc.channel_selected = index;
              channel_status(*shell);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->doc.color_range_open && layout.canvas.contains(mouse_x, mouse_y)) {
            /* 面板打开时画布点击只取样，不交给当前工具（上游 sheet 同语义）。 */
            color_range_sample(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->doc.filter_open && layout.canvas.contains(mouse_x, mouse_y) &&
              !progressive_axis_available(*shell)) {
            /*
             * §6.62 滤镜面板是模态的（与色彩范围同规矩）：预览帧只在画面里，文档还是
             * 干净状态，这时让画笔/选框落笔，等于在「看不见的那份文档」上画——预览会
             * 被 needs_render 冲掉，用户以为改的是他看到的那张图。
             * 唯一例外是 Progressive Blur 的轴拖拽（它本来就要画布坐标）。
             */
            set_status(*shell, L"滤镜面板打开中：画布点击不作用于工具（Esc 关闭面板）");
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (layout.canvas.contains(mouse_x, mouse_y)) {
            /*
             * ROI #3：从标尺拖出参考线 / Move 工具拖动已有参考线。必须排在工具
             * 状态机最前面——标尺条与参考线常驻可见，落到它们身上的按下如果先
             * 交给画笔，就会在画布上落一笔再把线拖走。
             */
            if (guide_drag_begin(window, *shell, mouse_x, mouse_y, layout)) {
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
            if (shell->tool == comp_ui::ToolId::Hand || shell->space_down) {
              begin_pan(window, *shell, mouse_x, mouse_y);
            } else if (zoom_tool_available(*shell)) {
              /* Z：单击放大 / Alt+单击缩小；拖出矩形 = 缩放到该区域（松手判定）。 */
              zoom_marquee_begin(window, *shell, mouse_x, mouse_y);
            } else if (progressive_axis_available(*shell)) {
              /* 滤镜面板指着 Progressive Blur：画布拖拽只定渐变轴（不画图）。 */
              progressive_axis_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (selection_handle_begin(window, *shell, mouse_x, mouse_y, layout)) {
              /*
               * §5.44 按在选区手柄上 = 扩缩选区。必须排在框选/套索/魔棒/移动之前：
               * 否则「按在把手上」会变成「重新框一个选区」或「把像素搬走」。
               * 返回 true 表示「这次点击归手柄」：调用链到此为止，不再退化成
               * 框选/套索/魔棒/整层拖动。
               */
            } else if (crop_available(*shell)) {
              /* 已有框时，再按一次画布 = 重新起框（对齐 PS 的「点框外取消、框内重来」语义） */
              crop_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (pen_available(*shell)) {
              pen_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (marquee_available(*shell)) {
              marquee_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (lasso_available(*shell)) {
              lasso_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (wand_available(*shell)) {
              wand_click(window, *shell, mouse_x, mouse_y, layout);
            } else if (gradient_available(*shell)) {
              gradient_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (shape_available(*shell)) {
              shape_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (type_available(*shell)) {
              type_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (history_brush_stroke_available(*shell)) {
              history_brush_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (tone_stroke_available(*shell)) {
              tone_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (retouch_stroke_available(*shell)) {
              retouch_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (paint_stroke_available(*shell)) {
              paint_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (heal_stroke_available(*shell)) {
              heal_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (clone_stroke_available(*shell) &&
                       (GetKeyState(VK_MENU) & 0x8000) != 0) {
              /* Alt+点击 = 落源点，不落笔（Shift 同时按住则整篇合成取样）。 */
              clone_pick_source(*shell, mouse_x, mouse_y, layout);
            } else if (clone_stroke_available(*shell)) {
              clone_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (eyedropper_available(*shell)) {
              eyedropper_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (move_available(*shell)) {
              move_begin(window, *shell, mouse_x, mouse_y, layout);
            } else {
              canvas_click(*shell, mouse_x, mouse_y, layout);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
      }
      break;
    case WM_LBUTTONDBLCLK:
      /*
       * S24：双击直方图绘图区 = 区间复位（与 PS 直方图一致）。
       * 其它位置把双击还原成一次普通按下，画笔/选框的连点行为不受 CS_DBLCLKS 影响
       * ——否则第二次点击会被 DBLCLK 吃掉，画笔连点就少一笔。
       */
      if (shell != nullptr) {
        nudge_flush(*shell);
        Layout layout{};
        /*
         * §6.60 双击分界线 = 复位该面板宽度。必须先于「还原成普通按下」，
         * 也必须先于直方图/路径/图层行：分界线窄带压在这些控件之上。
         */
        if (client_layout(*shell, &layout)) {
          /*
           * §5.43 双击收起/展开按钮 = 与单击同义（第二击不吞掉：收起后整条都是
           * 按钮，双击就是「收起再展开」，与连点两次完全一致）。
           */
          const int toggle = comp_ui::panel_toggle_at(layout, mouse_x, mouse_y);
          if (toggle != 0) {
            panel_toggle_collapsed(*shell, toggle);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          const int splitter = comp_ui::panel_splitter_at(layout, mouse_x, mouse_y);
          if (splitter != 0) {
            panel_resize_reset(*shell, splitter);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
        if (client_layout(*shell, &layout) && shell->doc.histogram_open &&
            !shell->doc.histogram_bins.empty() &&
            comp_ui::histogram_plot_rect(layout.layer_panel).contains(mouse_x, mouse_y)) {
          panel_histogram_reset(*shell);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        /*
         * P0-4 可选：双击路径行 = 该路径转成选区。方向键 + 回车是等价入口；
         * 单击只选中（见 WM_LBUTTONDOWN），所以双击不会先被「选中」吃掉。
         */
        if (client_layout(*shell, &layout) && path_panel_visible(*shell) &&
            layout.adjust_panel.w > 0) {
          const int count = static_cast<int>(shell->doc.paths.size());
          const int scroll =
              comp_ui::path_clamp_scroll(layout.adjust_panel, count, shell->doc.path_scroll);
          const int index =
              comp_ui::path_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
          if (index >= 0) {
            shell->doc.path_selected = index;
            path_apply_selected(*shell);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
        /* ROI #5：双击通道行 = 把它载入选区（与路径面板同一入口规矩）。 */
        if (client_layout(*shell, &layout) && channel_panel_visible(*shell) &&
            layout.adjust_panel.w > 0) {
          const int count = channel_count(*shell);
          const int scroll = comp_ui::channel_clamp_scroll(layout.adjust_panel, count,
                                                           shell->doc.channel_scroll);
          const int index =
              comp_ui::channel_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
          if (index >= 0) {
            shell->doc.channel_name_active = false;
            shell->doc.channel_name_skip_char = false;
            shell->doc.channel_name_input.clear();
            shell->doc.channel_selected = index;
            channel_load_selected(*shell, COMP_SELECTION_REPLACE);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
        /*
         * P0-1：双击图层行 = 重命名（与上游图层面板一致）。
         * 必须先于「还原成普通按下」——否则第二次点击只会重选一次，双击永远
         * 进不了改名态。
         */
        if (client_layout(*shell, &layout) && layout.layer_panel.contains(mouse_x, mouse_y)) {
          const int row = comp_ui::layer_row_at(layout.layer_panel, panel_row_count(*shell),
                                                mouse_x, mouse_y,
                                                comp_ui::layer_rows_top(shell->doc.histogram_open));
          if (row >= 0) {
            /*
             * S44：双击三角槽位只是「折叠再折叠」的连击，不能顺手进改名——
             * 否则用户想展开组，却先弹出组名编辑框。
             */
            const LayerRow* row_layer = layer_row_at(*shell, row);
            const comp_ui::Rect triangle = comp_ui::layer_triangle_rect(
                comp_ui::layer_row_rect(layout.layer_panel, row,
                                        comp_ui::layer_rows_top(shell->doc.histogram_open)),
                row < static_cast<int>(shell->doc.layer_depth.size())
                    ? shell->doc.layer_depth[static_cast<size_t>(row)]
                    : 0);
            if (row_layer != nullptr && row_layer->is_group != 0 &&
                triangle.contains(mouse_x, mouse_y)) {
              return 0;
            }
            shell->doc.extra_selection.clear();
            shell->doc.selected_layer = row;
            refresh_adjustments(*shell);
            refresh_effects(*shell);
            refresh_fill(*shell);
            panel_begin_rename(*shell);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
        /*
         * 缩放工具双击画布 = 100%（实际像素），与 PS 一致。放在「还原成普通
         * 按下」之前：双击不再额外放大 1.2 倍，只落到实际像素。
         */
        if (client_layout(*shell, &layout) && shell->doc.document != nullptr &&
            shell->tool == comp_ui::ToolId::Zoom && layout.canvas.contains(mouse_x, mouse_y)) {
          shell->doc.view.actual_pixels();
          shell->doc.needs_fit = false;
          set_status(*shell, L"缩放 100%（实际像素）：双击画布；Ctrl+0 适合窗口");
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      return SendMessageW(window, WM_LBUTTONDOWN, wparam, lparam);
    case WM_MBUTTONDOWN:
      if (shell != nullptr) {
        nudge_flush(*shell);
        Layout layout{};
        if (client_layout(*shell, &layout) && layout.canvas.contains(mouse_x, mouse_y)) {
          begin_pan(window, *shell, mouse_x, mouse_y);
          return 0;
        }
      }
      break;
    case WM_LBUTTONUP:
      /* ROI #3 参考线松手：它持有捕获且开着事务，必须最先收尾。 */
      if (shell != nullptr && shell->doc.guide_drag_active) {
        guide_drag_end(window, *shell, mouse_x, mouse_y);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /* §6.60 面板分界线松手：宽度已在拖动中实时生效，这里只收尾捕获与状态栏。 */
      if (shell != nullptr && shell->panel_resize_target != 0) {
        panel_resize_end(*shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /* 缩放拖框：单击/拖框在松手时区分，必须最先结算（它持有鼠标捕获）。 */
      if (shell != nullptr && shell->doc.zoom_marquee_active) {
        zoom_marquee_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /* P0-3 曲线松手：提交事务 = 一次拖动只占一步撤销。 */
      if (shell != nullptr && shell->doc.curve_drag_point >= 0) {
        curve_drag_finish(*shell, true);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /* P0-3 滑杆松手：提交事务 = 一次拖动只占一步撤销。 */
      if (shell != nullptr && shell->doc.adj_drag_param >= 0) {
        adjust_drag_finish(*shell, true);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /* P1-2 收尾 ⑧ 填充滑杆松手：有实际写入才提交，空拖动不占撤销步。 */
      if (shell != nullptr && shell->doc.fill_drag_param >= 0) {
        fill_panel_drag_finish(*shell, true);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * S24 直方图框选收尾：位移 < 4px 视为单击（不缩放）；否则把拖出的亮度区间
       * 设为新的可视窗口。夹取/排序全在 ui_core，这里只递交两个屏幕 x。
       */
      if (shell != nullptr && shell->doc.histogram_drag) {
        shell->doc.histogram_drag = false;
        shell->doc.histogram_drag_to_x = mouse_x;
        ReleaseCapture();
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          panel_histogram_commit_drag(*shell, comp_ui::histogram_plot_rect(layout.layer_panel));
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * S24 拖行重排收尾。pending 但未 active = 单纯点选，什么都不做（不能把
       * 「点一下图层」变成一次撤销步）；active 才提交，跨组由纯函数拒绝。
       */
      if (shell != nullptr && shell->doc.layer_drag_pending) {
        const bool dragged = shell->doc.layer_drag_active;
        const int32_t from_row = shell->doc.layer_drag_row;
        const int32_t to_row = shell->doc.layer_drag_target;
        bool drop_into_group = false;
        if (dragged && to_row >= 0) {
          Layout layout{};
          /* S44：行号是可见行，目标图层经树形 display 反查。 */
          const int32_t rows = panel_row_count(*shell);
          const LayerRow* target = layer_row_at(*shell, to_row);
          if (client_layout(*shell, &layout) && target != nullptr) {
            /* 只有组行、且指针停在行中间带时才解释成「拖进组」。 */
            drop_into_group =
                target->is_group != 0 &&
                comp_ui::layer_drag_inner_band(layout.layer_panel, rows, to_row,
                                               static_cast<double>(mouse_y),
                                               comp_ui::layer_rows_top(shell->doc.histogram_open));
          }
        }
        shell->doc.layer_drag_pending = false;
        shell->doc.layer_drag_active = false;
        shell->doc.layer_drag_row = -1;
        shell->doc.layer_drag_target = -1;
        ReleaseCapture();
        if (dragged && shell->doc.document != nullptr) {
          panel_drop_layer(*shell, from_row, to_row, drop_into_group);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.crop_active) {
        /*
         * 框在拖拽中已实时成型（不像画笔要松手才提交）。松手这一刻以**松手坐标**
         * 再规范化一次，然后立刻停止接收移动：脚本注入（PostMessageW）下松手后
         * 常有一条迟到的 WM_MOUSEMOVE，不能让它改掉待提交的区域。
         */
        Layout layout{};
        if (shell->doc.crop_dragging && client_layout(*shell, &layout)) {
          crop_extend(*shell, mouse_x, mouse_y, layout);
        }
        shell->doc.crop_dragging = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.pen_dragging) {
        pen_up(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.progressive_axis_dragging) {
        progressive_axis_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.gradient_active) {
        gradient_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.shape_active) {
        shape_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.marquee_active) {
        marquee_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.selection_handle_drag >= 0) {
        /* §5.44 松手才写引擎：整次拖动 = 一步撤销（不出手的拖动不产生撤销步）。 */
        selection_handle_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.lasso_active) {
        lasso_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && (shell->doc.move_active || shell->doc.layer_move_active ||
                               shell->doc.layer_handle_active)) {
        move_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.eyedropper_drag) {
        shell->doc.eyedropper_drag = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.stroke_kind == comp_ui::StrokeKind::HistoryBrush) {
        history_brush_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.stroke_kind == comp_ui::StrokeKind::Tone) {
        tone_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.stroke_kind == comp_ui::StrokeKind::Retouch) {
        retouch_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.stroke_kind == comp_ui::StrokeKind::Heal) {
        heal_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.stroke_kind == comp_ui::StrokeKind::Clone) {
        clone_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->doc.stroke_kind == comp_ui::StrokeKind::Paint) {
        paint_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr) end_pan(window, *shell);
      break;
    case WM_MBUTTONUP:
      if (shell != nullptr) end_pan(window, *shell);
      break;
    case WM_KILLFOCUS:
      /* 失焦后收不到 KEYUP / MOUSEUP：必须主动清状态，否则会「粘住」。 */
      if (shell != nullptr) {
        /* 先收口连续微调：下面各 cancel 会 abort 自己的拖动事务，不能撞上活动事务。 */
        nudge_flush(*shell);
        shell->space_down = false;
        /* S24：拖行/框选是「按住鼠标」的临时态，失焦必须取消，否则指示线一直挂着。 */
        shell->doc.layer_drag_pending = false;
        shell->doc.layer_drag_active = false;
        /* 智能滤镜拖拽同理：没有落点就不该留下悬挂的高亮/指示。 */
        shell->doc.smart_drag_pending = false;
        shell->doc.smart_drag_active = false;
        shell->doc.smart_drag_from = -1;
        shell->doc.smart_drag_to = -1;
        shell->doc.histogram_drag = false;
        /* P0-3：拖动中的滑杆/曲线点收不到松手事件，abort 事务回滚到按下前（幂等）。 */
        curve_drag_finish(*shell, false);
        adjust_drag_finish(*shell, false);
        fill_panel_drag_finish(*shell, false);
        end_pan(window, *shell);
        stroke_cancel(*shell);
        /* 整层拖动被 abort 后画面必须重画，否则图层会「停在」被拖到的位置。 */
        const bool drag_was_active =
            shell->doc.layer_move_active || shell->doc.layer_handle_active;
        move_cancel(*shell);
        if (drag_was_active) InvalidateRect(window, nullptr, FALSE);
        eyedropper_cancel(*shell);
        marquee_cancel(*shell);
        /* §5.44 手柄拖动也收不到松手：丢掉预览即可（引擎从未写入，无撤销步）。 */
        if (shell->doc.selection_handle_drag >= 0) {
          selection_handle_cancel(*shell);
          InvalidateRect(window, nullptr, FALSE);
        }
        /* 缩放拖框没有半成品可回滚，丢掉矩形即可（松手事件已收不到）。 */
        shell->doc.zoom_marquee_active = false;
        lasso_cancel(*shell);
        pen_cancel_drag(*shell);
        gradient_cancel(*shell);
        progressive_axis_cancel(*shell);
        shape_cancel(*shell);
        type_cancel(*shell);
      }
      break;
    case WM_CAPTURECHANGED:
      if (shell != nullptr) {
        /* 捕获被别处抢走（菜单弹出/切窗口）时，拖行与框选一并取消，不留悬挂状态。 */
        /* ROI #3 参考线拖动同理：abort 事务，新建的那条随回滚一起消失。 */
        guide_drag_cancel(*shell);
        shell->doc.layer_drag_pending = false;
        shell->doc.layer_drag_active = false;
        /* 智能滤镜拖拽同理：没有落点就不该留下悬挂的高亮/指示。 */
        shell->doc.smart_drag_pending = false;
        shell->doc.smart_drag_active = false;
        shell->doc.smart_drag_from = -1;
        shell->doc.smart_drag_to = -1;
        shell->doc.histogram_drag = false;
        /* P0-3：拖动中的滑杆/曲线点收不到松手事件，abort 事务回滚到按下前（幂等）。 */
        curve_drag_finish(*shell, false);
        adjust_drag_finish(*shell, false);
        fill_panel_drag_finish(*shell, false);
        end_pan(window, *shell);
        stroke_cancel(*shell);
        /* 同上：捕获被抢走时整层拖动也要回滚并重画。 */
        const bool drag_was_active =
            shell->doc.layer_move_active || shell->doc.layer_handle_active;
        move_cancel(*shell);
        if (drag_was_active) InvalidateRect(window, nullptr, FALSE);
        eyedropper_cancel(*shell);
        marquee_cancel(*shell);
        /* §5.44 手柄拖动也收不到松手：丢掉预览即可（引擎从未写入，无撤销步）。 */
        if (shell->doc.selection_handle_drag >= 0) {
          selection_handle_cancel(*shell);
          InvalidateRect(window, nullptr, FALSE);
        }
        /* 缩放拖框没有半成品可回滚，丢掉矩形即可（松手事件已收不到）。 */
        shell->doc.zoom_marquee_active = false;
        /* 裁剪同理：框保留（Enter 仍可提交），但停止跟随鼠标继续变形。 */
        shell->doc.crop_dragging = false;
        lasso_cancel(*shell);
        pen_cancel_drag(*shell);
        gradient_cancel(*shell);
        progressive_axis_cancel(*shell);
        shape_cancel(*shell);
        /* §6.60 面板宽度拖动：捕获被抢走时回到按下前宽度（幂等，目标已清空则跳过）。 */
        if (shell->panel_resize_target != 0) {
          panel_resize_cancel(*shell);
          InvalidateRect(window, nullptr, FALSE);
        }
      }
      break;
    case WM_SETCURSOR:
      /*
       * §5.43 收起/展开按钮：手形。先于分界线判定——两者几何不重叠（留 3px），
       * 但收起条压在画布边的光标逻辑之上，顺序固定后不会出现「竖条上显示箭头」。
       */
      if (shell != nullptr && LOWORD(lparam) == HTCLIENT && shell->panel_toggle_hover != 0) {
        HCURSOR cursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_HAND));
        if (cursor != nullptr) {
          SetCursor(cursor);
          return TRUE;
        }
      }
      /*
       * §6.60 面板分界线：左右箭头。悬停档位由 WM_MOUSEMOVE 维护；拖动中用 target
       * 判定——此时指针可能已拖出分界线甚至拖出窗口，但形状必须一直保持到松手。
       */
      if (shell != nullptr && LOWORD(lparam) == HTCLIENT &&
          (shell->panel_resize_target != 0 || shell->panel_splitter_hover != 0)) {
        /* IDC_SIZEWE 在本 SDK 里展开成 char*，与 LoadCursorW 的 LPCWSTR 不符，按同文件既有写法转换。 */
        HCURSOR cursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZEWE));
        if (cursor != nullptr) {
          SetCursor(cursor);
          return TRUE;
        }
      }
      /*
       * §5.44 选区手柄：悬停/拖动时给方向光标。拖动中用 drag 序号判定——
       * 指针可能已离开把手，但形状必须保持到松手（同分界线那条规矩）。
       */
      if (shell != nullptr && LOWORD(lparam) == HTCLIENT &&
          (shell->doc.selection_handle_drag >= 0 || selection_handles_available(*shell))) {
        POINT pt{};
        if (GetCursorPos(&pt) != 0 && ScreenToClient(window, &pt) != 0) {
          /* ROI #7：GetCursorPos 是屏幕像素，ScreenToClient 之后仍是设备像素。 */
          pt.x = dpi_logical(pt.x, shell_dpi(*shell));
          pt.y = dpi_logical(pt.y, shell_dpi(*shell));
          Layout layout{};
          if (client_layout(*shell, &layout) && layout.canvas.contains(pt.x, pt.y)) {
            int index = shell->doc.selection_handle_drag;
            if (index < 0) {
              comp_ui::Point centers[comp_ui::kSelectionHandleCount];
              selection_handle_centers(*shell, layout, shell->doc.selection_base, centers);
              index = comp_ui::selection_handle_hit(
                  centers, comp_ui::kSelectionHandleCount,
                  comp_ui::Point{static_cast<double>(pt.x), static_cast<double>(pt.y)},
                  kSelectionHandleHitPx);
            }
            HCURSOR cursor = selection_handle_cursor(index);
            if (cursor != nullptr) {
              SetCursor(cursor);
              return TRUE;
            }
          }
        }
      }
      /*
       * 缩放工具在画布内显示自绘放大镜（Alt 按下 = 缩小形态）；平移中保持抓手，
       * 其它区域/工具交给 DefWindowProc 维持默认光标。
       */
      if (shell != nullptr && LOWORD(lparam) == HTCLIENT && !shell->panning &&
          shell->tool == comp_ui::ToolId::Zoom && shell->doc.document != nullptr) {
        POINT pt{};
        if (GetCursorPos(&pt) != 0 && ScreenToClient(window, &pt) != 0) {
          /* ROI #7：同上，光标坐标换成逻辑像素再与画布矩形比。 */
          pt.x = dpi_logical(pt.x, shell_dpi(*shell));
          pt.y = dpi_logical(pt.y, shell_dpi(*shell));
          Layout layout{};
          if (client_layout(*shell, &layout) && layout.canvas.contains(pt.x, pt.y)) {
            HCURSOR cursor = zoom_cursor_handle((GetKeyState(VK_MENU) & 0x8000) != 0);
            if (cursor != nullptr) {
              SetCursor(cursor);
              return TRUE;
            }
          }
        }
      }
      break;
    case WM_MOUSEMOVE:
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          /* ROI #3 参考线拖动持有捕获：先于面板悬停与工具状态机。 */
          if (shell->doc.guide_drag_active) {
            guide_drag_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /* §6.60 面板分界线拖动优先：宽度实时生效，画面不动（pan 已补偿）。 */
          if (shell->panel_resize_target != 0) {
            panel_resize_extend(*shell, mouse_x);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /*
           * 分界线悬停高亮：5px 窄带 + 1px 分隔线，没有高亮用户找不到可拖的地方。
           * 只在档位变化时重画，移动量大时不产生额外闪烁。
           */
          const int hover = comp_ui::panel_splitter_at(layout, mouse_x, mouse_y);
          if (hover != shell->panel_splitter_hover) {
            shell->panel_splitter_hover = hover;
            InvalidateRect(window, nullptr, FALSE);
          }
          /* §5.43 收起/展开按钮同样要悬停高亮：18px 的小方块没有高亮没人会去点。 */
          const int toggle_hover = comp_ui::panel_toggle_at(layout, mouse_x, mouse_y);
          if (toggle_hover != shell->panel_toggle_hover) {
            shell->panel_toggle_hover = toggle_hover;
            InvalidateRect(window, nullptr, FALSE);
          }
          /* ROI #3：Move 工具贴近参考线时描亮（只在高亮序号变化时重画）。 */
          if (guide_hover_update(*shell, mouse_x, mouse_y, layout)) {
            InvalidateRect(window, nullptr, FALSE);
          }
          /* P0-3 曲线拖动优先：几何用按下时冻结的绘图区，写入落在同一个事务里。 */
          if (shell->doc.curve_drag_point >= 0) {
            curve_drag_to(*shell, shell->doc.curve_drag_plot, static_cast<double>(mouse_x),
                          static_cast<double>(mouse_y));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /* P0-3 滑杆拖动优先：持有鼠标捕获，写入落在按下时开的同一个事务里。 */
          if (shell->doc.adj_drag_param >= 0) {
            adjust_drag_value(*shell, static_cast<double>(mouse_x));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          /* P1-2 收尾 ⑧ 填充滑杆同理：几何用按下瞬间冻结的滑道。 */
          if (shell->doc.fill_drag_param >= 0) {
            fill_panel_drag_value(*shell, static_cast<double>(mouse_x));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          }
          /*
           * S24：图层拖行（超过 4px 阈值才进入 active）与直方图框选优先于工具状态机。
           * 两者都持有鼠标捕获，必须在这里更新，否则松手时目标行停在按下那一行。
           */
          if (shell->doc.layer_drag_pending) {
            const int dy = mouse_y - shell->doc.layer_drag_press_y;
            if (!shell->doc.layer_drag_active && (dy >= 4 || dy <= -4)) {
              shell->doc.layer_drag_active = true;
            }
            if (shell->doc.layer_drag_active) {
              /* S44：目标行只在可见行里找——折叠掉的子层不该成为落点。 */
              shell->doc.layer_drag_target = comp_ui::layer_drag_target_row(
                  layout.layer_panel, panel_row_count(*shell), static_cast<double>(mouse_y),
                  comp_ui::layer_rows_top(shell->doc.histogram_open));
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (shell->doc.histogram_drag) {
            shell->doc.histogram_drag_to_x = mouse_x;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->doc.zoom_marquee_active) {
            /* 不 return：后面的笔尖/光标跟踪仍要走，否则拖框时光标状态会冻住。 */
            zoom_marquee_extend(*shell, mouse_x, mouse_y);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.move_active || shell->doc.layer_move_active ||
                     shell->doc.layer_handle_active) {
            move_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.crop_active) {
            crop_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.pen_dragging) {
            pen_drag(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.progressive_axis_dragging) {
            progressive_axis_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.gradient_active) {
            gradient_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.shape_active) {
            shape_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.eyedropper_drag) {
            /* 吸管拖动连续取样：只更新前景色与状态栏，不改画布内容。 */
            eyedropper_sample(*shell, mouse_x, mouse_y, layout);
          } else if (shell->doc.marquee_active) {
            marquee_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.selection_handle_drag >= 0) {
            /* §5.44 只改预览落点：引擎在松手前不动，拖动中途没有撤销步。 */
            selection_handle_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->doc.lasso_active) {
            lasso_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (stroke_in_progress(*shell)) {
            stroke_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->panning) {
            shell->doc.view.pan_by(static_cast<double>(mouse_x - shell->pan_anchor.x),
                               static_cast<double>(mouse_y - shell->pan_anchor.y));
            shell->pan_anchor.x = mouse_x;
            shell->pan_anchor.y = mouse_y;
            InvalidateRect(window, nullptr, FALSE);
          }
        }
        /*
         * 笔尖轮廓跟随光标：只在笔刷类工具下刷新，避免每帧重画。
         * WM_MOUSELEAVE 是一次性的，收到后要重新 Track 才能再收到。
         * 分界线悬停同样要 Track：否则指针从面板直接移出窗口，高亮会一直留着。
         */
        const bool want_track = comp_ui::tool_has_brush_tip(shell->tool) ||
                                shell->panel_splitter_hover != 0 ||
                                shell->panel_toggle_hover != 0;
        if (want_track) {
          if (comp_ui::tool_has_brush_tip(shell->tool)) {
            shell->cursor_x = mouse_x;
            shell->cursor_y = mouse_y;
            shell->cursor_valid = true;
          }
          if (!shell->tracking_leave) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            TrackMouseEvent(&track);
            shell->tracking_leave = true;
          }
          /* 只有笔尖要跟随光标重画；纯悬停高亮已在档位变化时重画过，不必每帧刷。 */
          if (comp_ui::tool_has_brush_tip(shell->tool)) {
            InvalidateRect(window, nullptr, FALSE);
          }
        }
      }
      break;
    case WM_MOUSELEAVE:
      if (shell != nullptr) {
        shell->cursor_valid = false;
        shell->tracking_leave = false;
        /* §6.60 指针离开窗口：分界线高亮必须一起熄灭，否则会留一条亮边。 */
        shell->panel_splitter_hover = 0;
        /* §5.43 收起按钮同理。 */
        shell->panel_toggle_hover = 0;
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_TIMER:
      /* 蚂蚁线相位：只在有选区时跑，避免空转重绘。 */
      if (shell != nullptr && wparam == kAntsTimerId) {
        if (shell->doc.selection_active) {
          shell->doc.selection_phase = (shell->doc.selection_phase + 1) % kAntsPhasePeriod;
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      /* 字体扫描分片：每片探几个文件，扫完自己 KillTimer。面板关闭时停表，
       * 已探条目与 cursor 保留——下次打开面板接着扫，不必从头再来。 */
      if (shell != nullptr && wparam == kFontScanTimerId) {
        font_scan_step(window, *shell);
        return 0;
      }
      break;
    case WM_SYSKEYDOWN:
      /*
       * S22：Ctrl+Alt+G = 剪贴蒙版开关（对齐上游 Create Clipping Mask）。
       * Alt 组合只能在这里收到（WM_SYSKEYDOWN），G 单独按是渐变工具，因此必须
       * 带 Alt+Ctrl 才不抢键；放最前面先判，省得走到 OEM_4/OEM_6 分支。
       */
      if (shell != nullptr && wparam == 'G') {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
          run_shell_command(window, *shell, kCmdLayerToggleClipping);
          return 0;
        }
      }
      /*
       * P1：Ctrl+Alt+H = 历史记录面板。Ctrl+Shift+H 已是「水平翻转」且走 WM_KEYDOWN，
       * 这里必须带 Alt，否则 H 单独按是抓手工具。
       */
      if (shell != nullptr && wparam == 'H') {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
          run_shell_command(window, *shell, kCmdHistoryPanel);
          return 0;
        }
      }
      /*
       * Ctrl+Alt+N / S / I / C / R：新建文档… / 另存为… / 图像大小… / 画布大小… /
       * Camera Raw…
       * 放在 Alt 组合里是因为 Ctrl+N、Ctrl+S、Ctrl+I、Ctrl+R（旋转 90°）各自
       * 已被占用，不带 Alt 就会改掉既有手感；Ctrl+Shift+S 也已被导出占用。
       */
      if (shell != nullptr) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
          UINT command = 0;
          switch (wparam) {
            case 'N': command = kCmdNewDocumentDialog; break;
            case 'S': command = kCmdSaveAs; break;
            case 'I': command = kCmdImageSize; break;
            case 'C': command = kCmdCanvasSize; break;
            case 'E': command = kCmdLayerMergeVisible; break; /* P1-2 ② 合并可见 */
            case 'V': command = kCmdEditPasteInto; break; /* P0-4 贴入选区 */
            case 'R': command = kCmdCameraRaw; break;
            default: break;
          }
          if (command != 0) {
            run_shell_command(window, *shell, command);
            return 0;
          }
        }
      }
      /*
       * Alt+[ / Alt+]：模糊模式调半径，涂抹/液化与减淡/加深/海绵调强度。
       * 普通 [ ] 已用于笔刷，Ctrl+[ ] 用于图层不透明度，因此放到 Alt 组合，
       * 且必须在这里吞掉，否则 DefWindowProc 会响铃。
       */
      if (shell != nullptr && (wparam == VK_OEM_4 || wparam == VK_OEM_6)) {
        const int direction = wparam == VK_OEM_6 ? 1 : -1;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        wchar_t buffer[128];
        if (shell->tool == comp_ui::ToolId::Type) {
          /* S14：Alt+[ / ] 调行距（5% 一步，夹在 100%..800%）；键入态也可用。 */
          shell->doc.type_line_height =
              comp_ui::step_type_line_height(shell->doc.type_line_height, direction);
          if (shell->doc.type_active) {
            type_refresh_status(*shell);
          } else {
            wsprintfW(buffer, L"文字行距 %d%%（Alt+[ / ] 调整）",
                      static_cast<int>(shell->doc.type_line_height * 100.0 + 0.5));
            set_status(*shell, buffer);
          }
        } else if (shell->tool == comp_ui::ToolId::Wand) {
          /* S8：魔棒的取样窗口是 (2r+1)²，0 = 只取种子像素（S7 行为）。 */
          shell->wand_radius = comp_ui::step_wand_radius(shell->wand_radius, direction);
          wsprintfW(buffer, L"魔棒取样窗口 %d×%d（半径 %d px）",
                    shell->wand_radius * 2 + 1, shell->wand_radius * 2 + 1, shell->wand_radius);
        } else if (shell->tool == comp_ui::ToolId::Shape && shift &&
                   shell->shape_kind == comp_ui::kShapeRect) {
          /* S10：Shift+Alt+[ / ] 调矩形圆角半径（0 = 直角；引擎再夹到短边一半）。 */
          shell->shape_corner_radius =
              comp_ui::step_shape_corner_radius(shell->shape_corner_radius, direction);
          wsprintfW(buffer, L"矩形圆角半径 %d px（0 = 直角）",
                    static_cast<int>(shell->shape_corner_radius + 0.5));
        } else if (shell->tool == comp_ui::ToolId::Shape) {
          /* S10：Alt+[ / ] 调形状描边宽度（直线用；矩形/椭圆的外壳默认纯填充）。 */
          shell->shape_stroke_width =
              comp_ui::step_shape_stroke_width(shell->shape_stroke_width, direction);
          wsprintfW(buffer, L"形状描边宽度 %d px（直线描边用）",
                    static_cast<int>(shell->shape_stroke_width + 0.5));
        } else if (shell->tool == comp_ui::ToolId::HistoryBrush) {
          /* P1：Alt+[ / ] 调历史记录画笔的不透明度（下限 10%，0 会被引擎拒）。 */
          shell->history_brush_opacity =
              comp_ui::step_history_brush_opacity(shell->history_brush_opacity, direction);
          wsprintfW(buffer, L"历史记录画笔不透明度 %d%%（取样第 %d 个状态）",
                    static_cast<int>(shell->history_brush_opacity * 100.0 + 0.5),
                    history_brush_resolved_state(*shell));
        } else if (shell->tool == comp_ui::ToolId::Heal) {
          /* P1：Alt+[ / ] 调修补不透明度（下限 10%，0 会被域层拒）。 */
          shell->heal_opacity = comp_ui::step_heal_opacity(shell->heal_opacity, direction);
          wsprintfW(buffer, L"修补不透明度 %d%%（%s · 直径 %d px）",
                    static_cast<int>(comp_ui::clamp_heal_opacity(shell->heal_opacity) * 100.0 + 0.5),
                    widen_ascii(comp_ui::heal_mode_name(shell->heal_mode)).c_str(),
                    static_cast<int>(shell->brush_size + 0.5));
        } else if (shell->tool == comp_ui::ToolId::Clone) {
          /* 仿制不透明度：10% 一步，下限 10%（引擎允许 0，但 0 = 什么都不画）。 */
          shell->clone_opacity =
              comp_ui::step_clone_opacity(shell->clone_opacity, direction);
          wsprintfW(buffer, L"仿制不透明度 %d%%（%s）",
                    static_cast<int>(comp_ui::clamp_clone_opacity(shell->clone_opacity) * 100.0 + 0.5),
                    shell->clone_aligned ? L"对齐" : L"不对齐");
        } else if (shell->tool == comp_ui::ToolId::Tone) {
          /* P1：Alt+[ / ] 调减淡/加深/海绵的强度（Exposure / Flow 口径，10% 一步）。 */
          shell->tone_strength = comp_ui::step_tone_strength(shell->tone_strength, direction);
          wsprintfW(buffer, L"%s强度 %d%%",
                    widen_ascii(comp_ui::tone_state_name(shell->tone_state)).c_str(),
                    static_cast<int>(shell->tone_strength * 100.0 + 0.5));
        } else if (shell->tool == comp_ui::ToolId::Retouch &&
                   shell->retouch_mode != comp_ui::kRetouchBlur) {
          shell->retouch_strength =
              comp_ui::step_retouch_strength(shell->retouch_strength, direction);
          wsprintfW(buffer, L"%s强度 %d%%",
                    widen_ascii(comp_ui::retouch_mode_name(shell->retouch_mode)).c_str(),
                    static_cast<int>(shell->retouch_strength * 100.0 + 0.5));
        } else {
          shell->blur_radius = comp_ui::step_blur_radius(shell->blur_radius, direction);
          wsprintfW(buffer, L"模糊半径 %d px",
                    static_cast<int>(shell->blur_radius + 0.5));
        }
        set_status(*shell, buffer);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      /*
       * Alt+, / Alt+.：选中图层等比例缩放（中心锚定）。
       * Alt+[ ] 已被模糊半径 / 涂抹强度 / 文字行距占用，Ctrl+[ ] 是不透明度，
       * 普通 [ ] 是笔刷，因此缩放取逗号/句点这一对空闲键。
       */
      if (shell != nullptr && (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD)) {
        panel_scale_layer(*shell, wparam == VK_OEM_PERIOD ? 1 : -1);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_MOUSEWHEEL:
      if (shell != nullptr) {
        nudge_flush(*shell);
        /*
         * WM_MOUSEWHEEL 的坐标是屏幕坐标：先换算到客户区（设备像素），再按
         * DPI 换成逻辑像素（ROI #7）。这里不能用工人入口的 mouse_*——那两个
         * 值是拿 lParam 当客户区坐标换算出来的，对滚轮消息没有意义。
         */
        POINT cursor{device_x, device_y};
        ScreenToClient(window, &cursor);
        cursor.x = dpi_logical(cursor.x, window_dpi);
        cursor.y = dpi_logical(cursor.y, window_dpi);
        Layout layout{};
        if (client_layout(*shell, &layout) && layout.canvas.contains(cursor.x, cursor.y)) {
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const double factor = delta > 0 ? 1.2 : 1.0 / 1.2;
            shell->doc.view.zoom_by(factor, cursor.x, cursor.y, layout.canvas);
            wchar_t buffer[96];
            wsprintfW(buffer, L"缩放 %d%%",
                      static_cast<int>(shell->doc.view.zoom * 100.0 + 0.5));
            set_status(*shell, buffer);
            InvalidateRect(window, nullptr, FALSE);
          }
        } else if (client_layout(*shell, &layout) && layout.layer_panel.w > 0 &&
                   !layout.layer_collapsed && shell->doc.histogram_open &&
                   comp_ui::histogram_plot_rect(layout.layer_panel)
                       .contains(cursor.x, cursor.y)) {
          /*
           * 直方图滚轮缩放（S24，PS 同款）：以光标下的亮度为中心，一格 25%。
           * 必须先于画布缩放判断——直方图绘图区与画布不重叠，但顺序写反会让
           * 「光标在图内却缩放画布」成为可能；这里显式 return，语义单一。
           */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const comp_ui::Rect plot = comp_ui::histogram_plot_rect(layout.layer_panel);
            double focus = comp_ui::histogram_x_to_bin(plot, shell->doc.histogram_view,
                                                       static_cast<double>(cursor.x));
            if (focus < 0.0) focus = 0.0;
            if (focus > comp_ui::kHistogramMaxBin) focus = comp_ui::kHistogramMaxBin;
            panel_histogram_zoom(*shell, delta > 0 ? 1.0 / 1.25 : 1.25, focus);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        } else if (client_layout(*shell, &layout) && layout.adjust_panel.w > 0 &&
                   !layout.adjust_collapsed && history_panel_visible(*shell) &&
                   layout.adjust_panel.contains(cursor.x, cursor.y)) {
          /* 历史记录面板滚轮翻列表：一格 3 行，夹取在 ui_core。 */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const int count = history_state_count(*shell);
            const int next = shell->doc.history_scroll + (delta > 0 ? -3 : 3);
            shell->doc.history_scroll =
                comp_ui::history_clamp_scroll(layout.adjust_panel, count, next);
            InvalidateRect(window, nullptr, FALSE);
          }
        } else if (client_layout(*shell, &layout) && layout.adjust_panel.w > 0 &&
                   !layout.adjust_collapsed && font_panel_visible(*shell) &&
                   layout.adjust_panel.contains(cursor.x, cursor.y)) {
          /* 字体面板滚轮翻列表：一格 3 行，夹取在 ui_core。 */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const int count = font_row_count(*shell);
            const int next = shell->font_scroll + (delta > 0 ? -3 : 3);
            shell->font_scroll = comp_ui::font_clamp_scroll(layout.adjust_panel, count, next);
            InvalidateRect(window, nullptr, FALSE);
          }
          return 0;
        } else if (client_layout(*shell, &layout) && layout.adjust_panel.w > 0 &&
                   !layout.adjust_collapsed && channel_panel_visible(*shell) &&
                   layout.adjust_panel.contains(cursor.x, cursor.y)) {
          /* ROI #5 通道面板滚轮翻列表：一格 3 行，夹取在 ui_core。 */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const int count = channel_count(*shell);
            const int next = shell->doc.channel_scroll + (delta > 0 ? -3 : 3);
            shell->doc.channel_scroll =
                comp_ui::channel_clamp_scroll(layout.adjust_panel, count, next);
            InvalidateRect(window, nullptr, FALSE);
          }
          return 0;
        } else if (client_layout(*shell, &layout) && layout.adjust_panel.w > 0 &&
                   !layout.adjust_collapsed && shell->doc.fill_open &&
                   !shell->doc.effect_open && !shell->doc.filter_open &&
                   layout.adjust_panel.contains(cursor.x, cursor.y)) {
          /* 填充参数表滚轮翻行：一格 3 行，夹取在 ui_core（图案 10 行会用到）。 */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const int32_t count = static_cast<int32_t>(shell->doc.fill_values.size());
            const int next = shell->doc.fill_scroll + (delta > 0 ? -3 : 3);
            shell->doc.fill_scroll =
                comp_ui::adjust_param_clamp_scroll(layout.adjust_panel, count, next);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        } else if (client_layout(*shell, &layout) && layout.adjust_panel.w > 0 &&
                   !layout.adjust_collapsed && shell->doc.smart_open &&
                   layout.adjust_panel.contains(cursor.x, cursor.y)) {
                   !layout.adjust_collapsed && !shell->doc.effect_open && !shell->doc.filter_open &&
                   !type_panel_visible(*shell) && !font_panel_visible(*shell) &&
                   !channel_panel_visible(*shell) &&
                   layout.adjust_panel.contains(cursor.x, cursor.y)) {
          /*
           * P0-3：调整参数表滚动（色阶每页 5 行 / 小窗口），一格 3 行，夹取在 ui_core。
           * 曲线页没有参数表：滚轮不吞掉，交给下面的默认处理（不做事也不报错）。
           */
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const int count = adjust_shown_count(*shell);
            const int next = shell->doc.adj_scroll + (delta > 0 ? -3 : 3);
            shell->doc.adj_scroll =
                comp_ui::adjust_param_clamp_scroll(layout.adjust_panel, count, next);
            InvalidateRect(window, nullptr, FALSE);
          }
        }
        return 0;
      }
      break;
    case WM_COMMAND:
      /* lparam != 0 表示控件通知，本骨架无子控件，直接忽略。 */
      if (shell != nullptr && lparam == 0) {
        if (LOWORD(wparam) >= kCmdToolBase &&
            LOWORD(wparam) < kCmdToolBase + comp_ui::tool_count()) {
          const comp_ui::ToolInfo* info =
              comp_ui::tool_info_at(static_cast<int>(LOWORD(wparam) - kCmdToolBase));
          if (info != nullptr) set_tool(*shell, info->id);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        run_shell_command(window, *shell, LOWORD(wparam));
        return 0;
      }
      break;
    case WM_SIZE:
      if (shell != nullptr) InvalidateRect(window, nullptr, FALSE);
      break;
    case WM_DPICHANGED:
      /*
       * ROI #7：跨屏或改显示缩放时 Windows 给出新 DPI 与**建议窗口矩形**。
       * 建议矩形要照收——它是按新 DPI 换算过的物理尺寸，不采纳的话窗口会带着
       * 旧 DPI 的物理大小跳屏，视觉尺寸突跳一档。排版本身都是逻辑像素，改完
       * 状态重绘即可，不需要重算任何常量。
       */
      if (shell != nullptr) {
        const UINT new_dpi = HIWORD(static_cast<DWORD>(wparam));
        if (new_dpi >= 96 && new_dpi <= 480) shell->dpi = static_cast<int>(new_dpi);
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
          SetWindowPos(window, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_CLOSE:
      /* 关窗也收口：微调事务里的位移已经生效，不 commit 就丢掉那一步的撤销能力。 */
      if (shell != nullptr) {
        nudge_flush(*shell);
        /*
         * P0-3：脏文档先问「保存 / 不保存 / 取消」。用右栏面板而不是 MessageBox——
         * 自动化 stop() 是「post WM_CLOSE 后等几秒」，阻塞式弹窗只能等到超时，
         * 面板则能用按键回答（S / D / Esc，见 handle_key）。
         */
        if (shell->doc.doc_dialog == comp_ui::DocDialogKind::ClosePrompt) {
          /* 提示已经开着：再点一次关闭等同「取消」，不销毁，也不重置用户的选择。 */
          return 0;
        }
        int32_t dirty = 0;
        if (shell->doc.document != nullptr &&
            comp_document_is_dirty(shell->doc.document, &dirty) == COMP_OK && dirty != 0) {
          doc_dialog_begin(*shell, comp_ui::DocDialogKind::ClosePrompt);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

/* 菜单结构：文件 / 编辑 / 图层 / 视图 / 工具 / 调整；快捷键提示文本与 handle_key
 * 的映射保持一致，工具菜单直接取自 ui_core.h 的 tool_table。 */
HMENU build_menu() {
  HMENU menu = CreateMenu();

  HMENU file_menu = CreatePopupMenu();
  AppendMenuW(file_menu, MF_STRING, kCmdNew, L"新建(&N)\tCtrl+N");
  AppendMenuW(file_menu, MF_STRING, kCmdNewDocumentDialog, L"新建文档…(&W)\tCtrl+Alt+N");
  AppendMenuW(file_menu, MF_STRING, kCmdOpen, L"打开(&O)...\tCtrl+O");
  AppendMenuW(file_menu, MF_STRING, kCmdSave, L"保存(&S)\tCtrl+S");
  AppendMenuW(file_menu, MF_STRING, kCmdSaveAs, L"另存为…(&A)\tCtrl+Alt+S");
  AppendMenuW(file_menu, MF_STRING, kCmdOpenImage, L"打开位图(&I)...\tCtrl+Shift+O");
  AppendMenuW(file_menu, MF_STRING, kCmdImportImage, L"置入(&P)...");
  AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file_menu, MF_STRING, kCmdExportImage, L"导出为(&E)...\tCtrl+Shift+S");
  AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file_menu, MF_STRING, kCmdExit, L"退出(&X)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"文件(&F)");

  HMENU edit_menu = CreatePopupMenu();
  AppendMenuW(edit_menu, MF_STRING, kCmdUndo, L"撤销(&U)\tCtrl+Z");
  AppendMenuW(edit_menu, MF_STRING, kCmdRedo, L"重做(&R)\tCtrl+Y");
  AppendMenuW(edit_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(edit_menu, MF_STRING, kCmdEditCopy, L"拷贝(&C)\tCtrl+C");
  AppendMenuW(edit_menu, MF_STRING, kCmdEditCopyMerged, L"合并拷贝(&M)\tCtrl+Shift+C");
  AppendMenuW(edit_menu, MF_STRING, kCmdEditCut, L"剪切(&T)\tCtrl+X");
  AppendMenuW(edit_menu, MF_STRING, kCmdEditPaste, L"粘贴(&P)\tCtrl+V");
  AppendMenuW(edit_menu, MF_STRING, kCmdEditPasteInto, L"贴入选区(&I)…\tCtrl+Alt+V");
  AppendMenuW(edit_menu, MF_STRING, kCmdEditClear, L"清除(&E)\tBackspace");
  AppendMenuW(edit_menu, MF_SEPARATOR, 0, nullptr);
  /* 选区三条（全选/取消/反选）已搬到单独的「选择」菜单，ID 不变。 */
  AppendMenuW(edit_menu, MF_STRING, kCmdHistoryPanel, L"历史记录(&H)…\tCtrl+Alt+H");
  AppendMenuW(edit_menu, MF_STRING, kCmdHistorySettings, L"历史记录容量…(&C)");
  AppendMenuW(edit_menu, MF_STRING, kCmdListCommands, L"枚举引擎命令(&L)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit_menu), L"编辑(&E)");

  HMENU layer_menu = CreatePopupMenu();
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerAdd, L"新建图层(&A)\tCtrl+Shift+N");
  /*
   * P0-1：图层面板补全。这五条此前壳里完全没有入口——引擎早就支持建组
   * （is_group）与改名，但壳固定传 0、也没有 set_name；删除/复制更是连 ABI
   * 都没有。现在导出补齐（comp_layer_set_name / duplicate / move），入口在这里。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerNewGroup, L"新建组(&G)\tCtrl+G");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerDuplicate, L"复制图层(&P)\tCtrl+J");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerDelete, L"删除图层(&X)\tDel");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerRename, L"重命名图层(&N)\tF2");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMergeDown, L"向下合并(&M)\tCtrl+E");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFlatten, L"拼合图像(&F)");
  /*
   * P1-2 第二批：合并可见 / 反向排列。上游 Merge Visible 的 Ctrl+Shift+E 在本壳
   * 已被「效果参数」占用（§6.3x），这里退到 Ctrl+Alt+E；反向排列沿用上游的
   * 「作用于当前层所在层级」语义，不需要面板。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMergeVisible, L"合并可见(&V)\tCtrl+Alt+E");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerReverse, L"反向排列(&R)");
  /* P1-2 ③：链接图层（多选后点一次连上，再点一次解开）。 */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerLinkToggle, L"链接图层(&K)");
  /*
   * P1-2 收尾：按类型 / 名称 / 颜色过滤面板（纯视图态，不写文档）。PS 把这三条
   * 放在图层面板顶部的下拉里；本壳没有面板顶栏，收进「图层 > 过滤」子菜单。
   */
  HMENU layer_filter_menu = CreatePopupMenu();
  const wchar_t* filter_kind_labels[] = {L"全部类型", L"像素图层", L"文字图层",
                                         L"图层组",   L"调整图层", L"填充图层",
                                         L"智能对象"};
  for (UINT i = 0; i < 7; ++i) {
    AppendMenuW(layer_filter_menu, MF_STRING, kCmdLayerFilterKindBase + i, filter_kind_labels[i]);
  }
  HMENU layer_filter_color_menu = CreatePopupMenu();
  AppendMenuW(layer_filter_color_menu, MF_STRING, kCmdLayerFilterColorAll, L"全部颜色");
  AppendMenuW(layer_filter_color_menu, MF_STRING, kCmdLayerFilterColorNone, L"无标签");
  const wchar_t* filter_color_labels[] = {L"红", L"橙", L"黄", L"绿", L"蓝", L"紫", L"灰"};
  for (UINT i = 0; i < 7; ++i) {
    AppendMenuW(layer_filter_color_menu, MF_STRING, kCmdLayerFilterColorBase + i,
                filter_color_labels[i]);
  }
  AppendMenuW(layer_filter_menu, MF_POPUP,
              reinterpret_cast<UINT_PTR>(layer_filter_color_menu), L"颜色标签(&C)");
  AppendMenuW(layer_filter_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_filter_menu, MF_STRING, kCmdLayerFilterName, L"按名称过滤…(&N)");
  AppendMenuW(layer_filter_menu, MF_STRING, kCmdLayerFilterClear, L"清除过滤(&X)");
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layer_filter_menu),
              L"过滤图层(&F)");
  /*
   * P1-2 收尾：图层复合（PS 的 Layer Comps 面板）。复合记「每个现存图层的可见性 +
   * 不透明度」，存进文档、进撤销栈、随 .comp 落盘；「当前复合」只是视图态。
   * 上游的面板用缩略图列表，本壳没有该面板，改成子菜单 + 上一个/下一个循环切换。
   */
  HMENU layer_comp_menu = CreatePopupMenu();
  AppendMenuW(layer_comp_menu, MF_STRING, kCmdLayerCompCapture, L"捕获为新复合…(&C)");
  AppendMenuW(layer_comp_menu, MF_STRING, kCmdLayerCompApply, L"应用当前复合(&A)");
  AppendMenuW(layer_comp_menu, MF_STRING, kCmdLayerCompUpdate, L"用当前状态更新(&U)");
  AppendMenuW(layer_comp_menu, MF_STRING, kCmdLayerCompRename, L"重命名当前复合…(&N)");
  AppendMenuW(layer_comp_menu, MF_STRING, kCmdLayerCompDelete, L"删除当前复合(&D)");
  AppendMenuW(layer_comp_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_comp_menu, MF_STRING, kCmdLayerCompPrev, L"上一个复合(&P)");
  AppendMenuW(layer_comp_menu, MF_STRING, kCmdLayerCompNext, L"下一个复合(&X)");
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layer_comp_menu),
              L"图层复合(&O)");
  /*
   * P1-2 收尾 ⑦：智能对象。转换把当前像素冻结成不可变 source，之后的「滤镜 > 应用」
   * 变成非破坏的智能滤镜；栅格化丢掉可编辑性、保留当前渲染。清除滤镜整次只落一步
   * 撤销（壳开显式事务包住逐条删除）。
   */
  HMENU smart_menu = CreatePopupMenu();
  AppendMenuW(smart_menu, MF_STRING, kCmdLayerSmartConvert, L"转换为智能对象(&V)");
  AppendMenuW(smart_menu, MF_STRING, kCmdLayerSmartRasterize, L"栅格化(&R)");
  AppendMenuW(smart_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(smart_menu, MF_STRING, kCmdLayerSmartClear, L"清除全部智能滤镜(&X)");
  AppendMenuW(smart_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * P1-2 收尾 ⑧：新建填充图层。三种 kind 共用一套参数行（参数表由引擎驱动）：
   * 菜单项 = 开面板 + 用缺省参数立刻建层，之后在面板里调；「编辑」只对已选中的
   * 填充层生效（kind 取自图层本身，避免用上次的 kind 把渐变改写成纯色）。
   */
  HMENU fill_menu = CreatePopupMenu();
  AppendMenuW(fill_menu, MF_STRING, kCmdLayerFillSolid, L"纯色…(&C)");
  AppendMenuW(fill_menu, MF_STRING, kCmdLayerFillGradient, L"渐变…(&G)");
  AppendMenuW(fill_menu, MF_STRING, kCmdLayerFillPattern, L"图案…(&P)");
  AppendMenuW(fill_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(fill_menu, MF_STRING, kCmdLayerFillEdit, L"编辑填充参数(&E)");
  AppendMenuW(fill_menu, MF_STRING, kCmdLayerFillRasterize, L"栅格化填充(&Z)");
  AppendMenuW(fill_menu, MF_STRING, kCmdLayerFillStatus, L"状态（种类/参数）");
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fill_menu), L"新建填充图层(&F)");
  /*
   * P1-2 ④：图层样式（= 效果栈）的拷贝/粘贴。上游的 Ctrl+Alt+C / Ctrl+Alt+V
   * 在本壳分别是「画布大小…」与「贴入选区」，所以这两条只做菜单入口。
   */
  HMENU style_menu = CreatePopupMenu();
  AppendMenuW(style_menu, MF_STRING, kCmdLayerCopyStyle, L"拷贝图层样式(&C)");
  AppendMenuW(style_menu, MF_STRING, kCmdLayerPasteStyle, L"粘贴图层样式(&P)");
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(style_menu), L"图层样式(&Y)");
  /* P0-6 ①：文字图层的像素是文字对象的渲染缓存，要直接涂像素必须先显式栅格化。 */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerRasterizeText, L"栅格化文字(&R)");
  /* P0-6 ②：字体面板（枚举系统字体，切换下一次打字的字体轨道）。 */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFonts, L"字体…(&O)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSelectUp, L"选择上一图层(&U)\t↑");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSelectDown, L"选择下一图层(&D)\t↓");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerToggleVisible, L"切换可见性(&V)\tCtrl+Shift+V");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * S22 排列顺序：这四条接的是 comp_layer_reorder——T2 全量测试点名的
   * 「壳侧调用数为 0」缺口之一，用户此前完全无法调整图层堆叠次序。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerBringForward, L"上移一层(&W)\tCtrl+Shift+↑");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSendBackward, L"下移一层(&S)\tCtrl+Shift+↓");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerBringToFront, L"置于顶层(&T)\tCtrl+Shift+Home");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSendToBack, L"置于底层(&B)\tCtrl+Shift+End");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * S22 蒙版三连：建（选区->蒙版）/读（蒙版->选区）/清。引擎侧蒙版早已参与
   * 合成与 .comp 落盘，缺的只是壳没调 setter；补 comp_layer_get_mask 后面板
   * 也能回读「这一层有没有蒙版」，不会出现建了却看不出来的情况。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMaskFromSelection,
              L"从选区建立蒙版(&K)\tCtrl+Shift+M");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMaskToSelection, L"载入蒙版为选区(&Y)");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMaskRemove, L"清除蒙版(&R)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  /* S22 剪贴蒙版：挂到下方紧邻图层。 */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerToggleClipping, L"剪贴蒙版(&C)\tCtrl+Alt+G");
  /* S22 组隔离：只对图层组有意义。 */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerToggleIsolated, L"切换穿透/隔离(&I)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * S24 镜像：水平 / 垂直两个独立位。引擎一直在合成里用 flip_x/flip_y，但 ABI
   * 只有 setter，壳读不到现值就无法「切换」——所以 S24 补齐 getter 后接这里。
   * 快捷键避开已被占用的 Ctrl+Shift+V（图层显隐），改用 H / F（Flip Horizontal / Flip Vertical）。
   */
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFlipHorizontal, L"水平翻转(&H)\tCtrl+Shift+H");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFlipVertical, L"垂直翻转(&V)\tCtrl+Shift+F");
  /*
   * S24 效果子菜单：每种效果的开关（勾选 = 该层已启用），id = kCmdEffectBase + kind。
   * 引擎侧效果早已参与像素求值（compositing/effect_eval.cpp），此前壳没有入口也没有
   * 回读口，用户只能手改 .comp 里的 engineEffects 才能加投影/描边。
   */
  HMENU effect_menu = CreatePopupMenu();
  for (int kind = 0; kind < comp_ui::kEffectKindCount; ++kind) {
    wchar_t effect_label[96];
    wsprintfW(effect_label, L"%s", effect_kind_label(kind));
    AppendMenuW(effect_menu, MF_STRING, kCmdEffectBase + static_cast<UINT>(kind), effect_label);
  }
  AppendMenuW(effect_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(effect_menu, MF_STRING, kCmdEffectEdit, L"编辑参数…(&P)\tCtrl+Shift+E");
  AppendMenuW(effect_menu, MF_STRING, kCmdEffectClear, L"清除全部效果(&C)");
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(effect_menu), L"效果(&E)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacityDown, L"降低不透明度(&-)\tCtrl+[");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacityUp, L"提高不透明度(&+)\tCtrl+]");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacitySet, L"设置不透明度…(&O)\tShift+O");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMoveLeft, L"平移左(&L)	Shift+←");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMoveRight, L"平移右(&R)	Shift+→");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMoveUp, L"平移上(&U)	Shift+↑");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMoveDown, L"平移下(&D)	Shift+↓");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerRotateLeft, L"逆旋转 15°(&[)	Ctrl+Shift+←");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerRotateRight, L"顺旋转 15°(&])	Ctrl+Shift+→");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerScaleDown, L"缩小(&-)	Alt+,");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerScaleUp, L"放大(&+)	Alt+.");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerTransformNumeric, L"数值变换…(&T)	Ctrl+T");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerTransformAgain, L"再次变换(&A)	Ctrl+Shift+T");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerBlendPrev, L"上一个混合模式(&P)\tShift+B");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerBlendNext, L"下一个混合模式(&X)\tShift+N");
  /*
   * 混合模式子菜单（S21）：档位与分组逐字取自上游 LayerBlendMode.groups（同一组内不画
   * 分隔线），命令 id = kCmdBlendBase + 档位，即 comp_blend_mode 数值，菜单直达。
   */
  HMENU blend_menu = CreatePopupMenu();
  for (int group = 0; group < comp_ui::kBlendGroupCount; ++group) {
    if (group > 0) AppendMenuW(blend_menu, MF_SEPARATOR, 0, nullptr);
    for (int slot = 0; slot < 8; ++slot) {
      const int mode = comp_ui::kBlendGroups[group][slot];
      if (mode < 0) break;
      wchar_t label[64];
      wsprintfW(label, L"%s", widen_ascii(comp_ui::blend_mode_name(mode)).c_str());
      AppendMenuW(blend_menu, MF_STRING, kCmdBlendBase + static_cast<UINT>(mode), label);
    }
  }
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(blend_menu), L"混合模式(&B)");
  /*
   * S22 采样模式子菜单：kCmdSamplingBase + 档位（0 High quality / 1 Nearest /
   * 2 Smooth）。引擎真的按它改变缩放手感（resample.cpp 的 sampling_from_abi），
   * 不是展示开关；setter / getter 早就存在，壳此前一次都没调过。
   */
  HMENU sampling_menu = CreatePopupMenu();
  for (int mode = 0; mode < comp_ui::kSamplingCount; ++mode) {
    wchar_t label[64];
    wsprintfW(label, L"%s", widen_ascii(comp_ui::sampling_mode_name(mode)).c_str());
    AppendMenuW(sampling_menu, MF_STRING, kCmdSamplingBase + static_cast<UINT>(mode), label);
  }
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sampling_menu), L"采样模式(&Q)");
  /*
   * P1-2 锁定子菜单：五档单选（无 / 透明像素 / 图像像素 / 位置 / 全部）。
   * 引擎的锁位在 ABI 里可组合，但菜单只暴露 PS 那五种单值——组合态只可能来自
   * 别的 ABI 调用方或 .comp，面板按最先命中的位显示，点一次即归一。
   * 「无」单列一项，菜单里没有任何「不勾」的状态。
   */
  static const wchar_t* const kLockTexts[] = {L"无(&N)", L"透明像素(&T)", L"图像像素(&P)",
                                              L"位置(&S)", L"全部(&A)"};
  static const uint32_t kLockFlags[] = {COMP_LAYER_LOCK_NONE, COMP_LAYER_LOCK_TRANSPARENT,
                                        COMP_LAYER_LOCK_PIXELS, COMP_LAYER_LOCK_POSITION,
                                        COMP_LAYER_LOCK_ALL};
  HMENU lock_menu = CreatePopupMenu();
  for (size_t i = 0; i < sizeof(kLockFlags) / sizeof(kLockFlags[0]); ++i) {
    AppendMenuW(lock_menu, MF_STRING, kCmdLayerLockNone + static_cast<UINT>(i), kLockTexts[i]);
  }
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(lock_menu), L"锁定(&Z)");
  /*
   * P1-2 颜色标签子菜单：id = kCmdLayerColorBase + label（0 = 无，1..7 与 PS 色板同序）。
   * 纯元数据，只驱动面板行配色；不参与合成，也不推进缓存。
   */
  static const wchar_t* const kColorTexts[] = {L"无(&N)", L"红(&R)", L"橙(&O)", L"黄(&Y)",
                                               L"绿(&G)", L"蓝(&B)", L"紫(&V)", L"灰(&E)"};
  HMENU color_menu = CreatePopupMenu();
  for (uint32_t label = 0; label < COMP_LAYER_COLOR_COUNT; ++label) {
    AppendMenuW(color_menu, MF_STRING, kCmdLayerColorBase + label, kColorTexts[label]);
  }
  AppendMenuW(layer_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(color_menu), L"颜色标签(&J)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layer_menu), L"图层(&L)");

  /*
   * P1：图像菜单。三件事的 ABI（comp_document_rotate_canvas / comp_document_trim /
   * comp_layer_invert）自 §6.35 就在，壳此前一条调用点都没有——用户拿不到
   * 「转画布 / 修边 / 反相」。命令 id 与菜单、快捷键共用 run_shell_command 一条通路。
   */
  HMENU image_rotate_menu = CreatePopupMenu();
  AppendMenuW(image_rotate_menu, MF_STRING, kCmdImageRotateCW, L"顺时针 90°(&C)");
  AppendMenuW(image_rotate_menu, MF_STRING, kCmdImageRotateCCW, L"逆时针 90°(&W)");
  AppendMenuW(image_rotate_menu, MF_STRING, kCmdImageRotate180, L"180°(&1)");
  HMENU image_menu = CreatePopupMenu();
  /*
   * ROI #8：图像 > 调整（PS Image > Adjustments）。这里的每一项都是**破坏性**
   * 一次性落墨（comp_layer_apply_adjustment），与「调整」面板里的可编辑调整层
   * 并存；两边的参数键 / 量程 / 缺省值同源（ABI 参数表），壳内不抄类型名表。
   * 无参数的去色点菜单即应用，其余进面板态等 Ctrl+Shift+U。
   */
  HMENU image_adjust_menu = CreatePopupMenu();
  for (size_t i = 0; i < kImageAdjustKindCount; ++i) {
    const uint32_t kind = kImageAdjustKinds[i];
    /* 前 7 项是上游冻结的调整类型，其后是本仓 S27 追加的四项。 */
    if (i == 7) AppendMenuW(image_adjust_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(image_adjust_menu, MF_STRING,
                kCmdImageAdjustBase + static_cast<UINT>(kind),
                widen_ascii(adjustment_kind_name(kind)).c_str());
  }
  AppendMenuW(image_adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(image_adjust_menu, MF_STRING, kCmdImageInvert, L"反相(&I)\tCtrl+I");
  /* 上游 Image Size / Canvas Size：壳侧面板 + 既有 ABI，不新增导出。 */
  AppendMenuW(image_menu, MF_STRING, kCmdImageSize, L"图像大小…(&I)\tCtrl+Alt+I");
  AppendMenuW(image_menu, MF_STRING, kCmdCanvasSize, L"画布大小…(&C)\tCtrl+Alt+C");
  AppendMenuW(image_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(image_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(image_rotate_menu),
              L"图像旋转(&R)");
  AppendMenuW(image_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(image_menu, MF_STRING, kCmdImageTrim, L"修边…(&T)");
  AppendMenuW(image_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(image_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(image_adjust_menu),
              L"调整(&J)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(image_menu), L"图像(&I)");

  /*
   * P0-4：把选区命令从「编辑」里拆出来，单列一个菜单。全选/取消/反选三条
   * 只搬位置、ID 不变（handle_key 与菜单共用 run_shell_command，两条入口不会分叉）；
   * 新增的三条此前壳里没有入口——引擎的 comp_selection_modify(EXPAND/CONTRACT)
   * 与 comp_selection_load_layer(LOAD_PIXELS) 早已实现并有单测，缺的只是 UI。
   */
  HMENU select_menu = CreatePopupMenu();
  AppendMenuW(select_menu, MF_STRING, kCmdSelectAll, L"全选(&A)\tCtrl+A");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectNone, L"取消选择(&D)\tCtrl+D");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectInvert, L"反选(&I)\tCtrl+Shift+I");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectColorRange, L"色彩范围(&C)...");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectExpand, L"扩展(&E)...");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectContract, L"收缩(&O)...");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectBorder, L"边界(&B)...");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectSmooth, L"平滑(&S)...");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectGrow, L"扩大选取(&G)");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectSimilar, L"选取相似(&R)");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectTransform, L"变换选区(&T)...");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectLoadPixels, L"按图层像素载入选区(&P)");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectPaths, L"路径面板(&T)...\tCtrl+Shift+P");
  AppendMenuW(select_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(select_menu, MF_STRING, kCmdSelectChannels, L"通道面板(&C)...");
  AppendMenuW(select_menu, MF_STRING, kCmdSelectSaveChannel, L"存储选区到通道(&A)...");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(select_menu), L"选择(&S)");

  HMENU view_menu = CreatePopupMenu();
  AppendMenuW(view_menu, MF_STRING, kCmdViewFit, L"适合窗口(&F)\tCtrl+0");
  AppendMenuW(view_menu, MF_STRING, kCmdViewActual, L"实际像素(&1)\tCtrl+1");
  AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view_menu, MF_STRING, kCmdViewZoomIn, L"放大(&I)\tCtrl+=");
  AppendMenuW(view_menu, MF_STRING, kCmdViewZoomOut, L"缩小(&O)\tCtrl+-");
  AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
  /*
   * S22 直盘开关（F6）：数据来自 comp_layer_histogram 的只读入口。
   * 上游 Levels 面板左半边就是这张图，而本壳此前只有一排数字控件——
   * 用户看不到图现在偏暗还是偏亮，只能凭感觉拖参数。
   */
  AppendMenuW(view_menu, MF_STRING, kCmdHistogramToggle, L"亮度直方图(&H)\tF6");
  AppendMenuW(view_menu, MF_STRING, kCmdHistogramReset, L"直方图区间复位(&R)\t双击图内");
  /*
   * §5.43 面板整块收起：F7 / F8 与 PS 同键位（Layers / Adjustments）。勾选态 =
   * 已收起，菜单里一眼能看出哪块现在是竖条。
   */
  AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view_menu, MF_STRING, kCmdPanelLayers, L"图层面板收起/展开(&L)\tF7");
  AppendMenuW(view_menu, MF_STRING, kCmdPanelAdjust, L"调整面板收起/展开(&J)\tF8");
  /*
   * ROI #3 视图参考：标尺 / 参考线 / 网格 / 对齐。四项都是**只读覆盖层**，
   * 「对齐」只影响拖动落点，因此这整个子菜单里的开关都不会改变像素结果。
   * 勾选态由 sync_menu_state 按当前文档刷新（换文档会跟着变）。
   */
  AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view_menu, MF_STRING, kCmdViewRulers, L"标尺(&U)\tCtrl+R");
  AppendMenuW(view_menu, MF_STRING, kCmdViewGuides, L"参考线(&G)\tCtrl+;");
  AppendMenuW(view_menu, MF_STRING, kCmdViewGrid, L"网格(&D)\tCtrl+'");
  AppendMenuW(view_menu, MF_STRING, kCmdViewSnap, L"对齐(&A)\tCtrl+Shift+;");
  AppendMenuW(view_menu, MF_STRING, kCmdViewClearGuides, L"清除参考线(&C)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"视图(&V)");

  /* 工具菜单与工具条、快捷键共用同一张表（tool_table），不另抄一份键位。 */
  HMENU tool_menu = CreatePopupMenu();
  for (int i = 0; i < comp_ui::tool_count(); ++i) {
    const comp_ui::ToolInfo* info = comp_ui::tool_info_at(i);
    if (info == nullptr) continue;
    wchar_t label[128];
    wsprintfW(label, L"%s\t%c", widen_ascii(info->name).c_str(),
              info->shortcut != 0 ? static_cast<wchar_t>(info->shortcut - 'a' + 'A') : L' ');
    AppendMenuW(tool_menu, MF_STRING, kCmdToolBase + static_cast<UINT>(i), label);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tool_menu), L"工具(&T)");

  HMENU adjust_menu = CreatePopupMenu();
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjKindPrev, L"上一类型(&P)\tCtrl+Shift+J");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjKindNext, L"下一类型(&N)\tCtrl+Shift+K");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRowPrev, L"上一调整(&E)\t,");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRowNext, L"下一调整(&X)\t.");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjParamPrev, L"上一参数(&R)\tShift+Tab");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjParamNext, L"下一参数(&T)\tTab");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjValueDown, L"参数减(&-)\t-");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjValueUp, L"参数加(&+)\t=");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjAdd, L"新增调整(&A)\tCtrl+Shift+A");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjApply, L"应用到选中调整(&U)\tCtrl+Shift+U");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRemove, L"删除选中调整(&D)\tCtrl+Shift+D");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(adjust_menu), L"调整(&J)");

  /* 滤镜菜单与面板参数表共用 ui_core.h 的 filter_table（量程与 comp_filter.h 对齐）。 */
  HMENU filter_menu = CreatePopupMenu();
  /* P1-5：Camera Raw 是图层级破坏性滤镜，位置与 PS 一致放在滤镜菜单最上面。 */
  AppendMenuW(filter_menu, MF_STRING, kCmdCameraRaw, L"Camera Raw 滤镜…(&C)\tCtrl+Alt+R");
  AppendMenuW(filter_menu, MF_SEPARATOR, 0, nullptr);
  for (int i = 0; i < comp_ui::filter_count(); ++i) {
    const comp_ui::FilterSpec* spec = comp_ui::filter_spec_at(i);
    if (spec == nullptr) continue;
    wchar_t label[128];
    wsprintfW(label, L"%s", widen_ascii(spec->name).c_str());
    AppendMenuW(filter_menu, MF_STRING, kCmdFilterBase + static_cast<UINT>(i), label);
  }
  AppendMenuW(filter_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterApply, L"应用(&A)\t回车");
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterReset, L"重置参数(&R)");
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterClose, L"关闭面板(&C)\tEsc");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(filter_menu), L"滤镜(&F)");

  return menu;
}

/* ------------------------------------------------------------ 无窗口自检 */

const char* backend_name(comp_backend backend) {
  switch (backend) {
    case COMP_BACKEND_METAL:  return "METAL";
    case COMP_BACKEND_D3D12:  return "D3D12";
    case COMP_BACKEND_VULKAN: return "VULKAN";
    case COMP_BACKEND_CPU:    return "CPU";
    default:                  return "AUTO";
  }
}

}  // namespace comp_shell
