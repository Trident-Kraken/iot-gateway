#ifndef IOT_GATEWAY_CONFIG_PARSER_H
#define IOT_GATEWAY_CONFIG_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 加载 INI 格式的配置文件。
 *
 * 解析 key=value 行（忽略节标题、注释和空行）。
 * 键和值前后的空白字符会被修剪。
 *
 * @param path 配置文件路径。
 *
 * @return 成功返回 0，失败返回 -1。
 */
int config_load(const char *path);

/**
 * @brief 查找字符串类型的配置值。
 *
 * @param key           待查找的键。
 * @param default_value 未找到键时返回的默认值。
 *
 * @return 指向存储值的指针，未找到时返回 @p default_value。
 */
const char *config_get_string(const char *key, const char *default_value);

/**
 * @brief 查找整数类型的配置值。
 *
 * @param key           待查找的键。
 * @param default_value 未找到键或值不是有效整数时返回的默认值。
 *
 * @return 整数值，或 @p default_value。
 */
int config_get_int(const char *key, int default_value);

/**
 * @brief 释放在 config_load() 中分配的所有内存。
 */
void config_free(void);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_CONFIG_PARSER_H */
