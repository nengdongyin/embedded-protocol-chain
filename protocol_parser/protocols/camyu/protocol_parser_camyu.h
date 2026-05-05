/**
 * @file  protocol_parser_camyu.h
 * @brief Camyu 协议解析器接口
 *
 * Camyu 帧格式(类 PPP 协议, 带字节转义):
 *   [0x7E][FTF][LEN][CMD...][DATA...][BCC]
 *    1B    1B   1B    nB      nB      1B
 *
 * 字节转义: 0x7E → 0x7D 0x5E, 0x7D → 0x7D 0x5D.
 * FTF(帧类型标志)位域:
 *   [opcode:3][version:2][addrlen:2][bcc_en:1]
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#pragma once
#include "../../core/protocol_parser.h"
#include "../../core/protocol_parser_internal.h"

/* ======================== FTF 位域 ================================== */

/** @brief Camyu 帧类型标志位域 */
typedef struct {
    uint8_t bcccode    : 1;  /**< BCC 校验使能 */
    uint8_t addrlen    : 2;  /**< 地址长度编码 */
    uint8_t versioncode: 2;  /**< 协议版本编码 */
    uint8_t opcode     : 3;  /**< 操作码 */
} ftf_t;

/* ======================== 解析器状态 ================================ */

/** @brief Camyu 解析器状态枚举 */
typedef enum {
    CAMYU_PARSER_STATE_WAIT_HEAD,    /**< 等待帧头(0x7E) */
    CAMYU_PARSER_STATE_WAIT_TFT,     /**< 等待 FTF 字节 */
    CAMYU_PARSER_STATE_WAIT_DATALEN, /**< 等待数据长度字节 */
    CAMYU_PARSER_STATE_WAIT_CMD,     /**< 等待命令/地址字节 */
    CAMYU_PARSER_STATE_WAIT_DATA,    /**< 等待负载数据 */
    CAMYU_PARSER_STATE_WAIT_BCC,     /**< 等待 BCC 校验字节 */
} camyu_parser_state_t;

/* ======================== OpCode 类型 =============================== */

/** @brief Camyu OpCode 类型枚举 */
typedef enum {
    PCTOCAMERA_WRITE_RESPONSE   = 0, /**< PC→相机 带响应写 */
    PCTOCAMERA_WRITE_NORESPONSE = 1, /**< PC→相机 无响应写 */
    PCTOCAMERA_READ             = 2, /**< PC→相机 读 */
    CAMERATOPC_RESPONSE         = 3, /**< 相机→PC 响应 */
    CAMERATOPC_REPORT           = 4, /**< 相机→PC 主动上报 */
} Opcode_type_t;

/* ======================== 协议特定错误码 ============================ */

/** @brief Camyu 协议完整错误码(自包含, 不依赖基类枚举值) */
typedef enum {
    CAMYU_ERR_NONE = 0,          /**< 无错误 */
    CAMYU_ERR_INCOMPLETE,        /**< 数据不完整, 需要更多数据 */
    CAMYU_ERR_INVALID_PARAM,     /**< 无效参数 */
    CAMYU_ERR_INVALID_HEADER,    /**< 无效帧头 */
    CAMYU_ERR_BCC_CHECKSUM,      /**< BCC 校验失败 */
    CAMYU_ERR_FRAME_TOO_LONG,    /**< 帧超长 */
} camyu_error_t;

/* ======================== 协议版本 ================================== */

/** @brief Camyu 协议版本枚举 */
typedef enum {
    PROTOCOL_VERSION_V1 = 0, /**< 协议版本 1 */
    PROTOCOL_VERSION_V2 = 1, /**< 协议版本 2 */
    PROTOCOL_VERSION_V3 = 2, /**< 协议版本 3 */
    PROTOCOL_VERSION_V4 = 3, /**< 协议版本 4 */
} protocol_version_t;

/* ======================== 帧信息 ==================================== */

/** @brief Camyu 帧信息(解析中间状态) */
typedef struct {
    ftf_t              tft_value;               /**< 原始 FTF 值 */
    Opcode_type_t      opcode;                  /**< 解析出的 OpCode */
    bool               verify_en;               /**< BCC 校验是否使能 */
    bool               resp_en;                 /**< 是否需要应答 */
    uint32_t           cmd_len_byte;            /**< 命令/地址字段长度(字节) */
    protocol_version_t protocol_version;        /**< 协议版本 */
    uint32_t           current_frame_data_len;  /**< 预期负载数据长度 */
    uint32_t           current_frame_total_len; /**< 帧总长度(不含转义) */
} frame_info_t;

/* ======================== 协议特定统计 ============================== */

/** @brief Camyu 协议特定统计 */
typedef struct {
    uint32_t header_errors;         /**< 无效帧头错误次数 */
    uint32_t checksum_errors;       /**< BCC 校验错误次数 */
    uint32_t frame_too_long_count;  /**< 帧超长错误次数 */
} camyu_stats_t;

/* ======================== 私有数据 ================================== */

/** @brief Camyu 协议私有数据 */
typedef struct {
    frame_info_t         current_frame_info;    /**< 当前帧信息 */
    camyu_parser_state_t camyu_parser_state;    /**< 当前解析器状态 */
    bool                 transchar_active;      /**< 字节转义标志 */
    uint32_t             current_rev_cmd_len;   /**< 已接收的命令字节数 */
    uint32_t             current_rev_data_len;  /**< 已接收的数据字节数 */
    uint64_t             parsed_id;             /**< 解析出的地址/ID */
    uint8_t*             parsed_data;           /**< 指向解析出的负载(在 rx_buffer 内部) */
    uint32_t             parsed_data_len;       /**< 解析出的负载长度 */
    camyu_stats_t        stats;                 /**< 协议特定统计 */
} camyu_private_t;

/* ======================== 解析器结构体 ============================== */

/** @brief Camyu 协议解析器结构体(继承 protocol_parser_t) */
typedef struct {
    protocol_parser_t base;  /**< 基类(必须为第一个成员) */
    camyu_private_t   pri;   /**< 协议特定私有数据 */
} camyu_protocol_parser_t;

/* ======================== 公共 API ================================== */

/**
 * @brief  创建 Camyu 解析器(可选静态缓冲区)
 *
 * @param rx_buffer       外部接收缓冲区(NULL = 动态分配)
 * @param rx_buffer_size  接收缓冲区大小
 * @param tx_buffer       外部发送缓冲区(NULL = 动态分配)
 * @param tx_buffer_size  发送缓冲区大小
 * @return  Camyu 解析器实例, 失败返回 NULL
 */
camyu_protocol_parser_t* camyu_protocol_create(void* rx_buffer, uint32_t rx_buffer_size,
                                                void* tx_buffer, uint32_t tx_buffer_size);
