// AI生成
/*
 * shell_guides.cpp — 标尺 / 参考线 / 网格 / 吸附（ROI #3 外壳侧）。
 *
 * 分工：
 *   - 参考线是**文档状态**，读写全部走 comp_guide_* 六个导出（同批尾部追加，
 *     152 → 158；壳层本身零新增导出），因此它随 .comp 的 engineGuides 往返、
 *     随撤销/重做回退；
 *   - 本文件只管外壳的三件事：开关、拖动、覆盖层绘制。
 *
 * 三条不变式：
 *   1. 吸附只改**拖动落点**，从不参与像素合成——开/关吸附不会改变任何一张图的
 *      结果，这正是「对齐开关不改变像素」的可验证口径；
 *   2. 一整次拖动 = 一步撤销（按下开显式事务，松手提交）；值没变就 abort，
 *      绝不 commit 空步（Document::txn_commit 无条件进历史）；
 *   3. 拖到标尺上松手 = 删除；本次新建的那条则 abort 回滚，等于「拖出来又拖回去」。
 */
#include "shell_internal.h"

#include <cmath>
#include <cwchar>

#include <compositor/comp_guide.h>

namespace comp_shell {
namespace {

/* 吸附半径（设备像素）：换算成文档像素后与缩放无关地保持同一手感。 */
constexpr double kGuideSnapPx = 8.0;
/* 参考线命中半径（设备像素）：线宽 1px，太细点不中，太粗会吃掉画笔起笔。 */
constexpr double kGuideHitPx = 4.0;
/* 网格间距（文档像素）：排版参考，不随缩放变密度（变密度会让人以为文档变了）。 */
constexpr double kGridStep = 50.0;
/* 每 5 格加粗一条，便于快速数格子。 */
constexpr int kGridMajorEvery = 5;

comp_ui::Point canvas_doc_point(const ShellState& shell, const Layout& layout, int x, int y) {
  return shell.doc.view.screen_to_doc(static_cast<double>(x), static_cast<double>(y),
                                      layout.canvas, shell.doc.width, shell.doc.height);
}

/*
 * 把 raw 吸附到最近的候选（画布两边 + 中线、网格线、其它同轴参考线）。
 * 候选**不进数组**：网格只需要「raw 最近的那一条」，参考线最多 128 条就地扫，
 * 画布尺度再大也不产生每次 mousemove 的分配。
 * 距离相等取先出现的候选（画布边优先于网格，网格优先于已有参考线），
 * 结果因此逐次可复现。返回是否命中；未命中 out_position = raw。
 */
bool snap_position(const ShellState& shell, uint32_t axis, double raw, int32_t skip_index,
                   double* out_position) {
  *out_position = raw;
  if (!shell.doc.snap_enabled) return false;
  if (shell.doc.document == nullptr) return false;
  const double tolerance = comp_ui::snap_tolerance_doc(shell.doc.view.zoom, kGuideSnapPx);
  const double extent = axis == COMP_GUIDE_VERTICAL ? static_cast<double>(shell.doc.width)
                                                    : static_cast<double>(shell.doc.height);
  double best = raw;
  double best_distance = tolerance;
  bool found = false;
  const auto consider = [&](double candidate) {
    if (!(candidate == candidate)) return; /* NaN 候选直接跳过，不带坏结果 */
    const double distance = std::fabs(candidate - raw);
    if (distance > tolerance) return;
    if (found && !(distance < best_distance)) return; /* 相等时保留先出现的 */
    best = candidate;
    best_distance = distance;
    found = true;
  };
  if (extent > 0.0) {
    consider(0.0);
    consider(extent * 0.5);
    consider(extent);
  }
  if (shell.doc.show_grid) {
    /* 网格候选只可能是「最近的那条线」：枚举全部网格线在 8K 文档上要上千次。 */
    const double nearest = std::round(raw / kGridStep) * kGridStep;
    consider(nearest);
  }
  size_t count = 0;
  if (comp_guide_count(shell.doc.document, &count) == COMP_OK) {
    for (size_t i = 0; i < count; ++i) {
      if (static_cast<int32_t>(i) == skip_index) continue;
      comp_guide guide{};
      if (comp_guide_get(shell.doc.document, i, &guide) != COMP_OK) continue;
      if (guide.axis != axis) continue;
      consider(guide.position);
    }
  }
  if (!found) return false;
  *out_position = best;
  return true;
}

/* 指针下最近的参考线（只认可见的参考线：看不见的线不能被拖动）。 */
int32_t guide_at(const ShellState& shell, const Layout& layout, int x, int y) {
  if (shell.doc.document == nullptr || !shell.doc.show_guides) return -1;
  size_t count = 0;
  if (comp_guide_count(shell.doc.document, &count) != COMP_OK) return -1;
  const double tolerance = comp_ui::snap_tolerance_doc(shell.doc.view.zoom, kGuideHitPx);
  int32_t best = -1;
  double best_distance = tolerance;
  for (size_t i = 0; i < count; ++i) {
    comp_guide guide{};
    if (comp_guide_get(shell.doc.document, i, &guide) != COMP_OK) continue;
    const comp_ui::Point probe = shell.doc.view.doc_to_screen(
        guide.axis == COMP_GUIDE_VERTICAL ? guide.position : 0.0,
        guide.axis == COMP_GUIDE_HORIZONTAL ? guide.position : 0.0, layout.canvas,
        shell.doc.width, shell.doc.height);
    const double distance = guide.axis == COMP_GUIDE_VERTICAL
                                ? std::fabs(probe.x - static_cast<double>(x))
                                : std::fabs(probe.y - static_cast<double>(y));
    if (distance > tolerance) continue;
    if (best >= 0 && !(distance < best_distance)) continue;
    best = static_cast<int32_t>(i);
    best_distance = distance;
  }
  return best;
}

}  // namespace

/* ------------------------------------------------------------ 开关 */

void view_toggle_rulers(ShellState& shell) {
  shell.doc.show_rulers = !shell.doc.show_rulers;
  set_status(shell, shell.doc.show_rulers
                        ? L"标尺：显示（从标尺拖出参考线）"
                        : L"标尺：隐藏（参考线仍在，随文档保存）");
}

void view_toggle_guides(ShellState& shell) {
  shell.doc.show_guides = !shell.doc.show_guides;
  if (!shell.doc.show_guides) shell.doc.guide_hover = -1;
  set_status(shell, shell.doc.show_guides ? L"参考线：显示" : L"参考线：隐藏（不影响存盘与像素）");
}

void view_toggle_grid(ShellState& shell) {
  shell.doc.show_grid = !shell.doc.show_grid;
  set_status(shell, shell.doc.show_grid ? L"网格：显示（50 px 一格）" : L"网格：隐藏");
}

void view_toggle_snap(ShellState& shell) {
  shell.doc.snap_enabled = !shell.doc.snap_enabled;
  set_status(shell, shell.doc.snap_enabled
                        ? L"对齐：开（只吸附拖动落点，不改变像素结果）"
                        : L"对齐：关（拖动落点按指针原样取整）");
}

void view_clear_guides(ShellState& shell) {
  if (shell.doc.document == nullptr) return;
  int32_t changed = 0;
  if (comp_guide_clear(shell.doc.document, &changed) != COMP_OK) {
    set_status(shell, L"清除参考线失败（文档不可用）");
    return;
  }
  shell.doc.guide_hover = -1;
  set_status(shell, changed != 0 ? L"已清除全部参考线（一步撤销可还原）" : L"没有参考线可清除");
}

/* ------------------------------------------------------------ 拖动 */

bool guide_drag_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (shell.doc.document == nullptr || layout.canvas.empty()) return false;
  const int ruler = comp_ui::ruler_hit(layout, x, y, shell.doc.show_rulers);
  int32_t index = -1;
  uint32_t axis = COMP_GUIDE_HORIZONTAL;
  if (ruler != 0) {
    /* 上尺拖出水平线（position = y），左尺拖出垂直线（position = x）。 */
    axis = ruler == 1 ? COMP_GUIDE_HORIZONTAL : COMP_GUIDE_VERTICAL;
  } else {
    /*
     * 画布内只有 Move 工具拖得动已有参考线。其它工具在参考线上起笔必须还是
     * 落笔——参考线是常驻可见的，抢占落笔点等于把画布戳出一排洞。
     */
    if (shell.tool != comp_ui::ToolId::Move) return false;
    index = guide_at(shell, layout, x, y);
    if (index < 0) return false;
    comp_guide existing{};
    if (comp_guide_get(shell.doc.document, index, &existing) != COMP_OK) return false;
    axis = existing.axis;
  }

  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, ruler != 0 ? "Add Guide" : "Move Guide", &txn) !=
          COMP_OK ||
      txn == nullptr) {
    return false;
  }
  shell.doc.guide_drag_active = true;
  shell.doc.guide_drag_created = ruler != 0;
  shell.doc.guide_drag_index = index;
  shell.doc.guide_drag_axis = axis;
  shell.doc.guide_drag_drop = 0.0;
  shell.doc.guide_drag_changed = false;
  shell.doc.guide_drag_txn = txn;
  shell.doc.guide_drag_doc = shell.doc.document;
  shell.doc.guide_hover = index;
  SetCapture(window);
  guide_drag_extend(shell, x, y, layout);
  set_status(shell, ruler != 0 ? L"拖动放置参考线（拖回标尺 = 不创建）"
                               : L"拖动参考线（拖到标尺上 = 删除）");
  return true;
}

void guide_drag_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.doc.guide_drag_active || shell.doc.document == nullptr) return;
  if (shell.doc.document != shell.doc.guide_drag_doc) return; /* 换过文档：旧句柄不可写 */
  const comp_ui::Point point = canvas_doc_point(shell, layout, x, y);
  const double raw = shell.doc.guide_drag_axis == COMP_GUIDE_VERTICAL ? point.x : point.y;
  double position = raw;
  snap_position(shell, shell.doc.guide_drag_axis, raw, shell.doc.guide_drag_index, &position);
  shell.doc.guide_drag_drop = position;
  if (shell.doc.guide_drag_index < 0) {
    /* 首次移动才真正创建：按下即拖，松手前松开（取消）不会留下任何参考线。 */
    size_t index = 0;
    if (comp_guide_add(shell.doc.document, shell.doc.guide_drag_axis, position, &index) ==
        COMP_OK) {
      shell.doc.guide_drag_index = static_cast<int32_t>(index);
      shell.doc.guide_drag_changed = true;
    }
  } else {
    int32_t changed = 0;
    if (comp_guide_move(shell.doc.document, shell.doc.guide_drag_index, position, &changed) ==
            COMP_OK &&
        changed != 0) {
      shell.doc.guide_drag_changed = true;
    }
  }
}

void guide_drag_end(HWND window, ShellState& shell, int x, int y) {
  if (!shell.doc.guide_drag_active) return;
  /*
   * 先清状态再放捕获：ReleaseCapture 会同步重入 WM_CAPTURECHANGED，那个分支
   * 看到 active 还是 true 就会再 abort 一次（对已提交事务操作 = 未定义行为）。
   */
  shell.doc.guide_drag_active = false;
  const bool created = shell.doc.guide_drag_created;
  const int32_t index = shell.doc.guide_drag_index;
  const bool changed = shell.doc.guide_drag_changed;
  comp_txn txn = shell.doc.guide_drag_txn;
  comp_document document = shell.doc.guide_drag_doc;
  shell.doc.guide_drag_txn = nullptr;
  shell.doc.guide_drag_doc = nullptr;
  shell.doc.guide_drag_index = -1;
  shell.doc.guide_drag_created = false;
  shell.doc.guide_drag_changed = false;
  shell.doc.guide_hover = -1;
  if (GetCapture() == window) ReleaseCapture();

  if (document == nullptr || document != shell.doc.document) {
    /* 文档已换：旧事务随旧文档作废，句柄不再碰。 */
    return;
  }
  Layout layout{};
  const bool on_ruler = client_layout(shell, &layout) &&
                        comp_ui::ruler_hit(layout, x, y, shell.doc.show_rulers) != 0;
  if (on_ruler) {
    if (created) {
      /* 从标尺拖出来又拖回去：回滚整次事务，等于什么都没发生（无撤销步）。 */
      if (txn != nullptr) comp_txn_abort(txn);
      set_status(shell, L"参考线已取消（松手落在标尺上）");
      return;
    }
    /* 已有参考线拖回标尺 = 删除；删除落在同一事务里，整次操作仍是一步撤销。 */
    int32_t removed = 0;
    if (index >= 0 && comp_guide_remove(document, static_cast<size_t>(index), &removed) ==
                          COMP_OK &&
        removed != 0) {
      if (txn != nullptr) comp_txn_commit(txn);
      set_status(shell, L"参考线已删除");
    } else {
      if (txn != nullptr) comp_txn_abort(txn);
    }
    return;
  }
  if (!changed) {
    /* 值没变：abort。Document::txn_commit 无条件进历史，commit 就等于空撤销步。 */
    if (txn != nullptr) comp_txn_abort(txn);
    if (created) set_status(shell, L"参考线已达上限（128 条），本次未创建");
    return;
  }
  if (txn != nullptr) comp_txn_commit(txn);
  set_status(shell, created ? L"已添加参考线" : L"已移动参考线");
}

void guide_drag_cancel(ShellState& shell) {
  if (!shell.doc.guide_drag_active && shell.doc.guide_drag_txn == nullptr) return;
  shell.doc.guide_drag_active = false;
  comp_txn txn = shell.doc.guide_drag_txn;
  comp_document document = shell.doc.guide_drag_doc;
  shell.doc.guide_drag_txn = nullptr;
  shell.doc.guide_drag_doc = nullptr;
  shell.doc.guide_drag_index = -1;
  shell.doc.guide_drag_created = false;
  shell.doc.guide_drag_changed = false;
  shell.doc.guide_hover = -1;
  if (txn != nullptr && document != nullptr && document == shell.doc.document) {
    comp_txn_abort(txn);
  }
}

void guide_drag_forget(ShellState& shell) {
  /*
   * 换文档 / 销毁文档前调用。与 cancel 的区别：这里**允许**旧文档已经失效，
   * 所以只在句柄仍等于当前文档时才 abort（旧文档上挂着的事务随文档一起丢弃）。
   */
  if (shell.doc.guide_drag_active || shell.doc.guide_drag_txn != nullptr) {
    guide_drag_cancel(shell);
  }
  shell.doc.guide_hover = -1;
}

bool guide_hover_update(ShellState& shell, int x, int y, const Layout& layout) {
  int32_t hover = -1;
  if (!shell.doc.guide_drag_active && shell.doc.show_guides &&
      shell.tool == comp_ui::ToolId::Move && layout.canvas.contains(x, y)) {
    hover = guide_at(shell, layout, x, y);
  }
  if (hover == shell.doc.guide_hover) return false;
  shell.doc.guide_hover = hover;
  return true;
}

/* ------------------------------------------------------------ 绘制 */

void draw_grid_lines(HDC dc, const ShellState& shell, const Layout& layout) {
  if (dc == nullptr || !shell.doc.show_grid || shell.doc.document == nullptr) return;
  if (shell.doc.width <= 0 || shell.doc.height <= 0 || layout.canvas.empty()) return;
  const comp_ui::Rect& canvas = layout.canvas;
  const int saved = SaveDC(dc);
  IntersectClipRect(dc, canvas.x, canvas.y, canvas.right(), canvas.bottom());

  HPEN minor_pen = CreatePen(PS_SOLID, 1, RGB(96, 102, 116));
  HPEN major_pen = CreatePen(PS_SOLID, 1, RGB(132, 140, 158));
  HGDIOBJ old_pen = SelectObject(dc, minor_pen);

  int index = 1;
  for (double doc_x = kGridStep; doc_x < static_cast<double>(shell.doc.width);
       doc_x += kGridStep, ++index) {
    const comp_ui::Point point = shell.doc.view.doc_to_screen(doc_x, 0.0, canvas, shell.doc.width,
                                                              shell.doc.height);
    const int sx = static_cast<int>(std::lround(point.x));
    /* 0 与 width 是画布边界本身，不画（与选区的边界线重叠会看成脏点）。 */
    if (sx <= canvas.x || sx >= canvas.right()) continue;
    SelectObject(dc, index % kGridMajorEvery == 0 ? major_pen : minor_pen);
    MoveToEx(dc, sx, canvas.y, nullptr);
    LineTo(dc, sx, canvas.bottom());
  }

  index = 1;
  for (double doc_y = kGridStep; doc_y < static_cast<double>(shell.doc.height);
       doc_y += kGridStep, ++index) {
    const comp_ui::Point point = shell.doc.view.doc_to_screen(0.0, doc_y, canvas, shell.doc.width,
                                                              shell.doc.height);
    const int sy = static_cast<int>(std::lround(point.y));
    if (sy <= canvas.y || sy >= canvas.bottom()) continue;
    SelectObject(dc, index % kGridMajorEvery == 0 ? major_pen : minor_pen);
    MoveToEx(dc, canvas.x, sy, nullptr);
    LineTo(dc, canvas.right(), sy);
  }

  SelectObject(dc, old_pen);
  DeleteObject(minor_pen);
  DeleteObject(major_pen);
  RestoreDC(dc, saved);
}

void draw_guides(HDC dc, const ShellState& shell, const Layout& layout) {
  if (dc == nullptr || !shell.doc.show_guides || shell.doc.document == nullptr) return;
  if (layout.canvas.empty()) return;
  size_t count = 0;
  if (comp_guide_count(shell.doc.document, &count) != COMP_OK || count == 0) return;
  const comp_ui::Rect& canvas = layout.canvas;
  const int saved = SaveDC(dc);
  IntersectClipRect(dc, canvas.x, canvas.y, canvas.right(), canvas.bottom());

  HPEN normal_pen = CreatePen(PS_SOLID, 1, RGB(0, 174, 239));
  HPEN hot_pen = CreatePen(PS_SOLID, 2, RGB(140, 225, 255));
  HGDIOBJ old_pen = SelectObject(dc, normal_pen);
  for (size_t i = 0; i < count; ++i) {
    comp_guide guide{};
    if (comp_guide_get(shell.doc.document, i, &guide) != COMP_OK) break;
    const bool hot = static_cast<int32_t>(i) == shell.doc.guide_hover ||
                     (shell.doc.guide_drag_active &&
                      static_cast<int32_t>(i) == shell.doc.guide_drag_index);
    SelectObject(dc, hot ? hot_pen : normal_pen);
    if (guide.axis == COMP_GUIDE_VERTICAL) {
      const comp_ui::Point point = shell.doc.view.doc_to_screen(
          guide.position, 0.0, canvas, shell.doc.width, shell.doc.height);
      const int sx = static_cast<int>(std::lround(point.x));
      MoveToEx(dc, sx, canvas.y, nullptr);
      LineTo(dc, sx, canvas.bottom());
    } else {
      const comp_ui::Point point = shell.doc.view.doc_to_screen(
          0.0, guide.position, canvas, shell.doc.width, shell.doc.height);
      const int sy = static_cast<int>(std::lround(point.y));
      MoveToEx(dc, canvas.x, sy, nullptr);
      LineTo(dc, canvas.right(), sy);
    }
  }
  SelectObject(dc, old_pen);
  DeleteObject(normal_pen);
  DeleteObject(hot_pen);
  RestoreDC(dc, saved);
}

void draw_rulers(HDC dc, const ShellState& shell, const Layout& layout) {
  if (dc == nullptr || !shell.doc.show_rulers || layout.canvas.empty()) return;
  const comp_ui::Rect& canvas = layout.canvas;
  const comp_ui::Rect top = comp_ui::ruler_top_rect(canvas);
  const comp_ui::Rect left = comp_ui::ruler_left_rect(canvas);
  if (top.h <= 0 || left.w <= 0) return;
  const int saved = SaveDC(dc);
  IntersectClipRect(dc, canvas.x, canvas.y, canvas.right(), canvas.bottom());
  SetBkMode(dc, TRANSPARENT);

  RECT top_rect{top.x, top.y, top.right(), top.bottom()};
  RECT left_rect{left.x, left.y, left.right(), left.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(46, 48, 54));
  FillRect(dc, &top_rect, background);
  FillRect(dc, &left_rect, background);
  DeleteObject(background);

  /* 分隔线：标尺条与画面之间一条 1px 暗线，标尺打开时画面边界仍然可读。 */
  HPEN edge = CreatePen(PS_SOLID, 1, RGB(26, 28, 32));
  HGDIOBJ old_pen = SelectObject(dc, edge);
  MoveToEx(dc, canvas.x, top.bottom() - 1, nullptr);
  LineTo(dc, canvas.right(), top.bottom() - 1);
  MoveToEx(dc, left.right() - 1, canvas.y, nullptr);
  LineTo(dc, left.right() - 1, canvas.bottom());
  SelectObject(dc, old_pen);
  DeleteObject(edge);

  /* 刻度：主刻度 = 1-2-5 序列（标签永不相撞），每主刻度再分 5 小格。 */
  const double step = comp_ui::ruler_major_step(shell.doc.view.zoom);
  const int minor_divisor = 5;
  const bool minor_visible = step * shell.doc.view.zoom >= 40.0;
  HPEN tick = CreatePen(PS_SOLID, 1, RGB(150, 156, 170));
  HPEN dark_tick = CreatePen(PS_SOLID, 1, RGB(104, 110, 124));
  SetTextColor(dc, RGB(190, 196, 208));
  HFONT ruler_font =
      CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HGDIOBJ old_font = ruler_font != nullptr ? SelectObject(dc, ruler_font) : nullptr;

  /*
   * 可见范围用**文档坐标**：标尺读数是文档像素，屏幕坐标只在换算时出现。
   * 两端各放宽半步，避免边界上的主刻度因为取整被漏掉。
   */
  const comp_ui::Point doc_tl = shell.doc.view.screen_to_doc(
      static_cast<double>(canvas.x), static_cast<double>(canvas.y), canvas, shell.doc.width,
      shell.doc.height);
  const comp_ui::Point doc_br = shell.doc.view.screen_to_doc(
      static_cast<double>(canvas.right() - 1), static_cast<double>(canvas.bottom() - 1), canvas,
      shell.doc.width, shell.doc.height);
  /* 标签一律按尺子条裁切：4 位以上读数会超出 18px，绝不能压到画面上。 */
  RECT top_clip{top.x, top.y, top.right(), top.bottom()};
  RECT left_clip{left.x, left.y, left.right(), left.bottom()};

  for (double doc_x = std::floor(doc_tl.x / step) * step; doc_x <= doc_br.x + step * 0.5;
       doc_x += step) {
    const comp_ui::Point point =
        shell.doc.view.doc_to_screen(doc_x, 0.0, canvas, shell.doc.width, shell.doc.height);
    const int sx = static_cast<int>(std::lround(point.x));
    if (sx < canvas.x || sx >= canvas.right()) continue;
    SelectObject(dc, tick);
    MoveToEx(dc, sx, top.bottom() - 6, nullptr);
    LineTo(dc, sx, top.bottom() - 1);
    wchar_t label[32] = L"";
    swprintf(label, 32, L"%d", static_cast<int>(std::lround(doc_x)));
    ExtTextOutW(dc, sx + 2, top.y + 1, ETO_CLIPPED, &top_clip, label,
                static_cast<UINT>(wcslen(label)), nullptr);
    if (minor_visible) {
      SelectObject(dc, dark_tick);
      for (int m = 1; m < minor_divisor; ++m) {
        const double minor_x = doc_x + step * static_cast<double>(m) / minor_divisor;
        const comp_ui::Point p =
            shell.doc.view.doc_to_screen(minor_x, 0.0, canvas, shell.doc.width, shell.doc.height);
        const int mx = static_cast<int>(std::lround(p.x));
        if (mx < canvas.x || mx >= canvas.right()) continue;
        MoveToEx(dc, mx, top.bottom() - 3, nullptr);
        LineTo(dc, mx, top.bottom() - 1);
      }
    }
  }

  for (double doc_y = std::floor(doc_tl.y / step) * step; doc_y <= doc_br.y + step * 0.5;
       doc_y += step) {
    const comp_ui::Point point =
        shell.doc.view.doc_to_screen(0.0, doc_y, canvas, shell.doc.width, shell.doc.height);
    const int sy = static_cast<int>(std::lround(point.y));
    if (sy < canvas.y || sy >= canvas.bottom()) continue;
    SelectObject(dc, tick);
    MoveToEx(dc, left.right() - 6, sy, nullptr);
    LineTo(dc, left.right() - 1, sy);
    wchar_t label[32] = L"";
    swprintf(label, 32, L"%d", static_cast<int>(std::lround(doc_y)));
    ExtTextOutW(dc, left.x + 1, sy + 1, ETO_CLIPPED, &left_clip, label,
                static_cast<UINT>(wcslen(label)), nullptr);
    if (minor_visible) {
      SelectObject(dc, dark_tick);
      for (int m = 1; m < minor_divisor; ++m) {
        const double minor_y = doc_y + step * static_cast<double>(m) / minor_divisor;
        const comp_ui::Point p =
            shell.doc.view.doc_to_screen(0.0, minor_y, canvas, shell.doc.width, shell.doc.height);
        const int my = static_cast<int>(std::lround(p.y));
        if (my < canvas.y || my >= canvas.bottom()) continue;
        MoveToEx(dc, left.right() - 3, my, nullptr);
        LineTo(dc, left.right() - 1, my);
      }
    }
  }

  if (old_font != nullptr) SelectObject(dc, old_font);
  if (ruler_font != nullptr) DeleteObject(ruler_font);
  SelectObject(dc, old_pen);
  DeleteObject(tick);
  DeleteObject(dark_tick);

  /* 左上角交叉块盖住两把尺子在角落的刻度（与上游标尺同形状）。 */
  const comp_ui::Rect corner = comp_ui::ruler_corner_rect(canvas);
  RECT corner_rect{corner.x, corner.y, corner.right(), corner.bottom()};
  HBRUSH corner_brush = CreateSolidBrush(RGB(38, 40, 46));
  FillRect(dc, &corner_rect, corner_brush);
  DeleteObject(corner_brush);

  RestoreDC(dc, saved);
}

}  // namespace comp_shell
