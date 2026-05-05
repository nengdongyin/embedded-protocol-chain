/**
 * @file  protocol_parser_internal.h
 * @brief 协议解析器子类内部接口
 *
 * 本文件中声明的函数仅供协议特定子类实现使用, 不属于公共 API.
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#ifndef PROTOCOL_PARSER_INTERNAL_H
#define PROTOCOL_PARSER_INTERNAL_H

#include "protocol_parser.h"

/* ======================== 内部辅助函数 ============================== */

/**
 * @brief  将基类状态重置为默认值
 *
 * 清空 rx/tx 长度, 停用超时计时, 将 parsed_result 置 NULL.
 * 子类应在其自己的 reset 实现中调用此函数.
 *
 * @param parser  解析器实例
 */
void default_reset(protocol_parser_t* parser);

/**
 * @brief  更新解析器最后活动时间戳
 *
 * 用于超时检测机制.
 * 由 @ref protocol_parser_parse_data() 自动调用.
 *
 * @param parser  解析器实例
 */
void protocol_parser_update_time(protocol_parser_t* parser);

/**
 * @brief  通知框架发生帧错误
 *
 * 更新通用统计, 触发子类 reset, 若 tx_buffer 中有数据则调用 tx_ready.
 * 子类在检测到协议违规时调用此函数.
 *
 * @param parser  解析器实例
 * @param err     错误码(子类特定或通用)
 */
void protocol_parser_on_frame_error(protocol_parser_t* parser, int err);

/**
 * @brief  通知框架帧解析成功完成
 *
 * 依次调用用户 frame_ready 回调, 子类 encode 函数生成响应,
 * 然后调用 tx_ready 发送, 最后调用子类 reset.
 *
 * @param parser  解析器实例
 */
void protocol_parser_on_frame_ready(protocol_parser_t* parser);

/* ======================== 初始化函数 ================================ */

/**
 * @brief  静态初始化(使用外部提供的缓冲区)
 *
 * @param parser         待初始化的解析器实例
 * @param ops            虚函数表
 * @param config         配置参数
 * @param rx_buffer      外部接收缓冲区
 * @param rx_buffer_size 接收缓冲区大小
 * @param tx_buffer      外部发送缓冲区
 * @param tx_buffer_size 发送缓冲区大小
 * @retval true  初始化成功
 * @retval false 初始化失败
 */
bool protocol_parser_static_init(
    protocol_parser_t* parser,
    const struct protocol_parser_ops* ops,
    const parser_config_t* config,
    uint8_t* rx_buffer,
    uint32_t rx_buffer_size,
    uint8_t* tx_buffer,
    uint32_t tx_buffer_size
);

/**
 * @brief  动态初始化(内部动态分配缓冲区)
 *
 * 从堆中分配 rx 和 tx 缓冲区.
 *
 * @param parser  待初始化的解析器实例
 * @param ops     虚函数表
 * @param config  配置参数(NULL = 使用默认值)
 * @retval true  初始化成功
 * @retval false 初始化失败
 */
bool protocol_parser_dynamic_init(
    protocol_parser_t* parser,
    const struct protocol_parser_ops* ops,
    const parser_config_t* config
);

/**
 * @brief  通用解析器创建辅助函数
 *
 * 分配内存并根据是否提供外部缓冲区决定使用静态或动态初始化.
 *
 * 子类 create 函数使用此函数以避免代码重复.
 *
 * @param instance_size   子类解析器结构体总大小(字节)
 * @param ops             虚函数表
 * @param config          配置参数
 * @param rx_buffer       外部接收缓冲区(NULL = 动态分配)
 * @param rx_buffer_size  接收缓冲区大小
 * @param tx_buffer       外部发送缓冲区(NULL = 动态分配)
 * @param tx_buffer_size  发送缓冲区大小
 * @return  解析器基类部分指针, 失败返回 NULL
 */
protocol_parser_t* protocol_parser_create_common(
    size_t instance_size,
    const struct protocol_parser_ops* ops,
    const parser_config_t* config,
    uint8_t* rx_buffer,
    uint32_t rx_buffer_size,
    uint8_t* tx_buffer,
    uint32_t tx_buffer_size
);

/**
 * @brief  通用解析器创建辅助函数(独立缓冲区约束)
 *
 * 与 @ref protocol_parser_create_common 功能相同,
 * 但接受独立的 rx_min / tx_min 最小缓冲区约束,
 * 代替原来强制两者均 ≥ config.max_frame_len 的限制.
 *
 * 适用于收发缓冲区大小不对称的协议(如 Ymodem:
 * 接收需要大 rx + 小 tx; 发送需要小 rx + 大 tx).
 *
 * @param instance_size   子类解析器结构体总大小(字节)
 * @param ops             虚函数表
 * @param config          配置参数
 * @param rx_buffer       外部接收缓冲区(NULL = 动态分配)
 * @param rx_buffer_size  接收缓冲区大小
 * @param rx_min          接收缓冲区最小要求(字节)
 * @param tx_buffer       外部发送缓冲区(NULL = 动态分配)
 * @param tx_buffer_size  发送缓冲区大小
 * @param tx_min          发送缓冲区最小要求(字节)
 * @return  解析器基类部分指针, 失败返回 NULL
 */
protocol_parser_t* protocol_parser_create_common_ex(
    size_t instance_size,
    const struct protocol_parser_ops* ops,
    const parser_config_t* config,
    uint8_t* rx_buffer,
    uint32_t rx_buffer_size,
    uint32_t rx_min,
    uint8_t* tx_buffer,
    uint32_t tx_buffer_size,
    uint32_t tx_min
);

#endif /* PROTOCOL_PARSER_INTERNAL_H */
