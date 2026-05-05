/*
 * Copyright (c) 2024 Protocol Parser Framework
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file protocol_parser_test_zephyr.c
 * @brief Zephyr RTOS 责任链协议解析器上机测试
 *
 * 在同一 UART 物理链路上同时挂载四种协议解析器:
 *   - Imperx 寄存器访问协议 (读/写)
 *   - Camyu PPP 类协议 (读/写)
 *   - Ymodem 文件接收协议
 *   - Ymodem 文件发送协议
 *
 * 责任链自动匹配: 数据到达后依次尝试所有解析器,
 * 首个匹配成功的解析器被"锁定", 后续数据直接转发.
 *
 * Ymodem 启动方式:
 *   通过 Imperx WRITE 命令写入控制寄存器触发:
 *     - 0xF000: 启动 Ymodem 接收
 *     - 0xF001: 启动 Ymodem 发送
 *
 * 配置 (Kconfig 或编译宏):
 *   CONFIG_YMODEM_SENDER_FILE_PATH  - 发送文件路径 (默认: /lfs/test.bin)
 *   CONFIG_YMODEM_RECV_SAVE_DIR     - 接收保存目录 (默认: /lfs)
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>

#include "core/protocol_parser.h"
#include "core/protocol_chain.h"
#include "protocols/imperx/protocol_parser_imperx.h"
#include "protocols/camyu/protocol_parser_camyu.h"
#include "protocols/ymodem/adapter/protocol_parser_ymodem.h"

LOG_MODULE_REGISTER(chain_test, LOG_LEVEL_INF);

/* ================================================================
 *  编译期配置
 * ================================================================ */

#define TEST_BAUD_RATE           460800
#define STACK_SIZE               12288
#define THREAD_PRIORITY          7
#define PIPE_BUFFER_SIZE         2048
#define FILE_PATH_MAX            256

#define IMPERX_RX_BUF_SIZE       32
#define IMPERX_TX_BUF_SIZE       32
#define CAMYU_RX_BUF_SIZE        320
#define CAMYU_TX_BUF_SIZE        320
#define YMODEM_RECV_RX_BUF_SIZE  YMODEM_STX_FRAME_LEN_BYTE
#define YMODEM_RECV_TX_BUF_SIZE  4
#define YMODEM_SEND_RX_BUF_SIZE  4
#define YMODEM_SEND_TX_BUF_SIZE  YMODEM_STX_FRAME_LEN_BYTE
#define BLOCK_SIZE_1K            1024

/* Kconfig 覆盖 */
#ifndef CONFIG_YMODEM_SENDER_FILE_PATH
#define CONFIG_YMODEM_SENDER_FILE_PATH "/lfs/test.bin"
#endif
#ifndef CONFIG_YMODEM_RECV_SAVE_DIR
#define CONFIG_YMODEM_RECV_SAVE_DIR     "/lfs"
#endif

/* ================================================================
 *  Imperx 控制寄存器 (触发 Ymodem 启动)
 * ================================================================ */

#define IMPERX_CTRL_YMODEM_RECV_START  0xF000
#define IMPERX_CTRL_YMODEM_SEND_START  0xF001

/* ================================================================
 *  设备
 * ================================================================ */

#define UART1_NODE DT_NODELABEL(uart1)
const struct device *const uart1_dev = DEVICE_DT_GET(UART1_NODE);

/* ================================================================
 *  线程资源
 * ================================================================ */

K_THREAD_STACK_DEFINE(protocol_stack, STACK_SIZE);
static struct k_thread protocol_thread_data;

K_PIPE_DEFINE(uart_pipe, PIPE_BUFFER_SIZE, 1);

/* ================================================================
 *  平台时间
 * ================================================================ */

uint32_t system_get_time_ms(void)
{
    return k_uptime_get_32();
}

/* ================================================================
 *  文件 I/O 封装 (线程安全, 带错误处理和日志)
 * ================================================================ */

typedef enum {
    FILE_IO_OK = 0,
    FILE_IO_ERR_OPEN,
    FILE_IO_ERR_READ,
    FILE_IO_ERR_WRITE,
    FILE_IO_ERR_CLOSE,
    FILE_IO_ERR_SEEK,
    FILE_IO_ERR_READ_SIZE,
} file_io_result_t;

static int file_io_open(struct fs_file_t *f, const char *path, int flags)
{
    fs_file_t_init(f);
    int rc = fs_open(f, path, flags);
    if (rc != 0) {
        LOG_ERR("fs_open(%s) failed: %d", path, rc);
        return FILE_IO_ERR_OPEN;
    }
    LOG_INF("file opened: %s", path);
    return FILE_IO_OK;
}

static int file_io_read(struct fs_file_t *f, uint8_t *buf, uint32_t len,
                        uint32_t *out_read)
{
    ssize_t n = fs_read(f, buf, len);
    if (n < 0) {
        LOG_ERR("fs_read(%u) failed: %d", len, (int)n);
        return FILE_IO_ERR_READ;
    }
    *out_read = (uint32_t)n;
    if (*out_read != len) {
        LOG_WRN("fs_read partial: %u/%u bytes", *out_read, len);
    }
    return FILE_IO_OK;
}

static int file_io_write(struct fs_file_t *f, const uint8_t *data, uint32_t len)
{
    ssize_t n = fs_write(f, data, len);
    if (n < 0) {
        LOG_ERR("fs_write(%u) failed: %d", len, (int)n);
        return FILE_IO_ERR_WRITE;
    }
    if ((uint32_t)n != len) {
        LOG_ERR("fs_write partial: %d/%u bytes", (int)n, len);
        return FILE_IO_ERR_WRITE;
    }
    return FILE_IO_OK;
}

static int file_io_close(struct fs_file_t *f)
{
    int rc = fs_close(f);
    if (rc != 0) {
        LOG_ERR("fs_close failed: %d", rc);
        return FILE_IO_ERR_CLOSE;
    }
    return FILE_IO_OK;
}

static int file_io_seek(struct fs_file_t *f, uint32_t offset)
{
    int rc = fs_seek(f, offset, FS_SEEK_SET);
    if (rc != 0) {
        LOG_ERR("fs_seek(%u) failed: %d", offset, rc);
        return FILE_IO_ERR_SEEK;
    }
    return FILE_IO_OK;
}

static bool get_file_size(const char *path, uint32_t *out_size)
{
    struct fs_dirent entry;
    int rc = fs_stat(path, &entry);
    if (rc != 0) {
        LOG_ERR("fs_stat(%s) failed: %d", path, rc);
        return false;
    }
    if (entry.type != FS_DIR_ENTRY_FILE) {
        LOG_ERR("%s is not a regular file", path);
        return false;
    }
    *out_size = (uint32_t)entry.size;
    return true;
}

/* ================================================================
 *  测试上下文
 * ================================================================ */

typedef struct {
    /* 协议链实例 */
    protocol_chain *chain;

    /* 协议解析器实例 */
    imperx_protocol_parser_t  *imperx_parser;
    camyu_protocol_parser_t   *camyu_parser;
    ymodem_protocol_parser_t  *ymodem_recv_parser;
    ymodem_protocol_parser_t  *ymodem_send_parser;

    /* 缓冲区 */
    uint8_t imperx_rx_buf[IMPERX_RX_BUF_SIZE];
    uint8_t imperx_tx_buf[IMPERX_TX_BUF_SIZE];
    uint8_t camyu_rx_buf[CAMYU_RX_BUF_SIZE];
    uint8_t camyu_tx_buf[CAMYU_TX_BUF_SIZE];
    uint8_t ymodem_recv_rx_buf[YMODEM_RECV_RX_BUF_SIZE];
    uint8_t ymodem_recv_tx_buf[YMODEM_RECV_TX_BUF_SIZE];
    uint8_t ymodem_send_rx_buf[YMODEM_SEND_RX_BUF_SIZE];
    uint8_t ymodem_send_tx_buf[YMODEM_SEND_TX_BUF_SIZE];

    /* Ymodem 发送器状态 */
    struct fs_file_t send_file;
    bool             send_file_open;
    const char      *send_file_path;
    uint32_t         send_file_size;
    bool             send_prepared;
    uint32_t         send_sent_bytes;
    uint32_t         send_last_log_ms;

    /* Ymodem 接收器状态 */
    struct fs_file_t recv_file;
    bool             recv_file_open;
    const char      *recv_save_dir;
    uint32_t         recv_total_bytes;
    uint32_t         recv_last_log_ms;

    /* 统计 */
    uint32_t total_rx_bytes;
    uint32_t total_tx_bytes;
} test_ctx_t;

static test_ctx_t test_ctx;

/* ================================================================
 *  通用 TX 回调 (所有协议共用)
 * ================================================================ */

static void on_tx_ready(protocol_parser_t *parser, void *user_ctx)
{
    uint32_t len = parser->tx.data_len;
    for (uint32_t i = 0; i < len; i++) {
        uart_poll_out(uart1_dev, parser->tx.buffer[i]);
    }
    test_ctx.total_tx_bytes += len;
    LOG_DBG("TX: %u bytes", len);
}

/* ================================================================
 *  Imperx 回调
 * ================================================================ */

static void imperx_on_frame_ready(protocol_parser_t *parser,
                                  void *parsed_data, void *user_ctx)
{
    imperx_private_t *pri = (imperx_private_t *)parsed_data;
    imperx_cmd_type_t cmd = (pri->parsed_len == 0) ? IMPERX_CMD_READ
                                                    : IMPERX_CMD_WRITE;

    LOG_INF("Imperx %s, Address: 0x%04llX",
            (cmd == IMPERX_CMD_READ) ? "READ" : "WRITE",
            (unsigned long long)pri->parsed_id);

    /* ---- Ymodem 控制寄存器 ---- */
    if (cmd == IMPERX_CMD_WRITE) {
        switch (pri->parsed_id) {
        case IMPERX_CTRL_YMODEM_RECV_START:
            LOG_INF("Triggering Ymodem receiver start...");
            if (test_ctx.ymodem_recv_parser) {
                ymodem_adapter_start_receiver(test_ctx.ymodem_recv_parser);
                protocol_chain_set_locked_parser(test_ctx.chain,
                    (protocol_parser_t *)test_ctx.ymodem_recv_parser);
            }
            break;

        case IMPERX_CTRL_YMODEM_SEND_START:
            LOG_INF("Triggering Ymodem sender start...");
            if (test_ctx.send_prepared) {
                ymodem_adapter_start_sender(test_ctx.ymodem_send_parser);
                protocol_chain_set_locked_parser(test_ctx.chain,
                    (protocol_parser_t *)test_ctx.ymodem_send_parser);
            } else {
                LOG_WRN("Ymodem sender not prepared (file not available)");
            }
            break;

        default:
            break;
        }
    }

    /* 读命令: 填充固定应答数据 */
    if (cmd == IMPERX_CMD_READ) {
        static uint8_t s_resp[4] = { 0x12, 0x34, 0x56, 0x78 };
        pri->parsed_data = s_resp;
        pri->parsed_len  = sizeof(s_resp);
        LOG_DBG("Imperx responding: %02X %02X %02X %02X",
                s_resp[0], s_resp[1], s_resp[2], s_resp[3]);
    }
}

/* ================================================================
 *  Camyu 回调
 * ================================================================ */

static void camyu_on_frame_ready(protocol_parser_t *parser,
                                 void *parsed_data, void *user_ctx)
{
    camyu_private_t *pri = (camyu_private_t *)parsed_data;

    LOG_INF("Camyu Opcode:%d, Address: 0x%08llX",
            pri->current_frame_info.opcode,
            (unsigned long long)pri->parsed_id);

    /* 读命令: 填充固定应答数据 */
    if (pri->current_frame_info.opcode == PCTOCAMERA_READ) {
        static uint8_t s_resp[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        pri->parsed_data     = s_resp;
        pri->parsed_data_len = sizeof(s_resp);
    }
}

/* ================================================================
 *  Ymodem 接收器回调
 * ================================================================ */

static void ymodem_recv_on_frame_ready(protocol_parser_t *parser,
                                       void *parsed_data, void *user_ctx)
{
    ymodem_receiver_event_t *event = (ymodem_receiver_event_t *)parsed_data;

    if (!event) {
        return;
    }

    char full_path[FILE_PATH_MAX];

    switch (event->type) {

    case YMODEM_RECV_EVENT_FILE_INFO:
        if (snprintf(full_path, sizeof(full_path), "%s/%s",
                     test_ctx.recv_save_dir, event->file_name)
            >= (int)sizeof(full_path)) {
            LOG_ERR("Ymodem receive: file path too long");
            return;
        }

        if (file_io_open(&test_ctx.recv_file, full_path,
                         FS_O_CREATE | FS_O_RDWR) != FILE_IO_OK) {
            LOG_ERR("Ymodem receive: cannot create %s", full_path);
            return;
        }
        test_ctx.recv_file_open = true;

        LOG_INF("Ymodem recv FILE_INFO: name=\"%s\" size=%u -> %s",
                event->file_name, event->file_size, full_path);
        break;

    case YMODEM_RECV_EVENT_DATA_PACKET:
        if (!test_ctx.recv_file_open) {
            LOG_ERR("Ymodem recv: DATA_PACKET but no file open");
            return;
        }

        if (file_io_write(&test_ctx.recv_file, event->data,
                          event->data_len) != FILE_IO_OK) {
            LOG_ERR("Ymodem recv: write failed at seq=%u", event->data_seq);
            file_io_close(&test_ctx.recv_file);
            test_ctx.recv_file_open = false;
            return;
        }

        test_ctx.recv_total_bytes = event->total_received;

        uint32_t now = k_uptime_get_32();
        if (event->file_size > 0 && now - test_ctx.recv_last_log_ms > 2000) {
            test_ctx.recv_last_log_ms = now;
            LOG_INF("Ymodem recv progress: %u/%u bytes (%u%%)",
                    event->total_received, event->file_size,
                    (uint32_t)(100ULL * event->total_received / event->file_size));
        } else {
            LOG_DBG("Ymodem recv: seq=%u len=%u total=%u",
                    event->data_seq, event->data_len, event->total_received);
        }
        break;

    case YMODEM_RECV_EVENT_TRANSFER_COMPLETE:
        LOG_INF("Ymodem recv: TRANSFER_COMPLETE (%u bytes total)",
                event->total_received);
        if (test_ctx.recv_file_open) {
            file_io_close(&test_ctx.recv_file);
            test_ctx.recv_file_open = false;
        }
        break;

    case YMODEM_RECV_EVENT_TRANSFER_FINISHED:
        LOG_INF("Ymodem recv: session finished");
        break;

    case YMODEM_RECV_EVENT_ERROR:
        LOG_ERR("Ymodem recv: transfer aborted");
        if (test_ctx.recv_file_open) {
            file_io_close(&test_ctx.recv_file);
            test_ctx.recv_file_open = false;
        }
        break;

    default:
        break;
    }
}

/* ================================================================
 *  Ymodem 发送器回调
 * ================================================================ */

static void ymodem_send_on_frame_ready(protocol_parser_t *parser,
                                        void *parsed_data, void *user_ctx)
{
    ymodem_sender_event_t *event = (ymodem_sender_event_t *)parsed_data;
    ymodem_sender_t *send;
    int rc;
    uint32_t n;

    switch (event->type) {

    case YMODEM_SENDER_EVENT_FILE_INFO:
        send = ymodem_adapter_get_sender((ymodem_protocol_parser_t *)parser);

        if (event->file_index != 0) {
            if (test_ctx.send_file_open) {
                file_io_close(&test_ctx.send_file);
                test_ctx.send_file_open = false;
            }
            send->file_info.file_name[0] = '\0';
            send->file_info.file_total_size = 0;
            LOG_INF("Ymodem send: no more files, ending session");
        } else {
            if (test_ctx.send_file_open) {
                file_io_close(&test_ctx.send_file);
                test_ctx.send_file_open = false;
            }

            rc = file_io_open(&test_ctx.send_file,
                              test_ctx.send_file_path, FS_O_READ);
            if (rc != FILE_IO_OK) {
                LOG_ERR("Ymodem send: cannot open source file, aborting");
                return;
            }
            test_ctx.send_file_open = true;
            test_ctx.send_sent_bytes = 0;

            const char *name = strrchr(test_ctx.send_file_path, '/');
            name = name ? name + 1 : test_ctx.send_file_path;
            strncpy(send->file_info.file_name, name,
                    sizeof(send->file_info.file_name) - 1);
            send->file_info.file_total_size = test_ctx.send_file_size;

            LOG_INF("Ymodem send: FILE_INFO idx=%u name=\"%s\" size=%u",
                    event->file_index, send->file_info.file_name,
                    test_ctx.send_file_size);
        }
        break;

    case YMODEM_SENDER_EVENT_DATA_PACKET: {
        if (!test_ctx.send_file_open) {
            LOG_ERR("Ymodem send: DATA_PACKET but file not open");
            return;
        }

        uint32_t offset = event->data_seq * BLOCK_SIZE_1K;
        uint32_t remaining = test_ctx.send_file_size - offset;
        uint32_t to_read = (remaining < BLOCK_SIZE_1K) ? remaining : BLOCK_SIZE_1K;

        uint32_t n = 0;
        rc = file_io_read(&test_ctx.send_file, event->data, to_read, &n);
        if (rc != FILE_IO_OK) {
            LOG_ERR("Ymodem send: read failed at offset %u", offset);
            return;
        }

        event->data_len = n;
        test_ctx.send_sent_bytes += n;

        uint32_t now = k_uptime_get_32();
        if (now - test_ctx.send_last_log_ms > 2000) {
            test_ctx.send_last_log_ms = now;
            LOG_INF("Ymodem send progress: %u/%u bytes (%u%%)",
                    test_ctx.send_sent_bytes, test_ctx.send_file_size,
                    (uint32_t)(100ULL * test_ctx.send_sent_bytes
                               / test_ctx.send_file_size));
        } else {
            LOG_DBG("Ymodem send: seq=%u off=%u len=%u/%u sent=%u/%u",
                    event->data_seq, offset, n, to_read,
                    test_ctx.send_sent_bytes, test_ctx.send_file_size);
        }
        break;
    }

    case YMODEM_SENDER_EVENT_TRANSFER_COMPLETE:
        LOG_INF("Ymodem send: TRANSFER_COMPLETE (sent %u bytes)",
                test_ctx.send_sent_bytes);
        if (test_ctx.send_file_open) {
            file_io_close(&test_ctx.send_file);
            test_ctx.send_file_open = false;
        }
        break;

    case YMODEM_SENDER_EVENT_SESSION_FINISHED:
        LOG_INF("Ymodem send: SESSION_FINISHED");
        break;

    case YMODEM_SENDER_EVENT_ERROR:
        LOG_ERR("Ymodem send: transfer aborted (sent %u/%u bytes)",
                test_ctx.send_sent_bytes, test_ctx.send_file_size);
        if (test_ctx.send_file_open) {
            file_io_close(&test_ctx.send_file);
            test_ctx.send_file_open = false;
        }
        break;

    default:
        break;
    }
}

/* ================================================================
 *  初始化协议链
 * ================================================================ */

static bool init_protocol_chain(test_ctx_t *ctx)
{
    /* 创建协议链 (容纳 4 个解析器) */
    ctx->chain = protocol_chain_create(4);
    if (!ctx->chain) {
        LOG_ERR("Failed to create protocol chain");
        return false;
    }

    /* 1. Imperx 解析器 */
    ctx->imperx_parser = imperx_protocol_create(
        ctx->imperx_rx_buf, sizeof(ctx->imperx_rx_buf),
        ctx->imperx_tx_buf, sizeof(ctx->imperx_tx_buf));
    if (!ctx->imperx_parser) {
        LOG_ERR("Failed to create Imperx parser");
        goto fail_imperx;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->imperx_parser,
                                  imperx_on_frame_ready, ctx,
                                  on_tx_ready, NULL);

    /* 2. Camyu 解析器 */
    ctx->camyu_parser = camyu_protocol_create(
        ctx->camyu_rx_buf, sizeof(ctx->camyu_rx_buf),
        ctx->camyu_tx_buf, sizeof(ctx->camyu_tx_buf));
    if (!ctx->camyu_parser) {
        LOG_ERR("Failed to create Camyu parser");
        goto fail_camyu;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->camyu_parser,
                                  camyu_on_frame_ready, ctx,
                                  on_tx_ready, NULL);

    /* 3. Ymodem 接收器 (创建后静默, 待 Imperx 控制寄存器触发) */
    ctx->ymodem_recv_parser = ymodem_protocol_create_receiver(
        ctx->ymodem_recv_rx_buf, sizeof(ctx->ymodem_recv_rx_buf),
        ctx->ymodem_recv_tx_buf, sizeof(ctx->ymodem_recv_tx_buf));
    if (!ctx->ymodem_recv_parser) {
        LOG_ERR("Failed to create Ymodem receiver parser");
        goto fail_ymodem_recv;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->ymodem_recv_parser,
                                  ymodem_recv_on_frame_ready, ctx,
                                  on_tx_ready, NULL);

    /* 4. Ymodem 发送器 (创建后静默, 待 Imperx 控制寄存器触发) */
    ctx->ymodem_send_parser = ymodem_protocol_create_sender(
        ctx->ymodem_send_rx_buf, sizeof(ctx->ymodem_send_rx_buf),
        ctx->ymodem_send_tx_buf, sizeof(ctx->ymodem_send_tx_buf));
    if (!ctx->ymodem_send_parser) {
        LOG_ERR("Failed to create Ymodem sender parser");
        goto fail_ymodem_send;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->ymodem_send_parser,
                                  ymodem_send_on_frame_ready, ctx,
                                  on_tx_ready, NULL);

    /* 按优先级加入协议链:
     * Ymodem 接收/发送优先, 确保被 Imperx 控制寄存器触发后
     * 数据能正确路由到已在通信中的 Ymodem 解析器.
     * IDLE 阶段的 Ymodem 解析器不会误匹配普通数据帧. */
    protocol_chain_add_parser(ctx->chain,
        (protocol_parser_t *)ctx->ymodem_recv_parser);
    protocol_chain_add_parser(ctx->chain,
        (protocol_parser_t *)ctx->ymodem_send_parser);
    protocol_chain_add_parser(ctx->chain,
        (protocol_parser_t *)ctx->imperx_parser);
    protocol_chain_add_parser(ctx->chain,
        (protocol_parser_t *)ctx->camyu_parser);

    LOG_INF("Protocol chain initialized: Ymodem(Rx), Ymodem(Tx), Imperx, Camyu");
    return true;

fail_ymodem_send:
    protocol_parser_destroy((protocol_parser_t *)ctx->ymodem_recv_parser);
    ctx->ymodem_recv_parser = NULL;
fail_ymodem_recv:
    protocol_parser_destroy((protocol_parser_t *)ctx->camyu_parser);
    ctx->camyu_parser = NULL;
fail_camyu:
    protocol_parser_destroy((protocol_parser_t *)ctx->imperx_parser);
    ctx->imperx_parser = NULL;
fail_imperx:
    protocol_chain_destroy(ctx->chain);
    ctx->chain = NULL;
    return false;
}

/* ================================================================
 *  资源清理
 * ================================================================ */

static void cleanup_resources(test_ctx_t *ctx)
{
    if (ctx->chain) {
        protocol_chain_destroy(ctx->chain);
        ctx->chain = NULL;
    }
    if (ctx->imperx_parser) {
        protocol_parser_destroy((protocol_parser_t *)ctx->imperx_parser);
        ctx->imperx_parser = NULL;
    }
    if (ctx->camyu_parser) {
        protocol_parser_destroy((protocol_parser_t *)ctx->camyu_parser);
        ctx->camyu_parser = NULL;
    }
    if (ctx->ymodem_recv_parser) {
        protocol_parser_destroy((protocol_parser_t *)ctx->ymodem_recv_parser);
        ctx->ymodem_recv_parser = NULL;
    }
    if (ctx->ymodem_send_parser) {
        protocol_parser_destroy((protocol_parser_t *)ctx->ymodem_send_parser);
        ctx->ymodem_send_parser = NULL;
    }
    if (ctx->send_file_open) {
        file_io_close(&ctx->send_file);
        ctx->send_file_open = false;
    }
    if (ctx->recv_file_open) {
        file_io_close(&ctx->recv_file);
        ctx->recv_file_open = false;
    }
}

/* ================================================================
 *  UART 中断处理
 * ================================================================ */

static void uart1_irq_callback(const struct device *dev, void *user_data)
{
    if (!uart_irq_update(dev)) {
        return;
    }

    if (uart_irq_rx_ready(dev)) {
        uint8_t rx_buf[YMODEM_STX_FRAME_LEN_BYTE];
        int bytes_read = uart_fifo_read(dev, rx_buf, sizeof(rx_buf));
        if (bytes_read > 0) {
            k_pipe_write(&uart_pipe, rx_buf, (size_t)bytes_read, K_NO_WAIT);
        }
    }

    if (uart_irq_tx_ready(dev)) {
        /* 保留给中断驱动 TX */
    }
}

/* ================================================================
 *  UART 配置
 * ================================================================ */

static void set_baud_rate(const struct device *uart_dev, uint32_t new_baud)
{
    struct uart_config cfg;
    int ret;

    ret = uart_config_get(uart_dev, &cfg);
    if (ret != 0) {
        printk("Error: uart_config_get() failed (%d)\n", ret);
        return;
    }
    cfg.baudrate = new_baud;

    ret = uart_configure(uart_dev, &cfg);
    if (ret == 0) {
        printk("UART baudrate set to %u\n", new_baud);
    } else {
        printk("Error: uart_configure() failed (%d)\n", ret);
    }
}

/* ================================================================
 *  Ymodem 发送准备
 * ================================================================ */

static bool prepare_ymodem_send(test_ctx_t *ctx)
{
    if (!ctx->send_file_path || ctx->send_file_path[0] == '\0') {
        LOG_WRN("No Ymodem sender file path configured");
        return false;
    }

    if (!get_file_size(ctx->send_file_path, &ctx->send_file_size)) {
        LOG_ERR("Cannot access sender file: %s", ctx->send_file_path);
        return false;
    }

    if (ctx->send_file_size == 0) {
        LOG_ERR("Sender file is empty: %s", ctx->send_file_path);
        return false;
    }

    ymodem_sender_t *sender = ymodem_adapter_get_sender(ctx->ymodem_send_parser);
    ymodem_sender_enable_1k(sender);

    ctx->send_prepared = true;

    LOG_INF("Ymodem sender prepared (1K mode), file: %s (%u bytes)",
            ctx->send_file_path, ctx->send_file_size);
    LOG_INF("Send Imperx WRITE 0xF001 to start Ymodem send");

    return true;
}

/* ================================================================
 *  协议处理线程
 *
 *  从 UART 管道读取数据, 送入协议链责任链.
 *  责任链自动匹配协议: Imperx / Camyu / Ymodem(Rx) / Ymodem(Tx).
 *  定期轮询超时.
 * ================================================================ */

void protocol_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    test_ctx_t *ctx = &test_ctx;

    /* 初始化上下文 */
    memset(ctx, 0, sizeof(test_ctx_t));
    ctx->send_file_path = CONFIG_YMODEM_SENDER_FILE_PATH;
    ctx->recv_save_dir   = CONFIG_YMODEM_RECV_SAVE_DIR;

    /* 初始化协议链 */
    if (!init_protocol_chain(ctx)) {
        LOG_ERR("Protocol chain init failed, thread exiting");
        return;
    }

    /* 准备 Ymodem 发送器 */
    prepare_ymodem_send(ctx);

    LOG_INF("===== Protocol Chain Test Started =====");
    LOG_INF("  Imperx:     ready (send hex frames to test)");
    LOG_INF("  Camyu:      ready (send hex frames to test)");
    LOG_INF("  Ymodem Rx:  idle (start via Imperx WRITE 0xF000)");
    LOG_INF("  Ymodem Tx:  %s (start via Imperx WRITE 0xF001)",
            ctx->send_prepared ? "prepared" : "not prepared");
    LOG_INF("=========================================");

    uint8_t data_buf[YMODEM_STX_FRAME_LEN_BYTE];
    protocol_parser_t *locked_parser;
    uint32_t last_status_ms = k_uptime_get_32();

    while (1) {
        locked_parser = protocol_chain_get_locked_parser(ctx->chain);
        bool is_recv_locked = (locked_parser ==
            (protocol_parser_t *)ctx->ymodem_recv_parser);

        int n;
        if (is_recv_locked) {
            n = k_pipe_read(&uart_pipe, data_buf,
                            sizeof(data_buf), K_MSEC(100));
        } else {
            n = k_pipe_read(&uart_pipe, data_buf,
                            1, K_MSEC(100));
        }

        if (n > 0) {
            ctx->total_rx_bytes += (uint32_t)n;
            (void)protocol_chain_feed(ctx->chain, data_buf, (uint32_t)n);
        } else {
            protocol_chain_check_timeout_poll(ctx->chain);
        }

        uint32_t now = k_uptime_get_32();
        if (now - last_status_ms >= 5000) {
            last_status_ms = now;
            LOG_INF("Status: rx=%u tx=%u locked=%s",
                    ctx->total_rx_bytes, ctx->total_tx_bytes,
                    locked_parser ? "yes" : "no");
        }
    }
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(void)
{
    printk("=== Protocol Chain Test (Zephyr) ===\n");
    printk("  Protocols: Imperx, Camyu, Ymodem(Rx), Ymodem(Tx)\n");
    printk("  Config:\n");
    printk("    Sender file: %s\n", CONFIG_YMODEM_SENDER_FILE_PATH);
    printk("    Recv dir:    %s\n", CONFIG_YMODEM_RECV_SAVE_DIR);
    printk("\n");

    if (!device_is_ready(uart1_dev)) {
        printk("Error: UART1 device not ready\n");
        return -1;
    }

    set_baud_rate(uart1_dev, TEST_BAUD_RATE);

    uart_irq_callback_user_data_set(uart1_dev, uart1_irq_callback, NULL);
    uart_irq_rx_enable(uart1_dev);

    k_thread_create(&protocol_thread_data, protocol_stack,
                    K_THREAD_STACK_SIZEOF(protocol_stack),
                    protocol_thread_entry, NULL, NULL, NULL,
                    THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&protocol_thread_data, "protocol_chain");

    while (1) {
        k_msleep(5000);
    }
    return 0;
}
