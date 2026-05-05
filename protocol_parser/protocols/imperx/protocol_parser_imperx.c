/**
 * @file  protocol_parser_imperx.c
 * @brief Imperx 协议解析器实现
 *
 * Imperx 是一种简单的寄存器访问协议:
 * - 读:   [0x52][ADDR_H][ADDR_L]          → ACK[0x06] 或 NACK[0x15]
 * - 写:   [0x57][ADDR_H][ADDR_L][DATAx4]  → ACK[0x06] 或 NACK[0x15]
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#include "protocol_parser_imperx.h"

/* ======================== 内部常量 ================================== */

#define IMPERX_FRAME_HEAD_READ              0x52
#define IMPERX_FRAME_HEAD_WRITE             0x57
#define IMPERX_ACK_SUCCESS                  0x06
#define IMPERX_NACK_ERROR                   0x15
#define IMPERX_ERROR_CODE_INVALID_HEADER    0x01
#define IMPERX_ERROR_CODE_TIMEOUT           0x02
#define IMPERX_READ_FRAME_LEN               3   /**< 读命令: HEAD(1) + ADDR(2) */
#define IMPERX_WRITE_FRAME_LEN              7   /**< 写命令: HEAD(1) + ADDR(2) + DATA(4) */
#define IMPERX_CMD_INDEX                    1   /**< 地址字段起始索引 */
#define IMPERX_DATA_INDEX                   3   /**< 数据字段起始索引 */
#define IMPERX_DATA_LEN                     4   /**< 数据字段长度 */

/* ==================== 前置声明 ====================================== */

static int imperx_parse_data(protocol_parser_t* parser,
                            const uint8_t* data, uint32_t len);
static uint32_t imperx_encode(protocol_parser_t* parser, const void* data);
static void imperx_reset(protocol_parser_t* parser);
static void imperx_destroy(protocol_parser_t* parser);

/* ==================== 虚函数表 ====================================== */

static const struct protocol_parser_ops imperx_protocol_ops = {
    .parse_data = imperx_parse_data,
    .encode     = imperx_encode,
    .reset      = imperx_reset,
    .destroy    = imperx_destroy,
    .poll       = NULL,
};

/* ==================== 错误映射表 ================================== */

static const ErrorMapEntry imperx_error_map[] = {
    { IMPERX_ERR_NONE,           PARSER_ERR_NONE },
    { IMPERX_ERR_INCOMPLETE,     PARSER_ERR_INCOMPLETE },
    { IMPERX_ERR_INVALID_PARAM,  PARSER_ERR_INVALID_PARAM },
    { IMPERX_ERR_TIMEOUT,        PARSER_ERR_TIMEOUT },
    { IMPERX_ERR_INVALID_HEADER, PARSER_ERR_FRAME },
};

static const ErrorMapper imperx_error_mapper = {
    .table = imperx_error_map,
    .count = sizeof(imperx_error_map) / sizeof(imperx_error_map[0]),
};

/* ======================== 创建 ====================================== */

imperx_protocol_parser_t* imperx_protocol_create(void* rx_buffer,
                                                  uint32_t rx_buffer_size,
                                                  void* tx_buffer,
                                                  uint32_t tx_buffer_size) {
    parser_config_t config = get_default_config();
    config.max_frame_len = 10;
    config.timeout_ms = 100;

    protocol_parser_t* base = protocol_parser_create_common(
        sizeof(imperx_protocol_parser_t),
        &imperx_protocol_ops,
        &config,
        (uint8_t*)rx_buffer, rx_buffer_size,
        (uint8_t*)tx_buffer, tx_buffer_size);

    if (base) {
        base->error_mapper = imperx_error_mapper;
    }

    return (imperx_protocol_parser_t*)base;
}

protocol_parser_t* imperx_protocol_parser_create(const parser_config_t* config) {
    (void)config;
    return (protocol_parser_t*)imperx_protocol_create(NULL, 0, NULL, 0);
}

/* ==================== 错误应答编码 ================================== */

static uint32_t imperx_encode_error(protocol_parser_t* parser, imperx_error_t err) {
    if (!parser) {
        return 0;
    }
    imperx_protocol_parser_t* ip = (imperx_protocol_parser_t*)parser;

    ip->base.tx.data_len = 0;

    switch (err) {
    case IMPERX_ERR_INVALID_HEADER:
        ip->base.tx.buffer[ip->base.tx.data_len++] = IMPERX_NACK_ERROR;
        ip->base.tx.buffer[ip->base.tx.data_len++] = IMPERX_ERROR_CODE_INVALID_HEADER;
        return ip->base.tx.data_len;
    case IMPERX_ERR_TIMEOUT:
        ip->base.tx.buffer[ip->base.tx.data_len++] = IMPERX_NACK_ERROR;
        ip->base.tx.buffer[ip->base.tx.data_len++] = IMPERX_ERROR_CODE_TIMEOUT;
        return ip->base.tx.data_len;
    default:
        return 0;
    }
}

/* ==================== 正常应答编码 ================================== */

static uint32_t imperx_encode(protocol_parser_t* parser, const void* data) {
    (void)data;
    imperx_protocol_parser_t* ip = (imperx_protocol_parser_t*)parser;

    ip->base.tx.data_len = 0;
    ip->base.tx.buffer[ip->base.tx.data_len++] = IMPERX_ACK_SUCCESS;

    /* 仅 READ 命令且用户提供数据时才追加 */
    if (ip->pri.parsed_cmd == IMPERX_CMD_READ
        && ip->pri.parsed_data
        && ip->pri.parsed_len > 0) {
        for (uint32_t i = 0; i < ip->pri.parsed_len; i++) {
            ip->base.tx.buffer[ip->base.tx.data_len++] = ip->pri.parsed_data[i];
        }
    }

    return ip->base.tx.data_len;
}

/* ==================== 数据解析 ====================================== */

static int imperx_parse_data(protocol_parser_t* parser,
                            const uint8_t* data, uint32_t len) {
    imperx_protocol_parser_t* ip = (imperx_protocol_parser_t*)parser;

    if (!data || len == 0) {
        return IMPERX_ERR_INVALID_PARAM;
    }

    for (uint32_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        switch (ip->pri.state) {

        case IMPERX_STATE_WAIT_HEAD:
        {
            if (byte == IMPERX_FRAME_HEAD_READ) {
                ip->pri.state = IMPERX_STATE_WAIT_ID;
                ip->base.rx.buffer[ip->base.rx.data_len++] = IMPERX_FRAME_HEAD_READ;
                ip->pri.parsed_cmd = IMPERX_CMD_READ;
            }
            else if (byte == IMPERX_FRAME_HEAD_WRITE) {
                ip->pri.state = IMPERX_STATE_WAIT_ID;
                ip->base.rx.buffer[ip->base.rx.data_len++] = IMPERX_FRAME_HEAD_WRITE;
                ip->pri.parsed_cmd = IMPERX_CMD_WRITE;
            }
            else {
                ip->pri.header_errors++;
                imperx_encode_error(parser, IMPERX_ERR_INVALID_HEADER);
                protocol_parser_on_frame_error(parser, IMPERX_ERR_INVALID_HEADER);
                return IMPERX_ERR_INVALID_HEADER;
            }
            break;
        }
        case IMPERX_STATE_WAIT_ID:
        {
            ip->base.rx.buffer[ip->base.rx.data_len++] = byte;
            if (ip->base.rx.data_len == IMPERX_READ_FRAME_LEN) {
                if (ip->pri.parsed_cmd == IMPERX_CMD_READ) {
                    /* 完整的 READ 命令 — 通知框架 */
                    ip->pri.state = IMPERX_STATE_WAIT_HEAD;
                    ip->pri.parsed_id =
                        (ip->base.rx.buffer[IMPERX_CMD_INDEX] << 8)
                        | ip->base.rx.buffer[IMPERX_CMD_INDEX + 1];
                    ip->pri.parsed_data = NULL;
                    ip->pri.parsed_len = 0;

                    parser->parsed_result = &ip->pri;
                    protocol_parser_on_frame_ready(parser);
                    return IMPERX_ERR_NONE;
                }
                else {
                    /* 写命令 — 进入数据阶段 */
                    ip->pri.state = IMPERX_STATE_WAIT_DATA;
                }
            }
            break;
        }
        case IMPERX_STATE_WAIT_DATA:
        {
            ip->base.rx.buffer[ip->base.rx.data_len++] = byte;
            if (ip->base.rx.data_len == IMPERX_WRITE_FRAME_LEN) {
                /* 完整的 WRITE 命令 */
                ip->pri.parsed_id =
                    (ip->base.rx.buffer[IMPERX_CMD_INDEX] << 8)
                    | ip->base.rx.buffer[IMPERX_CMD_INDEX + 1];
                ip->pri.parsed_data = &ip->base.rx.buffer[IMPERX_DATA_INDEX];
                ip->pri.parsed_len = IMPERX_DATA_LEN;

                parser->parsed_result = &ip->pri;
                protocol_parser_on_frame_ready(parser);
                return IMPERX_ERR_NONE;
            }
            break;
        }
        default:
            break;
        }
    }

    return IMPERX_ERR_INCOMPLETE;
}

/* ==================== 重置与销毁 ==================================== */

static void imperx_reset(protocol_parser_t* parser) {
    imperx_protocol_parser_t* ip = (imperx_protocol_parser_t*)parser;
    ip->pri.state      = IMPERX_STATE_WAIT_HEAD;
    ip->pri.parsed_cmd = IMPERX_CMD_READ;
    ip->pri.parsed_id  = 0;
    ip->pri.parsed_data = NULL;
    ip->pri.parsed_len  = 0;
    default_reset(parser);
}

static void imperx_destroy(protocol_parser_t* parser) {
    (void)parser;
}
