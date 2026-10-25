// AI生成
/*
 * shell_filter.cpp — 滤镜面板与裁切（trim）
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {


/*
 * §6.62 滤镜实时预览的收尾：预览帧只在 framebuffer 里，像素从未改过。任何「离开预览」
 * 的路径（关面板、换面板、换文档、切到文字工具）都要清 pending 并强制重画一帧，
 * 否则画面会停在参数已经作废的预览上。needs_render 已为真的调用点重复置位无害。
 */
void filter_preview_forget(ShellState& shell) {
  shell.doc.filter_preview_pending = false;
  shell.doc.needs_render = true;
}

void open_filter_panel(ShellState& shell, comp_ui::FilterId id) {
  /* 右栏一次只挂一个面板：打开滤镜即收起效果参数面板、色彩范围、路径与历史面板。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.path_open = false;
  shell.doc.history_open = false;
  channel_panel_close(shell);
  shell.doc.filter.select(id);
  shell.doc.filter_open = true;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell); /* 上一个滤镜的预览帧不能留在新滤镜面板底下 */
  const comp_ui::FilterSpec* spec = shell.doc.filter.spec();
  wchar_t buffer[192];
  wsprintfW(buffer, L"滤镜：%s（Tab 选参数 · -/= 调值即预览 · 数字键入 · 回车应用 · Esc 关闭）",
            widen_ascii(spec != nullptr ? spec->name : "?").c_str());
  set_status(shell, buffer);
}

void close_filter_panel(ShellState& shell) {
  const bool had_preview = shell.doc.filter_preview_pending;
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell); /* 预览帧作废：Document 从未改动，重画即回真状态 */
    /* 从「编辑某条」出来：右栏回到滤镜栈列表，焦点停在该条。 */
    smart_panel_reopen_after_edit(shell, static_cast<int>(edit_index));
    set_status(shell, had_preview ? L"已返回智能滤镜列表（本次编辑未写回，预览已丢弃）"
                                  : L"已返回智能滤镜列表（本次编辑未写回）");
    return;
  }
  set_status(shell, had_preview ? L"已关闭滤镜面板（预览已丢弃，图层未改动）"
                                : L"已关闭滤镜面板");
}

void filter_reset_values(ShellState& shell) {
  shell.doc.filter_input.clear();
  shell.doc.filter.reset_values();
  /* 复位也是一次参数变化：照样出预览；默认值若正好恒等，助手会如实回报「无预览」。 */
  filter_preview_render(shell, L"滤镜参数已重置为默认值");
}

void filter_focus_row(ShellState& shell, int delta) {
  if (!shell.doc.filter_open) return;
  shell.doc.filter.focus(delta);
  const std::wstring text = param_status_text(shell.doc.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"参数 %d/%d：%s", shell.doc.filter.focused + 1, shell.doc.filter.param_count(),
            text.c_str());
  set_status(shell, buffer);
}

void filter_step_value(ShellState& shell, int direction) {
  if (!shell.doc.filter_open) return;
  const comp_ui::FilterParamSpec* spec = shell.doc.filter.param_at(shell.doc.filter.focused);
  if (spec == nullptr) return;
  shell.doc.filter.step(direction);
  const std::wstring text = param_status_text(shell.doc.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s（步长 %s）", text.c_str(),
            widen_ascii(number_text(spec->step).c_str()).c_str());
  filter_preview_render(shell, buffer); /* 状态行由助手补「预览中 / 恒等 / 失败」后缀 */
}

void filter_begin_input(ShellState& shell, wchar_t character) {
  if (!shell.doc.filter_open) return;
  shell.doc.filter_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.doc.filter_input.c_str());
  set_status(shell, buffer);
}

void filter_backspace(ShellState& shell) {
  if (shell.doc.filter_input.empty()) return;
  shell.doc.filter_input.pop_back();
}

/* 键入值先夹到面板量程再落值：面板永不产生越界参数（ABI 仍会拒绝越界）。
 * with_preview = false 只用于「键入后紧接一次真应用」的回车：那一帧随时被应用结果盖掉，
 * 先预览等于白算一遍全分辨率滤镜。 */
void filter_commit_input(ShellState& shell, bool with_preview) {
  if (shell.doc.filter_input.empty()) return;
  const std::string text(shell.doc.filter_input.begin(), shell.doc.filter_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  shell.doc.filter.set_value(shell.doc.filter.focused, value);
  shell.doc.filter_input.clear();
  const std::wstring shown = param_status_text(shell.doc.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"已设置 %s", shown.c_str());
  if (with_preview) {
    filter_preview_render(shell, buffer);
  } else {
    set_status(shell, buffer);
  }
}

/* 面板值按固定行序搬进 ABI 结构（唯一映射点；顺序见 ui_core 的 filter_table）。
 * Progressive Blur 的渐变轴不在面板行里，从 ShellState 的画布拖拽状态取。 */
void build_filter_desc(const ShellState& shell, comp_filter_desc* out) {
  const comp_ui::FilterPanel& panel = shell.doc.filter;
  comp_filter_desc desc{};
  desc.kind = static_cast<comp_filter_kind>(static_cast<int>(panel.id));
  switch (panel.id) {
    case comp_ui::FilterId::Vignette:
      desc.vignette.amount = panel.value(0);
      desc.vignette.midpoint = panel.value(1);
      desc.vignette.roundness = panel.value(2);
      desc.vignette.feather = panel.value(3);
      desc.vignette.highlights = panel.value(4);
      /* 面板 v1 不暴露边缘色：固定黑（与上游默认色一致），已在文档记边界。 */
      desc.vignette.color[0] = 0.0;
      desc.vignette.color[1] = 0.0;
      desc.vignette.color[2] = 0.0;
      break;
    case comp_ui::FilterId::TonalContrast:
      desc.tonal_contrast.amount = panel.value(0);
      desc.tonal_contrast.radius = panel.value(1);
      desc.tonal_contrast.shadows = panel.value(2);
      desc.tonal_contrast.midtones = panel.value(3);
      desc.tonal_contrast.highlights = panel.value(4);
      break;
    case comp_ui::FilterId::Bloom:
      desc.bloom.amount = panel.value(0);
      desc.bloom.radius = panel.value(1);
      break;
    case comp_ui::FilterId::LensCorrection:
      desc.lens_correction.distortion = panel.value(0);
      break;
    case comp_ui::FilterId::GaussianBlur:
      desc.gaussian_blur.radius = panel.value(0);
      desc.gaussian_blur.preserve_transparency = panel.value(1) >= 0.5 ? 1 : 0;
      break;
    case comp_ui::FilterId::AddNoise:
      desc.add_noise.amount = panel.value(0);
      desc.add_noise.gaussian = panel.value(1) >= 0.5 ? 1 : 0;
      desc.add_noise.monochromatic = panel.value(2) >= 0.5 ? 1 : 0;
      desc.add_noise.seed = static_cast<uint32_t>(std::llround(panel.value(3)));
      break;
    case comp_ui::FilterId::Grain:
      desc.grain.amount = panel.value(0);
      desc.grain.size = panel.value(1);
      desc.grain.roughness = panel.value(2);
      desc.grain.seed = static_cast<uint32_t>(std::llround(panel.value(3)));
      break;
    case comp_ui::FilterId::MotionBlur:
      desc.motion_blur.angle = panel.value(0);
      desc.motion_blur.distance = panel.value(1);
      break;
    case comp_ui::FilterId::ProgressiveBlur:
      desc.progressive_blur.from = panel.value(0);
      desc.progressive_blur.to = panel.value(1);
      desc.progressive_blur.radial = panel.value(2) >= 0.5 ? 1 : 0;
      desc.progressive_blur.inverted = panel.value(3) >= 0.5 ? 1 : 0;
      /* 没拖过轴 = 交给引擎的缺省轴（图层像素矩形中线），不传伪端点。 */
      if (shell.doc.progressive_axis_set) {
        desc.progressive_blur.explicit_axis = 1;
        desc.progressive_blur.start_x = shell.doc.progressive_axis_start.x;
        desc.progressive_blur.start_y = shell.doc.progressive_axis_start.y;
        desc.progressive_blur.end_x = shell.doc.progressive_axis_final.x;
        desc.progressive_blur.end_y = shell.doc.progressive_axis_final.y;
      }
      break;
    case comp_ui::FilterId::Sharpen:
      desc.sharpen.amount = panel.value(0);
      desc.sharpen.radius = panel.value(1);
      desc.sharpen.threshold = panel.value(2);
      break;
    case comp_ui::FilterId::ReduceNoise:
      desc.reduce_noise.strength = panel.value(0);
      desc.reduce_noise.preserve_details = panel.value(1);
      desc.reduce_noise.reduce_color_noise = panel.value(2);
      break;
    default:
      break;
  }
  *out = desc;
}

/*
 * §6.62 滤镜实时预览：改一个参数就出一帧，但**文档不动**。
 *
 * 引擎的 comp_filter_apply 会加入宿主已开的事务（owns_txn = !txn_active()），所以这里
 * 开事务 -> 应用一次 -> 先把这一帧画进 framebuffer -> abort 把像素回滚。文档因此始终停在
 * 「预览前」，预览只存在于画面里。相比「像色彩范围那样长期挂着事务」，这条路的好处是：
 *   - 关面板/换面板/换文档/切工具都不需要记忆并收拾一个半开事务（漏一处就会把别的
 *     命令的写入卷进同一个撤销步，甚至 abort 到已销毁的文档上）；
 *   - 预览失败的代价是「画面停在旧预览」，ram 而不是数据——最坏情况重画一帧即可。
 * 代价是回车真应用时同一组参数算两遍，这是明知故犯：结果正确比少算一次重要。
 *
 * 返回 true = 画面已经是本组参数的预览帧。状态行一律由这里写（含失败与恒等）。
 */
bool filter_preview_render(ShellState& shell, const wchar_t* lead) {
  if (!shell.doc.filter_open) return false;
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return false;
  }
  const comp_layer_id id = selected_layer_id(shell);
  const comp_ui::FilterSpec* spec = shell.doc.filter.spec();
  if (id == 0 || spec == nullptr) {
    set_status(shell, L"滤镜预览：请先用 ↑↓ 选择一个图层");
    return false;
  }

  comp_filter_desc desc{};
  build_filter_desc(shell, &desc);

  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.doc.document, "Filter Preview", &txn) != COMP_OK) {
    /* 没有事务就没有回滚：宁可不预览，也不把像素留在半改状态。 */
    filter_preview_forget(shell);
    set_status(shell, L"滤镜预览：无法开启事务，像素未改动（回车仍可直接应用）");
    return false;
  }

  int32_t changed = 0;
  char note[512] = {0};
  size_t note_size = 0;
  int32_t smart = 0;
  const bool smart_layer =
      comp_layer_smart_object_is_smart(shell.doc.document, id, &smart) == COMP_OK && smart != 0;
  comp_result result = COMP_OK;
  if (editing) {
    /* 智能图层：预览 = 在事务里往栈尾加一条，取帧后 abort。整条栈都从 source 重算，
     * 所以看到的就是「真应用后」的画面；abort 把像素与栈一起回滚，文档不动。 */
    changed = 1;
    result = comp_layer_smart_filter_add(shell.doc.document, id, &desc);
  } else {
    result = comp_filter_apply(shell.doc.document, id, &desc, 1.0, &changed, note,
                               sizeof(note), &note_size);
  }
  bool drawn = false;
  if (result == COMP_OK && changed != 0) {
    drawn = render_canvas(shell); /* 必须在 abort 之前取帧：此时像素还是「应用后」 */
  }
  comp_txn_abort(txn);

  if (!drawn) {
    /* 这一帧不能留着上一组参数的预览：参数已经变了，旧预览是错的。 */
    filter_preview_forget(shell);
    wchar_t buffer[384];
    if (result != COMP_OK) {
      wsprintfW(buffer, L"%s · 预览失败，像素未改动", lead);
    } else if (changed == 0) {
      wsprintfW(buffer, L"%s · 参数为恒等，无预览", lead);
    } else {
      wsprintfW(buffer, L"%s · 预览渲染失败，像素未改动", lead);
    }
    set_status(shell, buffer);
    return false;
  }

  /* render_canvas 已清掉 needs_render：这一帧是预览，下次绘制别从文档覆盖它。 */
  shell.doc.filter_preview_pending = true;
  wchar_t buffer[384];
  wsprintfW(buffer, L"%s · 预览中（回车应用 · Esc 取消）", lead);
  set_status(shell, buffer);
  return true;
}

void apply_filter(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  if (id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }

  comp_filter_desc desc{};

  int32_t changed = 0;
  char note[512] = {0};
  size_t note_size = 0;
    /* 单条编辑：把面板值写回第 edit_index 条。引擎幂等——设置与启用状态都没变时
     * COMP_OK 且不落撤销步，所以状态行不能承诺「一步撤销」。 */
    result = comp_layer_smart_filter_set(shell.doc.document, id, edit_index, &desc, edit_enabled);
  } else if (smart_layer) {
    /*
     * P1-2 收尾 ⑦：智能图层的像素是求值缓存，comp_filter_apply 会（正确地）拒绝。
     * 同一条路径在这里改走「往栈里加一条」——与 PS 一样：同一个面板、同一次回车，
     * 普通层是破坏性应用，智能层是叠加一条可随时删掉的智能滤镜。
     * changed 记 1：栈确实变了（恒等参数也照常入栈，便于之后调参数）。
     */
    changed = 1;
    result = comp_layer_smart_filter_add(shell.doc.document, id, &desc);
  } else {
    /* 全分辨率渲染，scale = 1：高斯半径不缩放。 */
    result = comp_filter_apply(shell.doc.document, id, &desc, 1.0, &changed, note,
                               sizeof(note), &note_size);
  }
  shell.doc.needs_render = true;
  shell.doc.filter_preview_pending = false; /* 真应用已落文档：画面重画即真状态 */

  wchar_t buffer[768];
  const std::wstring name = widen_ascii(spec->name);
  if (result != COMP_OK) {
    if (editing) {
      /* 绑定留着：用户还能改参数重试，或 Esc 退出（退出时同样回到栈列表）。 */
      wsprintfW(buffer, L"%s：写回第 %d 条失败（未改动）", name.c_str(),
                static_cast<int>(edit_index + 1));
    } else {
      wsprintfW(buffer, L"%s 失败", name.c_str());
    }
    set_status(shell, buffer);
    return;
  }
  if (editing) {
    return;
  }
  if (changed == 0) {
    /* 恒等（例如 Amount 仍为 0）：引擎不碰像素、不开事务，这里如实回报。 */
    wsprintfW(buffer, L"%s：参数为恒等，未改动", name.c_str());
    set_status(shell, buffer);
    return;
  }
  if (smart_layer) {
    size_t count = 0;
    if (comp_layer_smart_filter_count(shell.doc.document, id, &count) != COMP_OK) count = 0;
    refresh_layers(shell); /* 行内 S 标记与滤镜条数同源，立即反映 */
    wsprintfW(buffer, L"%s：已添加为智能滤镜（共 %d 条 · 一步撤销 · 图层 > 智能对象 可清除）",
              name.c_str(), static_cast<int>(count));
    refresh_history_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"%s：已应用｜%s", name.c_str(), widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"%s：已应用", name.c_str());
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 修边面板 */

void trim_begin(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"修边：还没有文档");
    return;
  }
  /* 右栏一次只挂一个面板（与滤镜/效果/路径/历史/色彩范围互斥）。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.path_open = false;
  shell.doc.history_open = false;
  channel_panel_close(shell);
  shell.doc.trim_open = true;
  shell.doc.trim_input.clear();
  /* 参数保留上次设置（上游 Trim 对话框同样记住上次），只把焦点放回第一行。 */
  shell.doc.trim.focused = 0;
  set_status(shell, L"修边：Tab 选行 · -/= 调值 · 回车应用 · Esc 关闭（撤销一步）");
}

void trim_close(ShellState& shell) {
  shell.doc.trim_open = false;
  shell.doc.trim_input.clear();
  set_status(shell, L"已关闭修边面板（未改动画布）");
}

void trim_apply(ShellState& shell) {
  if (!shell.doc.trim_open || shell.doc.document == nullptr) return;
  if (!shell.doc.trim.any_edge()) {
    /* 引擎也会拒（trimsAny guard），但状态栏要说得比错误码具体。 */
    set_status(shell, L"修边：至少勾选一条边（当前四条边都关）");
    return;
  }
  comp_trim_options options{};
  build_trim_options(shell, &options);
  int32_t changed = 0;
  const comp_result result = comp_document_trim(shell.doc.document, options, &changed);
  if (result == COMP_ERR_INVALID_ARGUMENT) {
    set_status(shell, L"修边失败：选项不合法（未改动）");
    return;
  }
  if (result == COMP_ERR_STATE) {
    set_status(shell, L"修边失败：空文档、或整篇没有可保留的内容（未改动）");
    return;
  }
  if (result != COMP_OK) {
    set_status(shell, L"修边失败（未改动）");
    return;
  }
  if (changed == 0) {
    /* 算出来的边框与当前画布相同：引擎不裁、不进历史；面板留着让用户改参数。 */
    set_status(shell, L"修边后画布尺寸未变（未产生撤销步）");
    return;
  }
  shell.doc.trim_open = false;
  shell.doc.trim_input.clear();
  finish_canvas_command(shell);
  wchar_t buffer[288];
  wsprintfW(buffer, L"已修边（基于 %s · 容差 %d）",
            widen_ascii(comp_ui::trim_based_on_name(shell.doc.trim.based_on)).c_str(),
            shell.doc.trim.tolerance);
  refresh_history_status(shell, buffer);
}

void trim_focus_row(ShellState& shell, int delta) {
  if (!shell.doc.trim_open) return;
  shell.doc.trim_input.clear();
  shell.doc.trim.focus(delta);
  wchar_t buffer[192];
  wsprintfW(buffer, L"修边 %d/%d：%s", shell.doc.trim.focused + 1, shell.doc.trim.row_count(),
            widen_ascii(comp_ui::trim_row_label(shell.doc.trim.focused)).c_str());
  set_status(shell, buffer);
}

/* - / = ：按当前焦点行步进（基于方式循环 / 四条边开关 / 容差 ±1）。 */
void trim_step_value(ShellState& shell, int direction) {
  if (!shell.doc.trim_open) return;
  if (!shell.doc.trim.step(direction)) {
    set_status(shell, L"该行没有可继续变化的值");
    return;
  }
  wchar_t buffer[192];
  const int row = shell.doc.trim.focused;
  if (row == 0) {
    wsprintfW(buffer, L"Based On = %s",
              widen_ascii(comp_ui::trim_based_on_name(shell.doc.trim.based_on)).c_str());
  } else if (row >= 1 && row <= 4) {
    wsprintfW(buffer, L"%s = %s", widen_ascii(comp_ui::trim_row_label(row)).c_str(),
              shell.doc.trim.edge[row - 1] ? L"on" : L"off");
  } else {
    wsprintfW(buffer, L"Tolerance = %d（逐通道 0..255）", shell.doc.trim.tolerance);
  }
  set_status(shell, buffer);
}

void trim_begin_input(ShellState& shell, wchar_t character) {
  if (!shell.doc.trim_open) return;
  if (shell.doc.trim_input.size() >= 3) return; /* 0..255，三位够了 */
  shell.doc.trim_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.doc.trim_input.c_str());
  set_status(shell, buffer);
}

void trim_commit_input(ShellState& shell) {
  if (shell.doc.trim_input.empty()) return;
  int value = 0;
  for (const wchar_t character : shell.doc.trim_input) {
    value = value * 10 + static_cast<int>(character - L'0');
  }
  shell.doc.trim_input.clear();
  shell.doc.trim.set_tolerance(value);
  wchar_t buffer[160];
  wsprintfW(buffer, L"Tolerance = %d（逐通道 0..255）", shell.doc.trim.tolerance);
  set_status(shell, buffer);
}

}  // namespace comp_shell
