/**
 * @file  protocol_parser_ymodem.h
 * @brief Ymodem 协议适配器接口
 *
 * 将独立实现的 Ymodem 协议收发器适配为 @ref protocol_parser 基类接口，
 * 可挂载到 @ref protocol_chain 责任链中自动匹配。
 *
 * 适配器内部集成 ymodem_receiver_parser_t 和 ymodem_sender_t，
 * 通过桥接回调将 Ymodem 原生事件/应答映射到 protocol_parser 的
 * frame_ready / tx_ready 回调体系。
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#pragma once
#include "../../../core/protocol_parser.h"
#include "../../../core/protocol_parser_internal.h"
#include "../src/ymodem_receiver.h"
#include "../src/ymodem_sender.h"

/**
 * @brief Ymodem 适配器私有数据
 */
typedef struct {
    ymodem_receiver_parser_t  receiver;          /**< Ymodem 接收器实例(静态嵌入) */
    ymodem_sender_t           sender;            /**< Ymodem 发送器实例(静态嵌入) */
    union {
        ymodem_receiver_event_t  recv_evt;       /**< 接收桥接用事件缓存 */
        ymodem_sender_event_t    send_evt;       /**< 发送桥接用事件缓存 */
    } parsed_result;                              /**< 收发分用, frame_ready 通过 void* 传递 */
} ymodem_private_t;

/**
 * @brief Ymodem 协议适配器结构体(继承 protocol_parser_t)
 */
typedef struct {
    protocol_parser_t base;  /**< 基类(必须为第一个成员) */
    ymodem_private_t  pri;   /**< 协议特定私有数据 */
} ymodem_protocol_parser_t;

/**
 * @brief  创建 Ymodem 接收器适配器(仅接收)
 *
 * 创建后处于静默 @c YMODEM_STAGE_IDLE 状态, 不会发送任何字节。
 * 需通过 @ref ymodem_adapter_start_receiver() 显式激活方可开始握手。
 *
 * 挂载到 @ref protocol_chain 后, 建议流程:
 * - Imperx/Camyu 等其他协议处理日常指令
 * - 收到"启动Ymodem接收"命令后, 调用 @ref ymodem_adapter_start_receiver()
 * - 接收器发送 'C' 字节启动握手, 后续通过责任链喂入数据即可
 *
 * @note rx_buffer 至少需 YMODEM_STX_FRAME_LEN_BYTE (1029) 字节;
 *       tx_buffer 只需 4 字节(接收器应答 < 4B), NULL 将动态分配。
 *
 * @param rx_buffer       外部接收缓冲区(NULL = 动态分配)
 * @param rx_buffer_size  接收缓冲区大小
 * @param tx_buffer       外部发送缓冲区(NULL = 动态分配)
 * @param tx_buffer_size  发送缓冲区大小
 * @return  适配器实例, 失败返回 NULL
 */
ymodem_protocol_parser_t* ymodem_protocol_create_receiver(void* rx_buffer, uint32_t rx_buffer_size,
                                                           void* tx_buffer, uint32_t tx_buffer_size);

/**
 * @brief  创建 Ymodem 发送器适配器(仅发送)
 *
 * 创建后处于静默 @c YMODEM_STAGE_IDLE 状态, 不会发送任何字节。
 * 需通过 @ref ymodem_adapter_start_sender() 显式激活方可开始握手。
 *
 * @note tx_buffer 至少需 YMODEM_STX_FRAME_LEN_BYTE (1029) 字节;
 *       rx_buffer 只需 4 字节(虚拟, 发送器不通过基类收数据),
 *       NULL 将动态分配。
 *
 * @param rx_buffer       外部接收缓冲区(NULL = 动态分配)
 * @param rx_buffer_size  接收缓冲区大小
 * @param tx_buffer       外部发送缓冲区(NULL = 动态分配)
 * @param tx_buffer_size  发送缓冲区大小
 * @return  适配器实例, 失败返回 NULL
 */
ymodem_protocol_parser_t* ymodem_protocol_create_sender(void* rx_buffer, uint32_t rx_buffer_size,
                                                         void* tx_buffer, uint32_t tx_buffer_size);

/**
 * @brief  获取内部 Ymodem 发送器实例
 *
 * 发送器 event_callback 和 send_packet 已由适配器桥接到基类的
 * frame_ready / tx_ready 回调。用户应在调用 @ref ymodem_adapter_start_sender()
 * 之前通过 @ref protocol_parser_set_callbacks() 设置好回调。
 *
 * 发送器已挂载到 @ref protocol_chain, 无需手动调用 parse_data。
 * 典型流程:
 * @code
 *   parser = ymodem_protocol_create_sender(rx_buf, 4, tx_buf, 1029);
 *   protocol_parser_set_callbacks(base, my_frame_ready, ctx, my_tx_ready, NULL);
 *   protocol_chain_add_parser(chain, (protocol_parser_t*)parser);
 *   // ---- 发送器 IDLE 静默, 等待触发 ----
 *   // 收到 Imperx "start ymodem send" 命令后:
 *   ymodem_adapter_start_sender(parser);
 *   protocol_chain_set_locked_parser(chain, (protocol_parser_t*)parser);
 *   // 后续数据由 protocol_chain_feed 自动转发到发送器
 * @endcode
 *
 * @param parser  适配器实例
 * @return  Ymodem 发送器指针, 失败返回 NULL
 */
ymodem_sender_t* ymodem_adapter_get_sender(ymodem_protocol_parser_t* parser);

/**
 * @brief  获取内部 Ymodem 接收器实例
 *
 * 适配器已预设 event_callback 和 send_response 桥接回调。
 * 用户应通过 @ref protocol_parser_set_callbacks() 设置 frame_ready 回调。
 *
 * 接收器创建后静默, 需通过 @ref ymodem_adapter_start_receiver() 激活:
 * @code
 *   recv = ymodem_adapter_get_receiver(parser);
 *   ymodem_receiver_set_event_callback(recv, my_evt_cb, ctx);
 *   // ---- 接收器静默, 等待触发 ----
 *   // 收到 Imperx "start ymodem recv" 命令后:
 *   ymodem_adapter_start_receiver(parser);
 *   // 传入数据路径: protocol_chain_feed → parse_data → ymodem_receiver_parse
 * @endcode
 *
 * @param parser  适配器实例
 * @return  Ymodem 接收器指针, 失败返回 NULL
 */
ymodem_receiver_parser_t* ymodem_adapter_get_receiver(ymodem_protocol_parser_t* parser);

/**
 * @brief  启动 Ymodem 接收握手
 *
 * 发送初始 'C' 字节请求 CRC16 校验模式, 通过 tx_ready 回调输出。
 * 仅在 @c YMODEM_STAGE_IDLE 状态下有效。
 *
 * @param parser  适配器实例
 * @retval true  启动成功
 * @retval false parser 为 NULL
 */
bool ymodem_adapter_start_receiver(ymodem_protocol_parser_t* parser);

/**
 * @brief  启动 Ymodem 发送握手
 *
 * 切换发送器到 @c YMODEM_STAGE_ESTABLISHING 阶段,
 * 收到对方 'C' 后通过 frame_ready 回调请求文件信息。
 *
 * @param parser  适配器实例
 * @retval true  启动成功
 * @retval false parser 为 NULL
 */
bool ymodem_adapter_start_sender(ymodem_protocol_parser_t* parser);
