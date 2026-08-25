#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "at32f403a_407.h"

static StaticSemaphore_t s_log_mutex_storage;
static SemaphoreHandle_t s_log_mutex = NULL;
static volatile uint32_t s_log_isr_drop_count = 0U;

/**
 * @brief 初始化日志串行化互斥量
 */
void Log_Init(void) {
    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateMutexStatic(&s_log_mutex_storage);
    }
}

/**
 * @brief 在任务上下文串行输出一行日志
 * @param color ANSI颜色，NULL表示不使用颜色
 * @param tag 日志标签
 * @param fmt printf格式字符串
 */
void Log_Printf(const char *color, const char *tag, const char *fmt, ...) {
    BaseType_t locked = pdFALSE;
    BaseType_t scheduler_state;
    va_list args;

    if ((tag == NULL) || (fmt == NULL)) {
        return;
    }

    if (__get_IPSR() != 0U) {
        s_log_isr_drop_count++;
        return;
    }

    scheduler_state = xTaskGetSchedulerState();
    if ((s_log_mutex != NULL) &&
        (scheduler_state == taskSCHEDULER_RUNNING)) {
        locked = xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    } else if ((s_log_mutex != NULL) &&
               (scheduler_state == taskSCHEDULER_SUSPENDED)) {
        locked = xSemaphoreTake(s_log_mutex, 0U);
        if (locked != pdTRUE) {
            return;
        }
    } else {
        /* 调度器启动前只有单一执行上下文，不需要互斥。 */
    }

    if (color != NULL) {
        printf("%s", color);
    }

    printf("[%s] ", tag);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (color != NULL) {
        printf("%s", COLOR_RESET);
    }
    printf("\r\n");

    if (locked == pdTRUE) {
        (void)xSemaphoreGive(s_log_mutex);
    }
}

/**
 * @brief 获取因误在中断上下文调用而丢弃的日志数量
 * @return 丢弃日志累计数量
 */
uint32_t Log_IsrDropCountGet(void) {
    return s_log_isr_drop_count;
}
