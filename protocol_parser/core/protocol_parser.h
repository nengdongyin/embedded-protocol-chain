/**
 * @file    protocol_parser.h
 * @brief   协议解析器基类接口定义
 *
 * 采用C语言面向对象设计:
 * - 基类定义通用框架, 协议特定类型由子类定义
 * - 通过虚函数表 @ref protocol_parser_ops 实现多态
 * - parsed_result 为 void* 泛型指针, 指向子类自己的解析结果
 *
 * @author  Protocol Parser Framework
 * @version 3.0
 * @date    2024
 */
#ifndef PROTOCOL_PARSER_H
#define PROTOCOL_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "system_adapter.h"

/* ========================== 通用宏定义 ============================== */

/** @brief 默认最大帧长度(字节) */
#define DEFAULT_MAX_FRAME_LENGTH  1024

/** @brief 默认超时时间(毫秒) */
#define DEFAULT_TIMEOUT_MS        1000

/** @brief 最小有效帧长度(字节) */
#define MIN_FRAME_LENGTH          4

/* ======================== 错误类型枚举 ============================== */

/**
 * @enum   parser_error_t
 * @brief  解析器通用错误码
 *
 * 协议特定的帧格式错误(帧头/帧尾/校验等)由子类定义自己的错误码,
 * 通过 @ref parser_error_map() 映射到 @ref PARSER_ERR_FRAME.
 */
typedef enum {
    PARSER_ERR_NONE = 0,             /**< 无错误 */
    PARSER_ERR_INCOMPLETE,           /**< 数据不完整, 需要更多数据 */
    PARSER_ERR_FRAME,                /**< 通用帧格式错误(帧头/帧尾/校验/长度等) */
    PARSER_ERR_TIMEOUT,              /**< 帧接收超时 */
    PARSER_ERR_INVALID_PARAM,        /**< 无效参数 */
    PARSER_ERR_UNKNOWN,              /**< 未知/未分类错误 */
} parser_error_t;

// 单条映射
typedef struct {
    int specific_error;
    parser_error_t generic;
} ErrorMapEntry;

// 映射器实例（每个协议一个）
typedef struct {
    const ErrorMapEntry *table;
    size_t count;
} ErrorMapper;

/* ==================== 错误映射辅助函数 ============================== */

/**
 * @brief  通过映射表将协议特定错误码转换为通用错误码(表驱动)
 *
 * 基类只认 @ref ErrorMapper, 不感知具体协议.
 * 子类在初始化时将自己的映射表注入到映射器实例中.
 *
 * @param mapper  映射器实例指针
 * @param err     待映射的错误码
 * @return        通用错误码; 若表中无匹配项则原样返回
 */
parser_error_t parser_error_map(const ErrorMapper* mapper, int err);

/**
 * @brief  判断错误是否为致命性
 *
 * 致命错误(除 NONE / INCOMPLETE 之外的任何错误)需要协议链
 * 解锁当前解析器并重新匹配.
 *
 * @param err  待判断的错误码
 * @retval true   致命 – 解析器需要重置/解锁
 * @retval false  非致命 – 可以继续
 */
bool parser_error_is_fatal(parser_error_t err);

/* ======================== 配置参数结构 ============================== */

/**
 * @struct parser_config_t
 * @brief  解析器配置参数
 */
typedef struct {
    uint16_t max_frame_len;     /**< 最大帧长度, 包含帧头/帧尾/校验等 */
    uint32_t timeout_ms;        /**< 超时阈值(毫秒), 0=禁用 */
} parser_config_t;

/**
 * @brief  获取默认配置
 * @return 包含合理默认值的配置
 */
parser_config_t get_default_config(void);

/* ======================== 统计信息结构 ============================== */

/**
 * @struct parser_stats_t
 * @brief  解析器通用统计信息
 *
 * 仅记录与协议无关的通用统计项.
 * 协议特定错误(帧头/校验/帧过长等)由各子类自行维护.
 */
typedef struct {
    uint32_t frames_received;       /**< 成功接收的帧数 */
    uint32_t frames_encoded;        /**< 成功编码的帧数 */
    uint32_t timeout_errors;        /**< 超时错误次数 */
    uint32_t invalid_param_errors;  /**< 无效参数错误次数 */
    uint32_t frame_errors;          /**< 帧格式错误次数(通用+子类特定) */
    uint32_t other_errors;          /**< 其他/未分类错误次数 */
} parser_stats_t;

/* ======================== 收发缓冲区结构 ============================ */

/**
 * @struct parser_rx_buffer_t
 * @brief  接收缓冲区管理结构
 */
typedef struct {
    uint8_t*  buffer;      /**< 缓冲区指针(外部提供或动态分配) */
    uint32_t  size;        /**< 缓冲区总容量 */
    uint32_t  data_len;    /**< 当前已缓存的数据长度 */
    bool      own_buffer;  /**< 是否由解析器拥有(自动释放) */
} parser_rx_buffer_t;

/**
 * @struct parser_tx_buffer_t
 * @brief  发送缓冲区管理结构
 */
typedef struct {
    uint8_t*  buffer;      /**< 发送缓冲区指针(外部提供或动态分配) */
    uint32_t  size;        /**< 缓冲区容量 */
    uint32_t  data_len;    /**< 待发送数据长度 */
    bool      own_buffer;  /**< 是否由解析器拥有(自动释放) */
} parser_tx_buffer_t;

/* ======================== 回调函数类型定义 ========================== */

/** @brief 解析器结构前置声明 */
typedef struct protocol_parser protocol_parser_t;

/**
 * @typedef on_frame_ready_t
 * @brief  帧解析完成回调函数类型
 *
 * 用户可在回调中填充子类解析结果数据用于响应编码.
 * 编码和发送由框架自动处理, 用户不得在此回调中操作 tx_buffer.
 *
 * @param parser       解析器实例
 * @param parsed_data  指向子类解析结果的 void* 指针
 * @param user_ctx     用户上下文指针
 */
typedef void (*on_frame_ready_t)(
    protocol_parser_t* parser,
    void* parsed_data,
    void* user_ctx
    );

/**
 * @typedef on_tx_ready_t
 * @brief  发送数据就绪回调函数类型
 *
 * 当 tx_buffer 中有数据需要发送到通信接口(UART/SPI/以太网等)时调用.
 * 在以下场景触发:
 * - 帧解析完成后自动编码的应答数据
 * - 帧解析出错时协议生成的错误应答数据
 *
 * @param parser   解析器实例
 * @param user_ctx 用户上下文指针
 */
typedef void (*on_tx_ready_t)(
    protocol_parser_t* parser,
    void* user_ctx
    );

/**
 * @struct parser_callbacks_t
 * @brief  解析器回调函数集合
 */
typedef struct {
    on_frame_ready_t frame_ready;      /**< 帧完成回调 */
    void*            frame_ready_ctx;  /**< 帧完成回调用户上下文 */
    on_tx_ready_t    tx_ready;         /**< 发送就绪回调 */
    void*            tx_ready_ctx;     /**< 发送就绪回调用户上下文 */
} parser_callbacks_t;

/* ======================== 超时管理器 ================================ */

/**
 * @struct parser_timeout_t
 * @brief  超时管理器状态
 */
typedef struct {
    uint32_t last_activity_ms;  /**< 最后活动时间戳(毫秒) */
    bool     is_active;         /**< 是否正在计时 */
} parser_timeout_t;

/* ======================== 虚函数表 ================================== */

/**
 * @struct protocol_parser_ops
 * @brief  协议解析器虚函数表
 *
 * 每个协议子类提供一个静态的虚函数表实例.
 */
struct protocol_parser_ops {
    /**
     * @brief  解析接收数据
     *
     * 返回 int 而非 parser_error_t, 因为子类返回的可能是
     * 超出通用枚举范围的协议特定错误码(如 CAMYU_ERR_BCC_CHECKSUM).
     * 调用方必须通过 @ref parser_error_map() 转换为通用错误码后再判断.
     *
     * @param parser  解析器实例
     * @param data    接收数据指针
     * @param len     数据长度
     * @return        原始错误码(子类特定或通用)
     */
    int (*parse_data)(protocol_parser_t* parser, const uint8_t* data, uint32_t len);

    /**
     * @brief  将响应数据编码到 tx_buffer
     *
     * @param parser  解析器实例
     * @param data    子类特定数据的 void* 指针(可为 NULL)
     * @return        编码后字节数, 失败返回 0
     */
    uint32_t (*encode)(protocol_parser_t* parser, const void* data);

    /**
     * @brief  将解析器状态重置为初始值
     * @param parser  解析器实例
     */
    void (*reset)(protocol_parser_t* parser);

    /**
     * @brief  释放协议特定资源
     * @param parser  解析器实例
     */
    void (*destroy)(protocol_parser_t* parser);

    /**
     * @brief  协议特定周期性维护(可为 NULL)
     *
     * 由 @ref protocol_chain_check_timeout_poll 在每次轮询时调用。
     * 用于驱动协议层超时重传/握手重试等逻辑, 与基类通用超时正交。
     * NULL 表示该协议无需周期性维护。
     *
     * @param parser  解析器实例
     */
    bool (*poll)(protocol_parser_t* parser);
};

/* ======================== 基类结构体 ================================ */

/**
 * @struct protocol_parser
 * @brief  协议解析器基类结构体
 *
 * 子类通过嵌入基类作为首成员来"继承":
 * @code
 *   typedef struct {
 *       protocol_parser_t base;   // 必须为第一个成员
 *       xxx_private_t pri;
 *   } xxx_protocol_parser_t;
 * @endcode
 *
 * @c parsed_result 是一个 void* 指针, 子类在调用
 * @ref protocol_parser_on_frame_ready() 之前将其指向私有解析结果存储.
 */
struct protocol_parser {
    parser_config_t                     config;          /**< 解析器配置 */
    parser_stats_t                      stats;           /**< 通用统计信息 */
    parser_rx_buffer_t                  rx;              /**< 接收缓冲区 */
    parser_tx_buffer_t                  tx;              /**< 发送缓冲区 */
    parser_callbacks_t                  callbacks;       /**< 回调函数集 */
    parser_timeout_t                    timeout;         /**< 超时管理器 */
    const struct protocol_parser_ops*   ops;             /**< 虚函数表指针 */
    void*                               parsed_result;   /**< 子类解析结果泛型指针 */
    bool                                locked;          /**< 是否已被协议链锁定 */
    ErrorMapper                         error_mapper;    /**< 错误码映射器(子类注入) */
};

/* ======================== 辅助函数声明 ============================== */

/**
 * @brief  验证配置参数有效性
 *
 * @param config  待验证的配置
 * @retval true   配置有效
 * @retval false  配置无效
 */
bool protocol_parser_validate_config(const parser_config_t* config);

/* ======================== 公共 API 接口 ============================= */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  设置解析器回调函数
 *
 * @param parser           解析器实例
 * @param frame_ready_cb   帧完成回调(可为 NULL)
 * @param frame_ready_ctx  帧完成回调用户上下文
 * @param tx_ready_cb      发送就绪回调(可为 NULL)
 * @param tx_ready_ctx     发送就绪回调用户上下文
 * @retval true  设置成功
 * @retval false 失败(parser 为 NULL)
 */
bool protocol_parser_set_callbacks(protocol_parser_t* parser,
                                   on_frame_ready_t frame_ready_cb,
                                   void* frame_ready_ctx,
                                   on_tx_ready_t tx_ready_cb,
                                   void* tx_ready_ctx);

/**
 * @brief  接收数据解析入口(公共 API)
 *
 * 更新超时时间戳后通过虚函数表分发到子类的 parse_data 实现.
 * 内部通过 @ref parser_error_map() 将子类错误码映射为通用错误码.
 *
 * @param parser  解析器实例
 * @param data    接收数据指针
 * @param len     数据长度
 * @return        通用错误码(已映射)
 */
parser_error_t protocol_parser_parse_data(protocol_parser_t* parser,
                                          const uint8_t* data, uint32_t len);

/**
 * @brief  编码数据到发送缓冲区
 *
 * 通过虚函数表分发到子类的 encode 实现.
 *
 * @param parser  解析器实例
 * @param data    待编码数据的 void* 指针(子类定义类型, 可为 NULL)
 * @return        编码后字节数, 失败返回 0
 */
uint32_t protocol_parser_encode(protocol_parser_t* parser, const void* data);

/**
 * @brief  轮询检查解析器是否超时
 *
 * 应在主循环中定期调用.
 * 如果检测到超时, 内部会触发 @ref protocol_parser_on_frame_error().
 *
 * @param parser  解析器实例
 * @retval true   超时(错误处理已在内部触发)
 * @retval false  未超时
 */
bool protocol_parser_check_timeout_poll(protocol_parser_t* parser);

/**
 * @brief  获取发送缓冲区数据指针
 *
 * @param parser  解析器实例
 * @param len     [out] 接收待发送数据长度
 * @return        tx_buffer 指针
 */
uint8_t* protocol_parser_get_tx_data(protocol_parser_t* parser, uint32_t* len);

/**
 * @brief  获取解析器统计信息
 *
 * @param parser  解析器实例
 * @param out     [out] 接收统计信息副本
 */
void protocol_parser_get_stats(const protocol_parser_t* parser, parser_stats_t* out);

/**
 * @brief  重置解析器统计信息为零
 * @param parser  解析器实例
 */
void protocol_parser_reset_stats(protocol_parser_t* parser);

/**
 * @brief  销毁解析器实例并释放所有资源
 *
 * 释放 rx/tx 缓冲区(如果由解析器拥有)并调用子类 destroy 钩子.
 *
 * @param parser  待销毁的解析器实例
 */
void protocol_parser_destroy(protocol_parser_t* parser);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_PARSER_H */
