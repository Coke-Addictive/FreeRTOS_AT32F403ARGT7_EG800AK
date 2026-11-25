#include "log.h"

#if LOG_MAIN_ENABLE
#include <stdarg.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "at32f403a_407.h"

static StaticSemaphore_t s_log_mutex_storage;       // 日志互斥锁的静态存储区。
static SemaphoreHandle_t s_log_mutex = NULL;        // UART4 整行输出互斥锁。

static const char *const s_log_colors[] = {         // 按日志等级选择 ANSI 前景色。
    [LOG_LEVEL_ERROR]       = LOG_COLOR_ERROR,
    [LOG_LEVEL_WARN]        = LOG_COLOR_WARN,
    [LOG_LEVEL_SYSCYCLE]    = LOG_COLOR_SYSCYCLE,
    [LOG_LEVEL_INFO]        = LOG_COLOR_INFO,
    [LOG_LEVEL_DEBUG]       = LOG_COLOR_DEBUG
};
#endif

/**
 * @brief 在调度器启动前初始化 UART4 日志互斥锁。
 */
void Log_Init(void) {
#if LOG_MAIN_ENABLE
    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateMutexStatic(&s_log_mutex_storage);
    }
#endif
}

#if LOG_MAIN_ENABLE
/**
 * @brief 在任务上下文串行输出一整行日志。
 * @param level 日志等级，用于选择前景色。
 * @param tag 日志标签，以空字符结尾。
 * @param fmt printf 格式字符串。
 * @param ... 与格式字符串匹配的参数。
 */
void Log_Printf(LogLevel_e level, const char *tag, const char *fmt, ...) {
    BaseType_t scheduler_state;
    BaseType_t locked = pdFALSE;
    va_list args;

    if ((__get_IPSR() != 0U) ||
        ((unsigned int)level > (unsigned int)LOG_LEVEL_DEBUG) ||
        (tag == NULL) || (fmt == NULL)) {
        return;
    }

    scheduler_state = xTaskGetSchedulerState();
    if (scheduler_state != taskSCHEDULER_NOT_STARTED) {
        // 调度器挂起时只尝试取锁，避免进入阻塞状态。
        const TickType_t wait_ticks = (scheduler_state == taskSCHEDULER_RUNNING) ? portMAX_DELAY : 0U;

        if ((s_log_mutex == NULL) || (xSemaphoreTake(s_log_mutex, wait_ticks) != pdTRUE)) {
            return;
        }
        locked = pdTRUE;
    }

    printf("%s[%s] ", s_log_colors[level], tag);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("%s\r\n", LOG_COLOR_RESET);

    if (locked == pdTRUE) {
        (void)xSemaphoreGive(s_log_mutex);
    }
}
#endif
