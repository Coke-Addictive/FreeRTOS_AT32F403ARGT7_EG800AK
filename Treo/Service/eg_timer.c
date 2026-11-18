#include "eg_timer.h"

#include <stdio.h>

#define EG800_TIMER_AT_TIMEOUT_MS               3000U           // CTZU/CCLK 手册最大响应 300 ms，软件等待预留至 3 s
#define EG800_TIMER_CLOCK_RESPONSE_SIZE         64U             // CCLK 响应缓冲区容量，单位字节

// Timer 层内部 AT 命令编号。
typedef enum {
    EG800_TIMER_CMD_CTZU = 0U,                         // 配置网络时间自动更新
    EG800_TIMER_CMD_CCLK                               // 查询模块 RTC
} Eg800TimerCmd_e;




static const uint8_t field_offsets[] = {0U, 3U, 6U, 9U, 12U, 15U, 18U};
static const uint8_t month_days[] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};


//--------------------------------------------------------工具函数-----------------------------------------
/**
 * @brief 将普通文本 AT 请求提交到 Service，同步等待执行结果。
 */
static Eg800AtResult_e Eg800_Timer_At_Execute(Eg800TimerCmd_e cmd_id, const char *name, const char *tx, const char *rsp_prefix, char *rx_buf, size_t rx_buf_size, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {
    Eg800AtRequest_t request = {0};

    request.cmd.owner = EG800_AT_OWNER_TIMER;
    request.cmd.id = (uint8_t)cmd_id;
    request.flow = EG800_AT_FLOW_TEXT;
    request.name = name;
    request.tx = tx;
    request.rsp_prefix = rsp_prefix;
    request.rx_buf = rx_buf;
    request.rx_buf_size = rx_buf_size;
    request.final_timeout_ms = EG800_TIMER_AT_TIMEOUT_MS;

    return Eg800_AT_Execute(&request, result_info, completion_sem);
}

/**
 * @brief 解析 +CCLK: "yy/MM/dd,hh:mm:ss+zz"，校验完整格式和公历日期。
 * @note 时区可为负数；只有全部校验通过后才更新输出。
 */
static bool Eg800_Timer_Parse_Clock(const char *line, Eg800TimerClock_t *clock) {
    const char *start;
    const char *end;
    const char *tail;
    uint8_t fields[7];
    size_t index;
    uint8_t days;
    int timezone;
    Eg800TimerClock_t parsed_clock = {0};

    if ((line == NULL) || (clock == NULL) || (strncmp(line, "+CCLK:", 6U) != 0)) {
        return false;
    }

    start = line + 6U;
    while ((*start == ' ') || (*start == '\t')) {
        start++;
    }
    if (*start != '"') {
        return false;
    }
    start++;
    end = strchr(start, '"');
    if ((end == NULL) || ((size_t)(end - start) != 20U)) {
        return false;
    }

    tail = end + 1;
    while ((*tail == ' ') || (*tail == '\t') || (*tail == '\r') || (*tail == '\n')) {
        tail++;
    }
    if ((*tail != '\0') || (start[2] != '/') || (start[5] != '/') || (start[8] != ',') || (start[11] != ':') || (start[14] != ':') || ((start[17] != '+') && (start[17] != '-'))) {
        return false;
    }

    // 固定两位十进制字段，避免将缺位、符号或空白误当作有效数字。
    for (index = 0U; index < ARRAY_SIZE(field_offsets); index++) {
        const char *field = start + field_offsets[index];
        if ((field[0] < '0') || (field[0] > '9') || (field[1] < '0') || (field[1] > '9')) {
            return false;
        }
        fields[index] = (uint8_t)((field[0] - '0') * 10 + (field[1] - '0'));
    }

    timezone = (start[17] == '-') ? -(int)fields[6] : (int)fields[6];
    if ((fields[1] < 1U) || (fields[1] > 12U) || (fields[2] < 1U) || (fields[3] > 23U) || (fields[4] > 59U) || (fields[5] > 59U) || (timezone < -48) || (timezone > 56)) {
        return false;
    }

    parsed_clock.year = (uint16_t)(2000U + fields[0]);
    days = month_days[fields[1] - 1U];
    // 本接口只解释 2000~2099 年，此范围内可被 4 整除的年份均为闰年。
    if ((fields[1] == 2U) && ((parsed_clock.year % 4U) == 0U)) {
        days = 29U;
    }
    if (fields[2] > days) {
        return false;
    }

    parsed_clock.month = fields[1];
    parsed_clock.day = fields[2];
    parsed_clock.hour = fields[3];
    parsed_clock.minute = fields[4];
    parsed_clock.second = fields[5];
    parsed_clock.timezone_quarters = (int8_t)timezone;
    *clock = parsed_clock;
    return true;
}








//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
/**
 * @brief 配置 NITZ 自动更新模式，仅接受手册支持的 0、1、3。
 * @return 参数无效返回 PARAM_ERROR，其余返回 Service 的 AT 执行结果。
 * @note 配置自动保存，重启模块后生效；本接口不主动重启模块。
 */
Eg800AtResult_e Eg800_Timer_Set_Auto_Update(Eg800TimerUpdateMode_e mode, SemaphoreHandle_t completion_sem) {
    char command[16];
    int command_len;

    if ((completion_sem == NULL) || ((mode != EG800_TIMER_AUTO_UPDATE_DISABLED) && (mode != EG800_TIMER_AUTO_UPDATE_GMT) && (mode != EG800_TIMER_AUTO_UPDATE_LOCAL))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+CTZU=%u\r\n", (unsigned int)mode);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Timer_At_Execute(EG800_TIMER_CMD_CTZU, "配置网络时间自动更新", command, NULL, NULL, 0U, NULL, completion_sem);
}

/**
 * @brief 查询模块本地时间，保留时区，不进行时区换算或设置 MCU RTC。
 * @return 格式错误、非法日期或响应截断返回 DATA_FAIL；AT 失败返回原错误码。
 * @note clock 仅在查询和解析均成功时更新，失败时保持原值。
 */
Eg800AtResult_e Eg800_Timer_Query_Clock(Eg800TimerClock_t *clock, SemaphoreHandle_t completion_sem) {
    char response[EG800_TIMER_CLOCK_RESPONSE_SIZE] = {0};
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if ((clock == NULL) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    result = Eg800_Timer_At_Execute(EG800_TIMER_CMD_CCLK, "查询模块时间", "AT+CCLK?\r\n", "+CCLK:", response, sizeof(response), &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if ((result_info.rx_len >= (sizeof(response) - 1U)) || (Eg800_Timer_Parse_Clock(response, clock) == false)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    return EG800_AT_RESULT_OK;
}
