/**
 * @file  protocol_parser_test.c
 * @brief 协议解析器框架集成测试
 *
 * 测试用例以数据表形式定义。用户通过向 @ref test_cases[] 追加行来
 * 添加新测试 — 无需修改核心逻辑。
 *
 * 每个测试用例输出输入/结果/输出的一一对应:
 *   [Input]  输入十六进制转储
 *   [Result] PARSER_ERR_xxx
 *   [Output] 输出十六进制转储(如果有)
 *
 * 回调:
 *   - frame_ready: 为 READ 命令填充标准 4 字节应答
 *   - tx_ready:    捕获输出数据用于验证
 *
 * @author Protocol Parser Framework
 * @version 2.0
 * @date   2024
 */
#include "../core/protocol_chain.h"
#include "../protocols/imperx/protocol_parser_imperx.h"
#include "../protocols/camyu/protocol_parser_camyu.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief 系统时间桩实现(测试环境, 无真实时钟)
 *
 * 测试用例不依赖超时机制, 返回 0 即可满足链接。
 * 嵌入式平台应替换为真实毫秒级时间戳。
 */
uint32_t system_get_time_ms(void) {
    return 0;
}

/* ======================== 测试用例类型 ============================== */

/** @brief 测试用例的送入模式 */
typedef enum {
    TEST_FEED_WHOLE,    /**< 一次性送入完整帧 */
    TEST_FEED_STREAM,   /**< 逐字节送入 */
} test_feed_mode_t;

/** @brief 用于结果匹配的协议标识 */
typedef enum {
    TEST_PROTO_IMPERX,
    TEST_PROTO_CAMYU,
    TEST_PROTO_NONE,    /**< 预期无 frame_ready (无效数据) */
} test_proto_t;

/**
 * @brief 单个测试用例描述
 *
 * 向 @ref test_cases[] 中加入条目即可创建新测试。
 */
typedef struct {
    const char*       name;               /**< 测试描述(打印用) */
    test_proto_t      proto;              /**< 预期协议 */
    const uint8_t*    input;              /**< 输入帧数据 */
    uint32_t          input_len;          /**< 输入长度 */
    test_feed_mode_t  feed_mode;          /**< WHOLE 或 STREAM */
    parser_error_t    expected_result;    /**< 预期最终错误码 */
    bool              expects_frame_ready;/**< true 表示必须触发 frame_ready */
    const uint8_t*    expected_output;    /**< 预期 tx_buffer 内容(NULL=跳过检查) */
    uint32_t          expected_output_len;/**< 预期输出长度 */
} test_case_t;

/* ======================== 测试数据缓冲区 ============================ */

/** @brief 捕获最后一次 tx_ready 输出的缓冲区 */
static uint8_t  g_last_output[256];
static uint32_t g_last_output_len = 0;

/** @brief 回调计数器 */
static int g_imperx_frame_ready = 0;
static int g_camyu_frame_ready  = 0;

/* ======================== 辅助: 打印十六进制 ======================== */

/** @brief 带标签前缀打印十六进制转储 */
static void print_hex(const char* label, const uint8_t* data, uint32_t len) {
    printf("  %s ", label);
    for (uint32_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("(%u bytes)\n", len);
}

/* ======================== 回调函数 ================================== */

/**
 * @brief tx_ready 回调 — 捕获输出并打印
 */
void on_tx_ready(protocol_parser_t* parser, void* user_ctx) {
    uint32_t len = parser->tx.data_len;
    if (len > sizeof(g_last_output)) {
        len = sizeof(g_last_output);
    }
    memcpy(g_last_output, parser->tx.buffer, len);
    g_last_output_len = len;
    (void)user_ctx;
}

/**
 * @brief 统一的 Imperx frame_ready 回调
 *
 * READ 命令时填充 4 字节样本应答。
 */
void imperx_on_frame_ready(protocol_parser_t* parser,
                            void* parsed_data, void* user_ctx) {
    imperx_private_t* pri = (imperx_private_t*)parsed_data;
    g_imperx_frame_ready++;

    imperx_cmd_type_t cmd = (pri->parsed_len == 0) ? IMPERX_CMD_READ
                                                    : IMPERX_CMD_WRITE;

    printf("  [FrameReady] Imperx %s, Address: 0x%04llX\n",
           (cmd == IMPERX_CMD_READ) ? "READ" : "WRITE",
           (unsigned long long)pri->parsed_id);

    if (cmd == IMPERX_CMD_READ) {
        static uint8_t s_resp[4];
        const uint8_t sample[] = { 0x12, 0x34, 0x56, 0x78 };
        memcpy(s_resp, sample, sizeof(sample));
        pri->parsed_data = s_resp;
        pri->parsed_len  = sizeof(sample);
    }
    (void)parser;
    (void)user_ctx;
}

/**
 * @brief 统一的 Camyu frame_ready 回调
 *
 * READ 命令时填充 4 字节样本应答。
 */
void camyu_on_frame_ready(protocol_parser_t* parser,
                           void* parsed_data, void* user_ctx) {
    camyu_private_t* pri = (camyu_private_t*)parsed_data;
    g_camyu_frame_ready++;

    printf("  [FrameReady] Camyu Opcode:%d, Address: 0x%08llX\n",
           pri->current_frame_info.opcode,
           (unsigned long long)pri->parsed_id);

    if (pri->current_frame_info.opcode == PCTOCAMERA_READ) {
        static uint8_t s_resp[4];
        const uint8_t sample[] = { 0x12, 0x34, 0x56, 0x78 };
        memcpy(s_resp, sample, sizeof(sample));
        pri->parsed_data     = s_resp;
        pri->parsed_data_len = sizeof(sample);
    }
    (void)parser;
    (void)user_ctx;
}

/* ======================== 测试运行器 ================================ */

/**
 * @brief  运行单个测试用例
 *
 * 向链中送入输入数据，检查结果码、回调计数器和预期输出。
 * 以清晰的格式打印输入/结果/输出。
 *
 * @param chain  协议链实例
 * @param tc     测试用例描述符
 * @retval 0   通过
 * @retval 1   失败
 */
static int run_test_case(protocol_chain* chain, const test_case_t* tc) {
    /* ---- 重置捕获状态 ---- */
    g_last_output_len = 0;
    int old_imperx = g_imperx_frame_ready;
    int old_camyu  = g_camyu_frame_ready;

    /* ---- 打印测试头 ---- */
    printf("\n>>> %s >>>\n", tc->name);

    /* ---- 打印输入 ---- */
    print_hex("[Input]", tc->input, tc->input_len);

    /* ---- 送入数据 ---- */
    parser_error_t result;
    if (tc->feed_mode == TEST_FEED_WHOLE) {
        result = protocol_chain_feed(chain, tc->input, tc->input_len);
    } else {
        /* STREAM 模式: 逐字节送入，检查中间 INCOMPLETE */
        for (uint32_t i = 0; i < tc->input_len; i++) {
            result = protocol_chain_feed(chain, &tc->input[i], 1);
            if (i < tc->input_len - 1) {
                if (result != PARSER_ERR_INCOMPLETE) {
                    printf("  [FAIL] Byte %u expected INCOMPLETE, got %d\n",
                           i, (int)result);
                    return 1;
                }
            }
        }
    }

    /* ---- 打印结果 ---- */
    printf("  [Result] %s",
           (result == PARSER_ERR_NONE)       ? "PARSER_ERR_NONE"       :
           (result == PARSER_ERR_INCOMPLETE) ? "PARSER_ERR_INCOMPLETE" :
           (result == PARSER_ERR_FRAME)      ? "PARSER_ERR_FRAME"      :
           (result == PARSER_ERR_TIMEOUT)    ? "PARSER_ERR_TIMEOUT"    :
           (result == PARSER_ERR_INVALID_PARAM) ? "PARSER_ERR_INVALID_PARAM" :
           (result == PARSER_ERR_UNKNOWN)    ? "PARSER_ERR_UNKNOWN"    :
           "PARSER_ERR_SPECIFIC");

    /* 对特定错误打印原始码值 */
    if (result > PARSER_ERR_UNKNOWN) {
        printf("(%d)", (int)result);
    }
    printf("\n");

    /* ---- 检查结果码 ---- */
    if (result != tc->expected_result) {
        printf("  [FAIL] Expected result %d, got %d\n",
               (int)tc->expected_result, (int)result);
        return 1;
    }

    /* ---- 打印输出(如果有) ---- */
    if (g_last_output_len > 0) {
        print_hex("[Output]", g_last_output, g_last_output_len);
    } else {
        printf("  [Output] (none)\n");
    }

    /* ---- 检查预期输出(如果指定) ---- */
    if (tc->expected_output && tc->expected_output_len > 0) {
        if (g_last_output_len != tc->expected_output_len) {
            printf("  [FAIL] Output length: expected %u, got %u\n",
                   tc->expected_output_len, g_last_output_len);
            return 1;
        }
        if (memcmp(g_last_output, tc->expected_output, g_last_output_len) != 0) {
            printf("  [FAIL] Output data mismatch\n");
            return 1;
        }
    }

    /* ---- 检查 frame_ready 回调 ---- */
    int imperx_fired = g_imperx_frame_ready - old_imperx;
    int camyu_fired  = g_camyu_frame_ready - old_camyu;

    if (tc->expects_frame_ready) {
        if (tc->proto == TEST_PROTO_IMPERX && imperx_fired != 1) {
            printf("  [FAIL] Expected Imperx frame_ready, got %d\n", imperx_fired);
            return 1;
        }
        if (tc->proto == TEST_PROTO_CAMYU && camyu_fired != 1) {
            printf("  [FAIL] Expected Camyu frame_ready, got %d\n", camyu_fired);
            return 1;
        }
    } else {
        if (imperx_fired != 0 || camyu_fired != 0) {
            printf("  [FAIL] Unexpected frame_ready (Imperx:%d Camyu:%d)\n",
                   imperx_fired, camyu_fired);
            return 1;
        }
    }

    printf("<<< PASS <<<\n");
    return 0;
}

/* ======================== 测试用例表 ================================ */

/** @brief Imperx WRITE: 0x57 + ADDR(0x0010) + DATA(0x12345678) */
static const uint8_t WRITE_IMP[] = { 0x57, 0x00, 0x10, 0x12, 0x34, 0x56, 0x78 };
/** @brief Imperx WRITE 预期输出: ACK(0x06) */
static const uint8_t WRITE_IMP_OUT[] = { 0x06 };

/** @brief Imperx READ: 0x52 + ADDR(0x0010) */
static const uint8_t READ_IMP[] = { 0x52, 0x00, 0x10 };
/** @brief Imperx READ 预期输出: ACK(0x06) + DATA(0x12345678) */
static const uint8_t READ_IMP_OUT[] = { 0x06, 0x12, 0x34, 0x56, 0x78 };

/** @brief 无效命令(无协议能匹配) */
static const uint8_t INVALID[] = { 0xAA, 0xBB, 0xCC, 0xDD };

/** @brief Camyu WRITE */
static const uint8_t WRITE_CAM[] = { 0x7E, 0x43, 0x00, 0x00, 0x00, 0x10, 0x08, 0x02 };

/**
 * @brief 主测试用例表
 */
static const test_case_t test_cases[] = {

    {
        .name                 = "Camyu WRITE (whole frame)",
        .proto                = TEST_PROTO_CAMYU,
        .input                = WRITE_CAM,
        .input_len            = sizeof(WRITE_CAM),
        .feed_mode            = TEST_FEED_WHOLE,
        .expected_result      = PARSER_ERR_NONE,
        .expects_frame_ready  = true,
    },

    {
        .name                 = "Camyu WRITE (byte stream)",
        .proto                = TEST_PROTO_CAMYU,
        .input                = WRITE_CAM,
        .input_len            = sizeof(WRITE_CAM),
        .feed_mode            = TEST_FEED_STREAM,
        .expected_result      = PARSER_ERR_NONE,
        .expects_frame_ready  = true,
    },

    {
        .name                 = "Imperx WRITE (whole frame)",
        .proto                = TEST_PROTO_IMPERX,
        .input                = WRITE_IMP,
        .input_len            = sizeof(WRITE_IMP),
        .feed_mode            = TEST_FEED_WHOLE,
        .expected_result      = PARSER_ERR_NONE,
        .expects_frame_ready  = true,
        .expected_output      = WRITE_IMP_OUT,
        .expected_output_len  = sizeof(WRITE_IMP_OUT),
    },

    {
        .name                 = "Imperx WRITE (locked, second frame)",
        .proto                = TEST_PROTO_IMPERX,
        .input                = WRITE_IMP,
        .input_len            = sizeof(WRITE_IMP),
        .feed_mode            = TEST_FEED_WHOLE,
        .expected_result      = PARSER_ERR_NONE,
        .expects_frame_ready  = true,
        .expected_output      = WRITE_IMP_OUT,
        .expected_output_len  = sizeof(WRITE_IMP_OUT),
    },

    {
        .name                 = "Imperx READ",
        .proto                = TEST_PROTO_IMPERX,
        .input                = READ_IMP,
        .input_len            = sizeof(READ_IMP),
        .feed_mode            = TEST_FEED_WHOLE,
        .expected_result      = PARSER_ERR_NONE,
        .expects_frame_ready  = true,
        .expected_output      = READ_IMP_OUT,
        .expected_output_len  = sizeof(READ_IMP_OUT),
    },

    {
        .name                 = "Invalid data (unlock + unknown)",
        .proto                = TEST_PROTO_NONE,
        .input                = INVALID,
        .input_len            = sizeof(INVALID),
        .feed_mode            = TEST_FEED_WHOLE,
        .expected_result      = PARSER_ERR_UNKNOWN,
        .expects_frame_ready  = false,
    },

    {
        .name                 = "Camyu WRITE (re-match after unlock)",
        .proto                = TEST_PROTO_CAMYU,
        .input                = WRITE_CAM,
        .input_len            = sizeof(WRITE_CAM),
        .feed_mode            = TEST_FEED_WHOLE,
        .expected_result      = PARSER_ERR_NONE,
        .expects_frame_ready  = true,
    },

};

static const int test_case_count =
    sizeof(test_cases) / sizeof(test_cases[0]);

/* ======================== 主入口 ==================================== */

int test(void) {
    int passed = 0;
    int failed = 0;

    printf("=== Protocol Chain Test ===\n");
    printf("Test cases: %d\n\n", test_case_count);

    /* ---- 初始化 ---- */
    protocol_chain* chain = protocol_chain_create(2);
    if (!chain) {
        printf("FAIL: Create protocol chain\n");
        return 1;
    }

    uint8_t rx_buf[32], tx_buf[32];

    imperx_protocol_parser_t* imperx = imperx_protocol_create(
        rx_buf, sizeof(rx_buf), tx_buf, sizeof(tx_buf));
    if (!imperx) { printf("FAIL: Create Imperx parser\n"); return 1; }
    protocol_parser_set_callbacks((protocol_parser_t*)imperx,
        imperx_on_frame_ready, NULL, on_tx_ready, NULL);

    camyu_protocol_parser_t* camyu = camyu_protocol_create(NULL, 0, NULL, 0);
    if (!camyu) { printf("FAIL: Create Camyu parser\n"); return 1; }
    protocol_parser_set_callbacks((protocol_parser_t*)camyu,
        camyu_on_frame_ready, NULL, on_tx_ready, NULL);

    protocol_chain_add_parser(chain, (protocol_parser_t*)camyu);
    protocol_chain_add_parser(chain, (protocol_parser_t*)imperx);

    /* ---- 运行所有测试用例 ---- */
    for (int i = 0; i < test_case_count; i++) {
        protocol_chain_set_locked_parser(chain, NULL);
        g_imperx_frame_ready = 0;
        g_camyu_frame_ready  = 0;

        int ret = run_test_case(chain, &test_cases[i]);
        if (ret == 0) {
            passed++;
        } else {
            failed++;
        }
    }

    /* ---- 清理 ---- */
    protocol_parser_destroy((protocol_parser_t*)imperx);
    protocol_parser_destroy((protocol_parser_t*)camyu);
    protocol_chain_destroy(chain);

    /* ---- 汇总 ---- */
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return (failed > 0) ? 1 : 0;
}
