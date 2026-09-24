#include "eg_network.h"

#include <stdio.h>

#define LOG_TAG "EG_NET"
#include "log.h"

#define EG800_NETWORK_CONFIG_TIMEOUT_MS             5000U               // QICSGP配置超时
#define EG800_NETWORK_ACTIVATE_TIMEOUT_MS           60000U              // QIACT激活超时
#define EG800_NETWORK_QUERY_TIMEOUT_MS              5000U               // QIACT查询超时
#define EG800_NETWORK_DEACTIVATE_TIMEOUT_MS         40000U              // QIDEACT去激活超时

#define EG800_NETWORK_CONTEXT_ID_MAX                15U                 // TCP/IP PDP 场景 ID 上限
#define EG800_NETWORK_APN_MAX_LENGTH                50U                 // 当前封装允许的 APN 最大长度，单位字节
#define EG800_NETWORK_QUERY_RESPONSE_SIZE           512U                // QIACT 查询响应缓冲区容量，单位字节

// Network层内部AT命令编号
typedef enum {
    EG800_NETWORK_CMD_QICSGP = 0U,              // 配置PDP场景和APN
    EG800_NETWORK_CMD_QIACT,                    // 激活PDP场景
    EG800_NETWORK_CMD_QIACT_QUERY,              // 查询PDP场景状态和IP
    EG800_NETWORK_CMD_QIDEACT                   // 去激活PDP场景
} Eg800NetworkCmd_e;

// 单个运营商对应的APN配置
typedef struct {
    Eg800BasicOperator_e operator_type;         // 运营商类型
    const char *apn;                            // 运营商对应的APN
    uint8_t auth_type;                          // QICSGP鉴权方式
} Eg800NetworkApnConfig_t;

static TaskHandle_t s_eg800_network_task_handle = NULL;                 // 任务句柄

static bool s_eg800_network_initialized = false;                        // 初始化标志位

static bool s_eg800_network_should_connect = false;                     // 上层是否希望网络保持连接（目标状态）

static uint8_t s_eg800_network_retry_count = 0U;                        // 当前联网流程已经尝试的次数

static Eg800NetworkSnapshot_t s_eg800_network_snapshot = {0};           // Network 信息缓存及刷新状态

static SemaphoreHandle_t s_network_at_completion_sem = NULL;            // Network 层 AT 请求完成信号量

// 实际处理URC的函数声明
static void Eg800_Network_TCPIP_Urc_Handler(const char *line);

// 路由表需要在声明之后
static const Eg800UrcRegistration_t s_eg800_network_urc_routes[] = {
    {"+QIURC:", EG800_AT_OWNER_NETWORK, Eg800_Network_TCPIP_Urc_Handler},
};

// 当前支持的运营商APN表
static const Eg800NetworkApnConfig_t s_eg800_network_apn_configs[] = {
    {EG800_BASIC_OPERATOR_CHINA_MOBILE, "CMNET", 0U},
    {EG800_BASIC_OPERATOR_CHINA_TELECOM, "CTNET", 0U},
    {EG800_BASIC_OPERATOR_CHINA_UNICOM, "UNINET", 0U}
};

// ------------------------------------------------- 函数声明 -----------------------------------------------------
// URC 注册
static Result_t Eg800_Network_Register_Urcs(void);

//
static void Eg800_Network_Info_Reset(void);

// 核心任务
static void Eg800_Network_Task(void *argument);

//--------------------------------------------------------URC 处理-----------------------------------------
static void Eg800_Network_TCPIP_Urc_Handler(const char *line) {
    LOG_ERROR("%s", line);
    LOG("收到PDP去激活通知或者说是断网通知");
}

//--------------------------------------------------------关键函数实体-----------------------------------------
/**
 * @brief 将 Network 层保存的信息恢复为未知状态。
 */
static void Eg800_Network_Info_Reset(void) {
    (void)memset(&s_eg800_network_snapshot, 0, sizeof(s_eg800_network_snapshot));
    s_eg800_network_snapshot.info.state = EG800_NETWORK_STATE_NONE;
    s_eg800_network_snapshot.info.PDP_ID = EG800_NETWORK_CONTEXT_ID;
    s_eg800_network_snapshot.info.operator_type = EG800_NETWORK_CONTEXT_TYPE;
}

/**
 * @brief 注册 Network 层负责的全部主动上报 URC。
 * @return RESULT_SUCCESS 表示全部注册成功，RESULT_FAIL 表示存在注册失败。
 */
static Result_t Eg800_Network_Register_Urcs(void) {
    return Eg800_Urc_Register_Table(s_eg800_network_urc_routes, ARRAY_SIZE(s_eg800_network_urc_routes));
}



/**
 * @brief 同步执行一条 Network 服务的普通文本 AT 命令。
 * @param cmd_id Network 服务内部命令编号。
 * @param name 命令日志名称，可为 NULL。
 * @param tx 完整 AT 发送字符串，必须包含 "\r\n"，调用期间保持有效。
 * @param rsp_prefix 需要保存的响应前缀，NULL 表示保存未被 URC 路由消费的文本。
 * @param rx_buf 响应接收缓冲区，调用期间保持有效；不需要响应内容时传 NULL。
 * @param rx_buf_size 响应缓冲区容量，单位字节；rx_buf 为 NULL 时必须为 0。
 * @param timeout_ms 等待 OK 或 ERROR 的超时时间，单位 ms；0 表示使用 Service 默认值。
 * @param result_info AT 执行详细结果，可为 NULL。
 * @param completion_sem 当前调用任务专用的二值信号量，调用期间保持有效。
 * @return EG800_AT_RESULT_OK 表示命令成功，其他值表示对应的执行错误。
 * @note Network 和 AT 核心服务应已初始化；仅供任务上下文串行调用。
 * @note 禁止在 ISR、AT 核心 Service 任务及其 URC 回调中调用。
 * @note 同一信号量同一时间只允许对应一条未完成请求。
 */
static Eg800AtResult_e Eg800_Network_At_Execute(Eg800NetworkCmd_e cmd_id, const char *name, const char *tx, const char *rsp_prefix, char *rx_buf, size_t rx_buf_size, uint32_t timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {
    Eg800AtRequest_t request = {0};

    request.cmd.owner = EG800_AT_OWNER_NETWORK;
    request.cmd.id = (uint8_t)cmd_id;
    request.flow = EG800_AT_FLOW_TEXT;

    request.name = name;
    request.tx = tx;
    request.rsp_prefix = rsp_prefix;
    request.rx_buf = rx_buf;
    request.rx_buf_size = rx_buf_size;

    request.final_timeout_ms = timeout_ms;

    return Eg800_AT_Execute(&request, result_info, completion_sem);
}






























//--------------------------------------------------------公开的API接口--------------------------------------------------------
/**
 * @brief 初始化 Network 层并注册基础 URC。
 * @return RESULT_SUCCESS 表示初始化成功。
 * @note 本函数不发送 AT 命令，可以在模块上电前调用。
 */
Result_t Eg800_Network_Init(void) {
    BaseType_t task_result;

    // 幂等检查
    if (s_eg800_network_initialized == true) {
        return RESULT_SUCCESS;
    }

    Eg800_Network_Info_Reset();

    // 注册URC
    if (Eg800_Network_Register_Urcs() != RESULT_SUCCESS) {
        return RESULT_FAIL;
    }

    // 创建network 信号量
    s_network_at_completion_sem = xSemaphoreCreateBinary();
    if (s_network_at_completion_sem == NULL) {
        LOG_ERROR("Network AT 完成信号量创建失败");
        return RESULT_FAIL;
    }

    task_result = xTaskCreate(Eg800_Network_Task,
                              "EG Network Core Task",
                              EG800_NETWORK_TASK_STACK_SIZE,
                              NULL,
                              EG800_NETWORK_TASK_PRIORITY,
                              &s_eg800_network_task_handle);

    if (task_result != pdPASS) {
        s_eg800_network_task_handle = NULL;
        LOG_ERROR("Network 后台刷新任务创建失败");
        return RESULT_FAIL;
    }

    s_eg800_network_initialized = true;

    return RESULT_SUCCESS;
}

/**
 * @brief 判断PDP网络当前是否正常可用。
 * @return true表示PDP已激活并已经获得IP地址。
 */
bool Eg800_Network_Is_Active(void) {
    bool active;

    if (s_eg800_network_initialized == false) {
        return false;
    }

    taskENTER_CRITICAL();
    active = (s_eg800_network_snapshot.info.state == EG800_NETWORK_STATE_ACTIVE ? true : false);
    taskEXIT_CRITICAL();

    return active;
}


//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
/**
 * @brief 配置指定 PDP 场景的协议类型和 APN，使用空用户名、空密码及无鉴权方式。
 * @param context_id PDP 场景 ID，范围 1~15。
 * @param context_type 协议类型，1 为 IPv4，2 为 IPv6，3 为 IPv4v6。
 * @param apn APN 字符串，最多 20 字节；允许空字符串，不允许引号、反斜杠及控制字符。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量，同一时间仅供一条请求使用。
 * @return AT 执行结果；未初始化、参数无效或命令生成失败时返回 PARAM_ERROR。
 * @note 本接口只配置 PDP 参数，不激活网络，也不修改联网意图及 Network 快照。
 */
Eg800AtResult_e Eg800_Network_Configure_PDP_Context(uint8_t context_id, uint8_t context_type, const char *apn, SemaphoreHandle_t completion_sem) {
    size_t apn_len;
    char ch;
    int command_len;
    char command[EG800_NETWORK_APN_MAX_LENGTH + 40U];

    // 合法性检查
    if ((s_eg800_network_initialized == false) || (context_id == 0U) || (context_id > EG800_NETWORK_CONTEXT_ID_MAX) || (context_type < 1U) || (context_type > 3U) || (apn == NULL) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // APN 会直接写入带引号的 AT 参数，禁止破坏参数边界或引入额外命令。
    for (apn_len = 0U; apn_len <= EG800_NETWORK_APN_MAX_LENGTH; apn_len++) {
        ch = apn[apn_len];
        if (ch == '\0') {
            break;
        }
        if ((ch < 0x20U) || (ch > 0x7EU) || (ch == '"') || (ch == '\\')) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
    }

    if (apn_len > EG800_NETWORK_APN_MAX_LENGTH) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QICSGP=%u,%u,\"%s\",\"\",\"\",0\r\n", (unsigned int)context_id, (unsigned int)context_type, apn);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Network_At_Execute(EG800_NETWORK_CMD_QICSGP, "配置 PDP 上下文场景", command, NULL, NULL, 0U, EG800_NETWORK_CONFIG_TIMEOUT_MS, NULL, completion_sem);
}


/**
 * @brief 激活指定 PDP 场景。
 * @param context_id PDP 场景 ID，范围 1~15。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量，同一时间仅供一条请求使用。
 * @return AT 执行结果；未初始化、参数无效或命令生成失败时返回 PARAM_ERROR。
 * @note OK 仅表示激活命令成功；IP 地址应通过 Query_PDP_Context 查询，不在此更新快照。
 */
Eg800AtResult_e Eg800_Network_Activate_PDP(uint8_t context_id, SemaphoreHandle_t completion_sem) {
    char command[24];
    int command_len;

    // 合法性检查
    if ((s_eg800_network_initialized == false) || (context_id == 0U) || (context_id > EG800_NETWORK_CONTEXT_ID_MAX) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QIACT=%u\r\n", (unsigned int)context_id);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Network_At_Execute(EG800_NETWORK_CMD_QIACT, "激活 PDP 场景", command, NULL, NULL, 0U, EG800_NETWORK_ACTIVATE_TIMEOUT_MS, NULL, completion_sem);
}


/**
 * @brief 查询 PDP 激活列表，提取指定场景的激活状态、协议类型和 IP 地址。
 * @param context_id 需要查询的 PDP 场景 ID，范围 1~15。
 * @param context 查询结果输出；参数检查通过后先清空，只有返回 OK 时才可使用。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量，同一时间仅供一条请求使用。
 * @return AT 执行结果；参数无效时返回 PARAM_ERROR，分配失败、响应截断或格式错误时返回 DATA_FAIL。
 * @note 查询成功但列表中没有指定 ID 时，返回 OK，active 为 false，context_type 为 0，IP 为空。
 * @note 结构体只保存一个地址；同一场景同时返回 IPv4 和 IPv6 时优先保存 IPv4。
 * @note 本接口不修改 Network 快照；响应缓冲区按调用分配并释放，避免占用大块任务栈。
 */
Eg800AtResult_e Eg800_Network_Query_PDP_Context(uint8_t context_id, Eg800NetworkPdpContext_t *context, SemaphoreHandle_t completion_sem) {
    char response[EG800_NETWORK_QUERY_RESPONSE_SIZE] = {0}; // 整块相应缓冲区
    char *line;                                         //当前正在解析的这一行的首地址
    char *next_line;                                    //当前行末尾的 '\n' 地址；加 1 才是下一行起点
    char *ip_start;                                     //解析过程中逐步移动，最终指向 IP 的第一个字符
    char *ip_end;                                       //IP 后面那个结束双引号的地址
    char *tail;                                         //结束双引号后面的地址，用于检查行尾
    size_t ip_len;              
    unsigned int parsed_id;                             //当前这一行解析出来的场景 ID
    unsigned int parsed_state;                          //当前这一行的激活状态
    unsigned int parsed_type;                           //当前这一行的地址类型
    int fields_end;                                     //sscanf() 解析完三个数值字段后，已经读取的字符数
    Eg800NetworkPdpContext_t parsed_context = {0};      //暂存准备返回给调用者的结果
    Eg800AtResultInfo_t result_info = {0};              //AT 核心返回的详细执行信息，这里主要使用 rx_len
    Eg800AtResult_e result;

    // 合法性检查
    if ((s_eg800_network_initialized == false) || (context_id == 0U) || (context_id > EG800_NETWORK_CONTEXT_ID_MAX) || (context == NULL) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 清空缓冲区
    (void)memset(context, 0, sizeof(*context));

    context->context_id = context_id;
    parsed_context.context_id = context_id;


    result = Eg800_Network_At_Execute(EG800_NETWORK_CMD_QIACT_QUERY, "查询 PDP 场景", "AT+QIACT?\r\n", "+QIACT:", response, EG800_NETWORK_QUERY_RESPONSE_SIZE, EG800_NETWORK_QUERY_TIMEOUT_MS, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // Service 在缓冲区不足时截断文本，不能据此认定未出现在响应中的场景没有激活。
    if (result_info.rx_len >= (EG800_NETWORK_QUERY_RESPONSE_SIZE - 1U)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    line = response;
    while (*line != '\0') {
        next_line = strchr(line, '\n');
        if (next_line != NULL) {
            *next_line = '\0';
        }

        // 限制数值字段宽度，并用 %n 确认三个字段均已完整匹配。
        fields_end = 0;
        if ((sscanf(line, "+QIACT: %2u , %1u , %1u %n", &parsed_id, &parsed_state, &parsed_type, &fields_end) != 3) || (fields_end == 0) || (parsed_id == 0U) || (parsed_id > EG800_NETWORK_CONTEXT_ID_MAX) || (parsed_state > 1U) || (parsed_type < 1U) || (parsed_type > 2U)) {
            result = EG800_AT_RESULT_DATA_FAIL;
            break;
        }

        ip_start = line + fields_end;
        ip_len = 0U;
        if (*ip_start != '\0') {
            if (*ip_start != ',') {
                result = EG800_AT_RESULT_DATA_FAIL;
                break;
            }
            ip_start++;
            while ((*ip_start == ' ') || (*ip_start == '\t')) {
                ip_start++;
            }
            if (*ip_start != '"') {
                result = EG800_AT_RESULT_DATA_FAIL;
                break;
            }
            ip_start++;
            ip_end = strchr(ip_start, '"');
            if (ip_end == NULL) {
                result = EG800_AT_RESULT_DATA_FAIL;
                break;
            }
            ip_len = (size_t)(ip_end - ip_start);
            tail = ip_end + 1;
            while ((*tail == ' ') || (*tail == '\t') || (*tail == '\r')) {
                tail++;
            }
            if ((ip_len >= sizeof(parsed_context.ip_addr)) || (*tail != '\0')) {
                result = EG800_AT_RESULT_DATA_FAIL;
                break;
            }
        }

        if ((parsed_state == 1U) && (ip_len == 0U)) {
            result = EG800_AT_RESULT_DATA_FAIL;
            break;
        }

        // 必须解析完整列表；后续响应损坏时，不能把前面已找到的数据当成有效查询结果。
        if ((parsed_id == context_id) && ((parsed_context.active == false) || ((parsed_context.context_type == 2U) && (parsed_type == 1U) && (parsed_state == 1U)))) {
            parsed_context.active = (parsed_state == 1U);
            parsed_context.context_type = (uint8_t)parsed_type;
            (void)memset(parsed_context.ip_addr, 0, sizeof(parsed_context.ip_addr));
            if (parsed_context.active == true) {
                (void)memcpy(parsed_context.ip_addr, ip_start, ip_len);
            }
        }

        if (next_line == NULL) {
            break;
        }
        line = next_line + 1;
    }

    if (result == EG800_AT_RESULT_OK) {
        *context = parsed_context;
    }
    return result;
}

/**
 * @brief 去激活指定 PDP 场景，同时关闭依附该场景的 TCP/IP 连接。
 * @param context_id PDP 场景 ID，范围 1~15。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量，同一时间仅供一条请求使用。
 * @return AT 执行结果；未初始化、参数无效或命令生成失败时返回 PARAM_ERROR。
 * @note 本接口只执行去激活命令；停止自动重连应由 Disconnect 设置联网意图。
 */
Eg800AtResult_e Eg800_Network_Deactivate_PDP(uint8_t context_id, SemaphoreHandle_t completion_sem) {
    char command[24];
    int command_len;

    if ((s_eg800_network_initialized == false) || (context_id == 0U) || (context_id > EG800_NETWORK_CONTEXT_ID_MAX) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QIDEACT=%u\r\n", (unsigned int)context_id);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Network_At_Execute(EG800_NETWORK_CMD_QIDEACT, "去激活 PDP 场景", command, NULL, NULL, 0U, EG800_NETWORK_DEACTIVATE_TIMEOUT_MS, NULL, completion_sem);
}





















































//-----------------------------------------------网络层核心维护任务-------------------------------------------------------

static void Eg800_Network_Task(void *argument) {
    uint32_t events;                       // 事件
    BaseType_t wait_result;                // 等待完成通知时取得的通知结果
    TickType_t wait_ticks = portMAX_DELAY; // 下一层最多要等多久？
    bool should_connect;                   // 上层目前是否希望联网
    bool connect_now;                      // 这一次循环是否需要执行联网？
    Eg800NetworkInfo_t network_info;       // 当前网络处于哪个阶段
    (void)argument;

    while (1) {
        events = 0U;
        connect_now = false;
        wait_result = xTaskNotifyWait(0, UINT32_MAX, &events, wait_ticks);
        if (events & EG800_NETWORK_EVENT_CONNECT) {
        } else if (events & EG800_NETWORK_EVENT_DISCONNECT) {
        }
    }
}
