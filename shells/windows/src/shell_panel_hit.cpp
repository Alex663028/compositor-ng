// AI生成
/*
 * shell_panel_hit.cpp — 面板命中与交互层。
 *
 * 为什么单独成文件：面板交互过去内联在主窗口的 WM_LBUTTONDOWN / WM_MOUSEMOVE /
 * WM_LBUTTONUP 里（那段有 1200 行）。要支持**浮动面板**（独立顶层窗口）就必须让
 * 两个窗口走同一份交互代码，否则浮起来的面板会变成「能看不能点」的摆设。
 *
 * 抽取方式：把原代码整段搬过来，只做两处机械改写——
 *   1. `shell->` / `*shell` -> `shell.`（主窗口里 shell 是指针，这里是引用）；
 *   2. `return 0;` -> `return true;`（原来是「吃掉这次点击」，现在是「我处理了」）。
 * 行几何、状态字段、撤销步一概没动，因此既有行为逐字节保持。
 *
 * 坐标口径：mouse_x / mouse_y 是**该面板所在窗口的客户区坐标**（逻辑像素）。
 * 主窗口传 client_layout 的结果；浮动窗口传一份「面板矩形 = 浮动窗口客户区」的
 * 合成布局——行几何全部由 layout 里的面板矩形推出，因此两边天然一致。
 */
#include "shell_panel_hit.h"

#include "shell_internal.h"

namespace comp_shell {


/* 图层面板命中：直方图框选 + 图层行点击（含直方图拖拽起点）。 */
bool layer_panel_click(ShellState& shell, const Layout& layout, int mouse_x, int mouse_y, HWND window) {
          /*
           * 图层列表的滚动条：滑块按下 = 开始拖动；轨道空白处按下 = 翻一页。
           *
           * 放在**最前面**：滚动条贴着面板右缘，点它绝不能被当成「点图层行」
           * 或「直方图框选」。
           */
          {
            const int row_top = comp_ui::layer_rows_top(shell.doc.histogram_open);
            const int count = panel_row_count(shell);
            const int viewport_h =
                layout.layer_panel.h - (comp_ui::kLayerHeaderHeight + row_top);
            const int content_h = count * comp_ui::kLayerRowHeight;
            const int max_scroll =
                comp_ui::layer_scroll_max(layout.layer_panel, count, row_top);
            const comp_ui::ScrollBarGeometry bar = comp_ui::scroll_bar_vertical(
                layout.layer_panel, comp_ui::kLayerHeaderHeight + row_top, viewport_h, content_h,
                shell.doc.layer_scroll, max_scroll);
            if (bar.visible && bar.track.contains(mouse_x, mouse_y)) {
              if (bar.thumb.contains(mouse_x, mouse_y)) {
                shell.doc.layer_scroll_drag = true;
                SetCapture(window);
                InvalidateRect(window, nullptr, FALSE);
                return true;
              }
              /* 点轨道空白处：按点在滑块上方还是下方翻页。 */
              const int step = comp_ui::scroll_page_step(
                  comp_ui::layer_visible_rows(layout.layer_panel, row_top));
              const int next = mouse_y < bar.thumb.y ? shell.doc.layer_scroll - step
                                                      : shell.doc.layer_scroll + step;
              shell.doc.layer_scroll =
                  comp_ui::layer_scroll_clamp(layout.layer_panel, count, row_top, next);
              SetCapture(window);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          /*
           * S24 直方图交互：绘图区按下 = 开始框选（松开超过 4px 才真的缩放）；
           * 标签行按下 = 复位全幅。放在图层行命中之前，两块几何互不重叠，但顺序
           * 固定后「图里拖」与「点图层行」不会互相吃掉点击。
           */
          if (shell.doc.histogram_open && !shell.doc.histogram_bins.empty()) {
            const comp_ui::Rect plot = comp_ui::histogram_plot_rect(layout.layer_panel);
            if (plot.contains(mouse_x, mouse_y)) {
              shell.doc.histogram_drag = true;
              shell.doc.histogram_drag_from_x = mouse_x;
              shell.doc.histogram_drag_to_x = mouse_x;
              SetCapture(window);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
            const comp_ui::Rect histogram_label =
                comp_ui::histogram_label_rect(layout.layer_panel);
            if (histogram_label.contains(mouse_x, mouse_y)) {
              panel_histogram_reset(shell);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          const int row_top = comp_ui::layer_rows_top(shell.doc.histogram_open);
          /* S44：行数用可见行数（折叠后子层不占行），行号经 display 换算成图层。 */
          const int row = comp_ui::layer_row_at_scrolled(
              layout.layer_panel, panel_row_count(shell), mouse_x, mouse_y, row_top,
              shell.doc.layer_scroll);
          if (row >= 0) {
            const LayerRow* clicked_layer = layer_row_at(shell, row);
            if (clicked_layer != nullptr && clicked_layer->is_group != 0) {
              /*
               * S44 组折叠：三角落在行首的固定槽位里，只对组行生效，且必须先于
               * 行选中与双击改名——否则点三角会顺手把该组选上/改起名来。
               */
              const comp_ui::Rect row_rect = comp_ui::layer_row_rect_scrolled(
                  layout.layer_panel, row, row_top, shell.doc.layer_scroll);
              const int depth = row < static_cast<int>(shell.doc.layer_depth.size())
                                    ? shell.doc.layer_depth[static_cast<size_t>(row)]
                                    : 0;
              const comp_ui::Rect triangle = comp_ui::layer_triangle_rect(row_rect, depth);
              if (triangle.contains(mouse_x, mouse_y)) {
                panel_toggle_group_collapsed(shell, clicked_layer->id);
                refresh_effects(shell);
                /* 组内层数取全量数组（折叠后子层不在可见行里，数不到）。 */
                int32_t children = 0;
                for (const LayerRow& layer : shell.doc.layers) {
                  if (layer.parent == clicked_layer->id) ++children;
                }
                wchar_t buffer[192];
                wsprintfW(buffer, L"%s组 #%I64u（%d 层）",
                          layer_group_collapsed(shell, clicked_layer->id) ? L"已折叠 "
                                                                          : L"已展开 ",
                          static_cast<unsigned long long>(clicked_layer->id), children);
                set_status(shell, buffer);
                InvalidateRect(window, nullptr, FALSE);
                return true;
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
              if (panel_selection_contains(shell, clicked_id)) {
                if (row == shell.doc.selected_layer) {
                  if (!shell.doc.extra_selection.empty()) {
                    const comp_layer_id promoted = shell.doc.extra_selection.front();
                    shell.doc.extra_selection.erase(shell.doc.extra_selection.begin());
                    /* S44：行号随折叠变化，按 id 反查可见行，而不是数组逆序推算。 */
                    panel_select_layer_id(shell, promoted);
                  }
                } else {
                  auto& extras = shell.doc.extra_selection;
                  for (size_t i = 0; i < extras.size(); ++i) {
                    if (extras[i] == clicked_id) {
                      extras.erase(extras.begin() + static_cast<std::ptrdiff_t>(i));
                      break;
                    }
                  }
                }
              } else {
                shell.doc.extra_selection.push_back(clicked_id);
              }
            } else if (click_shift && shell.doc.selected_layer >= 0) {
              const int32_t from = shell.doc.selected_layer < row ? shell.doc.selected_layer : row;
              const int32_t to = shell.doc.selected_layer < row ? row : shell.doc.selected_layer;
              shell.doc.extra_selection.clear();
              for (int32_t line = from; line <= to; ++line) {
                const LayerRow* line_layer = layer_row_at(shell, line);
                if (line_layer != nullptr && line_layer->id != clicked_id) {
                  shell.doc.extra_selection.push_back(line_layer->id);
                }
              }
              shell.doc.selected_layer = row;
            } else {
              shell.doc.extra_selection.clear();
              shell.doc.selected_layer = row;
              /*
               * S24 拖行重排：按下即 pending（记录起点行与 y），移动超过阈值才进入
               * active。单击必须还是「选中」，手抖 1px 不该把层挪走；捕获在
               * WM_LBUTTONUP / WM_CAPTURECHANGED 里释放。
               */
              shell.doc.layer_drag_row = row;
              shell.doc.layer_drag_target = row;
              shell.doc.layer_drag_press_y = mouse_y;
              shell.doc.layer_drag_pending = true;
              shell.doc.layer_drag_active = false;
              SetCapture(window);
            }
            refresh_adjustments(shell);
            /* S24：效果列表同样跟随选中图层，否则菜单勾选与 fx 标记会停在上一层。 */
            refresh_effects(shell);
            /* P1-2 收尾 ⑧：填充面板也跟随选中图层（绑定层或退回新建态）。 */
            refresh_fill(shell);
            const LayerRow* selected = selected_row(shell);
            if (selected != nullptr) {
              wchar_t buffer[160];
              if (panel_selection_ids(shell, false).size() > 1) {
                wsprintfW(buffer, L"已选中 %d 个图层（主选中 #%I64u，第 %d 行；Ctrl+J/Del 批量）",
                          static_cast<int>(panel_selection_ids(shell, false).size()),
                          static_cast<unsigned long long>(selected->id), row + 1);
              } else {
                wsprintfW(buffer, L"已选中图层 #%I64u（第 %d 行）",
                          static_cast<unsigned long long>(selected->id), row + 1);
              }
              set_status(shell, buffer);
            }
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
  return false;
}


/* 右栏命中：文档对话框 / 效果 / 修边 / 滤镜 / 填充 / 智能滤镜 / 文字 / 字体 / 调整 / 色彩范围 / 路径 / 历史 / 通道各面板的行与滑杆。 */
bool adjust_panel_click(ShellState& shell, const Layout& layout, int mouse_x, int mouse_y, HWND window) {
          /*
           * 调整栏可滚子面板（历史 / 通道 / 字体）的可见滚动条：
           * 点滑块 = 开始拖动；点轨道空白 = 翻一页。
           *
           * 放在最前面：滚动条贴在面板右缘，点它绝不能被当成「点某一行」。
           */
          if (layout.adjust_panel.w > 0 && !layout.adjust_collapsed) {
            const comp_ui::ScrollBarGeometry bar =
                adjust_panel_scroll_bar(shell, layout.adjust_panel);
            if (bar.visible && bar.track.contains(mouse_x, mouse_y)) {
              const ScrollDragTarget target = adjust_panel_scroll_target(shell);
              if (bar.thumb.contains(mouse_x, mouse_y)) {
                shell.scroll_drag_target = target;
                SetCapture(window);
                InvalidateRect(window, nullptr, FALSE);
                return true;
              }
              /* 点轨道：按在滑块上方还是下方翻页。 */
              const int visible = adjust_panel_visible_rows(shell, target, layout.adjust_panel);
              const int step = comp_ui::scroll_page_step(visible);
              const int current = adjust_panel_scroll_value(shell, target);
              const int next = mouse_y < bar.thumb.y ? current - step : current + step;
              adjust_panel_set_scroll(shell, target, next, layout.adjust_panel);
              SetCapture(window);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          if (shell.doc.doc_dialog != comp_ui::DocDialogKind::None && layout.adjust_panel.w > 0) {
            /* 点行 = 聚焦到该行；行几何与绘制共用 doc_dialog_row_rect。 */
            const int row = comp_ui::doc_dialog_row_at(
                layout.adjust_panel, comp_ui::doc_dialog_row_count(shell.doc.doc_dialog),
                mouse_x, mouse_y);
            if (row >= 0) {
              shell.doc.doc_dialog_input.clear();
              doc_dialog_focus_row(shell, row - shell.doc.doc_dialog_focus);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          if (shell.doc.effect_open && layout.adjust_panel.w > 0) {
            /* 点参数行 = 聚焦到该行；行几何与绘制共用 effect_param_row_rect。 */
            const int row = comp_ui::effect_param_row_at(
                layout.adjust_panel, static_cast<int>(shell.doc.effect_values.size()), mouse_x,
                mouse_y);
            if (row >= 0) {
              panel_effect_focus_row(shell, row);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          if (shell.doc.trim_open && layout.adjust_panel.w > 0) {
            /* 点行 = 聚焦到该行；行几何与绘制共用 trim_row_rect。 */
            const int row = comp_ui::trim_row_at(layout.adjust_panel, shell.doc.trim.row_count(),
                                                 mouse_x, mouse_y);
            if (row >= 0) {
              shell.doc.trim_input.clear();
              shell.doc.trim.focused = row;
              wchar_t buffer[192];
              wsprintfW(buffer, L"修边 %d/%d：%s", row + 1, shell.doc.trim.row_count(),
                        widen_ascii(comp_ui::trim_row_label(row)).c_str());
              set_status(shell, buffer);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          if (shell.doc.filter_open && layout.adjust_panel.w > 0) {
            /* 点参数行 = 聚焦到该行；行几何与绘制共用 filter_param_row_rect。 */
            const comp_ui::FilterPanel& panel = shell.doc.filter;
            const int row = comp_ui::filter_param_row_at(layout.adjust_panel, panel.param_count(),
                                                         mouse_x, mouse_y, panel.columns(),
                                                         panel.grid_count());
            if (row >= 0) {
              shell.doc.filter_input.clear();
              filter_focus_row(shell, row - shell.doc.filter.focused);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          if (shell.doc.fill_open && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 填充参数行（P1-2 收尾 ⑧）：上半行 = 聚焦，下半行（滑道）= 聚焦 + 拖动。
             * 行/滑道几何与 draw_fill_panel 同源（ui_core::adjust_param_row_rect /
             * adjust_slider_track_rect）。布尔没有滑道，点一下就切换。
             * 面板里点空白也吞掉：面板开着时右栏没有第二个来源。
             */
            const int32_t total = static_cast<int32_t>(shell.doc.fill_values.size());
            const int32_t scroll = comp_ui::adjust_param_clamp_scroll(
                layout.adjust_panel, total, shell.doc.fill_scroll);
            shell.doc.fill_scroll = scroll;
            const int row = comp_ui::adjust_param_row_at(layout.adjust_panel, total, scroll,
                                                        mouse_x, mouse_y);
            if (row >= 0) {
              comp_fill_param param{};
              if (fill_param_query(shell, row, &param)) {
                shell.doc.fill_input.clear();
                if (param.type == COMP_PARAM_BOOL) {
                  fill_panel_focus_row(shell, row);
                  fill_panel_step_value(shell, 1);
                  /* 已绑定则一次写入（一步撤销）；新建态只改缓冲，回车建层。 */
                  if (shell.doc.fill_layer != 0) panel_apply_fill(shell);
                } else if (comp_ui::adjust_row_track_hit(layout.adjust_panel, row, scroll,
                                                         mouse_x, mouse_y)) {
                  fill_panel_begin_drag(shell, layout.adjust_panel, row,
                                        static_cast<double>(mouse_x));
                  if (shell.doc.fill_drag_param >= 0) SetCapture(window);
                } else {
                  fill_panel_focus_row(shell, row);
                }
              }
            }
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          if (shell.doc.smart_open && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 智能滤镜栈：点行 = 聚焦该条（只移动键盘焦点，不写文档）。面板里
             * 点空白也吞掉——面板开着时右栏没有第二个来源。行几何与
             * draw_smart_panel 同源（smart_row_rect / smart_row_at）；双击改
             * 参数在 WM_LBUTTONDBLCLK 里处理，单击这一下已经先把焦点落好。
             */
            const int count = smart_panel_count(shell);
            const int scroll = comp_ui::smart_clamp_scroll(layout.adjust_panel, count,
                                                           shell.doc.smart_scroll);
            shell.doc.smart_scroll = scroll;
            const int row = comp_ui::smart_row_at(layout.adjust_panel, count, scroll, mouse_x,
                                                  mouse_y);
            if (row >= 0) {
              smart_panel_focus_row(shell, row);
              /*
               * P1-2 收尾 ⑩ 拖拽重排：按下即 pending（记起点行与 y），纵向移动超过
               * 4 px 才进入 active。单击必须仍是「选中」，手抖 1px 不该把滤镜挪位；
               * 捕获在 WM_LBUTTONUP / WM_CAPTURECHANGED 里释放。
               */
              shell.doc.smart_drag_from = row;
              shell.doc.smart_drag_to = row;
              shell.doc.smart_drag_press_y = mouse_y;
              shell.doc.smart_drag_pending = true;
              shell.doc.smart_drag_active = false;
              SetCapture(window);
            }
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          if (type_panel_visible(shell) && layout.adjust_panel.w > 0) {
            /* 点属性行 = 聚焦到该行；行几何与绘制共用 type_param_row_rect。 */
            const int row =
                comp_ui::type_param_row_at(layout.adjust_panel, mouse_x, mouse_y);
            if (row >= 0) {
              shell.doc.type_input.clear();
              type_panel_focus(shell, row - shell.type_panel_focused);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          if (font_panel_visible(shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 字体面板：单击 = 选中并应用（改轨道 + 若是文字图层则重排一次，一步撤销）。
             * 空白处也吞掉，别漏给调整面板——面板开着时右栏没有第二个来源。
             */
            const int count = font_row_count(shell);
            const int scroll =
                comp_ui::font_clamp_scroll(layout.adjust_panel, count, shell.font_scroll);
            const int row =
                comp_ui::font_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (row >= 0) font_apply_row(shell, row);
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          if (!shell.doc.effect_open && !shell.doc.filter_open && !shell.doc.trim_open &&
              !shell.doc.fill_open && !shell.doc.smart_open &&
              !type_panel_visible(shell) &&
              !font_panel_visible(shell) &&
              shell.doc.doc_dialog == comp_ui::DocDialogKind::None &&
              !shell.doc.color_range_open && !path_panel_visible(shell) &&
              !history_panel_visible(shell) && !channel_panel_visible(shell) &&
              layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * P0-3 调整面板：上半行 = 聚焦，下半行（滑道）= 聚焦 + 开始拖动。
             * 行/滑道几何全部来自 ui_core::adjust_param_row_rect /
             * adjust_slider_track_rect，与 draw_adjust_panel 同源。
             * 分页视图（色阶）里几何用「相对行」（row - base），数据用绝对下标。
             */
            const int32_t shown_total = adjust_shown_count(shell);
            const int32_t base = adjust_param_base(shell);
            const int32_t scroll = comp_ui::adjust_param_clamp_scroll(
                layout.adjust_panel, shown_total, shell.doc.adj_scroll);
            shell.doc.adj_scroll = scroll;

            /* 色阶通道条：先于参数行命中（它在标题行，行命中吃不到这里）。 */
            if (adjust_paged_kind(static_cast<uint32_t>(shell.doc.adj_kind))) {
              const int32_t chip = comp_ui::adjust_channel_chip_at(
                  layout.adjust_panel, kLevelsChannelCount, mouse_x, mouse_y);
              if (chip >= 0) {
                adjust_set_channel(shell, chip);
                InvalidateRect(window, nullptr, FALSE);
                return true;
              }
            }

            /* 曲线：通道条 -> 绘图区（命中控制点 = 拖动，空白 = 加点后拖动）。 */
            if (shell.doc.adj_kind == COMP_ADJUSTMENT_CURVES) {
              const int32_t chip = comp_ui::curve_channel_chip_at(
                  layout.adjust_panel, kCurveChannels, mouse_x, mouse_y);
              if (chip >= 0) {
                curve_set_channel(shell, chip);
                InvalidateRect(window, nullptr, FALSE);
                return true;
              }
              const comp_ui::Rect plot = comp_ui::adjust_curve_plot_rect(layout.adjust_panel);
              if (!plot.empty() && plot.contains(mouse_x, mouse_y)) {
                const int32_t count = static_cast<int32_t>(shell.doc.curve_xy.size() / 2);
                const int32_t hit =
                    count >= 2 ? comp_ui::curve_point_hit(
                                     plot, shell.doc.curve_xy.data(), static_cast<int>(count),
                                     mouse_x, mouse_y, 7.0)
                               : -1;
                if (hit >= 0) {
                  curve_begin_drag(shell, plot, hit, mouse_x, mouse_y);
                  if (shell.doc.curve_drag_point >= 0) SetCapture(window);
                } else {
                  curve_add_point(shell, plot, mouse_x, mouse_y);
                  if (shell.doc.curve_drag_point >= 0) SetCapture(window);
                }
                InvalidateRect(window, nullptr, FALSE);
                return true;
              }
            }

            const int row = comp_ui::adjust_param_row_at(layout.adjust_panel, shown_total,
                                                         scroll, mouse_x, mouse_y);
            if (row >= 0) {
              const int32_t index = base + row;
              shell.doc.adj_param = index;
              shell.doc.adj_input.clear();
              if (comp_ui::adjust_row_track_hit(layout.adjust_panel, row, scroll, mouse_x,
                                                mouse_y)) {
                comp_adjustment_param param{};
                if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.doc.adj_kind),
                                                   static_cast<size_t>(index),
                                                   &param) == COMP_OK) {
                  if (param.type == COMP_PARAM_BOOL) {
                    /* 布尔参数没有滑道：点一下就是切换。 */
                    panel_step_value(shell, 1);
                  } else {
                    adjust_begin_drag(shell, layout.adjust_panel, index, mouse_x);
                    SetCapture(window);
                  }
                }
              } else {
                wchar_t buffer[220];
                comp_adjustment_param param{};
                if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.doc.adj_kind),
                                                   static_cast<size_t>(index),
                                                   &param) == COMP_OK) {
                  wsprintfW(buffer, L"%s = %s（范围 [%s, %s]；拖动滑杆实时预览）",
                            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
                            widen_ascii(shell.doc.adj_values[static_cast<size_t>(index)].c_str())
                                .c_str(),
                            widen_ascii(number_text(param.min_value).c_str()).c_str(),
                            widen_ascii(number_text(param.max_value).c_str()).c_str());
                  set_status(shell, buffer);
                }
              }
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          if (shell.doc.color_range_open && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 色彩范围面板：点色块 = 删掉该取样色；点「反相」行 = 切换；
             * 点「容差」行 = 提示键入（数字键在任何位置都能起键入态）。
             */
            const int slot = comp_ui::color_range_swatch_at(
                layout.adjust_panel,
                static_cast<int>(shell.doc.color_range_include.size() / 3),
                static_cast<int>(shell.doc.color_range_exclude.size() / 3), mouse_x, mouse_y);
            if (slot >= 0) {
              color_range_remove_swatch(shell, slot);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
            const int row = comp_ui::color_range_row_at(layout.adjust_panel, mouse_x, mouse_y);
            if (row == 1) {
              color_range_toggle_invert(shell);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
            if (row == 0) {
              set_status(shell,
                         L"色彩范围：- / = 步进容差，或直接按数字键入（回车落值）");
              return true;
            }
          }
          if (path_panel_visible(shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 路径面板：点行 = 选中该条；转选区走双击（WM_LBUTTONDBLCLK）或回车，
             * 单击不落动作——避免误点把用户已有选区顶掉。空白处也吞掉，别漏给调整面板。
             */
            const int count = static_cast<int>(shell.doc.paths.size());
            const int scroll =
                comp_ui::path_clamp_scroll(layout.adjust_panel, count, shell.doc.path_scroll);
            const int index =
                comp_ui::path_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (index >= 0) {
              shell.doc.path_selected = index;
              path_status(shell);
              InvalidateRect(window, nullptr, FALSE);
            }
            return true;
          }
          if (history_panel_visible(shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * 历史记录面板：点行 = 直接回跳到该状态（单击即落动作，与路径面板不同——
             * 回跳本身可再回跳，不产生历史记录，误点没有破坏性）。空白处吞掉。
             * Shift+点行 = 只把它指定给历史记录画笔当取样状态，**不回跳**：
             * 取样源必须能停在「过去某一格」而当前画面保持在最新，否则这支笔
             * 就没有意义了。
             */
            const int count = history_state_count(shell);
            const int scroll =
                comp_ui::history_clamp_scroll(layout.adjust_panel, count, shell.doc.history_scroll);
            const int index =
                comp_ui::history_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (index >= 0) {
              if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                shell.doc.history_brush_state = index;
                wchar_t buffer[288];
                wsprintfW(buffer, L"历史记录画笔取样：第 %d 个状态（%s）· 当前画面未回跳",
                          index, history_state_label(shell, index).c_str());
                set_status(shell, buffer);
              } else {
                history_jump_to(shell, index);
              }
            }
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          if (channel_panel_visible(shell) && layout.adjust_panel.w > 0 &&
              layout.adjust_panel.contains(mouse_x, mouse_y)) {
            /*
             * ROI #5 通道面板：点行 = 选中该条；载入走双击或回车，单击不落动作
             * （与路径面板同规矩——载入会顶掉用户现有选区，误点代价太大）。
             * 命名态下点面板 = 先取消命名（不保存），再按行选中。
             */
            if (shell.doc.channel_name_active) channel_name_cancel(shell);
            const int count = channel_count(shell);
            const int scroll = comp_ui::channel_clamp_scroll(layout.adjust_panel, count,
                                                             shell.doc.channel_scroll);
            const int index =
                comp_ui::channel_row_at(layout.adjust_panel, count, scroll, mouse_x, mouse_y);
            if (index >= 0) {
              shell.doc.channel_selected = index;
              channel_status(shell);
            }
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
  return false;
}


/* 面板拖动推进：曲线点 / 调整滑杆 / 填充滑杆 / 智能滤镜重排 / 图层拖行 / 直方图框选。 */
bool panel_drag_move(ShellState& shell, const Layout& layout, int mouse_x, int mouse_y, HWND window) {
          /* 调整栏滚动条滑块拖动：优先级最高——拖动期间鼠标消息归它。 */
          if (shell.scroll_drag_target != ScrollDragTarget::None) {
            const comp_ui::ScrollBarGeometry bar =
                adjust_panel_scroll_bar(shell, layout.adjust_panel);
            const int max_scroll =
                adjust_panel_max_scroll(shell, shell.scroll_drag_target, layout.adjust_panel);
            const int next = comp_ui::scroll_from_thumb_drag(bar, mouse_y, max_scroll);
            adjust_panel_set_scroll(shell, shell.scroll_drag_target, next, layout.adjust_panel);
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          /*
           * 滚动条滑块拖动：优先级最高——拖动期间鼠标消息归它，
           * 不能被曲线/滑杆/图层拖行抢走。
           */
          if (shell.doc.layer_scroll_drag) {
            const int row_top = comp_ui::layer_rows_top(shell.doc.histogram_open);
            const int count = panel_row_count(shell);
            const int viewport_h =
                layout.layer_panel.h - (comp_ui::kLayerHeaderHeight + row_top);
            const int content_h = count * comp_ui::kLayerRowHeight;
            const int max_scroll =
                comp_ui::layer_scroll_max(layout.layer_panel, count, row_top);
            const comp_ui::ScrollBarGeometry bar = comp_ui::scroll_bar_vertical(
                layout.layer_panel, comp_ui::kLayerHeaderHeight + row_top, viewport_h, content_h,
                shell.doc.layer_scroll, max_scroll);
            const int next = comp_ui::scroll_from_thumb_drag(bar, mouse_y, max_scroll);
            shell.doc.layer_scroll =
                comp_ui::layer_scroll_clamp(layout.layer_panel, count, row_top, next);
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          /* P0-3 曲线拖动优先：几何用按下时冻结的绘图区，写入落在同一个事务里。 */
          if (shell.doc.curve_drag_point >= 0) {
            curve_drag_to(shell, shell.doc.curve_drag_plot, static_cast<double>(mouse_x),
                          static_cast<double>(mouse_y));
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          /* P0-3 滑杆拖动优先：持有鼠标捕获，写入落在按下时开的同一个事务里。 */
          if (shell.doc.adj_drag_param >= 0) {
            adjust_drag_value(shell, static_cast<double>(mouse_x));
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          /* P1-2 收尾 ⑧ 填充滑杆同理：几何用按下瞬间冻结的滑道。 */
          if (shell.doc.fill_drag_param >= 0) {
            fill_panel_drag_value(shell, static_cast<double>(mouse_x));
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
          /*
           * P1-2 收尾 ⑩ 智能滤镜拖拽重排：与图层拖行同一套 4 px 阈值。落点是
           * 「松手后这条会落在的行」；指针跑出列表时夹到首/末行——拖出面板仍然是
           * 合法落点，否则想挪到栈底得精确对准 22 px 的高亮带。
           */
          if (shell.doc.smart_drag_pending) {
            const int dy = mouse_y - shell.doc.smart_drag_press_y;
            if (!shell.doc.smart_drag_active && (dy >= 4 || dy <= -4)) {
              shell.doc.smart_drag_active = true;
            }
            if (shell.doc.smart_drag_active) {
              const int count = smart_panel_count(shell);
              if (count > 0) {
                const int scroll = comp_ui::smart_clamp_scroll(layout.adjust_panel, count,
                                                               shell.doc.smart_scroll);
                int target = comp_ui::smart_row_at(layout.adjust_panel, count, scroll, mouse_x,
                                                   mouse_y);
                if (target < 0) {
                  target = mouse_y < layout.adjust_panel.y + comp_ui::kSmartRowsTop ? 0
                                                                                    : count - 1;
                }
                shell.doc.smart_drag_to = target;
              }
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          /*
           * S24：图层拖行（超过 4px 阈值才进入 active）与直方图框选优先于工具状态机。
           * 两者都持有鼠标捕获，必须在这里更新，否则松手时目标行停在按下那一行。
           */
          if (shell.doc.layer_drag_pending) {
            const int dy = mouse_y - shell.doc.layer_drag_press_y;
            if (!shell.doc.layer_drag_active && (dy >= 4 || dy <= -4)) {
              shell.doc.layer_drag_active = true;
            }
            if (shell.doc.layer_drag_active) {
              /* S44：目标行只在可见行里找——折叠掉的子层不该成为落点。 */
              shell.doc.layer_drag_target = comp_ui::layer_drag_target_row(
                  layout.layer_panel, panel_row_count(shell), static_cast<double>(mouse_y),
                  comp_ui::layer_rows_top(shell.doc.histogram_open), shell.doc.layer_scroll);
              InvalidateRect(window, nullptr, FALSE);
              return true;
            }
          }
          if (shell.doc.histogram_drag) {
            shell.doc.histogram_drag_to_x = mouse_x;
            InvalidateRect(window, nullptr, FALSE);
            return true;
          }
  return false;
}


/* 面板拖动收尾：提交事务 / 落子 / 释放捕获。 */
bool panel_drag_end(ShellState& shell, const Layout& layout, int mouse_x, int mouse_y, HWND window) {
      /* 调整栏滚动条松手：结束拖动。 */
      if (shell.scroll_drag_target != ScrollDragTarget::None) {
        shell.scroll_drag_target = ScrollDragTarget::None;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return true;
      }
      /* 滚动条滑块松手：结束拖动（其余拖动类型不受影响）。 */
      if (shell.doc.layer_scroll_drag) {
        shell.doc.layer_scroll_drag = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return true;
      }
      /* P0-3 曲线松手：提交事务 = 一次拖动只占一步撤销。 */
      if (shell.doc.curve_drag_point >= 0) {
        curve_drag_finish(shell, true);
        InvalidateRect(window, nullptr, FALSE);
        return true;
      }
      /* P0-3 滑杆松手：提交事务 = 一次拖动只占一步撤销。 */
      if (shell.doc.adj_drag_param >= 0) {
        adjust_drag_finish(shell, true);
        InvalidateRect(window, nullptr, FALSE);
        return true;
      }
      /* P1-2 收尾 ⑧ 填充滑杆松手：有实际写入才提交，空拖动不占撤销步。 */
      if (shell.doc.fill_drag_param >= 0) {
        fill_panel_drag_finish(shell, true);
        InvalidateRect(window, nullptr, FALSE);
        return true;
      }
      /*
       * P1-2 收尾 ⑩ 智能滤镜拖拽落点。pending 未 active = 单纯点选，什么都不做
       * （不能把「点一下」变成一次撤销步）；active 才把 move 交给引擎。
       */
      if (shell.doc.smart_drag_pending) {
        const bool dragged = shell.doc.smart_drag_active;
        const int32_t from = shell.doc.smart_drag_from;
        const int32_t to = shell.doc.smart_drag_to;
        shell.doc.smart_drag_pending = false;
        shell.doc.smart_drag_active = false;
        shell.doc.smart_drag_from = -1;
        shell.doc.smart_drag_to = -1;
        ReleaseCapture();
        if (dragged) smart_panel_drop(shell, from, to);
        InvalidateRect(window, nullptr, FALSE);
        return true;
      }
      /*
       * S24 直方图框选收尾：位移 < 4px 视为单击（不缩放）；否则把拖出的亮度区间
       * 设为新的可视窗口。夹取/排序全在 ui_core，这里只递交两个屏幕 x。
       */
      if (shell.doc.histogram_drag) {
        shell.doc.histogram_drag = false;
        shell.doc.histogram_drag_to_x = mouse_x;
        ReleaseCapture();
        panel_histogram_commit_drag(shell, comp_ui::histogram_plot_rect(layout.layer_panel));
        InvalidateRect(window, nullptr, FALSE);
        return true;
      }
      /*
       * S24 拖行重排收尾。pending 但未 active = 单纯点选，什么都不做（不能把
       * 「点一下图层」变成一次撤销步）；active 才提交，跨组由纯函数拒绝。
       */
      if (shell.doc.layer_drag_pending) {
        const bool dragged = shell.doc.layer_drag_active;
        const int32_t from_row = shell.doc.layer_drag_row;
        const int32_t to_row = shell.doc.layer_drag_target;
        bool drop_into_group = false;
        if (dragged && to_row >= 0) {
          /* S44：行号是可见行，目标图层经树形 display 反查。 */
          const int32_t rows = panel_row_count(shell);
          const LayerRow* target = layer_row_at(shell, to_row);
          if (target != nullptr) {
            /* 只有组行、且指针停在行中间带时才解释成「拖进组」。 */
            drop_into_group =
                target->is_group != 0 &&
                comp_ui::layer_drag_inner_band(layout.layer_panel, rows, to_row,
                                               static_cast<double>(mouse_y),
                                               comp_ui::layer_rows_top(shell.doc.histogram_open),
                                               shell.doc.layer_scroll);
          }
        }
        shell.doc.layer_drag_pending = false;
        shell.doc.layer_drag_active = false;
        shell.doc.layer_drag_row = -1;
        shell.doc.layer_drag_target = -1;
        ReleaseCapture();
        if (dragged && shell.doc.document != nullptr) {
          panel_drop_layer(shell, from_row, to_row, drop_into_group);
        }
        InvalidateRect(window, nullptr, FALSE);
        return true;
      }
  return false;
}

}  // namespace comp_shell
