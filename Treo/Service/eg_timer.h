#ifndef EG_TIMER_H_
#define EG_TIMER_H_

#include "eg_service.h"

// AT+CTZU 支持的网络时间自动更新模式。
typedef enum {
    EG800_TIMER_AUTO_UPDATE_DISABLED = 0U,         // 禁用 NITZ 自动更新时区
    EG800_TIMER_AUTO_UPDATE_GMT = 1U,              // 更新时区并将 GMT 时间上报至 URC
    EG800_TIMER_AUTO_UPDATE_LOCAL = 3U             // 更新时区并将本地时间写入模块 RTC
} Eg800TimerUpdateMode_e;

// AT+CCLK? 返回的模块本地时间，时区保留模块原始单位。
typedef struct {
    uint16_t year;                                // 年：2000~2099，按 2000 + yy 解释
    uint8_t month;                                // 月：1~12
    uint8_t day;                                  // 日：1~31，按月份和闰年校验
    uint8_t hour;                                 // 时：0~23
    uint8_t minute;                               // 分：0~59
    uint8_t second;                               // 秒：0~59
    int8_t timezone_quarters;                     // 本地时间相对 GMT 的偏移：-48~+56，单位 15 分钟；+32 即 UTC+8
} Eg800TimerClock_t;


// AT+CTZU=<mode>：配置自动保存，重启模块后生效；OK 只表示配置成功，不表示已获得网络时间。
Eg800AtResult_e Eg800_Timer_Set_Auto_Update(Eg800TimerUpdateMode_e mode, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Timer_Query_Clock(Eg800TimerClock_t *clock, SemaphoreHandle_t completion_sem);

#endif // EG_TIMER_H_
