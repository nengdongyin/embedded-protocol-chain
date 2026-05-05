/**
 * @file  protocol_parser_ymodem.c
 * @brief Ymodem 协议适配器实现
 *
 * 桥接 Ymodem 原生收发器到 protocol_parser 基类虚函数表，
 * 实现字节流喂入、错误码映射、应答数据转发。
 *
 * 接收链路:
 *   protocol_chain_feed → parse_data → ymodem_receiver_parse
 *     → frame_stage_process → send_response 桥接
 *     → 复制 ACK/NAK 到 base.tx.buffer → fire tx_ready
 *   frame_ready 事件通过 receiver_event_bridge 直接触发用户回调,
 *   Ymodem 采用延迟复位机制, 不经过 protocol_parser_on_frame_ready.
 *
 * 发送链路:
 *   protocol_chain_feed → parse_data → ymodem_sender_parse
 *     → frame_stage_process → frame_build_with_data
 *     → send_packet 桥接 → base.tx.data_len 同步 → fire tx_ready
 *   frame_ready 事件通过 sender_event_bridge 直接触发用户回调,
 *   帧构建在回调返回后完成, 不经过 protocol_parser_on_frame_ready.
 *
 * @author Protocol Parser Framework
 * @version 1.1
 * @date   2024
 */
#include "protocol_parser_ymodem.h"
#include <string.h>

/* ==================== 前置声明 ====================================== */

static int ymodem_parse_data(protocol_parser_t* parser, const uint8_t* data, uint32_t len);
static int ymodem_sender_parse_data(protocol_parser_t* parser, const uint8_t* data, uint32_t len);
static void ymodem_receiver_reset_adapter(protocol_parser_t* parser);
static void ymodem_sender_reset_adapter(protocol_parser_t* parser);
static void ymodem_destroy(protocol_parser_t* parser);
static bool ymodem_receiver_poll_bridge(protocol_parser_t* parser);
static bool ymodem_sender_poll_bridge(protocol_parser_t* parser);

/* ==================== 虚函数表 ====================================== */

static const struct protocol_parser_ops ymodem_receiver_protocol_ops = {
    .parse_data = ymodem_parse_data,
    .encode     = NULL,
    .reset      = ymodem_receiver_reset_adapter,
    .destroy    = ymodem_destroy,
    .poll       = ymodem_receiver_poll_bridge,
};

/* 发送器虚函数桥接 */
static int ymodem_sender_parse_data(protocol_parser_t* parser, const uint8_t* data, uint32_t len) {
    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)parser;
    ymodem_error_e err = ymodem_sender_parse(&yp->pri.sender, data, len);
    return (int)err;
}

static bool ymodem_receiver_poll_bridge(protocol_parser_t* parser) {
    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)parser;
    return ymodem_receiver_poll(&yp->pri.receiver);
}

static bool ymodem_sender_poll_bridge(protocol_parser_t* parser) {
    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)parser;
    return ymodem_sender_poll(&yp->pri.sender);
}

static const struct protocol_parser_ops ymodem_sender_protocol_ops = {
    .parse_data = ymodem_sender_parse_data,
    .encode     = NULL,
    .reset      = ymodem_sender_reset_adapter,
    .destroy    = ymodem_destroy,
    .poll       = ymodem_sender_poll_bridge,
};

/* ==================== 错误映射表 ==================================== */

static const ErrorMapEntry ymodem_error_map[] = {
    { YMODEM_ERROR_NONE,                    PARSER_ERR_NONE },
    { YMODEM_ERROR_WAIT_MORE,              PARSER_ERR_INCOMPLETE },
    { YMODEM_ERROR_GARBAGE,                PARSER_ERR_FRAME },
    { YMODEM_ERROR_TIME_OUT,               PARSER_ERR_TIMEOUT },
    { YMODEM_ERROR_CRC,                    PARSER_ERR_FRAME },
    { YMODEM_ERROR_SEQ,                    PARSER_ERR_FRAME },
    { YMODEM_ERROR_HANDSHAKE_NACK,         PARSER_ERR_FRAME },
    { YMODEM_ERROR_RETRANSMISSION_COUNT_MAX, PARSER_ERR_FRAME },
    { YMODEM_ERROR_SENDER_NO_REV_ACK,      PARSER_ERR_INCOMPLETE },
    { YMODEM_ERROR_RESEND,                 PARSER_ERR_INCOMPLETE },
    { YMODEM_ERROR_CAN,                    PARSER_ERR_FRAME },
};

static const ErrorMapper ymodem_error_mapper = {
    .table = ymodem_error_map,
    .count = sizeof(ymodem_error_map) / sizeof(ymodem_error_map[0]),
};

/* ==================== 回调桥接函数 ================================== */

/**
 * @brief  接收器事件 → 基类 frame_ready 桥接
 *
 * 将 Ymodem 接收器内部事件(FILE_INFO/DATA_PACKET/TRANSFER_COMPLETE等)
 * 封装到 base->parsed_result 并直接触发 frame_ready 用户回调。
 *
 * 不走 protocol_parser_on_frame_ready(), 因为:
 * - encode: Ymodem 接收器的 ACK/NAK 由 send_response 桥接独立发送,
 *   不需要框架自动编码
 * - tx_ready: 应答已由 send_response 桥接触发
 * - reset: Ymodem 采用延迟复位机制(frame_is_end), 由下次帧头触发,
 *   框架立即复位会破坏该机制
 *
 * @note  ERROR 事件不触发 frame_ready(走 parse_data 错误码返回路径)。
 */
static void ymodem_receiver_event_bridge(ymodem_receiver_parser_t* recv,
                                          const ymodem_receiver_event_t* event,
                                          void* user_ctx)
{
    (void)user_ctx;

    if (event->type == YMODEM_RECV_EVENT_ERROR) {
        return;
    }

    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)
        ((uint8_t*)recv - offsetof(ymodem_protocol_parser_t, pri.receiver));
    protocol_parser_t* base = &yp->base;

    yp->pri.parsed_result.recv_evt = *event;
    base->parsed_result = &yp->pri.parsed_result.recv_evt;
    base->stats.frames_received++;
    if (base->callbacks.frame_ready) {
        base->callbacks.frame_ready(base, base->parsed_result, base->callbacks.frame_ready_ctx);
    }
}

/**
 * @brief  发送器事件 → 基类 frame_ready 桥接
 *
 * 将 Ymodem 发送器内部事件(FILE_INFO/DATA_PACKET/TRANSFER_COMPLETE等)
 * 封装到 base->parsed_result 并直接触发 frame_ready 用户回调。
 *
 * 不走 protocol_parser_on_frame_ready(), 因为:
 * - encode: Ymodem 发送器帧构建在 frame_user_event_callback 返回后
 *   由 frame_build_with_data 继续执行, 其依赖的 frame_info 不能被
 *   框架的 reset 清零
 * - tx_ready: 发送器的数据帧由 send_packet 桥接独立触发
 * - reset: 由 frame_stage_process 末尾统一调用 ymodem_sender_reset,
 *   框架提前 reset 会清零 frame_info 导致 build_data_packet 读到全零字段
 *
 * ERROR 和 SESSION_FINISHED 事件不触发 frame_ready:
 * - SESSION_FINISHED: 发送器自行回到 IDLE, 用户通过 tx_ready 收最后一帧
 * - ERROR: 走 sender_parse 返回的错误码路径
 */
static void ymodem_sender_event_bridge(ymodem_sender_t* send,
                                        ymodem_sender_event_t* event,
                                        void* user_ctx)
{
    (void)user_ctx;

    if (event->type == YMODEM_SENDER_EVENT_ERROR ||
        event->type == YMODEM_SENDER_EVENT_SESSION_FINISHED) {
        return;
    }

    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)
        ((uint8_t*)send - offsetof(ymodem_protocol_parser_t, pri.sender));
    protocol_parser_t* base = &yp->base;

    yp->pri.parsed_result.send_evt = *event;
    base->parsed_result = &yp->pri.parsed_result.send_evt;
    base->stats.frames_received++;
    if (base->callbacks.frame_ready) {
        base->callbacks.frame_ready(base, base->parsed_result, base->callbacks.frame_ready_ctx);
    }
}

/**
 * @brief  接收器 send_response → 基类 tx_ready 桥接
 *
 * Ymodem 接收器内部构造 ACK/NAK/C/CAN 应答后，通过此桥接
 * 将应答数据复制到基类 tx_buffer 并触发 tx_ready 回调。
 *
 * 通过 offsetof 从 receiver 指针反推 adapter 基类指针。
 */
static void ymodem_send_response_bridge(ymodem_receiver_parser_t* recv, void* user_ctx)
{
    (void)user_ctx;

    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)
        ((uint8_t*)recv - offsetof(ymodem_protocol_parser_t, pri.receiver));
    protocol_parser_t* base = &yp->base;

    base->tx.data_len = recv->buffer.tx_buffer_ack_len;
    if (base->tx.data_len > 0 && base->tx.data_len <= base->tx.size) {
        memcpy(base->tx.buffer, recv->buffer.tx_buffer, recv->buffer.tx_buffer_ack_len);
    }

    if (base->tx.data_len > 0 && base->callbacks.tx_ready) {
        base->callbacks.tx_ready(base, base->callbacks.tx_ready_ctx);
    }
}

/**
 * @brief  发送器 send_packet → 基类 tx_ready 桥接
 *
 * Ymodem 发送器组装完数据帧后，通过此桥接同步
 * tx_buffer_active_len 到基类并触发 tx_ready 回调。
 *
 * 发送器的 tx_buffer 与基类 tx_buffer 指向同一块内存，
 * 因此无需复制数据，仅同步长度。
 */
static void ymodem_send_packet_bridge(ymodem_sender_t* send, ymodem_sender_event_t* evt, void* user_ctx)
{
    (void)evt;
    (void)user_ctx;

    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)
        ((uint8_t*)send - offsetof(ymodem_protocol_parser_t, pri.sender));
    protocol_parser_t* base = &yp->base;

    base->tx.data_len = send->buffer.tx_buffer_active_len;

    if (base->tx.data_len > 0 && base->callbacks.tx_ready) {
        base->callbacks.tx_ready(base, base->callbacks.tx_ready_ctx);
    }
}

/* ======================== create_receiver ============================ */

ymodem_protocol_parser_t* ymodem_protocol_create_receiver(void* rx_buffer, uint32_t rx_buffer_size,
                                                           void* tx_buffer, uint32_t tx_buffer_size) {
    parser_config_t cfg = get_default_config();
    cfg.max_frame_len = YMODEM_STX_FRAME_LEN_BYTE;
    cfg.timeout_ms = YMODEM_TIMEOUT_MS;

    protocol_parser_t* base = protocol_parser_create_common_ex(
        sizeof(ymodem_protocol_parser_t),
        &ymodem_receiver_protocol_ops,
        &cfg,
        (uint8_t*)rx_buffer, rx_buffer_size, YMODEM_STX_FRAME_LEN_BYTE,
        (uint8_t*)tx_buffer, tx_buffer_size, 4);
    if (!base) {
        return NULL;
    }
    base->error_mapper = ymodem_error_mapper;

    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)base;
    ymodem_receiver_create(&yp->pri.receiver, base->rx.buffer, base->rx.size);
    ymodem_receiver_set_event_callback(&yp->pri.receiver,
        ymodem_receiver_event_bridge, NULL);
    ymodem_receiver_set_send_response_callback(&yp->pri.receiver,
        ymodem_send_response_bridge, NULL);

    return yp;
}

/* ======================== create_sender ============================== */

ymodem_protocol_parser_t* ymodem_protocol_create_sender(void* rx_buffer, uint32_t rx_buffer_size,
                                                         void* tx_buffer, uint32_t tx_buffer_size) {
    parser_config_t cfg = get_default_config();
    cfg.max_frame_len = YMODEM_STX_FRAME_LEN_BYTE;
    cfg.timeout_ms = YMODEM_TIMEOUT_MS;

    protocol_parser_t* base = protocol_parser_create_common_ex(
        sizeof(ymodem_protocol_parser_t),
        &ymodem_sender_protocol_ops,
        &cfg,
        (uint8_t*)rx_buffer, rx_buffer_size, 4,
        (uint8_t*)tx_buffer, tx_buffer_size, YMODEM_STX_FRAME_LEN_BYTE);
    if (!base) {
        return NULL;
    }
    base->error_mapper = ymodem_error_mapper;

    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)base;
    ymodem_sender_create(&yp->pri.sender, base->tx.buffer, base->tx.size);
    ymodem_sender_set_event_callback(&yp->pri.sender,
        ymodem_sender_event_bridge, NULL);
    ymodem_sender_set_send_packet_callback(&yp->pri.sender,
        ymodem_send_packet_bridge, NULL);

    return yp;
}

/* ==================== 访问器 ======================================== */

ymodem_sender_t* ymodem_adapter_get_sender(ymodem_protocol_parser_t* parser) {
    if (!parser) {
        return NULL;
    }
    return &parser->pri.sender;
}

ymodem_receiver_parser_t* ymodem_adapter_get_receiver(ymodem_protocol_parser_t* parser) {
    if (!parser) {
        return NULL;
    }
    return &parser->pri.receiver;
}

/* ==================== 生命周期控制 =================================== */

bool ymodem_adapter_start_receiver(ymodem_protocol_parser_t* parser) {
    if (!parser) {
        return false;
    }
    return ymodem_receiver_start(&parser->pri.receiver);
}

bool ymodem_adapter_start_sender(ymodem_protocol_parser_t* parser) {
    if (!parser) {
        return false;
    }
    return ymodem_sender_start(&parser->pri.sender);
}

/* ==================== 数据解析 ====================================== */

static int ymodem_parse_data(protocol_parser_t* parser, const uint8_t* data, uint32_t len) {
    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)parser;

    if (!data || len == 0) {
        return YMODEM_ERROR_WAIT_MORE;
    }

    if (!yp->pri.receiver.buffer.rx_buffer) {
        return YMODEM_ERROR_GARBAGE;
    }

    ymodem_error_e err = ymodem_receiver_parse(&yp->pri.receiver, data, len);
    return (int)err;
}

/* ==================== 重置与销毁 ==================================== */

static void ymodem_receiver_reset_adapter(protocol_parser_t* parser) {
    if (!parser) {
        return;
    }
    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)parser;
    default_reset(parser);
    (void)yp;
    /* Ymodem 接收器采用延迟复位机制:
     * frame_stage_process() 仅设置 frame_is_end=1,
     * 真正的 ymodem_receiver_reset() 在下次收到帧头字节时
     * 由 ymodem_receiver_parse() 内部自动触发。
     * 此处不调用 ymodem_receiver_reset(), 否则会与延迟机制冲突:
     * - 适配器立即复位 → 清除 frame_is_end
     * - frame_stage_process 重新置回 frame_is_end
     * - 下次帧头再次触发复位 → 一帧内重复复位, 状态被反复清空 */
}

static void ymodem_sender_reset_adapter(protocol_parser_t* parser) {
    if (!parser) {
        return;
    }
    ymodem_protocol_parser_t* yp = (ymodem_protocol_parser_t*)parser;
    default_reset(parser);
    ymodem_sender_reset(&yp->pri.sender);
}

static void ymodem_destroy(protocol_parser_t* parser) {
    (void)parser;
}
