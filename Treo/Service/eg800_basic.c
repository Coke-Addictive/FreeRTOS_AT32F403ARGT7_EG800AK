#include "eg800_basic.h"


#define LOG_TAG "EG_BASIC"
#include "log.h"


#include "eg_service.h"


#include "stdbool.h"


#include "FreeRTOS.h"
#include "task.h"





// Basic层内部AT命令编号，用于填充Eg800AtRequest_t.cmd.id
typedef enum {
    EG800_BASIC_CMD_ATE0 = 0U,      // 关闭 AT 回显
    EG800_BASIC_CMD_URC_PORT,       // 配置 URC 上报端口
    EG800_BASIC_CMD_URC_IND,        // 开启 URC 指示
    EG800_BASIC_CMD_RI_SIGNAL,      // 配置 RI 信号类型
    EG800_BASIC_CMD_RI_OTHER,       // 配置 其他URC 的 RI 行为
    EG800_BASIC_CMD_URC_DELAY,      // 配置 URC 延迟
    EG800_BASIC_CMD_CPIN,           // 查询 SIM 状态
    EG800_BASIC_CMD_CSQ,            // 查询信号强度
    EG800_BASIC_CMD_IMEI,           // 查询 IMEI
    EG800_BASIC_CMD_IMSI,           // 查询 IMSI
    EG800_BASIC_CMD_ICCID,          // 查询 ICCID
    EG800_BASIC_CMD_CNUM,           // 查询本机号码
    EG800_BASIC_CMD_COPS,           // 查询运营商
    EG800_BASIC_CMD_GMR             // 查询模块固件版本
} Eg800BasicCmd_e;



static bool s_eg800_basic_initialized = false;   // Basic 层是初始化标志位
static bool s_eg800_basic_boot_ready = false;    // 本次冷启动周期是否已经收到RDY


static Eg800BasicInfo_t s_eg800_basic_info;      // Basic 层保存的模块基础信息







// 实际处理URC的函数声明
static void Eg800_Basic_Ready_Urc_Handler(const char *line);
static void Eg800_Basic_Power_Down_Urc_Handler(const char *line);
static void Eg800_Basic_Sim_Urc_Handler(const char *line);


// 路由表需要在声明之后
static const Eg800UrcRegistration_t   s_eg800_basic_urc_routes[] = {
    {"RDY",          EG800_AT_OWNER_BASIC,  Eg800_Basic_Ready_Urc_Handler},
    {"POWERED DOWN", EG800_AT_OWNER_BASIC,  Eg800_Basic_Power_Down_Urc_Handler},
    {"POWER DOWN",   EG800_AT_OWNER_BASIC,  Eg800_Basic_Power_Down_Urc_Handler},
    {"+CPIN:",       EG800_AT_OWNER_BASIC,  Eg800_Basic_Sim_Urc_Handler},
    // {"+CFUN:",       EG800_AT_OWNER_BASIC,  NULL},
    // {"+QUSIM:",      EG800_AT_OWNER_BASIC,  NULL},
    // {"+QIND:",       EG800_AT_OWNER_BASIC,  NULL},
};






// ------------------------ 函数声明 -----------------------------------------------------
// URC 注册
static Result_t Eg800_Basic_Register_Urcs(void);







//--------------------------------------------------------URC 处理-----------------------------------------
static void Eg800_Basic_Ready_Urc_Handler(const char *line) {
    LOG("处理READY");
}
static void Eg800_Basic_Power_Down_Urc_Handler(const char *line) {
    LOG("处理POWER DOWN");
}
static void Eg800_Basic_Sim_Urc_Handler(const char *line) {
    LOG("处理SIM URC");
}




//--------------------------------------------------------函数实体-----------------------------------------
/**
 * @brief 注册 Basic 层负责的全部主动上报 URC。
 * @return RESULT_SUCCESS 表示全部注册成功，RESULT_FAIL 表示存在注册失败。
 */
static Result_t Eg800_Basic_Register_Urcs(void) {
    return Eg800_Urc_Register_Table(s_eg800_basic_urc_routes, ARRAY_SIZE(s_eg800_basic_urc_routes));
}










//---------------------------------------------------工具函数------------------------------------------
/**
 * @brief 将 Basic 层保存的信息恢复为未知状态。
 */
static void Eg800_Basic_Info_Reset(void) {
    (void)memset(&s_eg800_basic_info, 0, sizeof(s_eg800_basic_info));
    s_eg800_basic_info.sim_state = EG800_BASIC_SIM_STATE_UNKNOWN;
    s_eg800_basic_info.csq = 99U;
    s_eg800_basic_info.ber = 99U;
    s_eg800_basic_info.operator_type = EG800_BASIC_OPERATOR_UNKNOWN;
}


/**
 * @brief 判断状态文本是否与期望文本精确匹配。
 * @param text 待判断文本。
 * @param expect 期望文本。
 * @return true表示匹配，false表示不匹配。
 */
static bool Eg800_Basic_Text_Equals(const char *text, const char *expect) {
    size_t expect_len;
    const char *end;

    if ((text == NULL) || (expect == NULL)) {
        return false;
    }

    expect_len = strlen(expect);

    if (strncmp(text, expect, expect_len) != 0) {
        return false;
    }

    end = text + expect_len;

    while ((*end == ' ') || (*end == '\t') || (*end == '\r') || (*end == '\n')) {
        end++;
    }

    return *end == '\0';
}







//--------------------------------------------------------处理解析---------------------------------------------
/**
 * @brief 解析 CPIN 响应或查询错误对应的 SIM 状态。
 * @param line CPIN响应或URC文本，可为NULL。
 * @param result AT执行结果。
 * @param module_error 模块CME错误码，未知时为-1。
 * @return 解析得到的SIM状态。
 */
static Eg800BasicSimState_e Eg800_Basic_Parse_Sim_State(const char *line, Eg800AtResult_e result, int16_t module_error) {
    const char *state;

    if ((result == EG800_AT_RESULT_CME_ERROR) && (module_error == EG800_BASIC_CME_SIM_NOT_INSERTED)) {
        return EG800_BASIC_SIM_STATE_NOT_INSERTED;
    }

    if (line == NULL) {
        return EG800_BASIC_SIM_STATE_UNKNOWN;
    }

    state = strchr(line, ':');

    if (state == NULL) {
        return EG800_BASIC_SIM_STATE_UNKNOWN;
    }

    state++;

    while ((*state == ' ') || (*state == '\t')) {
        state++;
    }

    if (Eg800_Basic_Text_Equals(state, "READY") == true) {
        return EG800_BASIC_SIM_STATE_READY;
    }

    if (Eg800_Basic_Text_Equals(state, "NOT INSERTED") == true) {
        return EG800_BASIC_SIM_STATE_NOT_INSERTED;
    }

    if (Eg800_Basic_Text_Equals(state, "NOT READY") == true) {
        return EG800_BASIC_SIM_STATE_NOT_READY;
    }

    return EG800_BASIC_SIM_STATE_UNKNOWN;
}




/**
 * @brief 解析 +CSQ 响应或主动上报。
 * @param line 完整的 +CSQ 文本行。
 * @param csq 信号强度输出，范围0~31或99。
 * @param ber 误码率输出，范围0~7或99。
 * @return true表示解析成功，false表示格式或数值错误。
 */
static bool Eg800_Basic_Parse_Signal(const char *line, uint8_t *csq, uint8_t *ber) {
    const char *cursor;
    uint16_t csq_value = 0U;
    uint16_t ber_value = 0U;
    bool has_digit = false;

    if ((line == NULL) || (csq == NULL) || (ber == NULL)) {
        return false;
    }

    cursor = strchr(line, ':');
    if (cursor == NULL) {
        return false;
    }

    cursor++;
    while ((*cursor == ' ') || (*cursor == '\t')) {
        cursor++;
    }

    while ((*cursor >= '0') && (*cursor <= '9')) {
        has_digit = true;
        csq_value = (uint16_t)((csq_value * 10U) + (uint16_t)(*cursor - '0'));
        if (csq_value > 99U) {
            return false;
        }
        cursor++;
    }

    if (has_digit == false) {
        return false;
    }

    while ((*cursor == ' ') || (*cursor == '\t')) {
        cursor++;
    }

    if (*cursor != ',') {
        return false;
    }

    cursor++;
    while ((*cursor == ' ') || (*cursor == '\t')) {
        cursor++;
    }

    has_digit = false;

    while ((*cursor >= '0') && (*cursor <= '9')) {
        has_digit = true;
        ber_value = (uint16_t)((ber_value * 10U) + (uint16_t)(*cursor - '0'));
        if (ber_value > 99U) {
            return false;
        }
        cursor++;
    }

    if (has_digit == false) {
        return false;
    }

    while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == '\r') || (*cursor == '\n')) {
        cursor++;
    }

    if (*cursor != '\0') {
        return false;
    }

    if (((csq_value > 31U) && (csq_value != 99U)) || ((ber_value > 7U) && (ber_value != 99U))) {
        return false;
    }

    *csq = (uint8_t)csq_value;
    *ber = (uint8_t)ber_value;

    return true;
}




/**
 * @brief 根据 COPS 返回的运营商名称识别运营商类型。
 */
static Eg800BasicOperator_e Eg800_Basic_Parse_Operator(const char *operator_name) {
    if ((operator_name == NULL) || (operator_name[0] == '\0')) {
        return EG800_BASIC_OPERATOR_UNKNOWN;
    }

    if (strstr(operator_name, "CHINA MOBILE") != NULL) {
        return EG800_BASIC_OPERATOR_CHINA_MOBILE;
    }

    if (strstr(operator_name, "CHN-CT") != NULL) {
        return EG800_BASIC_OPERATOR_CHINA_TELECOM;
    }

    if (strstr(operator_name, "CHN-UNICOM") != NULL) {
        return EG800_BASIC_OPERATOR_CHINA_UNICOM;
    }

    return EG800_BASIC_OPERATOR_UNKNOWN;
}



















//--------------------------------------------------------公开的API接口--------------------------------------------------------
/**
 * @brief 初始化 Basic 层并注册基础 URC。
 * @return RESULT_SUCCESS 表示初始化成功。
 * @note 本函数不发送 AT 命令，可以在模块上电前调用。
 */
Result_t Eg800_Basic_Init(void) {
    // 幂等检查
    if (s_eg800_basic_initialized == true) {
        return RESULT_SUCCESS;
    }

    Eg800_Basic_Info_Reset();
    s_eg800_basic_boot_ready = false;

    // 注册URC
    if (Eg800_Basic_Register_Urcs() != RESULT_SUCCESS) {
        return RESULT_FAIL;
    }


    s_eg800_basic_initialized = true;

    return RESULT_SUCCESS;
}


