// AI生成
/*
 * test_import_matrix.cpp — s7 格式导入策略矩阵的回归护栏。
 *
 * 锁定三件事：
 *   1. 扩展名 → comp_image_format 的映射：HEIC/HEIF、WebP 有登记；SVG/PSB 未登记；
 *   2. 未落地格式的导入必须如实 UNSUPPORTED，且「格式支持」先于「文件是否存在」判定
 *      （不该让用户以为只是路径写错了）；
 *   3. 失败路径不产生文档、不改动目标文档（out_document 哨兵不变、图层数不变）。
 *
 * 矩阵文档见 docs/format-import-matrix.md；本文件是它的可执行证据。
 */
#include "test_framework.h"

#include <cstring>
#include <string>

#include <compositor/comp_core.h>
#include <compositor/comp_engine.h>
#include <compositor/comp_io.h>
#include <compositor/comp_layer.h>
#include <compositor/comp_undo.h>

namespace {

comp_engine make_engine() {
  comp_engine engine = nullptr;
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.backend = COMP_BACKEND_CPU;
  if (comp_engine_create(&desc, &engine) != COMP_OK) return nullptr;
  return engine;
}

/* 登记表生成的文案特征：「<NAME> is not available in this build (...)」。 */
bool mentions_registry_reason(const char* text) {
  return text != nullptr && std::strstr(text, "not available in this build") != nullptr;
}

/* 未登记格式的文案特征：没有任何 codec 条目认领该扩展名。 */
bool mentions_unknown_format(const char* text) {
  return text != nullptr && std::strstr(text, "unknown image format") != nullptr;
}

}  // namespace

COMP_TEST(import_matrix_extension_mapping_is_explicit) {
  /* 有登记：HEIC 与 HEIF 同一枚举（同一容器）。 */
  COMP_CHECK(comp_io_format_from_path("photo.heic") == COMP_FORMAT_HEIF);
  COMP_CHECK(comp_io_format_from_path("photo.heif") == COMP_FORMAT_HEIF);
  /* 大小写不敏感：扩展名先转小写再匹配。 */
  COMP_CHECK(comp_io_format_from_path("photo.HEIC") == COMP_FORMAT_HEIF);
  COMP_CHECK(comp_io_format_from_path("photo.WebP") == COMP_FORMAT_WEBP);
  /* PSD 有登记，但 PSB（Photoshop 大文档）不在同一枚举里，不得冒充 PSD。 */
  COMP_CHECK(comp_io_format_from_path("poster.psd") == COMP_FORMAT_PSD);
  COMP_CHECK(comp_io_format_from_path("poster.psb") == COMP_FORMAT_UNKNOWN);
  /* SVG 是矢量文档，不在位图导入通道里。 */
  COMP_CHECK(comp_io_format_from_path("art.svg") == COMP_FORMAT_UNKNOWN);
  /* 路径解释：目录里的点不算扩展名；空路径/无扩展名一律 UNKNOWN。 */
  COMP_CHECK(comp_io_format_from_path("dir.with.dot/file") == COMP_FORMAT_UNKNOWN);
  COMP_CHECK(comp_io_format_from_path("noextension") == COMP_FORMAT_UNKNOWN);
  COMP_CHECK(comp_io_format_from_path(nullptr) == COMP_FORMAT_UNKNOWN);
}

/*
 * 已登记但本构建未链接解码器的格式：必须在读文件之前就判掉，并给出登记表原因。
 * 这里故意用不存在的文件：如果实现先查文件，会得到 COMP_ERR_IO 而不是 UNSUPPORTED。
 */
COMP_TEST(import_matrix_unavailable_formats_reject_before_io) {
  comp_engine engine = make_engine();
  COMP_CHECK(engine != nullptr);
  if (engine == nullptr) return;

  const char* paths[] = {"missing_path/photo.heic", "missing_path/photo.heif",
                         "missing_path/picture.webp"};
  for (const char* path : paths) {
    comp_document document = nullptr;
    const comp_result result = comp_io_import(engine, path, &document);
    std::string label = std::string("format support is checked before file IO: ") + path;
    COMP_CHECK_MSG(result == COMP_ERR_UNSUPPORTED, label.c_str());
    COMP_CHECK_MSG(document == nullptr, "failed import must not return a document handle");
    const char* error = comp_engine_last_error(engine);
    std::string message = std::string("registry reason expected for ") + path + ": " +
                          (error != nullptr ? error : "(null)");
    COMP_CHECK_MSG(mentions_registry_reason(error), message.c_str());
  }

  comp_engine_destroy(engine);
}

/* 完全未登记的扩展名（SVG/PSB）：UNSUPPORTED + 「没有 codec 认领」的原因。 */
COMP_TEST(import_matrix_unknown_formats_are_not_guessed) {
  comp_engine engine = make_engine();
  COMP_CHECK(engine != nullptr);
  if (engine == nullptr) return;

  const char* paths[] = {"art.svg", "poster.psb"};
  for (const char* path : paths) {
    comp_document document = nullptr;
    const comp_result result = comp_io_import(engine, path, &document);
    std::string label = std::string("unknown format is refused, not guessed: ") + path;
    COMP_CHECK_MSG(result == COMP_ERR_UNSUPPORTED, label.c_str());
    COMP_CHECK_MSG(document == nullptr, "failed import must not return a document handle");
    const char* error = comp_engine_last_error(engine);
    std::string message = std::string("unknown-format reason expected for ") + path + ": " +
                          (error != nullptr ? error : "(null)");
    COMP_CHECK_MSG(mentions_unknown_format(error), message.c_str());
  }

  comp_engine_destroy(engine);
}

/*
 * 追加导入失败时目标文档必须原样：图层数不变、没有空图层残影、不进入撤销栈。
 * 这是「失败不留半成品」的回归点（P1-4 的事务化语义）。
 */
COMP_TEST(import_matrix_layer_import_failure_leaves_document_untouched) {
  comp_engine engine = make_engine();
  COMP_CHECK(engine != nullptr);
  if (engine == nullptr) return;

  comp_document_desc desc{};
  desc.width = 8;
  desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &desc, &document) == COMP_OK);
  COMP_CHECK(document != nullptr);
  if (document == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  size_t count_before = 0;
  COMP_CHECK(comp_layer_count(document, &count_before) == COMP_OK);
  size_t undo_before = 0;
  size_t redo_before = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_before, &redo_before) == COMP_OK);

  comp_layer_id id = 0;
  COMP_CHECK(comp_io_import_layer(document, "poster.psb", &id) == COMP_ERR_UNSUPPORTED);
  COMP_CHECK_MSG(id == 0, "failed import must not hand out a layer id");

  size_t count_after = 0;
  COMP_CHECK(comp_layer_count(document, &count_after) == COMP_OK);
  size_t undo_after = 0;
  size_t redo_after = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_after, &redo_after) == COMP_OK);
  COMP_CHECK_MSG(count_after == count_before, "failed import must not add a layer");
  COMP_CHECK_MSG(undo_after == undo_before && redo_after == redo_before,
                 "failed import must not record history");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
