#include "eg_mqtt.h"


#include <stdarg.h>


#define LOG_TAG "EG_MQTT"
#include "log.h"



// AT 配置、提示符和终止响应超时。
#define EG800_MQTT_CONFIG_TIMEOUT_MS            5000U       // QMTCFG 等待 OK/ERROR 的超时时间，单位 ms
#define EG800_MQTT_FINAL_TIMEOUT_MS             5000U       // 其他命令等待提示符或 OK/ERROR 的超时时间，单位 ms

// 客户端网络和 MQTT 会话操作的结果 URC 超时。
#define EG800_MQTT_OPEN_URC_TIMEOUT_MS          125000U     // QMTOPEN 结果等待时间，单位 ms
#define EG800_MQTT_DISCONNECT_URC_TIMEOUT_MS    35000U      // QMTDISC 结果等待时间，单位 ms
#define EG800_MQTT_CLOSE_URC_TIMEOUT_MS         35000U      // QMTCLOSE 结果等待时间，单位 ms


// 按当前配置的报文超时和重传次数，额外保留结果 URC 等待余量。
#define EG800_MQTT_URC_MARGIN_MS                5000U       // 软件等待结果 URC 的额外余量，单位 ms
#define EG800_MQTT_SSL_CONTEXT_MAX             5U          // SSL 上下文索引上限
#define EG800_MQTT_RESULT_RESPONSE_SIZE         96U         // 连接或单主题订阅结果 URC 缓冲区容量，单位字节


// Mqtt层内部AT命令编号
typedef enum {
    EG800_MQTT_CMD_CFG_VERSION = 0U,                    // 配置MQTT协议版本
    EG800_MQTT_CMD_CFG_PDP_CONTEXT,                     // 配置 MQTT 使用的 PDP 场景
    EG800_MQTT_CMD_CFG_KEEPALIVE,                       // 配置心跳频率
    EG800_MQTT_CMD_CFG_RECV_MODE,                       // 配置接收模式（URC中包含接收消息的长度信息）
    EG800_MQTT_CMD_CFG_WAKEUP_TOPIC,                    // 配置唤醒TOPIC
    EG800_MQTT_CMD_OPEN_CLIENT_NET,                     // 打开MQTT客户端的网络
    EG800_MQTT_CMD_CLOSE_CLIENT_NET,                    // 关闭MQTT客户端的网络
    EG800_MQTT_CMD_CONNECT_CLIENT_MQTT_SERVICE,         // 连接客户端到MQTT服务器 （建立会话，可携带用户名和密码）
    EG800_MQTT_CMD_DISCONNECT_CLIENT_MQTT_SERVICE,      // 断开客户端断开与MQTT服务器的连接
    EG800_MQTT_CMD_SUBSCRIBE_TOPIC,                     // 订阅TOPIC
    EG800_MQTT_CMD_UNSUBSCRIBE_TOPIC,                   // 退订TOPIC
    EG800_MQTT_CMD_PUBLISH_TOPIC,                       // 发布TOPIC
    EG800_MQTT_CMD_CFG_SSL,                             // 配置 SSL 模式及上下文
    EG800_MQTT_CMD_CFG_SESSION,                         // 配置会话类型
    EG800_MQTT_CMD_CFG_TIMEOUT,                         // 配置报文超时与重传
    EG800_MQTT_CMD_CFG_DATA_FORMAT,                     // 配置发送和接收数据格式
    EG800_MQTT_CMD_CFG_EDIT_TIMEOUT,                    // 配置负载输入超时
    EG800_MQTT_CMD_CFG_WILL,                            // 配置字符串遗嘱
    EG800_MQTT_CMD_CFG_WILL_EX,                         // 配置定长遗嘱
    EG800_MQTT_CMD_CFG_SSL_CERT,                        // 绑定 TLS 证书或私钥
    EG800_MQTT_CMD_CFG_ALIYUN_AUTH,                     // 配置阿里云认证信息
    EG800_MQTT_CMD_CFG_ONENET_AUTH,                     // 配置 OneNET 认证信息
    EG800_MQTT_CMD_CFG_QUERY,                           // 查询模块当前配置
    EG800_MQTT_CMD_CFG_SUPPORT,                         // 查询模块支持的配置项及范围
} Eg800MqttCmd_e;

// 当前请求用于识别最终结果 URC 的信息。
typedef struct {
    Eg800MqttCmd_e cmd_id;                              // 结果所属的 MQTT 命令
    uint16_t msg_id;                                   // 订阅消息编号；非订阅命令忽略
} Eg800MqttUrcContext_t;




static TaskHandle_t s_eg800_mqtt_task_handle = NULL;                    // 任务句柄

static bool s_eg800_mqtt_initialized = false;                           // Mqtt 层初始化标志位

static Eg800MqttConfig_t s_eg800_mqtt_config = {0};                     // MQTT 用户连接配置的内部副本，供后续连接和重连使用

static bool s_eg800_mqtt_should_connect = false;                        // 上层是否希望MQTT保持连接（目标状态）

static uint8_t s_eg800_mqtt_retry_count = 0U;                           // 当前Mqtt连接流程已经尝试的次数

static Eg800MqttSnapshot_t s_eg800_mqtt_snapshot = {0};                 // Mqtt 信息缓存及刷新状态

static SemaphoreHandle_t s_mqtt_at_completion_sem = NULL;                    // Mqtt 层 AT 请求完成信号量
static uint8_t s_eg800_mqtt_packet_timeout_s = EG800_MQTT_PACKET_TIMEOUT_S;   // 最近一次设置或查询成功的报文超时，单位 s
static uint8_t s_eg800_mqtt_packet_retry_times = EG800_MQTT_PACKET_RETRY_TIMES; // 最近一次设置或查询成功的报文重传次数



// 实际处理URC的函数声明
static void Eg800_Mqtt_Temp_Urc_Handler(const char *line);

// 路由表需要在声明之后
static const Eg800UrcRegistration_t s_eg800_mqtt_urc_routes[] = {
    {"+TEMP:",      EG800_AT_OWNER_MQTT,        Eg800_Mqtt_Temp_Urc_Handler},
};



// ------------------------------------------------- 函数声明 -----------------------------------------------------
// URC 注册
static Result_t Eg800_Mqtt_Register_Urcs(void);


// 核心任务
static void Eg800_Mqtt_Task(void *argument);




//--------------------------------------------------------URC 注册/处理-----------------------------------------

/**
 * @brief 注册 Mqtt 层负责主动上报的URC
 */
static Result_t Eg800_Mqtt_Register_Urcs(void) {
    return Eg800_Urc_Register_Table(s_eg800_mqtt_urc_routes,ARRAY_SIZE(s_eg800_mqtt_urc_routes));
}


static void Eg800_Mqtt_Temp_Urc_Handler(const char *line) {
    LOG("占位");
}







//--------------------------------------------------------关键函数实体-----------------------------------------
/**
 * @brief 同步执行一条 MQTT 服务的 AT 请求。
 * @param cmd_id MQTT 服务内部命令编号。
 * @param flow 执行流程，支持 TEXT、TEXT_URC 和 SEND_DATA。
 * @param name 命令日志名称，可为 NULL。
 * @param tx 完整 AT 命令，必须包含 "\r\n"，调用期间保持有效。
 * @param rsp_prefix 需要保存的文本响应前缀，可为 NULL。
 * @param expect_urc_prefix 期望的结果 URC 前缀；TEXT_URC 时必须非空，
 *                         SEND_DATA 时填写后也会在 OK 后继续等待该 URC。
 * @param tx_data 提示符后发送的数据，仅 SEND_DATA 使用，调用期间保持有效。
 * @param tx_data_len 发送数据长度，单位字节；非 SEND_DATA 流程传 0。
 * @param rx_buf 响应缓冲区，调用期间保持有效；不需要响应内容时传 NULL。
 * @param rx_buf_size 响应缓冲区容量，单位字节；rx_buf 为 NULL 时必须为 0。
 * @param final_timeout_ms 等待提示符或 OK/ERROR 的超时时间，单位 ms；
 *                         0 表示使用 Service 默认值。
 * @param urc_timeout_ms 等待结果 URC 的超时时间，单位 ms；
 *                       0 表示使用 Service 默认值。
 * @param result_info AT 执行详细结果，可为 NULL。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示 AT 流程完成，其他值表示参数、发送、超时等执行错误；
 *         结果 URC 中的业务结果由调用者进一步解析。
 * @note MQTT 和 Service 应已初始化；仅供任务上下文调用。
 * @note 禁止在 ISR、Service 任务及其 URC 回调中调用。
 * @note 同一信号量同一时间只允许对应一条未完成请求。
 */
static Eg800AtResult_e Eg800_Mqtt_At_Execute(Eg800MqttCmd_e cmd_id, Eg800AtFlow_e flow, const char *name, const char *tx, const char *rsp_prefix, const char *expect_urc_prefix, const uint8_t *tx_data, size_t tx_data_len, char *rx_buf, size_t rx_buf_size, uint32_t final_timeout_ms, uint32_t urc_timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {
    Eg800AtRequest_t request = {0};

    request.cmd.owner = EG800_AT_OWNER_MQTT;
    request.cmd.id = (uint8_t)cmd_id;
    request.flow = flow;

    request.name = name;
    request.tx = tx;
    request.rsp_prefix = rsp_prefix;
    request.expect_urc_prefix = expect_urc_prefix;

    request.tx_data = tx_data;
    request.tx_data_len = tx_data_len;

    request.rx_buf = rx_buf;
    request.rx_buf_size = rx_buf_size;

    request.final_timeout_ms = final_timeout_ms;
    request.urc_timeout_ms = urc_timeout_ms;

    return Eg800_AT_Execute(&request, result_info, completion_sem);
}





/**
 * @brief 检查可直接放入 AT 指令双引号中的字符串。
 * @param text 以 '\0' 结尾的字符串。
 * @param max_length 最大有效长度，单位字节，不包含结尾 '\0'。
 * @param allow_empty 是否允许空字符串。
 * @return true 表示长度和字符有效；false 表示空指针、超长或包含指令分隔字符。
 */
static bool Eg800_Mqtt_String_Is_Valid(const char *text, size_t max_length, bool allow_empty) {
    size_t index;
    unsigned char ch;

    if (text == NULL) {
        return false;
    }
    for (index = 0U; index <= max_length; index++) {
        ch = (unsigned char)text[index];
        if (ch == '\0') {
            return allow_empty || (index > 0U);
        }
        if ((ch < 0x20U) || (ch == 0x7FU) || (ch == '"') || (ch == '\\')) {
            return false;
        }
    }
    return false;
}

/**
 * @brief 校验格式化后的普通配置命令，并等待 OK 或 ERROR。
 * @param cmd_id MQTT 内部命令编号。
 * @param name 命令日志名称。
 * @param command 已格式化且包含回车换行的命令。
 * @param command_len snprintf 返回的所需长度。
 * @param command_size 命令缓冲区容量，单位字节。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数或命令截断返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
static Eg800AtResult_e Eg800_Mqtt_Execute_Config(Eg800MqttCmd_e cmd_id, const char *name, const char *command, int command_len, size_t command_size, SemaphoreHandle_t completion_sem) {
    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (command_len <= 0) || ((size_t)command_len >= command_size)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_At_Execute(cmd_id, EG800_AT_FLOW_TEXT, name, command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}

/**
 * @brief 生成参数较短的配置指令并同步执行。
 * @param cmd_id MQTT 内部命令编号。
 * @param name 命令日志名称。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @param format snprintf 格式字符串，必须包含回车换行。
 * @param ... 与 format 对应的格式参数。
 * @return OK 表示配置成功；格式化失败或参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
static Eg800AtResult_e Eg800_Mqtt_Execute_Short_Config(Eg800MqttCmd_e cmd_id, const char *name, SemaphoreHandle_t completion_sem, const char *format, ...) {
    char command[96];
    int command_len;
    va_list arguments;

    va_start(arguments, format);
    command_len = vsnprintf(command, sizeof(command), format, arguments);
    va_end(arguments);
    return Eg800_Mqtt_Execute_Config(cmd_id, name, command, command_len, sizeof(command), completion_sem);
}

/**
 * @brief 严格解析一行内以逗号分隔的十进制整数，拒绝跨行、溢出和多余字段。
 * @param text 第一个整数所在的文本。
 * @param values 整数输出数组。
 * @param capacity 数组最多容纳的整数个数。
 * @param count 实际解析的字段个数输出地址。
 * @return true 表示整行有效；false 表示字段缺失、超出 -65535~65535 或格式错误。
 */
static bool Eg800_Mqtt_Parse_Values(const char *text, int *values, size_t capacity, size_t *count) {
    unsigned int value;
    bool negative;

    *count = 0U;
    while (*count < capacity) {
        while ((*text == ' ') || (*text == '\t')) {
            text++;
        }
        negative = (*text == '-');
        if (negative == true) {
            text++;
        }
        if ((*text < '0') || (*text > '9')) {
            return false;
        }
        value = 0U;
        while ((*text >= '0') && (*text <= '9')) {
            value = (value * 10U) + (unsigned int)(*text - '0');
            if (value > 65535U) {
                return false;
            }
            text++;
        }
        values[*count] = (negative == true) ? -(int)value : (int)value;
        (*count)++;
        while ((*text == ' ') || (*text == '\t')) {
            text++;
        }
        if (*text == ',') {
            text++;
            continue;
        }
        while ((*text == '\r') || (*text == '\n')) {
            text++;
        }
        return *text == '\0';
    }
    return false;
}

/**
 * @brief 查询固定客户端的一项配置并提取整数参数。
 * @param key 配置项名称，不含双引号。
 * @param values 整数参数输出数组。
 * @param count 要求返回的整数参数个数。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示整行解析成功；截断、配置项不符或格式错误返回 DATA_FAIL；其他值为 AT 执行错误。
 */
static Eg800AtResult_e Eg800_Mqtt_Query_Config_Values(const char *key, int *values, size_t count, SemaphoreHandle_t completion_sem) {
    char command[64];
    char response[128] = {0};
    const char *text;
    size_t key_len = strlen(key);
    size_t parsed_count;
    int command_len;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"%s\",%u\r\n", key, (unsigned int)EG800_MQTT_CLIENT_INDEX);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    result = Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_QUERY, EG800_AT_FLOW_TEXT, "查询 MQTT 配置", command, "+QMTCFG:", NULL, NULL, 0U, response, sizeof(response), EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }
    if ((result_info.rx_len >= (sizeof(response) - 1U)) || (strncmp(response, "+QMTCFG:", 8U) != 0)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    text = response + 8U;
    while ((*text == ' ') || (*text == '\t')) {
        text++;
    }
    if (*text != '"') {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    text++;
    if ((strncmp(text, key, key_len) != 0) || (text[key_len] != '"')) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    text += key_len + 1U;
    while ((*text == ' ') || (*text == '\t')) {
        text++;
    }
    if ((*text != ',') || (Eg800_Mqtt_Parse_Values(text + 1U, values, count, &parsed_count) == false) || (parsed_count != count)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    return EG800_AT_RESULT_OK;
}

/**
 * @brief 筛选当前客户端、当前消息的最终结果，忽略其他客户端结果和中间重传通知。
 * @param line Service 已按结果前缀匹配的 URC 文本行。
 * @param context 当前请求的命令和消息编号，在同步等待期间有效。
 * @return true 表示交给调用方解析最终结果；false 表示继续等待且不重置原有超时。
 */
static bool Eg800_Mqtt_Match_Result_Urc(const char *line, const void *context) {
    const Eg800MqttUrcContext_t *expected = context;
    const char *text = strchr(line, ':');
    int values[4];
    size_t count;

    // 格式损坏的响应也应结束本次等待，由调用方返回 DATA_FAIL，避免误报成功。
    if ((text == NULL) || (Eg800_Mqtt_Parse_Values(text + 1U, values, ARRAY_SIZE(values), &count) == false) || (count < 2U)) {
        return true;
    }
    if (values[0] != (int)EG800_MQTT_CLIENT_INDEX) {
        return false;
    }
    if (expected->cmd_id == EG800_MQTT_CMD_SUBSCRIBE_TOPIC) {
        if (values[1] != (int)expected->msg_id) {
            return false;
        }
        return !((count == 4U) && (values[2] == 1) && (values[3] >= 1) && (values[3] <= 10));
    }
    if ((expected->cmd_id == EG800_MQTT_CMD_CONNECT_CLIENT_MQTT_SERVICE) && (count == 2U) && (values[1] == 1)) {
        return false;
    }
    return true;
}

/**
 * @brief 执行打开网络、登录或单主题订阅，并检查最终结果 URC 的业务状态。
 * @param cmd_id 打开网络、登录服务器或订阅主题的命令编号。
 * @param name 命令日志名称。
 * @param command 已生成的完整 AT 命令，同步等待期间保持有效。
 * @param msg_id 订阅消息编号；非订阅命令忽略。
 * @param requested_qos 订阅请求的 QoS；非订阅命令忽略。
 * @param granted_qos 实际获准的订阅 QoS 输出，可为 NULL；失败时不修改。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示业务成功；有效失败结果返回 ERROR；响应损坏返回 DATA_FAIL；其他值为 AT 执行错误。
 */
static Eg800AtResult_e Eg800_Mqtt_Execute_Result(Eg800MqttCmd_e cmd_id, const char *name, const char *command, uint16_t msg_id, uint8_t requested_qos, uint8_t *granted_qos, SemaphoreHandle_t completion_sem) {
    Eg800AtRequest_t request = {0};
    Eg800AtResultInfo_t result_info = {0};
    Eg800MqttUrcContext_t context = {cmd_id, msg_id};
    char response[EG800_MQTT_RESULT_RESPONSE_SIZE] = {0};
    const char *text;
    int values[4];
    size_t count;
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    request.cmd.owner = EG800_AT_OWNER_MQTT;
    request.cmd.id = (uint8_t)cmd_id;
    request.flow = EG800_AT_FLOW_TEXT_URC;
    request.name = name;
    request.tx = command;
    request.rx_buf = response;
    request.rx_buf_size = sizeof(response);
    request.final_timeout_ms = EG800_MQTT_FINAL_TIMEOUT_MS;
    request.urc_timeout_ms = ((uint32_t)s_eg800_mqtt_packet_timeout_s * ((uint32_t)s_eg800_mqtt_packet_retry_times + 1U) * 1000U) + EG800_MQTT_URC_MARGIN_MS;
    request.expect_urc_match = Eg800_Mqtt_Match_Result_Urc;
    request.expect_urc_context = &context;

    switch (cmd_id) {
        case EG800_MQTT_CMD_OPEN_CLIENT_NET: {
            request.expect_urc_prefix = "+QMTOPEN:";
            request.urc_timeout_ms = EG800_MQTT_OPEN_URC_TIMEOUT_MS;
            break;
        }
        case EG800_MQTT_CMD_CONNECT_CLIENT_MQTT_SERVICE: {
            request.expect_urc_prefix = "+QMTCONN:";
            break;
        }
        case EG800_MQTT_CMD_SUBSCRIBE_TOPIC: {
            request.expect_urc_prefix = "+QMTSUB:";
            break;
        }
        default: {
            return EG800_AT_RESULT_PARAM_ERROR;
            break;
        }
    }

    result = Eg800_AT_Execute(&request, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }
    text = strchr(response, ':');
    if ((result_info.rx_len >= (sizeof(response) - 1U)) || (text == NULL) ||
        (Eg800_Mqtt_Parse_Values(text + 1U, values, ARRAY_SIZE(values), &count) == false) || (count < 2U) || (values[0] != (int)EG800_MQTT_CLIENT_INDEX)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    if (cmd_id == EG800_MQTT_CMD_OPEN_CLIENT_NET) {
        if ((count != 2U) || (values[1] < -1) || (values[1] > 5)) {
            return EG800_AT_RESULT_DATA_FAIL;
        }
        return (values[1] == 0) ? EG800_AT_RESULT_OK : EG800_AT_RESULT_ERROR;
    }
    if (cmd_id == EG800_MQTT_CMD_CONNECT_CLIENT_MQTT_SERVICE) {
        if ((count == 2U) && (values[1] == 2)) {
            return EG800_AT_RESULT_ERROR;
        }
        if ((count != 3U) || (values[1] != 0) || (values[2] < 0) || (values[2] > 5)) {
            return EG800_AT_RESULT_DATA_FAIL;
        }
        return (values[2] == 0) ? EG800_AT_RESULT_OK : EG800_AT_RESULT_ERROR;
    }
    if ((count < 3U) || (values[1] != (int)msg_id)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    if ((count == 3U) && (values[2] == 2)) {
        return EG800_AT_RESULT_ERROR;
    }
    if ((count != 4U) || (values[2] != 0)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    if (values[3] == 128) {
        return EG800_AT_RESULT_ERROR;
    }
    if ((values[3] < 0) || (values[3] > (int)requested_qos)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    if (granted_qos != NULL) {
        *granted_qos = (uint8_t)values[3];
    }
    return EG800_AT_RESULT_OK;
}

//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
/**
 * @brief 配置固定 MQTT 客户端的协议版本。
 * @param version 协议版本，3 表示 MQTT v3.1，4 表示 MQTT v3.1.1。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note 客户端编号固定使用 EG800_MQTT_CLIENT_INDEX；MQTT 和 Service 应已初始化，且目标 MQTT 连接尚未建立。
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Version(uint8_t version, SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || ((version != 3U) && (version != 4U)) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"version\",%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)version);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_VERSION, EG800_AT_FLOW_TEXT, "配置 MQTT 协议版本", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}



/**
 * @brief 配置固定 MQTT 客户端使用的 PDP 场景。
 * @param context_id PDP 场景编号，范围 1～15。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note MQTT 和 Service 应已初始化，且目标 MQTT 连接尚未建立；PDP 激活由 Network 负责。
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Set_PDP_Context(uint8_t context_id, SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || (context_id == 0U) || (context_id > 15U) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"pdpcid\",%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)context_id);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_PDP_CONTEXT, EG800_AT_FLOW_TEXT, "配置 MQTT PDP 场景", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}









/**
 * @brief 查询模块支持的 MQTT 配置项和参数范围。
 * @param response 调用者提供的文本输出缓冲区。
 * @param response_size 缓冲区容量，单位字节，至少为 2。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示收到完整配置列表；为空或截断返回 DATA_FAIL；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Query_Config_Support(char *response, size_t response_size, SemaphoreHandle_t completion_sem) {
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (response == NULL) || (response_size < 2U) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    result = Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_SUPPORT, EG800_AT_FLOW_TEXT, "查询 MQTT 配置能力", "AT+QMTCFG=?\r\n", "+QMTCFG:", NULL, NULL, 0U, response, response_size, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }
    if ((result_info.rx_len == 0U) || (result_info.rx_len >= (response_size - 1U))) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    return EG800_AT_RESULT_OK;
}

/**
 * @brief 配置固定 MQTT 客户端使用普通 TCP 或指定 SSL 上下文。
 * @param enabled true 启用 TLS，false 使用普通 TCP。
 * @param ssl_context_index SSL 上下文索引，启用时范围 0~5；关闭时忽略。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_SSL(bool enabled, uint8_t ssl_context_index, SemaphoreHandle_t completion_sem) {
    if (enabled == false) {
        return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_SSL, "关闭 MQTT TLS", completion_sem, "AT+QMTCFG=\"ssl\",%u,0\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX);
    }
    if (ssl_context_index > EG800_MQTT_SSL_CONTEXT_MAX) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_SSL, "绑定 MQTT TLS 上下文", completion_sem, "AT+QMTCFG=\"ssl\",%u,1,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)ssl_context_index);
}

/**
 * @brief 配置由模块维护的 MQTT 保活间隔。
 * @param keepalive_s 保活间隔，范围 0~3600，单位 s；0 禁用保活。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Keepalive(uint16_t keepalive_s, SemaphoreHandle_t completion_sem) {
    if (keepalive_s > 3600U) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_KEEPALIVE, "配置 MQTT 保活间隔", completion_sem, "AT+QMTCFG=\"keepalive\",%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)keepalive_s);
}

/**
 * @brief 配置连接时是否清除旧会话。
 * @param enabled true 请求干净会话，false 请求保留会话。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Clean_Session(bool enabled, SemaphoreHandle_t completion_sem) {
    return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_SESSION, "配置 MQTT 会话类型", completion_sem, "AT+QMTCFG=\"session\",%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)enabled);
}

/**
 * @brief 配置模块内部的报文超时、重传及中间超时通知。
 * @param timeout_s 单次报文超时，范围 1~60，单位 s。
 * @param retry_times 报文重传次数，范围 0~10，与断线重连无关。
 * @param timeout_notice 是否上报中间重传通知；连接和订阅接口会继续等待最终结果。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置及本地等待预算已更新；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Packet_Timeout(uint8_t timeout_s, uint8_t retry_times, bool timeout_notice, SemaphoreHandle_t completion_sem) {
    Eg800AtResult_e result;

    if ((timeout_s == 0U) || (timeout_s > 60U) || (retry_times > 10U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    result = Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_TIMEOUT, "配置 MQTT 报文超时", completion_sem, "AT+QMTCFG=\"timeout\",%u,%u,%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)timeout_s, (unsigned int)retry_times, (unsigned int)timeout_notice);
    if (result == EG800_AT_RESULT_OK) {
        s_eg800_mqtt_packet_timeout_s = timeout_s;
        s_eg800_mqtt_packet_retry_times = retry_times;
    }
    return result;
}

/**
 * @brief 配置服务器下行消息的接收方式和负载长度上报。
 * @param mode 0 直接通过 URC 上报，1 缓存在模块后主动读取。
 * @param include_length 是否在直接上报的 URC 中包含负载长度。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Receive_Mode(uint8_t mode, bool include_length, SemaphoreHandle_t completion_sem) {
    if (mode > 1U) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_RECV_MODE, "配置 MQTT 接收方式", completion_sem, "AT+QMTCFG=\"recv/mode\",%u,%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)mode, (unsigned int)include_length);
}

/**
 * @brief 配置 MQTT 负载的发送和接收表示格式。
 * @param send_mode 发送格式，0 为字符串，1 为十六进制。
 * @param recv_mode 接收格式，0 为字符串，1 为十六进制。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Data_Format(uint8_t send_mode, uint8_t recv_mode, SemaphoreHandle_t completion_sem) {
    if ((send_mode > 1U) || (recv_mode > 1U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_DATA_FORMAT, "配置 MQTT 数据格式", completion_sem, "AT+QMTCFG=\"dataformat\",%u,%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)send_mode, (unsigned int)recv_mode);
}

/**
 * @brief 配置等待 MCU 输入负载时是否超时退出。
 * @param enabled true 启用输入超时退出，false 禁用。
 * @param timeout_s 输入超时，范围 1~120，单位 s。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Edit_Timeout(bool enabled, uint8_t timeout_s, SemaphoreHandle_t completion_sem) {
    if ((timeout_s == 0U) || (timeout_s > 120U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_EDIT_TIMEOUT, "配置 MQTT 负载输入超时", completion_sem, "AT+QMTCFG=\"edit/timeout\",%u,%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)enabled, (unsigned int)timeout_s);
}

/**
 * @brief 查询模块当前的 MQTT 保活间隔。
 * @param keepalive_s 成功时输出 0~3600 的保活间隔，单位 s；失败时保持原值。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示查询成功；输出指针无效返回 PARAM_ERROR；字段异常返回 DATA_FAIL；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Query_Keepalive(uint16_t *keepalive_s, SemaphoreHandle_t completion_sem) {
    int values[1];
    Eg800AtResult_e result;

    if (keepalive_s == NULL) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    result = Eg800_Mqtt_Query_Config_Values("keepalive", values, ARRAY_SIZE(values), completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }
    if ((values[0] < 0) || (values[0] > 3600)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    *keepalive_s = (uint16_t)values[0];
    return EG800_AT_RESULT_OK;
}

/**
 * @brief 查询模块当前的 MQTT 会话类型。
 * @param enabled 成功时输出是否请求干净会话；失败时保持原值。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示查询成功；输出指针无效返回 PARAM_ERROR；字段异常返回 DATA_FAIL；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Query_Clean_Session(bool *enabled, SemaphoreHandle_t completion_sem) {
    int values[1];
    Eg800AtResult_e result;

    if (enabled == NULL) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    result = Eg800_Mqtt_Query_Config_Values("session", values, ARRAY_SIZE(values), completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }
    if ((values[0] < 0) || (values[0] > 1)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    *enabled = (values[0] == 1);
    return EG800_AT_RESULT_OK;
}

/**
 * @brief 查询模块的报文超时配置并同步本地结果等待预算。
 * @param timeout_s 成功时输出报文超时，范围 1~60，单位 s。
 * @param retry_times 成功时输出报文重传次数，范围 0~10。
 * @param timeout_notice 成功时输出是否启用中间超时通知。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示查询成功；输出指针无效返回 PARAM_ERROR；字段异常返回 DATA_FAIL；其他值为 AT 执行错误；失败时输出保持原值。
 */
Eg800AtResult_e Eg800_Mqtt_Query_Packet_Timeout(uint8_t *timeout_s, uint8_t *retry_times, bool *timeout_notice, SemaphoreHandle_t completion_sem) {
    int values[3];
    Eg800AtResult_e result;

    if ((timeout_s == NULL) || (retry_times == NULL) || (timeout_notice == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    result = Eg800_Mqtt_Query_Config_Values("timeout", values, ARRAY_SIZE(values), completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }
    if ((values[0] < 1) || (values[0] > 60) || (values[1] < 0) || (values[1] > 10) || (values[2] < 0) || (values[2] > 1)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }
    *timeout_s = (uint8_t)values[0];
    *retry_times = (uint8_t)values[1];
    *timeout_notice = (values[2] == 1);
    s_eg800_mqtt_packet_timeout_s = *timeout_s;
    s_eg800_mqtt_packet_retry_times = *retry_times;
    return EG800_AT_RESULT_OK;
}

/**
 * @brief 关闭遗嘱，或在 AT 指令内配置字符串遗嘱。
 * @param enabled false 关闭遗嘱并忽略其余遗嘱参数，true 配置遗嘱。
 * @param qos 遗嘱 QoS，范围 0~2。
 * @param retain 是否由服务器保留遗嘱消息。
 * @param topic 遗嘱主题，长度 1~256 字节，不允许通配符。
 * @param message 遗嘱字符串，长度 0~256 字节，不接受指令分隔字符。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Will(bool enabled, uint8_t qos, bool retain, const char *topic, const char *message, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_TOPIC_MAX_LENGTH + EG800_MQTT_WILL_MAX_LENGTH + 48U];
    int command_len;

    if (enabled == false) {
        return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_WILL, "关闭 MQTT 遗嘱", completion_sem, "AT+QMTCFG=\"will\",%u,0\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX);
    }
    if ((qos > 2U) || (Eg800_Mqtt_String_Is_Valid(topic, EG800_MQTT_TOPIC_MAX_LENGTH, false) == false) ||
        (strchr(topic, '+') != NULL) || (strchr(topic, '#') != NULL) || (Eg800_Mqtt_String_Is_Valid(message, EG800_MQTT_WILL_MAX_LENGTH, true) == false)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"will\",%u,1,%u,%u,\"%s\",\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)qos, (unsigned int)retain, topic, message);
    return Eg800_Mqtt_Execute_Config(EG800_MQTT_CMD_CFG_WILL, "配置 MQTT 字符串遗嘱", command, command_len, sizeof(command), completion_sem);
}

/**
 * @brief 关闭遗嘱，或在提示符后按准确长度发送遗嘱负载。
 * @param enabled false 关闭遗嘱并忽略其余遗嘱参数，true 配置遗嘱。
 * @param qos 遗嘱 QoS，范围 0~2。
 * @param retain 是否由服务器保留遗嘱消息。
 * @param topic 遗嘱主题，长度 1~256 字节，不允许通配符。
 * @param payload 负载字节缓冲区，仅长度为 0 时可为 NULL；调用期间保持有效。
 * @param payload_len 负载长度，范围 0~256，单位字节；不额外追加结束字符。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示负载发送及配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Will_Ex(bool enabled, uint8_t qos, bool retain, const char *topic, const uint8_t *payload, size_t payload_len, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_TOPIC_MAX_LENGTH + 64U];
    int command_len;

    if (enabled == false) {
        return Eg800_Mqtt_Execute_Short_Config(EG800_MQTT_CMD_CFG_WILL_EX, "关闭 MQTT 定长遗嘱", completion_sem, "AT+QMTCFG=\"willex\",%u,0\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX);
    }
    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (qos > 2U) ||
        (Eg800_Mqtt_String_Is_Valid(topic, EG800_MQTT_TOPIC_MAX_LENGTH, false) == false) || (strchr(topic, '+') != NULL) || (strchr(topic, '#') != NULL) ||
        (payload_len > EG800_MQTT_WILL_MAX_LENGTH) || ((payload_len > 0U) && (payload == NULL))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"willex\",%u,1,%u,%u,\"%s\",%lu\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)qos, (unsigned int)retain, topic, (unsigned long)payload_len);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_WILL_EX, EG800_AT_FLOW_SEND_DATA, "配置 MQTT 定长遗嘱", command, NULL, NULL, payload, payload_len, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}

/**
 * @brief 将指定证书或私钥文件绑定到 SSL 上下文。
 * @param key QSSLCFG 配置项名称，由模块内部固定提供。
 * @param ssl_context_index SSL 上下文索引，范围 0~5。
 * @param path 已上传文件的路径，长度 1~255 字节。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示绑定成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
static Eg800AtResult_e Eg800_Mqtt_Set_SSL_File(const char *key, uint8_t ssl_context_index, const char *path, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_CONFIG_TEXT_SIZE + 48U];
    int command_len;

    if ((ssl_context_index > EG800_MQTT_SSL_CONTEXT_MAX) || (Eg800_Mqtt_String_Is_Valid(path, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, false) == false)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    command_len = snprintf(command, sizeof(command), "AT+QSSLCFG=\"%s\",%u,\"%s\"\r\n", key, (unsigned int)ssl_context_index, path);
    return Eg800_Mqtt_Execute_Config(EG800_MQTT_CMD_CFG_SSL_CERT, "绑定 MQTT TLS 文件", command, command_len, sizeof(command), completion_sem);
}

/**
 * @brief 为 MQTT 使用的 SSL 上下文绑定 CA 证书。
 * @param ssl_context_index SSL 上下文索引，范围 0~5。
 * @param path 已上传的 CA 证书路径，长度 1~255 字节。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示绑定成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_SSL_CA_Cert(uint8_t ssl_context_index, const char *path, SemaphoreHandle_t completion_sem) {
    return Eg800_Mqtt_Set_SSL_File("cacert", ssl_context_index, path, completion_sem);
}

/**
 * @brief 为 MQTT 使用的 SSL 上下文绑定客户端证书。
 * @param ssl_context_index SSL 上下文索引，范围 0~5。
 * @param path 已上传的客户端证书路径，长度 1~255 字节。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示绑定成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_SSL_Client_Cert(uint8_t ssl_context_index, const char *path, SemaphoreHandle_t completion_sem) {
    return Eg800_Mqtt_Set_SSL_File("clientcert", ssl_context_index, path, completion_sem);
}

/**
 * @brief 为 MQTT 使用的 SSL 上下文绑定客户端私钥。
 * @param ssl_context_index SSL 上下文索引，范围 0~5。
 * @param path 已上传的客户端私钥路径，长度 1~255 字节。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示绑定成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_SSL_Client_Key(uint8_t ssl_context_index, const char *path, SemaphoreHandle_t completion_sem) {
    return Eg800_Mqtt_Set_SSL_File("clientkey", ssl_context_index, path, completion_sem);
}

/**
 * @brief 配置固定客户端的阿里云设备认证信息。
 * @param product_key 非空产品密钥，最长 255 字节。
 * @param device_name 非空设备名称，最长 255 字节。
 * @param device_secret 非空设备校验凭证，最长 255 字节。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Aliyun_Auth(const char *product_key, const char *device_name, const char *device_secret, SemaphoreHandle_t completion_sem) {
    char command[(3U * EG800_MQTT_CONFIG_TEXT_SIZE) + 48U];
    int command_len;

    if ((Eg800_Mqtt_String_Is_Valid(product_key, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, false) == false) ||
        (Eg800_Mqtt_String_Is_Valid(device_name, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, false) == false) ||
        (Eg800_Mqtt_String_Is_Valid(device_secret, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, false) == false)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"aliauth\",%u,\"%s\",\"%s\",\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, product_key, device_name, device_secret);
    return Eg800_Mqtt_Execute_Config(EG800_MQTT_CMD_CFG_ALIYUN_AUTH, "配置 MQTT 阿里云认证", command, command_len, sizeof(command), completion_sem);
}

/**
 * @brief 配置固定客户端的 OneNET 设备认证信息。
 * @param product_id 非空产品 ID，最长 255 字节。
 * @param access_key 非空产品密钥，最长 255 字节。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_OneNET_Auth(const char *product_id, const char *access_key, SemaphoreHandle_t completion_sem) {
    char command[(2U * EG800_MQTT_CONFIG_TEXT_SIZE) + 48U];
    int command_len;

    if ((Eg800_Mqtt_String_Is_Valid(product_id, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, false) == false) ||
        (Eg800_Mqtt_String_Is_Valid(access_key, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, false) == false)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"onenet\",%u,\"%s\",\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, product_id, access_key);
    return Eg800_Mqtt_Execute_Config(EG800_MQTT_CMD_CFG_ONENET_AUTH, "配置 MQTT OneNET 认证", command, command_len, sizeof(command), completion_sem);
}

/**
 * @brief 打开固定 MQTT 客户端到服务器的网络连接，等待并检查 QMTOPEN 结果。
 * @param host 服务器域名或 IP，长度 1~100 字节。
 * @param port 服务器端口，范围 1~65535。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示网络打开成功；模块业务失败返回 ERROR；参数无效返回 PARAM_ERROR；响应异常返回 DATA_FAIL；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Open_Network(const char *host, uint16_t port, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_HOST_MAX_LENGTH + 40U];
    int command_len;

    if ((port == 0U) || (Eg800_Mqtt_String_Is_Valid(host, EG800_MQTT_HOST_MAX_LENGTH, false) == false)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    command_len = snprintf(command, sizeof(command), "AT+QMTOPEN=%u,\"%s\",%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, host, (unsigned int)port);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_Execute_Result(EG800_MQTT_CMD_OPEN_CLIENT_NET, "打开 MQTT 网络", command, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 登录 MQTT 服务器，并检查 QMTCONN 最终结果及服务器返回码。
 * @param client_id 非空客户端标识符，最长 255 字节。
 * @param username 用户名，最长 255 字节，可为空字符串；NULL 表示不发送用户名和密码。
 * @param password 密码，最长 255 字节，可为空字符串；NULL 表示省略密码。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示服务器接受连接；模块失败或服务器拒绝返回 ERROR；参数无效返回 PARAM_ERROR；响应异常返回 DATA_FAIL；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Connect_Server(const char *client_id, const char *username, const char *password, SemaphoreHandle_t completion_sem) {
    char command[(3U * EG800_MQTT_CONFIG_TEXT_SIZE) + 40U];
    int command_len;

    if ((Eg800_Mqtt_String_Is_Valid(client_id, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, false) == false) ||
        ((username == NULL) && (password != NULL)) ||
        ((username != NULL) && (Eg800_Mqtt_String_Is_Valid(username, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, true) == false)) ||
        ((password != NULL) && (Eg800_Mqtt_String_Is_Valid(password, EG800_MQTT_CONFIG_TEXT_SIZE - 1U, true) == false))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    if (username == NULL) {
        command_len = snprintf(command, sizeof(command), "AT+QMTCONN=%u,\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, client_id);
    } else if (password == NULL) {
        command_len = snprintf(command, sizeof(command), "AT+QMTCONN=%u,\"%s\",\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, client_id, username);
    } else {
        command_len = snprintf(command, sizeof(command), "AT+QMTCONN=%u,\"%s\",\"%s\",\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, client_id, username, password);
    }
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_Execute_Result(EG800_MQTT_CMD_CONNECT_CLIENT_MQTT_SERVICE, "登录 MQTT 服务器", command, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 订阅单个主题并检查消息编号、结果及实际获准的 QoS。
 * @param msg_id 数据包标识符，范围 1~65535。
 * @param topic 非空订阅主题或过滤器，最长 256 字节。
 * @param qos 请求的 QoS，范围 0~2。
 * @param granted_qos 成功时输出实际获准的 QoS，可为 NULL；失败时保持原值。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示订阅获准；发送失败或服务器拒绝返回 ERROR；参数无效返回 PARAM_ERROR；响应异常返回 DATA_FAIL；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Subscribe_Topic_Ex(uint16_t msg_id, const char *topic, uint8_t qos, uint8_t *granted_qos, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_TOPIC_MAX_LENGTH + 48U];
    int command_len;

    if ((msg_id == 0U) || (qos > 2U) || (Eg800_Mqtt_String_Is_Valid(topic, EG800_MQTT_TOPIC_MAX_LENGTH, false) == false)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    command_len = snprintf(command, sizeof(command), "AT+QMTSUB=%u,%u,\"%s\",%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)msg_id, topic, (unsigned int)qos);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Mqtt_Execute_Result(EG800_MQTT_CMD_SUBSCRIBE_TOPIC, "订阅 MQTT 主题", command, msg_id, qos, granted_qos, completion_sem);
}

/**
 * @brief 订阅单个主题并等待最终结果，不输出实际获准的 QoS。
 * @param msg_id 数据包标识符，范围 1~65535。
 * @param topic 非空订阅主题或过滤器，最长 256 字节。
 * @param qos 请求的 QoS，范围 0~2。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示订阅获准；发送失败或服务器拒绝返回 ERROR；参数无效返回 PARAM_ERROR；响应异常返回 DATA_FAIL；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Subscribe_Topic(uint16_t msg_id, const char *topic, uint8_t qos, SemaphoreHandle_t completion_sem) {
    return Eg800_Mqtt_Subscribe_Topic_Ex(msg_id, topic, qos, NULL, completion_sem);
}

//--------------------------------------------------------公开 API-----------------------------------------
/**
 * @brief 初始化 Mqtt 层并注册基础 URC。
 * @return RESULT_SUCCESS 表示初始化成功。
 * @note 本函数不发送 AT 命令，可以在模块上电前调用。
 */
Result_t Eg800_Mqtt_Init(void) {
    BaseType_t task_result;

    // 幂等检查
    if (s_eg800_mqtt_initialized == true) {
        return RESULT_SUCCESS;
    }

    // 注册URC
    if (Eg800_Mqtt_Register_Urcs() != RESULT_SUCCESS) {
        return RESULT_FAIL;
    }

    // 创建 MQTT 任务专用的 AT 完成信号量。
    s_mqtt_at_completion_sem = xSemaphoreCreateBinary();
    if (s_mqtt_at_completion_sem == NULL) {
        LOG_ERROR("MQTT AT 完成信号量创建失败");
        return RESULT_FAIL;
    }

    task_result = xTaskCreate(Eg800_Mqtt_Task,
                              "EG MQTT Core Task",
                              EG800_MQTT_TASK_STACK_SIZE,
                              NULL,
                              EG800_MQTT_TASK_PRIORITY,
                              &s_eg800_mqtt_task_handle);

    if (task_result != pdPASS) {
        s_eg800_mqtt_task_handle = NULL;
        vSemaphoreDelete(s_mqtt_at_completion_sem);
        s_mqtt_at_completion_sem = NULL;
        LOG_ERROR("MQTT 后台任务创建失败");
        return RESULT_FAIL;
    }

    s_eg800_mqtt_initialized = true;

    return RESULT_SUCCESS;
}




//-----------------------------------------------MQTT层核心维护任务-------------------------------------------------------
/**
 * @brief 初始化 Mqtt 维护任务，并接收上层控制请求。
 * @param argument 任务创建参数，当前未使用。
 */
static void Eg800_Mqtt_Task(void *argument) {
    (void)argument;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


