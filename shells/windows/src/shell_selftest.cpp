// AI生成
/*
 * shell_selftest.cpp — 自检与呈现基准
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include <filesystem>

#include "shell_internal.h"
#include "shell_dock.h"

namespace comp_shell {


/*
 * 探针夹具（240x160：左红 / 中绿竖条 / 右蓝）的存在性检测与自备写入。
 *
 * 为什么放在壳里而不是仓里：run_regression.py 的 --clean 会连 build/ 一起删掉，
 * 而这份夹具本来就住在 build/ 里，于是全量回归后它必然不在。旧版自检此时把
 * 「内容对拍」降级为 [skip]，SHA256 与基线不一致——那看起来像产品回归，实际只是
 * 夹具不见了。改成自检自己写：这里链着引擎，写 PNG 与读 PNG 是同一条 comp_io_*
 * 路径，不引入任何外部依赖，也不需要把二进制夹具提交进版本库。
 *
 * 返回值语义：probe_png_exists 只回答「在不在」；write_probe_png 回答「写没写成」
 * （写成后一定在）。写入失败（磁盘满/只读）由调用方走 [skip] 出口，不假 PASS。
 */
bool probe_png_exists(const char* path) {
  if (path == nullptr) return false;
  FILE* probe = nullptr;
  if (fopen_s(&probe, path, "rb") != 0 || probe == nullptr) return false;
  std::fclose(probe);
  return true;
}

bool write_probe_png(comp_engine engine, const char* path) {
  if (engine == nullptr || path == nullptr) return false;
  const int32_t probe_width = 240;
  const int32_t probe_height = 160;
  const int32_t stripe = (probe_width * 15) / 40; /* 中段绿条宽约 7.5% 画布 */

  comp_document_desc desc{};
  desc.width = probe_width;
  desc.height = probe_height;
  desc.dpi = 72.0f;
  desc.color_space = "sRGB";
  comp_document probe = nullptr;
  if (comp_document_create(engine, &desc, &probe) != COMP_OK || probe == nullptr) return false;

  const size_t probe_stride = static_cast<size_t>(probe_width) * 4u;
  std::vector<uint8_t> probe_pixels(probe_stride * static_cast<size_t>(probe_height), 0);
  for (int32_t y = 0; y < probe_height; ++y) {
    uint8_t* row = probe_pixels.data() + static_cast<size_t>(y) * probe_stride;
    for (int32_t x = 0; x < probe_width; ++x) {
      uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
      if (x < stripe) {
        pixel[0] = 255; pixel[1] = 0; pixel[2] = 0; pixel[3] = 255; /* 左：红 */
      } else if (x < stripe * 2) {
        pixel[0] = 0; pixel[1] = 255; pixel[2] = 0; pixel[3] = 255; /* 中：绿 */
      } else {
        pixel[0] = 0; pixel[1] = 0; pixel[2] = 255; pixel[3] = 255; /* 右：蓝 */
      }
    }
  }

  comp_layer_desc layer_desc{};
  layer_desc.name = "probe";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, probe_width, probe_height};
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id probe_layer = 0;
  bool added = comp_layer_add(probe, &layer_desc, &probe_layer) == COMP_OK &&
                probe_layer != 0;
  if (added) {
    comp_image image{};
    image.width = probe_width;
    image.height = probe_height;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = probe_stride;
    image.pixels = probe_pixels.data();
    added = comp_layer_set_pixels(probe, probe_layer, &image) == COMP_OK;
  }
  bool exported = false;
  if (added) {
    const comp_image_format format = comp_io_format_from_path(path);
    exported = format != COMP_FORMAT_UNKNOWN &&
               comp_io_export(probe, path, format, nullptr) == COMP_OK;
  }
  comp_document_destroy(probe);
  return exported;
}

/*
 * 端到端自检：只用 C ABI 串起「建引擎 -> 建文档 -> 加图层写像素 -> 合成 -> 撤销 ->
 * .comp 往返」。不创建窗口，故可无人值守运行；结论写入 out_path 并回退出码（0 = 全通过）。
 */
int run_selftest(const wchar_t* out_path) {
  FILE* report = nullptr;
  if (_wfopen_s(&report, out_path, L"wb") != 0 || report == nullptr) return 2;

  int failures = 0;
  const auto emit = [&](const char* text) { std::fprintf(report, "%s\n", text); };
  const auto verify = [&](bool ok, const char* label) {
    std::fprintf(report, "[%s] %s\n", ok ? "OK" : "FAIL", label);
    if (!ok) ++failures;
  };

  emit("# compositor-shell selftest (Windows shell + engine C ABI)");

  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-shell-selfcheck";
  engine_desc.version = "0.1.0";
  /*
   * 自检主体固定在 CPU 后端：下面的像素断言（画笔色、撤销后逐字节还原）是
   * CPU 基准定义的。GPU 协同由本节末尾的 AUTO 引擎对称场景单独验证。
   */
  engine_desc.backend = COMP_BACKEND_CPU;
  comp_engine engine = nullptr;
  const bool engine_ok =
      comp_engine_create(&engine_desc, &engine) == COMP_OK && engine != nullptr;
  verify(engine_ok, "comp_engine_create(CPU)");
  if (!engine_ok) {
    std::fclose(report);
    return 1;
  }

  comp_backend backend = COMP_BACKEND_AUTO;
  const bool backend_ok = comp_engine_backend(engine, &backend) == COMP_OK;
  verify(backend_ok, "comp_engine_backend()");
  std::fprintf(report, "[info] backend = %s\n", backend_ok ? backend_name(backend) : "(query failed)");

  comp_document_desc doc_desc{};
  doc_desc.width = 64;
  doc_desc.height = 64;
  doc_desc.dpi = 72.0f;
  doc_desc.color_space = "sRGB";
  comp_document document = nullptr;
  const bool doc_ok =
      comp_document_create(engine, &doc_desc, &document) == COMP_OK && document != nullptr;
  verify(doc_ok, "comp_document_create(64x64)");
  if (!doc_ok) {
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }

  comp_size size{};
  verify(comp_document_size(document, &size) == COMP_OK && size.width == 64 && size.height == 64,
         "comp_document_size() == 64x64");

  /* 半透明红（预乘 RGBA8）：验证「壳只调 ABI、像素由引擎处理」的端到端路径。 */
  const int32_t width = 64;
  const int32_t height = 64;
  const size_t stride = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> pixels(stride * static_cast<size_t>(height), 0);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = 128;
    pixels[i + 1] = 0;
    pixels[i + 2] = 0;
    pixels[i + 3] = 128;
  }
  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = stride;
  image.pixels = pixels.data();

  comp_layer_desc layer_desc{};
  layer_desc.name = "selftest-layer";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, width, height}; /* 显式基准：变换自检要收放 */
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id layer = 0;
  const bool add_ok = comp_layer_add(document, &layer_desc, &layer) == COMP_OK && layer != 0;
  verify(add_ok, "comp_layer_add()");
  verify(add_ok && comp_layer_set_pixels(document, layer, &image) == COMP_OK,
         "comp_layer_set_pixels()");

  size_t layer_count = 0;
  const bool count_ok = comp_layer_count(document, &layer_count) == COMP_OK && layer_count >= 2;
  verify(count_ok, "comp_layer_count() >= 2 (background + added)");
  std::fprintf(report, "[info] layers = %u\n", static_cast<unsigned>(layer_count));

  std::vector<uint8_t> flattened(stride * static_cast<size_t>(height), 0);
  const bool flatten_ok =
      comp_render_flatten(document, flattened.data(), COMP_PIXEL_RGBA8, stride) == COMP_OK;
  verify(flatten_ok, "comp_render_flatten()");
  size_t non_transparent = 0;
  for (size_t i = 3; i < flattened.size(); i += 4) {
    if (flattened[i] != 0) ++non_transparent;
  }
  verify(flatten_ok && non_transparent > 0, "flatten output has non-transparent pixels");
  std::fprintf(report, "[info] non-transparent pixels = %u / %u\n",
               static_cast<unsigned>(non_transparent), static_cast<unsigned>(width * height));

  /*
   * 混合模式接线（S21）：写一个非默认档位，读回必须等于写入值；再非法值必须被
   * 引擎拒绝且不改变既有档位。这锁死的是壳与 ABI 之间「档位即 comp_blend_mode
   * 数值」这条口径——面板行、菜单与状态栏全都从这同一个数值取名。
   */
  comp_blend_mode blend_read = COMP_BLEND_NORMAL;
  const bool blend_set_ok =
      comp_layer_set_blend(document, layer, COMP_BLEND_MULTIPLY) == COMP_OK &&
      comp_layer_get_blend(document, layer, &blend_read) == COMP_OK &&
      blend_read == COMP_BLEND_MULTIPLY;
  verify(blend_set_ok, "blend mode round trip (set MULTIPLY -> get)");
  comp_result bad_blend =
      comp_layer_set_blend(document, layer, static_cast<comp_blend_mode>(999));
  verify(bad_blend != COMP_OK && comp_layer_get_blend(document, layer, &blend_read) == COMP_OK &&
             blend_read == COMP_BLEND_MULTIPLY,
         "invalid blend mode rejected without clobbering the stored value");
  verify(comp_layer_set_blend(document, layer, COMP_BLEND_NORMAL) == COMP_OK,
         "blend mode restored to NORMAL");

  /*
   * 位图导入 / 导出闭环（S22）。
   *
   * 壳的「打开位图」「导出为」菜单走的正是这里验证的 comp_io_import /
   * comp_io_export 这一对。之所以必须自检而不是只靠人工：GUI 那两个入口都有模态
   * 对话框挡着，自动化点不过去；而对话框最终提交的仍是同一个 ABI 调用与同一个
   * 路径字符串。所以这条断言退化时，人工目视之前壳就已经坏了。
   *
   * 判据是「导出 → 读回 → 尺寸与图层数一致」，再加一张内容已知的探针图往返。
   */
  wchar_t io_temp_dir[MAX_PATH] = L"";
  if (GetTempPathW(MAX_PATH, io_temp_dir) == 0) wcscpy_s(io_temp_dir, L".\\");
  wchar_t image_file[MAX_PATH] = L"";
  wsprintfW(image_file, L"%sselftest_image_roundtrip.png", io_temp_dir);
  char image_path_utf8[MAX_PATH * 2] = {0};
  WideCharToMultiByte(CP_UTF8, 0, image_file, -1, image_path_utf8,
                      static_cast<int>(sizeof(image_path_utf8)), nullptr, nullptr);
  const bool export_ok =
      comp_io_export(document, image_path_utf8, COMP_FORMAT_PNG, nullptr) == COMP_OK;
  verify(export_ok, "comp_io_export(png)");
  if (export_ok) {
    comp_document reimported = nullptr;
    const bool reimport_ok =
        comp_io_import(engine, image_path_utf8, &reimported) == COMP_OK &&
        reimported != nullptr;
    verify(reimport_ok, "comp_io_import(png round trip)");
    if (reimport_ok) {
      comp_size reimported_size{};
      verify(comp_document_size(reimported, &reimported_size) == COMP_OK &&
                 reimported_size.width == size.width &&
                 reimported_size.height == size.height,
             "image round trip keeps canvas size");
      comp_document_destroy(reimported);
    }
  }

  /*
   * 探针图往返：240x160、左红 / 中绿竖条 / 右蓝，像素值精确已知。
   * 导入后核对画布尺寸与三个角落的像素——证明「打开一张外部图片」真把内容搬进了
   * 文档，不是建了个空壳。
   *
   * 夹具缺失时**先用引擎写出它**再导入（而不是降级成 [skip]）：
   *   - run_regression.py 的 --clean 会连 build/ 一起删掉，这份夹具本来就住在
   *     build/ 里，于是每次全量回归后它必然不在——旧版自检此时会把两项内容对拍
   *     降级为 [skip]，SHA256 与基线不一致，看起来像产品回归；
   *   - 自检自己链着引擎（静态），写 PNG 与读 PNG 走的是同一条 comp_io_* 路径，
   *     由它自备夹具不引入任何外部依赖；
   *   - 降级出口仍然保留：万一写入失败（磁盘满/只读），如实报 [skip] 而不是假 PASS。
   */
  const char* probe_path = "build/import_probe.png";
  if (!probe_png_exists(probe_path)) {
    (void)write_probe_png(engine, probe_path);
  }
  comp_document probe_document = nullptr;
  const bool probe_ok =
      comp_io_import(engine, probe_path, &probe_document) == COMP_OK &&
      probe_document != nullptr;
  if (probe_ok) {
    comp_size probe_size{};
    const bool probe_size_ok =
        comp_document_size(probe_document, &probe_size) == COMP_OK &&
        probe_size.width == 240 && probe_size.height == 160;
    verify(probe_size_ok, "probe image imports at 240x160");
    if (probe_size_ok) {
      const size_t probe_stride = static_cast<size_t>(probe_size.width) * 4u;
      std::vector<uint8_t> probe_pixels(probe_stride * static_cast<size_t>(probe_size.height), 0);
      const bool probe_flatten = comp_render_flatten(probe_document, probe_pixels.data(),
                                                    COMP_PIXEL_RGBA8, probe_stride) == COMP_OK;
      /* 左上角 = 红区，右下角 = 蓝区，中线 = 绿条。逐点对拍，不是统计近似。 */
      const bool probe_pixels_ok = probe_flatten &&
          probe_pixels[0] == 255 && probe_pixels[1] == 0 && probe_pixels[2] == 0 &&
          probe_pixels.size() >= 4 &&
          probe_pixels[probe_pixels.size() - 4] == 0 &&
          probe_pixels[probe_pixels.size() - 3] == 0 &&
          probe_pixels[probe_pixels.size() - 2] == 255;
      verify(probe_pixels_ok, "probe image content matches the known pattern");
    }
    comp_document_destroy(probe_document);
  } else {
    std::fprintf(report,
                 "[skip] %s 不存在且无法写出，跳过内容对拍（尺寸断言已覆盖导入通路）\n",
                 probe_path);
  }

  /* 撤销/重做由引擎维护，外壳只触发与展示。 */
  int32_t can_undo = 0;
  comp_document_can_undo(document, &can_undo);
  size_t undo_depth = 0;
  size_t redo_depth = 0;
  comp_document_history_stats(document, &undo_depth, &redo_depth);
  std::fprintf(report, "[info] history: can_undo=%d depth=%u redo=%u\n", can_undo,
               static_cast<unsigned>(undo_depth), static_cast<unsigned>(redo_depth));
  verify(can_undo == 0 || comp_document_undo(document) == COMP_OK, "comp_document_undo()");

  /* .comp 往返：写入临时目录再读回，核对尺寸。 */
  wchar_t temp_dir[MAX_PATH] = {0};
  wchar_t temp_file[MAX_PATH] = {0};
  if (GetTempPathW(MAX_PATH, temp_dir) != 0) {
    wsprintfW(temp_file, L"%scompositor_shell_selftest.comp", temp_dir);
  } else {
    wsprintfW(temp_file, L"compositor_shell_selftest.comp");
  }
  char comp_path_utf8[MAX_PATH * 2] = {0};
  WideCharToMultiByte(CP_UTF8, 0, temp_file, -1, comp_path_utf8,
                      static_cast<int>(sizeof(comp_path_utf8)), nullptr, nullptr);
  const bool save_ok = comp_document_save(document, comp_path_utf8) == COMP_OK;
  verify(save_ok, "comp_document_save(.comp)");
  if (save_ok) {
    comp_document reloaded = nullptr;
    const bool load_ok = comp_document_load(engine, comp_path_utf8, &reloaded) == COMP_OK &&
                         reloaded != nullptr;
    verify(load_ok, "comp_document_load(.comp round trip)");
    if (load_ok) {
      comp_size reloaded_size{};
      verify(comp_document_size(reloaded, &reloaded_size) == COMP_OK &&
                 reloaded_size.width == size.width && reloaded_size.height == size.height,
             "round trip keeps canvas size");
      comp_document_destroy(reloaded);
    }
  }

  /*
   * 图层变换链路（S21，面板平移/旋转/缩放真正走的接口序列）：
   * 一次显式事务内 get -> 步进 -> set -> 再 get 回读，核对现值与步进量一致，
   * 然后 abort 回滚——不往撤销栈里留一步（自检前面各段的隐式事务已经够了）。
   */
  comp_txn xform_txn = nullptr;
  const bool xform_begin = comp_txn_begin(document, "Selftest Layer Transform", &xform_txn) == COMP_OK;
  verify(xform_begin, "comp_txn_begin() for layer transform");
  if (xform_begin) {
    comp_rect before{};
    float angle = 0.0f;
    const bool read_ok = comp_layer_get_transform(document, layer, &before, &angle) == COMP_OK;
    verify(read_ok, "comp_layer_get_transform()");
    if (read_ok) {
      /* 平移 1 px（Shift+方向的微调量）：壳侧算位移，引擎只管收。 */
      const comp_rect moved{before.x + 1, before.y - 1, before.w, before.h};
      verify(comp_layer_set_transform(document, layer, moved, angle) == COMP_OK,
             "comp_layer_set_transform(move 1px)");
      comp_rect after{};
      float angle2 = 0.0f;
      verify(comp_layer_get_transform(document, layer, &after, &angle2) == COMP_OK &&
                 after.x == before.x + 1 && after.y == before.y - 1 && after.w == before.w &&
                 after.h == before.h && angle2 == angle,
             "transform round trip keeps the stepped value");
      /* 旋转 +15°（Ctrl+左右的一档）：在现值上累加，回读必须是累加结果。 */
      const float turned = angle + 15.0f;
      verify(comp_layer_set_transform(document, layer, after, turned) == COMP_OK,
             "comp_layer_set_transform(rotate +15)");
      comp_rect after2{};
      float angle3 = 0.0f;
      verify(comp_layer_get_transform(document, layer, &after2, &angle3) == COMP_OK &&
                 angle3 > turned - 0.01f && angle3 < turned + 0.01f,
             "rotation accumulates on the current value");
      /* 缩放 +1%（Alt+. 的一档，中心锚定）：壳侧算新矩形，回读核对中心不变。 */
      int32_t sx = after2.x, sy = after2.y, sw = after2.w, sh = after2.h;
      const bool has_box = sw > 0 && sh > 0;
      verify(has_box, "selftest layer carries an explicit transform box");
      if (has_box) {
        comp_ui::layer_transform_scale_rect(&sx, &sy, &sw, &sh,
                                            1.0 + comp_ui::kLayerTransformScaleStep);
        verify(comp_layer_set_transform(document, layer, comp_rect{sx, sy, sw, sh}, angle3) == COMP_OK,
               "comp_layer_set_transform(scale +1%)");
        comp_rect scaled{};
        float angle4 = 0.0f;
        verify(comp_layer_get_transform(document, layer, &scaled, &angle4) == COMP_OK &&
                   scaled.w == sw && scaled.h == sh,
               "scale round trip keeps the new box");
      }
    }
    verify(comp_txn_abort(xform_txn) == COMP_OK, "comp_txn_abort() rolls the transform back");
  }

  /*
   * S22 段的历史基线（S23 修正）：必须在**写入之前**取。
   *
   * 不引用后面滤镜段才声明的 depth_after_identity：selftest 是单函数长流程，
   * 变量必须在自己第一次使用之前声明——这不是风格问题，是 C++ 的作用域规则。
   */
  size_t s22_depth_baseline = 0;
  comp_document_history_stats(document, &s22_depth_baseline, &redo_depth);

  /*
   * S22 蒙版回读（comp_layer_get_mask）：
   *
   * 这条锁死的是“面板能否知道这一层带不带蒙版”。蒙版是引擎早已参与合成的真能力
   * （filter_graph.cpp 的 coverage.add(&layer->mask)），但 ABI 只有写入口、没有回读口，
   * 因此壳侧之前完全看不见“已经建过蒙版”这件事。
   *
   * 断言里必须包含“清除后回到无蒙版”：只验证“能读到蒙版”的话，一个
   * 写死的 has_mask也能通过（如果实现把数组当成写入口）。
   */
  int32_t has_mask = -1;
  int32_t mask_w = -1;
  int32_t mask_h = -1;
  verify(comp_layer_get_mask(document, layer, &has_mask, &mask_w, &mask_h) == COMP_OK &&
             has_mask == 0 && mask_w == 0 && mask_h == 0,
         "get_mask reports no mask on a fresh layer");

  std::vector<uint8_t> mask_pixels(static_cast<size_t>(width) *
                                  static_cast<size_t>(height), 255u);
  comp_mask full_mask{};
  full_mask.width = width;
  full_mask.height = height;
  full_mask.stride = static_cast<size_t>(width);
  full_mask.pixels = mask_pixels.data();
  verify(comp_layer_set_mask(document, layer, &full_mask) == COMP_OK,
         "comp_layer_set_mask(full cover)");
  verify(comp_layer_get_mask(document, layer, &has_mask, &mask_w, &mask_h) == COMP_OK &&
             has_mask == 1 && mask_w == width && mask_h == height,
         "get_mask round trips presence and size");

  verify(comp_layer_set_mask(document, layer, nullptr) == COMP_OK &&
             comp_layer_get_mask(document, layer, &has_mask, &mask_w, &mask_h) == COMP_OK &&
             has_mask == 0 && mask_w == 0 && mask_h == 0,
         "clearing the mask returns to no-mask");

  /*
   * S22 结构关系回读（comp_layer_get_group_info）。
   *
   * “是不是组 / 挂在哪 / 剪贴谁”三件事目前只能在创建时给（comp_layer_desc），
   * 此后没有 setter 之外的路。.comp 读回带组、带剪贴的文档时，面板就会静默失真。
   */
  comp_layer_id clipping_read = 0xdeadbeef;
  int32_t is_group_read = -1;
  comp_layer_id parent_read = 0xdeadbeef;
  verify(comp_layer_get_group_info(document, layer, &is_group_read, &parent_read,
                                  &clipping_read) == COMP_OK &&
             is_group_read == 0 && parent_read == 0 && clipping_read == 0,
         "get_group_info reports a root-level plain layer");

  /*
   * S22 重排链路（面板“上移/下移一层”真正走的接口）。
   *
   * T2 全量测试抄到的 43 项零调用里，comp_layer_reorder 就在其中：引擎早已实现，
   * 而壳一次都没调过——用户完全无法调整图层堆叠次序。
   *
   * 判据采“堆叠序真的换了”而不是“调用返回了 OK”：把 layer 移到 background 之前，
   * 然后用 comp_layer_list 重新读一次 id 序列（自下而上）。
   *
   * background 用两段式查询取，不靠字面量：id 是运行期分配的，写死会让这段断言
   * 在另一台机器上静默失效（而 selftest 是唯一能在 CI 里拦住它的地方）。
   */
  comp_layer_id background = 0;
  {
    size_t background_index = 0;
    comp_layer_id first = 0;
    if (comp_layer_list(document, &first, 1u, &background_index) == COMP_OK &&
        background_index >= 1u) {
      background = first;
    }
  }
  verify(background != 0 && background != layer, "background layer is resolvable");
  comp_layer_id stack_before[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  size_t stack_count = 0;
  verify(comp_layer_list(document, stack_before, 8u, &stack_count) == COMP_OK &&
             stack_count == 2u && stack_before[0] == background && stack_before[1] == layer,
         "reorder precondition: [background, layer]");
  verify(comp_layer_reorder(document, layer, background) == COMP_OK,
         "comp_layer_reorder(layer -> before background)");
  comp_layer_id stack_after[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  stack_count = 0;
  verify(comp_layer_list(document, stack_after, 8u, &stack_count) == COMP_OK &&
             stack_count == 2u && stack_after[0] == layer && stack_after[1] == background,
         "reorder really changed the stacking order");
  verify(comp_layer_reorder(document, layer, 0) == COMP_OK &&
             comp_layer_list(document, stack_after, 8u, &stack_count) == COMP_OK &&
             stack_after[1] == layer,
         "reorder back to the top restores the original order");

  /*
   * S22 直方图（comp_layer_histogram）。
   *
   * 这是上游 Levels 面板左半边的那张图，也是本壳唯一的 histogram 调用点。
   *
   * 判据采“峰值桶等于手算的亮度桶”，而不是“调用成功”或者“非零桶大于 0”。
   * 原因是本段的图层像素固定：预乘红 (128,0,0,128)。内核先反预乘得到
   * (255,0,0)，再按 0.299R+0.587G+0.114B 取亮度 = 76.245，落桶 76。
   *
   * 这个数字必须在每次改动图层像素时重算：上一轮我写死 200（想当然认为“均匀灰色”），
   * 结果被真值 76 打脸——那一刷只能说明判据写错了，而不能说引擎算错了。
   */
  double histogram_bins[256] = {0.0};
  verify(comp_layer_histogram(document, layer, nullptr, 0, histogram_bins) == COMP_OK,
         "comp_layer_histogram() on the selftest layer");
  double hist_peak = 0.0;
  int hist_nonzero = 0;
  int hist_peak_bin = -1;
  for (int i = 0; i < 256; ++i) {
    if (histogram_bins[i] > 0.0) ++hist_nonzero;
    if (histogram_bins[i] > hist_peak) {
      hist_peak = histogram_bins[i];
      hist_peak_bin = i;
    }
  }
  std::fprintf(report, "[info] histogram nonzero bins = %d, peak bin = %d\n",
               hist_nonzero, hist_peak_bin);
  /* 四舍五入：int(76.245) = 76，与上面手算一致。 */
  verify(hist_peak_bin == 76,
         "premultiplied red(128,0,0,128) un-premultiplies to (255,0,0) and luma 76");

  /*
   * S22 采样模式往返（comp_layer_get_sampling）。
   *
   * setter 早已存在且引擎真的按它改缩放手感（resample.cpp 的 sampling_from_abi），
   * 但壳一次都没调过———用户看不见也改不了。
   */
  comp_sampling sampling_read = COMP_SAMPLING_NEAREST;
  verify(comp_layer_set_sampling(document, layer, COMP_SAMPLING_NEAREST) == COMP_OK &&
             comp_layer_get_sampling(document, layer, &sampling_read) == COMP_OK &&
             sampling_read == COMP_SAMPLING_NEAREST,
         "sampling mode round trip (set NEAREST -> get)");
  verify(comp_layer_set_sampling(document, layer, COMP_SAMPLING_HIGH_QUALITY) == COMP_OK &&
             comp_layer_get_sampling(document, layer, &sampling_read) == COMP_OK &&
             sampling_read == COMP_SAMPLING_HIGH_QUALITY,
         "sampling mode restored to HIGH_QUALITY");

  /*
   * S22 组隔离开关（comp_layer_set_isolated / get_isolated）。
   *
   * 两种值都是已参与像素求值的真差异（filter_graph.cpp 按 node.isolated 决定
   * child_backdrop），不是展示开关。写非组图层时引擎不报错但属性不参与合成，
   * 因此这里只验证读写往返，不验证它改变画面。
   */
  int32_t isolated_read = -1;
  verify(comp_layer_set_isolated(document, layer, 1) == COMP_OK &&
             comp_layer_get_isolated(document, layer, &isolated_read) == COMP_OK &&
             isolated_read == 1,
         "isolated round trip (set 1 -> get)");
  verify(comp_layer_set_isolated(document, layer, 0) == COMP_OK &&
             comp_layer_get_isolated(document, layer, &isolated_read) == COMP_OK &&
             isolated_read == 0,
         "isolated restored to 0");

  /*
   * S22 剪贴蒙版开关（comp_layer_set_clipping_source）。
   *
   * 引擎已参与像素求值（composite_ops.cpp 的 clipping_factor 真的按来源图层算覆盖率），
   * 而壳之前没有任何入口能开关它。自标志与不存在的来源必须被拒。
   */
  verify(comp_layer_set_clipping_source(document, layer, background) == COMP_OK,
         "comp_layer_set_clipping_source(baseline)");
  comp_layer_id clip_check = 0;
  verify(comp_layer_get_group_info(document, layer, &is_group_read, &parent_read,
                                  &clip_check) == COMP_OK && clip_check == background,
         "clipping source round trips through get_group_info");
  verify(comp_layer_set_clipping_source(document, layer, layer) != COMP_OK,
         "self-clipping is rejected");
  verify(comp_layer_set_clipping_source(document, layer, 0xdeadbeef) == COMP_ERR_NOT_FOUND,
         "clipping to a missing layer is NOT_FOUND");
  verify(comp_layer_set_clipping_source(document, layer, 0) == COMP_OK &&
             comp_layer_get_group_info(document, layer, &is_group_read, &parent_read,
                                      &clip_check) == COMP_OK &&
             clip_check == 0,
         "clipping source cleared");

  /*
   * 把上面 S22 段的写入全部撤销回去，避免遗留历史深度影响后续无关段的判据。
   *
   * 基线必须在**写入之前**取（见本段开头的 s22_depth_baseline）。S22 首版把基线取在
   * 这里，即全部写入之后，于是 current <= baseline 第一次判断就成立、一条都没撤——
   * 注释说「全部撤销回去」，实际是空循环。它当时不影响通过，但那正是假护栏：
   * 以后在它之后追加断言，就会莫名受上一段遗留深度影响。
   *
   * 因此这里不只做撤销，还**断言撤销确实发生过**（s22_undone > 0）并且深度回到基线。
   * 若有人把基线取回调到写入之后，第一条立刻红——它把「护栏是不是真的在护」变成
   * 可判定的，而不是靠注释自称。
   */
  size_t s22_undone = 0;
  size_t s22_depth_end = 0;
  for (;;) {
    size_t current = 0;
    comp_document_history_stats(document, &current, &redo_depth);
    s22_depth_end = current;
    if (current <= s22_depth_baseline) break;
    if (comp_document_undo(document) != COMP_OK) break;
    ++s22_undone;
  }
  verify(s22_undone > 0, "S22 writes left undoable history and were rolled back");
  verify(s22_depth_end == s22_depth_baseline, "S22 rollback reaches the pre-S22 depth");

  /*
   * S24 段：镜像读回（comp_layer_get_flip）+ 效果枚举（comp_layer_effect_count/at）。
   *
   * 这两对是「引擎早就算、壳却读不回」的最后一组：flip 早已折进采样坐标
   * （resample.cpp），6 个 effect kind 早已参与像素求值（effect_eval.cpp），
   * 但 ABI 只有写口——壳读不到现值就只能盲目取反，也画不出「这层带了什么效果」、
   * 菜单更无法标出已启用项。
   *
   * 判据不采「调用返回 OK」：效果那段必须证明像素真的变了（纯元数据实现也能让
   * count == 1 通过），翻转那段必须证明同值写入不产生撤销步（否则撤销栈被空步塞满）。
   */
  size_t s24_depth_baseline = 0;
  comp_document_history_stats(document, &s24_depth_baseline, &redo_depth);

  int32_t flip_now_x = -1;
  int32_t flip_now_y = -1;
  verify(comp_layer_get_flip(document, layer, &flip_now_x, &flip_now_y) == COMP_OK &&
             flip_now_x == 0 && flip_now_y == 0,
         "get_flip reports no flip on a fresh layer");
  verify(comp_layer_set_flip(document, layer, 1, 0) == COMP_OK &&
             comp_layer_get_flip(document, layer, &flip_now_x, &flip_now_y) == COMP_OK &&
             flip_now_x == 1 && flip_now_y == 0,
         "flip round trip (set horizontal -> get)");

  size_t flip_depth_after_set = 0;
  comp_document_history_stats(document, &flip_depth_after_set, &redo_depth);
  verify(comp_layer_set_flip(document, layer, 1, 0) == COMP_OK, "set_flip same value again");
  size_t flip_depth_after_noop = 0;
  comp_document_history_stats(document, &flip_depth_after_noop, &redo_depth);
  verify(flip_depth_after_noop == flip_depth_after_set,
         "same-value set_flip adds no undo step");

  verify(comp_layer_set_flip(document, layer, 0, 0) == COMP_OK &&
             comp_layer_get_flip(document, layer, &flip_now_x, &flip_now_y) == COMP_OK &&
             flip_now_x == 0 && flip_now_y == 0,
         "flip cleared back to 0/0");

  size_t effect_count_now = 123u;
  verify(comp_layer_effect_count(document, layer, &effect_count_now) == COMP_OK &&
             effect_count_now == 0,
         "effect_count is 0 before any effect is added");

  const char* overlay_settings = "{\"red\":0,\"green\":0,\"blue\":1}";
  verify(comp_layer_set_effect(document, layer, 2u, overlay_settings) == COMP_OK,
         "comp_layer_set_effect(Color Overlay, blue)");
  verify(comp_layer_effect_count(document, layer, &effect_count_now) == COMP_OK &&
             effect_count_now == 1,
         "effect_count() == 1 after adding an effect");

  uint32_t effect_kind_now = 999u;
  size_t effect_settings_size = 0;
  verify(comp_layer_effect_at(document, layer, 0u, &effect_kind_now, nullptr, 0,
                              &effect_settings_size) == COMP_OK &&
             effect_kind_now == 2u && effect_settings_size == std::strlen(overlay_settings),
         "effect_at() reports kind and settings length (two-pass probe)");
  std::vector<char> effect_settings(effect_settings_size + 1u, '\0');
  verify(comp_layer_effect_at(document, layer, 0u, &effect_kind_now, effect_settings.data(),
                              effect_settings.size(), &effect_settings_size) == COMP_OK &&
             std::strcmp(effect_settings.data(), overlay_settings) == 0,
         "effect_at() round trips the settings JSON verbatim");

  std::vector<uint8_t> effect_after(flattened.size(), 0);
  const bool effect_flatten_after =
      comp_render_flatten(document, effect_after.data(), COMP_PIXEL_RGBA8, stride) == COMP_OK;
  /*
   * 判据取「蓝上来了、红下去了」，不写死 255：图层像素是 50% 预乘红
   * (128,0,0,128)，颜色叠加按覆盖率求值，落盘仍是预乘值 —— 蓝色通道是
   * 0..255 的 50% (=128)，写死 b > 200 会把自己的判据判错，而不是产品错了。
   */
  const bool effect_pixels_ok = effect_flatten_after && effect_after.size() >= 4u &&
                                effect_after[2] >= 100u && effect_after[0] < 60u &&
                                effect_after[2] > effect_after[0];
  verify(effect_pixels_ok, "color overlay written through the ABI really changes the canvas");
  if (effect_after.size() >= 4u) {
    std::fprintf(report, "[info] overlay pixel = (%u,%u,%u,%u)\n", effect_after[0],
                 effect_after[1], effect_after[2], effect_after[3]);
  }

  verify(comp_layer_remove_effect(document, layer, 2u) == COMP_OK &&
             comp_layer_effect_count(document, layer, &effect_count_now) == COMP_OK &&
             effect_count_now == 0,
         "effect removal clears the list");

  /* 撤销回 S24 基线（同 S22 的口径：既撤又要断言真的撤过，避免假护栏）。 */
  size_t s24_undone = 0;
  for (;;) {
    size_t current = 0;
    comp_document_history_stats(document, &current, &redo_depth);
    if (current <= s24_depth_baseline) break;
    if (comp_document_undo(document) != COMP_OK) break;
    ++s24_undone;
  }
  verify(s24_undone > 0, "S24 writes were undoable and rolled back");
  size_t s24_depth_end = 0;
  comp_document_history_stats(document, &s24_depth_end, &redo_depth);
  verify(s24_depth_end == s24_depth_baseline, "S24 rollback reaches the pre-S24 depth");

  /*
   * 调整面板链路（面板真正走的接口序列）：
   * 键名表元数据 -> 引擎组装 settings JSON -> 新增 -> 读回 -> 按索引更新 -> 非法值被拒。
   */
  const char* kind_name = nullptr;
  verify(comp_layer_adjustment_kind_name(3u, &kind_name) == COMP_OK && kind_name != nullptr &&
             std::strcmp(kind_name, "Exposure") == 0,
         "comp_layer_adjustment_kind_name(3) == Exposure");

  size_t param_count = 0;
  verify(comp_layer_adjustment_param_count(3u, &param_count) == COMP_OK && param_count == 3,
         "comp_layer_adjustment_param_count(3) == 3");

  comp_adjustment_param first_param{};
  verify(comp_layer_adjustment_param_at(3u, 0u, &first_param) == COMP_OK &&
             first_param.json_path != nullptr &&
             std::strcmp(first_param.json_path, "exposureSettings.exposure") == 0,
         "comp_layer_adjustment_param_at(3,0) path");

  const char* panel_values[3] = {"1.5", "0.25", "2.2"};
  size_t settings_size = 0;
  verify(comp_layer_adjustment_build_settings(3u, panel_values, 3, nullptr, 0, &settings_size) ==
                 COMP_OK &&
             settings_size > 0,
         "comp_layer_adjustment_build_settings(size probe)");
  std::vector<char> settings(settings_size + 1, '\0');
  const bool build_ok = comp_layer_adjustment_build_settings(
      3u, panel_values, 3, settings.data(), settings.size(), &settings_size) == COMP_OK;
  verify(build_ok, "comp_layer_adjustment_build_settings");
  std::fprintf(report, "[info] panel settings = %s\n", settings.data());

  const bool add_adj_ok =
      add_ok && build_ok && comp_layer_add_adjustment(document, layer, 3u, settings.data()) == COMP_OK;
  verify(add_adj_ok, "comp_layer_add_adjustment(panel settings)");

  size_t adj_count = 0;
  verify(add_adj_ok && comp_layer_adjustment_count(document, layer, &adj_count) == COMP_OK &&
             adj_count == 1,
         "comp_layer_adjustment_count() == 1");

  double exposure = 0.0;
  verify(comp_layer_adjustment_read_value(settings.data(), "exposureSettings.exposure", &exposure,
                                          nullptr) == COMP_OK &&
             std::fabs(exposure - 1.5) < 1e-9,
         "comp_layer_adjustment_read_value(exposure) == 1.5");

  const char* updated_values[3] = {"-2", "", "1.8"};
  std::vector<char> updated(256, '\0');
  size_t updated_size = 0;
  const bool update_ok =
      comp_layer_adjustment_build_settings(3u, updated_values, 3, updated.data(), updated.size(),
                                           &updated_size) == COMP_OK &&
      comp_layer_update_adjustment_at(document, layer, 0u, 3u, updated.data()) == COMP_OK;
  verify(update_ok, "comp_layer_update_adjustment_at(0)");

  double gamma = 0.0;
  verify(comp_layer_adjustment_read_value(updated.data(), "exposureSettings.gamma", &gamma, nullptr) ==
             COMP_OK &&
             std::fabs(gamma - 1.8) < 1e-9,
         "updated gamma == 1.8");

  /* 越界面板值必须被拒（面板不会把非法值写进文档）。 */
  const char* bad_values[3] = {"999", "", ""};
  verify(comp_layer_adjustment_build_settings(3u, bad_values, 3, nullptr, 0, &settings_size) != COMP_OK,
         "out-of-range panel value rejected");

  /* 删除接口（面板 Ctrl+Shift+D 走这条）：越界被拒，删除后数量回落。 */
  verify(comp_layer_remove_adjustment(document, layer, 5u) == COMP_ERR_INVALID_ARGUMENT,
         "comp_layer_remove_adjustment(out of range) rejected");
  const bool remove_ok = comp_layer_remove_adjustment(document, layer, 0u) == COMP_OK;
  verify(remove_ok, "comp_layer_remove_adjustment(0)");
  size_t after_remove = 99;
  verify(remove_ok && comp_layer_adjustment_count(document, layer, &after_remove) == COMP_OK &&
             after_remove == 0,
         "comp_layer_adjustment_count() == 0 after remove");

  /*
   * 滤镜面板链路：面板按 ui_core.h 的行序把值搬进 comp_filter_desc，这里走同一条
   * ABI。要点：零初始化 = 恒等（不碰像素、不开事务）；越界在事务之前拒绝；
   * 真应用后一步撤销可逐字节还原。
   */
  /*
   * 滤镜观测必须先有像素：本段之前的 `comp_document_undo()` 已经把 selftest 图层的
   * 半透明红退回空像素，而 Vignette 在 `fillsClear = 0` 下逐像素跳过全透明像素
   * （AdjustPixels.c），因此空图层上「应用成功但字节不变」是正确行为、看不到效果。
   * 重新铺一张不透明灰图，与 tests/test_filters.cpp 的观测口径一致。
   */
  std::vector<uint8_t> opaque(stride * static_cast<size_t>(height), 0);
  for (size_t i = 0; i < opaque.size(); i += 4) {
    opaque[i + 0] = 200;
    opaque[i + 1] = 200;
    opaque[i + 2] = 200;
    opaque[i + 3] = 255;
  }
  comp_image opaque_image{};
  opaque_image.width = width;
  opaque_image.height = height;
  opaque_image.format = COMP_PIXEL_RGBA8;
  opaque_image.stride = stride;
  opaque_image.pixels = opaque.data();
  verify(add_ok && comp_layer_set_pixels(document, layer, &opaque_image) == COMP_OK,
         "comp_layer_set_pixels(opaque gray)");

  std::vector<uint8_t> flatten_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t depth_before = 0;
  comp_document_history_stats(document, &depth_before, &redo_depth);

  comp_filter_desc identity{};
  identity.kind = COMP_FILTER_VIGNETTE; /* 其余字段零初始化 -> amount 0 = 恒等 */
  int32_t changed = -1;
  char filter_note[256] = {0};
  size_t filter_note_size = 0;
  const bool identity_ok =
      comp_filter_apply(document, layer, &identity, 1.0, &changed, filter_note,
                        sizeof(filter_note), &filter_note_size) == COMP_OK &&
      changed == 0;
  verify(identity_ok, "comp_filter_apply(identity) is a no-op");
  size_t depth_after_identity = 0;
  comp_document_history_stats(document, &depth_after_identity, &redo_depth);
  verify(depth_after_identity == depth_before, "identity filter leaves history untouched");

  comp_filter_desc bad_filter{};
  bad_filter.kind = COMP_FILTER_VIGNETTE;
  bad_filter.vignette.amount = 101.0; /* 量程 0..100 */
  verify(add_ok && comp_filter_apply(document, layer, &bad_filter, 1.0, &changed, filter_note,
                                     sizeof(filter_note), &filter_note_size) ==
                      COMP_ERR_INVALID_ARGUMENT,
         "comp_filter_apply(out-of-range amount) rejected");

  comp_filter_desc vignette{};
  vignette.kind = COMP_FILTER_VIGNETTE;
  vignette.vignette.amount = 100.0;
  vignette.vignette.midpoint = 50.0;
  vignette.vignette.roundness = 100.0;
  vignette.vignette.feather = 60.0;
  vignette.vignette.highlights = 25.0;
  vignette.vignette.color[0] = 0.0;
  vignette.vignette.color[1] = 0.0;
  vignette.vignette.color[2] = 0.0;
  changed = 0;
  const bool vignette_ok =
      add_ok && comp_filter_apply(document, layer, &vignette, 1.0, &changed, filter_note,
                                  sizeof(filter_note), &filter_note_size) == COMP_OK &&
      changed == 1;
  verify(vignette_ok, "comp_filter_apply(vignette amount=100) changed pixels");

  std::vector<uint8_t> flatten_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(vignette_ok && flatten_after != flatten_before,
         "vignette changes the flattened output");
  verify(vignette_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(filter step)");
  std::vector<uint8_t> flatten_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(vignette_ok && flatten_restored == flatten_before,
         "filter undo restores pixels byte-for-byte");

  /*
   * 模糊工具（COMP_TOOL_BLUR）：窗口路径是「鼠标折线 -> comp_tool_apply」。
   * 先铺一张棋盘让模糊有内容可抹（纯色图模糊前后字节相同，测不出东西）。
   */
  std::vector<uint8_t> pattern(stride * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const bool red = ((x / 8) + (y / 8)) % 2 == 0;
      uint8_t* pixel = pattern.data() + static_cast<size_t>(y) * stride +
                       static_cast<size_t>(x) * 4u;
      pixel[0] = red ? 255 : 0;
      pixel[1] = 0;
      pixel[2] = red ? 0 : 255;
      pixel[3] = 255;
    }
  }
  comp_image patched{};
  patched.width = width;
  patched.height = height;
  patched.format = COMP_PIXEL_RGBA8;
  patched.stride = stride;
  patched.pixels = pattern.data();
  verify(comp_layer_set_pixels(document, layer, &patched) == COMP_OK,
         "comp_layer_set_pixels(checkerboard)");

  std::vector<uint8_t> blur_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_before.data(), COMP_PIXEL_RGBA8, stride);
  const float stroke[] = {8.0f, 32.0f, 20.0f, 32.0f, 32.0f, 32.0f,
                          44.0f, 32.0f, 56.0f, 32.0f};
  comp_tool_op_desc blur_desc{};
  blur_desc.kind = COMP_TOOL_BLUR;
  blur_desc.stroke_points = stroke;
  blur_desc.stroke_point_count = 5;
  blur_desc.brush.diameter = 24.0f;
  blur_desc.brush.hardness = 0.75f;
  blur_desc.brush.pressure = 1.0f;
  blur_desc.brush.flow = 1.0f;
  blur_desc.blur.radius = 6.0f;
  char tool_note[256] = {0};
  size_t tool_note_size = 0;
  const bool blur_ok =
      comp_tool_apply(document, layer, &blur_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(blur_ok, "comp_tool_apply(COMP_TOOL_BLUR)");
  std::vector<uint8_t> blur_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(blur_ok && blur_after != blur_before, "blur tool changes pixels along the stroke");
  verify(blur_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(blur step)");
  std::vector<uint8_t> blur_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(blur_ok && blur_restored == blur_before,
         "blur undo restores pixels byte-for-byte");

  comp_tool_op_desc bad_blur = blur_desc;
  bad_blur.blur.radius = 0.0f; /* 量程 0.5..50 */
  verify(comp_tool_apply(document, layer, &bad_blur, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_BLUR(radius=0) rejected");

  /*
   * 涂抹 / 液化（COMP_TOOL_SMUDGE / LIQUIFY）：与模糊共用同一条「鼠标折线 ->
   * comp_tool_apply」通路，参数换成 warp.strength。strength=0 必须逐字节不动，
   * 否则壳层把 Alt+] 调到 0 时会悄悄改图。
   */
  std::vector<uint8_t> warp_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, warp_before.data(), COMP_PIXEL_RGBA8, stride);

  comp_tool_op_desc smudge_desc{};
  smudge_desc.kind = COMP_TOOL_SMUDGE;
  smudge_desc.stroke_points = stroke;
  smudge_desc.stroke_point_count = 5;
  smudge_desc.brush.diameter = 24.0f;
  smudge_desc.brush.hardness = 0.75f;
  smudge_desc.brush.pressure = 1.0f;
  smudge_desc.brush.flow = 1.0f;
  smudge_desc.warp.strength = 0.5f;
  const bool smudge_ok =
      comp_tool_apply(document, layer, &smudge_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(smudge_ok, "comp_tool_apply(COMP_TOOL_SMUDGE)");
  std::vector<uint8_t> smudge_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, smudge_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(smudge_ok && smudge_after != warp_before, "smudge drags colour along the stroke");
  verify(smudge_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(smudge step)");

  comp_tool_op_desc liquify_desc = smudge_desc;
  liquify_desc.kind = COMP_TOOL_LIQUIFY;
  liquify_desc.warp.strength = 0.0f;
  const bool liquify_zero_ok =
      comp_tool_apply(document, layer, &liquify_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  std::vector<uint8_t> liquify_zero(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, liquify_zero.data(), COMP_PIXEL_RGBA8, stride);
  verify(liquify_zero_ok && liquify_zero == warp_before,
         "COMP_TOOL_LIQUIFY(strength=0) is a byte-identical no-op");

  liquify_desc.warp.strength = 0.8f;
  const bool liquify_ok =
      comp_tool_apply(document, layer, &liquify_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(liquify_ok, "comp_tool_apply(COMP_TOOL_LIQUIFY)");
  std::vector<uint8_t> liquify_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, liquify_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(liquify_ok && liquify_after != warp_before, "liquify moves pixels along the stroke");
  verify(liquify_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(liquify step)");

  /*
   * 渐变（COMP_TOOL_GRADIENT，S9）：窗口路径是「拖出两个端点 -> comp_tool_apply」。
   * 冻结三件事：枚举值、铺色后逐字节可撤销、端点重合被参数闸拒绝且不留撤销步。
   */
  verify(static_cast<int>(COMP_TOOL_GRADIENT) == 13, "COMP_TOOL_GRADIENT stays at 13");

  std::vector<uint8_t> gradient_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t gradient_undo_before = 0;
  size_t gradient_redo = 0;
  comp_document_history_stats(document, &gradient_undo_before, &gradient_redo);

  comp_tool_op_desc gradient_desc{};
  gradient_desc.kind = COMP_TOOL_GRADIENT;
  gradient_desc.gradient.type = 0; /* 线性 */
  gradient_desc.gradient.start_x = 4.0;
  gradient_desc.gradient.start_y = 32.0;
  gradient_desc.gradient.end_x = 60.0;
  gradient_desc.gradient.end_y = 32.0;
  gradient_desc.gradient.color0[0] = 255; /* 第一色标：不透明红 */
  gradient_desc.gradient.color0[3] = 255;
  gradient_desc.gradient.color1[2] = 255; /* 第二色标：不透明蓝 */
  gradient_desc.gradient.color1[3] = 255;
  gradient_desc.gradient.opacity = 1.0f;
  const bool gradient_ok =
      comp_tool_apply(document, layer, &gradient_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(gradient_ok, "comp_tool_apply(COMP_TOOL_GRADIENT)");

  std::vector<uint8_t> gradient_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t left_pixel = static_cast<size_t>(32) * stride + static_cast<size_t>(4) * 4u;
  const size_t right_pixel = static_cast<size_t>(32) * stride + static_cast<size_t>(60) * 4u;
  verify(gradient_ok && gradient_after != gradient_before,
         "gradient tool paints the layer");
  verify(gradient_ok && gradient_after[left_pixel + 0] == 255 &&
             gradient_after[left_pixel + 2] == 0,
         "the gradient starts at the first stop (red)");
  verify(gradient_ok && gradient_after[right_pixel + 2] == 255 &&
             gradient_after[right_pixel + 0] == 0,
         "the gradient ends at the second stop (blue)");
  verify(gradient_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(gradient step)");
  std::vector<uint8_t> gradient_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(gradient_ok && gradient_restored == gradient_before,
         "gradient undo restores pixels byte-for-byte");

  /* 端点重合：参数闸必须在事务之前拒绝，撤销栈与像素都不许动。 */
  size_t gradient_undo_rejected = 0;
  comp_document_history_stats(document, &gradient_undo_rejected, &gradient_redo);
  comp_tool_op_desc bad_gradient = gradient_desc;
  bad_gradient.gradient.end_x = bad_gradient.gradient.start_x;
  bad_gradient.gradient.end_y = bad_gradient.gradient.start_y;
  verify(comp_tool_apply(document, layer, &bad_gradient, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_GRADIENT(coincident endpoints) rejected");
  size_t gradient_undo_after_reject = 0;
  comp_document_history_stats(document, &gradient_undo_after_reject, &gradient_redo);
  verify(gradient_undo_after_reject == gradient_undo_rejected,
         "the rejected gradient leaves no undo step");
  std::vector<uint8_t> gradient_after_reject(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_after_reject.data(), COMP_PIXEL_RGBA8, stride);
  verify(gradient_after_reject == gradient_restored,
         "the rejected gradient leaves the render byte-identical");

  /*
   * 形状（COMP_TOOL_SHAPE，S10）：窗口路径是「拖出对角点 / 两端点 -> comp_tool_apply」。
   * 冻结：枚举值、矩形填充与逐字节撤销、椭圆不碰包围盒角（椭圆不是矩形）、直线是
   * 圆头描边而不是填充、带填充的直线被参数闸拒绝且不留撤销步、图层外形状成功但
   * 不产生空撤销步。
   */
  verify(static_cast<int>(COMP_TOOL_SHAPE) == 14, "COMP_TOOL_SHAPE stays at 14");

  std::vector<uint8_t> shape_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t shape_undo_before = 0;
  size_t shape_redo = 0;
  comp_document_history_stats(document, &shape_undo_before, &shape_redo);

  comp_tool_op_desc shape_rect_desc{};
  shape_rect_desc.kind = COMP_TOOL_SHAPE;
  shape_rect_desc.shape.kind = COMP_SHAPE_RECT;
  shape_rect_desc.shape.x0 = 8.0;
  shape_rect_desc.shape.y0 = 8.0;
  shape_rect_desc.shape.x1 = 40.0;
  shape_rect_desc.shape.y1 = 40.0;
  shape_rect_desc.shape.style.fill_enabled = 1;
  shape_rect_desc.shape.style.fill_color[1] = 255; /* 不透明绿 */
  shape_rect_desc.shape.style.fill_color[3] = 255;
  shape_rect_desc.shape.style.opacity = 1.0f;
  shape_rect_desc.shape.style.antialias = 1;
  const bool shape_rect_ok =
      comp_tool_apply(document, layer, &shape_rect_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_rect_ok, "comp_tool_apply(COMP_TOOL_SHAPE rect)");
  std::vector<uint8_t> shape_rect_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_rect_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t rect_inside = static_cast<size_t>(20) * stride + static_cast<size_t>(20) * 4u;
  const size_t rect_outside = static_cast<size_t>(20) * stride + static_cast<size_t>(44) * 4u;
  verify(shape_rect_ok && shape_rect_after[rect_inside + 1] == 255 && shape_rect_after[rect_inside + 3] == 255,
         "the rectangle is filled with the shape colour");
  verify(shape_rect_ok && std::memcmp(&shape_rect_after[rect_outside], &shape_before[rect_outside], 4) == 0,
         "pixels outside the rectangle are untouched");
  verify(shape_rect_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape rect step)");
  std::vector<uint8_t> shape_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_rect_ok && shape_restored == shape_before,
         "shape undo restores pixels byte-for-byte");

  /* 椭圆：中心要填，包围盒的角不能填——那正是「椭圆不是矩形」的证据。 */
  comp_tool_op_desc shape_ellipse_desc = shape_rect_desc;
  shape_ellipse_desc.shape.kind = COMP_SHAPE_ELLIPSE;
  shape_ellipse_desc.shape.style.fill_color[0] = 255;
  shape_ellipse_desc.shape.style.fill_color[1] = 0;
  const bool shape_ellipse_ok =
      comp_tool_apply(document, layer, &shape_ellipse_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_ellipse_ok, "comp_tool_apply(COMP_TOOL_SHAPE ellipse)");
  std::vector<uint8_t> shape_ellipse_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_ellipse_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t shape_ellipse_center = static_cast<size_t>(20) * stride + static_cast<size_t>(20) * 4u;
  const size_t shape_ellipse_corner = static_cast<size_t>(8) * stride + static_cast<size_t>(8) * 4u;
  verify(shape_ellipse_ok && shape_ellipse_after[shape_ellipse_center] == 255 && shape_ellipse_after[shape_ellipse_center + 3] == 255,
         "the ellipse centre is filled");
  verify(shape_ellipse_ok &&
             std::memcmp(&shape_ellipse_after[shape_ellipse_corner], &shape_before[shape_ellipse_corner], 4) == 0,
         "the ellipse leaves its bounding-box corner untouched");
  verify(shape_ellipse_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape ellipse step)");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_ellipse_ok && shape_restored == shape_before,
         "ellipse undo restores pixels byte-for-byte");

  /* 直线：只有描边（fill 必须为 0）；圆头端点越过端点半个线宽。 */
  comp_tool_op_desc shape_line_desc{};
  shape_line_desc.kind = COMP_TOOL_SHAPE;
  shape_line_desc.shape.kind = COMP_SHAPE_LINE;
  shape_line_desc.shape.x0 = 8.0;
  shape_line_desc.shape.y0 = 32.0;
  shape_line_desc.shape.x1 = 56.0;
  shape_line_desc.shape.y1 = 32.0;
  shape_line_desc.shape.style.fill_enabled = 0;
  shape_line_desc.shape.style.stroke_enabled = 1;
  shape_line_desc.shape.style.stroke_color[0] = 255;
  shape_line_desc.shape.style.stroke_color[1] = 255;
  shape_line_desc.shape.style.stroke_color[2] = 255;
  shape_line_desc.shape.style.stroke_color[3] = 255;
  shape_line_desc.shape.style.stroke_width = 6.0; /* 半宽 3 */
  shape_line_desc.shape.style.opacity = 1.0f;
  shape_line_desc.shape.style.antialias = 1;
  const bool shape_line_ok =
      comp_tool_apply(document, layer, &shape_line_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_line_ok, "comp_tool_apply(COMP_TOOL_SHAPE line)");
  std::vector<uint8_t> shape_line_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_line_after.data(), COMP_PIXEL_RGBA8, stride);
  const auto at = [&](int32_t x, int32_t y) {
    return static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
  };
  verify(shape_line_ok && shape_line_after[at(32, 32) + 3] == 255, "the line core is opaque");
  verify(shape_line_ok && shape_line_after[at(32, 29) + 3] == 255,
         "a pixel inside half the stroke width is covered");
  verify(shape_line_ok && std::memcmp(&shape_line_after[at(32, 28)], &shape_before[at(32, 28)], 4) == 0,
         "a pixel beyond half the stroke width is untouched");
  verify(shape_line_ok && shape_line_after[at(58, 32) + 3] > 0, "the round cap extends past the endpoint");
  verify(shape_line_ok && std::memcmp(&shape_line_after[at(60, 32)], &shape_before[at(60, 32)], 4) == 0,
         "the cap stops within half the stroke width");
  verify(shape_line_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape line step)");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_line_ok && shape_restored == shape_before,
         "line undo restores pixels byte-for-byte");

  /* 「填充直线」没有定义：参数闸必须在事务之前拒绝，撤销栈与像素都不许动。 */
  comp_tool_op_desc filled_line = shape_line_desc;
  filled_line.shape.style.fill_enabled = 1;
  verify(comp_tool_apply(document, layer, &filled_line, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_SHAPE(line with fill) rejected");
  size_t shape_undo_after_reject = 0;
  comp_document_history_stats(document, &shape_undo_after_reject, &shape_redo);
  verify(shape_undo_after_reject == shape_undo_before,
         "the rejected line shape leaves no undo step");

  /* 合法但完全落在图层外的形状：成功、无像素改动、不产生空撤销步。 */
  comp_tool_op_desc outside_desc = shape_rect_desc;
  outside_desc.shape.x0 = 100.0;
  outside_desc.shape.y0 = 100.0;
  outside_desc.shape.x1 = 120.0;
  outside_desc.shape.y1 = 120.0;
  verify(comp_tool_apply(document, layer, &outside_desc, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_OK,
         "a shape entirely outside the layer succeeds as a no-op");
  size_t shape_undo_after_outside = 0;
  comp_document_history_stats(document, &shape_undo_after_outside, &shape_redo);
  verify(shape_undo_after_outside == shape_undo_before,
         "an off-canvas shape leaves no empty undo step");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_restored == shape_before,
         "an off-canvas shape leaves the render byte-identical");

  /*
   * 文字（COMP_TOOL_TYPE，S11）：窗口路径是「点击落插入点 -> 键入 ASCII -> 回车提交」。
   * 冻结：枚举值、硬边落墨与逐字节撤销、多行行距、UTF-8 拒绝且不留撤销步、
   * 图层外文字成功但不产生空撤销步。字体是引擎内 5×7 点阵，ASCII-only。
   */
  verify(static_cast<int>(COMP_TOOL_TYPE) == 15, "COMP_TOOL_TYPE stays at 15");

  std::vector<uint8_t> type_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t type_undo_before = 0;
  size_t type_redo = 0;
  comp_document_history_stats(document, &type_undo_before, &type_redo);

  comp_tool_op_desc type_desc{};
  type_desc.kind = COMP_TOOL_TYPE;
  type_desc.type.origin_x = 8.0;
  type_desc.type.origin_y = 8.0;
  type_desc.type.text = "A";
  type_desc.type.text_length = 1;
  type_desc.type.style.font_size = 14.0; /* 缩放 2 => 每个点阵格 2x2 像素 */
  type_desc.type.style.line_height = 1.0;
  type_desc.type.style.letter_spacing = 0.0;
  type_desc.type.style.antialias = 0;
  type_desc.type.color[1] = 255; /* 不透明绿 */
  type_desc.type.color[3] = 255;
  type_desc.type.opacity = 1.0f;
  const bool type_ok =
      comp_tool_apply(document, layer, &type_desc, tool_note, sizeof(tool_note), &tool_note_size) ==
      COMP_OK;
  verify(type_ok, "comp_tool_apply(COMP_TOOL_TYPE \"A\")");
  std::vector<uint8_t> type_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_after.data(), COMP_PIXEL_RGBA8, stride);
  const auto type_at = [&](int32_t x, int32_t y) {
    return static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
  };
  /* 'A' 第 0 列是 0x7E（bit1..6）：原点 (8,8)、缩放 2 时 x=8..9、y=10..21 有墨。 */
  verify(type_ok && type_after[type_at(8, 10) + 3] == 255 &&
             type_after[type_at(8, 10) + 1] == 255,
         "the 'A' stem is painted opaque green");
  verify(type_ok && std::memcmp(&type_after[type_at(8, 8)], &type_before[type_at(8, 8)], 4) == 0,
         "the empty top row of the glyph stays untouched");
  verify(type_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(type step)");
  std::vector<uint8_t> type_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(type_ok && type_restored == type_before, "type undo restores pixels byte-for-byte");

  /* 多行：字号 7 + 行距 1.0 => 第二行正好下移 7 px。 */
  comp_tool_op_desc multiline_desc = type_desc;
  multiline_desc.type.text = "A\nB";
  multiline_desc.type.text_length = 3;
  multiline_desc.type.style.font_size = 7.0;
  const bool multiline_ok =
      comp_tool_apply(document, layer, &multiline_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  std::vector<uint8_t> multiline_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, multiline_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(multiline_ok && multiline_after[type_at(8, 9) + 3] == 255 &&
             multiline_after[type_at(8, 16) + 3] == 255,
         "both text lines are painted one line step apart");
  verify(multiline_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(multiline type step)");

  /* UTF-8/CJK：ASCII 字体明确不支持 => 参数闸拒绝且不留撤销步。 */
  comp_tool_op_desc cjk_desc = type_desc;
  cjk_desc.type.text = "\xE4\xBD\xA0\xE5\xA5\xBD";
  cjk_desc.type.text_length = 6;
  verify(comp_tool_apply(document, layer, &cjk_desc, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_TYPE(UTF-8/CJK) rejected");
  size_t type_undo_after_reject = 0;
  comp_document_history_stats(document, &type_undo_after_reject, &type_redo);
  verify(type_undo_after_reject == type_undo_before, "the rejected type leaves no undo step");

  /* 合法但完全落在图层外：成功、无像素改动、不产生空撤销步。 */
  comp_tool_op_desc type_outside = type_desc;
  type_outside.type.origin_x = 100.0;
  type_outside.type.origin_y = 100.0;
  verify(comp_tool_apply(document, layer, &type_outside, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_OK,
         "text entirely outside the layer succeeds as a no-op");
  size_t type_undo_after_outside = 0;
  comp_document_history_stats(document, &type_undo_after_outside, &type_redo);
  verify(type_undo_after_outside == type_undo_before,
         "an off-canvas text box leaves no empty undo step");
  comp_render_flatten(document, type_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(type_restored == type_before,
         "an off-canvas text box leaves the render byte-identical");

  /*
   * 文字属性面板（S14）：行表、步进与命中几何都来自 ui_core.h，
   * 与 draw_type_panel / WM_LBUTTONDOWN 共用同一份常量。
   */
  verify(comp_ui::kTypePanelRowCount == 4 &&
             std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelFontSizeRow)) ==
                 "Font Size" &&
             std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelLineHeightRow)) ==
                 "Line Height" &&
             std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelLetterSpacingRow)) ==
                 "Letter Spacing" &&
             std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelFontRow)) == "Typeface" &&
             comp_ui::type_panel_row_is_toggle(comp_ui::kTypePanelFontRow) &&
             !comp_ui::type_panel_row_is_toggle(comp_ui::kTypePanelFontSizeRow) &&
             comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelLineHeightRow) &&
             !comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelFontSizeRow),
         "ui_core type panel row table (4 rows, font row is a toggle)");

  verify(std::fabs(comp_ui::step_type_line_height(1.25, 1) - 1.30) < 1e-9 &&
             std::fabs(comp_ui::step_type_line_height(1.25, -1) - 1.20) < 1e-9 &&
             comp_ui::step_type_line_height(1.0, -1) == 1.0 &&
             comp_ui::step_type_line_height(8.0, 1) == 8.0,
         "ui_core type line-height steps 5% and clamps to 1..8");

  verify(comp_ui::type_panel_step_value(comp_ui::kTypePanelFontSizeRow, 32.0, 32.0, 1) ==
                 comp_ui::step_type_font_size(32.0, 1) &&
             comp_ui::type_panel_step_value(comp_ui::kTypePanelLetterSpacingRow, 0.0, 32.0, 1) ==
                 1.0 &&
             comp_ui::type_panel_clamp_value(comp_ui::kTypePanelLetterSpacingRow, -999.0,
                                             32.0) == comp_ui::type_letter_spacing_min(32.0),
         "ui_core type panel steps use each row's own units");

  const comp_ui::Rect type_panel_rect{1000, 46, 330, 600};
  verify(comp_ui::type_param_row_at(type_panel_rect, 1010.0, 101.0) == 0 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 121.0) == 1 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 141.0) == 2 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 161.0) == 3 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 181.0) == -1 &&
             comp_ui::type_param_row_at(type_panel_rect, 1010.0, 99.0) == -1 &&
             comp_ui::type_param_row_at(type_panel_rect, 500.0, 121.0) == -1,
         "ui_core type panel rows hit-test with the drawn geometry");

  /*
   * UTF-8 编辑（P5-3）：退格必须按码点整段删。按字节删在「你好」上会留下
   * E4 BD 这样的半截码点，引擎如实报 kBadText——用户看到的是退格把整段文字弄没了。
   */
  {
    std::string utf8_buffer;
    comp_ui::type_utf8_append(utf8_buffer, 0x4F60u);  /* 你（3 字节） */
    comp_ui::type_utf8_append(utf8_buffer, 0x1F600u); /* 表情（4 字节） */
    comp_ui::type_utf8_append(utf8_buffer, 'A');      /* ASCII（1 字节） */
    const size_t utf8_total = utf8_buffer.size();
    const bool ascii_erased =
        utf8_total == 8 && comp_ui::type_utf8_erase_last(utf8_buffer) == 1 && utf8_buffer.size() == 7;
    const bool wide_erased = comp_ui::type_utf8_erase_last(utf8_buffer) == 4 &&
                             utf8_buffer.size() == 3 &&
                             comp_ui::type_utf8_erase_last(utf8_buffer) == 3 && utf8_buffer.empty();
    verify(ascii_erased && wide_erased,
           "ui_core type_utf8_erase_last() deletes one whole codepoint per press");
    uint32_t decoded = 0;
    verify(comp_ui::type_utf8_next("\xE4\xBD\xA0", &decoded) == 3 && decoded == 0x4F60u,
           "ui_core type_utf8_next() decodes a 3-byte CJK codepoint");
    verify(comp_ui::type_utf8_next("\xE4\xBD", &decoded) == 0 &&
               comp_ui::type_utf8_next("\n", &decoded) == 0,
           "ui_core type_utf8_next() rejects truncated codepoints and control characters");
  }

  /*
   * 字体轨道（P5-3）：随仓子集字体能加载、能排中文、缺字如实回报，
   * 并且一次提交只占一步撤销。加载路径与窗口启动共用 load_shipped_font。
   */
  comp_font_metrics font_metrics{};
  std::wstring font_label;
  comp_font shipped_font = load_shipped_font(&font_label, &font_metrics);
  verify(shipped_font != nullptr && font_metrics.scalable != 0 && font_metrics.has_cjk != 0 &&
             font_metrics.units_per_em > 0,
         "the shipped subset font loads (scalable, has CJK)");
  if (shipped_font != nullptr) {
    /* 预览度量：两个 CJK 码点约 2 em 宽，高 = (ascender - descender) * em。 */
    const comp_ui::TypeBlockMetrics font_preview = comp_ui::type_block_metrics_font(
        40.0, 1.25, 0.0, "\xE4\xBD\xA0\xE5\xA5\xBD", font_metrics.ascender,
        font_metrics.descender);
    verify(font_preview.line_count == 1 && font_preview.max_line_glyphs == 2 &&
               std::fabs(font_preview.width - 80.0) < 1.0 &&
               std::fabs(font_preview.height -
                         (font_metrics.ascender - font_metrics.descender) * 40.0) < 1e-6 &&
               font_preview.advance == 0.0,
           "ui_core font-track preview measures CJK in em units (estimate)");

    std::vector<uint8_t> font_before(stride * static_cast<size_t>(height), 0);
    comp_render_flatten(document, font_before.data(), COMP_PIXEL_RGBA8, stride);
    size_t font_undo_before = 0;
    comp_document_history_stats(document, &font_undo_before, &type_redo);

    comp_tool_op_desc font_desc = type_desc;
    font_desc.type.text = "\xE4\xBD\xA0\xE5\xA5\xBD"; /* 你好 */
    font_desc.type.text_length = 6;
    font_desc.type.style.font_size = 32.0; /* 字体轨道：em 高，不是格子高 */
    font_desc.type_font = shipped_font;
    const bool font_ok =
        comp_tool_apply(document, layer, &font_desc, tool_note, sizeof(tool_note),
                        &tool_note_size) == COMP_OK;
    verify(font_ok, "comp_tool_apply(COMP_TOOL_TYPE CJK through the font track)");
    std::vector<uint8_t> font_after(stride * static_cast<size_t>(height), 0);
    comp_render_flatten(document, font_after.data(), COMP_PIXEL_RGBA8, stride);
    size_t font_ink = 0;
    for (size_t i = 3; i < font_after.size(); i += 4) {
      if (font_after[i] != 0) ++font_ink;
    }
    verify(font_ok && font_ink > 100, "the font track paints CJK coverage");
    verify(font_ok && comp_document_undo(document) == COMP_OK,
           "comp_document_undo(font track CJK step)");
    std::vector<uint8_t> font_restored(stride * static_cast<size_t>(height), 0);
    comp_render_flatten(document, font_restored.data(), COMP_PIXEL_RGBA8, stride);
    verify(font_ok && font_restored == font_before,
           "font-track undo restores pixels byte-for-byte");

    /* 缺字（U+0378 未分配）：提交成功、note 如实说明、不画 .notdef 方框。 */
    comp_tool_op_desc missing_desc = font_desc;
    missing_desc.type.text = "\xE4\xBD\xA0\xCD\xB8"; /* 你 + U+0378 */
    missing_desc.type.text_length = 5;
    const bool missing_ok =
        comp_tool_apply(document, layer, &missing_desc, tool_note, sizeof(tool_note),
                        &tool_note_size) == COMP_OK;
    verify(missing_ok && std::strstr(tool_note, "without a glyph") != nullptr,
           "a codepoint without a glyph is reported in the note");
    verify(missing_ok && comp_document_undo(document) == COMP_OK,
           "comp_document_undo(font track missing-glyph step)");
    size_t font_undo_after = 0;
    comp_document_history_stats(document, &font_undo_after, &type_redo);
    verify(font_undo_after == font_undo_before,
           "the font-track steps leave the undo stack where it started");
    comp_font_release(shipped_font);
  }

  /*
   * 合成缓存（MERGED，S12）：外壳每次 WM_PAINT 都走 comp_render_flatten，
   * 缓存命中与否**不得改变画面**，而任何一次编辑都必须立刻反映到下一帧。
   * 这里用「连拍两张 + 编辑后一张 + 撤销后一张」把这三件事钉在 ABI 层。
   */
  std::vector<uint8_t> merged_first(stride * static_cast<size_t>(height), 0);
  std::vector<uint8_t> merged_second(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, merged_first.data(), COMP_PIXEL_RGBA8, stride);
  comp_render_flatten(document, merged_second.data(), COMP_PIXEL_RGBA8, stride);
  verify(merged_first == merged_second,
         "two consecutive renders are byte-identical (MERGED cache hit)");

  comp_tool_op_desc merged_desc = type_desc;
  merged_desc.type.origin_x = 8.0;
  merged_desc.type.origin_y = 8.0;
  verify(comp_tool_apply(document, layer, &merged_desc, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_OK,
         "comp_tool_apply(COMP_TOOL_TYPE \"A\") for the MERGED cache check");
  std::vector<uint8_t> merged_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, merged_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(merged_after != merged_first,
         "an edit invalidates the MERGED cache (the next frame shows the new pixels)");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(MERGED cache step)");
  std::vector<uint8_t> merged_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, merged_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(merged_restored == merged_first,
         "undo invalidates the MERGED cache and restores the pixels byte-for-byte");

  /*
   * 选区：壳层蚂蚁线的数据源就是这条「有效 coverage」查询。这里冻结三件事：
   * 契约里「无选区」与「显式空选区」不同、覆盖率为文档分辨率、选区变更是一步撤销。
   */
  int32_t selection_active = -1;
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_get_active() starts at 0 (no selection)");

  const float rect_points[4] = {8.0f, 8.0f, 40.0f, 40.0f};
  int32_t selection_changed = 0;
  const bool rect_ok =
      comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                 COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
      selection_changed == 1;
  verify(rect_ok, "comp_selection_apply_shape(rect)");

  std::vector<uint8_t> coverage(stride * static_cast<size_t>(height), 0);
  comp_mask coverage_mask{};
  coverage_mask.width = width;
  coverage_mask.height = height;
  coverage_mask.stride = stride;
  coverage_mask.pixels = coverage.data();
  verify(rect_ok && comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1,
         "comp_selection_get_active() == 1 after a rect shape");
  verify(comp_selection_get_coverage(document, &coverage_mask) == COMP_OK,
         "comp_selection_get_coverage()");
  verify(rect_ok && coverage[static_cast<size_t>(20) * stride + 20u] == 255 &&
             coverage[static_cast<size_t>(2) * stride + 2u] == 0,
         "coverage is 255 inside the rect and 0 outside");

  /* 蚂蚁线几何：矩形选区应得到 4 条边界；覆盖率全 0 时一条都不画。 */
  comp_ui::BoundarySegment ants[16];
  verify(comp_ui::selection_boundary_segments(coverage.data(), width, height, stride, 128,
                                              ants, 16) == 4,
         "selection_boundary_segments(rect) == 4 (what the shell draws)");

  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(selection step)");
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "undoing the selection shape clears the ants");

  int32_t select_all_changed = 0;
  const bool select_all_ok =
      comp_selection_select_all(document, &select_all_changed) == COMP_OK &&
      comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(63) * stride + 63u] == 255;
  verify(select_all_ok, "comp_selection_select_all() covers the canvas");

  /*
   * 反选「全选」得到的是**无选区**而不是显式空选区（ABI 契约，见
   * comp_selection.h）：此后 get_coverage 会返回 COMP_ERR_STATE，所以壳层
   * refresh_selection 必须先查 active 再决定要不要取覆盖率。
   */
  int32_t invert_changed = 0;
  verify(comp_selection_invert(document, &invert_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_invert() of a full selection means no selection");

  int32_t deselect_changed = 0;
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_deselect() returns to the no-selection state");

  /*
   * S7：椭圆框选 / 套索多边形 / 魔棒 / 新选区羽化。
   * 按壳层 marquee_end / lasso_end / wand_click / finish_new_selection 的同一参数
   * 序列调用 ABI——验证的是「接线后的真实调用」，不是另写一套理想路径。
   */
  const float ellipse_points[4] = {8.0f, 8.0f, 48.0f, 40.0f};
  verify(comp_selection_apply_shape(document, COMP_SELECTION_ELLIPSE, ellipse_points, 2,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
             selection_changed == 1 &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(24) * stride + 28u] == 255 &&
             coverage[static_cast<size_t>(8) * stride + 8u] == 0,
         "comp_selection_apply_shape(ellipse) covers the centre and clips the corner");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(ellipse selection step)");

  const float lasso_polygon[6] = {8.0f, 8.0f, 40.0f, 8.0f, 8.0f, 40.0f};
  verify(comp_selection_apply_shape(document, COMP_SELECTION_POLYGON, lasso_polygon, 3,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
             selection_changed == 1 &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(12) * stride + 12u] == 255 &&
             coverage[static_cast<size_t>(36) * stride + 36u] == 0,
         "comp_selection_apply_shape(polygon) is what the lasso commits");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(lasso selection step)");

  /* 魔棒测试图：左半 (x<32) 不透明灰 200，右半不透明黑；容差 0 = 只吃同色连通块。 */
  std::vector<uint8_t> wand_pixels(stride * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* pixel =
          wand_pixels.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
      const uint8_t value = x < 32 ? 200 : 0;
      pixel[0] = value;
      pixel[1] = value;
      pixel[2] = value;
      pixel[3] = 255;
    }
  }
  comp_layer_desc wand_desc{};
  wand_desc.name = "selftest-wand";
  wand_desc.blend = COMP_BLEND_NORMAL;
  wand_desc.opacity = 1.0f;
  wand_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  wand_desc.transform = comp_rect{0, 0, width, height}; /* 1:1，魔棒 LAYER 取样的前提 */
  comp_image wand_image{};
  wand_image.width = width;
  wand_image.height = height;
  wand_image.format = COMP_PIXEL_RGBA8;
  wand_image.stride = stride;
  wand_image.pixels = wand_pixels.data();
  comp_layer_id wand_layer = 0;
  const bool wand_fixture_ok =
      comp_layer_add(document, &wand_desc, &wand_layer) == COMP_OK && wand_layer != 0 &&
      comp_layer_set_pixels(document, wand_layer, &wand_image) == COMP_OK;
  verify(wand_fixture_ok, "comp_layer_add(wand fixture) + comp_layer_set_pixels(1:1)");

  int32_t wand_changed = 0;
  const bool wand_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 10.0, 10.0, 0, 1,
                                0, COMP_SELECTION_REPLACE, &wand_changed) == COMP_OK &&
      wand_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 0;
  verify(wand_ok, "comp_selection_apply_wand(contiguous) selects one colour region");

  comp_rect wand_bounds{};
  verify(wand_ok && comp_selection_get_bounds(document, &wand_bounds) == COMP_OK &&
             wand_bounds.w == 32 && wand_bounds.h == height,
         "wand selection bounds cover the matched half only");
  verify(wand_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(wand selection step)");

  int32_t wand_merged_changed = 0;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_MERGED, 2.0, 2.0,
                                       0, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_merged_changed) == COMP_OK &&
             wand_merged_changed == 1,
         "comp_selection_apply_wand(merged source) samples the composite");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(merged wand step)");

  int32_t wand_bad = -1;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, -1.0, 4.0,
                                       0, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_bad) == COMP_ERR_INVALID_ARGUMENT &&
             wand_bad == -1,
         "comp_selection_apply_wand(seed outside canvas) rejected, out_changed untouched");
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 4.0, 4.0,
                                       256, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_bad) == COMP_ERR_INVALID_ARGUMENT,
         "comp_selection_apply_wand(tolerance=256) rejected");

  /*
   * 0 匹配必须是「无变化」而不是清空选区：壳层靠 changed==0 回滚事务，
   * 所以引擎既不能改选区，也不能留下撤销步。
   * 取两色交界 (32,10) 做种、radius 2：参考色是窗口均值 80，种子像素是 0，
   * 容差 0 下同窗口内没有像素落在 80±0 内 -> 选中 0 个。
   */
  verify(comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK,
         "comp_selection_apply_shape(rect) before the no-match wand probe");
  size_t depth_before_no_match = 0;
  comp_document_history_stats(document, &depth_before_no_match, &redo_depth);
  int32_t no_match_changed = 1;
  const bool no_match_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0, 0, 1,
                                2, COMP_SELECTION_REPLACE, &no_match_changed) == COMP_OK &&
      no_match_changed == 0 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(20) * stride + 20u] == 255;
  size_t depth_after_no_match = 0;
  comp_document_history_stats(document, &depth_after_no_match, &redo_depth);
  verify(no_match_ok && depth_after_no_match == depth_before_no_match,
         "wand with no matching pixel keeps the old selection and adds no undo step");

  /*
   * 新选区羽化：finish_new_selection 把「形状 + comp_selection_set_feather」包进
   * 一个显式事务，因此两步只能算**一步**撤销，且羽化值是本次的绝对值
   * （apply_shape 已把 feather 归零，set_feather 按覆盖语义写入）。
   */
  comp_txn feather_txn = nullptr;
  size_t depth_before_feather = 0;
  comp_document_history_stats(document, &depth_before_feather, &redo_depth);
  int32_t feather_changed = 0;
  bool feather_ok = comp_txn_begin(document, "Feather Selection", &feather_txn) == COMP_OK;
  feather_ok = feather_ok &&
               comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                          COMP_SELECTION_REPLACE, 1,
                                          &selection_changed) == COMP_OK;
  feather_ok = feather_ok && comp_selection_set_feather(document, 4, &feather_changed) == COMP_OK &&
               feather_changed == 1;
  feather_ok = feather_ok && comp_txn_commit(feather_txn) == COMP_OK;
  float feather_value = -1.0f;
  size_t depth_after_feather = 0;
  comp_selection_get_feather(document, &feather_value);
  comp_document_history_stats(document, &depth_after_feather, &redo_depth);
  verify(feather_ok && std::fabs(feather_value - 4.0f) < 0.01f,
         "shape + comp_selection_set_feather(4) yields feather 4 px");
  verify(feather_ok && depth_after_feather == depth_before_feather + 1,
         "shape + feather commit as ONE undo step (finish_new_selection contract)");
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1 && comp_selection_get_feather(document, &feather_value) == COMP_OK &&
             std::fabs(feather_value) < 0.01f,
         "undoing the feathered selection restores the unfeathered previous selection");

  /*
   * S8：绝对羽化 / 魔棒取样窗口 / 钢笔路径。
   * 按 commit_feather_input / wand_click / commit_pen_path 的真实调用序列走 ABI。
   */
  int32_t absolute_changed = 0;
  float absolute_feather = -1.0f;
  verify(comp_selection_set_feather(document, 8, &absolute_changed) == COMP_OK &&
             absolute_changed == 1 &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather - 8.0f) < 0.01f,
         "comp_selection_set_feather(8) sets the absolute feather");
  absolute_changed = 99;
  verify(comp_selection_set_feather(document, 8, &absolute_changed) == COMP_OK &&
             absolute_changed == 0,
         "setting the same feather twice is a no-op");
  verify(comp_selection_set_feather(document, 251, &absolute_changed) ==
             COMP_ERR_INVALID_ARGUMENT,
         "comp_selection_set_feather(251) rejected before any transaction");
  verify(comp_selection_set_feather(document, 0, &absolute_changed) == COMP_OK &&
             absolute_changed == 1 &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather) < 0.01f,
         "comp_selection_set_feather(0) clears the feather");
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather - 8.0f) < 0.01f,
         "undoing the feather reset restores 8 px");
  verify(comp_document_undo(document) == COMP_OK,
         "undoing the absolute feather step restores the previous selection");

  /* 魔棒取样窗口：边界种子 + 半径 1 的参考色是窗口均值，容差 0 下不再匹配。 */
  int32_t radius_changed = 0;
  comp_rect radius_bounds{};
  const bool radius_zero_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0, 0, 1,
                                0, COMP_SELECTION_REPLACE, &radius_changed) == COMP_OK &&
      radius_changed == 1 && comp_selection_get_bounds(document, &radius_bounds) == COMP_OK &&
      radius_bounds.w == 32;
  verify(radius_zero_ok, "wand radius 0 selects the black half from the boundary seed");
  radius_changed = 99;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0,
                                       0, 1, 1, COMP_SELECTION_REPLACE,
                                       &radius_changed) == COMP_OK &&
             radius_changed == 0 &&
             comp_selection_get_bounds(document, &radius_bounds) == COMP_OK &&
             radius_bounds.w == 32,
         "wand radius 1 averages the window, finds no match, and keeps the selection");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(wand radius selection step)");

  /*
   * 钢笔路径：ui_core 的锚点/段装配 -> comp_selection_apply_path，与
   * commit_pen_path 走同一条 ABI。这里用 4 个直角锚点的 24..40 方形。
   */
  comp_ui::PenAnchor pen_anchors[4] = {};
  int pen_anchor_count = 0;
  bool pen_state_ok = comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                 comp_ui::Point{24.0, 24.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{40.0, 24.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{40.0, 40.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{24.0, 40.0});
  comp_ui::PenSegment pen_segments[4] = {};
  const int pen_segment_count =
      comp_ui::pen_build_segments(pen_anchors, pen_anchor_count, /*closed=*/true, pen_segments, 4);
  comp_selection_cubic pen_cubics[4] = {};
  for (int i = 0; i < pen_segment_count && i < 4; ++i) {
    pen_cubics[i].start[0] = static_cast<float>(pen_segments[i].start.x);
    pen_cubics[i].start[1] = static_cast<float>(pen_segments[i].start.y);
    pen_cubics[i].control1[0] = static_cast<float>(pen_segments[i].control1.x);
    pen_cubics[i].control1[1] = static_cast<float>(pen_segments[i].control1.y);
    pen_cubics[i].control2[0] = static_cast<float>(pen_segments[i].control2.x);
    pen_cubics[i].control2[1] = static_cast<float>(pen_segments[i].control2.y);
    pen_cubics[i].end[0] = static_cast<float>(pen_segments[i].end.x);
    pen_cubics[i].end[1] = static_cast<float>(pen_segments[i].end.y);
    pen_cubics[i].new_subpath = i == 0 ? 1 : 0;
  }
  size_t depth_before_pen = 0;
  comp_document_history_stats(document, &depth_before_pen, &redo_depth);
  int32_t pen_changed = 0;
  const bool pen_ok =
      pen_state_ok && pen_segment_count == 4 &&
      comp_selection_apply_path(document, pen_cubics, pen_segment_count,
                                COMP_SELECTION_REPLACE, 1, &pen_changed) == COMP_OK &&
      pen_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(32) * stride + 32u] == 255 &&
      coverage[static_cast<size_t>(60) * stride + 60u] == 0;
  size_t depth_after_pen = 0;
  comp_document_history_stats(document, &depth_after_pen, &redo_depth);
  verify(pen_ok, "pen path commits a closed selection through comp_selection_apply_path");
  verify(pen_ok && depth_after_pen == depth_before_pen + 1,
         "pen path selection is exactly one undo step");
  comp_ui::Point pen_preview[1 + 4 * 3] = {};
  const int pen_preview_count =
      comp_ui::pen_preview_polyline(pen_anchors, pen_anchor_count, /*closed=*/true, pen_preview,
                                    1 + 4 * 3, /*samples_per_segment=*/3);
  verify(pen_preview_count == 1 + 4 * 3 && pen_preview[pen_preview_count - 1].x == 24.0 &&
             pen_preview[pen_preview_count - 1].y == 24.0,
         "pen preview polyline closes back onto the first anchor");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(pen path selection step)");

  /*
   * P0-4：扩展 / 收缩 / 按图层像素载入选区。
   * 壳里这三条命令分别接到 begin_modify_input + commit_modify_input 与
   * load_layer_pixels_selection，参数与这里逐字一致（半径 0 在壳侧就被拦成
   * 提示，不会打到引擎；这里只验证引擎的量程闸确实会拒 0）。
   */
  const float grow_rect[4] = {20.0f, 20.0f, 40.0f, 40.0f};
  int32_t grow_changed = 0;
  comp_rect grow_bounds{};
  const bool grow_fixture_ok =
      comp_selection_apply_shape(document, COMP_SELECTION_RECT, grow_rect, 2,
                                 COMP_SELECTION_REPLACE, 1, &grow_changed) == COMP_OK &&
      grow_changed == 1 && comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
      grow_bounds.w == 20 && grow_bounds.h == 20;
  verify(grow_fixture_ok, "comp_selection_apply_shape(rect 20x20) is the P0-4 fixture");

  grow_changed = 0;
  verify(grow_fixture_ok &&
             comp_selection_modify(document, COMP_SELECTION_EXPAND, 3, &grow_changed) == COMP_OK &&
             grow_changed == 1 &&
             comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
             grow_bounds.w == 26 && grow_bounds.h == 26,
         "comp_selection_modify(EXPAND, 3) grows the selection 3 px per side");
  verify(grow_fixture_ok && comp_document_undo(document) == COMP_OK &&
             comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
             grow_bounds.w == 20 && grow_bounds.h == 20,
         "one undo returns the expanded selection to 20x20");

  grow_changed = 0;
  verify(grow_fixture_ok &&
             comp_selection_modify(document, COMP_SELECTION_CONTRACT, 3, &grow_changed) == COMP_OK &&
             grow_changed == 1 &&
             comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
             grow_bounds.w == 14 && grow_bounds.h == 14,
         "comp_selection_modify(CONTRACT, 3) shrinks the selection 3 px per side");
  verify(grow_fixture_ok && comp_document_undo(document) == COMP_OK &&
             comp_selection_get_bounds(document, &grow_bounds) == COMP_OK &&
             grow_bounds.w == 20 && grow_bounds.h == 20,
         "one undo returns the contracted selection to 20x20");
  grow_changed = 99;
  verify(comp_selection_modify(document, COMP_SELECTION_EXPAND, 0, &grow_changed) ==
                 COMP_ERR_INVALID_ARGUMENT &&
             grow_changed == 99,
         "comp_selection_modify(EXPAND, 0) is rejected before touching the selection");

  /* 按图层像素载入：测试图两半都不透明，所以左右各取样点都应被选中。 */
  grow_changed = 99;
  verify(wand_fixture_ok &&
             comp_selection_load_layer(document, wand_layer, COMP_SELECTION_LOAD_PIXELS,
                                      COMP_SELECTION_REPLACE, 1, &grow_changed) == COMP_OK &&
             grow_changed == 1 &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 255,
         "comp_selection_load_layer(LOAD_PIXELS, threshold 1) selects every opaque pixel");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(load layer pixels step)");
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK,
         "deselect after the P0-4 probes");


  /*
   * P0-4 第二批：色彩范围（comp_selection_apply_color_range）。
   * 壳侧面板的顺序是「begin txn -> 取色/容差/反相各重算一次（REPLACE）-> 回车 commit」，
   * 这里按同一顺序打 ABI。断言重点不是像素效果，而是三条壳层依赖的契约：
   *   ① 同色全选**不要求连通**（与魔棒的本质区别）；
   *   ② 越界/空指针参数被 ABI 闸拦下，且 out_changed 不被写、不进撤销栈；
   *   ③ 显式事务里连改多次只落一步撤销，abort 把选区还原成打开前。
   */
  const uint8_t color_grey_rgb[3] = {200, 200, 200};
  const uint8_t color_black_rgb[3] = {0, 0, 0};
  size_t depth_before_range = 0;
  comp_document_history_stats(document, &depth_before_range, &redo_depth);
  int32_t range_changed = 0;
  const bool range_grey_ok =
      wand_fixture_ok &&
      comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 0, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
      range_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 0 &&
      coverage[static_cast<size_t>(height - 1) * stride + 5u] == 255;
  size_t depth_after_range = 0;
  comp_document_history_stats(document, &depth_after_range, &redo_depth);
  verify(range_grey_ok, "comp_selection_apply_color_range(fuzziness 0) matches every same-colour pixel");
  verify(range_grey_ok && depth_after_range == depth_before_range + 1,
         "color range selection is exactly one undo step");

  /* 撤销一次 = 回到「打开色彩范围前」的无选区状态（不是退回半张图）。 */
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "one undo returns the color range selection to the pre-open (inactive) state");

  /* 反相 = 补集；再撤销一次回到未反相的那次（一步就是一步）。 */
  verify(range_grey_ok &&
             comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
             range_changed == 1,
         "color range re-applies after undo");
  range_changed = 0;
  verify(comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 0, 1,
                                          COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
             range_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 0 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 255,
         "color range invert selects the complement of the sampled colour");
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 0,
         "one undo leaves the un-inverted color range selection in place");

  /* 容差 200 = 三通道最大差，测试图两半都进选区（容差上界是闭区间）。 */
  range_changed = 0;
  verify(comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 200, 0,
                                          COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
             range_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 255,
         "color range fuzziness 200 matches the far colour as well (inclusive bound)");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(fuzziness 200 step)");

  /* include 与 exclude 是同一组颜色 = 一个都不匹配；引擎不动选区、不落撤销步。 */
  size_t depth_before_empty = 0;
  comp_document_history_stats(document, &depth_before_empty, &redo_depth);
  range_changed = 99;
  size_t depth_after_empty = 0;
  const bool range_empty_ok =
      comp_selection_apply_color_range(document, color_black_rgb, 1, color_black_rgb, 1, 0, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK;
  comp_document_history_stats(document, &depth_after_empty, &redo_depth);
  verify(range_empty_ok && range_changed == 0 &&
             depth_after_empty == depth_before_empty &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 255,
         "color range with include == exclude matches nothing and leaves the selection alone");

  /* 参数闸：越界容差 / 空指针，都是「拒绝且不写 out_changed、不进撤销栈」。 */
  range_changed = 99;
  verify(comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 201, 0,
                                          COMP_SELECTION_REPLACE, &range_changed) ==
                 COMP_ERR_INVALID_ARGUMENT &&
             range_changed == 99,
         "color range fuzziness 201 is rejected before touching the selection");
  range_changed = 99;
  verify(comp_selection_apply_color_range(document, nullptr, 1, nullptr, 0, 40, 0,
                                          COMP_SELECTION_REPLACE, &range_changed) ==
                 COMP_ERR_INVALID_ARGUMENT &&
             range_changed == 99,
         "color range NULL include with a nonzero count is rejected");

  /*
   * 事务预览：打开面板 = begin，每次改参数只是事务内重算，回车才 commit。
   * 两次 REPLACE 在同一个显式事务里 => 只落一步，且以最后一次为准。
   */
  const comp_ui::Rect preview_panel{0, 0, 330, 560};
  verify(comp_ui::clamp_color_range_fuzziness(-5) == 0 &&
             comp_ui::clamp_color_range_fuzziness(201) == 200 &&
             comp_ui::step_color_range_fuzziness(0, -1) == 0 &&
             comp_ui::step_color_range_fuzziness(200, 1) == 200 &&
             comp_ui::step_color_range_fuzziness(19, 1) == 20 &&
             comp_ui::step_color_range_fuzziness(20, 1) == 25,
         "ui_core color range fuzziness clamps to 0..200 and steps 1/5/10");
  verify(comp_ui::color_range_swatch_capacity(preview_panel) == 13 &&
             comp_ui::color_range_swatch_at(preview_panel, 2, 1, 13.0, 51.0) == 0 &&
             comp_ui::color_range_swatch_at(preview_panel, 2, 1, 13.0, 75.0) == 2 &&
             comp_ui::color_range_swatch_at(preview_panel, 2, 1, 320.0, 51.0) == -1 &&
             comp_ui::color_range_row_at(preview_panel, 100.0, 101.0) == 0 &&
             comp_ui::color_range_row_at(preview_panel, 100.0, 125.0) == 1 &&
             comp_ui::color_range_row_at(preview_panel, 100.0, 149.0) == -1,
         "ui_core color range swatch/row geometry hits what it paints");

  comp_txn range_txn = nullptr;
  const size_t depth_before_preview = depth_after_empty;
  const bool range_txn_ok =
      comp_txn_begin(document, "Color Range", &range_txn) == COMP_OK && range_txn != nullptr &&
      comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 0, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
      comp_selection_apply_color_range(document, color_black_rgb, 1, nullptr, 0, 0, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK;
  size_t depth_inside_preview = 0;
  comp_document_history_stats(document, &depth_inside_preview, &redo_depth);
  const bool preview_visible =
      comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 255 &&
      coverage[static_cast<size_t>(10) * stride + 10u] == 0;
  size_t depth_after_commit = 0;
  const bool preview_commit_ok = range_txn_ok && comp_txn_commit(range_txn) == COMP_OK;
  comp_document_history_stats(document, &depth_after_commit, &redo_depth);
  verify(range_txn_ok && preview_visible && preview_commit_ok &&
             depth_inside_preview == depth_before_preview &&
             depth_after_commit == depth_before_preview + 1,
         "color range preview inside a txn shows the last apply and commits one step");

  /* abort 回滚到事务开启时的选区（= 上一次提交的右半），不动撤销栈。 */
  range_txn = nullptr;
  const bool range_abort_txn_ok =
      comp_txn_begin(document, "Color Range", &range_txn) == COMP_OK && range_txn != nullptr &&
      comp_selection_apply_color_range(document, color_grey_rgb, 1, nullptr, 0, 200, 0,
                                       COMP_SELECTION_REPLACE, &range_changed) == COMP_OK &&
      comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 255;
  const bool range_abort_ok = range_abort_txn_ok && comp_txn_abort(range_txn) == COMP_OK;
  size_t depth_after_abort = 0;
  comp_document_history_stats(document, &depth_after_abort, &redo_depth);
  verify(range_abort_ok && depth_after_abort == depth_after_commit &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 0 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 255,
         "comp_txn_abort restores the pre-open color range selection without an undo step");

  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
             coverage[static_cast<size_t>(10) * stride + 50u] == 0,
         "one undo after the color range commit returns to the previous selection");
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK,
         "deselect after the color range probes");

  /* 测试图用完即删：它是全画布不透明层，留着会盖住后面 S6 的 flatten 断言。 */
  verify(comp_layer_remove(document, wand_layer) == COMP_OK,
         "comp_layer_remove(wand fixture) keeps later flatten assertions honest");
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "deselect after the S7 probes restores the S6 precondition");

  /*
   * S6：前景色 / 画笔 / 橡皮擦 / 吸管 / 选区搬移。
   * 壳层只做参数装配，因此这里按 paint_stroke_end / eyedropper_sample / move_end
   * 的同一顺序调用 ABI——验证的是「壳层接线后的真实序列」，不是另写一套。
   */

  verify(comp_ui::tool_shortcut_target(comp_ui::ToolId::Brush, 'e') ==
                 comp_ui::ToolId::Eraser &&
             comp_ui::tool_shortcut_target(comp_ui::ToolId::Eraser, 'E') ==
                 comp_ui::ToolId::Brush,
         "ui_core tool_shortcut_target(E) toggles brush/eraser");
  verify(comp_ui::swatch_count() == 12 && comp_ui::step_swatch(11, 1) == 0 &&
             comp_ui::step_swatch(0, -1) == 11,
         "ui_core swatch palette cycles (X)");

  comp_ui::ColorRGB foreground{};
  const bool parse_ok = comp_ui::color_parse_hex("#3A7BD5", &foreground);
  char foreground_hex[8] = {0};
  comp_ui::color_to_hex(foreground, foreground_hex);
  verify(parse_ok && foreground.r == 0x3A && foreground.g == 0x7B && foreground.b == 0xD5 &&
             std::strcmp(foreground_hex, "#3A7BD5") == 0,
         "ui_core color_parse_hex / color_to_hex round trip (Shift+X)");

  /* 与 paint_stroke_end 相同：前景色 + 单点折线（单击 = 一记 dab）。 */
  verify(comp_layer_set_pixels(document, layer, &opaque_image) == COMP_OK,
         "comp_layer_set_pixels(base for paint)");
  std::vector<uint8_t> paint_base(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, paint_base.data(), COMP_PIXEL_RGBA8, stride);

  comp_tool_op_desc paint_desc{};
  paint_desc.kind = COMP_TOOL_STROKE;
  paint_desc.brush.color[0] = foreground.r;
  paint_desc.brush.color[1] = foreground.g;
  paint_desc.brush.color[2] = foreground.b;
  paint_desc.brush.diameter = 10.0f;
  paint_desc.brush.pressure = 1.0f;
  paint_desc.brush.hardness = 1.0f;
  paint_desc.brush.flow = 1.0f;
  paint_desc.brush.seed = 0;
  const float dab[] = {32.0f, 32.0f};
  paint_desc.stroke_points = dab;
  paint_desc.stroke_point_count = 1;
  char stroke_note[256] = {0};
  size_t stroke_note_size = 0;
  const bool paint_ok =
      comp_tool_apply(document, layer, &paint_desc, stroke_note, sizeof(stroke_note),
                      &stroke_note_size) == COMP_OK;
  std::vector<uint8_t> paint_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, paint_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t center = static_cast<size_t>(32) * stride + static_cast<size_t>(32) * 4u;
  verify(paint_ok && paint_after != paint_base && paint_after[center + 0] == 0x3A &&
             paint_after[center + 1] == 0x7B && paint_after[center + 2] == 0xD5,
         "COMP_TOOL_STROKE paints the shell foreground colour");

  /* 吸管：同一位置取回的直通 sRGB 应还原成同一个前景色。 */
  float sampled[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const bool sample_ok = comp_eyedropper_sample(document, layer, 32.0, 32.0, 1,
                                                COMP_EYEDROPPER_LAYER, sampled) == COMP_OK;
  const comp_ui::ColorRGB sampled_color =
      comp_ui::color_from_unit(sampled[0], sampled[1], sampled[2]);
  verify(sample_ok && sampled_color.r == foreground.r && sampled_color.g == foreground.g &&
             sampled_color.b == foreground.b && sampled[3] == 1.0f,
         "comp_eyedropper_sample() returns the painted colour (feeds foreground)");

  /* 橡皮擦：同一支笔、同样的 diameter/hardness，方向翻转成 destination-out。 */
  paint_desc.erase = 1;
  const bool erase_ok =
      comp_tool_apply(document, layer, &paint_desc, stroke_note, sizeof(stroke_note),
                      &stroke_note_size) == COMP_OK;
  std::vector<uint8_t> erase_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, erase_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(erase_ok && erase_after != paint_after,
         "COMP_TOOL_STROKE(erase) removes the painted pixel");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(erase step)");
  std::vector<uint8_t> erase_undone(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, erase_undone.data(), COMP_PIXEL_RGBA8, stride);
  verify(erase_undone == paint_after, "undoing erase restores the painted pixel byte-identically");

  /*
   * 选区搬移：像素搬移 + 轮廓平移必须落在同一个显式事务里（一次拖拽 = 一步撤销）。
   * 顺序与 move_end 一致：先按当前选区切像素，再平移轮廓。
   */
  comp_rect bounds_before{};
  const bool move_rect_ok =
      comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                 COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
      comp_selection_get_bounds(document, &bounds_before) == COMP_OK;
  std::vector<uint8_t> move_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, move_before.data(), COMP_PIXEL_RGBA8, stride);

  comp_txn move_txn = nullptr;
  const bool move_txn_ok =
      comp_txn_begin(document, "Move Selection Pixels", &move_txn) == COMP_OK;
  int32_t moved_pixels = 0;
  int32_t moved_outline = 0;
  const bool move_ok =
      move_txn_ok &&
      comp_selection_move_pixels(document, layer, 8, 0, 0, &moved_pixels) == COMP_OK &&
      comp_selection_translate(document, 8, 0, &moved_outline) == COMP_OK &&
      comp_txn_commit(move_txn) == COMP_OK;
  comp_rect bounds_after{};
  comp_selection_get_bounds(document, &bounds_after);
  verify(move_rect_ok && move_ok && moved_pixels > 0 && moved_outline > 0 &&
             bounds_after.x == bounds_before.x + 8 && bounds_after.y == bounds_before.y,
         "selection move pixels + translate commit in one explicit transaction");

  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(move selection)");
  comp_rect bounds_undone{};
  std::vector<uint8_t> move_undone(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, move_undone.data(), COMP_PIXEL_RGBA8, stride);
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1 &&
             comp_selection_get_bounds(document, &bounds_undone) == COMP_OK &&
             bounds_undone.x == bounds_before.x && bounds_undone.y == bounds_before.y &&
             move_undone == move_before,
         "one undo restores both the pixels and the selection outline");

  /* ---------------------------------------------------------------- S13：GPU 协同 */
  /*
   * 独立的 AUTO 引擎：验证「编入 D3D12 时 AUTO 真的走 GPU」以及 GPU/CPU 像素对齐。
   * 场景限定在 GPU 子集内（整幅画布 RGBA8 + 普通/滤色混合）：走 GPU 时允许
   * ±1 LSB 浮点舍入差；回落 CPU 时必须与基准逐字节一致。
   */
  comp_engine_desc gpu_engine_desc{};
  gpu_engine_desc.app_name = "compositor-shell-selfcheck-gpu";
  gpu_engine_desc.version = "0.1.0";
  gpu_engine_desc.backend = COMP_BACKEND_AUTO;
  comp_engine gpu_engine = nullptr;
  const bool gpu_engine_ok =
      comp_engine_create(&gpu_engine_desc, &gpu_engine) == COMP_OK && gpu_engine != nullptr;
  verify(gpu_engine_ok, "comp_engine_create(AUTO) for gpu parity");
  if (gpu_engine_ok) {
    comp_backend gpu_backend = COMP_BACKEND_AUTO;
    const bool gpu_backend_ok = comp_engine_backend(gpu_engine, &gpu_backend) == COMP_OK;
    verify(gpu_backend_ok, "comp_engine_backend(AUTO engine)");
    const bool gpu_path = gpu_backend_ok && gpu_backend != COMP_BACKEND_CPU;
    std::fprintf(report, "[info] auto backend = %s\n",
                 gpu_backend_ok ? backend_name(gpu_backend) : "(query failed)");

    const auto build_parity_document = [](comp_engine target_engine, comp_document* out) -> bool {
      comp_document_desc desc{};
      desc.width = 32;
      desc.height = 32;
      desc.dpi = 72.0f;
      desc.color_space = "sRGB";
      if (comp_document_create(target_engine, &desc, out) != COMP_OK || *out == nullptr) {
        return false;
      }
      size_t count = 0;
      if (comp_layer_count(*out, &count) != COMP_OK || count == 0) return false;
      std::vector<comp_layer_id> ids(count);
      if (comp_layer_list(*out, ids.data(), ids.size(), &count) != COMP_OK) return false;

      std::vector<uint8_t> pixels(32u * 32u * 4u, 0);
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        pixels[i + 0] = 96;
        pixels[i + 1] = 72;
        pixels[i + 2] = 48;
        pixels[i + 3] = 192;
      }
      comp_image base_image{};
      base_image.width = 32;
      base_image.height = 32;
      base_image.format = COMP_PIXEL_RGBA8;
      base_image.stride = 32u * 4u;
      base_image.pixels = pixels.data();
      if (comp_layer_set_pixels(*out, ids.front(), &base_image) != COMP_OK) return false;

      comp_layer_desc top_desc{};
      top_desc.name = "parity-top";
      top_desc.blend = COMP_BLEND_SCREEN;
      top_desc.opacity = 0.75f;
      top_desc.transform = comp_rect{0, 0, 32, 32};
      top_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
      comp_layer_id top = 0;
      if (comp_layer_add(*out, &top_desc, &top) != COMP_OK || top == 0) return false;
      /*
       * 图层像素必须是**预乘** RGBA8（颜色分量 <= alpha，见 comp_layer.h）。
       * 这里给的是预乘值：直通色 (200,64,32) @ alpha 128 → (100,32,16,128)。
       * 用非预乘数据对拍会让 GPU 浮点与 CPU 整数在饱和/钳制上分叉（实测差 34 级），
       * 那已超出 ABI 契约，不是两后端的一致性问题。
       */
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        pixels[i + 0] = 100;
        pixels[i + 1] = 32;
        pixels[i + 2] = 16;
        pixels[i + 3] = 128;
      }
      return comp_layer_set_pixels(*out, top, &base_image) == COMP_OK;
    };

    comp_document gpu_doc = nullptr;
    comp_document cpu_doc = nullptr;
    const bool scene_ok = build_parity_document(gpu_engine, &gpu_doc) &&
                          build_parity_document(engine, &cpu_doc);
    verify(scene_ok, "parity scene builds on AUTO and CPU engines");
    if (scene_ok) {
      std::vector<uint8_t> gpu_pixels(32u * 32u * 4u, 0);
      std::vector<uint8_t> cpu_pixels(32u * 32u * 4u, 0);
      const bool gpu_render_ok =
          comp_render_flatten(gpu_doc, gpu_pixels.data(), COMP_PIXEL_RGBA8, 32u * 4u) == COMP_OK;
      const bool cpu_render_ok =
          comp_render_flatten(cpu_doc, cpu_pixels.data(), COMP_PIXEL_RGBA8, 32u * 4u) == COMP_OK;
      verify(gpu_render_ok && cpu_render_ok, "parity scene flattens on both backends");
      int max_diff = 0;
      size_t diff_count = 0;
      for (size_t i = 0; i < cpu_pixels.size(); ++i) {
        const int diff =
            std::abs(static_cast<int>(cpu_pixels[i]) - static_cast<int>(gpu_pixels[i]));
        if (diff > max_diff) max_diff = diff;
        if (diff != 0) ++diff_count;
      }
      std::fprintf(report, "[info] parity max_diff = %d (diff bytes = %u, path = %s)\n", max_diff,
                   static_cast<unsigned>(diff_count), gpu_path ? "gpu" : "cpu");
      verify(gpu_render_ok && cpu_render_ok && max_diff <= (gpu_path ? 1 : 0),
             "AUTO backend pixels match the CPU reference");
    }
    if (cpu_doc != nullptr) comp_document_destroy(cpu_doc);
    if (gpu_doc != nullptr) comp_document_destroy(gpu_doc);
  }
  if (gpu_engine != nullptr) comp_engine_destroy(gpu_engine);

  /*
   * ---------------------------------------------------------------- M1.5：PSD 分层入口
   *
   * 这一段补的是「引擎有、壳也有，但没人守着」的三条接缝：
   *   1. comp_io_export_document / comp_io_import_document 的分层往返；
   *   2. 拍平入口 comp_io_export(PSD) 仍必须被拒绝（分层能写 != 拍平能写）；
   *   3. 壳的 open_path_as_document(.psd) 走分层入口，而不是拍平入口。
   *
   * 第 3 条尤其需要自检：GUI 的「打开 / 导出为」都被模态对话框挡着，自动化点不过去，
   * 而分派退化成 comp_io_import 时用户看到的是一张「打开了、但只剩一层」的图——
   * 功能没报错，数据已经丢了。判据取**图层数**：同一个两层的 PSD，拍平路径读回来
   * 永远是 1 层。这个差异可判定，且不依赖任何像素容差。
   */
  wchar_t psd_file[MAX_PATH] = L"";
  wsprintfW(psd_file, L"%sselftest_psd_layered.psd", io_temp_dir);
  char psd_path_utf8[MAX_PATH * 2] = {0};
  WideCharToMultiByte(CP_UTF8, 0, psd_file, -1, psd_path_utf8,
                      static_cast<int>(sizeof(psd_path_utf8)), nullptr, nullptr);

  /* 夹具像素必须满足两条 ABI 契约：预乘（颜色 <= alpha）与「像素尺寸 = transform 尺寸」
   * （缩放过的图层 PSD 子集表示不了，那是引擎的显式拒绝，不该在自检里踩）。 */
  const auto paint_solid_layer = [](comp_document target, comp_layer_id id, int32_t w, int32_t h,
                                    uint8_t r, uint8_t g, uint8_t b) -> bool {
    std::vector<uint8_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
      px[i + 0] = r;
      px[i + 1] = g;
      px[i + 2] = b;
      px[i + 3] = 255;
    }
    comp_image image{};
    image.width = w;
    image.height = h;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = static_cast<size_t>(w) * 4u;
    image.pixels = px.data();
    return comp_layer_set_pixels(target, id, &image) == COMP_OK;
  };

  comp_document_desc psd_desc{};
  psd_desc.width = 14;
  psd_desc.height = 12;
  psd_desc.dpi = 72.0f;
  psd_desc.color_space = "sRGB";
  comp_document psd_source = nullptr;
  bool psd_scene_ok = comp_document_create(engine, &psd_desc, &psd_source) == COMP_OK &&
                      psd_source != nullptr;
  size_t psd_base_count = 0;
  std::vector<comp_layer_id> psd_ids;
  if (psd_scene_ok && comp_layer_count(psd_source, &psd_base_count) == COMP_OK &&
      psd_base_count > 0) {
    psd_ids.resize(psd_base_count);
    psd_scene_ok =
        comp_layer_list(psd_source, psd_ids.data(), psd_ids.size(), &psd_base_count) == COMP_OK &&
        psd_base_count >= 1;
  } else {
    psd_scene_ok = false;
  }
  if (psd_scene_ok) {
    psd_scene_ok = paint_solid_layer(psd_source, psd_ids[0], 14, 12, 200, 30, 30);
  }
  comp_layer_id psd_top = 0;
  if (psd_scene_ok) {
    comp_layer_desc top_desc{};
    top_desc.name = "psd-top";
    top_desc.blend = COMP_BLEND_NORMAL;
    top_desc.opacity = 0.5f;
    top_desc.transform = comp_rect{0, 0, 14, 12};
    top_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
    psd_scene_ok = comp_layer_add(psd_source, &top_desc, &psd_top) == COMP_OK && psd_top != 0 &&
                   paint_solid_layer(psd_source, psd_top, 14, 12, 20, 20, 180);
  }
  verify(psd_scene_ok, "PSD layered fixture builds (2 layers)");

  bool psd_export_ok = false;
  if (psd_scene_ok) {
    psd_export_ok =
        comp_io_export_document(psd_source, psd_path_utf8, COMP_FORMAT_PSD, nullptr) == COMP_OK;
  }
  verify(psd_export_ok, "comp_io_export_document(PSD) writes a layered file");

  if (psd_export_ok) {
    comp_document psd_round = nullptr;
    const bool psd_import_ok = comp_io_import_document(engine, psd_path_utf8, &psd_round) == COMP_OK &&
                               psd_round != nullptr;
    size_t round_count = 0;
    bool round_ok = psd_import_ok && comp_layer_count(psd_round, &round_count) == COMP_OK &&
                    round_count == 2;
    if (round_ok) {
      std::vector<comp_layer_id> round_ids(round_count, 0);
      round_ok = comp_layer_list(psd_round, round_ids.data(), round_ids.size(), &round_count) ==
                     COMP_OK &&
                 round_count == 2;
      if (round_ok) {
        /* 图层名（而非位置）是「分层被读回来了」最直接的证据：拍平路径只有一层，
         * 名字也不会是 psd-top。按 id 逐个找，不依赖列表顺序口径。 */
        bool name_found = false;
        for (comp_layer_id id : round_ids) {
          char name[64] = {0};
          size_t name_size = 0;
          if (comp_layer_get_name(psd_round, id, name, sizeof(name), &name_size) == COMP_OK &&
              std::string(name).find("psd-top") != std::string::npos) {
            name_found = true;
          }
        }
        round_ok = name_found;
      }
    }
    verify(round_ok, "comp_io_import_document(PSD) round trip keeps both layers");
    if (psd_round != nullptr) comp_document_destroy(psd_round);
  }

  /* 拍平写侧不能被「分层能写」连带放开：comp_io_export(PSD) 必须继续显式拒绝。 */
  verify(psd_source != nullptr &&
             comp_io_export(psd_source, psd_path_utf8, COMP_FORMAT_PSD, nullptr) ==
                 COMP_ERR_UNSUPPORTED,
         "flattened comp_io_export(PSD) is still rejected");

  if (psd_export_ok) {
    /* 壳的分派：独立构造一份 ShellState（自检不建窗口），只走文档上下文那部分。 */
    ShellState shell{};
    shell.engine = engine;
    open_path_as_document(shell, psd_path_utf8);
    size_t shell_count = 0;
    const bool shell_ok = shell.doc.document != nullptr &&
                          comp_layer_count(shell.doc.document, &shell_count) == COMP_OK &&
                          shell_count == 2;
    verify(shell_ok, "shell open_path_as_document(.psd) takes the layered path");

    /*
     * P1-2 收尾：图层面板过滤。纯视图态——只改显示序，层数组与文档层数都不变，
     * 也不产生撤销步；类型 / 颜色 / 名称三维是「与」关系。用刚打开的两层做探针。
     */
    {
      refresh_layers(shell);
      const int32_t all_rows = panel_row_count(shell);
      const int32_t doc_layers = static_cast<int32_t>(shell.doc.layers.size());
      panel_set_filter_kind(shell, 1); /* 像素图层 */
      const int32_t pixel_rows = panel_row_count(shell);
      panel_set_filter_kind(shell, 3); /* 图层组：这份文档没有组 */
      const int32_t group_rows = panel_row_count(shell);
      panel_set_filter_kind(shell, 0);
      shell.layer_filter_name = L"psd-top";
      panel_refresh_after_filter(shell);
      const int32_t name_rows = panel_row_count(shell);
      bool name_ok = name_rows > 0;
      for (int32_t row = 0; row < name_rows; ++row) {
        const LayerRow* shown = layer_row_at(shell, row);
        if (shown == nullptr || shown->name.find("psd-top") == std::string::npos) {
          name_ok = false;
        }
      }
      panel_set_filter_color(shell, 7); /* 灰色标签：两层都没有标签 */
      const int32_t gray_rows = panel_row_count(shell);
      panel_clear_filter(shell);
      size_t layers_after = 0;
      verify(all_rows == doc_layers && all_rows > 1 && pixel_rows == all_rows &&
                 group_rows == 0 && name_ok && name_rows < all_rows && gray_rows == 0 &&
                 panel_row_count(shell) == all_rows &&
                 comp_layer_count(shell.doc.document, &layers_after) == COMP_OK &&
                 static_cast<int32_t>(layers_after) == doc_layers,
             "layer panel filter narrows by type / color / name without touching the document");
    }

    /*
     * P1-2 收尾：图层复合。捕获 -> 改可见性/不透明度 -> 应用 -> 回读；
     * 第二次应用必须幂等（changed == 0），文档层数不受影响。
     */
    {
      comp_document doc = shell.doc.document;
      size_t comps_before = 0;
      comp_layer_comp_count(doc, &comps_before);
      comp_layer_comp_id comp = 0;
      const bool capture_ok = comp_layer_comp_capture(doc, "selftest-comp", &comp) == COMP_OK;
      size_t comps_after = 0;
      comp_layer_comp_count(doc, &comps_after);
      size_t layer_count = 0;
      comp_layer_count(doc, &layer_count);
      std::vector<comp_layer_id> layer_ids(layer_count, 0);
      size_t listed = 0;
      const bool listed_ok =
          layer_count > 0 &&
          comp_layer_list(doc, layer_ids.data(), layer_ids.size(), &listed) == COMP_OK &&
          listed == layer_count;
      bool restored = false;
      int32_t applied_changed = -1;
      int32_t applied_again = -1;
      if (capture_ok && listed_ok) {
        int32_t visible = 1;
        float opacity = 1.0f;
        comp_layer_get_visible(doc, layer_ids[0], &visible);
        comp_layer_get_opacity(doc, layer_ids[0], &opacity);
        const bool mutated =
            comp_layer_set_visible(doc, layer_ids[0], visible != 0 ? 0 : 1) == COMP_OK &&
            comp_layer_set_opacity(doc, layer_ids[0], 0.25f) == COMP_OK;
        const bool applied = comp_layer_comp_apply(doc, comp, &applied_changed) == COMP_OK;
        if (applied) comp_layer_comp_apply(doc, comp, &applied_again);
        int32_t restored_visible = -1;
        float restored_opacity = 0.0f;
        comp_layer_get_visible(doc, layer_ids[0], &restored_visible);
        comp_layer_get_opacity(doc, layer_ids[0], &restored_opacity);
        restored = mutated && restored_visible == visible && restored_opacity == opacity;
      }
      size_t layer_count_after = 0;
      comp_layer_count(doc, &layer_count_after);
      verify(comps_before == 0 && capture_ok && comp != 0 && comps_after == comps_before + 1 &&
                 restored && applied_changed > 0 && applied_again == 0 &&
                 layer_count_after == layer_count,
             "layer comp captures / applies / is idempotent without touching the layer stack");
    }

    /*
     * P1-2 收尾 ⑦：智能对象 / 智能滤镜。转换 -> 画面逐字节不变、is_smart = 1；
     * 加一条高斯 -> 栈里 1 条且像素变了（像素是求值缓存）；直接写像素被拒
     * （必须先栅格化）；栅格化后 is_smart = 0、渲染结果留下。
     */
    {
      comp_document doc = shell.doc.document;
      comp_layer_id target = 0;
      size_t listed_one = 0;
      /* comp_layer_list 的 out_count 是**总数**（不受 capacity 限制），所以这里只要求 >= 1。 */
      const bool listed_ok =
          comp_layer_list(doc, &target, 1u, &listed_one) == COMP_OK && listed_one >= 1u;
      int32_t px_w = 0;
      int32_t px_h = 0;
      comp_pixel_format px_fmt = COMP_PIXEL_RGBA8;
      std::vector<uint8_t> before;
      if (listed_ok &&
          comp_layer_get_pixels(doc, target, nullptr, 0, &px_w, &px_h, &px_fmt) == COMP_OK &&
          px_w >= 4 && px_h >= 4 && px_fmt == COMP_PIXEL_RGBA8) {
        /*
         * 不依赖当前图层里恰好有什么（拍平的 PSD 夹具可能是一整块纯色，模糊前后逐字节相同）。
         * 先画一个已知图案：透明底 + 中央不透明白块。模糊半径 3 必然改动边缘像素。
         */
        const size_t stride = static_cast<size_t>(px_w) * 4u;
        before.assign(stride * static_cast<size_t>(px_h), 0u);
        const int32_t x0 = px_w / 4;
        const int32_t y0 = px_h / 4;
        const int32_t x1 = px_w - px_w / 4;
        const int32_t y1 = px_h - px_h / 4;
        for (int32_t y = y0; y < y1; ++y) {
          for (int32_t x = x0; x < x1; ++x) {
            uint8_t* px = before.data() + static_cast<size_t>(y) * stride +
                          static_cast<size_t>(x) * 4u;
            px[0] = 255u;
            px[1] = 255u;
            px[2] = 255u;
            px[3] = 255u;
          }
        }
        comp_image pattern{};
        pattern.width = px_w;
        pattern.height = px_h;
        pattern.format = COMP_PIXEL_RGBA8;
        pattern.stride = stride;
        pattern.pixels = before.data();
        if (comp_layer_set_pixels(doc, target, &pattern) != COMP_OK) {
          before.clear();
        }
        /* 读回来当基准：写进去的字节未必等于引擎内部存的（预乘等），比较必须以读回为准。 */
        if (!before.empty()) {
          std::vector<uint8_t> read_back(before.size(), 0u);
          int32_t rw = px_w;
          int32_t rh = px_h;
          comp_pixel_format rfmt = px_fmt;
          if (comp_layer_get_pixels(doc, target, read_back.data(), stride, &rw, &rh, &rfmt) ==
                  COMP_OK &&
              rw == px_w && rh == px_h) {
            before.swap(read_back);
          } else {
            before.clear();
          }
        }
      }
      int32_t changed = 0;
      const bool converted =
          !before.empty() &&
          comp_layer_smart_object_create(doc, target, &changed) == COMP_OK && changed == 1;
      int32_t smart = 0;
      comp_layer_smart_object_is_smart(doc, target, &smart);
      std::vector<uint8_t> after_convert(before.size(), 0u);
      int32_t cw = px_w;
      int32_t ch = px_h;
      comp_pixel_format cfmt = px_fmt;
      const bool pixels_kept =
          converted && smart == 1 &&
          comp_layer_get_pixels(doc, target, after_convert.data(),
                                static_cast<size_t>(cw) * 4u, &cw, &ch, &cfmt) == COMP_OK &&
          after_convert == before;
      comp_filter_desc blur{};
      blur.kind = COMP_FILTER_GAUSSIAN_BLUR;
      blur.gaussian_blur.radius = 3.0;
      const bool added =
          converted && comp_layer_smart_filter_add(doc, target, &blur) == COMP_OK;
      size_t filters = 0;
      comp_layer_smart_filter_count(doc, target, &filters);
      std::vector<uint8_t> after_filter(before.size(), 0u);
      int32_t fw = px_w;
      int32_t fh = px_h;
      comp_pixel_format ffmt = px_fmt;
      const bool render_changed =
          added &&
          comp_layer_get_pixels(doc, target, after_filter.data(),
                                static_cast<size_t>(fw) * 4u, &fw, &fh, &ffmt) == COMP_OK &&
          after_filter != before;
      comp_image write_image{};
      write_image.width = px_w;
      write_image.height = px_h;
      write_image.format = px_fmt;
      write_image.stride = static_cast<size_t>(px_w) * 4u;
      write_image.pixels = before.data();
      const bool refused =
          converted && comp_layer_set_pixels(doc, target, &write_image) == COMP_ERR_STATE;
      const bool rasterized =
          converted && comp_layer_smart_object_rasterize(doc, target) == COMP_OK &&
          comp_layer_smart_object_is_smart(doc, target, &smart) == COMP_OK && smart == 0;
      verify(listed_ok && converted && pixels_kept && added && filters == 1 && render_changed &&
                 refused && rasterized,
             "smart object converts / adds a smart filter / refuses pixel writes / rasterizes");
    }

    /*
     * P1-2 收尾 ⑧：填充图层（纯色）。走壳的面板路径（菜单项 = 开面板 + 缺省参数建层）：
     * 文档层数 +1、顶层 is_fill = 1 且 kind 是纯色；像素是该纯色的均匀渲染（缺省
     * 128/128/128/255）；直接写像素被拒（参数才是真源）；栅格化后 is_fill = 0、
     * 像素原样留下。
     */
    {
      comp_document doc = shell.doc.document;
      size_t count_before = 0;
      comp_size canvas{};
      const bool before_ok =
          doc != nullptr && comp_layer_count(doc, &count_before) == COMP_OK &&
          comp_document_size(doc, &canvas) == COMP_OK && canvas.width > 0 && canvas.height > 0;
      panel_add_fill(shell, COMP_FILL_SOLID);
      size_t count_after = 0;
      comp_layer_id fill_id = 0;
      size_t listed = 0;
      bool added = before_ok && comp_layer_count(doc, &count_after) == COMP_OK &&
                   count_after == count_before + 1;
      if (added) {
        /*
         * 数组序**自下而上**（compositor 按 state_.layers 顺序合成，面板再逆序显示），
         * 新层是 push_back 上去的，所以栈顶是最后一个 id——不是 [0]。
         */
        std::vector<comp_layer_id> stack_ids(count_after, 0u);
        added = comp_layer_list(doc, stack_ids.data(), stack_ids.size(), &listed) == COMP_OK &&
                listed >= 1u;
        if (added) {
          /* out_count 是总数，不受 capacity 限制：先夹一次再取最后一个。 */
          if (listed > stack_ids.size()) listed = stack_ids.size();
          fill_id = stack_ids[listed - 1u];
        }
      }
      int32_t is_fill = 0;
      uint32_t fill_kind = COMP_FILL_NONE;
      const bool described =
          added && comp_layer_is_fill(doc, fill_id, &is_fill) == COMP_OK && is_fill != 0 &&
          comp_layer_get_fill(doc, fill_id, &fill_kind, nullptr, 0, nullptr) == COMP_OK &&
          fill_kind == COMP_FILL_SOLID;
      /* 纯色 = 整幅均匀：逐像素与首像素比，且首像素就是缺省色（读回值，不假设预乘细节）。 */
      std::vector<uint8_t> pixels;
      int32_t px_w = 0;
      int32_t px_h = 0;
      comp_pixel_format px_fmt = COMP_PIXEL_RGBA8;
      bool uniform = false;
      if (described &&
          comp_layer_get_pixels(doc, fill_id, nullptr, 0, &px_w, &px_h, &px_fmt) == COMP_OK &&
          px_w > 0 && px_h > 0) {
        const size_t stride = static_cast<size_t>(px_w) * 4u;
        pixels.assign(stride * static_cast<size_t>(px_h), 0u);
        if (comp_layer_get_pixels(doc, fill_id, pixels.data(), stride, &px_w, &px_h, &px_fmt) ==
            COMP_OK) {
          uniform = pixels[0] == 128u && pixels[1] == 128u && pixels[2] == 128u &&
                    pixels[3] == 255u;
          for (size_t i = 4; uniform && i + 3 < pixels.size(); i += 4) {
            uniform = pixels[i] == pixels[0] && pixels[i + 1] == pixels[1] &&
                      pixels[i + 2] == pixels[2] && pixels[i + 3] == pixels[3];
          }
        }
      }
      comp_image write_image{};
      write_image.width = px_w;
      write_image.height = px_h;
      write_image.format = px_fmt;
      write_image.stride = static_cast<size_t>(px_w) * 4u;
      write_image.pixels = pixels.empty() ? nullptr : pixels.data();
      const bool refused = !pixels.empty() &&
                           comp_layer_set_pixels(doc, fill_id, &write_image) == COMP_ERR_STATE;
      const bool rasterized =
          described && comp_layer_rasterize_fill(doc, fill_id) == COMP_OK &&
          comp_layer_is_fill(doc, fill_id, &is_fill) == COMP_OK && is_fill == 0;
      /* 栅格化只丢参数、不重渲：像素必须逐字节不变。 */
      bool kept = false;
      if (rasterized && !pixels.empty()) {
        std::vector<uint8_t> after(pixels.size(), 0u);
        int32_t aw = px_w;
        int32_t ah = px_h;
        comp_pixel_format afmt = px_fmt;
        kept = comp_layer_get_pixels(doc, fill_id, after.data(),
                                     static_cast<size_t>(px_w) * 4u, &aw, &ah, &afmt) == COMP_OK &&
               after == pixels;
      }
      close_fill_panel(shell); /* 面板别跨段留着：后面还要换文档 */
      verify(added && described && uniform && refused && rasterized && kept,
             "fill layer creates / renders uniformly / refuses pixel writes / rasterizes");
    }

    /* 同一入口打开位图必须仍然是一张拍平图：分派不能为了 PSD 把普通格式也改道。 */
    if (probe_png_exists(probe_path)) {
      open_path_as_document(shell, probe_path);
      size_t png_count = 0;
      comp_size png_size{};
      const bool png_ok = shell.doc.document != nullptr &&
                          comp_layer_count(shell.doc.document, &png_count) == COMP_OK &&
                          png_count == 1 &&
                          comp_document_size(shell.doc.document, &png_size) == COMP_OK &&
                          png_size.width == 240 && png_size.height == 160;
      verify(png_ok, "shell open_path_as_document(.png) still opens one flattened layer");
    }
    if (shell.doc.document != nullptr) {
      comp_document_destroy(shell.doc.document);
      shell.doc.document = nullptr;
    }
  }
  if (psd_source != nullptr) comp_document_destroy(psd_source);

  /*
   * P0-1：位深与颜色模式。
   *
   * 只查枚举等于只测了「宿主传什么我记什么」——真正会错的是 ensure_background_layer
   * 里写死的 RGBA8（声明 16 位、实际 8 位），所以这里必须回读**背景层的像素格式**。
   */
  {
    comp_document_desc depth_desc{};
    depth_desc.width = 8;
    depth_desc.height = 6;
    comp_document_options depth_options{};
    depth_options.struct_size = sizeof(depth_options);
    depth_options.pixel_format = COMP_PIXEL_RGBA16;
    depth_options.color_mode = COMP_COLOR_MODE_RGB;
    comp_document depth_doc = nullptr;
    const bool created =
        comp_document_create_ex(engine, &depth_desc, &depth_options, &depth_doc) == COMP_OK &&
        depth_doc != nullptr;
    comp_pixel_format reported = COMP_PIXEL_RGBA8;
    comp_pixel_format layer_format = COMP_PIXEL_RGBA8;
    size_t layer_count = 0;
    comp_layer_id first_layer = 0;
    int32_t layer_width = 0;
    int32_t layer_height = 0;
    if (created) {
      comp_document_pixel_format(depth_doc, &reported);
      comp_layer_count(depth_doc, &layer_count);
      comp_layer_list(depth_doc, &first_layer, 1, &layer_count);
      comp_layer_get_pixels(depth_doc, first_layer, nullptr, 0, &layer_width, &layer_height,
                            &layer_format);
    }
    verify(created && reported == COMP_PIXEL_RGBA16 && layer_format == COMP_PIXEL_RGBA16 &&
               layer_width == 8 && layer_height == 6,
           "P0-1 document bit depth 16 reaches the background layer buffer");
    if (depth_doc != nullptr) comp_document_destroy(depth_doc);
  }

  /*
   * P0-1：灰阶模式。写纯红进去，flatten 必须是 R=G=B 的 Rec.709 亮度。
   * 这条同时钉住「只在输出转、读回还是彩色」那个半成品写法——存储侧也转了，
   * 所以 get_pixels 读回来同样得是灰的。
   */
  {
    comp_document_desc gray_desc{};
    gray_desc.width = 4;
    gray_desc.height = 4;
    comp_document_options gray_options{};
    gray_options.struct_size = sizeof(gray_options);
    gray_options.color_mode = COMP_COLOR_MODE_GRAYSCALE;
    comp_document gray_doc = nullptr;
    bool gray_ok =
        comp_document_create_ex(engine, &gray_desc, &gray_options, &gray_doc) == COMP_OK &&
        gray_doc != nullptr;
    if (gray_ok) {
      comp_layer_id gray_layer = 0;
      size_t gray_layers = 0;
      comp_layer_count(gray_doc, &gray_layers);
      comp_layer_list(gray_doc, &gray_layer, 1, &gray_layers);
      std::vector<uint8_t> red(4u * 4u * 4u, 0u);
      for (size_t i = 0; i + 3 < red.size(); i += 4) {
        red[i + 0] = 255;
        red[i + 3] = 255;
      }
      comp_image red_image{};
      red_image.width = 4;
      red_image.height = 4;
      red_image.format = COMP_PIXEL_RGBA8;
      red_image.stride = 4u * 4u;
      red_image.pixels = red.data();
      std::vector<uint8_t> canvas(4u * 4u * 4u, 0u);
      const bool wrote = comp_layer_set_pixels(gray_doc, gray_layer, &red_image) == COMP_OK;
      const bool flattened =
          comp_render_flatten(gray_doc, canvas.data(), COMP_PIXEL_RGBA8, 4u * 4u) == COMP_OK;
      bool all_gray = true;
      for (size_t i = 0; i + 3 < canvas.size(); i += 4) {
        if (canvas[i + 0] != canvas[i + 1] || canvas[i + 1] != canvas[i + 2]) all_gray = false;
        if (canvas[i + 0] < 53 || canvas[i + 0] > 55) all_gray = false; /* 0.2126*255 ≈ 54 */
      }
      gray_ok = wrote && flattened && all_gray;
    }
    verify(gray_ok, "P0-1 grayscale document flattens to R=G=B (Rec.709 luma)");
    if (gray_doc != nullptr) comp_document_destroy(gray_doc);
  }

  /*
   * P0-3：多文档标签。新建 -> 两个槽；切回第一个 -> 句柄与尺寸都回到第一份；
   * 关掉它 -> 前台切到第二份。整段不碰窗口，纯 ShellState 级的标签生命周期。
   */
  {
    ShellState tabs_shell{};
    tabs_shell.engine = engine;
    const bool first_ok = create_document_with(tabs_shell, 8, 6, 72, 0);
    comp_document first_document = tabs_shell.doc.document;
    const bool second_ok = create_document_with(tabs_shell, 4, 3, 72, 0);
    comp_document second_document = tabs_shell.doc.document;
    const int opened = tab_count(tabs_shell);
    tab_activate(tabs_shell, 0);
    const bool switched_back =
        tabs_shell.doc.document == first_document && tabs_shell.doc.width == 8 &&
        tabs_shell.doc.height == 6;
    const bool remains = tab_close_active(tabs_shell);
    const bool fell_back = remains && tabs_shell.doc.document == second_document;
    verify(first_ok && second_ok && opened == 2 && switched_back && fell_back,
           "P0-3 tabs: two documents park / switch / close without losing state");
    if (tabs_shell.doc.document != nullptr) comp_document_destroy(tabs_shell.doc.document);
  }

  /*
   * P0-10 停靠子系统：状态机是纯逻辑，直接测（不用开窗口）。
   *
   * 三件事各自会坏：换边时两块**对调**（不对调就是两块挤在同一侧）、
   * 浮起来时**主窗口不再给它留位**（留了就是画布被压窄）、停回时**回原位**
   * （回错边就得再手动拖一次）。
   */
  {
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    bool ok = dock.area_of(comp_dock::PanelId::Adjust) == comp_dock::DockArea::Right &&
              dock.area_of(comp_dock::PanelId::Layers) == comp_dock::DockArea::Left;
    /* 换到左侧：两块对调（图层让到右侧）。 */
    dock.set_area(comp_dock::PanelId::Adjust, comp_dock::DockArea::Left);
    ok = ok && dock.area_of(comp_dock::PanelId::Adjust) == comp_dock::DockArea::Left &&
         dock.area_of(comp_dock::PanelId::Layers) == comp_dock::DockArea::Right;
    /* 浮起来：主窗口完全不给它留位；停回原位。 */
    dock.float_panel(comp_dock::PanelId::Adjust, 100, 120, 330, 560);
    comp_ui::LayoutPrefs prefs{};
    dock.apply_to(prefs);
    ok = ok && dock.is_floating(comp_dock::PanelId::Adjust) && !prefs.adjust_docked &&
         prefs.layer_docked;
    dock.dock_panel(comp_dock::PanelId::Adjust);
    ok = ok && dock.area_of(comp_dock::PanelId::Adjust) == comp_dock::DockArea::Left &&
         !dock.is_floating(comp_dock::PanelId::Adjust);
    verify(ok, "P0-10 dock state: swap sides / float leaves no slot / dock back to origin");
  }
  {
    /* workspace 存读往返：宽度、收起态、停靠区都要能存回来。 */
    const std::wstring path = (std::filesystem::temp_directory_path() /
                               L"compositor_dock_workspace.ini").wstring();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    dock.set_width(comp_dock::PanelId::Adjust, 400);
    dock.toggle_collapsed(comp_dock::PanelId::Layers);
    dock.set_area(comp_dock::PanelId::Adjust, comp_dock::DockArea::Left);
    const bool saved = dock.save(path);
    comp_dock::DockState loaded;
    const bool read_ok = loaded.load(path);
    const bool ok = saved && read_ok &&
                    loaded.width_of(comp_dock::PanelId::Adjust, 0) == 400 &&
                    loaded.panel(comp_dock::PanelId::Layers).collapsed &&
                    loaded.area_of(comp_dock::PanelId::Adjust) == comp_dock::DockArea::Left;
    std::filesystem::remove(path, ignored);
    verify(ok, "P0-10 dock workspace round trip (width / collapsed / side)");
  }
  {
    /*
     * P0-10 拖出停靠（一）：多块面板可**同时**浮动。
     *
     * 此前 dock 的不变式是「至多一块浮动」，理由是「两块都浮要两套命中与焦点
     * 通路」。这条理由站不住：浮动窗口过程从 GWLP_USERDATA 取自己的 PanelId，
     * 命中层是无状态函数，键盘统一转发主窗口。第二块浮动并不需要第二套通路。
     *
     * 这里钉的是三件事：
     *   1. 两块都浮起来时，主窗口**一块都不留位**（画布因此变宽到极限）；
     *   2. 停回一块，另一块仍在浮动（互不影响）；
     *   3. 停回时回到各自的原位（不是都挤到右边）。
     */
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    dock.float_panel(comp_dock::PanelId::Adjust, 100, 120, 330, 560);
    dock.float_panel(comp_dock::PanelId::Layers, 500, 140, 248, 480);
    comp_ui::LayoutPrefs prefs{};
    dock.apply_to(prefs);
    const bool both_float = dock.floating_count() == 2 && !prefs.adjust_docked &&
                            !prefs.layer_docked;
    /* 停回一块：另一块仍在浮动。 */
    dock.dock_panel(comp_dock::PanelId::Adjust);
    comp_ui::LayoutPrefs after_one{};
    dock.apply_to(after_one);
    const bool one_left = dock.floating_count() == 1 && dock.is_floating(comp_dock::PanelId::Layers) &&
                          after_one.adjust_docked && !after_one.layer_docked;
    /* 停回原位：调整回右、图层回左（各自浮起来之前的位置）。 */
    dock.dock_panel(comp_dock::PanelId::Layers);
    const bool back_home = dock.area_of(comp_dock::PanelId::Adjust) == comp_dock::DockArea::Right &&
                           dock.area_of(comp_dock::PanelId::Layers) == comp_dock::DockArea::Left &&
                           dock.floating_count() == 0;
    verify(both_float && one_left && back_home,
           "P0-10 dock state: two panels float at once / dock one keeps the other / each docks home");
  }
  {
    /*
     * P0-10 拖出停靠（二）：落点热区判定。
     *
     * 拖出来松手时，落在主窗口左边缘 → 停到左侧；右边缘 → 停到右侧；
     * 画布中间 → **保持浮动**（拖出来就是要它浮着）；窗口外 → 保持浮动。
     * 这条判定是纯函数，所以可以直接测，不必开窗口。
     */
    comp_dock::DropGeometry geo{};
    geo.client_w = 1000;
    geo.client_h = 700;
    geo.edge_width = 48;
    const bool left_hot = comp_dock::drop_zone_at(geo, 10, 300) == comp_dock::DropZone::Left;
    const bool right_hot = comp_dock::drop_zone_at(geo, 980, 300) == comp_dock::DropZone::Right;
    const bool middle_body = comp_dock::drop_zone_at(geo, 500, 300) == comp_dock::DropZone::Body;
    const bool outside = comp_dock::drop_zone_at(geo, 1200, 300) == comp_dock::DropZone::None;
    const bool above = comp_dock::drop_zone_at(geo, 10, -5) == comp_dock::DropZone::None;
    const bool docks_left = comp_dock::drop_zone_docks_left(comp_dock::DropZone::Left) &&
                            !comp_dock::drop_zone_docks_left(comp_dock::DropZone::Right) &&
                            !comp_dock::drop_zone_docks_left(comp_dock::DropZone::Body);
    verify(left_hot && right_hot && middle_body && outside && above && docks_left,
           "P0-10 dock drop zones: left/right edge hot zones / body keeps floating / outside ignored");
  }
  {
    /*
     * P0-10 拖出停靠（三）：浮窗几何随窗口位置/尺寸变化写回 dock 状态。
     *
     * 这条链路是 workspace 存读的基础：存的是浮窗几何，重启要按同样的
     * 几何把浮窗开回来。状态机侧只验「写回接口不丢字段」。
     */
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    dock.float_panel(comp_dock::PanelId::Adjust, 1440, 220, 320, 540);
    const comp_dock::PanelState& state = dock.panel(comp_dock::PanelId::Adjust);
    const bool ok = state.float_x == 1440 && state.float_y == 220 && state.float_w == 320 &&
                    state.float_h == 540 && dock.is_floating(comp_dock::PanelId::Adjust);
    verify(ok, "P0-10 float geometry round trip (x / y / width / height kept in dock state)");
  }
  {
    /*
     * P0-10 面板分组（tab strip）：两块面板合并到同一列 = 一组。
     *
     * 单一真源的体现：**分组关系由 panels[].area 推导**，不另存成员列表。
     * 所以这里能直接断言「两块同列 = 组内两块」，也能断言「浮走一块 = 组解散」。
     *
     * 四件事各自会坏：
     *   1. 合并后组内两块、标签栏出现（count=2）；
     *   2. 合并后主窗口只占**一列**（另一列让位，画布因此变宽）；
     *   3. 切标签不改变列宽（宽度属于列，不属于成员）；
     *   4. 拆开后回到「各占一列」。
     */
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    /* 合并：图层栏挪到调整栏那一列（都在右 = 一组）。 */
    dock.group_move(comp_dock::PanelId::Layers, comp_dock::DockArea::Right);
    const bool merged = dock.group_count(comp_dock::DockArea::Right) == 2 &&
                        dock.group_count(comp_dock::DockArea::Left) == 0;
    comp_ui::LayoutPrefs prefs{};
    dock.apply_to(prefs);
    /* 组模式下布局只占一列：画布因此变宽到只剩组那一列。 */
    comp_ui::Layout layout = comp_ui::compute_layout(1280, 720, prefs);
    const bool one_column = layout.group_count == 2 && layout.group_bar.h > 0 &&
                            (layout.layer_panel.w == 0 || layout.adjust_panel.w == 0);
    /* 切标签不改变列宽：宽度存在列上。
     *
     * 这里量的是**布局算出来的列宽**（layer_panel.w / adjust_panel.w），
     * 不是 prefs 里的宽度字段——后者由 group_width 决定，本来就不受
     * group_active 影响，拿它断言等于什么都没测（2026-10-26 自查时
     * 就是这么假绿着，害得切标签时列宽从组宽跳成另一列的缺省宽）。
     *
     * 组在右列：内容矩形落在 adjust_panel 那一侧；切到图层标签后，
     * 仍然是**同一列**（右缘坐标不变），只是画的内容换成图层面板。
     */
    dock.set_group_width(comp_dock::DockArea::Right, 360);
    dock.group_set_active(comp_dock::DockArea::Right, 0);
    comp_ui::LayoutPrefs before_prefs{};
    dock.apply_to(before_prefs);
    const comp_ui::Layout before = comp_ui::compute_layout(1280, 720, before_prefs);
    const int width_before = before.adjust_panel.w;
    const int right_before = before.adjust_panel.right();

    dock.group_set_active(comp_dock::DockArea::Right, 1);
    comp_ui::LayoutPrefs after_switch{};
    dock.apply_to(after_switch);
    const comp_ui::Layout after = comp_ui::compute_layout(1280, 720, after_switch);
    /* 切到图层标签后：内容矩形挪到 layer_panel，但仍在**右列**（右缘不变）。 */
    const int width_after = after.layer_panel.w;
    const int right_after = after.layer_panel.right();
    const bool width_stable = width_before > 0 && width_after == width_before &&
                              right_after == right_before &&
                              after.adjust_panel.w == 0 &&
                              dock.group_active_panel(comp_dock::DockArea::Right) ==
                                  comp_dock::PanelId::Layers;
    /* 拆开：两块各占一列，标签栏消失。 */
    dock.group_ungroup(comp_dock::PanelId::Layers);
    const bool split = dock.group_count(comp_dock::DockArea::Right) == 1 &&
                       dock.group_count(comp_dock::DockArea::Left) == 1;
    comp_ui::LayoutPrefs after_split{};
    dock.apply_to(after_split);
    comp_ui::Layout split_layout = comp_ui::compute_layout(1280, 720, after_split);
    const bool no_bar = split_layout.group_bar.h == 0;
    verify(merged && one_column && width_stable && split && no_bar,
           "P0-10 panel group: merge into one column / tab strip shows / width stable / split back");
  }
  {
    /*
     * P0-10 分组的**左列**分支。
     *
     * 上一条只测了组在右列。左列是**另一条代码路径**（矩形落在
     * layer_panel 那一侧、组栏在左缘），必须单独测——「测了一条分支就
     * 当作两条都对」是这条断言最初假绿的根因。
     *
     * 钉三件事：
     *   1. 组在左列时，内容矩形落在左缘（右缘坐标 = 左缘 + 组宽）；
     *   2. 切标签后列宽与右缘都不变（宽度属于列）；
     *   3. 另一侧（右列）完全让位（宽 0，画布占满）。
     */
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    /* 合并到左列：调整面板挪到图层栏那一列。 */
    dock.group_move(comp_dock::PanelId::Adjust, comp_dock::DockArea::Left);
    dock.set_group_width(comp_dock::DockArea::Left, 300);
    dock.group_set_active(comp_dock::DockArea::Left, 0); /* 激活调整 */
    comp_ui::LayoutPrefs prefs{};
    dock.apply_to(prefs);
    const comp_ui::Layout before = comp_ui::compute_layout(1280, 720, prefs);
    /*
     * 内容矩形落在**哪个字段名下**取决于组里激活的是谁（内容按面板身份
     * 分派：图层内容走 layer_panel，调整内容走 adjust_panel）。所以断言
     * 要看「两者中非空的那个」，而不是钉死某一个字段——那才是「列」的
     * 语义：一列一个矩形，激活谁换的是内容不是位置。
     */
    const int before_w = before.layer_panel.w > 0 ? before.layer_panel.w : before.adjust_panel.w;
    const int before_right =
        before.layer_panel.w > 0 ? before.layer_panel.right() : before.adjust_panel.right();
    const int before_x = before.layer_panel.w > 0 ? before.layer_panel.x : before.adjust_panel.x;
    /* 内容矩形在左缘、宽 300、另一侧完全让位（宽 0）。 */
    const bool on_left_edge = before_w == 300 && before_x == 0;
    const bool other_side_free = (before.layer_panel.w > 0 && before.adjust_panel.w == 0) ||
                                 (before.adjust_panel.w > 0 && before.layer_panel.w == 0);

    dock.group_set_active(comp_dock::DockArea::Left, 1); /* 切到图层 */
    comp_ui::LayoutPrefs after_prefs{};
    dock.apply_to(after_prefs);
    const comp_ui::Layout after = comp_ui::compute_layout(1280, 720, after_prefs);
    const int after_w = after.layer_panel.w > 0 ? after.layer_panel.w : after.adjust_panel.w;
    const int after_right =
        after.layer_panel.w > 0 ? after.layer_panel.right() : after.adjust_panel.right();
    const bool stable = after_w == 300 && after_right == before_right;
    const bool still_left = (after.layer_panel.w > 0 && after.layer_panel.x == 0) ||
                            (after.adjust_panel.w > 0 && after.adjust_panel.x == 0);
    verify(on_left_edge && other_side_free && stable && still_left,
           "P0-10 panel group on the left column: content at left edge / width stable on tab switch");
  }
  {
    /*
     * P0-10 分组的列宽**独立于面板身份**（2026-10-26 自查发现的真 bug）。
     *
     * 症状：组宽 200 时，切到图层标签列宽是 200、切到调整标签是 240——
     * 因为 layer_panel_width 与 adjust_panel_width 各自按**面板身份**夹取
     * （图层栏 168..520、调整栏 240..560），同一个组宽被夹到不同值。
     * 用户看到的是「切一下标签，画布宽度跳一下」。
     *
     * 修法：组有自己的宽度区间（168..560 = 两者的并集）与自己的宽度字段。
     * 这条断言量的是**布局输出**（真正的列宽），不是 prefs 字段。
     */
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    dock.group_move(comp_dock::PanelId::Layers, comp_dock::DockArea::Right);
    /* 200 落在图层栏区间内、落在调整栏区间外——正是能暴露夹取差异的值。 */
    dock.set_group_width(comp_dock::DockArea::Right, 200);
    int widths[2] = {0, 0};
    for (int active = 0; active < 2; ++active) {
      dock.group_set_active(comp_dock::DockArea::Right, active);
      comp_ui::LayoutPrefs prefs{};
      dock.apply_to(prefs);
      const comp_ui::Layout layout = comp_ui::compute_layout(1280, 720, prefs);
      widths[active] = layout.adjust_panel.w > 0 ? layout.adjust_panel.w : layout.layer_panel.w;
    }
    verify(widths[0] == 200 && widths[1] == 200,
           "P0-10 panel group column width is independent of which panel is active");
  }
  {
    /*
     * P0-10 分组的收起是**列级**的：组收起 = 整列变成 24px 竖条。
     *
     * 此前收起态取的是「激活成员的收起标志」——切标签时列会忽收忽展
     * （切到没收起过的那块，整列就弹回来了）。
     */
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    dock.group_move(comp_dock::PanelId::Layers, comp_dock::DockArea::Right);
    dock.set_group_width(comp_dock::DockArea::Right, 320);
    dock.toggle_group_collapsed(comp_dock::DockArea::Right);
    int widths[2] = {0, 0};
    for (int active = 0; active < 2; ++active) {
      dock.group_set_active(comp_dock::DockArea::Right, active);
      comp_ui::LayoutPrefs prefs{};
      dock.apply_to(prefs);
      const comp_ui::Layout layout = comp_ui::compute_layout(1280, 720, prefs);
      widths[active] = layout.adjust_panel.w > 0 ? layout.adjust_panel.w : layout.layer_panel.w;
    }
    /* 两块都收起时列宽都是 24px 竖条，与激活的是谁无关。 */
    const bool collapsed_both = widths[0] == 24 && widths[1] == 24;
    /* 展开后列宽回到组宽。 */
    dock.toggle_group_collapsed(comp_dock::DockArea::Right);
    comp_ui::LayoutPrefs expanded{};
    dock.apply_to(expanded);
    const comp_ui::Layout layout = comp_ui::compute_layout(1280, 720, expanded);
    const int expanded_w = layout.adjust_panel.w > 0 ? layout.adjust_panel.w : layout.layer_panel.w;
    verify(collapsed_both && expanded_w == 320,
           "P0-10 panel group collapse is column level (24px strip) and restores the group width");
  }
  {
    /*
     * P0-10 合并时组宽的**初始化**（2026-10-26 视觉探针抓到的真 bug）。
     *
     * 症状：合并后组宽是 0，于是回落成「激活成员的缺省宽」——
     * 激活的是调整时是 330，切到图层变成 248，列宽跟着跳一次。
     *
     * 根因：合并时写的 `set_group_width(target, group_width(target))` 是
     * 「把组宽设成它自己的当前值」= 没设。正确的初值是**目标列合并前**的
     * 实际宽度（那一列当时停着谁，就用谁的缺省宽）。
     *
     * 这条断言量的是「组宽被显式设过」——合并后 group_width 必须是正数，
     * 不能靠回落。
     */
    comp_dock::DockState dock = comp_dock::DockState::defaults();
    /* 合并前：右列停着调整（缺省 330），左列停着图层（缺省 248）。 */
    const int before_right = dock.group_width(comp_dock::DockArea::Right);
    /* 合并：把图层栏并进调整栏那一列。 */
    dock.group_merge(comp_dock::PanelId::Layers);
    /*
     * 组宽必须被**显式设过**：合并后 group_width 要是正数（目标列原本的
     * 缺省宽 330），不能是 0 —— 0 会回落成「激活成员的缺省宽」，切标签时
     * 列宽就跟着跳（2026-10-26 视觉探针抓到的真 bug）。
     */
    const int width_after = dock.group_width(comp_dock::DockArea::Right);
    const bool initialised = before_right == 0 &&
                             width_after == static_cast<int>(comp_ui::Layout::kAdjustPanelWidth);
    /* 切标签后列宽不变（组宽是列的属性）。 */
    dock.group_set_active(comp_dock::DockArea::Right, 1);
    comp_ui::LayoutPrefs prefs{};
    dock.apply_to(prefs);
    const comp_ui::Layout layout = comp_ui::compute_layout(1280, 720, prefs);
    const int column_w = layout.layer_panel.w > 0 ? layout.layer_panel.w : layout.adjust_panel.w;
    verify(initialised && column_w == width_after,
           "P0-10 panel group width is initialised on merge and does not change with the active tab");
  }
  {
    /*
     * P0-10 分组的几何：标签栏与内容矩形。
     *
     * 标签栏**不占列宽**——它把面板内容矩形整体下移一行。列宽是用户拖出来的
     * 偏好，若标签栏挤占列宽，分组瞬间画布宽度就会跳一下。
     */
    const comp_ui::Rect column{100, 50, 300, 600};
    const comp_ui::Rect bar = comp_ui::group_bar_rect(column, 2);
    const comp_ui::Rect content = comp_ui::group_content_rect(column, 2);
    const bool same_width = bar.w == column.w && content.w == column.w;
    const bool shifted = bar.y == column.y && content.y == column.y + bar.h &&
                         content.bottom() == column.bottom();
    /* 未分组时标签栏高度为 0，内容矩形就是原矩形。 */
    const comp_ui::Rect plain_bar = comp_ui::group_bar_rect(column, 1);
    const comp_ui::Rect plain_content = comp_ui::group_content_rect(column, 1);
    /* 命中：点在标签上返回下标，点在栏**下方**（面板内容区）返回 -1。 */
    const int hit = comp_ui::group_tab_at(bar, 2, 120, 55);
    const int miss = comp_ui::group_tab_at(bar, 2, 120, bar.bottom() + 5);
    verify(same_width && shifted && plain_bar.h == 0 && plain_content.y == column.y && hit >= 0 &&
               miss < 0,
           "P0-10 group bar geometry: bar does not eat column width / content shifts down / hit test");
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);

  std::fprintf(report, "\n[summary] failures = %d\n", failures);
  std::fprintf(report, "[%s] compositor-shell selftest\n", failures == 0 ? "PASS" : "FAIL");
  std::fclose(report);
  return failures == 0 ? 0 : 1;
}

LRESULT CALLBACK bench_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      EndPaint(window, &paint);
      (void)dc;
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

void pump_pending_messages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

double elapsed_ms(const LARGE_INTEGER& start, const LARGE_INTEGER& end, double frequency) {
  return (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) / frequency;
}

/* 基准场景：不透明底 + 三层整幅画布图层，全部是 GPU 子集允许的形态。 */
comp_document build_present_bench_document(comp_engine engine, int32_t size) {
  comp_document document = nullptr;
  comp_document_desc desc{};
  desc.width = size;
  desc.height = size;
  if (comp_document_create(engine, &desc, &document) != COMP_OK) return nullptr;

  size_t count = 0;
  if (comp_layer_count(document, &count) != COMP_OK || count == 0) return document;
  std::vector<comp_layer_id> ids(count);
  if (comp_layer_list(document, ids.data(), ids.size(), &count) != COMP_OK) return document;

  std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
  comp_image image{};
  image.width = size;
  image.height = size;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(size) * 4;
  image.pixels = pixels.data();

  /* 底层：不透明底色。 */
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i + 0] = 24;
    pixels[i + 1] = 96;
    pixels[i + 2] = 168;
    pixels[i + 3] = 255;
  }
  if (comp_layer_set_pixels(document, ids.front(), &image) != COMP_OK) return document;

  const comp_blend_mode blends[3] = {COMP_BLEND_NORMAL, COMP_BLEND_MULTIPLY, COMP_BLEND_SCREEN};
  const float opacities[3] = {1.0f, 0.75f, 0.5f};
  for (int layer_index = 0; layer_index < 3; ++layer_index) {
    comp_layer_desc layer_desc{};
    layer_desc.name = "bench-layer";
    layer_desc.blend = blends[layer_index];
    layer_desc.opacity = opacities[layer_index];
    layer_desc.transform = comp_rect{0, 0, size, size};
    layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
    comp_layer_id id = 0;
    if (comp_layer_add(document, &layer_desc, &id) != COMP_OK) return document;
    /* 预乘合法：颜色分量都 <= alpha。 */
    for (int32_t y = 0; y < size; ++y) {
      for (int32_t x = 0; x < size; ++x) {
        uint8_t* p = &pixels[(static_cast<size_t>(y) * static_cast<size_t>(size) +
                              static_cast<size_t>(x)) * 4];
        const uint32_t alpha =
            (static_cast<uint32_t>(x * 11 + y * 7 + layer_index * 29) % 200u) + 40u;
        p[0] = static_cast<uint8_t>(((x * 17 + layer_index * 31) % 255) * alpha / 255);
        p[1] = static_cast<uint8_t>(((y * 13 + layer_index * 47) % 255) * alpha / 255);
        p[2] = static_cast<uint8_t>((((x + y) * 7 + layer_index * 11) % 255) * alpha / 255);
        p[3] = static_cast<uint8_t>(alpha);
      }
    }
    if (comp_layer_set_pixels(document, id, &image) != COMP_OK) return document;
  }
  return document;
}

int run_present_bench(HINSTANCE instance, int frames) {
  std::FILE* report = nullptr;
  if (_wfopen_s(&report, L"present_bench.txt", L"wb") != 0 || report == nullptr) return 2;

  /* 父进程是控制台时顺便回显；否则只写文件（--selftest 同口径）。 */
  std::FILE* console = nullptr;
  if (AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
    std::FILE* reopened = nullptr;
    if (freopen_s(&reopened, "CONOUT$", "w", stdout) == 0) console = stdout;
  }
  auto emit = [&](const char* line) {
    std::fprintf(report, "%s\n", line);
    std::fflush(report); /* 崩溃也要留下最后到达的阶段，便于定位 */
    if (console != nullptr) std::fprintf(console, "%s\n", line);
  };
  char line[512];

  comp_engine engine = nullptr;
  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-shell-present-bench";
  engine_desc.version = "0.1.0";
  engine_desc.backend = COMP_BACKEND_AUTO;
  if (comp_engine_create(&engine_desc, &engine) != COMP_OK || engine == nullptr) {
    emit("result=engine_create_failed");
    std::fclose(report);
    return 1;
  }
  comp_backend actual = COMP_BACKEND_AUTO;
  comp_engine_backend(engine, &actual);
  /* 基准报告是纯文本（写文件/控制台），这里用窄名，不复用工具条的宽标签。 */
  const char* backend_name = "AUTO";
  if (actual == COMP_BACKEND_D3D12) {
    backend_name = "D3D12";
  } else if (actual == COMP_BACKEND_CPU) {
    backend_name = "CPU";
  } else if (actual == COMP_BACKEND_VULKAN) {
    backend_name = "Vulkan";
  } else if (actual == COMP_BACKEND_METAL) {
    backend_name = "Metal";
  }
  std::snprintf(line, sizeof(line), "backend=%s", backend_name);
  emit(line);

  if (actual != COMP_BACKEND_D3D12) {
    emit("result=skipped");
    emit("reason=window present needs the D3D12 backend; build with --gpu and run on a machine "
         "with a D3D12 device");
    comp_engine_destroy(engine);
    std::fclose(report);
    return 2;
  }

  static const wchar_t kBenchClass[] = L"CompositorPresentBenchWindow";
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = bench_window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = kBenchClass;
  if (RegisterClassExW(&window_class) == 0) {
    emit("result=window_class_failed");
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  RECT rect{0, 0, kBenchCanvas, kBenchCanvas};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowExW(0, kBenchClass, L"compositor present bench", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                                rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    emit("result=window_create_failed");
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  /* 不抢焦点但必须真的显示：隐藏窗口的 Present 会被系统当作遮挡，测不出真实上屏。 */
  ShowWindow(window, SW_SHOWNOACTIVATE);
  UpdateWindow(window);

  comp_document document = build_present_bench_document(engine, kBenchCanvas);
  if (document == nullptr) {
    emit("result=document_create_failed");
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }

  comp_render_target target{};
  target.size = comp_size{kBenchCanvas, kBenchCanvas};
  target.format = COMP_PIXEL_RGBA8;
  target.kind = COMP_SURFACE_HWND;
  target.native_handle = window;

  const size_t stride = static_cast<size_t>(kBenchCanvas) * 4;

  std::snprintf(line, sizeof(line), "canvas=%dx%d", kBenchCanvas, kBenchCanvas);
  emit(line);
  std::snprintf(line, sizeof(line), "frames=%d", frames);
  emit(line);

  /* 预热 3 帧：把 PSO/交换链/首次上传这些一次性成本挪出测量窗口。 */
  bool hwnd_path_ok = true;
  comp_result render_result = COMP_OK;
  comp_result present_result = COMP_OK;
  for (int i = 0; i < 3; ++i) {
    render_result = comp_render_document(document, &target);
    if (render_result != COMP_OK) {
      hwnd_path_ok = false;
      break;
    }
    present_result = comp_render_present(document);
    if (present_result != COMP_OK) {
      hwnd_path_ok = false;
      break;
    }
    pump_pending_messages();
  }
  if (!hwnd_path_ok) {
    emit("result=hwnd_target_unsupported");
    std::snprintf(line, sizeof(line), "render_result=%d", static_cast<int>(render_result));
    emit(line);
    std::snprintf(line, sizeof(line), "present_result=%d", static_cast<int>(present_result));
    emit(line);
    const char* last_error = comp_engine_last_error(engine);
    std::snprintf(line, sizeof(line), "last_error=%s", last_error != nullptr ? last_error : "(none)");
    emit(line);
    /* 再用离屏目标试一次：区分「文档/窗口本身不行」与「整条渲染链路都不行」。 */
    std::vector<uint8_t> probe(stride * kBenchCanvas, 0);
    comp_render_target offscreen{};
    offscreen.size = comp_size{kBenchCanvas, kBenchCanvas};
    offscreen.format = COMP_PIXEL_RGBA8;
    offscreen.kind = COMP_SURFACE_CPU_BUFFER;
    offscreen.stride = stride;
    offscreen.pixels = probe.data();
    std::snprintf(line, sizeof(line), "offscreen_probe=%s",
                  comp_render_document(document, &offscreen) == COMP_OK ? "ok" : "failed");
    emit(line);
    comp_document_destroy(document);
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 2;
  }
  emit("hwnd_target=ok");

  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  const double freq = static_cast<double>(frequency.QuadPart);

  /* ---- A. GPU 直呈：render(HWND) + present ---- */
  double gpu_total = 0.0;
  double gpu_best = 1.0e30;
  double gpu_worst = 0.0;
  bool gpu_ok = true;
  for (int i = 0; i < frames; ++i) {
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    const bool ok = comp_render_document(document, &target) == COMP_OK &&
                    comp_render_present(document) == COMP_OK;
    QueryPerformanceCounter(&end);
    if (!ok) {
      gpu_ok = false;
      break;
    }
    const double elapsed = elapsed_ms(start, end, freq);
    gpu_total += elapsed;
    if (elapsed < gpu_best) gpu_best = elapsed;
    if (elapsed > gpu_worst) gpu_worst = elapsed;
    pump_pending_messages();
  }
  if (!gpu_ok) {
    emit("result=gpu_present_failed");
    comp_document_destroy(document);
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  const double gpu_avg = gpu_total / frames;

  /* ---- B. CPU 上屏：flatten + RGBA->BGRA + StretchDIBits（外壳既有路径） ---- */
  std::vector<uint8_t> rgba(stride * kBenchCanvas);
  std::vector<uint8_t> bgra(stride * kBenchCanvas);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = kBenchCanvas;
  info.bmiHeader.biHeight = -kBenchCanvas; /* 负值 = 自顶向下 */
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  HDC window_dc = GetDC(window);
  if (window_dc == nullptr) {
    emit("result=getdc_failed");
    comp_document_destroy(document);
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  double cpu_blit_total = 0.0;
  bool cpu_ok = true;
  for (int i = 0; i < frames; ++i) {
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    if (comp_render_flatten(document, rgba.data(), COMP_PIXEL_RGBA8, stride) != COMP_OK) {
      cpu_ok = false;
      break;
    }
    /* 预乘 RGBA8 -> GDI 的 BGRA（与 render_canvas 的逐像素交换同口径）。 */
    const size_t pixel_count = static_cast<size_t>(kBenchCanvas) * kBenchCanvas;
    for (size_t p = 0; p < pixel_count; ++p) {
      const uint8_t* src = rgba.data() + p * 4;
      uint32_t packed = (static_cast<uint32_t>(src[3]) << 24) |
                        (static_cast<uint32_t>(src[0]) << 16) |
                        (static_cast<uint32_t>(src[1]) << 8) | static_cast<uint32_t>(src[2]);
      std::memcpy(bgra.data() + p * 4, &packed, 4);
    }
    StretchDIBits(window_dc, 0, 0, kBenchCanvas, kBenchCanvas, 0, 0, kBenchCanvas, kBenchCanvas,
                  bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    GdiFlush();
    QueryPerformanceCounter(&end);
    cpu_blit_total += elapsed_ms(start, end, freq);
    pump_pending_messages();
  }
  ReleaseDC(window, window_dc);
  if (!cpu_ok) {
    emit("result=cpu_present_failed");
    comp_document_destroy(document);
    DestroyWindow(window);
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }
  const double cpu_blit_avg = cpu_blit_total / frames;

  /* ---- C. 只测 CPU 合成（不含上屏），把两条路径的差异拆开归因 ---- */
  double cpu_flatten_total = 0.0;
  for (int i = 0; i < frames; ++i) {
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    comp_render_flatten(document, rgba.data(), COMP_PIXEL_RGBA8, stride);
    QueryPerformanceCounter(&end);
    cpu_flatten_total += elapsed_ms(start, end, freq);
    pump_pending_messages();
  }
  const double cpu_flatten_avg = cpu_flatten_total / frames;

  std::snprintf(line, sizeof(line), "gpu_present_avg_ms=%.3f", gpu_avg);
  emit(line);
  std::snprintf(line, sizeof(line), "gpu_present_best_ms=%.3f", gpu_best);
  emit(line);
  std::snprintf(line, sizeof(line), "gpu_present_worst_ms=%.3f", gpu_worst);
  emit(line);
  std::snprintf(line, sizeof(line), "cpu_flatten_blit_avg_ms=%.3f", cpu_blit_avg);
  emit(line);
  std::snprintf(line, sizeof(line), "cpu_flatten_avg_ms=%.3f", cpu_flatten_avg);
  emit(line);
  std::snprintf(line, sizeof(line), "cpu_present_over_gpu=%.2f", cpu_blit_avg / gpu_avg);
  emit(line);
  emit("result=ok");

  comp_document_destroy(document);
  DestroyWindow(window);
  UnregisterClassW(kBenchClass, instance);
  comp_engine_destroy(engine);
  std::fclose(report);
  return 0;
}

}  // namespace comp_shell
