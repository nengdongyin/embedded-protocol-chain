/**
 * @file  protocol_parser_camyu.c
 * @brief Camyu 协议解析器实现
 *
 * 类 PPP 协议，带字节转义。帧格式:
 *   [0x7E][FTF][LEN][CMD...][DATA...][BCC]
 * 字节转义: 0x7E→0x7D 0x5E, 0x7D→0x7D 0x5D。
 * BCC: CRC-8/MAXIM，覆盖除帧头外的所有字节。
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#include "protocol_parser_camyu.h"

/* ======================== 内部常量 ================================== */

/** @name 帧字段标记 */
/**@{*/
#define HEADCHAR            (0x7E)   /**< 帧头字符 */
#define TRANSCHAR           (0x7D)   /**< 转义字符 */
#define TRANSHEAD           (0x5E)   /**< 转义后的帧头(0x7E ^ 0x20) */
#define TRANSTRANS          (0x5D)   /**< 转义后的转义符(0x7D ^ 0x20) */
#define CAMYU_ESCAPE_XOR_MASK   (0x20)
/**@}*/

/** @name 字段长度 */
/**@{*/
#define HEADCHAR_LEN          (1)
#define TFT_LEN               (1)
#define DATALEN_LEN           (1)
#define BCC_LEN               (1)
#define DATALEN_MAX_LEN       (256)
/**@}*/

/** @name 字段索引 */
/**@{*/
#define HEADCHAR_INDEX        (0)
#define TFT_INDEX             (1)
#define CMD_INDEX             (3)
/**@}*/

/* ==================== 前置声明 ====================================== */

static int camyu_parse_data(protocol_parser_t* parser,
                           const uint8_t* data, uint32_t len);
static uint32_t camyu_encode(protocol_parser_t* parser, const void* data);
static void camyu_reset(protocol_parser_t* parser);
static void camyu_destroy(protocol_parser_t* parser);

/* ==================== 虚函数表 ====================================== */

static const struct protocol_parser_ops camyu_protocol_ops = {
    .parse_data = camyu_parse_data,
    .encode     = camyu_encode,
    .reset      = camyu_reset,
    .destroy    = camyu_destroy,
    .poll       = NULL,
};

/* ==================== 错误映射表 ================================== */

static const ErrorMapEntry camyu_error_map[] = {
    { CAMYU_ERR_NONE,           PARSER_ERR_NONE },
    { CAMYU_ERR_INCOMPLETE,     PARSER_ERR_INCOMPLETE },
    { CAMYU_ERR_INVALID_PARAM,  PARSER_ERR_INVALID_PARAM },
    { CAMYU_ERR_INVALID_HEADER, PARSER_ERR_FRAME },
    { CAMYU_ERR_BCC_CHECKSUM,   PARSER_ERR_FRAME },
    { CAMYU_ERR_FRAME_TOO_LONG, PARSER_ERR_FRAME },
};

static const ErrorMapper camyu_error_mapper = {
    .table = camyu_error_map,
    .count = sizeof(camyu_error_map) / sizeof(camyu_error_map[0]),
};

/* ======================== 创建 ====================================== */

camyu_protocol_parser_t* camyu_protocol_create(void* rx_buffer,
                                                uint32_t rx_buffer_size,
                                                void* tx_buffer,
                                                uint32_t tx_buffer_size) {
    parser_config_t config = get_default_config();
    config.max_frame_len = DATALEN_MAX_LEN + 64;
    config.timeout_ms = 0;   /* Camyu 不使用超时 */

    protocol_parser_t* base = protocol_parser_create_common(
        sizeof(camyu_protocol_parser_t),
        &camyu_protocol_ops,
        &config,
        (uint8_t*)rx_buffer, rx_buffer_size,
        (uint8_t*)tx_buffer, tx_buffer_size);

    if (base) {
        base->error_mapper = camyu_error_mapper;
    }

    return (camyu_protocol_parser_t*)base;
}

/* ==================== 帧信息更新 ==================================== */

/**
 * @brief  根据 FTF 字节更新帧信息
 *
 * 提取 opcode、地址长度、协议版本、校验使能，
 * 计算命令字段长度和总帧长度。
 *
 * @param parser  解析器实例
 */
static void frame_info_update(protocol_parser_t* parser) {
    camyu_protocol_parser_t* cp = (camyu_protocol_parser_t*)parser;
    ftf_t* ftf = &cp->pri.current_frame_info.tft_value;

    cp->pri.current_frame_info.opcode      = (Opcode_type_t)ftf->opcode;
    cp->pri.current_frame_info.verify_en   = (bool)ftf->bcccode;
    cp->pri.current_frame_info.resp_en =
        (cp->pri.current_frame_info.opcode != PCTOCAMERA_WRITE_NORESPONSE);
    cp->pri.current_frame_info.cmd_len_byte = (ftf->addrlen + 1) * 2;
    cp->pri.current_frame_info.protocol_version = (protocol_version_t)ftf->versioncode;
    cp->pri.current_frame_info.current_frame_total_len =
        HEADCHAR_LEN + TFT_LEN + DATALEN_LEN
        + cp->pri.current_frame_info.cmd_len_byte
        + cp->pri.current_frame_info.current_frame_data_len
        + (uint32_t)cp->pri.current_frame_info.verify_en;
}

/* ==================== BCC 计算 ====================================== */

/** @brief CRC-8/MAXIM 查表 */
static const uint8_t CRC8Table[] = {
    0, 94, 188, 226, 97, 63, 221, 131, 194, 156, 126, 32, 163, 253, 31, 65,
    157, 195, 33, 127, 252, 162, 64, 30, 95, 1, 227, 189, 62, 96, 130, 220,
    35, 125, 159, 193, 66, 28, 254, 160, 225, 191, 93, 3, 128, 222, 60, 98,
    190, 224, 2, 92, 223, 129, 99, 61, 124, 34, 192, 158, 29, 67, 161, 255,
    70, 24, 250, 164, 39, 121, 155, 197, 132, 218, 56, 102, 229, 187, 89, 7,
    219, 133, 103, 57, 186, 228, 6, 88, 25, 71, 165, 251, 120, 38, 196, 154,
    101, 59, 217, 135, 4, 90, 184, 230, 167, 249, 27, 69, 198, 152, 122, 36,
    248, 166, 68, 26, 153, 199, 37, 123, 58, 100, 134, 216, 91, 5, 231, 185,
    140, 210, 48, 110, 237, 179, 81, 15, 78, 16, 242, 172, 47, 113, 147, 205,
    17, 79, 173, 243, 112, 46, 204, 146, 211, 141, 111, 49, 178, 236, 14, 80,
    175, 241, 19, 77, 206, 144, 114, 44, 109, 51, 209, 143, 12, 82, 176, 238,
    50, 108, 142, 208, 83, 13, 239, 177, 240, 174, 76, 18, 145, 207, 45, 115,
    202, 148, 118, 40, 171, 245, 23, 73, 8, 86, 180, 234, 105, 55, 213, 139,
    87, 9, 235, 181, 54, 104, 138, 212, 149, 203, 41, 119, 244, 170, 72, 22,
    233, 183, 85, 11, 136, 214, 52, 106, 43, 117, 151, 201, 74, 20, 246, 168,
    116, 42, 200, 150, 21, 75, 169, 247, 182, 232, 10, 84, 215, 137, 107, 53
};

/**
 * @brief  使用查表法计算 CRC-8/MAXIM
 *
 * 计算范围: [FTF ... 最后一个数据字节] (不含帧头)。
 *
 * @param data  输入数据指针
 * @param len   数据长度
 * @return      8位 CRC 值
 */
static uint8_t camyu_calculate_bcc(const uint8_t* data, uint32_t len) {
    uint8_t crc = 0;
    for (; len > 0; len--) {
        crc = CRC8Table[crc ^ *data];
        data++;
    }
    return crc;
}

/* ==================== 内部解析(单字节) ============================== */

/**
 * @brief  通过 Camyu 状态机处理一个字节
 *
 * 处理字节转义并驱动解析器状态机通过帧的各字段。
 * 由 @ref camyu_parse_data 在转义处理后调用。
 *
 * @param parser  解析器实例
 * @param byte    待处理的字节(已解除转义)
 * @return        错误码
 */
static camyu_error_t camyu_parse_data_internal(protocol_parser_t* parser,
                                                 uint8_t byte) {
    camyu_protocol_parser_t* cp = (camyu_protocol_parser_t*)parser;
    camyu_error_t err = CAMYU_ERR_INCOMPLETE;

    switch (cp->pri.camyu_parser_state) {

    case CAMYU_PARSER_STATE_WAIT_HEAD:
        if (byte != HEADCHAR) {
            cp->pri.stats.header_errors++;
            protocol_parser_on_frame_error(parser, CAMYU_ERR_INVALID_HEADER);
            err = CAMYU_ERR_INVALID_HEADER;
        } else {
            cp->pri.camyu_parser_state = CAMYU_PARSER_STATE_WAIT_TFT;
            cp->base.rx.buffer[cp->base.rx.data_len++] = byte;
        }
        break;

    case CAMYU_PARSER_STATE_WAIT_TFT: {
        ftf_t* ftf = (ftf_t*)&byte;
        if (ftf->opcode > PCTOCAMERA_READ) {
            cp->pri.stats.header_errors++;
            err = CAMYU_ERR_INVALID_HEADER;
            protocol_parser_on_frame_error(parser, err);
        } else {
            cp->pri.current_frame_info.tft_value = *ftf;
            cp->pri.camyu_parser_state = CAMYU_PARSER_STATE_WAIT_DATALEN;
            cp->base.rx.buffer[cp->base.rx.data_len++] = byte;
        }
        break;
    }
    case CAMYU_PARSER_STATE_WAIT_DATALEN:
        cp->pri.current_frame_info.current_frame_data_len = byte;
        cp->pri.camyu_parser_state = CAMYU_PARSER_STATE_WAIT_CMD;
        cp->base.rx.buffer[cp->base.rx.data_len++] = byte;
        frame_info_update(parser);
        break;

    case CAMYU_PARSER_STATE_WAIT_CMD:
        cp->base.rx.buffer[cp->base.rx.data_len++] = byte;
        cp->pri.current_rev_cmd_len++;
        if (cp->pri.current_rev_cmd_len == cp->pri.current_frame_info.cmd_len_byte) {
            if (cp->pri.current_frame_info.current_frame_data_len == 0) {
                cp->pri.camyu_parser_state = CAMYU_PARSER_STATE_WAIT_BCC;
            } else {
                cp->pri.camyu_parser_state = CAMYU_PARSER_STATE_WAIT_DATA;
            }
        }
        break;

    case CAMYU_PARSER_STATE_WAIT_DATA:
        cp->base.rx.buffer[cp->base.rx.data_len++] = byte;
        cp->pri.current_rev_data_len++;
        if (cp->pri.current_rev_data_len == cp->pri.current_frame_info.current_frame_data_len) {
            cp->pri.camyu_parser_state = CAMYU_PARSER_STATE_WAIT_BCC;
        }
        break;

    case CAMYU_PARSER_STATE_WAIT_BCC: {
        cp->base.rx.buffer[cp->base.rx.data_len++] = byte;

        /* 如果校验使能，验证 BCC */
        uint32_t bcc_len = cp->pri.current_frame_info.current_frame_total_len
                           - HEADCHAR_LEN - BCC_LEN;
        uint8_t calc = camyu_calculate_bcc(&cp->base.rx.buffer[1], bcc_len);
        if (calc != byte) {
            cp->pri.stats.checksum_errors++;
            err = CAMYU_ERR_BCC_CHECKSUM;
            protocol_parser_on_frame_error(parser, err);
        } else {
            /* 提取解析字段 */
            memcpy(&cp->pri.parsed_id,
                   &cp->base.rx.buffer[CMD_INDEX],
                   cp->pri.current_frame_info.cmd_len_byte);

            cp->pri.parsed_data =
                &cp->base.rx.buffer[HEADCHAR_LEN + TFT_LEN + DATALEN_LEN
                                    + cp->pri.current_frame_info.cmd_len_byte];
            cp->pri.parsed_data_len = cp->pri.current_frame_info.current_frame_data_len;

            /* 通知框架 — 内部调用 encode 和 tx_ready */
            parser->parsed_result = &cp->pri;
            protocol_parser_on_frame_ready(parser);
            err = CAMYU_ERR_NONE;
        }
        break;
    }
    }
    return err;
}

/* ==================== 外部解析入口 ================================== */

static int camyu_parse_data(protocol_parser_t* parser,
                           const uint8_t* data, uint32_t len) {
    if (!parser || !data || len == 0) {
        return CAMYU_ERR_INVALID_PARAM;
    }

    camyu_protocol_parser_t* cp = (camyu_protocol_parser_t*)parser;
    camyu_error_t err = CAMYU_ERR_INCOMPLETE;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        /* 帧头字节无条件重置解析器 */
        if (byte == HEADCHAR) {
            camyu_reset(parser);
            err = camyu_parse_data_internal(parser, byte);
            continue;
        }

        if (cp->pri.transchar_active) {
            /* 期望转义后的字节 */
            if (byte != TRANSHEAD && byte != TRANSTRANS) {
                err = CAMYU_ERR_INVALID_PARAM;
                protocol_parser_on_frame_error(parser, err);
            } else {
                byte ^= CAMYU_ESCAPE_XOR_MASK;
                err = camyu_parse_data_internal(parser, byte);
                cp->pri.transchar_active = false;
            }
        } else if (byte == TRANSCHAR) {
            cp->pri.transchar_active = true;
            err = CAMYU_ERR_INCOMPLETE;
        } else {
            err = camyu_parse_data_internal(parser, byte);
        }
    }
    return err;
}

/* ==================== 编码 ========================================== */

static uint32_t camyu_encode(protocol_parser_t* parser, const void* data) {
    (void)data;
    camyu_protocol_parser_t* cp = (camyu_protocol_parser_t*)parser;
    uint32_t raw_len = 0;
    uint32_t escaped_len = 0;
    uint8_t* tx = cp->base.tx.buffer;
    uint32_t tx_size = cp->base.tx.size;

    /* 1. 帧头 */
    tx[raw_len++] = HEADCHAR;

    /* 2. FTF — 保留接收字段，将 opcode 设为 RESPONSE */
    ftf_t ftf_resp = cp->pri.current_frame_info.tft_value;
    ftf_resp.opcode = CAMERATOPC_RESPONSE;
    tx[raw_len++] = *((uint8_t*)(&ftf_resp));

    /* 3. 数据长度 */
    if (cp->pri.current_frame_info.opcode == PCTOCAMERA_READ) {
        tx[raw_len++] = (uint8_t)(cp->pri.parsed_data_len & 0xFF);
    } else {
        tx[raw_len++] = 0;
    }

    /* 4. 命令/地址字段 */
    memcpy(&tx[raw_len], &cp->pri.parsed_id,
           cp->pri.current_frame_info.cmd_len_byte);
    raw_len += cp->pri.current_frame_info.cmd_len_byte;

    /* 5. 状态字节(固定为 0 = 成功) */
    tx[raw_len++] = 0;

    /* 6. 负载数据(仅当存在时) */
    if (cp->pri.parsed_data_len > 0 && cp->pri.parsed_data
        && cp->pri.parsed_data_len + raw_len < tx_size) {
        memcpy(&tx[raw_len], cp->pri.parsed_data, cp->pri.parsed_data_len);
        raw_len += cp->pri.parsed_data_len;
    } else if (cp->pri.parsed_data_len > 0) {
        protocol_parser_on_frame_error(parser, CAMYU_ERR_FRAME_TOO_LONG);
        return 0;
    }

    /* 7. BCC 校验(覆盖 [FTF ... 最后一个数据字节]) */
    uint8_t crc8 = camyu_calculate_bcc(&tx[TFT_INDEX], raw_len - HEADCHAR_LEN);
    tx[raw_len++] = crc8;

    /* 8. 计算转义后长度 */
    escaped_len = 1;  /* 帧头不转义 */
    for (uint32_t i = 1; i < raw_len; i++) {
        if (tx[i] == HEADCHAR || tx[i] == TRANSCHAR) {
            escaped_len += 2;
        } else {
            escaped_len += 1;
        }
    }

    /* 9. 缓冲区溢出检查 */
    if (escaped_len > tx_size) {
        protocol_parser_on_frame_error(parser, CAMYU_ERR_FRAME_TOO_LONG);
        return 0;
    }

    /* 10. 字节转义(逆序, 原地操作) */
    uint32_t src = raw_len - 1;
    uint32_t dst = escaped_len - 1;
    while (src > 0) {
        if (tx[src] == HEADCHAR) {
            tx[dst--] = TRANSHEAD;
            tx[dst--] = TRANSCHAR;
        } else if (tx[src] == TRANSCHAR) {
            tx[dst--] = TRANSTRANS;
            tx[dst--] = TRANSCHAR;
        } else {
            tx[dst--] = tx[src];
        }
        src--;
    }

    cp->base.tx.data_len = escaped_len;
    return escaped_len;
}

/* ==================== 重置与销毁 ==================================== */

static void camyu_reset(protocol_parser_t* parser) {
    camyu_protocol_parser_t* cp = (camyu_protocol_parser_t*)parser;
    camyu_stats_t saved = cp->pri.stats;
    default_reset(parser);
    memset(&cp->pri, 0, sizeof(camyu_private_t));
    cp->pri.camyu_parser_state = CAMYU_PARSER_STATE_WAIT_HEAD;
    cp->pri.stats = saved;  /* 跨重置保留统计信息 */
}

static void camyu_destroy(protocol_parser_t* parser) {
    (void)parser;
}
