/**
 * @file  protocol_chain.h
 * @brief 协议链(责任链模式)接口
 *
 * 实现责任链模式, 支持多协议自动匹配与切换.
 * 当数据到达时, 首先成功匹配帧的解析器被"锁定",
 * 后续数据直接转发给该解析器, 提高处理效率.
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#ifndef PROTOCOL_CHAIN_H
#define PROTOCOL_CHAIN_H

#include "protocol_parser.h"

/**
 * @struct protocol_chain
 * @brief  协议链管理器(不透明结构体)
 */
typedef struct protocol_chain protocol_chain;

/* ======================== 公共 API ================================== */

/**
 * @brief  创建协议链实例
 *
 * @param max_parsers  最大解析器数量
 * @return protocol_chain*  协议链实例, 失败返回 NULL
 */
protocol_chain* protocol_chain_create(uint32_t max_parsers);

/**
 * @brief  销毁协议链实例
 *
 * 释放内部资源. 解析器本身不会被销毁, 由调用者负责释放.
 *
 * @param chain  协议链实例
 */
void protocol_chain_destroy(protocol_chain* chain);

/**
 * @brief  添加解析器到协议链
 *
 * 解析器按添加顺序依次尝试匹配.
 *
 * @param chain   协议链实例
 * @param parser  待添加的解析器
 * @retval true   添加成功
 * @retval false  失败(链已满或参数无效)
 */
bool protocol_chain_add_parser(protocol_chain* chain, protocol_parser_t* parser);

/**
 * @brief  从协议链移除指定解析器
 *
 * 后续解析器向前移动填补空缺. 若被移除的解析器是当前锁定解析器,
 * 则清除锁定.
 *
 * @param chain   协议链实例
 * @param parser  待移除的解析器
 * @retval true   移除成功
 * @retval false  未找到解析器
 */
bool protocol_chain_remove_parser(protocol_chain* chain, protocol_parser_t* parser);

/**
 * @brief  向协议链送入流式数据
 *
 * 如果已锁定解析器, 数据直接转发给该解析器.
 * 否则按顺序尝试每个解析器, 首个返回成功(PARSER_ERR_NONE)的
 * 解析器被锁定用于后续数据.
 *
 * @param chain  协议链实例
 * @param data   流式数据
 * @param len    数据长度
 * @return       解析器错误码
 * @retval PARSER_ERR_NONE        解析成功
 * @retval PARSER_ERR_INCOMPLETE  需要更多数据
 * @retval PARSER_ERR_UNKNOWN     无解析器能匹配
 */
parser_error_t protocol_chain_feed(protocol_chain* chain, const uint8_t* data, uint32_t len);

/**
 * @brief  向协议链送入完整帧
 *
 * 与 @ref protocol_chain_feed() 不同, 本函数将输入视为完整帧.
 * 若锁定解析器返回任何错误, 则解锁.
 *
 * @param chain  协议链实例
 * @param data   完整帧数据
 * @param len    帧长度
 * @return       解析器错误码
 * @retval PARSER_ERR_NONE     解析成功
 * @retval PARSER_ERR_UNKNOWN  无解析器能匹配
 */
parser_error_t protocol_chain_feed_frame(protocol_chain* chain, const uint8_t* data, uint32_t len);

/**
 * @brief  获取当前锁定的解析器
 *
 * @param chain  协议链实例
 * @return       锁定解析器, 无锁定返回 NULL
 */
protocol_parser_t* protocol_chain_get_locked_parser(protocol_chain* chain);

/**
 * @brief  手动设置(或清除)锁定解析器
 *
 * 可用于强制切换协议.
 *
 * @param chain   协议链实例
 * @param parser  待锁定的解析器(NULL = 解锁)
 * @retval true   设置成功(parser 必须在协议链中, 或为 NULL)
 * @retval false  解析器不在协议链中
 */
bool protocol_chain_set_locked_parser(protocol_chain* chain, protocol_parser_t* parser);

/**
 * @brief  轮询检查协议链中各解析器的超时状态
 *
 * 若有锁定解析器, 仅检查该解析器.
 * 否则检查所有注册的解析器.
 * 在主循环中定期调用.
 *
 * @param chain  协议链实例
 * @retval true  至少一个解析器超时(错误处理已在内部触发)
 * @retval false 无超时
 */
bool protocol_chain_check_timeout_poll(protocol_chain* chain);

#endif /* PROTOCOL_CHAIN_H */
