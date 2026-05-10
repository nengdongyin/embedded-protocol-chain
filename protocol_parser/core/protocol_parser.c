/**
 * @file  protocol_parser.c
 * @brief 协议解析器基类实现
 *
 * 提供通用基础设施: 配置验证, 静态/动态初始化,
 * 超时轮询, 以及帧就绪/帧错误通知管道.
 *
 * @author Protocol Parser Framework
 * @version 3.0
 * @date   2024
 */
#include "protocol_parser.h"
#include "protocol_parser_internal.h"

/* ==================== 配置验证 ====================================== */

bool protocol_parser_validate_config(const parser_config_t* config) {
    if (!config) {
        return false;
    }
    if (config->max_frame_len < MIN_FRAME_LENGTH) {
        return false;
    }
    return true;
}

/* ====================== 默认重置 ==================================== */

void default_reset(protocol_parser_t* parser) {
    if (!parser) {
        return;
    }
    parser->rx.data_len = 0;
    parser->tx.data_len = 0;
    parser->timeout.is_active = false;
    parser->parsed_result = NULL;
}

/* ==================== 静态初始化 ==================================== */

bool protocol_parser_static_init(
    protocol_parser_t* parser,
    const struct protocol_parser_ops* ops,
    const parser_config_t* config,
    uint8_t* rx_buffer,
    uint32_t rx_buffer_size,
    uint8_t* tx_buffer,
    uint32_t tx_buffer_size
) {
    if (!parser || !ops || !config || !rx_buffer || !tx_buffer) {
        return false;
    }
    if (!protocol_parser_validate_config(config)) {
        return false;
    }

    memset(parser, 0, sizeof(*parser));
    parser->ops = ops;
    parser->config = *config;

    if (rx_buffer_size < parser->config.max_frame_len) {
        return false;
    }
    parser->rx.buffer = rx_buffer;
    parser->rx.size = rx_buffer_size;
    parser->rx.data_len = 0;
    parser->rx.own_buffer = false;

    if (tx_buffer_size < parser->config.max_frame_len) {
        return false;
    }
    parser->tx.buffer = tx_buffer;
    parser->tx.size = tx_buffer_size;
    parser->tx.data_len = 0;
    parser->tx.own_buffer = false;

    parser->ops->reset(parser);
    return true;
}

/* ==================== 动态初始化 ==================================== */

bool protocol_parser_dynamic_init(
    protocol_parser_t* parser,
    const struct protocol_parser_ops* ops,
    const parser_config_t* config
) {
    if (!parser || !ops) {
        return false;
    }

    memset(parser, 0, sizeof(*parser));
    parser->ops = ops;

    if (config) {
        if (!protocol_parser_validate_config(config)) {
            return false;
        }
        parser->config = *config;
    } else {
        parser->config = get_default_config();
    }

    parser->rx.buffer = system_malloc(parser->config.max_frame_len);
    if (!parser->rx.buffer) {
        return false;
    }
    parser->rx.size = parser->config.max_frame_len;
    parser->rx.data_len = 0;
    parser->rx.own_buffer = true;

    parser->tx.buffer = system_malloc(parser->config.max_frame_len);
    if (!parser->tx.buffer) {
        system_free(parser->rx.buffer);
        parser->rx.buffer = NULL;
        parser->rx.own_buffer = false;
        return false;
    }
    parser->tx.size = parser->config.max_frame_len;
    parser->tx.data_len = 0;
    parser->tx.own_buffer = true;

    parser->ops->reset(parser);
    return true;
}

/* ===================== 超时管理 ===================================== */

void protocol_parser_update_time(protocol_parser_t* parser) {
    if (parser) {
        parser->timeout.last_activity_ms = system_get_time_ms();
        parser->timeout.is_active = true;
    }
}

/* ===================== 帧错误通知 =================================== */

void protocol_parser_on_frame_error(protocol_parser_t* parser, int err) {
    if (!parser) {
        return;
    }

    /* 将子类特定错误码映射为通用码后再统计 */
    parser_error_t gen_err = parser_error_map(&parser->error_mapper, err);

    switch (gen_err) {
    case PARSER_ERR_TIMEOUT:
        parser->stats.timeout_errors++;
        break;
    case PARSER_ERR_INVALID_PARAM:
        parser->stats.invalid_param_errors++;
        break;
    case PARSER_ERR_FRAME:
        parser->stats.frame_errors++;
        break;
    default:
        parser->stats.other_errors++;
        break;
    }

    /* 若解析器已被协议链锁定, 发送错误应答.
     * 匹配阶段(locked == false)时错误应答被抑制,
     * 防止非匹配协议产生总线噪声.
     *
     * 重要: tx_ready 必须在 reset() 之前调用. */
    if (parser->tx.data_len > 0 && parser->callbacks.tx_ready && parser->locked) {
        parser->callbacks.tx_ready(parser, parser->callbacks.tx_ready_ctx);
    }

    /* 将解析器重置为干净状态, 保护 locked 标志不被子类 reset 误清 */
    bool saved_locked = parser->locked;
    parser->ops->reset(parser);
    parser->locked = saved_locked;
}

/* ===================== 帧就绪通知 =================================== */

void protocol_parser_on_frame_ready(protocol_parser_t* parser) {
    if (!parser) {
        return;
    }

    parser->stats.frames_received++;

    /* 1. 通知用户 – 他们可以填充子类解析数据 */
    if (parser->callbacks.frame_ready) {
        parser->callbacks.frame_ready(parser, parser->parsed_result,
                                       parser->callbacks.frame_ready_ctx);
    }

    /* 2. 自动编码应答到 tx_buffer */
    if (parser->ops && parser->ops->encode) {
        parser->tx.data_len = parser->ops->encode(parser, NULL);
        if (parser->tx.data_len > 0) {
            parser->stats.frames_encoded++;
        }
    }

    /* 3. 通过用户发送回调发送应答 */
    if (parser->tx.data_len > 0 && parser->callbacks.tx_ready) {
        parser->callbacks.tx_ready(parser, parser->callbacks.tx_ready_ctx);
    }

    /* 4. 重置准备下一帧, 保护 locked 标志不被子类 reset 误清 */
    bool saved_locked = parser->locked;
    parser->ops->reset(parser);
    parser->locked = saved_locked;
}

/* ===================== 通用创建 ===================================== */

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
) {
    if (!ops || !config || instance_size < sizeof(protocol_parser_t)) {
        return NULL;
    }

    protocol_parser_t* parser = (protocol_parser_t*)system_malloc(instance_size);
    if (!parser) {
        return NULL;
    }
    memset(parser, 0, instance_size);

    parser_config_t local_config = *config;
    if (!protocol_parser_validate_config(&local_config)) {
        system_free(parser);
        return NULL;
    }
    parser->config = local_config;
    parser->ops = ops;

    /* --- rx_buffer --- */
    if (rx_buffer) {
        if (rx_buffer_size < rx_min) {
            system_free(parser);
            return NULL;
        }
        parser->rx.buffer = rx_buffer;
        parser->rx.size = rx_buffer_size;
        parser->rx.own_buffer = false;
    } else {
        uint32_t rx_size = rx_min;
        parser->rx.buffer = (uint8_t*)system_malloc(rx_size);
        if (!parser->rx.buffer) {
            system_free(parser);
            return NULL;
        }
        parser->rx.size = rx_size;
        parser->rx.own_buffer = true;
    }
    parser->rx.data_len = 0;

    /* --- tx_buffer --- */
    if (tx_buffer) {
        if (tx_buffer_size < tx_min) {
            if (parser->rx.own_buffer) system_free(parser->rx.buffer);
            system_free(parser);
            return NULL;
        }
        parser->tx.buffer = tx_buffer;
        parser->tx.size = tx_buffer_size;
        parser->tx.own_buffer = false;
    } else {
        uint32_t tx_size = tx_min;
        parser->tx.buffer = (uint8_t*)system_malloc(tx_size);
        if (!parser->tx.buffer) {
            if (parser->rx.own_buffer) system_free(parser->rx.buffer);
            system_free(parser);
            return NULL;
        }
        parser->tx.size = tx_size;
        parser->tx.own_buffer = true;
    }
    parser->tx.data_len = 0;

    parser->ops->reset(parser);
    return parser;
}

protocol_parser_t* protocol_parser_create_common(
    size_t instance_size,
    const struct protocol_parser_ops* ops,
    const parser_config_t* config,
    uint8_t* rx_buffer,
    uint32_t rx_buffer_size,
    uint8_t* tx_buffer,
    uint32_t tx_buffer_size
) {
    return protocol_parser_create_common_ex(
        instance_size, ops, config,
        rx_buffer, rx_buffer_size, config ? config->max_frame_len : 0,
        tx_buffer, tx_buffer_size, config ? config->max_frame_len : 0);
}

/* ==================== 公共 API ====================================== */

parser_error_t protocol_parser_parse_data(protocol_parser_t* parser,
                                          const uint8_t* data, uint32_t len) {
    if (!parser || !parser->ops || !data || !len || !parser->ops->parse_data) {
        return PARSER_ERR_INVALID_PARAM;
    }
    protocol_parser_update_time(parser);
    int raw_err = (int)parser->ops->parse_data(parser, data, len);
    return parser_error_map(&parser->error_mapper, raw_err);
}

bool protocol_parser_set_callbacks(protocol_parser_t* parser,
                                   on_frame_ready_t frame_ready_cb,
                                   void* frame_ready_ctx,
                                   on_tx_ready_t tx_ready_cb,
                                   void* tx_ready_ctx) {
    if (!parser) {
        return false;
    }
    parser->callbacks.frame_ready = frame_ready_cb;
    parser->callbacks.frame_ready_ctx = frame_ready_ctx;
    parser->callbacks.tx_ready = tx_ready_cb;
    parser->callbacks.tx_ready_ctx = tx_ready_ctx;
    return true;
}

uint32_t protocol_parser_encode(protocol_parser_t* parser, const void* data) {
    if (!parser || !parser->ops || !parser->ops->encode) {
        return 0;
    }
    uint32_t encoded = parser->ops->encode(parser, data);
    if (encoded > 0) {
        parser->tx.data_len = encoded;
        parser->stats.frames_encoded++;
    }
    return encoded;
}

bool protocol_parser_check_timeout_poll(protocol_parser_t* parser) {
    if (!parser) {
        return false;
    }

    /* 协议特定周期性维护 (握手重试/帧 NAK 等), 独立于基类超时 */
    bool poll_result = false;
    if (parser->ops && parser->ops->poll) {
        poll_result = parser->ops->poll(parser);
    }

    if (parser->config.timeout_ms == 0) {
        return poll_result;
    }
    if (!parser->timeout.is_active) {
        return poll_result;
    }

    uint32_t now = system_get_time_ms();

    /* 处理计时器回绕 */
    if (now < parser->timeout.last_activity_ms) {
        parser->timeout.last_activity_ms = now;
        return poll_result;
    }

    uint32_t elapsed = now - parser->timeout.last_activity_ms;
    if (elapsed >= parser->config.timeout_ms) {
        parser->stats.timeout_errors++;
        protocol_parser_on_frame_error(parser, PARSER_ERR_TIMEOUT);
        return true;
    }

    return poll_result;
}

void protocol_parser_destroy(protocol_parser_t* parser) {
    if (!parser) {
        return;
    }

    /* 释放自有缓冲区 */
    if (parser->rx.own_buffer && parser->rx.buffer) {
        system_free(parser->rx.buffer);
        parser->rx.buffer = NULL;
    }
    if (parser->tx.own_buffer && parser->tx.buffer) {
        system_free(parser->tx.buffer);
        parser->tx.buffer = NULL;
    }

    /* 子类特定清理 */
    if (parser->ops && parser->ops->destroy) {
        parser->ops->destroy(parser);
    }

    system_free(parser);
}

/* ==================== 错误码映射 ============================== */

parser_error_t parser_error_map(const ErrorMapper* mapper, int err) {
    if (!mapper || !mapper->table) return (parser_error_t)err;
    for (size_t i = 0; i < mapper->count; i++) {
        if (mapper->table[i].specific_error == err) {
            return mapper->table[i].generic;
        }
    }
    return (parser_error_t)err;
}

bool parser_error_is_fatal(parser_error_t err) {
    return err != PARSER_ERR_NONE && err != PARSER_ERR_INCOMPLETE;
}

/* ==================== 配置参数 ============================== */

parser_config_t get_default_config(void) {
    return (parser_config_t) {
        .max_frame_len = DEFAULT_MAX_FRAME_LENGTH,
        .timeout_ms = DEFAULT_TIMEOUT_MS,
    };
}

/* ==================== 访问器 ============================== */

uint8_t* protocol_parser_get_tx_data(protocol_parser_t* parser, uint32_t* len) {
    if (!parser) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) {
        *len = parser->tx.data_len;
    }
    return parser->tx.buffer;
}

void protocol_parser_get_stats(const protocol_parser_t* parser, parser_stats_t* out) {
    if (parser && out) {
        *out = parser->stats;
    }
}

void protocol_parser_reset_stats(protocol_parser_t* parser) {
    if (parser) {
        memset(&parser->stats, 0, sizeof(parser->stats));
    }
}
