/**
 * @file  protocol_parser_test_windows.c
 * @brief Protocol Parser Chain Integration Test for Windows
 * 
 * Test protocol chain functionality through physical serial port on Windows, supports:
 * - Imperx register access protocol
 * - Camyu PPP-like protocol
 * - Ymodem file receive protocol
 * - Ymodem file send protocol
 * 
 * Usage:
 *   1. Install virtual serial port software (com0com / Virtual Serial Port Emulator)
 *   2. Create virtual serial pair (e.g., COM15 <-> COM16)
 *   3. Open terminal connected to one serial port
 *   4. Run test program connected to the other serial port
 * 
 * Compilation command (MinGW):
 *   gcc -Icore -Iprotocols/imperx -Iprotocols/camyu -Iprotocols/ymodem/src -Iprotocols/ymodem/adapter \
 *       protocol_parser_test_windows.c \
 *       core/protocol_parser.c core/protocol_chain.c core/protocol_parser_internal.c \
 *       protocols/imperx/protocol_parser_imperx.c \
 *       protocols/camyu/protocol_parser_camyu.c \
 *       protocols/ymodem/src/ymodem_common.c protocols/ymodem/src/ymodem_receiver.c \
 *       protocols/ymodem/src/ymodem_sender.c protocols/ymodem/adapter/protocol_parser_ymodem.c \
 *       -o protocol_parser_test_windows.exe -lws2_32
 * 
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Framework core headers */
#include "../core/protocol_parser.h"
#include "../core/protocol_chain.h"

/* Protocol headers */
#include "../protocols/imperx/protocol_parser_imperx.h"
#include "../protocols/camyu/protocol_parser_camyu.h"
#include "../protocols/ymodem/adapter/protocol_parser_ymodem.h"

/* ================================================================
 *  Serial port default parameters
 * ================================================================ */

#define TEST_SERIAL_BAUD_RATE   CBR_115200
#define TEST_SERIAL_BYTE_SIZE   8
#define TEST_SERIAL_PARITY      NOPARITY
#define TEST_SERIAL_STOP_BITS   ONESTOPBIT
#define TEST_SERIAL_READ_TO_MS  50
#define TEST_POLL_INTERVAL_MS   10
#define TEST_YMODEM_TIMEOUT_MS  30000

/* Imperx control register addresses (WRITE triggers Ymodem start) */
#define IMPERX_CTRL_YMODEM_RECV_START  0xF000
#define IMPERX_CTRL_YMODEM_SEND_START  0xF001

/* ================================================================
 *  Test context
 * ================================================================ */

typedef struct {
    HANDLE    h_com;
    bool      verbose;
    bool      running;
    
    /* Ymodem send related */
    FILE*     fp_send;
    const char* send_file_path;
    uint32_t  send_file_size;
    bool      send_1k_mode;
    bool      ymodem_send_prepared;
    bool      transfer_done;
    bool      transfer_error;

    /* Ymodem receive related */
    const char* recv_dir;
    FILE*     fp_recv;
    
    /* Protocol chain instance */
    protocol_chain* chain;
    
    /* Protocol parser instances */
    imperx_protocol_parser_t*   imperx_parser;
    camyu_protocol_parser_t*    camyu_parser;
    ymodem_protocol_parser_t*   ymodem_recv_parser;
    ymodem_protocol_parser_t*   ymodem_send_parser;

    /* Buffers */
    uint8_t   imperx_rx_buf[32];
    uint8_t   imperx_tx_buf[32];
    uint8_t   camyu_rx_buf[320];
    uint8_t   camyu_tx_buf[320];
    uint8_t   ymodem_rx_buf[YMODEM_STX_FRAME_LEN_BYTE];
    uint8_t   ymodem_tx_buf[YMODEM_STX_FRAME_LEN_BYTE];
    uint8_t   ymodem_send_rx_buf[4];
    uint8_t   ymodem_send_tx_buf[YMODEM_STX_FRAME_LEN_BYTE];
} test_ctx_t;

/* ================================================================
 *  System time implementation (Windows)
 * ================================================================ */

uint32_t system_get_time_ms(void) {
    return (uint32_t)GetTickCount64();
}

/* ================================================================
 *  Global variables (for signal handling)
 * ================================================================ */

static test_ctx_t* g_test_ctx = NULL;

/* ================================================================
 *  Signal handling (Windows)
 * ================================================================ */

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
        if (g_test_ctx) {
            printf("\n[INFO] Ctrl+C received, stopping...\n");
            g_test_ctx->running = false;
        }
        return TRUE;
    default:
        return FALSE;
    }
}

/* ================================================================
 *  Serial port operation functions
 * ================================================================ */

static HANDLE serial_open(const char* port_name) {
    char path[64];
    snprintf(path, sizeof(path), "\\\\.\\%s", port_name);

    HANDLE h = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[FATAL] Cannot open serial port %s (error: %lu)\n", 
                port_name, GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) {
        fprintf(stderr, "[FATAL] GetCommState failed\n");
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = TEST_SERIAL_BAUD_RATE;
    dcb.ByteSize = TEST_SERIAL_BYTE_SIZE;
    dcb.Parity   = TEST_SERIAL_PARITY;
    dcb.StopBits = TEST_SERIAL_STOP_BITS;
    dcb.fBinary  = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(h, &dcb)) {
        fprintf(stderr, "[FATAL] SetCommState failed\n");
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout         = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadTotalTimeoutConstant    = TEST_SERIAL_READ_TO_MS;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = 1000;

    if (!SetCommTimeouts(h, &timeouts)) {
        fprintf(stderr, "[FATAL] SetCommTimeouts failed\n");
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    printf("[SERIAL] %s opened (%d bps, %dN%d)\n",
           port_name,
           TEST_SERIAL_BAUD_RATE == CBR_115200 ? 115200 : 9600,
           TEST_SERIAL_BYTE_SIZE,
           TEST_SERIAL_STOP_BITS == ONESTOPBIT ? 1 : 2);

    return h;
}

static void serial_close(HANDLE h) {
    if (h != INVALID_HANDLE_VALUE) {
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        CloseHandle(h);
    }
}

static bool serial_write(HANDLE h, const uint8_t* data, uint32_t len) {
    DWORD written = 0;
    if (!WriteFile(h, data, len, &written, NULL) || written != len) {
        return false;
    }
    return true;
}

static int serial_read(HANDLE h, uint8_t* buf, uint32_t buf_size) {
    DWORD n = 0;
    if (!ReadFile(h, buf, buf_size, &n, NULL)) {
        return -1;
    }
    return (int)n;
}

/* ================================================================
 *  Helper functions
 * ================================================================ */

static void print_hex(const char* label, const uint8_t* data, uint32_t len) {
    printf("%s ", label);
    for (uint32_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("(%u bytes)\n", len);
}

static void print_usage(const char* prog) {
    printf("Protocol Parser Chain Test for Windows\n");
    printf("Usage: %s <COM port> [options]\n\n", prog);
    printf("Arguments:\n");
    printf("  <COM port>    Serial port name, e.g. COM15\n\n");
    printf("Options:\n");
    printf("  -v            Verbose mode\n");
    printf("  -s <file>     Prepare Ymodem sender with file (does NOT auto-start)\n");
    printf("  -o <dir>      Output directory for received Ymodem files\n");
    printf("  --1k          Use 1024-byte Ymodem frames (default 128)\n");
    printf("  -h            Show this help\n\n");
    printf("Examples:\n");
    printf("  %s COM15\n", prog);
    printf("  %s COM15 -v\n", prog);
    printf("  %s COM15 -s firmware.bin --1k\n", prog);
    printf("\n");
    printf("Test setup:\n");
    printf("  1. Install virtual serial port software (com0com, VSPE)\n");
    printf("  2. Create virtual serial pair: COMx <-> COMy\n");
    printf("  3. Open COMy with terminal (115200 8N1 HEX mode)\n");
    printf("  4. Run: %s COMx\n", prog);
    printf("\n");
    printf("Protocol test commands (send from terminal as HEX):\n");
    printf("  Imperx READ:   52 00 10        (Read register 0x0010)\n");
    printf("  Imperx WRITE:  57 00 10 12 34 56 78 (Write 0x12345678 to 0x0010)\n");
    printf("  Camyu WRITE:   7E 43 00 00 00 10 08 02\n");
    printf("\n");
    printf("Ymodem control (send Imperx WRITE to control registers):\n");
    printf("  Start receiver: 57 F0 00 XX XX XX XX (Write any to 0xF000)\n");
    printf("  Start sender:   57 F0 01 XX XX XX XX (Write any to 0xF001, needs -s)\n");
    printf("  Ymodem stays silent until explicitly started via these commands\n");
}

/* ================================================================
 *  Protocol callback functions
 * ================================================================ */

static void on_tx_ready(protocol_parser_t* parser, void* user_ctx) {
    test_ctx_t* ctx = (test_ctx_t*)user_ctx;
    uint32_t len = parser->tx.data_len;
    
    if (ctx->verbose) {
        printf("[TX] Protocol response: ");
        for (uint32_t i = 0; i < len && i < 16; i++) {
            printf("%02X ", parser->tx.buffer[i]);
        }
        if (len > 16) printf("...");
        printf("(%u bytes)\n", len);
    }
    
    if (!serial_write(ctx->h_com, parser->tx.buffer, len)) {
        fprintf(stderr, "[ERROR] Serial write failed\n");
    }
}

static void imperx_on_frame_ready(protocol_parser_t* parser,
                                  void* parsed_data, void* user_ctx) {
    test_ctx_t* ctx = (test_ctx_t*)user_ctx;
    imperx_private_t* pri = (imperx_private_t*)parsed_data;
    
    imperx_cmd_type_t cmd = (pri->parsed_len == 0) ? IMPERX_CMD_READ
                                                    : IMPERX_CMD_WRITE;

    printf("\n[FrameReady] Imperx %s, Address: 0x%04llX\n",
           (cmd == IMPERX_CMD_READ) ? "READ" : "WRITE",
           (unsigned long long)pri->parsed_id);

    /* ---- Ymodem control registers ---- */
    if (cmd == IMPERX_CMD_WRITE) {
        switch (pri->parsed_id) {
        case IMPERX_CTRL_YMODEM_RECV_START:
            printf("[CTRL] Triggering Ymodem receiver start...\n");
            if (ctx->ymodem_recv_parser) {
                ymodem_adapter_start_receiver(ctx->ymodem_recv_parser);
                protocol_chain_set_locked_parser(ctx->chain,
                    (protocol_parser_t*)ctx->ymodem_recv_parser);
            }
            break;

        case IMPERX_CTRL_YMODEM_SEND_START:
            printf("[CTRL] Triggering Ymodem sender start...\n");
            if (ctx->ymodem_send_prepared) {
                ymodem_adapter_start_sender(ctx->ymodem_send_parser);
                protocol_chain_set_locked_parser(ctx->chain,
                    (protocol_parser_t*)ctx->ymodem_send_parser);
            } else {
                printf("[CTRL] Ymodem sender not prepared, use -s <file> to prepare\n");
            }
            break;

        default:
            break;
        }
    }

    if (cmd == IMPERX_CMD_READ) {
        static uint8_t s_resp[4] = { 0x12, 0x34, 0x56, 0x78 };
        pri->parsed_data = s_resp;
        pri->parsed_len  = sizeof(s_resp);
        if (ctx->verbose) {
            printf("[IMPERX] Responding with: %02X %02X %02X %02X\n",
                   s_resp[0], s_resp[1], s_resp[2], s_resp[3]);
        }
    }
}

static void camyu_on_frame_ready(protocol_parser_t* parser,
                                 void* parsed_data, void* user_ctx) {
    camyu_private_t* pri = (camyu_private_t*)parsed_data;
    
    printf("\n[FrameReady] Camyu Opcode:%d, Address: 0x%08llX\n",
           pri->current_frame_info.opcode,
           (unsigned long long)pri->parsed_id);

    if (pri->current_frame_info.opcode == PCTOCAMERA_READ) {
        static uint8_t s_resp[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        pri->parsed_data     = s_resp;
        pri->parsed_data_len = sizeof(s_resp);
    }
}

/* Ymodem receiver event callback */
static void ymodem_recv_on_frame_ready(protocol_parser_t* parser,
                                       void* parsed_data, void* user_ctx) {
    test_ctx_t* ctx = (test_ctx_t*)user_ctx;
    ymodem_receiver_event_t* event = (ymodem_receiver_event_t*)parsed_data;
    
    if (!event) return;
    
    switch (event->type) {
    case YMODEM_RECV_EVENT_FILE_INFO: {
        printf("\n[FrameReady] Ymodem FILE_INFO - Name: %s, Size: %u bytes\n",
               event->file_name, event->file_size);

        if (ctx->recv_dir && event->file_name[0]) {
            char path[512];
            snprintf(path, sizeof(path), "%s\\%s", ctx->recv_dir, event->file_name);
            ctx->fp_recv = fopen(path, "wb");
            if (ctx->fp_recv) {
                printf("[YMODEM] Receiving to: %s\n", path);
            } else {
                fprintf(stderr, "[ERROR] Cannot create file: %s\n", path);
            }
        } else {
            printf("[YMODEM] No output dir, use -o <dir> to save files\n");
        }
        break;
    }

    case YMODEM_RECV_EVENT_DATA_PACKET:
        if (ctx->fp_recv && event->data && event->data_len > 0) {
            fwrite(event->data, 1, event->data_len, ctx->fp_recv);
        }
        if (ctx->verbose) {
            printf("[YMODEM] DATA Seq:%u Len:%u Total:%u/%u\n",
                   event->data_seq, event->data_len,
                   event->total_received, event->file_size);
        } else {
            uint32_t pct = event->file_size ? 
                (event->total_received * 100 / event->file_size) : 0;
            printf("\r[YMODEM] Receiving: %u / %u bytes (%u%%)",
                   event->total_received, event->file_size, pct);
            fflush(stdout);
        }
        break;
        
    case YMODEM_RECV_EVENT_TRANSFER_COMPLETE:
        printf("\n[YMODEM] TRANSFER_COMPLETE - Received: %u bytes\n",
               event->total_received);
        if (ctx->fp_recv) {
            fclose(ctx->fp_recv);
            ctx->fp_recv = NULL;
        }
        break;
        
    case YMODEM_RECV_EVENT_TRANSFER_FINISHED:
        printf("[YMODEM] TRANSFER_FINISHED\n");
        break;
        
    case YMODEM_RECV_EVENT_ERROR:
        printf("[YMODEM] ERROR\n");
        if (ctx->fp_recv) {
            fclose(ctx->fp_recv);
            ctx->fp_recv = NULL;
        }
        break;
        
    default:
        break;
    }
}

/* Ymodem sender frame_ready callback (via adapter bridge) */
static void ymodem_send_on_frame_ready(protocol_parser_t* parser,
                                        void* parsed_data, void* user_ctx) {
    test_ctx_t* ctx = (test_ctx_t*)user_ctx;
    ymodem_sender_event_t* event = (ymodem_sender_event_t*)parsed_data;
    
    switch (event->type) {
    case YMODEM_SENDER_EVENT_FILE_INFO: {
        if (event->file_index != 0) {
            ymodem_sender_t* send = ymodem_adapter_get_sender(
                (ymodem_protocol_parser_t*)parser);
            send->file_info.file_name[0] = '\0';
            send->file_info.file_total_size = 0;
            printf("[YMODEM] No more files, ending session\n");
        } else {
            const char* fname = strrchr(ctx->send_file_path, '\\');
            if (fname) fname++; else fname = ctx->send_file_path;
            ymodem_sender_t* send = ymodem_adapter_get_sender(
                (ymodem_protocol_parser_t*)parser);
            strncpy(send->file_info.file_name, fname,
                    sizeof(send->file_info.file_name) - 1);
            send->file_info.file_total_size = ctx->send_file_size;
            printf("[YMODEM] Sending file: %s (%u bytes)\n",
                   send->file_info.file_name, ctx->send_file_size);
        }
        break;
    }
    
    case YMODEM_SENDER_EVENT_DATA_PACKET: {
        uint32_t offset = event->data_seq * (ctx->send_1k_mode ? 1024 : 128);
        uint32_t remaining = ctx->send_file_size - offset;
        uint32_t to_read = (remaining < (ctx->send_1k_mode ? 1024 : 128))
                           ? remaining : (ctx->send_1k_mode ? 1024 : 128);

        if (fseek(ctx->fp_send, (long)offset, SEEK_SET) != 0) {
            fprintf(stderr, "[ERROR] fseek failed\n");
            ctx->transfer_error = true;
            return;
        }

        uint32_t n = (uint32_t)fread(event->data, 1, to_read, ctx->fp_send);
        event->data_len = n;

        printf("\r[YMODEM] Sending: %u / %u bytes (%.1f%%)",
               offset + n, ctx->send_file_size,
               100.0f * (offset + n) / ctx->send_file_size);
        fflush(stdout);
        break;
    }
    
    case YMODEM_SENDER_EVENT_TRANSFER_COMPLETE:
        printf("\n[YMODEM] Transfer complete\n");
        if (ctx->fp_send) {
            fclose(ctx->fp_send);
            ctx->fp_send = NULL;
        }
        ctx->transfer_done = true;
        break;
        
    default:
        break;
    }
}

/* ================================================================
 *  Send Ymodem file (via adapter, through protocol chain)
 * ================================================================ */

static bool prepare_ymodem_send(test_ctx_t* ctx) {
    if (!ctx->send_file_path) {
        return false;
    }
    
    ctx->fp_send = fopen(ctx->send_file_path, "rb");
    if (!ctx->fp_send) {
        fprintf(stderr, "[ERROR] Cannot open file: %s\n", ctx->send_file_path);
        return false;
    }
    
    fseek(ctx->fp_send, 0, SEEK_END);
    long fsize = ftell(ctx->fp_send);
    rewind(ctx->fp_send);
    
    if (fsize <= 0) {
        fprintf(stderr, "[ERROR] File is empty\n");
        fclose(ctx->fp_send);
        ctx->fp_send = NULL;
        return false;
    }
    
    ctx->send_file_size = (uint32_t)fsize;

    /* 发送器已在 init_protocol_chain 中创建并加入链,
     * 此处仅配置 1K 模式和标记已就绪 */
    ymodem_sender_t* sender = ymodem_adapter_get_sender(ctx->ymodem_send_parser);
    if (ctx->send_1k_mode) {
        ymodem_sender_enable_1k(sender);
    }

    ctx->ymodem_send_prepared = true;

    printf("[YMODEM] Sender prepared (%s mode), file: %s (%u bytes)\n",
           ctx->send_1k_mode ? "1K" : "128B",
           ctx->send_file_path, ctx->send_file_size);
    printf("[YMODEM] Send: 57 F0 01 XX XX XX XX to start (via Imperx control register)\n");
    return true;
}

/* ================================================================
 *  Initialize protocol chain
 * ================================================================ */

static bool init_protocol_chain(test_ctx_t* ctx) {
    /* 创建协议链(容纳 4 个解析器: Ymodem接收, Ymodem发送, Imperx, Camyu) */
    ctx->chain = protocol_chain_create(4);
    if (!ctx->chain) {
        fprintf(stderr, "[ERROR] Failed to create protocol chain\n");
        return false;
    }
    
    /* 创建 Imperx 解析器 */
    ctx->imperx_parser = imperx_protocol_create(
        ctx->imperx_rx_buf, sizeof(ctx->imperx_rx_buf),
        ctx->imperx_tx_buf, sizeof(ctx->imperx_tx_buf));
    if (!ctx->imperx_parser) {
        fprintf(stderr, "[ERROR] Failed to create Imperx parser\n");
        protocol_chain_destroy(ctx->chain);
        ctx->chain = NULL;
        return false;
    }
    protocol_parser_set_callbacks((protocol_parser_t*)ctx->imperx_parser,
                                  imperx_on_frame_ready, ctx, on_tx_ready, ctx);
    
    /* 创建 Camyu 解析器 */
    ctx->camyu_parser = camyu_protocol_create(
        ctx->camyu_rx_buf, sizeof(ctx->camyu_rx_buf),
        ctx->camyu_tx_buf, sizeof(ctx->camyu_tx_buf));
    if (!ctx->camyu_parser) {
        fprintf(stderr, "[ERROR] Failed to create Camyu parser\n");
        protocol_parser_destroy((protocol_parser_t*)ctx->imperx_parser);
        ctx->imperx_parser = NULL;
        protocol_chain_destroy(ctx->chain);
        ctx->chain = NULL;
        return false;
    }
    protocol_parser_set_callbacks((protocol_parser_t*)ctx->camyu_parser,
                                  camyu_on_frame_ready, ctx, on_tx_ready, ctx);
    
    /* 创建 Ymodem 接收解析器 */
    ctx->ymodem_recv_parser = ymodem_protocol_create_receiver(
        ctx->ymodem_rx_buf, sizeof(ctx->ymodem_rx_buf),
        NULL, 0);
    if (!ctx->ymodem_recv_parser) {
        fprintf(stderr, "[ERROR] Failed to create Ymodem receiver parser\n");
        protocol_parser_destroy((protocol_parser_t*)ctx->camyu_parser);
        ctx->camyu_parser = NULL;
        protocol_parser_destroy((protocol_parser_t*)ctx->imperx_parser);
        ctx->imperx_parser = NULL;
        protocol_chain_destroy(ctx->chain);
        ctx->chain = NULL;
        return false;
    }
    protocol_parser_set_callbacks((protocol_parser_t*)ctx->ymodem_recv_parser,
                                  ymodem_recv_on_frame_ready, ctx, on_tx_ready, ctx);

    /* 创建 Ymodem 发送解析器(始终创建, 但在 -s 指定文件前处于 IDLE 静默) */
    ctx->ymodem_send_parser = ymodem_protocol_create_sender(
        ctx->ymodem_send_rx_buf, sizeof(ctx->ymodem_send_rx_buf),
        ctx->ymodem_send_tx_buf, sizeof(ctx->ymodem_send_tx_buf));
    if (!ctx->ymodem_send_parser) {
        fprintf(stderr, "[ERROR] Failed to create Ymodem sender parser\n");
        protocol_parser_destroy((protocol_parser_t*)ctx->ymodem_recv_parser);
        ctx->ymodem_recv_parser = NULL;
        protocol_parser_destroy((protocol_parser_t*)ctx->camyu_parser);
        ctx->camyu_parser = NULL;
        protocol_parser_destroy((protocol_parser_t*)ctx->imperx_parser);
        ctx->imperx_parser = NULL;
        protocol_chain_destroy(ctx->chain);
        ctx->chain = NULL;
        return false;
    }
    protocol_parser_set_callbacks((protocol_parser_t*)ctx->ymodem_send_parser,
                                  ymodem_send_on_frame_ready, ctx, on_tx_ready, ctx);
    
    /* 加入协议链(按优先级: 接收在前, 发送次之; IDLE 阶段不会误匹配) */
    protocol_chain_add_parser(ctx->chain, (protocol_parser_t*)ctx->ymodem_recv_parser);
    protocol_chain_add_parser(ctx->chain, (protocol_parser_t*)ctx->ymodem_send_parser);
    protocol_chain_add_parser(ctx->chain, (protocol_parser_t*)ctx->imperx_parser);
    protocol_chain_add_parser(ctx->chain, (protocol_parser_t*)ctx->camyu_parser);
    
    printf("[PROTOCOL] Chain initialized with 4 parsers: Ymodem(Rx), Ymodem(Tx), Imperx, Camyu\n");
    
    return true;
}

/* ================================================================
 *  Cleanup resources
 * ================================================================ */

static void cleanup(test_ctx_t* ctx) {
    if (ctx->chain) {
        protocol_chain_destroy(ctx->chain);
    }
    
    if (ctx->imperx_parser) {
        protocol_parser_destroy((protocol_parser_t*)ctx->imperx_parser);
    }
    
    if (ctx->camyu_parser) {
        protocol_parser_destroy((protocol_parser_t*)ctx->camyu_parser);
    }
    
    if (ctx->ymodem_recv_parser) {
        protocol_parser_destroy((protocol_parser_t*)ctx->ymodem_recv_parser);
    }
    
    if (ctx->ymodem_send_parser) {
        protocol_parser_destroy((protocol_parser_t*)ctx->ymodem_send_parser);
    }
    
    if (ctx->fp_send) {
        fclose(ctx->fp_send);
    }
    
    if (ctx->fp_recv) {
        fclose(ctx->fp_recv);
    }
    
    serial_close(ctx->h_com);
}

/* ================================================================
 *  Main test loop
 * ================================================================ */

static int run_test(test_ctx_t* ctx) {
    uint8_t rx_buf[256];
    uint32_t last_poll_time = system_get_time_ms();
    
    printf("\n[INFO] Starting protocol chain test...\n");
    printf("[INFO] Press Ctrl+C to exit\n");
    printf("[INFO] Send protocol frames from terminal to test\n\n");
    
    /* Prepare Ymodem sender if file is specified (does not start yet) */
    if (ctx->send_file_path && !prepare_ymodem_send(ctx)) {
        fprintf(stderr, "[ERROR] Failed to prepare Ymodem sender\n");
        return 1;
    }
    
    ctx->running = true;
    
    while (ctx->running) {
        /* Read serial data */
        int n = serial_read(ctx->h_com, rx_buf, sizeof(rx_buf));
        
        if (n > 0) {
            if (ctx->verbose) {
                printf("[RECV] %d bytes: ", n);
                for (int i = 0; i < n && i < 16; i++) {
                    printf("%02X ", rx_buf[i]);
                }
                if (n > 16) printf("...");
                printf("\n");
            }
            
            /* Feed data to protocol chain (统一入口, 含发送器/接收器/Imperx/Camyu) */
            protocol_chain_feed(ctx->chain, rx_buf, (uint32_t)n);
        }
        else if (n < 0) {
            fprintf(stderr, "[ERROR] Serial read error\n");
            ctx->running = false;
            break;
        }
        
        /* Periodic timeout polling */
        uint32_t now = system_get_time_ms();
        if (now - last_poll_time >= TEST_POLL_INTERVAL_MS) {
            last_poll_time = now;
            
            /* Protocol chain timeout polling (统一轮询, 含所有解析器的 poll + 基类超时) */
            protocol_chain_check_timeout_poll(ctx->chain);
        }
        
        /* Check Ymodem transfer completion */
        if (ctx->transfer_done || ctx->transfer_error) {
            ctx->running = false;
            break;
        }
        
        /* Short sleep when no data */
        if (n == 0) {
            Sleep(TEST_POLL_INTERVAL_MS);
        }
    }
    
    return ctx->transfer_error ? 1 : 0;
}

/* ================================================================
 *  Main entry
 * ================================================================ */

int main(int argc, char* argv[]) {
    test_ctx_t ctx = {0};
    const char* port_name = NULL;
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            ctx.verbose = true;
        }
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            ctx.send_file_path = argv[++i];
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            ctx.recv_dir = argv[++i];
        }
        else if (strcmp(argv[i], "--1k") == 0) {
            ctx.send_1k_mode = true;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
        else if (!port_name) {
            port_name = argv[i];
        }
    }
    
    if (!port_name) {
        fprintf(stderr, "Error: COM port not specified\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    /* Open serial port */
    ctx.h_com = serial_open(port_name);
    if (ctx.h_com == INVALID_HANDLE_VALUE) {
        return 1;
    }

    /* Create output directory for Ymodem received files */
    if (ctx.recv_dir) {
        if (!CreateDirectoryA(ctx.recv_dir, NULL)) {
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                fprintf(stderr, "[ERROR] Cannot create directory: %s (error: %lu)\n",
                        ctx.recv_dir, GetLastError());
                cleanup(&ctx);
                return 1;
            }
        }
        printf("[INFO] Ymodem receive output dir: %s\n", ctx.recv_dir);
    }
    
    /* Initialize protocol chain */
    if (!init_protocol_chain(&ctx)) {
        cleanup(&ctx);
        return 1;
    }
    
    /* Register console control handler (Ctrl+C) */
    g_test_ctx = &ctx;
    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        fprintf(stderr, "[WARNING] Failed to set console ctrl handler\n");
    }
    
    /* Run test */
    int result = run_test(&ctx);
    
    /* Cleanup */
    cleanup(&ctx);
    
    printf("\n[RESULT] Test %s\n", result == 0 ? "completed successfully" : "failed");
    return result;
}
