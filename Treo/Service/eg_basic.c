#include "eg_basic.h"

#define LOG_TAG "EG_BASIC"
#include "log.h"

#include "eg_service.h"

#define EG800_BASIC_AT_RESPONSE_MAX_SIZE 128U               //基本AT回复长度最大值


// Basic层内部AT命令编号，用于填充Eg800AtRequest_t.cmd.id
typedef enum {
    EG800_BASIC_CMD_ATE0 = 0U, // 关闭 AT 回显
    EG800_BASIC_CMD_URC_PORT,  // 配置 URC 上报端口
    EG800_BASIC_CMD_URC_IND,   // 开启 URC 指示
    EG800_BASIC_CMD_RI_SIGNAL, // 配置 RI 信号类型
    EG800_BASIC_CMD_RI_OTHER,  // 配置 其他URC 的 RI 行为
    EG800_BASIC_CMD_URC_DELAY, // 配置 URC 延迟
    EG800_BASIC_CMD_CPIN,      // 查询 SIM 状态
    EG800_BASIC_CMD_CSQ,       // 查询信号强度
    EG800_BASIC_CMD_IMEI,      // 查询 IMEI
    EG800_BASIC_CMD_IMSI,      // 查询 IMSI
    EG800_BASIC_CMD_ICCID,     // 查询 ICCID
    EG800_BASIC_CMD_CNUM,      // 查询本机号码
    EG800_BASIC_CMD_COPS,      // 查询运营商
    EG800_BASIC_CMD_GMR        // 查询模块固件版本
} Eg800BasicCmd_e;

static bool s_eg800_basic_initialized   = false;    // Basic 层是初始化标志位
static bool s_eg800_basic_boot_ready    = false;    // 本次冷启动周期是否已经收到RDY

static Eg800BasicSnapshot_t s_eg800_basic_snapshot    = {0};        // Basic 信息缓存及刷新状态
static TaskHandle_t         s_eg800_basic_task_handle = NULL;       // Basic 后台刷新任务句柄
static SemaphoreHandle_t    s_basic_at_completion_sem = NULL;       // Basic 层 AT 请求完成信号量


// 实际处理URC的函数声明
static void Eg800_Basic_Ready_Urc_Handler(const char *line);
static void Eg800_Basic_Power_Down_Urc_Handler(const char *line);
static void Eg800_Basic_Sim_Urc_Handler(const char *line);
static void Eg800_Basic_CFUN_Urc_Handler(const char *line);

// 路由表需要在声明之后
static const Eg800UrcRegistration_t s_eg800_basic_urc_routes[] = {
    {"RDY",             EG800_AT_OWNER_BASIC,   Eg800_Basic_Ready_Urc_Handler},
    {"POWERED DOWN",    EG800_AT_OWNER_BASIC,   Eg800_Basic_Power_Down_Urc_Handler},
    {"POWER DOWN",      EG800_AT_OWNER_BASIC,   Eg800_Basic_Power_Down_Urc_Handler},
    {"+CPIN:",          EG800_AT_OWNER_BASIC,   Eg800_Basic_Sim_Urc_Handler},
    {"+CFUN:",          EG800_AT_OWNER_BASIC,   Eg800_Basic_CFUN_Urc_Handler},
    // {"+QUSIM:",      EG800_AT_OWNER_BASIC,  NULL},
    // {"+QIND:",       EG800_AT_OWNER_BASIC,  NULL},
};

// ------------------------ 函数声明 -----------------------------------------------------
// URC 注册
static Result_t Eg800_Basic_Register_Urcs(void);

// 工具
static bool Eg800_Basic_Text_Equals(const char *text, const char *expect);
static bool Eg800_Basic_Copy_Trimmed(char *dst, size_t dst_size, const char *src);
static bool Eg800_Basic_Copy_After_Colon(char *dst, size_t dst_size, const char *line);
static bool Eg800_Basic_Copy_Quoted_Field(char *dst, size_t dst_size, const char *line, uint8_t field_index);
static bool Eg800_Basic_Copy_String(char *dst, size_t dst_size, const char *src);

// 处理解析函数
static Eg800BasicSimState_e Eg800_Basic_Parse_Sim_State(const char *line, Eg800AtResult_e result, int16_t module_error);
static bool Eg800_Basic_Parse_Signal(const char *line, uint8_t *csq, uint8_t *ber);
static Eg800BasicOperator_e Eg800_Basic_Parse_Operator(const char *operator_name);

// 更新信息数据
static void Eg800_Basic_Update_String(char *dst, size_t dst_size, const char *src);

// 基础信息维护任务
static void Eg800_Basic_Task(void *parameters);


//--------------------------------------------------------URC 处理-----------------------------------------
static void Eg800_Basic_Ready_Urc_Handler(const char *line) {
    taskENTER_CRITICAL();
    s_eg800_basic_boot_ready = true;
    taskEXIT_CRITICAL();
    LOG("收到RDY,模块启动成功");
}
static void Eg800_Basic_Power_Down_Urc_Handler(const char *line) {
    LOG("处理POWER DOWN");
}
static void Eg800_Basic_Sim_Urc_Handler(const char *line) {
    Eg800BasicSimState_e state;
    // 当前处理的是URC主动上报，没有 AT 查询产生的 CME 错误码。
    state = Eg800_Basic_Parse_Sim_State(line, EG800_AT_RESULT_OK, -1);
    s_eg800_basic_snapshot.info.sim_state = state;
    if (state == EG800_BASIC_SIM_STATE_READY) {
        LOG("SIM 卡就绪");
    } else if (state == EG800_BASIC_SIM_STATE_NOT_INSERTED) {
        LOG_WARN("SIM 卡未插入");
    } else if (state == EG800_BASIC_SIM_STATE_NOT_READY) {
        LOG("SIM 卡未就绪");
    } else {
        LOG_WARN("未识别的 SIM 状态: %s", (line != NULL) ? line : "");
    }
}

static void Eg800_Basic_CFUN_Urc_Handler(const char *line) {
    char temp[10];
    Eg800_Basic_Copy_After_Colon(temp, sizeof(temp) / sizeof(temp[0]), line);
    if (temp[0] == '1') {
        LOG("当前模块ME为全功能模式");
    } else {
        LOG_ERROR("未识别");
    }
}

//---------------------------------------------------工具函数------------------------------------------
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

/**
 * @brief 复制一行文本，并去除首尾空格、制表符和引号。
 * @param dst 目标缓冲区。
 * @param dst_size 目标缓冲区大小。
 * @param src 源文本，可以指向多行响应的第一行。
 * @return true表示复制成功，false表示参数错误、内容为空或空间不足。
 */
static bool Eg800_Basic_Copy_Trimmed(char *dst, size_t dst_size, const char *src) {
    const char *start;
    const char *end;
    size_t len;

    if ((dst == NULL) || (dst_size == 0U)) {
        return false;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return false;
    }

    // 跳过当前响应行前面的空白和可能存在的引号。
    start = src;
    while ((*start == ' ') || (*start == '\t') || (*start == '\r') || (*start == '\n') || (*start == '"')) {
        start++;
    }

    // 只提取第一条非空响应行，避免把多行响应一起复制给单字段接口。
    end = start;
    while ((*end != '\0') && (*end != '\r') && (*end != '\n')) {
        end++;
    }

    while ((end > start) && ((*(end - 1) == ' ') || (*(end - 1) == '\t') || (*(end - 1) == '"'))) {
        end--;
    }

    len = (size_t)(end - start);
    if ((len == 0U) || (len >= dst_size)) {
        return false;
    }

    (void)memcpy(dst, start, len);
    dst[len] = '\0';
    return true;
}

/**
 * @brief 复制响应行冒号后面的有效文本。
 * @param dst 目标缓冲区。
 * @param dst_size 目标缓冲区大小。
 * @param line 完整响应行。
 * @return true表示复制成功，false表示失败。
 */
static bool Eg800_Basic_Copy_After_Colon(char *dst, size_t dst_size, const char *line) {
    const char *colon;

    if ((dst == NULL) || (dst_size == 0U)) {
        return false;
    }

    dst[0] = '\0';
    if (line == NULL) {
        return false;
    }

    colon = strchr(line, ':');
    if (colon == NULL) {
        return false;
    }

    return Eg800_Basic_Copy_Trimmed(dst, dst_size, colon + 1);
}

/**
 * @brief 从响应行中复制指定序号的引号字段。
 * @param dst 目标缓冲区。
 * @param dst_size 目标缓冲区大小。
 * @param line 完整响应行。
 * @param field_index 引号字段序号，从0开始。
 * @return true表示复制成功，false表示字段不存在、为空或空间不足。
 */
static bool Eg800_Basic_Copy_Quoted_Field(char *dst, size_t dst_size, const char *line, uint8_t field_index) {
    const char *start;
    const char *end;
    uint8_t index = 0U;
    size_t len;

    if ((dst == NULL) || (dst_size == 0U)) {
        return false;
    }

    dst[0] = '\0';
    if (line == NULL) {
        return false;
    }

    start = line;
    while (*start != '\0') {
        start = strchr(start, '"');
        if (start == NULL) {
            return false;
        }
        start++;

        end = strchr(start, '"');
        if (end == NULL) {
            return false;
        }

        if (index == field_index) {
            len = (size_t)(end - start);
            if ((len == 0U) || (len >= dst_size)) {
                return false;
            }

            (void)memcpy(dst, start, len);
            dst[len] = '\0';
            return true;
        }

        index++;
        start = end + 1;
    }

    return false;
}

/**
 * @brief 安全复制一个已经解析完成的字符串。
 * @return true表示复制成功，false表示参数错误、源为空或空间不足。
 */
static bool Eg800_Basic_Copy_String(char *dst, size_t dst_size, const char *src) {
    size_t len;

    if ((dst == NULL) || (dst_size == 0U)) {
        return false;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return false;
    }

    len = strlen(src);
    if ((len == 0U) || (len >= dst_size)) {
        return false;
    }

    (void)memcpy(dst, src, len + 1U);
    return true;
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
 * @brief 解析 COPS 返回的运营商名称识别运营商类型。
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

//--------------------------------------------------------关键函数实体-----------------------------------------
/**
 * @brief 将 Basic 层保存的信息恢复为未知状态。
 */
static void Eg800_Basic_Info_Reset(void) {
    (void)memset(&s_eg800_basic_snapshot, 0, sizeof(s_eg800_basic_snapshot));
    s_eg800_basic_snapshot.info.sim_state = EG800_BASIC_SIM_STATE_UNKNOWN;
    s_eg800_basic_snapshot.info.csq = 99U;
    s_eg800_basic_snapshot.info.ber = 99U;
    s_eg800_basic_snapshot.info.operator_type = EG800_BASIC_OPERATOR_UNKNOWN;
}

/**
 * @brief 注册 Basic 层负责的全部主动上报 URC。
 * @return RESULT_SUCCESS 表示全部注册成功，RESULT_FAIL 表示存在注册失败。
 */
static Result_t Eg800_Basic_Register_Urcs(void) {
    return Eg800_Urc_Register_Table(s_eg800_basic_urc_routes, ARRAY_SIZE(s_eg800_basic_urc_routes));
}

/**
 * @brief 同步执行一条 Basic 层普通文本 AT 命令。
 * @param cmd_id Basic 层内部命令编号。
 * @param name 命令日志名称，可为 NULL。
 * @param tx 完整 AT 发送字符串，必须包含 "\r\n"，调用期间保持有效。
 * @param rsp_prefix 需要保存的响应前缀，NULL 表示保存未被 URC 路由消费的文本。
 * @param rx_buf 响应接收缓冲区，调用期间保持有效；不需要响应内容时传 NULL。
 * @param rx_buf_size 响应缓冲区容量，单位字节；rx_buf 为 NULL 时必须为 0。
 * @param result_info AT 执行详细结果，可为 NULL。
 * @return Service 返回的 AT 执行结果
 * @note Basic 和 Service 必须初始化成功；仅供任务上下文串行调用，禁止在 URC 回调中调用。
 */
static Eg800AtResult_e Eg800_Basic_At_Execute(Eg800BasicCmd_e cmd_id, const char *name, const char *tx, const char *rsp_prefix, char *rx_buf, size_t rx_buf_size, Eg800AtResultInfo_t *result_info) {
    Eg800AtRequest_t request = {0};

    request.cmd.owner = EG800_AT_OWNER_BASIC;
    request.cmd.id = (uint8_t)cmd_id;
    request.flow = EG800_AT_FLOW_TEXT;

    request.name = name;
    request.tx = tx;
    request.rsp_prefix = rsp_prefix;
    request.rx_buf = rx_buf;
    request.rx_buf_size = rx_buf_size;

    // 超时字段保持为 0，使用 Service 的默认超时时间。
    return Eg800_AT_Execute(&request, result_info, s_basic_at_completion_sem);
}






//-----------------------------------------------更新信息数据-------------------------------------------------------

/**
 * @brief 在临界区内更新 Basic 信息结构中的字符串字段。
 */
static void Eg800_Basic_Update_String(char *dst, size_t dst_size, const char *src) {
    size_t len;

    if ((dst == NULL) || (dst_size == 0U) || (src == NULL)) {
        return;
    }

    len = strlen(src);
    if (len >= dst_size) {
        return;
    }

    taskENTER_CRITICAL();
    (void)memset(dst, 0, dst_size);
    (void)memcpy(dst, src, len);
    taskEXIT_CRITICAL();
}

/**
 * @brief 更新 CSQ 状态。
 */
static void Eg800_Basic_Update_Signal(uint8_t csq, uint8_t ber) {
    taskENTER_CRITICAL();
    s_eg800_basic_snapshot.info.csq = csq;
    s_eg800_basic_snapshot.info.ber = ber;
    taskEXIT_CRITICAL();
}

/**
 * @brief 更新 SIM 状态
 */
static void Eg800_Basic_Update_Sim_State(Eg800BasicSimState_e sim_state) {
    taskENTER_CRITICAL();
    s_eg800_basic_snapshot.info.sim_state = sim_state;
    taskEXIT_CRITICAL();
}



//-----------------------------------------------XXXX任务-------------------------------------------------------


/**
 * @brief 等待刷新请求并执行 Basic 信息全量刷新。
 * @param parameters 未使用。
 */
static void Eg800_Basic_Task(void *parameters) {
    Eg800BasicInfo_t info;

    (void)parameters;

    while (1) {
        // 等待首次刷新或后续过期刷新请求。
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        (void)memset(&info, 0, sizeof(info));

        (void)Eg800_Basic_Query_Sim(&info.sim_state);
        (void)Eg800_Basic_Query_Signal(&info.csq, &info.ber);
        (void)Eg800_Basic_Query_Imei(info.imei, sizeof(info.imei));
        (void)Eg800_Basic_Query_Imsi(info.imsi, sizeof(info.imsi));
        (void)Eg800_Basic_Query_Iccid(info.iccid, sizeof(info.iccid));
        (void)Eg800_Basic_Query_Phone_Number(info.phone_number, sizeof(info.phone_number));
        (void)Eg800_Basic_Query_Firmware_Version(info.firmware_version, sizeof(info.firmware_version));
        (void)Eg800_Basic_Query_Operator(&info.operator_type, info.operator_name, sizeof(info.operator_name));

        taskENTER_CRITICAL();
        s_eg800_basic_snapshot.update_tick = xTaskGetTickCount();
        s_eg800_basic_snapshot.refresh_in_progress = false;
        taskEXIT_CRITICAL();
    }
}











//--------------------------------------------------------公开的API接口--------------------------------------------------------
/**
 * @brief 初始化 Basic 层并注册基础 URC。
 * @return RESULT_SUCCESS 表示初始化成功。
 * @note 本函数不发送 AT 命令，可以在模块上电前调用。
 */
Result_t Eg800_Basic_Init(void) {
    BaseType_t task_result;

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

    // 创建basic 信号量
    s_basic_at_completion_sem = xSemaphoreCreateBinary();
    if (s_basic_at_completion_sem == NULL) {
        LOG_ERROR("Basic AT 完成信号量创建失败");
        return RESULT_FAIL;
    }

    task_result = xTaskCreate(Eg800_Basic_Task,
                          "EG Basic Info Maintain",
                          EG800_BASIC_TASK_STACK_SIZE,
                          NULL,
                          EG800_BASIC_TASK_PRIORITY,
                          &s_eg800_basic_task_handle);

    if (task_result != pdPASS) {
        s_eg800_basic_task_handle = NULL;
        LOG_ERROR("Basic 后台刷新任务创建失败");
        return RESULT_FAIL;
    }


    s_eg800_basic_initialized = true;

    return RESULT_SUCCESS;
}

// 使用ATE0确认模块能够响应AT，并关闭命令回显。
Eg800AtResult_e Eg800_Basic_Set_Echo_Close(void) {
    return Eg800_Basic_At_Execute(EG800_BASIC_CMD_ATE0, "关闭AT回显", "ATE0\r\n", NULL, NULL, 0U, NULL);
}

// 将模块 URC 上报端口设置为 UART1
Eg800AtResult_e Eg800_Basic_Set_Urc_Port_Uart1(void) {
    return Eg800_Basic_At_Execute(EG800_BASIC_CMD_URC_PORT, "设置 URC 上报口为 UART1", "AT+QURCCFG=\"urcport\",\"uart1\"\r\n", NULL, NULL, 0U, NULL);
}

// 将 RI 信号类型设置为物理引脚输出。
Eg800AtResult_e Eg800_Basic_Set_Ri_Physical(void) {
    return Eg800_Basic_At_Execute(EG800_BASIC_CMD_RI_SIGNAL, "设置 RI 物理引脚输出", "AT+QCFG=\"risignaltype\",\"physical\"\r\n", NULL, NULL, 0U, NULL);
}


/**
 * @brief 查询信号强度和误码率。
 * @param csq 信号强度输出，范围0~31或99。
 * @param ber 误码率输出，范围0~7或99。
 * @return AT命令执行结果；响应解析失败时返回DATA_FAIL。
 */
Eg800AtResult_e Eg800_Basic_Query_Signal(uint8_t *csq, uint8_t *ber) {
    char response[EG800_BASIC_AT_RESPONSE_MAX_SIZE];
    uint8_t parsed_csq;
    uint8_t parsed_ber;
    Eg800AtResult_e result;

    if ((s_eg800_basic_initialized == false) || (csq == NULL) || (ber == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    *csq = 99U;
    *ber = 99U;
    (void)memset(response, 0, sizeof(response));

    result = Eg800_Basic_At_Execute(EG800_BASIC_CMD_CSQ, "查询信号强度", "AT+CSQ\r\n", "+CSQ:", response, sizeof(response), NULL);

    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if (Eg800_Basic_Parse_Signal(response, &parsed_csq, &parsed_ber) == false) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    *csq = parsed_csq;
    *ber = parsed_ber;
    Eg800_Basic_Update_Signal(parsed_csq, parsed_ber);

    return EG800_AT_RESULT_OK;
}



/**
 * @brief 查询当前 SIM 卡状态。
 * @param sim_state SIM状态输出地址。
 * @return AT命令执行结果；响应格式错误时返回DATA_FAIL。
 */
Eg800AtResult_e Eg800_Basic_Query_Sim(Eg800BasicSimState_e *sim_state) {
    char response[EG800_BASIC_AT_RESPONSE_MAX_SIZE];
    Eg800AtResultInfo_t result_info;
    Eg800BasicSimState_e parsed_state;
    Eg800AtResult_e result;

    if ((s_eg800_basic_initialized == false) || (sim_state == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    *sim_state = EG800_BASIC_SIM_STATE_UNKNOWN; // 先定义为未知
    (void)memset(response, 0, sizeof(response));
    (void)memset(&result_info, 0, sizeof(result_info));
    result_info.module_error = -1;

    result = Eg800_Basic_At_Execute(EG800_BASIC_CMD_CPIN, "查询 SIM 状态", "AT+CPIN?\r\n", "+CPIN:", response, sizeof(response), &result_info);

    parsed_state = Eg800_Basic_Parse_Sim_State(response, result, result_info.module_error);
    *sim_state = parsed_state;

    if (parsed_state != EG800_BASIC_SIM_STATE_UNKNOWN) {
        Eg800_Basic_Update_Sim_State(parsed_state);
    }

    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if (parsed_state == EG800_BASIC_SIM_STATE_UNKNOWN) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    return EG800_AT_RESULT_OK;
}


/**
 * @brief 查询模块IMEI。
 * @param imei IMEI输出缓冲区。
 * @param imei_size 输出缓冲区大小。
 * @return AT命令执行结果；响应为空或空间不足时返回DATA_FAIL。
 */
Eg800AtResult_e Eg800_Basic_Query_Imei(char *imei, size_t imei_size) {
    char response[EG800_BASIC_AT_RESPONSE_MAX_SIZE];
    char parsed_imei[EG800_BASIC_IMEI_SIZE];
    Eg800AtResult_e result;

    if ((s_eg800_basic_initialized == false) || (imei == NULL) || (imei_size == 0U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    imei[0] = '\0';
    (void)memset(response, 0, sizeof(response));
    (void)memset(parsed_imei, 0, sizeof(parsed_imei));

    // AT+CGSN没有固定响应前缀，Service会保存非URC的文本行。
    result = Eg800_Basic_At_Execute(EG800_BASIC_CMD_IMEI, "查询 IMEI 号", "AT+CGSN\r\n", NULL, response, sizeof(response), NULL);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if ((Eg800_Basic_Copy_Trimmed(parsed_imei, sizeof(parsed_imei), response) == false) ||
        (Eg800_Basic_Copy_String(imei, imei_size, parsed_imei) == false)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    Eg800_Basic_Update_String(s_eg800_basic_snapshot.info.imei, sizeof(s_eg800_basic_snapshot.info.imei), parsed_imei);
    return EG800_AT_RESULT_OK;
}



/**
 * @brief 查询SIM卡IMSI。
 * @param imsi IMSI输出缓冲区。
 * @param imsi_size 输出缓冲区大小。
 * @return AT命令执行结果；响应为空或空间不足时返回DATA_FAIL。
 */
Eg800AtResult_e Eg800_Basic_Query_Imsi(char *imsi, size_t imsi_size) {
    char response[EG800_BASIC_AT_RESPONSE_MAX_SIZE];
    char parsed_imsi[EG800_BASIC_IMSI_SIZE];
    Eg800AtResult_e result;

    if ((s_eg800_basic_initialized == false) || (imsi == NULL) || (imsi_size == 0U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    imsi[0] = '\0';
    (void)memset(response, 0, sizeof(response));
    (void)memset(parsed_imsi, 0, sizeof(parsed_imsi));

    result = Eg800_Basic_At_Execute(EG800_BASIC_CMD_IMSI, "查询 IMSI 号", "AT+CIMI\r\n", NULL, response, sizeof(response), NULL);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if ((Eg800_Basic_Copy_Trimmed(parsed_imsi, sizeof(parsed_imsi), response) == false) ||
        (Eg800_Basic_Copy_String(imsi, imsi_size, parsed_imsi) == false)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    Eg800_Basic_Update_String(s_eg800_basic_snapshot.info.imsi, sizeof(s_eg800_basic_snapshot.info.imsi), parsed_imsi);
    return EG800_AT_RESULT_OK;
}

/**
 * @brief 查询SIM卡ICCID。
 * @param iccid ICCID输出缓冲区。
 * @param iccid_size 输出缓冲区大小。
 * @return AT命令执行结果；响应格式错误或空间不足时返回DATA_FAIL。
 */
Eg800AtResult_e Eg800_Basic_Query_Iccid(char *iccid, size_t iccid_size) {
    char response[EG800_BASIC_AT_RESPONSE_MAX_SIZE];
    char parsed_iccid[EG800_BASIC_ICCID_SIZE];
    Eg800AtResult_e result;

    if ((s_eg800_basic_initialized == false) || (iccid == NULL) || (iccid_size == 0U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    iccid[0] = '\0';
    (void)memset(response, 0, sizeof(response));
    (void)memset(parsed_iccid, 0, sizeof(parsed_iccid));

    result = Eg800_Basic_At_Execute(EG800_BASIC_CMD_ICCID, "查询 ICCID 号", "AT+QCCID\r\n", "+QCCID:", response, sizeof(response), NULL);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if ((Eg800_Basic_Copy_After_Colon(parsed_iccid, sizeof(parsed_iccid), response) == false) ||
        (Eg800_Basic_Copy_String(iccid, iccid_size, parsed_iccid) == false)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    Eg800_Basic_Update_String(s_eg800_basic_snapshot.info.iccid, sizeof(s_eg800_basic_snapshot.info.iccid), parsed_iccid);
    return EG800_AT_RESULT_OK;
}


/**
 * @brief 查询SIM卡本机号码。
 * @param phone_number 本机号码输出缓冲区。
 * @param phone_number_size 输出缓冲区大小。
 * @return AT命令执行结果；未配置号码或解析失败时返回DATA_FAIL。
 * @note 部分SIM卡没有写入本机号码，此时模块可能只返回OK或返回空字段。
 */
Eg800AtResult_e Eg800_Basic_Query_Phone_Number(char *phone_number, size_t phone_number_size) {
    char response[EG800_BASIC_AT_RESPONSE_MAX_SIZE];
    char parsed_number[EG800_BASIC_PHONE_NUMBER_SIZE];
    Eg800AtResult_e result;

    if ((s_eg800_basic_initialized == false) || (phone_number == NULL) || (phone_number_size == 0U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    phone_number[0] = '\0';
    (void)memset(response, 0, sizeof(response));
    (void)memset(parsed_number, 0, sizeof(parsed_number));

    result = Eg800_Basic_At_Execute(EG800_BASIC_CMD_CNUM, "查询本机号码", "AT+CNUM\r\n", "+CNUM:", response, sizeof(response), NULL);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // +CNUM的第0个引号字段是名称，第1个引号字段才是电话号码。
    if ((Eg800_Basic_Copy_Quoted_Field(parsed_number, sizeof(parsed_number), response, 1U) == false) ||
        (Eg800_Basic_Copy_String(phone_number, phone_number_size, parsed_number) == false)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    Eg800_Basic_Update_String(s_eg800_basic_snapshot.info.phone_number, sizeof(s_eg800_basic_snapshot.info.phone_number), parsed_number);
    return EG800_AT_RESULT_OK;
}




/**
 * @brief 查询模块固件版本。
 * @param version 固件版本输出缓冲区。
 * @param version_size 输出缓冲区大小。
 * @return AT命令执行结果；响应为空或空间不足时返回DATA_FAIL。
 * @note 当前保存AT+GMR返回的第一条非空版本信息行。
 */
Eg800AtResult_e Eg800_Basic_Query_Firmware_Version(char *version, size_t version_size) {
    char response[EG800_BASIC_AT_RESPONSE_MAX_SIZE];
    char parsed_version[EG800_BASIC_FIRMWARE_VERSION_SIZE];
    Eg800AtResult_e result;

    if ((s_eg800_basic_initialized == false) || (version == NULL) || (version_size == 0U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    version[0] = '\0';
    (void)memset(response, 0, sizeof(response));
    (void)memset(parsed_version, 0, sizeof(parsed_version));

    result = Eg800_Basic_At_Execute(EG800_BASIC_CMD_GMR, "查询固件版本", "AT+GMR\r\n", NULL, response, sizeof(response), NULL);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if ((Eg800_Basic_Copy_Trimmed(parsed_version, sizeof(parsed_version), response) == false) ||
        (Eg800_Basic_Copy_String(version, version_size, parsed_version) == false)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    Eg800_Basic_Update_String(s_eg800_basic_snapshot.info.firmware_version, sizeof(s_eg800_basic_snapshot.info.firmware_version), parsed_version);
    return EG800_AT_RESULT_OK;
}



/**
 * @brief 查询当前注册运营商。
 * @param operator_type 运营商枚举输出地址。
 * @param operator_name 运营商原始名称输出缓冲区。
 * @param operator_name_size 输出缓冲区大小。
 * @return AT命令执行结果；响应格式错误或空间不足时返回DATA_FAIL。
 */
Eg800AtResult_e Eg800_Basic_Query_Operator(Eg800BasicOperator_e *operator_type, char *operator_name, size_t operator_name_size) {
    char response[EG800_BASIC_AT_RESPONSE_MAX_SIZE];
    char parsed_name[EG800_BASIC_OPERATOR_NAME_SIZE];
    Eg800BasicOperator_e parsed_type;
    Eg800AtResult_e result;
    char fields[EG800_BASIC_AT_RESPONSE_MAX_SIZE];

    if ((s_eg800_basic_initialized == false) || (operator_type == NULL) || (operator_name == NULL) || (operator_name_size == 0U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    *operator_type = EG800_BASIC_OPERATOR_UNKNOWN;
    operator_name[0] = '\0';
    (void)memset(response, 0, sizeof(response));
    (void)memset(parsed_name, 0, sizeof(parsed_name));

    result = Eg800_Basic_At_Execute(EG800_BASIC_CMD_COPS, "查询运营商", "AT+COPS?\r\n", "+COPS:", response, sizeof(response), NULL);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // 提取冒号后的有效文本；内部已完成首尾清理，无需再次调用 Copy_Trimmed。
    if (Eg800_Basic_Copy_After_Colon(fields, sizeof(fields), response) == false) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if (Eg800_Basic_Text_Equals(fields, "0") == true) {
        // 本次响应未提供运营商名称，明确清空解析结果和调用者输出。
        parsed_type = EG800_BASIC_OPERATOR_UNKNOWN;
        parsed_name[0] = '\0';
        operator_name[0] = '\0';
        LOG_WARN("当前未返回运营商信息");
    } else {
        if ((Eg800_Basic_Copy_Quoted_Field(parsed_name, sizeof(parsed_name), response, 0U) == false) ||
                (Eg800_Basic_Copy_String(operator_name, operator_name_size, parsed_name) == false)) {
            return EG800_AT_RESULT_DATA_FAIL;
        }
        parsed_type = Eg800_Basic_Parse_Operator(parsed_name);
    }

    *operator_type = parsed_type;

    // 名称即使无法归类，也作为原始信息保留，供日志和后续扩展使用。
    taskENTER_CRITICAL();
    s_eg800_basic_snapshot.info.operator_type = parsed_type;
    (void)memset(s_eg800_basic_snapshot.info.operator_name, 0, sizeof(s_eg800_basic_snapshot.info.operator_name));
    (void)memcpy(s_eg800_basic_snapshot.info.operator_name, parsed_name, strlen(parsed_name));
    taskEXIT_CRITICAL();

    return EG800_AT_RESULT_OK;
}











/**
 * @brief 查询本次冷启动周期是否已经收到RDY。
 */
bool Eg800_Basic_Is_Boot_Ready(void) {
    bool ready;
    if (s_eg800_basic_initialized == false) {
        return false;
    }

    taskENTER_CRITICAL();
    ready = s_eg800_basic_boot_ready;
    taskEXIT_CRITICAL();

    return ready;
}


/**
 * @brief 冷启动前清除上一次启动周期留下的RDY标志。
 */
void Eg800_Basic_Boot_Ready_Clear(void) {
    taskENTER_CRITICAL();
    s_eg800_basic_boot_ready = false;
    taskEXIT_CRITICAL();
}







/**
 * @brief 获取 Basic 信息缓存，并在缓存过期时触发后台全量刷新。
 * @param snapshot Basic 信息快照输出地址。
 * @return RESULT_SUCCESS 表示成功取得缓存，RESULT_FAIL 表示参数错误或未初始化。
 * @note 本接口立即返回，不等待后台刷新完成。
 */
Result_t Eg800_Basic_Get_Snapshot(Eg800BasicSnapshot_t *snapshot) {
    TickType_t current_tick;    // 当前tick时间
    TickType_t elapsed_tick;    // 已度过的tick时间
    bool need_refresh;


    if ((s_eg800_basic_initialized == false) || (snapshot == NULL)) {
        return RESULT_FAIL;
    }

    current_tick = xTaskGetTickCount();

    taskENTER_CRITICAL();

    elapsed_tick = current_tick - s_eg800_basic_snapshot.update_tick;
    need_refresh = (s_eg800_basic_snapshot.refresh_in_progress == false) && ((s_eg800_basic_snapshot.update_tick == 0U) ||
                    (elapsed_tick >= pdMS_TO_TICKS(EG800_BASIC_REFRESH_INTERVAL_MS)));

    taskEXIT_CRITICAL();

    if (need_refresh == true) {
        (void)Eg800_Basic_Request_Refresh();
    }

    taskENTER_CRITICAL();
    *snapshot = s_eg800_basic_snapshot;
    taskEXIT_CRITICAL();

    return RESULT_SUCCESS;
}



/**
 * @brief 强制请求一次 Basic 信息后台全量刷新。
 * @return RESULT_SUCCESS 表示刷新请求已接受，RESULT_FAIL 表示未初始化。
 * @note 返回成功不表示刷新已经完成；正在刷新时重复调用不会再次启动。
 */
Result_t Eg800_Basic_Request_Refresh(void) {
    bool need_notify = false;

    if ((s_eg800_basic_initialized == false) || (s_eg800_basic_task_handle == NULL)) {
        return RESULT_FAIL;
    }


    taskENTER_CRITICAL();

    if (s_eg800_basic_snapshot.refresh_in_progress == false) {
        s_eg800_basic_snapshot.refresh_in_progress = true;
        need_notify = true;
    }

    taskEXIT_CRITICAL();


    if (need_notify == true) {
        xTaskNotifyGive(s_eg800_basic_task_handle);
    }

    return RESULT_SUCCESS;
}






