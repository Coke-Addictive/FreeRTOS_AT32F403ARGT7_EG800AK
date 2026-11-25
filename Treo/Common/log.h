#ifndef LOG_H_
#define LOG_H_

#include <stdio.h>

// 日志等级，数值越大越详细。
typedef enum {
    LOG_LEVEL_ERROR = 0,        // 当前操作失败。
    LOG_LEVEL_WARN,             // 出现异常，当前操作仍可继续。
    LOG_LEVEL_SYSCYCLE,         // 设备启动、睡眠和唤醒等运行状态切换。
    LOG_LEVEL_INFO,             // 正常运行中的关键事件。
    LOG_LEVEL_DEBUG             // 调试细节
} LogLevel_e;

#ifndef LOG_DEFAULT_LEVEL
#define LOG_DEFAULT_LEVEL       LOG_LEVEL_DEBUG     // 允许输出的最高日志等级。
#endif

#ifndef LOG_MAIN_ENABLE
#define LOG_MAIN_ENABLE         1                   // 日志总开关，1 为开启，0 为关闭。
#endif

#ifndef LOG_SUB_ENABLE
#define LOG_SUB_ENABLE          0                   // 日志子开关，1 为开启，0 为关闭。
#endif


#ifndef LOG_TAG
#define LOG_TAG                 "NONE"
#endif



// 日志等级对应的 ANSI 前景色。
#define LOG_COLOR_ERROR         "\x1B[31m"          // ERROR     日志使用红色。
#define LOG_COLOR_WARN          "\x1B[33m"          // WARN      日志使用黄色。
#define LOG_COLOR_SYSCYCLE      "\x1B[95m"          // SYSCYCLE  日志使用紫色。
#define LOG_COLOR_INFO          "\x1B[32m"          // INFO      日志使用绿色。
#define LOG_COLOR_DEBUG         "\x1B[37m"          // DEBUG     日志使用白色。
#define LOG_COLOR_RESET         "\x1B[0m"           // 一条日志输出完成后恢复终端样式。



// 调度器启动前初始化日志。
void Log_Init(void);

#if LOG_MAIN_ENABLE
// 日志宏共用的整行输出函数，仅供任务上下文调用。
// 业务代码请勿直接调用此接口，请使用 LOG() 或各等级日志宏。
void Log_Printf(LogLevel_e level, const char *tag, const char *fmt, ...);
#endif

#if LOG_MAIN_ENABLE && LOG_SUB_ENABLE
// 在调用点判断等级，过滤时不计算打印参数。
#define LOG_PRINT_LEVEL(level, ...) \
    do { \
        if ((level) <= LOG_DEFAULT_LEVEL) { \
            Log_Printf((level), LOG_TAG, __VA_ARGS__); \
        } \
    } while (0)
#else
#define LOG_PRINT_LEVEL(level, ...) ((void)0)    // 总开关或子开关关闭时丢弃打印调用。
#endif

// 各等级日志入口。
#define LOG_ERROR(...)    LOG_PRINT_LEVEL(LOG_LEVEL_ERROR, __VA_ARGS__)     // 输出 ERROR 日志。
#define LOG_WARN(...)     LOG_PRINT_LEVEL(LOG_LEVEL_WARN, __VA_ARGS__)      // 输出 WARN 日志。
#define LOG_SYSCYCLE(...) LOG_PRINT_LEVEL(LOG_LEVEL_SYSCYCLE, __VA_ARGS__)  // 输出设备状态切换日志。
#define LOG_INFO(...)     LOG_PRINT_LEVEL(LOG_LEVEL_INFO, __VA_ARGS__)      // 输出 INFO 日志。
#define LOG_DEBUG(...)    LOG_PRINT_LEVEL(LOG_LEVEL_DEBUG, __VA_ARGS__)     // 输出 DEBUG 日志。
#define LOG(...)          LOG_DEBUG(__VA_ARGS__)                            // DEBUG 日志的简写。

#endif // LOG_H_
