# embedded-protocol-chain

> A lightweight multi-protocol parser framework for embedded systems — Imperx, Camyu, Ymodem, one UART, auto-matched.

## 目录

1. [概述](#1-概述)
2. [工作原理](#2-工作原理)
3. [目录结构](#3-目录结构)
4. [快速开始](#4-快速开始)
5. [API 参考](#5-api-参考)
6. [添加新协议](#6-添加新协议)
7. [配置说明](#7-配置说明)
8. [平台移植](#8-平台移植)
9. [上机测试](#9-上机测试)
10. [常见问题](#10-常见问题)

---

## 1. 概述

embedded-protocol-chain 是一个轻量级、可扩展的嵌入式 C 语言协议解析框架。它采用**面向对象设计**和**责任链模式**，支持在同一物理链路（UART）上同时挂载多种通信协议，自动匹配和动态切换。

### 内置协议

| 协议 | 类型 | 典型帧长 | 说明 |
|------|------|---------|------|
| Imperx | 寄存器读写 | 3~7 字节 | 读/写命令，自动编码 ACK/应答 |
| Camyu | PPP 类协议 | 变长 | 字节转义、BCC 校验、多版本 |
| Ymodem (接收) | 文件传输 | ≤1029 字节 | 1K/128B 模式，CRC16 校验，接收保存到 FS |
| Ymodem (发送) | 文件传输 | ≤1029 字节 | 1K/128B 模式，CRC16 校验，从 FS 读取发送 |

Ymodem 收发器通过**适配器模式**桥接到基类接口，可挂载到协议链中与其他协议共存。日常由 Imperx/Camyu 处理指令通信，通过 Imperx 控制寄存器触发 Ymodem 文件传输。

### 核心特性

- **多协议自动匹配** — 接收数据时自动识别是哪种协议，无需应用层干预
- **Ymodem 文件传输** — 内置完整 Ymodem 收发器，支持 1K/128B 帧模式
- **零拷贝设计** — 解析结果直接指向接收缓冲区内部，不额外拷贝数据
- **职责分离** — 帧解析、业务处理、物理发送各层互不依赖
- **扩展友好** — 添加新协议只需实现 4 个函数，不改框架代码
- **低资源占用** — 典型四协议实例占用 ~3KB RAM，适合 Cortex-M4 及以上 MCU
- **平台无关** — 内置 FreeRTOS / Zephyr / 标准 C 适配层（Zephyr 已上机验证）

---

## 2. 工作原理

### 2.1 架构概览

```
物理层 (UART/SPI/以太网)
       ↓
[protocol_chain]  ← 责任链: 自动匹配协议 (4 协议共存)
       ↓
 ┌─────┼─────┬──────────┐
 ↓     ↓     ↓          ↓
[Imperx] [Camyu] [Ymodem Rx] [Ymodem Tx]  ← 子类: 协议特定解析
 ↓       ↓        ↓            ↓
[frame_ready 回调]   [frame_ready 回调]    ← 用户: 处理业务数据
 ↓       ↓        ↓            ↓
[encode 自动编码]   [tx_ready 回调]       ← 框架/适配器: 自动编码/发送
 ↓       ↓        ↓            ↓
[tx_ready 回调]     [uart_poll_out]       ← 用户: 发送到物理层
```

Ymodem 收发器通过适配器桥接到基类。创建后处于静默 IDLE 状态，不发送任何字节。通过 Imperx 控制寄存器触发启动后自动开始 Ymodem 握手。

### 2.2 关键设计

#### 面向对象基类

基类 `protocol_parser_t` 定义了所有协议共用的基础设施：

- 配置参数（最大帧长、超时时间）
- 统计信息（接收帧数、错误计数）
- 收发缓冲区管理
- 回调函数注册
- 超时检测
- 虚函数表（多态派发）

子类通过嵌入基类作为首个成员来实现"继承"：

```c
typedef struct {
    protocol_parser_t base;   // 基类 — 必须为第一个成员
    imperx_private_t  pri;    // 协议特定私有数据
} imperx_protocol_parser_t;
```

#### 责任链匹配

协议链 `protocol_chain` 管理多个协议解析器。未锁定时，每个传入字节尝试所有解析器，直到出现以下结果：

| 返回值 | 含义 | 链动作 |
|--------|------|--------|
| `PARSER_ERR_NONE` | 帧解析成功 | **锁定**该解析器，后续字节直接转发 |
| `PARSER_ERR_INCOMPLETE` | 需要更多数据 | 记录标记，继续尝试其他解析器 |
| 其他错误 | 帧格式无效 | 继续尝试其他解析器 |

#### 帧完成处理管道

帧解析完成后自动执行以下管道，用户无需手动调用：

```
1. frame_ready 回调  →  用户处理业务数据，填充应答
2. encode 编码      →  框架自动调用协议编码函数
3. tx_ready 回调    →  用户将应答数据发送到物理层
4. reset 重置状态   →  准备接收下一帧
```

#### 匹配阶段静默

未锁定状态下，非匹配协议产生的错误应答（如 NACK）被自动抑制，不会发送到物理总线上。只有被锁定的协议才会真正发送错误应答。

---

## 3. 目录结构

```
protocol_parser/
├── core/                           ← 框架核心(禁止修改)
│   ├── protocol_parser.h           ← 基类接口
│   ├── protocol_parser.c           ← 基类实现
│   ├── protocol_parser_internal.h  ← 子类内部接口
│   ├── protocol_chain.h            ← 协议链接口
│   ├── protocol_chain.c            ← 协议链实现
│   └── system_adapter.h            ← 平台适配层
│
├── protocols/                      ← 协议子类(用户扩展点)
│   ├── imperx/                     ← Imperx 寄存器读写协议
│   │   ├── protocol_parser_imperx.h
│   │   └── protocol_parser_imperx.c
│   ├── camyu/                      ← Camyu PPP 类协议
│   │   ├── protocol_parser_camyu.h
│   │   └── protocol_parser_camyu.c
│   └── ymodem/                     ← Ymodem 文件传输协议
│       ├── src/                    ← 核心实现(收发器独立于框架)
│       │   ├── ymodem_common.h     ← 帧结构/枚举/CRC
│       │   ├── ymodem_common.c
│       │   ├── ymodem_receiver.h   ← 接收器
│       │   ├── ymodem_receiver.c
│       │   ├── ymodem_sender.h     ← 发送器
│       │   └── ymodem_sender.c
│       └── adapter/                ← 适配器(桥接到基类)
│           ├── protocol_parser_ymodem.h
│           └── protocol_parser_ymodem.c
│
└── app/                            ← 示例 & 测试
    ├── main.c                      ← 最简入口
    ├── protocol_parser_test.c      ← 单元测试套件
    ├── protocol_parser_test_windows.c ← Windows 串口集成测试
    └── protocol_parser_test_zephyr.c  ← Zephyr 上机测试(四协议链, 已验证)
```

---

## 4. 快速开始

### 4.1 最小示例

以下代码演示如何创建协议链、添加解析器、送入数据：

```c
#include "protocol_chain.h"
#include "imperx/protocol_parser_imperx.h"

// 1. 定义回调
void on_frame_ready(protocol_parser_t* parser, void* parsed_data, void* ctx) {
    imperx_private_t* pri = (imperx_private_t*)parsed_data;
    // 解析出的地址: pri->parsed_id
    // 解析出的数据: pri->parsed_data (长度 pri->parsed_len)
}

void on_tx_ready(protocol_parser_t* parser, void* ctx) {
    // parser->tx.buffer 中 data_len 字节需要发送
    uart_send(parser->tx.buffer, parser->tx.data_len);
}

// 2. 初始化
protocol_chain* chain = protocol_chain_create(4);  // 最多4个协议

uint8_t rx_buf[64], tx_buf[64];
imperx_protocol_parser_t* imperx = imperx_protocol_create(
    rx_buf, sizeof(rx_buf), tx_buf, sizeof(tx_buf));

protocol_parser_set_callbacks((protocol_parser_t*)imperx,
    on_frame_ready, on_tx_ready, NULL);
protocol_chain_add_parser(chain, (protocol_parser_t*)imperx);

// 3. 接收数据
// 在 UART 中断或主循环中调用:
protocol_chain_feed(chain, received_byte, 1);

// 4. 轮询超时(主循环中定期调用):
protocol_chain_check_timeout_poll(chain);
```

### 4.2 流式数据与整帧数据

框架提供两种数据送入接口：

```c
// 流式数据(推荐): 适合 UART 逐字节到达
protocol_chain_feed(chain, data, len);

// 完整帧: 适合已缓存的整帧
protocol_chain_feed_frame(chain, frame, len);
```

`feed` 在未锁定时返回 `PARSER_ERR_INCOMPLETE` 表示数据不完整需要更多字节。`feed_frame` 将输入视为完整的帧，任何错误都导致解锁。

### 4.3 帧完成回调

`frame_ready` 回调在帧解析成功后调用。用户在此回调中：

- 读取解析结果（地址、数据等）
- 对 READ 命令填充应答数据

```c
void frame_ready(protocol_parser_t* parser, void* parsed_data, void* ctx) {
    imperx_private_t* pri = (imperx_private_t*)parsed_data;

    if (pri->parsed_len == 0) {
        // READ 命令: 用户填充应答数据
        static uint8_t resp[4] = { 0x12, 0x34, 0x56, 0x78 };
        pri->parsed_data = resp;   // 指向持久缓冲区
        pri->parsed_len  = 4;
    }
    // WRITE 命令: 无需填充, encode 自动发 ACK
}
```

> **注意**: `parsed_data` 指向的缓冲区必须是持久的（生命周期覆盖后续 encode 调用）。可以使用 `static` 缓冲区或协议私有的持久缓冲区。不能指向栈上的局部变量。

### 4.4 Ymodem 集成

Ymodem 通过适配器模式桥接到基类接口。创建后处于静默 IDLE 状态，不发送任何字节。通过其他协议（如 Imperx）的控制命令触发启动。

```c
#include "protocol_chain.h"
#include "imperx/protocol_parser_imperx.h"
#include "ymodem/adapter/protocol_parser_ymodem.h"

/* 声明来自其他协议回调, 见完整示例 */
static void imperx_on_frame_ready(protocol_parser_t* parser,
                                  void* parsed_data, void* user_ctx);
static void on_tx_ready(protocol_parser_t* parser, void* user_ctx);

// 1. 创建协议链 (容纳 4 个解析器)
protocol_chain* chain = protocol_chain_create(4);

// 2. 创建 Imperx / Camyu 解析器 (日常指令)
uint8_t irx[32], itx[32];
imperx_protocol_parser_t* imperx = imperx_protocol_create(irx, 32, itx, 32);
protocol_parser_set_callbacks((protocol_parser_t*)imperx,
    imperx_on_frame_ready, NULL, on_tx_ready, NULL); // frame_ready 中触发 Ymodem
protocol_chain_add_parser(chain, (protocol_parser_t*)imperx);

// 3. 创建 Ymodem 接收器 (静默, 待触发)
uint8_t yrrx[YMODEM_STX_FRAME_LEN_BYTE], yrtx[4];
ymodem_protocol_parser_t* yrecv = ymodem_protocol_create_receiver(
    yrrx, sizeof(yrrx), yrtx, sizeof(yrtx));
protocol_parser_set_callbacks((protocol_parser_t*)yrecv,
    ymodem_recv_on_frame_ready, NULL, on_tx_ready, NULL);
protocol_chain_add_parser(chain, (protocol_parser_t*)yrecv);

// 4. 创建 Ymodem 发送器 (静默, 待触发)
uint8_t ystx[YMODEM_STX_FRAME_LEN_BYTE], ysrx[4];
ymodem_protocol_parser_t* ysend = ymodem_protocol_create_sender(
    ysrx, sizeof(ysrx), ystx, sizeof(ystx));
ymodem_sender_enable_1k(ymodem_adapter_get_sender(ysend)); // 启用 1K 模式
protocol_parser_set_callbacks((protocol_parser_t*)ysend,
    ymodem_send_on_frame_ready, NULL, on_tx_ready, NULL);
protocol_chain_add_parser(chain, (protocol_parser_t*)ysend);

// 5. 主循环 — 送入数据和轮询超时
while (1) {
    // 逐字节送入 (Ymodem 接收器锁定时可批量读, 见 Zephyr 示例)
    protocol_chain_feed(chain, &rx_byte, 1);
    protocol_chain_check_timeout_poll(chain);
}
```

**Ymodem 启动时机** (在 Imperx `frame_ready` 中):

```c
void imperx_on_frame_ready(protocol_parser_t* parser,
                           void* parsed_data, void* ctx) {
    imperx_private_t* pri = (imperx_private_t*)parsed_data;
    if ((pri->parsed_len > 0) && (pri->parsed_id == 0xF000)) {
        // WRITE 0xF000: 启动 Ymodem 接收
        ymodem_adapter_start_receiver(ymodem_recv_parser);
        protocol_chain_set_locked_parser(chain,
            (protocol_parser_t*)ymodem_recv_parser);
    }
    if ((pri->parsed_len > 0) && (pri->parsed_id == 0xF001)) {
        // WRITE 0xF001: 启动 Ymodem 发送
        ymodem_adapter_start_sender(ymodem_send_parser);
        protocol_chain_set_locked_parser(chain,
            (protocol_parser_t*)ymodem_send_parser);
    }
}
```

**Ymodem 适配器 API 速查**:

| 函数 | 说明 |
|------|------|
| `ymodem_protocol_create_receiver(rx, rxsz, tx, txsz)` | 创建接收器适配器 (静默) |
| `ymodem_protocol_create_sender(rx, rxsz, tx, txsz)` | 创建发送器适配器 (静默) |
| `ymodem_adapter_start_receiver(parser)` | 发送初始 'C' 启动接收握手 |
| `ymodem_adapter_start_sender(parser)` | 切换到 ESTABLISHING 等待对方 'C' |
| `ymodem_adapter_get_receiver(parser)` | 获取内部接收器实例 |
| `ymodem_adapter_get_sender(parser)` | 获取内部发送器实例 |
| `ymodem_sender_enable_1k(sender)` | 启用 1024 字节帧模式 |

完整可运行示例参见:
- `app/protocol_parser_test_zephyr.c` — Zephyr 四协议链 (已上机验证)
- `app/protocol_parser_test_windows.c` — Windows 串口集成测试

---

## 5. API 参考

### 5.1 基类 API

#### `protocol_parser_parse_data`

```c
parser_error_t protocol_parser_parse_data(protocol_parser_t* parser,
                                          const uint8_t* data, uint32_t len);
```

解析接收数据。更新超时时间戳后分发到子类实现。通常通过 `protocol_chain_feed` 间接调用。

---

#### `protocol_parser_set_callbacks`

```c
bool protocol_parser_set_callbacks(protocol_parser_t* parser,
                                   on_frame_ready_t frame_ready_cb,
                                   on_tx_ready_t tx_ready_cb,
                                   void* user_ctx);
```

设置解析器回调函数。

| 参数 | 说明 |
|------|------|
| `frame_ready_cb` | 帧解析完成回调(可为 NULL) |
| `tx_ready_cb` | 发送数据就绪回调(可为 NULL) |
| `user_ctx` | 用户上下文指针, 传递给所有回调 |

---

#### `protocol_parser_check_timeout_poll`

```c
bool protocol_parser_check_timeout_poll(protocol_parser_t* parser);
```

轮询检查超时。超时时自动触发 `on_frame_error`。通过 `protocol_chain_check_timeout_poll` 间接调用。

---

#### `protocol_parser_encode`

```c
uint32_t protocol_parser_encode(protocol_parser_t* parser, const void* data);
```

手动编码数据到 tx_buffer。通常不需要手动调用，`on_frame_ready` 会自动执行编码。

---

#### `protocol_parser_destroy`

```c
void protocol_parser_destroy(protocol_parser_t* parser);
```

销毁解析器，释放动态分配的缓冲区和实例内存。

---

#### `protocol_parser_get_tx_data`

```c
uint8_t* protocol_parser_get_tx_data(protocol_parser_t* parser, uint32_t* len);
```

获取 tx_buffer 指针和待发送数据长度。可与 `tx_ready` 回调配合使用。

---

### 5.2 协议链 API

#### `protocol_chain_create`

```c
protocol_chain* protocol_chain_create(uint32_t max_parsers);
```

创建协议链实例。

| 参数 | 说明 |
|------|------|
| `max_parsers` | 支持的最大解析器数量 |

---

#### `protocol_chain_destroy`

```c
void protocol_chain_destroy(protocol_chain* chain);
```

销毁协议链。解析器本身不会被销毁，需调用者单独释放。

---

#### `protocol_chain_add_parser`

```c
bool protocol_chain_add_parser(protocol_chain* chain, protocol_parser_t* parser);
```

添加解析器到协议链。解析器按添加顺序依次匹配。

---

#### `protocol_chain_remove_parser`

```c
bool protocol_chain_remove_parser(protocol_chain* chain, protocol_parser_t* parser);
```

从协议链移除指定解析器。若被移除的是当前锁定解析器，自动解锁。

---

#### `protocol_chain_feed`

```c
parser_error_t protocol_chain_feed(protocol_chain* chain,
                                   const uint8_t* data, uint32_t len);
```

向链送入流式数据。

**返回值**:

| 返回值 | 说明 |
|--------|------|
| `PARSER_ERR_NONE` | 帧解析成功(锁定解析器) |
| `PARSER_ERR_INCOMPLETE` | 需要更多数据 |
| `PARSER_ERR_UNKNOWN` | 无解析器能匹配 |
| `PARSER_ERR_INVALID_PARAM` | 参数错误 |

---

#### `protocol_chain_feed_frame`

```c
parser_error_t protocol_chain_feed_frame(protocol_chain* chain,
                                         const uint8_t* frame, uint32_t len);
```

向链送入完整帧。与 `feed` 不同，任何错误都会解锁当前解析器。

---

#### `protocol_chain_get_locked_parser`

```c
protocol_parser_t* protocol_chain_get_locked_parser(protocol_chain* chain);
```

获取当前锁定的解析器，无锁定时返回 NULL。

---

#### `protocol_chain_set_locked_parser`

```c
bool protocol_chain_set_locked_parser(protocol_chain* chain,
                                      protocol_parser_t* parser);
```

手动锁定或解锁解析器。传 NULL 解锁。

---

#### `protocol_chain_check_timeout_poll`

```c
bool protocol_chain_check_timeout_poll(protocol_chain* chain);
```

轮询检查所有解析器的超时状态。

### 5.3 错误码

| 错误码 | 值 | 说明 |
|--------|-----|------|
| `PARSER_ERR_NONE` | 0 | 无错误 |
| `PARSER_ERR_INCOMPLETE` | 1 | 数据不完整 |
| `PARSER_ERR_FRAME` | 2 | 帧格式错误 |
| `PARSER_ERR_TIMEOUT` | 3 | 超时 |
| `PARSER_ERR_INVALID_PARAM` | 4 | 无效参数 |
| `PARSER_ERR_UNKNOWN` | 5 | 未知错误 |
| `PARSER_ERR_SPECIFIC_START` | 100 | 协议特定错误起始值 |

子类错误码从 `PARSER_ERR_SPECIFIC_START` 起始，通过 `parser_error_generic()` 自动映射为 `PARSER_ERR_FRAME`。

---

## 6. 添加新协议

### 6.1 文件模板

添加新协议 MyProtocol，在 `protocols/` 下创建 `myproto/` 目录，包含两个文件：

```
protocols/myproto/
├── protocol_parser_myproto.h      ← 接口定义
└── protocol_parser_myproto.c      ← 实现
```

### 6.2 接口头文件

```c
/**
 * @file  protocol_parser_myproto.h
 * @brief MyProtocol 协议解析器接口
 */
#pragma once
#include "protocol_parser.h"
#include "protocol_parser_internal.h"

/* ---- 命令类型(可选) ---- */
typedef enum {
    MYPROTO_CMD_READ  = 0,
    MYPROTO_CMD_WRITE = 1,
} myproto_cmd_type_t;

/* ---- 协议特定错误码(可选) ---- */
typedef enum {
    MYPROTO_ERR_INVALID_HEADER = PARSER_ERR_SPECIFIC_START,
    MYPROTO_ERR_CHECKSUM,
} myproto_error_t;

/* ---- 解析器状态 ---- */
typedef enum {
    MYPROTO_STATE_WAIT_HEAD,
    MYPROTO_STATE_WAIT_LEN,
    MYPROTO_STATE_WAIT_DATA,
} myproto_state_t;

/* ---- 私有数据 ---- */
typedef struct {
    myproto_state_t    state;
    uint64_t           parsed_id;
    uint8_t*           parsed_data;
    uint32_t           parsed_len;
    uint32_t           header_errors;
} myproto_private_t;

/* ---- 解析器结构体 ---- */
typedef struct {
    protocol_parser_t base;      // 基类(必须为第一个成员)
    myproto_private_t pri;
} myproto_protocol_parser_t;

/* ---- 创建函数 ---- */
myproto_protocol_parser_t* myproto_protocol_create(
    void* rx_buffer, uint32_t rx_buffer_size,
    void* tx_buffer, uint32_t tx_buffer_size);
```

### 6.3 源文件 (4 个必须实现的函数)

```c
#include "protocol_parser_myproto.h"

static parser_error_t myproto_parse_data(protocol_parser_t* parser,
                                          const uint8_t* data, uint32_t len);
static uint32_t myproto_encode(protocol_parser_t* parser, const void* data);
static void myproto_reset(protocol_parser_t* parser);
static void myproto_destroy(protocol_parser_t* parser);

/* 虚函数表 */
static const struct protocol_parser_ops myproto_ops = {
    .parse_data = myproto_parse_data,
    .encode     = myproto_encode,
    .reset      = myproto_reset,
    .destroy    = myproto_destroy,
};

/* 1. 创建函数 */
myproto_protocol_parser_t* myproto_protocol_create(
    void* rx_buffer, uint32_t rx_buffer_size,
    void* tx_buffer, uint32_t tx_buffer_size) {
    parser_config_t config = get_default_config();
    config.max_frame_len = 64;
    config.timeout_ms = 100;

    protocol_parser_t* base = protocol_parser_create_common(
        sizeof(myproto_protocol_parser_t),
        &myproto_ops,
        &config,
        (uint8_t*)rx_buffer, rx_buffer_size,
        (uint8_t*)tx_buffer, tx_buffer_size);
    return (myproto_protocol_parser_t*)base;
}

/* 2. 数据解析 */
static parser_error_t myproto_parse_data(protocol_parser_t* parser,
                                          const uint8_t* data, uint32_t len) {
    myproto_protocol_parser_t* mp = (myproto_protocol_parser_t*)parser;
    // 实现状态机解析
    // 完成时设置 parser->parsed_result = &mp->pri;
    // 然后调用 protocol_parser_on_frame_ready(parser);
    return PARSER_ERR_INCOMPLETE;
}

/* 3. 编码 */
static uint32_t myproto_encode(protocol_parser_t* parser, const void* data) {
    myproto_protocol_parser_t* mp = (myproto_protocol_parser_t*)parser;
    // 将应答数据编码到 mp->base.tx.buffer
    // 设置 mp->base.tx.data_len = 编码后长度
    return mp->base.tx.data_len;
}

/* 4. 重置 */
static void myproto_reset(protocol_parser_t* parser) {
    myproto_protocol_parser_t* mp = (myproto_protocol_parser_t*)parser;
    mp->pri.state = MYPROTO_STATE_WAIT_HEAD;
    mp->pri.parsed_id  = 0;
    mp->pri.parsed_data = NULL;
    mp->pri.parsed_len  = 0;
    default_reset(parser);  // 调用基类重置
}

/* 5. 销毁 */
static void myproto_destroy(protocol_parser_t* parser) {
    (void)parser;
}
```

### 6.4 使用新协议

```c
// 在应用中包含:
#include "protocol_chain.h"
#include "myproto/protocol_parser_myproto.h"

// 创建并添加:
myproto_protocol_parser_t* myproto = myproto_protocol_create(
    rx_buf, sizeof(rx_buf), tx_buf, sizeof(tx_buf));
protocol_parser_set_callbacks((protocol_parser_t*)myproto,
    on_frame_ready, on_tx_ready, NULL);
protocol_chain_add_parser(chain, (protocol_parser_t*)myproto);
```

### 6.5 子类实现要点

#### parsed_data 生命周期

`frame_ready` 回调后框架会自动调用 `encode`。如果用户在 `frame_ready` 中改变了 `parsed_data` 指向的缓冲区，该缓冲区必须在 `encode` 调用期间保持有效：

```c
// 正确: 使用 static 或持久缓冲区
void frame_ready(...) {
    static uint8_t resp[8];
    pri->parsed_data = resp;       // 持久, 安全
    pri->parsed_len = sizeof(resp);
}

// 错误: 指向栈上局部变量, encode 时已失效
void frame_ready(...) {
    uint8_t resp[8];               // 栈上, encode 时已释放
    pri->parsed_data = resp;       // ← 悬垂指针!
    pri->parsed_len = sizeof(resp);
}
```

#### 帧头触发的强制重置

Camyu 协议提供了一个好模式：收到帧头字节时无条件调用 `camyu_reset(parser)`，确保跨协议锁定切换后状态干净。新协议也可以采用类似策略。

#### on_frame_error 的调用

错误处理应遵循以下模式：

```c
// 检测到帧错误时:
pri->header_errors++;                    // 更新子类统计
encode_error(parser, error_code);        // 写入错误应答到 tx_buffer
protocol_parser_on_frame_error(parser, error_code);  // 通知框架
return error_code;                       // 返回错误码
```

> 匹配阶段 `protocol_parser_on_frame_error` 会自动抑制 tx_ready，不会发送错误应答到总线上。锁定后才会正常发送。

---

## 7. 配置说明

### 7.1 parser_config_t

```c
typedef struct {
    uint16_t max_frame_len;     // 最大帧长度(含头尾校验)
    uint32_t timeout_ms;        // 超时阈值(毫秒), 0 = 禁用
} parser_config_t;
```

默认值: `max_frame_len=1024`, `timeout_ms=1000`。

各协议在 `create` 函数中覆盖默认值：

```c
// Imperx: 帧很短, 短超时
config.max_frame_len = 10;
config.timeout_ms = 100;

// Camyu: 最大 256 字节数据帧, 不使用超时
config.max_frame_len = 320;
config.timeout_ms = 0;   // 禁用超时
```

---

## 8. 平台移植

### 8.1 内存分配

在编译时通过宏选择平台:

```c
#define USE_FREERTOS    // 使用 FreeRTOS 的 pvPortMalloc/vPortFree
#define USE_ZEPHYR      // 使用 Zephyr 的 k_malloc/k_free
#define USE_STD_LIBC    // 使用标准 C 的 malloc/free(默认)
```

### 8.2 时间查询

系统时间用于超时检测。标准 C 模式下返回 0，超时功能不可用。

```c
// FreeRTOS
#define system_get_time_ms()  (xTaskGetTickCount() * portTICK_PERIOD_MS)

// Zephyr (已上机验证, 参见 app/protocol_parser_test_zephyr.c)
#define system_get_time_ms()  k_uptime_get_32()

// Windows 测试桩 (参见 app/protocol_parser_test_windows.c)
#define system_get_time_ms()  GetTickCount64()

// 标准 C (测试环境): 需要用户自定义或设置 timeout_ms = 0
```

---

## 9. 上机测试

### 9.1 Zephyr 四协议链测试

`app/protocol_parser_test_zephyr.c` 在真实 Zephyr 硬件上验证了四协议共存：

| 项目 | 配置 |
|------|------|
| 平台 | Zephyr RTOS (Cortex-M7, 550MHz) |
| UART | UART1, 460800 bps, 8N1 |
| 管道 | 2048 字节 `k_pipe`, ISR 填充, 线程消费 |
| 协议 | Imperx + Camyu + Ymodem Rx + Ymodem Tx |
| 文件系统 | Zephyr FS (LittleFS) |
| 发送模式 | 逐字节送入责任链; Ymodem 接收器锁定时批量读 |

**性能数据** (460800 bps):

| 操作 | 速度 | 说明 |
|------|------|------|
| 下载 (Ymodem 接收) | ~25 KB/s | 流式接收 → FS 写入, 1K 块 CRC16 |
| 回读 (Ymodem 发送) | ~39 KB/s | FS 读取 → Ymodem 组帧发送 |

**测试流程**:
1. 上电后 Imperx/Camyu 就绪, Ymodem 静默
2. 外部终端发送 Imperx WRITE `57 F0 00` → 触发 Ymodem 接收
3. 外部终端发送 Imperx WRITE `57 F0 01` → 触发 Ymodem 发送
4. 传输完毕自动回到 Imperx/Camyu 匹配状态

**逐字节 vs 批量读取**:

Zephyr 示例中根据当前锁定解析器动态切换读取策略:
- Ymodem 接收器锁定 → `k_pipe_read(buf, 1029, 100ms)` 批量读大帧
- 其他协议/发送器锁定 → `k_pipe_read(&byte, 1, 100ms)` 逐字节读

这是因为 Ymodem 发送器的对方应答只有 1 字节 (C/ACK/NAK)，Imperx/Camyu 也是短帧。如果不区分，长缓冲区会导致每次读都阻塞 100ms。

### 9.2 Windows 串口测试

`app/protocol_parser_test_windows.c` 在 Windows 物理串口上测试:

```bash
# 编译 (MinGW)
gcc -Icore -Iprotocols/imperx -Iprotocols/camyu \
    -Iprotocols/ymodem/src -Iprotocols/ymodem/adapter \
    app/protocol_parser_test_windows.c \
    core/protocol_parser.c core/protocol_chain.c core/protocol_parser_internal.c \
    protocols/imperx/protocol_parser_imperx.c \
    protocols/camyu/protocol_parser_camyu.c \
    protocols/ymodem/src/ymodem_common.c \
    protocols/ymodem/src/ymodem_receiver.c \
    protocols/ymodem/src/ymodem_sender.c \
    protocols/ymodem/adapter/protocol_parser_ymodem.c \
    -o chain_test.exe -lws2_32

# 运行
chain_test.exe COM15 -v -o ./recv -s firmware.bin --1k
```

### 9.3 单元测试

`app/protocol_parser_test.c` 提供 Imperx/Camyu 的离线单元测试，验证帧解析、错误检测、编码链路。标准 C 环境编译，无需硬件。

编译命令:
```bash
gcc -Icore -Iprotocols/imperx -Iprotocols/camyu \
    app/protocol_parser_test.c \
    core/protocol_parser.c core/protocol_chain.c core/protocol_parser_internal.c \
    protocols/imperx/protocol_parser_imperx.c \
    protocols/camyu/protocol_parser_camyu.c \
    -o protocol_parser_test.exe
```

---

## 10. 常见问题

### Q: 为什么未锁定时收到很多 NACK？

**A**: 不会。未锁定(匹配阶段)时 `on_frame_error` 中的 `tx_ready` 被 `parser->locked` 标志抑制，错误应答不会被发送到总线上。只有锁定后才会真正发送。

### Q: 流式数据时锁定后能否切换协议？

**A**: 可以。锁定解析器返回致命错误（非 NONE/INCOMPLETE）时会自动解锁，重新进入匹配模式。

### Q: 同一个解析器可以同时加入多个链吗？

**A**: 不可以。一个解析器实例同时只能加入一个协议链。

### Q: 超时检测如何工作？

**A**: 每次 `parse_data` 调用时更新 `last_activity_ms`。`check_timeout_poll` 检查当前时间与 `last_activity_ms` 的差值是否超过 `timeout_ms`。超时时调用 `on_frame_error(PARSER_ERR_TIMEOUT)`。

### Q: 为什么 `parsed_data` 不能指向局部变量？

**A**: `frame_ready` 回调返回后框架会自动调用 `encode` 编码应答，此时局部变量已被销毁。`parsed_data` 指向的缓冲区必须在整个回调→encode 周期内有效。

### Q: `protocol_chain_feed` 和 `protocol_chain_feed_frame` 的区别？

**A**: `feed` 用于流式数据，未锁定时返回 `INCOMPLETE` 让调用者继续送更多字节。`feed_frame` 用于已经缓存的完整帧，任何错误都直接解锁。
