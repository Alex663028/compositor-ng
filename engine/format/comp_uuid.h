// AI生成
/*
 * comp_uuid.h — 小写依赖 UUID 读写（.comp 互通所需）。
 *
 * 上游 manifest 的 documentID / layers[].id / parentID / maskSourceID / activeLayerID
 * 都是 Foundation 的 UUID，JSON 中表现为大写、带连字符的 36 字符串
 * （如 "E621E1F8-C36C-495A-93FC-0C247A3E6E5F"）。
 *
 * 引擎内部的图层标识是不透明的 64 位 comp_layer_id，因此保存时把内部 id 映射为
 * 确定性 UUID（同一文档 + 同一图层 id 恒定），加载时再映射回内部 id 顺序号。
 */
#ifndef COMPOSITOR_FORMAT_COMP_UUID_H
#define COMPOSITOR_FORMAT_COMP_UUID_H

#include <cstdint>
#include <string>

namespace compositor {

/* 16 字节 -> 大写连字符字符串（8-4-4-4-12）。 */
std::string uuid_to_string(const uint8_t bytes[16]);

/* 字符串 -> 16 字节；格式非法时返回 false。
 * 只接受规范形态：36 字符、连字符固定在 8/13/18/23 位、其余为十六进制（大小写不敏感）；
 * 省略连字符或长度不符均返回 false。 */
bool uuid_from_string(const std::string& text, uint8_t bytes[16]);

/* 是否为规范形态的 UUID 文本（等价于 uuid_from_string 成功）。 */
bool uuid_is_wellformed(const std::string& text);

/* 随机 v4 UUID（新文档首次保存时生成 documentID）。 */
std::string uuid_v4_new();

/*
 * 由文档 UUID + 图层内部 id 派生稳定 UUID。
 * 同一 (document_uuid, key) 组合恒定产出同一字符串：文档再次保存不会让图层
 * 文件名漂移，上游侧的外部引用也因此保持稳定。
 */
std::string uuid_derive(const std::string& document_uuid, uint64_t key);

}  // namespace compositor

#endif /* COMPOSITOR_FORMAT_COMP_UUID_H */
