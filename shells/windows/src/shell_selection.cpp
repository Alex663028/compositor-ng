// AI生成
/*
 * shell_selection.cpp — 选区、套索、魔棒、裁剪、颜色范围、羽化与修改
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {



/*
 * 从引擎取完整选区状态：active、有效 coverage（含羽化）、边界线段。
 * 无选区时清空全部缓存——「上一次的蚂蚁线」绝不能留在屏幕上。
 *
 * coverage 是文档分辨率的 8 位灰度图；1500 万像素的文档约 15 MB 副本，
 * 只在选区/文档变化时取一次（不随每帧绘制重取）。
 */
void refresh_selection(ShellState& shell) {
  shell.doc.selection_active = false;
  shell.doc.selection_mask.clear();
  shell.doc.selection_segments.clear();
  shell.doc.selection_truncated = false;
  shell.doc.selection_base = comp_ui::Rect{};
  if (shell.doc.document == nullptr || shell.doc.width <= 0 || shell.doc.height <= 0) return;

  int32_t active = 0;
  if (comp_selection_get_active(shell.doc.document, &active) != COMP_OK || active == 0) return;

  const std::size_t pixel_count = static_cast<std::size_t>(shell.doc.width) *
                                  static_cast<std::size_t>(shell.doc.height);
  shell.doc.selection_mask.assign(pixel_count, 0);
  comp_mask mask{};
  mask.width = shell.doc.width;
  mask.height = shell.doc.height;
  mask.stride = static_cast<std::size_t>(shell.doc.width);
  mask.pixels = shell.doc.selection_mask.data();
  if (comp_selection_get_coverage(shell.doc.document, &mask) != COMP_OK) {
    shell.doc.selection_mask.clear();
    return;
  }
  shell.doc.selection_active = true;

  /*
   * §5.44 手柄几何：基础蒙版的**紧**包围盒。取不到就留空——空包围盒不会画
   * 手柄，也不会被命中，比用 coverage 反推一个「大概的」框诚实。
   * （显式空选区的紧包围盒是 {0,0,0,0}，同样不画手柄：没有可拖的边。）
   */
  comp_rect base{};
  if (comp_selection_get_base_bounds(shell.doc.document, &base) == COMP_OK) {
    shell.doc.selection_base = comp_ui::Rect{base.x, base.y, base.w, base.h};
  }

  shell.doc.selection_segments.resize(kMaxAntSegments);
  const int needed = comp_ui::selection_boundary_segments(
      shell.doc.selection_mask.data(), shell.doc.width, shell.doc.height,
      static_cast<std::size_t>(shell.doc.width), 128, shell.doc.selection_segments.data(),
      kMaxAntSegments);
  shell.doc.selection_truncated = needed > kMaxAntSegments;
  if (!shell.doc.selection_truncated) {
    shell.doc.selection_segments.resize(static_cast<std::size_t>(needed));
  }
}

/* 选区命令的共同尾部：刷新缓存 + 撤销深度 + 截断提示（如有）。 */
void finish_selection_command(ShellState& shell, const wchar_t* prefix) {
  refresh_selection(shell);
  wchar_t buffer[192];
  if (shell.doc.selection_truncated) {
    wsprintfW(buffer, L"%s（选区边界过多，蚂蚁线截断显示）", prefix);
    refresh_history_status(shell, buffer);
  } else {
    refresh_history_status(shell, prefix);
  }
}

void select_all(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_select_all(shell.doc.document, &changed) != COMP_OK) {
    set_status(shell, L"全选失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已全选（Ctrl+D 取消）" : L"已全选：选区未变化");
}

void deselect_all(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_deselect(shell.doc.document, &changed) != COMP_OK) {
    set_status(shell, L"取消选择失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已取消选择" : L"取消选择：本来就没有选区");
}

void invert_selection(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_invert(shell.doc.document, &changed) != COMP_OK) {
    set_status(shell, L"反选失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已反选" : L"反选：无选区，未变化");
}

/* ------------------------------------------------------- 选区工具共同尾部（S7） */

/*
 * 形状 / 套索 / 魔棒三个入口的共同尾部：一次选区操作只落**一个**撤销步。
 *
 * 引擎的 apply_shape / apply_wand 各自会自成一个隐式事务，而「新选区 + 羽化」是
 * 两次 ABI 调用；因此这里开显式事务把它们并成一步（隐式事务会加入已开启的显式
 * 事务，不会嵌套）。选区没有变化时回滚，避免空撤销步。
 */
void finish_new_selection(ShellState& shell, const char* label, const wchar_t* ok_text,
                          const wchar_t* noop_text, const wchar_t* fail_text,
                          const std::function<comp_result(int32_t*)>& apply,
                          bool* applied) {
  if (applied != nullptr) *applied = false;
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, label, &txn) != COMP_OK) {
    set_status(shell, L"选区：无法开始事务");
    return;
  }
  int32_t changed = 0;
  comp_result result = apply(&changed);
  /* 新选区羽化只作用于**本次真正新建**的选区：形状/路径/魔棒把 feather 归零后
   * 再用 comp_selection_set_feather 按绝对值设一次（S8 新增的覆盖语义入口）。
   * changed == 0 表示这次点击什么都没选中（例如魔棒没匹配），此时绝不能去动
   * 用户已有选区的羽化——那会把「点空处」变成一次羽化修改。 */
  if (result == COMP_OK && changed != 0 && shell.selection_feather > 0) {
    int32_t feathered = 0;
    result = comp_selection_set_feather(shell.doc.document, shell.selection_feather, &feathered);
    if (result == COMP_OK && feathered != 0) changed = 1;
  }
  if (result != COMP_OK) {
    /* 失败必须整体回滚：半成品选区比不动更坏。 */
    comp_txn_abort(txn);
    set_status(shell, fail_text);
    return;
  }
  if (changed == 0) {
    comp_txn_abort(txn);
    set_status(shell, noop_text);
    return;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"选区：提交失败");
    return;
  }
  if (applied != nullptr) *applied = true;
  finish_selection_command(shell, ok_text);
}

/* --------------------------------------------------------------- 羽化（S7） */

void refresh_feather_input_status(ShellState& shell) {
  wchar_t buffer[192];
  wsprintfW(buffer, L"新选区羽化：%s px（回车应用 / Esc 取消，0..%d）",
            shell.doc.feather_input.c_str(), comp_ui::kMaxSelectionFeather);
  set_status(shell, buffer);
}

void begin_feather_input(ShellState& shell) {
  shell.doc.feather_input.clear();
  shell.doc.feather_input_active = true;
  wchar_t buffer[192];
  wsprintfW(buffer, L"新选区羽化：键入 0..%d 后回车（当前 %d px）",
            comp_ui::kMaxSelectionFeather, shell.selection_feather);
  set_status(shell, buffer);
}

/*
 * 回车：写进「新选区羽化」默认值。若已有选区，用 S8 的
 * comp_selection_set_feather 按**绝对值覆盖**当前选区的羽化（不是叠加）——
 * 与新选区提交时同一口径：键入几次都只会得到键入的那个半径，0 即清除羽化。
 */
void commit_feather_input(ShellState& shell) {
  const int value =
      comp_ui::clamp_selection_feather(_wtoi(shell.doc.feather_input.c_str()));
  shell.doc.feather_input.clear();
  shell.doc.feather_input_active = false;
  shell.selection_feather = value;

  wchar_t buffer[224];
  if (shell.doc.document == nullptr || !shell.doc.selection_active) {
    wsprintfW(buffer, L"新选区羽化：%d px（对之后新建的选区生效）", value);
    set_status(shell, buffer);
    return;
  }
  int32_t changed = 0;
  if (comp_selection_set_feather(shell.doc.document, value, &changed) != COMP_OK) {
    set_status(shell, L"羽化失败：当前选区不支持羽化");
    return;
  }
  refresh_selection(shell);
  if (changed == 0) {
    wsprintfW(buffer, L"当前选区羽化已是 %d px（无变化）", value);
    refresh_history_status(shell, buffer);
    return;
  }
  wsprintfW(buffer, L"已把当前选区羽化设为 %d px（绝对值）", value);
  refresh_history_status(shell, buffer);
}

/* --------------------------------------------------- 扩展 / 收缩（P0-4） */

/*
 * 扩展/收缩/边界/平滑共用一套键入态：菜单点哪条只决定 modify_op，半径的键入/
 * 退格/回车/Esc 语义完全一致（与羽化同款），所以不做四份状态机。量程按 op 分：
 * 扩展/收缩/边界都是 1..500（引擎 kMaxSelectionGrow），平滑 1..100。
 */
const wchar_t* modify_op_name(comp_selection_modify_op op) {
  switch (op) {
    case COMP_SELECTION_CONTRACT:
      return L"收缩";
    case COMP_SELECTION_BORDER:
      return L"边界";
    case COMP_SELECTION_SMOOTH:
      return L"平滑";
    case COMP_SELECTION_EXPAND:
    default:
      return L"扩展";
  }
}

int modify_op_min(comp_selection_modify_op op) {
  return op == COMP_SELECTION_SMOOTH ? comp_ui::kMinSelectionSmooth : comp_ui::kMinSelectionGrow;
}

int modify_op_max(comp_selection_modify_op op) {
  return op == COMP_SELECTION_SMOOTH ? comp_ui::kMaxSelectionSmooth : comp_ui::kMaxSelectionGrow;
}

int modify_op_clamp(comp_selection_modify_op op, int value) {
  return op == COMP_SELECTION_SMOOTH ? comp_ui::clamp_selection_smooth(value)
                                     : comp_ui::clamp_selection_grow(value);
}

void refresh_modify_input_status(ShellState& shell) {
  wchar_t buffer[192];
  wsprintfW(buffer, L"选区%s：%s px（回车应用 / Esc 取消，%d..%d）",
            modify_op_name(shell.doc.modify_op), shell.doc.modify_input.c_str(),
            modify_op_min(shell.doc.modify_op), modify_op_max(shell.doc.modify_op));
  set_status(shell, buffer);
}

void begin_modify_input(ShellState& shell, comp_selection_modify_op op) {
  /* 重开一次 = 丢掉上一轮的半截输入（鼠标还能在键入态里点菜单）。 */
  shell.doc.modify_input.clear();
  shell.doc.modify_input_active = false;
  if (shell.doc.document == nullptr) {
    set_status(shell, L"选区：还没有文档");
    return;
  }
  /* 空选区扩展没有意义（引擎会如实返回无变化），先在入口拦下来。 */
  if (!shell.doc.selection_active) {
    set_status(shell, L"选区：先建立一个选区，再扩展/收缩/边界/平滑");
    return;
  }
  shell.doc.modify_op = op;
  shell.doc.modify_input_active = true;
  const int previous = modify_op_clamp(op, shell.selection_grow);
  wchar_t buffer[192];
  wsprintfW(buffer, L"选区%s：键入 %d..%d px 后回车（上次 %d px）",
            modify_op_name(op), modify_op_min(op), modify_op_max(op), previous);
  set_status(shell, buffer);
}

void commit_modify_input(ShellState& shell) {
  const int typed = _wtoi(shell.doc.modify_input.c_str());
  shell.doc.modify_input.clear();
  shell.doc.modify_input_active = false;
  /* 0 是「用户还没键入」的常见残留，按下限 1 处理会有 1px 的意外扩展，
   * 所以 0 先拦成提示，不写进画布也不占撤销步。 */
  if (typed <= 0) {
    set_status(shell, L"选区：半径必须 ≥ 1 px（未改动）");
    return;
  }
  if (shell.doc.document == nullptr) {
    set_status(shell, L"选区：还没有文档");
    return;
  }
  if (!shell.doc.selection_active) {
    set_status(shell, L"选区：当前没有选区可改");
    return;
  }
  const int value = modify_op_clamp(shell.doc.modify_op, typed);
  shell.selection_grow = value;

  int32_t changed = 0;
  if (comp_selection_modify(shell.doc.document, shell.doc.modify_op, value, &changed) != COMP_OK) {
    set_status(shell, L"选区修改失败（半径超出引擎量程？）");
    return;
  }

  wchar_t buffer[224];
  if (changed == 0) {
    /* changed = 0 只表示引擎没动蒙版（例如收缩把选区收空了不在此列——
     * 空选区也算一次真实变化）。 */
    wsprintfW(buffer, L"选区%s %d px：无变化", modify_op_name(shell.doc.modify_op), value);
  } else {
    wsprintfW(buffer, L"已将选区%s %d px（撤销一步）", modify_op_name(shell.doc.modify_op), value);
  }
  finish_selection_command(shell, buffer);
}

/*
 * 扩大选取 / 选取相似（P0-5 收尾）：容差直接用壳上的魔棒容差（shift+/ 可调），
 * 两条命令都是「点菜单即落墨、一步撤销」。similar=true 走不连续的颜色匹配。
 */
void grow_similar_selection(ShellState& shell, bool similar) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"选区：还没有文档");
    return;
  }
  if (!shell.doc.selection_active) {
    set_status(shell, L"选区：先建立一个选区，再扩大选取/选取相似");
    return;
  }
  const int tolerance = shell.wand_tolerance;
  int32_t changed = 0;
  const comp_result result =
      similar ? comp_selection_similar(shell.doc.document, tolerance, &changed)
              : comp_selection_grow(shell.doc.document, tolerance, &changed);
  if (result != COMP_OK) {
    set_status(shell, similar ? L"选取相似失败（合成不可用？）" : L"扩大选取失败（合成不可用？）");
    return;
  }

  wchar_t buffer[224];
  if (changed == 0) {
    wsprintfW(buffer, L"%s（容差 %d）：无变化", similar ? L"选取相似" : L"扩大选取", tolerance);
  } else {
    wsprintfW(buffer, L"已%s（容差 %d，撤销一步）", similar ? L"选取相似" : L"扩大选取", tolerance);
  }
  finish_selection_command(shell, buffer);
}

/*
 * 按图层像素载入选区（P0-4）：threshold 是 alpha 下限，取 1 = 「任何有像素的
 * 地方都算选中」。取 0 会把全透明像素也算进去（等于全选），不是「按内容载入」
 * 的本意；面板将来给阈值时再放出来。
 */
void load_layer_pixels_selection(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"选区：还没有文档");
    return;
  }
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"选区：先选中一个图层");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_load_layer(shell.doc.document, row->id, COMP_SELECTION_LOAD_PIXELS,
                                COMP_SELECTION_REPLACE, 1, &changed) != COMP_OK) {
    set_status(shell, L"按图层像素载入选区失败");
    return;
  }
  if (changed == 0) {
    refresh_selection(shell);
    refresh_history_status(shell, L"按图层像素载入选区：该层没有不透明像素");
    return;
  }
  const std::wstring name = widen_utf8(row->name);
  wchar_t buffer[224];
  wsprintfW(buffer, L"已按图层「%s」的像素载入选区", name.c_str());
  finish_selection_command(shell, buffer);
}

/* --------------------------------------------------- 色彩范围（P0-4 第二批） */

/*
 * 上游 `ColorRangeSheet` 的壳内等价物（Select > Color Range）：
 *   - 打开面板 = `comp_txn_begin`，事务快照停在「打开前」；
 *   - 每次取色 / 改容差 / 切反相都在事务里重算**整个**选区（REPLACE），
 *     所以预览本身不产生撤销步，松手也不会有几十步垃圾；
 *   - 回车提交 = 一步撤销；Esc 或从没取过色 = 事务 abort，选区回到打开前。
 * 与魔棒的本质区别：色彩范围匹配**整篇可见合成**里所有同色像素，不要求连通。
 */

/* 状态栏共用文案：颜色数用 include.size()/3 换算（ABI 按颜色数计数，不是字节数）。 */
void color_range_status(ShellState& shell, const wchar_t* prefix) {
  wchar_t buffer[288];
  wsprintfW(buffer, L"%s：取色 %d / 排除 %d · 容差 %d · 反相 %s",
            prefix, static_cast<int>(shell.doc.color_range_include.size() / 3),
            static_cast<int>(shell.doc.color_range_exclude.size() / 3),
            shell.doc.color_range_fuzziness, shell.doc.color_range_invert ? L"是" : L"否");
  set_status(shell, buffer);
}

/* 事务重开：没取过色时上游「恢复原选区」，abort 正好把我们送回那个快照。 */
void color_range_restart_txn(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    shell.doc.color_range_txn = nullptr;
    return;
  }
  if (shell.doc.color_range_txn != nullptr) {
    comp_txn_abort(shell.doc.color_range_txn);
    shell.doc.color_range_txn = nullptr;
  }
  if (comp_txn_begin(shell.doc.document, "Color Range", &shell.doc.color_range_txn) != COMP_OK) {
    shell.doc.color_range_txn = nullptr;
  }
  refresh_selection(shell);
  shell.doc.needs_render = true;
}

void color_range_begin(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"色彩范围：还没有文档");
    return;
  }
  if (shell.doc.color_range_open) {
    /* 再点一次菜单 = 回到打开前的选区再重开，避免两个事务叠在同一个文档上。 */
    if (shell.doc.color_range_txn != nullptr) comp_txn_abort(shell.doc.color_range_txn);
    shell.doc.color_range_txn = nullptr;
    shell.doc.color_range_open = false;
  }
  /* 右栏一次只挂一个面板（与滤镜/效果/路径/历史互斥，避免两块矩形互相遮住）。 */
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.path_open = false;
  shell.doc.history_open = false;
  channel_panel_close(shell);
  shell.doc.color_range_include.clear();
  shell.doc.color_range_exclude.clear();
  shell.doc.color_range_fuzziness = comp_ui::kDefaultColorRangeFuzziness;
  shell.doc.color_range_invert = 0;
  shell.doc.color_range_input.clear();
  shell.doc.color_range_open = true;
  if (comp_txn_begin(shell.doc.document, "Color Range", &shell.doc.color_range_txn) != COMP_OK) {
    shell.doc.color_range_txn = nullptr;
    /* 没有事务就没有「预览后回滚」，不如直接不开面板，别让用户改一堆无效参数。 */
    shell.doc.color_range_open = false;
    set_status(shell, L"色彩范围：无法开启事务（未改动）");
    return;
  }
  set_status(shell,
             L"色彩范围：点画布取色（Shift 加色 · Alt 减色）· -/= 容差 · I 反相 · "
             L"回车应用 · Esc 取消");
}

void color_range_cancel(ShellState& shell) {
  const comp_txn txn = shell.doc.color_range_txn;
  shell.doc.color_range_txn = nullptr;
  shell.doc.color_range_open = false;
  shell.doc.color_range_input.clear();
  if (txn != nullptr) comp_txn_abort(txn);
  refresh_selection(shell);
  shell.doc.needs_render = true;
  set_status(shell, L"已取消色彩范围（选区回到打开前）");
}

void color_range_commit(ShellState& shell) {
  const comp_txn txn = shell.doc.color_range_txn;
  const bool has_colors = !shell.doc.color_range_include.empty();
  shell.doc.color_range_txn = nullptr;
  shell.doc.color_range_open = false;
  shell.doc.color_range_input.clear();
  bool committed = false;
  if (txn != nullptr) {
    if (!has_colors) {
      /* 没取过色 = 上游的「什么都不选」：回滚，不留一步空撤销。 */
      comp_txn_abort(txn);
    } else {
      committed = comp_txn_commit(txn) == COMP_OK;
    }
  }
  refresh_selection(shell);
  shell.doc.needs_render = true;
  if (!has_colors) {
    set_status(shell, L"色彩范围：还没取色（选区未改动）");
    return;
  }
  wchar_t buffer[288];
  wsprintfW(buffer, L"色彩范围：已应用（取色 %d / 排除 %d · 容差 %d%s）",
            static_cast<int>(shell.doc.color_range_include.size() / 3),
            static_cast<int>(shell.doc.color_range_exclude.size() / 3),
            shell.doc.color_range_fuzziness, shell.doc.color_range_invert ? L" · 反相" : L"");
  refresh_history_status(shell, committed ? buffer : L"色彩范围：提交失败（已回滚）");
}

/*
 * 重算预览选区。没有 include 颜色时上游回到原选区（不是全不选），这里用
 * 「事务重开」实现：abort 回快照 + 重新 begin，快照随之变成当前（= 打开前）状态。
 * 引擎对 0 匹配的定义是「不改选区」，而上游面板此时显示空选区，所以显式 deselect。
 */
void color_range_refresh(ShellState& shell) {
  if (shell.doc.document == nullptr || !shell.doc.color_range_open) return;
  if (!shell.doc.color_range_input.empty()) return; /* 键入态还没落值，不预览 */
  if (shell.doc.color_range_include.empty()) {
    color_range_restart_txn(shell);
    return;
  }
  if (shell.doc.color_range_txn == nullptr) return;

  int32_t changed = 0;
  const comp_result result = comp_selection_apply_color_range(
      shell.doc.document,
      shell.doc.color_range_include.empty() ? nullptr : shell.doc.color_range_include.data(),
      shell.doc.color_range_include.size() / 3,
      shell.doc.color_range_exclude.empty() ? nullptr : shell.doc.color_range_exclude.data(),
      shell.doc.color_range_exclude.size() / 3, shell.doc.color_range_fuzziness, shell.doc.color_range_invert,
      COMP_SELECTION_REPLACE, &changed);
  if (result != COMP_OK) {
    set_status(shell, L"色彩范围：计算失败（参数被引擎拒绝）");
    return;
  }
  if (changed == 0) {
    int32_t deselected = 0;
    comp_selection_deselect(shell.doc.document, &deselected);
  }
  refresh_selection(shell);
  shell.doc.needs_render = true;
}

/* 取色：合并源 + 3×3 均值（上游 color(in:at:) 的口径），坐标落在画布内才取。 */
void color_range_sample(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.color_range_open || shell.doc.document == nullptr) return;
  if (!layout.canvas.contains(x, y)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  if (doc.x < 0.0 || doc.y < 0.0 || doc.x >= static_cast<double>(shell.doc.width) ||
      doc.y >= static_cast<double>(shell.doc.height)) {
    set_status(shell, L"色彩范围：点在画布外，没有取样");
    return;
  }
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (comp_eyedropper_sample(shell.doc.document, 0, doc.x, doc.y, /*sample_size=*/3,
                             COMP_EYEDROPPER_MERGED, rgba) != COMP_OK) {
    set_status(shell, L"色彩范围：取样失败（合成不可用）");
    return;
  }
  const comp_ui::ColorRGB color = comp_ui::color_from_unit(rgba[0], rgba[1], rgba[2]);
  const uint8_t triple[3] = {color.r, color.g, color.b};
  const bool remove = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool add = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const wchar_t* action = L"取色";
  if (remove) {
    shell.doc.color_range_exclude.insert(shell.doc.color_range_exclude.end(), triple, triple + 3);
    action = L"排除";
  } else if (add) {
    shell.doc.color_range_include.insert(shell.doc.color_range_include.end(), triple, triple + 3);
    action = L"加色";
  } else {
    /* replace：与上游 sampleMode = .replace 一致，重开取色并把排除清空。 */
    shell.doc.color_range_include.assign(triple, triple + 3);
    shell.doc.color_range_exclude.clear();
  }
  color_range_refresh(shell);

  char hex[8] = {0};
  comp_ui::color_to_hex(color, hex);
  wchar_t buffer[288];
  wsprintfW(buffer, L"色彩范围：%s %s（取色 %d / 排除 %d · 容差 %d%s）", action,
            widen_ascii(hex).c_str(), static_cast<int>(shell.doc.color_range_include.size() / 3),
            static_cast<int>(shell.doc.color_range_exclude.size() / 3),
            shell.doc.color_range_fuzziness, shell.doc.color_range_invert ? L" · 反相" : L"");
  set_status(shell, buffer);
}

/* 点掉一个色块：取色行删 include，排除行删 exclude；删空即回到打开前的选区。 */
void color_range_remove_swatch(ShellState& shell, int slot) {
  const int include_count = static_cast<int>(shell.doc.color_range_include.size() / 3);
  if (slot < 0) return;
  if (slot < include_count) {
    auto& list = shell.doc.color_range_include;
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(slot) * 3,
               list.begin() + static_cast<std::ptrdiff_t>(slot) * 3 + 3);
  } else {
    const int index = slot - include_count;
    auto& list = shell.doc.color_range_exclude;
    if (index * 3 + 3 > static_cast<int>(list.size())) return;
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index) * 3,
               list.begin() + static_cast<std::ptrdiff_t>(index) * 3 + 3);
  }
  color_range_refresh(shell);
  color_range_status(shell, L"色彩范围：已移除一个取样色");
}

void color_range_step_fuzziness(ShellState& shell, int direction) {
  shell.doc.color_range_fuzziness =
      comp_ui::step_color_range_fuzziness(shell.doc.color_range_fuzziness, direction);
  color_range_refresh(shell);
  wchar_t buffer[240];
  wsprintfW(buffer, L"色彩范围：容差 %d（%d..%d）%s", shell.doc.color_range_fuzziness,
            comp_ui::kMinColorRangeFuzziness, comp_ui::kMaxColorRangeFuzziness,
            shell.doc.color_range_txn != nullptr ? L"· 实时预览，回车应用" : L"");
  set_status(shell, buffer);
}

void color_range_toggle_invert(ShellState& shell) {
  shell.doc.color_range_invert = shell.doc.color_range_invert ? 0 : 1;
  color_range_refresh(shell);
  color_range_status(shell, shell.doc.color_range_invert ? L"色彩范围：反相开" : L"色彩范围：反相关");
}

void color_range_clear_colors(ShellState& shell) {
  shell.doc.color_range_include.clear();
  shell.doc.color_range_exclude.clear();
  color_range_refresh(shell);
  set_status(shell, L"色彩范围：已清空取样色（选区回到打开前）");
}

void color_range_begin_input(ShellState& shell, wchar_t character) {
  if (shell.doc.color_range_input.size() >= 3) return;
  shell.doc.color_range_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.doc.color_range_input.c_str());
  set_status(shell, buffer);
}

/* 键入值夹到 0..200 再落值：面板永不产生越界参数（ABI 仍会拒绝越界）。 */
void color_range_commit_input(ShellState& shell) {
  if (shell.doc.color_range_input.empty()) return;
  const std::wstring text = shell.doc.color_range_input;
  shell.doc.color_range_input.clear();
  int value = 0;
  for (size_t i = 0; i < text.size(); ++i) value = value * 10 + static_cast<int>(text[i] - L'0');
  shell.doc.color_range_fuzziness = comp_ui::clamp_color_range_fuzziness(value);
  color_range_refresh(shell);
  wchar_t buffer[200];
  wsprintfW(buffer, L"色彩范围：容差 = %d", shell.doc.color_range_fuzziness);
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 套索（S7） */

bool lasso_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Lasso;
}

void lasso_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!lasso_available(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.lasso_active = true;
  shell.doc.lasso_points.clear();
  shell.doc.lasso_points.push_back(static_cast<float>(doc.x));
  shell.doc.lasso_points.push_back(static_cast<float>(doc.y));
  SetCapture(window);
  set_status(shell, L"套索：按住拖出自由选区，松开闭合成选区（Shift 加选 / Alt 减选）");
}

void lasso_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.lasso_active) return;
  const int point_count = static_cast<int>(shell.doc.lasso_points.size() / 2);
  if (!comp_ui::lasso_can_append(point_count)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  const size_t count = shell.doc.lasso_points.size();
  const double last_x = count >= 2 ? shell.doc.lasso_points[count - 2] : doc.x;
  const double last_y = count >= 2 ? shell.doc.lasso_points[count - 1] : doc.y;
  if (!comp_ui::stroke_point_should_append(last_x, last_y, doc.x, doc.y)) return;
  shell.doc.lasso_points.push_back(static_cast<float>(doc.x));
  shell.doc.lasso_points.push_back(static_cast<float>(doc.y));
}

void lasso_cancel(ShellState& shell) {
  shell.doc.lasso_active = false;
  shell.doc.lasso_points.clear();
}

void lasso_end(HWND window, ShellState& shell) {
  if (!shell.doc.lasso_active) return;
  shell.doc.lasso_active = false;

  /* 同画笔：ReleaseCapture 同步触发 WM_CAPTURECHANGED -> lasso_cancel 清点。 */
  std::vector<float> points;
  points.swap(shell.doc.lasso_points);
  ReleaseCapture();
  if (shell.doc.document == nullptr) return;

  const int point_count = static_cast<int>(points.size() / 2);
  if (!comp_ui::lasso_should_close(point_count)) {
    set_status(shell, L"套索：至少需要拖出 3 个点才能闭合成选区");
    return;
  }

  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }

  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"套索：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"套索：已减选"
                                                                : L"套索：已应用";
  finish_new_selection(
      shell, "Lasso Selection", ok_text, L"套索：选区未变化", L"套索失败：多边形无效",
      [&](int32_t* changed) {
        return comp_selection_apply_shape(shell.doc.document, COMP_SELECTION_POLYGON, points.data(),
                                          point_count, combine, /*antialias=*/1, changed);
      });
}

/* ------------------------------------------------------- 选区手柄（§5.44） */

/*
 * 手柄只在「用户正在做选区」的工具下出现：Marquee / Lasso / Wand / Move。
 * 画笔类的落点含义是画图，画上去的手柄会抢命中、也会误导（PS 同规矩）。
 * 拖动中或其它工具的临时态里一律不出现，避免两套拖拽语义打架。
 */
bool selection_handles_available(const ShellState& shell) {
  if (shell.doc.document == nullptr || !shell.doc.selection_active) return false;
  if (shell.doc.selection_base.empty()) return false;
  if (shell.doc.marquee_active || shell.doc.lasso_active || shell.doc.crop_active ||
      shell.doc.pen_dragging || shell.doc.zoom_marquee_active) {
    return false;
  }
  switch (shell.tool) {
    case comp_ui::ToolId::Marquee:
    case comp_ui::ToolId::Lasso:
    case comp_ui::ToolId::Wand:
    case comp_ui::ToolId::Move:
      return true;
    default:
      return false;
  }
}

/*
 * 手柄锚点的**屏幕**坐标。手柄是屏幕尺寸（不随缩放变），所以命中与绘制都
 * 走同一份屏幕点：两边各算一遍必然分叉，这是本仓反复踩过的坑。
 */
void selection_handle_centers(const ShellState& shell, const Layout& layout,
                              const comp_ui::Rect& doc_rect, comp_ui::Point* out) {
  for (int i = 0; i < comp_ui::kSelectionHandleCount; ++i) {
    const comp_ui::Point doc = comp_ui::selection_handle_point(doc_rect, i);
    out[i] = shell.doc.view.doc_to_screen(doc.x, doc.y, layout.canvas, shell.doc.width, shell.doc.height);
  }
}

/*
 * 按在手柄上 = 开始扩缩。返回 true 表示「这次点击已经是手柄的」（无论成败），
 * 调用方据此**不要**再退化成框选/整层拖动——与 layer_handle_begin 同一约定。
 */
bool selection_handle_begin(HWND window, ShellState& shell, int x, int y,
                            const Layout& layout) {
  if (!selection_handles_available(shell)) return false;
  comp_ui::Point centers[comp_ui::kSelectionHandleCount];
  selection_handle_centers(shell, layout, shell.doc.selection_base, centers);
  const int hit = comp_ui::selection_handle_hit(
      centers, comp_ui::kSelectionHandleCount,
      comp_ui::Point{static_cast<double>(x), static_cast<double>(y)}, kSelectionHandleHitPx);
  if (hit < 0) return false;
  shell.doc.selection_handle_drag = hit;
  shell.doc.selection_handle_source = shell.doc.selection_base;
  shell.doc.selection_handle_current = shell.doc.selection_base;
  SetCapture(window);
  set_status(shell, L"选区手柄：拖动缩放选区（Shift 拖角 = 等比，Esc 取消）");
  return true;
}

void selection_handle_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (shell.doc.selection_handle_drag < 0) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  const bool lock_aspect = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  shell.doc.selection_handle_current = comp_ui::selection_drag_rect(
      shell.doc.selection_handle_source, shell.doc.selection_handle_drag, doc, lock_aspect);
}

/*
 * 取消：拖动阶段引擎**从未被写入**（松手才提交），所以这里只是丢掉预览。
 * 不 ReleaseCapture —— 与 marquee_cancel 等同一写法，捕获由调用点负责。
 */
void selection_handle_cancel(ShellState& shell) {
  shell.doc.selection_handle_drag = -1;
  shell.doc.selection_handle_source = comp_ui::Rect{};
  shell.doc.selection_handle_current = comp_ui::Rect{};
}

void selection_handle_end(HWND window, ShellState& shell) {
  if (shell.doc.selection_handle_drag < 0) return;
  /* 先落状态再放捕获：ReleaseCapture 会同步派发 WM_CAPTURECHANGED -> cancel。 */
  const comp_ui::Rect target = shell.doc.selection_handle_current;
  const comp_ui::Rect source = shell.doc.selection_handle_source;
  selection_handle_cancel(shell);
  ReleaseCapture();
  if (shell.doc.document == nullptr) return;

  /* 手柄没动（含只抖了一像素又拖回来）：不写引擎，也就不产生撤销步。 */
  if (target.x == source.x && target.y == source.y && target.w == source.w &&
      target.h == source.h) {
    set_status(shell, L"选区手柄：选区未变化");
    return;
  }

  comp_rect bounds{};
  bounds.x = target.x;
  bounds.y = target.y;
  bounds.w = target.w;
  bounds.h = target.h;
  int32_t changed = 0;
  const comp_result result = comp_selection_set_bounds(shell.doc.document, bounds, &changed);
  if (result != COMP_OK) {
    set_status(shell, L"选区手柄：缩放失败（选区状态异常）");
    return;
  }
  refresh_selection(shell);
  if (changed != 0) {
    wchar_t buffer[192];
    wsprintfW(buffer, L"选区手柄：已缩放至 %d×%d 像素", target.w, target.h);
    finish_selection_command(shell, buffer);
  } else {
    set_status(shell, L"选区手柄：选区未变化");
  }
}

/*
 * 手柄方向 -> 系统缩放光标。角用斜双箭头（NWSE / NESW），边用单轴箭头；
 * 与 PS 同一套手感，也让「能不能拖」在按下前就能看出来。
 * IDC_* 在本 SDK 里是 char* 宏（MAKEINTRESOURCEA），与 LoadCursorW 的
 * LPCWSTR 不符：按同文件既有写法转换。
 */
HCURSOR selection_handle_cursor(int index) {
  LPCWSTR id = nullptr;
  switch (index) {
    case 0:
    case 4:
      id = reinterpret_cast<LPCWSTR>(IDC_SIZENWSE);
      break;
    case 2:
    case 6:
      id = reinterpret_cast<LPCWSTR>(IDC_SIZENESW);
      break;
    case 1:
    case 5:
      id = reinterpret_cast<LPCWSTR>(IDC_SIZENS);
      break;
    case 3:
    case 7:
      id = reinterpret_cast<LPCWSTR>(IDC_SIZEWE);
      break;
    default:
      return nullptr;
  }
  return LoadCursorW(nullptr, id);
}

/* ------------------------------------------------------------ 魔棒（S7） */
bool wand_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Wand;
}

void wand_click(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!wand_available(shell)) return;
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"魔棒：请先选择一个图层");
    return;
  }
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }
  const comp_selection_wand_source source =
      shell.wand_merged ? COMP_SELECTION_WAND_MERGED : COMP_SELECTION_WAND_LAYER;

  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"魔棒：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"魔棒：已减选"
                                                                : L"魔棒：已应用";
  /* 图层带缩放/旋转/翻转时引擎拒绝 1:1 假设：提示切到合并取样，而不是静默失败。 */
  const wchar_t* fail_text = L"魔棒失败：参数无效或该图层非 1:1 变换（Shift+W 切合并取样）";
  finish_new_selection(shell, "Magic Wand Selection", ok_text, L"魔棒：没有匹配的像素，选区未变",
                       fail_text, [&](int32_t* changed) {
                         return comp_selection_apply_wand(
                             shell.doc.document, id, source, doc.x, doc.y, shell.wand_tolerance,
                             shell.wand_contiguous ? 1 : 0, shell.wand_radius, combine,
                             changed);
                       });
}

/* ------------------------------------------------------------ 框选（Marquee） */

bool marquee_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Marquee;
}

void marquee_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!marquee_available(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.marquee_active = true;
  shell.doc.marquee_start = doc;
  shell.doc.marquee_current = doc;
  SetCapture(window);
  set_status(shell, L"框选：拖出矩形后松开（Shift 加选 / Alt 减选）");
}

void marquee_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.marquee_active) return;
  shell.doc.marquee_current =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
}

void marquee_cancel(ShellState& shell) { shell.doc.marquee_active = false; }

void marquee_end(HWND window, ShellState& shell) {
  if (!shell.doc.marquee_active) return;
  shell.doc.marquee_active = false;
  ReleaseCapture();
  if (shell.doc.document == nullptr) return;

  /* 框选的两个角点（顺序任意，引擎按包围盒解释）：矩形与椭圆都是 2 点语义。 */
  const float points[4] = {static_cast<float>(shell.doc.marquee_start.x),
                           static_cast<float>(shell.doc.marquee_start.y),
                           static_cast<float>(shell.doc.marquee_current.x),
                           static_cast<float>(shell.doc.marquee_current.y)};
  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }
  const comp_selection_shape shape =
      shell.marquee_shape == comp_ui::kMarqueeEllipse ? COMP_SELECTION_ELLIPSE
                                                      : COMP_SELECTION_RECT;
  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"框选：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"框选：已减选"
                                                                : L"框选：已应用";
  finish_new_selection(
      shell, shell.marquee_shape == comp_ui::kMarqueeEllipse ? "Ellipse Marquee Selection"
                                                             : "Rect Marquee Selection",
      ok_text, L"框选：选区未变化", L"框选失败", [&](int32_t* changed) {
        return comp_selection_apply_shape(shell.doc.document, shape, points, 2, combine,
                                          /*antialias=*/1, changed);
      });
}

/* ------------------------------------------------------------ 裁剪（Crop） */

/*
 * 与 Marquee 的区别：Marquee 产出的是**选区**（不动画布），Crop 产出的是**新画布**
 * （动文档尺寸）。两者都拖框，但提交后果完全不同，所以不复用同一套状态。
 *
 * 交互对齐 PS：拖出框 → 框外变暗（三分段遮罩）→ Enter 提交 / Esc 取消。
 * 提交走 comp_crop_apply：它自己负责取整、校验、一步撤销，壳不重复实现这些规则。
 */
bool crop_available(const ShellState& shell) {
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Crop;
}

void crop_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!crop_available(shell)) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.crop_active = true;
  shell.doc.crop_dragging = true;
  shell.doc.crop_drag_start = doc;
  shell.doc.crop_rect.x = doc.x;
  shell.doc.crop_rect.y = doc.y;
  shell.doc.crop_rect.width = 1.0;
  shell.doc.crop_rect.height = 1.0;
  SetCapture(window);
  set_status(shell, L"裁剪：拖出框后 Enter 提交，Esc 取消");
}

void crop_extend(ShellState& shell, int x, int y, const Layout& layout) {
  /* 只有左键还按着才改框；松手后的鼠标移动不参与（否则框会跟着光标跑）。 */
  if (!shell.doc.crop_active || !shell.doc.crop_dragging) return;
  const comp_ui::Point doc =
      shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  /*
   * 每次都从**起点**重新规范化，而不是在上一帧的框上取 min/max：累积式抖动一下
   * 框就永久撑大，收不回来。
   */
  const double left = doc.x < shell.doc.crop_drag_start.x ? doc.x : shell.doc.crop_drag_start.x;
  const double top = doc.y < shell.doc.crop_drag_start.y ? doc.y : shell.doc.crop_drag_start.y;
  const double right = doc.x > shell.doc.crop_drag_start.x ? doc.x : shell.doc.crop_drag_start.x;
  const double bottom = doc.y > shell.doc.crop_drag_start.y ? doc.y : shell.doc.crop_drag_start.y;
  shell.doc.crop_rect.x = left;
  shell.doc.crop_rect.y = top;
  shell.doc.crop_rect.width = right - left;
  shell.doc.crop_rect.height = bottom - top;
}

void crop_cancel(ShellState& shell) {
  shell.doc.crop_active = false;
  shell.doc.crop_dragging = false;
}

/*
 * 提交裁剪。读取引擎给出的 out_changed：为 0 表示框恰好等于当前画布，
 * 这时引擎不产生撤销步，壳也不该把状态栏说成「已裁剪」。
 */
bool crop_commit(ShellState& shell) {
  if (!shell.doc.crop_active || shell.doc.document == nullptr) return false;
  int32_t changed = 0;
  const comp_result result = comp_crop_apply(shell.doc.document, shell.doc.crop_rect, &changed);
  shell.doc.crop_active = false;
  shell.doc.crop_dragging = false;
  if (result != COMP_OK) {
    set_status(shell, L"裁剪失败：框不合法");
    return false;
  }
  if (changed == 0) {
    set_status(shell, L"裁剪未改变画布尺寸");
    return true;
  }
  /* 画布换了坐标系：view 的偏移按新尺寸重算，否则残留的平移会把画布推出可视区。 */
  comp_size size{};
  if (comp_document_size(shell.doc.document, &size) == COMP_OK) {
    shell.doc.width = size.width;
    shell.doc.height = size.height;
  }
  shell.doc.needs_fit = true;
  shell.doc.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已裁剪");
  return true;
}

}  // namespace comp_shell
