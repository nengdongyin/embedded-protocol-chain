/**
 * @file  system_adapter.h
 * @brief 平台适配层
 *
 * 为内存分配和时间查询函数提供薄抽象层,
 * 使解析器框架与平台无关.
 *
 * 支持的平台(编译时选择):
 * - FreeRTOS   (定义 @c USE_FREERTOS)
 * - Zephyr     (定义 @c USE_ZEPHYR)
 * - Standard C (定义 @c USE_STD_LIBC, 或不定义以使用默认值)
 *
 * 若未定义任何平台宏, 默认选择 @c USE_STD_LIBC.
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#pragma once
#include <stdint.h>
/* ---- 平台选择 ---- */
#if !defined(USE_FREERTOS) && !defined(USE_ZEPHYR) && !defined(USE_STD_LIBC)
#define USE_STD_LIBC   /* 默认使用标准 C 库 */
#endif

/* ---- 内存分配 ---- */
#ifdef USE_FREERTOS
  #include "FreeRTOS.h"
  #define system_malloc  pvPortMalloc
  #define system_free    vPortFree
#elif defined(USE_ZEPHYR)
  #include <zephyr/kernel.h>
  #define system_malloc  k_malloc
  #define system_free    k_free
#elif defined(USE_STD_LIBC)
  #include <stdlib.h>
  #define system_malloc  malloc
  #define system_free    free
#else
  #error "system_adapter: 请定义 USE_FREERTOS, USE_ZEPHYR 或 USE_STD_LIBC"
#endif

/**
 * @brief 获取系统毫秒级时间戳
 *
 * 该函数**必须**由应用层实现。库不提供默认实现，
 * 链接时将因符号未定义而失败。
 *
 * @return uint32_t 当前系统时间（毫秒）
 */
uint32_t system_get_time_ms(void);
