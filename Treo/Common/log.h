#ifndef AURON_LOG_H_
#define AURON_LOG_H_

#include <stdint.h>
#include <stdio.h>


// 日志等级，数值越大输出越详细
#define LOG_LEVEL_NONE            0U
#define LOG_LEVEL_ERROR           1U
#define LOG_LEVEL_WARN            2U
#define LOG_LEVEL_INFO            3U
#define LOG_LEVEL_DEBUG           4U
#define LOG_LEVEL_TRACE           5U

#ifndef PROJECT_LOG_LEVEL
#define PROJECT_LOG_LEVEL         LOG_LEVEL_TRACE
#endif

#define COLOR_RED                 "\033[31m"
#define COLOR_GREEN               "\033[32m"
#define COLOR_YELLOW              "\033[33m"
#define COLOR_BLUE                "\033[34m"
#define COLOR_MAGENTA             "\033[35m"
#define COLOR_CYAN                "\033[36m"
#define COLOR_WHITE               "\033[37m"
#define COLOR_BRIGHT_RED          "\033[91m"
#define COLOR_BRIGHT_GREEN        "\033[92m"
#define COLOR_BRIGHT_YELLOW       "\033[93m"
#define COLOR_BRIGHT_BLUE         "\033[94m"
#define COLOR_BRIGHT_MAGENTA      "\033[95m"
#define COLOR_BRIGHT_CYAN         "\033[96m"
#define COLOR_BRIGHT_WHITE        "\033[97m"
#define COLOR_RESET               "\033[0m"

#define BG_RED                    "\033[41m"
#define BG_GREEN                  "\033[42m"
#define BG_YELLOW                 "\033[43m"
#define BG_BLUE                   "\033[44m"
#define BG_MAGENTA                "\033[45m"
#define BG_CYAN                   "\033[46m"
#define BG_RESET                  "\033[49m"

#define STYLE_BOLD                "\033[1m"
#define STYLE_UNDERLINE           "\033[4m"
#define STYLE_RESET               "\033[22m"

#ifndef LOG_TAG
#define LOG_TAG                   "NONE"
#endif

// 日志后端
void Log_Init(void);
void Log_Printf(const char *color, const char *tag, const char *fmt, ...);
uint32_t Log_IsrDropCountGet(void);

#if (PROJECT_LOG_LEVEL >= LOG_LEVEL_INFO)
#define LOG_PRINT(fmt, ...) \
    Log_Printf(NULL, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    Log_Printf(COLOR_GREEN, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_PRINT(fmt, ...)       ((void)0)
#define LOG_INFO(fmt, ...)        ((void)0)
#endif

#if (PROJECT_LOG_LEVEL >= LOG_LEVEL_WARN)
#define LOG_WARN(fmt, ...) \
    Log_Printf(COLOR_YELLOW, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_WARN(fmt, ...)        ((void)0)
#endif

#if (PROJECT_LOG_LEVEL >= LOG_LEVEL_ERROR)
#define LOG_ERROR(fmt, ...) \
    Log_Printf(COLOR_RED, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_ERROR(fmt, ...)       ((void)0)
#endif

#if (PROJECT_LOG_LEVEL >= LOG_LEVEL_DEBUG)
#define LOG_DEBUG(fmt, ...) \
    Log_Printf(COLOR_WHITE, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...)       ((void)0)
#endif

#if (PROJECT_LOG_LEVEL >= LOG_LEVEL_TRACE)
#define LOG_TRACE(fmt, ...) \
    Log_Printf(COLOR_CYAN, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_TRACE(fmt, ...)       ((void)0)
#endif

#define LOG(fmt, ...)             LOG_PRINT(fmt, ##__VA_ARGS__)
#define LOG_PRINT_COLOR(_color, fmt, ...) \
    Log_Printf(_color, LOG_TAG, fmt, ##__VA_ARGS__)

#endif /* AURON_LOG_H_ */
