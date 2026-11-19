#include "eg_http.h"

#include <stdio.h>
#include <string.h>

#include "eg_network.h"


#define LOG_TAG "EG_HTTP"
#include "log.h"



// AT 配置、提示符和终止响应超时。
#define EG800_HTTP_URL_RESPONSE_TIMEOUT_MS              5000U           // QHTTPURL 等待 CONNECT 或终止响应的单阶段超时，单位 ms
#define EG800_HTTP_CONFIG_TIMEOUT_MS                    5000U           // QHTTPCFG 和 QSSLCFG 等待终止响应的超时时间，单位 ms
#define EG800_HTTP_GET_FINAL_TIMEOUT_MS                 5000U           // GET/GETEX 等待 OK 或错误终止行的超时，单位 ms
#define EG800_HTTP_GET_URC_MARGIN_MS                    5000U           // GET/GETEX 在模组响应时限之外的 URC 接收余量，单位 ms
#define EG800_HTTP_READ_DEFAULT_WAIT_TIME_S             60U             // QHTTPREAD 默认包间隔超时，单位 s
#define EG800_HTTP_READ_FINAL_TIMEOUT_MS                5000U           // QHTTPREAD 等待 CONNECT 或终止响应的单阶段超时，单位 ms
#define EG800_HTTP_READ_DATA_MARGIN_MS                  5000U           // QHTTPREAD 的 RAW 接收余量，单位 ms
#define EG800_HTTP_STOP_TIMEOUT_MS                      10000U          // QHTTPSTOP 等待终止响应的超时时间，单位 ms




// HTTP 层内部 AT 命令编号
typedef enum {
    EG800_HTTP_CMD_CFG_PDP_CONTEXT = 0,                                 // 绑定 HTTP 使用的 PDP 上下文
    EG800_HTTP_CMD_CFG_SSL_CONTEXT,                                     // 绑定 HTTPS 使用的 SSL 上下文
    EG800_HTTP_CMD_CFG_SSL_VERSION,                                     // 配置 SSL/TLS 协议版本
    EG800_HTTP_CMD_CFG_SSL_SECURITY_LEVEL,                              // 配置 SSL 证书身份验证级别
    EG800_HTTP_CMD_CFG_SSL_SNI,                                         // 启用或禁用 TLS 服务器名称指示
    EG800_HTTP_CMD_CFG_SSL_CIPHER_SUITE,                                // 配置 SSL 加密套件
    EG800_HTTP_CMD_CFG_REQUEST_HEADER,                                  // 配置是否由模组生成 HTTP 请求头
    EG800_HTTP_CMD_CFG_RESPONSE_HEADER,                                 // 配置读取响应时是否输出 HTTP 响应头
    EG800_HTTP_CMD_SET_URL,                                             // 通过 QHTTPURL 设置请求 URL
    EG800_HTTP_CMD_SEND_GET,                                            // 通过 QHTTPGET 发送 GET 请求
    EG800_HTTP_CMD_SEND_GET_RANGE,                                      // 通过 QHTTPGETEX 请求指定范围的数据
    EG800_HTTP_CMD_READ_RESPONSE,                                       // 通过 QHTTPREAD 读取 HTTP 响应
    EG800_HTTP_CMD_STOP,                                                // 通过 QHTTPSTOP 取消 HTTP 请求并断开会话
} Eg800HttpCmd_e;

// 下载会话状态
typedef enum {
    EG800_HTTP_DOWNLOAD_STATE_IDLE = 0,    // 等待新请求
    EG800_HTTP_DOWNLOAD_STATE_ACTIVE,      // 已接受请求，等待执行或正在下载
    EG800_HTTP_DOWNLOAD_STATE_CANCEL,      // 发起任务已请求取消，等待下载任务收尾
    EG800_HTTP_DOWNLOAD_STATE_FINISHED,    // 终态结果已确定，等待发起任务取走
    EG800_HTTP_DOWNLOAD_STATE_CONFIGURING  // 基础配置正在使用 HTTP AT 完成信号量
} Eg800HttpDownloadState_e;



static TaskHandle_t s_eg800_http_task_handle = NULL;                    // 任务句柄

static bool s_eg800_http_initialized = false;                           // Http 层初始化标志位

static QueueHandle_t s_eg800_http_download_event_queue = NULL;          // 向调用方交付下载事件的队列

static SemaphoreHandle_t s_http_at_completion_sem = NULL;               // Http 层 AT 请求完成信号量

static Eg800HttpDownloadState_e s_eg800_http_download_state = EG800_HTTP_DOWNLOAD_STATE_IDLE; // 当前下载会话状态

static TaskHandle_t s_eg800_http_download_owner = NULL;                 // 本轮下载的发起任务，取走终态后清空
static bool s_eg800_http_download_recovery_required = false;            // 模组状态待确认时禁止接受新下载
static char s_eg800_http_download_url[EG800_HTTP_URL_MAX_LENGTH + 1U];  // 下载任务持有的 URL 副本
static Eg800HttpDownloadRequest_t s_eg800_http_download_request = {0};  // 已提交的请求，URL 指向内部副本
static uint8_t s_eg800_http_download_data[EG800_AT_MAX_RAW_SIZE];       // 单段响应体，归还后才允许写入下一段
static bool s_eg800_http_download_data_borrowed = false;                // 发起任务是否仍持有上一包数据

// 实际处理URC的函数声明
static void Eg800_Http_Temp_Urc_Handler(const char *line);

// 路由表需要在声明之后
static const Eg800UrcRegistration_t s_eg800_Http_urc_routes[] = {
    {"+TEMP:",      EG800_AT_OWNER_HTTP,        Eg800_Http_Temp_Urc_Handler},
};

// ------------------------------------------------- 函数声明 -----------------------------------------------------
// URC 注册
static Result_t Eg800_Http_Register_Urcs(void);

// 核心任务
static void Eg800_Http_Task(void *argument);




//--------------------------------------------------------URC 注册/处理-----------------------------------------
/**
 * @brief 注册 Http 层负责主动上报的URC
 */
static Result_t Eg800_Http_Register_Urcs(void) {
    return Eg800_Urc_Register_Table(s_eg800_Http_urc_routes,ARRAY_SIZE(s_eg800_Http_urc_routes));
}


static void Eg800_Http_Temp_Urc_Handler(const char *line) {
    LOG("TODO:%s",line);
}


//--------------------------------------------------------关键函数实体-----------------------------------------
/**
 * @brief 构造 HTTP AT 请求，提交给 Service 并同步等待结果。
 * @note 各命令负责参数校验、指令拼接和结果解析。
 */
static Eg800AtResult_e Eg800_Http_At_Execute(uint8_t cmd_id, Eg800AtFlow_e flow, const char *name, const char *tx, const char *rsp_prefix, const char *expect_urc_prefix, const char *prompt_prefix, const uint8_t *tx_data, size_t tx_data_len, uint8_t *raw_buf, size_t raw_buf_size, size_t raw_expect_len, char *rx_buf, size_t rx_buf_size, uint32_t final_timeout_ms, uint32_t urc_timeout_ms, uint32_t data_timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {

    Eg800AtRequest_t request = {0};

    // 命令身份与执行流程。
    request.cmd.owner = EG800_AT_OWNER_HTTP;
    request.cmd.id = cmd_id;
    request.flow = flow;
    request.name = name;
    request.tx = tx;

    // 文本响应、最终结果 URC 和数据提示符。
    request.rsp_prefix = rsp_prefix;
    request.expect_urc_prefix = expect_urc_prefix;
    request.prompt_prefix = prompt_prefix;
    request.rx_buf = rx_buf;
    request.rx_buf_size = rx_buf_size;

    // SEND_DATA：等待提示符后发送 URL 等原始数据。
    request.tx_data = tx_data;
    request.tx_data_len = tx_data_len;

    // RECV_RAW：等待 CONNECT 后接收指定长度的二进制数据。
    request.raw_buf = raw_buf;
    request.raw_buf_size = raw_buf_size;
    request.raw_expect_len = raw_expect_len;

    // 各阶段超时，单位 ms；传入 0 时使用 Service 默认值。
    request.final_timeout_ms = final_timeout_ms;
    request.urc_timeout_ms = urc_timeout_ms;
    request.data_timeout_ms = data_timeout_ms;

    return Eg800_AT_Execute(&request, result_info, completion_sem);
}

/**
 * @brief 取出 Service 在 OK 之后追加的最后一行 URC。
 * @param text Service 写入的文本缓冲区，会去掉最后的换行。
 * @param text_size 文本缓冲区容量，单位字节。
 * @param text_len 已写入的文本长度，单位字节。
 * @return 最后一行的起始地址。文本缺失或可能被截断时返回 NULL。
 */
static const char *Eg800_Http_Get_Result_Line(char *text, size_t text_size, size_t text_len) {
    const char *line;

    if ((text_len == 0U) || (text_len >= (text_size - 1U)) || (text[text_len - 1U] != '\n')) {
        return NULL;
    }

    text[text_len - 1U] = '\0';
    line = strrchr(text, '\n');
    return (line == NULL) ? text : line + 1;
}

/**
 * @brief 解析 QHTTPGET 和 QHTTPGETEX 共用的 +QHTTPGET 最终结果。
 * @param line 不含行终止符的 URC 文本。
 * @param response 调用方的响应信息，格式有效且模组成功时填入。
 * @return 模组错误码为 0 时返回 OK，模组报告失败时返回 ERROR，格式异常时返回 DATA_FAIL。
 */
static Eg800AtResult_e Eg800_Http_Parse_Get_Result(const char *line, Eg800HttpResponseInfo_t *response) {
    unsigned int module_error;
    unsigned int status_code;
    unsigned long long content_length = 0ULL;
    int fields_end = 0;
    int field_count;

    // 用宽类型承接最多 10 位的长度，确认可存入 uint32_t 后再转换。
    field_count = sscanf(line, "+QHTTPGET: %3u %n, %3u %n, %10llu %n",
                         &module_error, &fields_end, &status_code, &fields_end, &content_length, &fields_end);
    if ((fields_end != 0) && (line[fields_end] == '\0')) {
        if ((field_count == 1) && (module_error > 0U) && (module_error <= 999U)) {
            LOG_ERROR("HTTP GET 失败，模组错误码=%u", module_error);
            return EG800_AT_RESULT_ERROR;
        }
        if (((field_count == 2) || (field_count == 3)) && (module_error == 0U) &&
            (status_code >= 100U) && (status_code <= 599U) && (content_length <= UINT32_MAX)) {
            response->status_code = (uint16_t)status_code;
            response->content_length_valid = (field_count == 3);
            response->content_length = (uint32_t)content_length;
            return EG800_AT_RESULT_OK;
        }
    }

    LOG_ERROR("HTTP GET 结果格式无效: %s", line);
    return EG800_AT_RESULT_DATA_FAIL;
}

/**
 * @brief 执行已组装的 GET 或 GETEX 指令，等待并解析共用的结果 URC。
 * @param cmd_id HTTP 层内部命令编号。
 * @param name Service 日志使用的命令名称。
 * @param command 带行终止符的完整 AT 指令。
 * @param response 调用方已清零的响应信息。
 * @param rsptime_s 模组在 OK 后等待 HTTP 响应的时限，单位 s。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return 模组收到有效 HTTP 响应时返回 OK。模组失败、结果格式错误或 Service 执行错误分别返回对应状态。
 */
static Eg800AtResult_e Eg800_Http_Execute_Get(Eg800HttpCmd_e cmd_id, const char *name, const char *command, Eg800HttpResponseInfo_t *response, uint16_t rsptime_s, SemaphoreHandle_t completion_sem) {
    char text[96];
    uint32_t urc_timeout_ms;
    const char *line;
    Eg800AtResult_e result;
    Eg800AtResultInfo_t result_info;

    urc_timeout_ms = ((uint32_t)rsptime_s * 1000U) + EG800_HTTP_GET_URC_MARGIN_MS;
    result = Eg800_Http_At_Execute(cmd_id, EG800_AT_FLOW_TEXT_URC, name, command,
        "+QHTTPGET:", "+QHTTPGET:", NULL, NULL, 0U, NULL, 0U, 0U, text, sizeof(text),
        EG800_HTTP_GET_FINAL_TIMEOUT_MS, urc_timeout_ms, 0U, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    line = Eg800_Http_Get_Result_Line(text, sizeof(text), result_info.rx_len);
    if (line == NULL) {
        LOG_ERROR("HTTP GET 最终结果缺失或不完整");
        return EG800_AT_RESULT_DATA_FAIL;
    }

    return Eg800_Http_Parse_Get_Result(line, response);
}















//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
/**
 * @brief 配置 HTTP 使用 Network 层的 PDP 场景。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、信号量无效或命令生成失败返回 PARAM_ERROR。
 * @note 只绑定 PDP 场景，PDP 的激活由 Network 层负责。
 */
Eg800AtResult_e Eg800_Http_Set_PDP_Context(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPCFG=\"contextid\",%u\r\n", (unsigned int)EG800_NETWORK_CONTEXT_ID);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_CFG_PDP_CONTEXT, EG800_AT_FLOW_TEXT, "配置 HTTP PDP 场景", command, NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U, EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 绑定 HTTP 使用的 SSL 上下文。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、信号量无效或命令生成失败返回 PARAM_ERROR。
 * @note 绑定 HTTP 使用的 SSL 上下文。
 */
Eg800AtResult_e Eg800_Http_Set_SSL_Context(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPCFG=\"sslctxid\",%u\r\n", (unsigned int)EG800_HTTP_SSL_CONTEXT_ID);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_CFG_SSL_CONTEXT, EG800_AT_FLOW_TEXT, "配置 HTTP SSL 场景", command, NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U, EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 配置 HTTPS 上下文允许使用的 SSL/TLS 协议版本。
 * @param version 协议版本编号，0 表示 SSL 3.0，1~3 分别表示 TLS 1.0~1.2，4 表示全部。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Http_Set_SSL_Version(uint8_t version, SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (version > 4U) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QSSLCFG=\"sslversion\",%u,%u\r\n", (unsigned int)EG800_HTTP_SSL_CONTEXT_ID, (unsigned int)version);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_CFG_SSL_VERSION, EG800_AT_FLOW_TEXT, "配置 SSL/TLS 协议版本", command,
        NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U, EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 配置 HTTPS 上下文的证书身份验证级别。
 * @param seclevel 验证级别，0 表示不验证，1 表示验证服务器，2 表示按服务器要求进行双向验证。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Http_Set_SSL_Security_Level(uint8_t seclevel, SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (seclevel > 2U) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QSSLCFG=\"seclevel\",%u,%u\r\n",
                           (unsigned int)EG800_HTTP_SSL_CONTEXT_ID, (unsigned int)seclevel);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(
        EG800_HTTP_CMD_CFG_SSL_SECURITY_LEVEL, EG800_AT_FLOW_TEXT, "配置 SSL 证书验证级别", command,
        NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U,
        EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 配置 HTTPS 上下文的 TLS 服务器名称指示。
 * @param sni SNI 开关，0 表示禁用，1 表示启用。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Http_Set_SSL_SNI(uint8_t sni, SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (sni > 1U) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QSSLCFG=\"sni\",%u,%u\r\n", (unsigned int)EG800_HTTP_SSL_CONTEXT_ID, (unsigned int)sni);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_CFG_SSL_SNI, EG800_AT_FLOW_TEXT, "配置 TLS 服务器名称指示", command,
        NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U, EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 按 EG800_HTTP_CIPHERSUITE 配置 HTTPS 上下文允许使用的加密套件。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、信号量为空或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Http_Set_CipherSuite(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QSSLCFG=\"ciphersuite\",%u,0x%04X\r\n", (unsigned int)EG800_HTTP_SSL_CONTEXT_ID, (unsigned int)EG800_HTTP_CIPHERSUITE);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(
        EG800_HTTP_CMD_CFG_SSL_CIPHER_SUITE, EG800_AT_FLOW_TEXT, "配置 SSL 加密套件", command,
        NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U,
        EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 按 EG800_HTTP_REQUESTHEADER 配置 HTTP 请求头生成方式。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、信号量为空或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Http_Set_RequestHeader(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPCFG=\"requestheader\",%u\r\n", (unsigned int)EG800_HTTP_REQUESTHEADER);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_CFG_REQUEST_HEADER, EG800_AT_FLOW_TEXT, "配置 HTTP 请求头生成方式", command,
        NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U,EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 按 EG800_HTTP_RESPONSEHEADER 配置读取响应时是否输出 HTTP 响应头。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、信号量为空或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Http_Set_ResponseHeader(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPCFG=\"responseheader\",%u\r\n", (unsigned int)EG800_HTTP_RESPONSEHEADER);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_CFG_RESPONSE_HEADER, EG800_AT_FLOW_TEXT, "配置 HTTP 响应头输出", command,
        NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U,EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 等待 CONNECT 后发送调用方提供的 HTTP(S) URL。
 * @param url URL 字节缓冲区，以 http:// 或 https:// 开头，在函数返回前保持有效且内容不变。
 * @param url_length 实际发送的 URL 字节数，范围 1~EG800_HTTP_URL_MAX_LENGTH，不含结尾空字符。
 * @param input_timeout_s 模组等待 URL 输入的超时时间，单位 s，范围 1~255。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示设置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Http_Set_URL(const char *url, int32_t url_length, uint8_t input_timeout_s, SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;
    size_t url_size;
    bool has_http_prefix;
    bool has_https_prefix;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL) || (url == NULL) ||
        (url_length <= 0) || (url_length > (int32_t)EG800_HTTP_URL_MAX_LENGTH) || (input_timeout_s == 0U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    url_size = (size_t)url_length;
    has_http_prefix = (url_size > (sizeof("http://") - 1U)) && (memcmp(url, "http://", sizeof("http://") - 1U) == 0);
    has_https_prefix = (url_size > (sizeof("https://") - 1U)) && (memcmp(url, "https://", sizeof("https://") - 1U) == 0);
    if (((has_http_prefix == false) && (has_https_prefix == false)) ||
        (memchr(url, '\0', url_size) != NULL) || (memchr(url, '\r', url_size) != NULL) ||
        (memchr(url, '\n', url_size) != NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPURL=%u,%u\r\n",
                           (unsigned int)url_size, (unsigned int)input_timeout_s);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_SET_URL, EG800_AT_FLOW_SEND_DATA, "设置 HTTP URL", command,
        NULL, NULL, "CONNECT", (const uint8_t *)url, url_size, NULL, 0U, 0U, NULL, 0U,
        EG800_HTTP_URL_RESPONSE_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 发送 HTTP(S) GET 请求并取得响应信息。
 * @param response 响应信息输出，必须非 NULL。失败时清零。
 * @param rsptime_s 模组在 OK 后等待 HTTP 响应的时限，单位 s，传入 0 或超过 300 时使用默认值。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return 模组收到有效 HTTP 响应时返回 OK，模组报告失败时返回 ERROR，结果格式异常时返回 DATA_FAIL。
 *         参数无效或未初始化返回 PARAM_ERROR，其余 Service 执行错误原样返回。
 * @note 先设置 URL，并使用 requestheader=0 模式。HTTP 状态码和长度通过 response 返回。
 */
Eg800AtResult_e Eg800_Http_Send_Get(Eg800HttpResponseInfo_t *response, uint16_t rsptime_s, SemaphoreHandle_t completion_sem) {
    char command[32];
    int command_len;

    if (response == NULL) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    *response = (Eg800HttpResponseInfo_t){0};

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    if ((rsptime_s == 0U) || (rsptime_s > EG800_HTTP_GET_MAX_RESPONSE_TIME_S)) {
        rsptime_s = EG800_HTTP_GET_RESPONE_DEFAULT_TIME;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPGET=%u\r\n", (unsigned int)rsptime_s);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_Execute_Get(EG800_HTTP_CMD_SEND_GET, "发送 HTTP GET 请求", command, response, rsptime_s, completion_sem);
}

/**
 * @brief 请求 HTTP(S) 资源的指定范围并取得本次响应信息。
 * @param response 响应信息输出，必须非 NULL。失败时清零。
 * @param rsptime_s 模组在 OK 后等待 HTTP 响应的时限，单位 s，传入 0 使用默认值。
 * @param start_position 请求起始偏移，从 0 开始，单位字节。
 * @param read_len 请求的字节数，必须大于 0，计算出的末字节偏移须在 int32_t 范围内。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return 模组收到有效 HTTP 响应时返回 OK，模组报告失败时返回 ERROR，结果格式异常时返回 DATA_FAIL。
 *         参数无效或未初始化返回 PARAM_ERROR，其余 Service 执行错误原样返回。
 * @note 先设置 URL，并使用 requestheader=0 模式。分段读取前须检查状态码为 206，再使用模组上报的本段长度。
 */
Eg800AtResult_e Eg800_Http_Send_Get_Range(Eg800HttpResponseInfo_t *response, uint8_t rsptime_s, int32_t start_position, int32_t read_len, SemaphoreHandle_t completion_sem) {
    char command[48];
    int command_len;
    uint16_t response_time_s;

    if (response == NULL) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    *response = (Eg800HttpResponseInfo_t){0};

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL) ||
        (start_position < 0) || (read_len <= 0) || (start_position > (INT32_MAX - (read_len - 1)))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    response_time_s = (rsptime_s == 0U) ? EG800_HTTP_GET_RESPONE_DEFAULT_TIME : rsptime_s;
    command_len = snprintf(command, sizeof(command), "AT+QHTTPGETEX=%u,%ld,%ld\r\n",
                           (unsigned int)response_time_s, (long)start_position, (long)read_len);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_Execute_Get(EG800_HTTP_CMD_SEND_GET_RANGE, "发送 HTTP 分段 GET 请求", command,
        response, response_time_s, completion_sem);
}

/**
 * @brief 读取已知长度的响应体，直接写入调用方提供的缓冲区。
 * @param read_buffer 缓冲区及本次完整响应体长度，received_len 输出实收字节数。
 * @param rsptime_s 模组接收两个数据包的最大间隔，单位 s，传入 0 使用默认的 60 s。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return 收满预期字节且模组上报成功时返回 OK。模组报告失败时返回 ERROR，结果不完整或异常时返回 DATA_FAIL。
 *         参数无效或未初始化返回 PARAM_ERROR，其余 Service 执行错误原样返回。
 * @note 先取得本次 GET 或 GETEX 的响应信息，并使用 responseheader=0 模式。
 * @note 调用期间保持结构体和缓冲区有效。返回 OK 后可使用本次响应体。
 */
Eg800AtResult_e Eg800_Http_Get_Read(Eg800HttpReadBuffer_t *read_buffer, uint8_t rsptime_s, SemaphoreHandle_t completion_sem) {
    char command[32];
    char text[64];
    int command_len;
    uint32_t data_timeout_ms;
    const char *line;
    unsigned int module_error;
    int fields_end = 0;
    Eg800AtResult_e result;
    Eg800AtResultInfo_t result_info;

    if (read_buffer == NULL) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    read_buffer->received_len = 0U;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL) || (read_buffer->buffer == NULL) ||
        (read_buffer->expected_len == 0U) || (read_buffer->expected_len > read_buffer->buffer_size) ||
        (read_buffer->expected_len > EG800_AT_MAX_RAW_SIZE)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    if (rsptime_s == 0U) {
        rsptime_s = EG800_HTTP_READ_DEFAULT_WAIT_TIME_S;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPREAD=%u\r\n", (unsigned int)rsptime_s);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 模组使用包间隔超时，Service 在整个 RAW 接收阶段使用独立的等待预算。
    data_timeout_ms = ((uint32_t)rsptime_s * 1000U) + EG800_HTTP_READ_DATA_MARGIN_MS;
    result = Eg800_Http_At_Execute(EG800_HTTP_CMD_READ_RESPONSE, EG800_AT_FLOW_RECV_RAW, "读取 HTTP 响应体", command,
        "+QHTTPREAD:", "+QHTTPREAD:", "CONNECT", NULL, 0U, read_buffer->buffer, read_buffer->buffer_size,
        read_buffer->expected_len, text, sizeof(text), EG800_HTTP_READ_FINAL_TIMEOUT_MS, 0U,
        data_timeout_ms, &result_info, completion_sem);
    read_buffer->received_len = result_info.raw_len;
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    line = Eg800_Http_Get_Result_Line(text, sizeof(text), result_info.rx_len);
    if ((read_buffer->received_len != read_buffer->expected_len) || (line == NULL) ||
        (sscanf(line, "+QHTTPREAD: %3u %n", &module_error, &fields_end) != 1) ||
        (fields_end == 0) || (line[fields_end] != '\0') || (module_error > 999U)) {
        LOG_ERROR("HTTP READ 最终结果缺失或格式无效");
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if (module_error != 0U) {
        LOG_ERROR("HTTP READ 失败，模组错误码=%u", module_error);
        return EG800_AT_RESULT_ERROR;
    }

    return EG800_AT_RESULT_OK;
}

















































/**
 * @brief 取消 HTTP(S) 请求并断开会话连接。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示取消成功；未初始化或信号量为空返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note 通过 Service 队列执行，需等待前序 AT 请求结束。
 */
Eg800AtResult_e Eg800_Http_STOP(SemaphoreHandle_t completion_sem) {
    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_STOP, EG800_AT_FLOW_TEXT, "取消 HTTP 请求", "AT+QHTTPSTOP\r\n",
        NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U, EG800_HTTP_STOP_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}











//--------------------------------------------------------公开 API-----------------------------------------
/**
 * @brief 初始化 HTTP 基础命令接口、注册 URC 并创建 AT 完成信号量。
 * @return RESULT_SUCCESS 表示已初始化，RESULT_FAIL 表示 URC 注册或信号量创建失败。
 */
Result_t Eg800_Http_Init(void) {
    //幂等检查
    if (s_eg800_http_initialized == true) {
        return RESULT_SUCCESS;
    }

    // 注册URC
    if (Eg800_Http_Register_Urcs() != RESULT_SUCCESS) {
        return RESULT_FAIL;
    }

    // HTTP 基础配置和下载任务分时使用同一个 AT 完成信号量。
    s_http_at_completion_sem = xSemaphoreCreateBinary();
    if (s_http_at_completion_sem == NULL) {
        LOG_ERROR("HTTP AT 完成信号量创建失败");
        return RESULT_FAIL;
    }

    s_eg800_http_initialized = true;

    return RESULT_SUCCESS;
}

/**
 * @brief 按项目默认值依次配置 HTTP 的 PDP、请求头、响应头和 SSL 参数。
 * @return OK 表示全部配置成功，未初始化或会话占用时返回 PARAM_ERROR，其他值为第一条失败指令的结果。
 * @note 由业务任务在 HTTP 初始化后、发出请求或启动下载前调用。
 */
Eg800AtResult_e Eg800_Http_Configure_Basic(void) {
    Eg800AtResult_e result;

    taskENTER_CRITICAL();
    if ((s_eg800_http_initialized == false) || (s_http_at_completion_sem == NULL) ||
        (s_eg800_http_download_state != EG800_HTTP_DOWNLOAD_STATE_IDLE)) {
        taskEXIT_CRITICAL();
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 配置期间占用会话，避免下载任务同时等待同一个完成信号量。
    s_eg800_http_download_state = EG800_HTTP_DOWNLOAD_STATE_CONFIGURING;
    taskEXIT_CRITICAL();

    result = Eg800_Http_Set_PDP_Context(s_http_at_completion_sem);
    if (result == EG800_AT_RESULT_OK) {
        result = Eg800_Http_Set_RequestHeader(s_http_at_completion_sem);
    }
    if (result == EG800_AT_RESULT_OK) {
        result = Eg800_Http_Set_ResponseHeader(s_http_at_completion_sem);
    }
    if (result == EG800_AT_RESULT_OK) {
        result = Eg800_Http_Set_SSL_Context(s_http_at_completion_sem);
    }
    if (result == EG800_AT_RESULT_OK) {
        result = Eg800_Http_Set_SSL_Version(EG800_HTTP_SSL_VERSION, s_http_at_completion_sem);
    }
    if (result == EG800_AT_RESULT_OK) {
        result = Eg800_Http_Set_SSL_Security_Level(EG800_HTTP_SSL_SECURITY_LEVEL, s_http_at_completion_sem);
    }
    if (result == EG800_AT_RESULT_OK) {
        result = Eg800_Http_Set_SSL_SNI(EG800_HTTP_SSL_SNI, s_http_at_completion_sem);
    }
    if (result == EG800_AT_RESULT_OK) {
        result = Eg800_Http_Set_CipherSuite(s_http_at_completion_sem);
    }

    taskENTER_CRITICAL();
    s_eg800_http_download_state = EG800_HTTP_DOWNLOAD_STATE_IDLE;
    taskEXIT_CRITICAL();

    return result;
}

/**
 * @brief 初始化 HTTP 层并按需创建下载任务和事件队列。
 * @return RESULT_SUCCESS 表示任务已就绪，RESULT_FAIL 表示基础初始化或下载资源创建失败。
 * @note 由上层任务串行调用。
 */
Result_t Eg800_Http_Download_Init(void) {
    BaseType_t task_result;                         // 任务创建结果
    QueueHandle_t event_queue = NULL;               // 事件队列
    TaskHandle_t task_handle = NULL;                // 任务句柄

    if (s_eg800_http_task_handle != NULL) {
        return RESULT_SUCCESS;
    }

    if (Eg800_Http_Init() != RESULT_SUCCESS) {
        return RESULT_FAIL;
    }

    event_queue = xQueueCreate(EG800_HTTP_DOWNLOAD_EVENT_QUEUE_LENGTH, sizeof(Eg800HttpDownloadEvent_t));
    if (event_queue == NULL) {
        LOG_ERROR("HTTP 下载事件队列创建失败");
        return RESULT_FAIL;
    }

    task_result = xTaskCreate(Eg800_Http_Task,
                              "EG HTTP DL",
                              EG800_HTPP_TASK_STACK_SIZE,
                              NULL,
                              EG800_HTPP_TASK_PRIORITY,
                              &task_handle);
    if (task_result != pdPASS) {
        vQueueDelete(event_queue);
        LOG_ERROR("HTTP 下载任务创建失败");
        return RESULT_FAIL;
    }


    s_eg800_http_download_event_queue = event_queue;
    s_eg800_http_task_handle = task_handle;

    return RESULT_SUCCESS;
}




/**
 * @brief 提交一个文件下载请求，复制参数后通知下载任务。
 * @param request 下载请求，结构体和以 '\0' 结尾的 URL 在本函数返回前保持有效。
 * @return OK 表示已接受，其他结果分别表示参数无效、任务未就绪、会话忙或模组状态待确认。
 * @note 由负责接收本轮下载事件的业务任务调用。
 */
Eg800HttpDownloadStartResult_e Eg800_Http_Download_Start(const Eg800HttpDownloadRequest_t *request) {
    TaskHandle_t task_handle;
    TaskHandle_t caller_task;
    size_t url_len = 0U;

    if ((request == NULL) || (request->url == NULL) || (request->file_size == 0U) || ((request->file_size - 1U) > (uint32_t)INT32_MAX) ||
        (request->chunk_size == 0U) || (request->chunk_size > EG800_AT_MAX_RAW_SIZE)) {
        return EG800_HTTP_DOWNLOAD_START_INVALID_PARAM;
    }

    while ((url_len <= EG800_HTTP_URL_MAX_LENGTH) && (request->url[url_len] != '\0')) {
        url_len++;
    }
    if ((url_len == 0U) || (url_len > EG800_HTTP_URL_MAX_LENGTH)) {
        return EG800_HTTP_DOWNLOAD_START_INVALID_PARAM;
    }

    // 返回当前正在执行这行代码的 FreeRTOS 任务句柄。
    caller_task = xTaskGetCurrentTaskHandle();

    taskENTER_CRITICAL();

    // 下载任务还没创建成功
    task_handle = s_eg800_http_task_handle;
    if (task_handle == NULL) {
        taskEXIT_CRITICAL();
        return EG800_HTTP_DOWNLOAD_START_NOT_READY;
    }

    // 上一个 URL 还在下载，或 DONE / FAILED 还没被取走
    if (s_eg800_http_download_state != EG800_HTTP_DOWNLOAD_STATE_IDLE) {
        taskEXIT_CRITICAL();
        return EG800_HTTP_DOWNLOAD_START_BUSY;
    }

    // 上一次 AT 超时后模组状态待确认，暂时拒绝新的下载
    if (s_eg800_http_download_recovery_required == true) {
        taskEXIT_CRITICAL();
        return EG800_HTTP_DOWNLOAD_START_RECOVERY_REQUIRED;
    }

    // 先占用会话，再在临界区外复制 URL，避免长 URL 延长关中断时间。
    s_eg800_http_download_owner = caller_task;
    s_eg800_http_download_state = EG800_HTTP_DOWNLOAD_STATE_ACTIVE;
    taskEXIT_CRITICAL();

    memcpy(s_eg800_http_download_url, request->url, url_len + 1U);
    s_eg800_http_download_request.url = s_eg800_http_download_url;
    s_eg800_http_download_request.file_size = request->file_size;
    s_eg800_http_download_request.chunk_size = request->chunk_size;

    // 请求数据准备完成后，下载任务才可以开始读取。
    (void)xTaskNotify(task_handle, EG800_HTTP_DOWNLOAD_NOTIFY_START, eSetBits);

    return EG800_HTTP_DOWNLOAD_START_OK;
}




//-----------------------------------------------HTTP层核心维护任务-------------------------------------------------------
/**
 * @brief 检查发起任务是否已请求取消本轮下载。
 * @return true 表示收到取消请求，false 表示可以执行下一步。
 */
static bool Eg800_Http_Download_Is_Cancelled(void) {
    bool cancelled;

    taskENTER_CRITICAL();
    cancelled = (s_eg800_http_download_state == EG800_HTTP_DOWNLOAD_STATE_CANCEL);
    taskEXIT_CRITICAL();

    return cancelled;
}

/**
 * @brief 执行一轮下载，每交付一包就等待接收者归还缓冲区。
 * @param terminal 记录当前步骤、文件偏移和 HTTP 状态码的终态事件。
 * @param response_pending 输出模组是否可能留有待读的响应体。
 * @return OK 表示全部分段已交付，CANCELLED 表示收到取消请求，其他值为本轮失败原因。
 */
static Eg800AtResult_e Eg800_Http_Download_Run(Eg800HttpDownloadEvent_t *terminal, bool *response_pending) {
    const Eg800HttpDownloadRequest_t *request = &s_eg800_http_download_request;
    Eg800HttpReadBuffer_t read_buffer = {
        .buffer = s_eg800_http_download_data,
        .buffer_size = sizeof(s_eg800_http_download_data)
    };
    Eg800HttpDownloadEvent_t data_event = {
        .type = EG800_HTTP_DOWNLOAD_EVENT_DATA,
        .data = s_eg800_http_download_data
    };
    Eg800HttpResponseInfo_t response;
    Eg800AtResult_e result;
    size_t chunk_len;
    uint32_t notifications;
    BaseType_t queue_result;

    if (Eg800_Http_Download_Is_Cancelled() == true) {
        return EG800_AT_RESULT_CANCELLED;
    }

    terminal->stage = EG800_HTTP_DOWNLOAD_STAGE_SET_URL;
    result = Eg800_Http_Set_URL(request->url, (int32_t)strlen(request->url),
                               EG800_HTTP_DOWNLOAD_AT_TIMEOUT_S, s_http_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    while (terminal->offset < request->file_size) {
        if (Eg800_Http_Download_Is_Cancelled() == true) {
            return EG800_AT_RESULT_CANCELLED;
        }

        chunk_len = VALUE_MIN(request->file_size - terminal->offset, request->chunk_size);

        terminal->stage = EG800_HTTP_DOWNLOAD_STAGE_GET_RANGE;
        terminal->http_status = 0U;
        result = Eg800_Http_Send_Get_Range(&response, EG800_HTTP_DOWNLOAD_AT_TIMEOUT_S,
                                          (int32_t)terminal->offset, (int32_t)chunk_len,
                                          s_http_at_completion_sem);
        // 结果格式异常时，模组也可能已收到响应，后续需要确认会话状态。
        *response_pending = (result == EG800_AT_RESULT_OK) || (result == EG800_AT_RESULT_DATA_FAIL);
        if (result != EG800_AT_RESULT_OK) {
            return result;
        }

        terminal->http_status = response.status_code;
        if ((response.status_code != 206U) || (response.content_length_valid == false) ||
            (response.content_length != chunk_len)) {
            return EG800_AT_RESULT_DATA_FAIL;
        }
        if (Eg800_Http_Download_Is_Cancelled() == true) {
            return EG800_AT_RESULT_CANCELLED;
        }

        terminal->stage = EG800_HTTP_DOWNLOAD_STAGE_READ;
        read_buffer.expected_len = chunk_len;
        result = Eg800_Http_Get_Read(&read_buffer, EG800_HTTP_DOWNLOAD_AT_TIMEOUT_S,
                                    s_http_at_completion_sem);
        if (result != EG800_AT_RESULT_OK) {
            return result;
        }
        *response_pending = false;

        data_event.offset = terminal->offset;
        data_event.len = read_buffer.received_len;
        data_event.http_status = response.status_code;

        // 取消和投递互斥，确保取消后队列中只会出现本轮终态。
        taskENTER_CRITICAL();
        if (s_eg800_http_download_state == EG800_HTTP_DOWNLOAD_STATE_CANCEL) {
            taskEXIT_CRITICAL();
            return EG800_AT_RESULT_CANCELLED;
        }
        queue_result = xQueueSend(s_eg800_http_download_event_queue, &data_event, 0U);
        taskEXIT_CRITICAL();
        if (queue_result != pdPASS) {
            LOG_ERROR("HTTP 下载数据事件投递失败");
            return EG800_AT_RESULT_DATA_FAIL;
        }

        // 归还后可以复用缓冲区，取消也能唤醒正在等待的下载任务。
        do {
            (void)xTaskNotifyWait(0U, UINT32_MAX, &notifications, portMAX_DELAY);
        } while ((notifications & (EG800_HTTP_DOWNLOAD_NOTIFY_RELEASE | EG800_HTTP_DOWNLOAD_NOTIFY_CANCEL)) == 0U);

        if ((notifications & EG800_HTTP_DOWNLOAD_NOTIFY_RELEASE) != 0U) {
            terminal->offset += (uint32_t)read_buffer.received_len;
        }
        if ((notifications & EG800_HTTP_DOWNLOAD_NOTIFY_CANCEL) != 0U) {
            return EG800_AT_RESULT_CANCELLED;
        }
    }

    return EG800_AT_RESULT_OK;
}

/**
 * @brief 等待本轮下载的数据或终态事件。
 * @param event 调用方提供的事件输出缓冲区，必须非 NULL。
 * @param wait_ticks 等待时间，单位 tick。0 立即取，portMAX_DELAY 持续等待。
 * @return RESULT_SUCCESS 表示取到事件，RESULT_FAIL 表示调用无效或等待超时。
 * @note 由发起下载的任务调用。收到 DATA 后，下一次有效调用会归还数据缓冲区。
 */
Result_t Eg800_Http_Download_Receive(Eg800HttpDownloadEvent_t *event, TickType_t wait_ticks) {
    if ((event == NULL) || (s_eg800_http_download_owner == NULL) ||
        (s_eg800_http_download_owner != xTaskGetCurrentTaskHandle())) {
        return RESULT_FAIL;
    }

    if (s_eg800_http_download_data_borrowed == true) {
        s_eg800_http_download_data_borrowed = false;
        (void)xTaskNotify(s_eg800_http_task_handle, EG800_HTTP_DOWNLOAD_NOTIFY_RELEASE, eSetBits);
    }

    if (xQueueReceive(s_eg800_http_download_event_queue, event, wait_ticks) != pdPASS) {
        return RESULT_FAIL;
    }

    if (event->type == EG800_HTTP_DOWNLOAD_EVENT_DATA) {
        s_eg800_http_download_data_borrowed = true;
    } else {
        taskENTER_CRITICAL();
        s_eg800_http_download_owner = NULL;
        s_eg800_http_download_state = EG800_HTTP_DOWNLOAD_STATE_IDLE;
        taskEXIT_CRITICAL();
    }

    return RESULT_SUCCESS;
}

/**
 * @brief 请求结束当前下载并归还上一包缓冲区。
 * @return RESULT_SUCCESS 表示已接受取消请求，RESULT_FAIL 表示调用者不是发起任务或本轮已结束。
 * @note 下载任务等待当前 AT 返回后收尾，调用者继续接收直到取走终态。
 */
Result_t Eg800_Http_Download_Cancel(void) {
    Eg800HttpDownloadEvent_t discarded_event;
    TaskHandle_t caller_task = xTaskGetCurrentTaskHandle();

    taskENTER_CRITICAL();
    if ((s_eg800_http_download_owner != caller_task) ||
        ((s_eg800_http_download_state != EG800_HTTP_DOWNLOAD_STATE_ACTIVE) &&
         (s_eg800_http_download_state != EG800_HTTP_DOWNLOAD_STATE_CANCEL))) {
        taskEXIT_CRITICAL();
        return RESULT_FAIL;
    }

    s_eg800_http_download_state = EG800_HTTP_DOWNLOAD_STATE_CANCEL;
    s_eg800_http_download_data_borrowed = false;

    // 终态尚未确定时，队列中最多有一包尚未被调用者取走的数据。
    (void)xQueueReceive(s_eg800_http_download_event_queue, &discarded_event, 0U);
    taskEXIT_CRITICAL();

    (void)xTaskNotify(s_eg800_http_task_handle, EG800_HTTP_DOWNLOAD_NOTIFY_CANCEL, eSetBits);
    return RESULT_SUCCESS;
}

/**
 * @brief 等待下载请求，执行一轮下载后向发起任务交付一次终态。
 * @param argument 预留的 FreeRTOS 任务参数。
 */
static void Eg800_Http_Task(void *argument) {
    uint32_t notifications;
    Eg800HttpDownloadEvent_t terminal;
    Eg800AtResult_e stop_result;
    bool response_pending;

    (void)argument;

    while (1) {
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notifications, portMAX_DELAY);
        if ((notifications & EG800_HTTP_DOWNLOAD_NOTIFY_START) == 0U) {
            continue;
        }

        memset(&terminal, 0, sizeof(terminal));
        response_pending = false;

        terminal.result = Eg800_Http_Download_Run(&terminal, &response_pending);

        // 成功下载或已发出 GETEX 后主动取消时，由下载任务关闭会话。
        if ((terminal.result == EG800_AT_RESULT_OK) ||
            ((terminal.result == EG800_AT_RESULT_CANCELLED) &&
             ((terminal.stage == EG800_HTTP_DOWNLOAD_STAGE_GET_RANGE) ||
              (terminal.stage == EG800_HTTP_DOWNLOAD_STAGE_READ)))) {
            stop_result = Eg800_Http_STOP(s_http_at_completion_sem);
            if ((terminal.result == EG800_AT_RESULT_OK) || (stop_result != EG800_AT_RESULT_OK)) {
                terminal.stage = EG800_HTTP_DOWNLOAD_STAGE_STOP;
                terminal.http_status = 0U;
                terminal.result = stop_result;
            }
            if (stop_result == EG800_AT_RESULT_OK) {
                response_pending = false;
            }
        }

        // 终态确定后再投递，取消请求不能取走已经确定的终态。
        taskENTER_CRITICAL();
        if ((terminal.result == EG800_AT_RESULT_TIMEOUT) ||
            (terminal.result == EG800_AT_RESULT_SEND_FAIL) ||
            (response_pending == true) ||
            ((terminal.stage == EG800_HTTP_DOWNLOAD_STAGE_STOP) && (terminal.result != EG800_AT_RESULT_OK))) {
            s_eg800_http_download_recovery_required = true;
        }
        if ((terminal.result == EG800_AT_RESULT_OK) &&
            (s_eg800_http_download_state == EG800_HTTP_DOWNLOAD_STATE_CANCEL)) {
            terminal.result = EG800_AT_RESULT_CANCELLED;
        }
        s_eg800_http_download_state = EG800_HTTP_DOWNLOAD_STATE_FINISHED;
        taskEXIT_CRITICAL();

        if (terminal.result == EG800_AT_RESULT_OK) {
            terminal.type = EG800_HTTP_DOWNLOAD_EVENT_DONE;
            terminal.stage = EG800_HTTP_DOWNLOAD_STAGE_NONE;
        } else {
            terminal.type = EG800_HTTP_DOWNLOAD_EVENT_FAILED;
        }

        (void)xQueueSend(s_eg800_http_download_event_queue, &terminal, portMAX_DELAY);
    }
}
