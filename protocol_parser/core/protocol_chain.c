/**
 * @file  protocol_chain.c
 * @brief 协议链(责任链模式)实现
 *
 * 管理多个协议解析器, 自动将接收数据匹配到首个能成功解析的解析器.
 * 一旦匹配成功, 该解析器被"锁定"用于后续数据.
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#include "protocol_chain.h"


/**
 * @struct protocol_chain
 * @brief  协议链管理器(不透明结构体)
 */
struct protocol_chain {
    protocol_parser_t** parsers;       /**< 解析器指针数组 */
    uint32_t            max_parsers;  /**< 最大解析器数量 */
    uint32_t            count;        /**< 当前解析器数量 */
    protocol_parser_t*  locked_parser; /**< 当前锁定的解析器(NULL = 无) */
};

/* ======================== 创建/销毁 ================================= */

protocol_chain* protocol_chain_create(uint32_t max_parsers) {
    if (max_parsers == 0) {
        return NULL;
    }

    protocol_chain* chain = (protocol_chain*)system_malloc(sizeof(protocol_chain));
    if (!chain) {
        return NULL;
    }

    chain->parsers = (protocol_parser_t**)system_malloc(
        max_parsers * sizeof(protocol_parser_t*));
    if (!chain->parsers) {
        system_free(chain);
        return NULL;
    }
    memset(chain->parsers, 0, max_parsers * sizeof(protocol_parser_t*));

    chain->max_parsers = max_parsers;
    chain->count = 0;
    chain->locked_parser = NULL;

    return chain;
}

void protocol_chain_destroy(protocol_chain* chain) {
    if (chain) {
        system_free(chain->parsers);
        system_free(chain);
    }
}

/* ======================== 解析器管理 ================================ */

bool protocol_chain_add_parser(protocol_chain* chain, protocol_parser_t* parser) {
    if (!chain || !parser || chain->count >= chain->max_parsers) {
        return false;
    }
    chain->parsers[chain->count++] = parser;
    return true;
}

bool protocol_chain_remove_parser(protocol_chain* chain, protocol_parser_t* parser) {
    if (!chain || !parser) {
        return false;
    }

    for (uint32_t i = 0; i < chain->count; i++) {
        if (chain->parsers[i] == parser) {
            /* 将后续解析器前移 */
            for (uint32_t j = i; j < chain->count - 1; j++) {
                chain->parsers[j] = chain->parsers[j + 1];
            }
            chain->count--;

            if (chain->locked_parser == parser) {
                parser->locked = false;
                chain->locked_parser = NULL;
            }
            return true;
        }
    }
    return false;
}

/* ======================== 数据送入 ================================== */

parser_error_t protocol_chain_feed(protocol_chain* chain,
                                   const uint8_t* data, uint32_t len) {
    parser_error_t gen_err;

    if (!chain || !data || !len || chain->count == 0) {
        return PARSER_ERR_INVALID_PARAM;
    }

    /* 如果已锁定解析器, 直接转发数据 */
    if (chain->locked_parser != NULL) {
        gen_err = protocol_parser_parse_data(chain->locked_parser, data, len);
        if (parser_error_is_fatal(gen_err)) {
            chain->locked_parser->locked = false;
            chain->locked_parser = NULL;  /* 致命错误时解锁 */
        }
        else {
            return gen_err;
        }
    }

    /* 遍历每个解析器:
     * - NONE (成功)     → 锁定并返回
     * - INCOMPLETE      → 记录标记, 继续尝试其他解析器
     * - 致命错误        → 尝试下一个解析器 */
    bool saw_incomplete = false;
    for (uint32_t i = 0; i < chain->count; i++) {
        protocol_parser_t* parser = chain->parsers[i];

        gen_err = protocol_parser_parse_data(parser, data, len);

        if (gen_err == PARSER_ERR_NONE) {
            /* 若用户回调已提前锁定其他解析器(如通过控制寄存器切换协议),
             * 则不再覆盖, 尊重用户的手动指定 */
            if (chain->locked_parser == NULL) {
                parser->locked = true;
                chain->locked_parser = parser;
            }
            return gen_err;
        }

        if (gen_err == PARSER_ERR_INCOMPLETE) {
            saw_incomplete = true;
            /* 继续尝试其他解析器 */
        }
    }

    /* 仅当解析器完全匹配时才锁定; 流式数据返回 INCOMPLETE */
    if (saw_incomplete) {
        return PARSER_ERR_INCOMPLETE;
    }

    return PARSER_ERR_UNKNOWN;
}

parser_error_t protocol_chain_feed_frame(protocol_chain* chain,
                                         const uint8_t* frame, uint32_t len) {
    parser_error_t gen_err;

    if (!chain || !frame || !len || chain->count == 0) {
        return PARSER_ERR_INVALID_PARAM;
    }

    /* 锁定状态下送入整帧 — 任何错误都导致解锁 */
    if (chain->locked_parser != NULL) {
        gen_err = protocol_parser_parse_data(chain->locked_parser, frame, len);
        if (gen_err != PARSER_ERR_NONE) {
            chain->locked_parser->locked = false;
            chain->locked_parser = NULL;
        }
        return gen_err;
    }

    /* 遍历每个解析器 */
    for (uint32_t i = 0; i < chain->count; i++) {
        protocol_parser_t* parser = chain->parsers[i];

        gen_err = protocol_parser_parse_data(parser, frame, len);

        if (gen_err == PARSER_ERR_NONE) {
            if (chain->locked_parser == NULL) {
                parser->locked = true;
                chain->locked_parser = parser;
            }
            return gen_err;
        }
    }

    return PARSER_ERR_UNKNOWN;
}

/* ======================== 锁定管理 ================================== */

protocol_parser_t* protocol_chain_get_locked_parser(protocol_chain* chain) {
    return chain ? chain->locked_parser : NULL;
}

bool protocol_chain_set_locked_parser(protocol_chain* chain,
                                      protocol_parser_t* parser) {
    if (!chain) {
        return false;
    }
    if (parser == NULL) {
        if (chain->locked_parser) {
            chain->locked_parser->locked = false;
        }
        chain->locked_parser = NULL;
        return true;
    }

    for (uint32_t i = 0; i < chain->count; i++) {
        if (chain->parsers[i] == parser) {
            parser->locked = true;
            chain->locked_parser = parser;
            return true;
        }
    }
    return false;
}

/* ======================== 超时轮询 ================================== */

bool protocol_chain_check_timeout_poll(protocol_chain* chain) {
    if (!chain || chain->count == 0) {
        return false;
    }

    /* 若有锁定解析器, 仅轮询该解析器 */
    if (chain->locked_parser != NULL) {
        protocol_parser_t* parser = chain->locked_parser;

        if (protocol_parser_check_timeout_poll(parser)) {
            parser->locked = false;
            chain->locked_parser = NULL;
            return true;
        }
        return false;
    }

    /* 未锁定: 检查所有注册的解析器 */
    bool any_timeout = false;

    for (uint32_t i = 0; i < chain->count; i++) {
        protocol_parser_t* parser = chain->parsers[i];

        if (protocol_parser_check_timeout_poll(parser)) {
            any_timeout = true;
        }
    }
    return any_timeout;
}
