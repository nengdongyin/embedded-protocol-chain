/**
 * @file  protocol_parser_imperx.h
 * @brief Imperx 协议解析器接口
 *
 * Imperx 帧格式:
 *   读命令:   [0x52][ADDR_H][ADDR_L]           (3 字节, 无数据)
 *   写命令:   [0x57][ADDR_H][ADDR_L][DATA x 4]  (7 字节, 4 字节数据)
 *
 * 响应格式:
 *   成功:     [0x06] 或 [0x06][DATA...]
 *   错误:     [0x15][ERR_CODE]
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#pragma once
#include "../../core/protocol_parser.h"
#include "../../core/protocol_parser_internal.h"

/* ======================== 命令类型 ================================== */

/** @brief Imperx 命令类型枚举 */
typedef enum {
    IMPERX_CMD_READ  = 0,  /**< 读命令 */
    IMPERX_CMD_WRITE = 1,  /**< 写命令 */
} imperx_cmd_type_t;

/* ======================== 协议特定错误码 ============================ */

/** @brief Imperx 协议完整错误码(自包含, 不依赖基类枚举值) */
typedef enum {
    IMPERX_ERR_NONE = 0,           /**< 无错误 */
    IMPERX_ERR_INCOMPLETE,         /**< 数据不完整, 需要更多数据 */
    IMPERX_ERR_INVALID_PARAM,      /**< 无效参数 */
    IMPERX_ERR_TIMEOUT,            /**< 超时 */
    IMPERX_ERR_INVALID_HEADER,     /**< 无效帧头 */
} imperx_error_t;

/* ======================== 解析器状态 ================================ */

/** @brief Imperx 解析器状态枚举 */
typedef enum {
    IMPERX_STATE_WAIT_HEAD, /**< 等待帧头 */
    IMPERX_STATE_WAIT_ID,   /**< 等待地址 */
    IMPERX_STATE_WAIT_DATA, /**< 等待数据(写命令) */
} imperx_state_t;

/* ======================== 私有数据 ================================== */

/**
 * @struct imperx_private_t
 * @brief  Imperx 协议私有数据
 */
typedef struct {
    imperx_state_t      state;       /**< 当前解析器状态 */
    imperx_cmd_type_t   parsed_cmd;  /**< 解析出的命令类型(READ/WRITE) */
    uint64_t            parsed_id;   /**< 解析出的地址/ID */
    uint8_t*            parsed_data; /**< 指向解析出的数据(在 rx_buffer 内部) */
    uint32_t            parsed_len;  /**< 解析出的数据长度 */
    uint32_t            header_errors; /**< 无效帧头错误次数 */
} imperx_private_t;

/* ======================== 解析器结构体 ============================== */

/** @brief Imperx 协议解析器结构体(继承 protocol_parser_t) */
typedef struct {
    protocol_parser_t base;  /**< 基类(必须为第一个成员) */
    imperx_private_t  pri;   /**< 协议特定私有数据 */
} imperx_protocol_parser_t;

/* ======================== 公共 API ================================== */

/**
 * @brief  创建 Imperx 解析器(可选静态缓冲区)
 *
 * @param rx_buffer       外部接收缓冲区(NULL = 动态分配)
 * @param rx_buffer_size  接收缓冲区大小
 * @param tx_buffer       外部发送缓冲区(NULL = 动态分配)
 * @param tx_buffer_size  发送缓冲区大小
 * @return  Imperx 解析器实例, 失败返回 NULL
 */
imperx_protocol_parser_t* imperx_protocol_create(void* rx_buffer, uint32_t rx_buffer_size,
                                                  void* tx_buffer, uint32_t tx_buffer_size);

/**
 * @brief  创建 Imperx 解析器(动态缓冲区, 便捷封装)
 *
 * @param config  配置(仅使用 max_frame_len 和 timeout_ms)
 * @return  基类指针, 失败返回 NULL
 */
protocol_parser_t* imperx_protocol_parser_create(const parser_config_t* config);
