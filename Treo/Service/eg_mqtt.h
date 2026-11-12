#ifndef EG_MQTT_H_
#define EG_MQTT_H_



#include "eg_service.h"


// 任务资源
#define EG800_MQTT_TASK_STACK_SIZE           768U       // 栈深度，单位 StackType_t，当前平台共 3072 字节
#define EG800_MQTT_TASK_PRIORITY             10U        // 低于 Service 和 Network 任务


// MQTT 服务连接重试策略。
#define EG800_MQTT_RETRY_MAX                 5U         // 一轮最多尝试次数，包含第一次
#define EG800_MQTT_RETRY_INTERVAL_MS         120000U    // 两次连接尝试之间的冷却时间，单位 ms

// MQTT 默认连接参数。
#define EG800_MQTT_CLIENT_INDEX              0U         // 服务固定维护的客户端编号
#define EG800_MQTT_SERVER_PORT               1883U      // 非 TLS 服务器端口
#define EG800_MQTT_VERSION                   4U         // MQTT v3.1.1
#define EG800_MQTT_KEEPALIVE_S               60U        // 保活间隔，单位 s
#define EG800_MQTT_CLEAN_SESSION             true       // 建立干净会话，重连后重新订阅
#define EG800_MQTT_SSL_ENABLED               false      // 第一版使用普通 TCP 连接
#define EG800_MQTT_RECV_MODE                 0U         // 接收消息通过 URC 直接上报
#define EG800_MQTT_RECV_LENGTH_ENABLED       true       // 接收上报包含 payload 长度
#define EG800_MQTT_DEFAULT_QOS               0U         // 默认消息 QoS
#define EG800_MQTT_DEFAULT_RETAIN            false      // 默认不保留消息

// 模组内部协议包重发参数，与服务连接重试分别维护。
#define EG800_MQTT_PACKET_TIMEOUT_S          5U         // 协议包等待超时时间，单位 s
#define EG800_MQTT_PACKET_RETRY_TIMES        3U         // 协议包超时后的重发次数
#define EG800_MQTT_TIMEOUT_NOTICE_ENABLED    false      // 第一版关闭中间超时通知


// 接口长度约束；长度均按字节计算，不包含字符串结尾的 '\0'。
#define EG800_MQTT_HOST_MAX_LENGTH           100U       // MQTT V1.5 手册规定的服务器地址长度上限，单位字节
#define EG800_MQTT_TOPIC_MAX_LENGTH          256U       // 本项目约定的完整主题长度上限
#define EG800_MQTT_PUBLISH_MAX_LENGTH        1500U      // QMTPUBEX 单次 payload 最大长度
#define EG800_MQTT_WILL_MAX_LENGTH           256U       // 遗嘱负载长度上限，单位字节

// 用户连接配置的字符串存储容量。
#define EG800_MQTT_CONFIG_TEXT_SIZE          256U       // 每个字符串数组的容量，单位字节，包含结尾 '\0'

// 测试环境选择；1 使用测试服务器，0 使用正式服务器。
#define EG800_MQTT_TEST_ENVIRONMENT          1U
#if EG800_MQTT_TEST_ENVIRONMENT

#define EG800_MQTT_SERVER_HOST                    "broker.emqx.io"            // 测试域名

#else 
#define EG800_MQTT_LOGIN_KEY                  "GMDULTRA4317MQTT2025MQswCQYDVGMD4317Usp3mbMGA1"        // MD5登陆密钥
#define EG800_MQTT_SERVER_HOST                  "mqtt.gmd-cloud.com"                                    // 正式服务器域名



// 产品和主题定义同时用于测试环境与正式环境。
#define EG800_MQTT_PRODUCT_ID                   "GMD4318"                                              // 产品编号

// 订阅主题需要在前缀和后缀之间拼入设备 IMEI。
#define EG800_MQTT_SUB_TOPIC_PREFIX             "/GMD/Embedded/" EG800_MQTT_PRODUCT_ID "/Device/"       // 设备订阅主题前缀
#define EG800_MQTT_SUB_LOCATION_REPLY_SUFFIX    "/Position/Report/Reply"                                // 定位上报回复主题后缀
#define EG800_MQTT_SUB_FILE_UPDATE_SUFFIX       "/File/Update"                                          // 文件更新下发主题后缀
#define EG800_MQTT_SUB_OTA_SUFFIX               "/Firmware/Update"                                      // OTA 升级下发主题后缀
#define EG800_MQTT_SUB_CONFIG_GET_SUFFIX        "/Config"                                               // 获取设备配置主题后缀
#define EG800_MQTT_SUB_CONFIG_UPDATE_SUFFIX     "/Config/Update"                                        // 更新设备配置主题后缀
#define EG800_MQTT_SUB_REGISTER_REPLY_SUFFIX    "/Register/Reply"                                       // 设备注册回复主题后缀

// 发布主题由固定字符串组成，可以直接得到完整主题。
#define EG800_MQTT_PUB_TOPIC_PREFIX               "/GMD/Embedded/" EG800_MQTT_PRODUCT_ID "/Centre"                     // 发布主题公共前缀
#define EG800_MQTT_PUB_LOCATION_TOPIC             EG800_MQTT_PUB_TOPIC_PREFIX "/Position/Report"                       // 完整的定位上报主题
#define EG800_MQTT_PUB_FILE_UPDATE_REPLY_TOPIC    EG800_MQTT_PUB_TOPIC_PREFIX "/File/Update/Reply"                     // 完整的文件更新答复主题
#define EG800_MQTT_PUB_CONFIG_REPLY_TOPIC         EG800_MQTT_PUB_TOPIC_PREFIX "/Config/Reply"                          // 完整的获取配置答复主题
#define EG800_MQTT_PUB_CONFIG_UPDATE_REPLY_TOPIC  EG800_MQTT_PUB_TOPIC_PREFIX "/Config/Update/Reply"                   // 完整的更新配置答复主题
#define EG800_MQTT_PUB_OTA_REPLY_TOPIC            EG800_MQTT_PUB_TOPIC_PREFIX "/Firmware/Update/Reply"                 // 完整的 OTA 升级答复主题
#define EG800_MQTT_PUB_REGISTER_TOPIC             EG800_MQTT_PUB_TOPIC_PREFIX "/Register"                              // 完整的设备注册主题
#define EG800_MQTT_PUB_EVENT_TOPIC                EG800_MQTT_PUB_TOPIC_PREFIX "/Report/Event"                          // 完整的设备事件上报主题

#endif



// 用户提供的 MQTT 连接配置；字符串存放在实体数组中，均须以 '\0' 结尾。
typedef struct {
    uint16_t server_port;                               // 服务器端口
    uint8_t client_id;                                  // MQTT 客户端标
    char server_host[EG800_MQTT_CONFIG_TEXT_SIZE];      // 服务器域名或 IP 地址，有效长度仍受 HOST_MAX_LENGTH 限制
    char username[EG800_MQTT_CONFIG_TEXT_SIZE];         // 登录用户名
    char password[EG800_MQTT_CONFIG_TEXT_SIZE];         // 登录密码
} Eg800MqttConfig_t;


// MQTT 服务运行状态
typedef enum {
    EG800_MQTT_STATE_NONE = 0U,                         // 尚未确认模块中的 MQTT 客户端状态
    EG800_MQTT_STATE_IDLE,                              // 已确认 MQTT 连接资源清理完成，当前未连接
    EG800_MQTT_STATE_WAIT_NETWORK,                      // 存在连接意图，等待 Network 连接完成并提供可用的 PDP
    EG800_MQTT_STATE_CONNECTING,                        // 正在配置 MQTT 参数、打开连接并登录服务器
    EG800_MQTT_STATE_ONLINE,                            // MQTT 登录成功，可以提交订阅和发布请求
    EG800_MQTT_STATE_DISCONNECTING,                     // 正在断开 MQTT 会话并清理客户端连接资源
    EG800_MQTT_STATE_RETRY_WAIT,                        // 本次连接失败且清理完成，等待下一次重试
    EG800_MQTT_STATE_FAULT                              // 尝试次数耗尽、清理失败或配置无效，停止自动重试
} Eg800MqttState_e;



// MQTT 服务保存的客户端信息
typedef struct {
    uint8_t client_index;                               // 模块中的 MQTT 客户端编号
    Eg800MqttState_e state;                             // MQTT 服务当前运行状态
} Eg800MqttInfo_t;



// MQTT 服务状态快照
typedef struct {
    Eg800MqttInfo_t info;                               // 当前客户端编号和运行状态
    bool refresh_in_progress;                           // 是否正在建立连接或执行断开清理
    TickType_t update_tick;                             // 最近一次状态更新时刻，单位 tick
} Eg800MqttSnapshot_t;


//----------------------------------------------------基础命令封装-------------------------------------------------------------------

// 以下基础接口固定使用 EG800_MQTT_CLIENT_INDEX，不由调用者指定客户端编号。
// 仅供任务上下文调用，禁止在 ISR、Service 任务及 URC 回调中调用；同一客户端的操作由上层串行组织。
// 字符串按字节限长；指令内的字符串不接受双引号、反斜杠和控制字符，任意字节负载使用定长接口。
// 所有输入、输出缓冲区及 completion_sem 必须保持有效直到调用返回；结构化查询失败时输出保持原值。
// 连接和云认证接口包含约 0.8 KiB 的局部命令缓冲区，调用任务还需预留其余调用链的栈空间。
// 配置接口应在建立 MQTT 连接前调用；基础接口不驱动后台状态机，也不负责 PDP 激活、证书上传或自动重连。

// 查询模块支持的全部配置项；response 由调用者提供，容量不足返回 DATA_FAIL，并保留已收到的文本。
Eg800AtResult_e Eg800_Mqtt_Query_Config_Support(char *response, size_t response_size, SemaphoreHandle_t completion_sem);

// MQTT 参数配置。
Eg800AtResult_e Eg800_Mqtt_Set_Version(uint8_t version, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_PDP_Context(uint8_t context_id, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_SSL(bool enabled, uint8_t ssl_context_index, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_Keepalive(uint16_t keepalive_s, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_Clean_Session(bool enabled, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_Packet_Timeout(uint8_t timeout_s, uint8_t retry_times, bool timeout_notice, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_Receive_Mode(uint8_t mode, bool include_length, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_Data_Format(uint8_t send_mode, uint8_t recv_mode, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_Edit_Timeout(bool enabled, uint8_t timeout_s, SemaphoreHandle_t completion_sem);

// 查询当前模块配置；timeout_s 单位 s，retry_times 为报文重传次数。
Eg800AtResult_e Eg800_Mqtt_Query_Keepalive(uint16_t *keepalive_s, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Query_Clean_Session(bool *enabled, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Query_Packet_Timeout(uint8_t *timeout_s, uint8_t *retry_times, bool *timeout_notice, SemaphoreHandle_t completion_sem);

// 遗嘱配置；enabled 为 false 时忽略其余遗嘱参数。定长接口不附加回车、换行或 Ctrl+Z。
Eg800AtResult_e Eg800_Mqtt_Set_Will(bool enabled, uint8_t qos, bool retain, const char *topic, const char *message, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_Will_Ex(bool enabled, uint8_t qos, bool retain, const char *topic, const uint8_t *payload, size_t payload_len, SemaphoreHandle_t completion_sem);

// TLS 证书绑定；ssl_context_index 范围 0~5，文件应已上传到模块，认证方式和 TLS 版本需另行配置。
Eg800AtResult_e Eg800_Mqtt_Set_SSL_CA_Cert(uint8_t ssl_context_index, const char *path, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_SSL_Client_Cert(uint8_t ssl_context_index, const char *path, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_SSL_Client_Key(uint8_t ssl_context_index, const char *path, SemaphoreHandle_t completion_sem);

// 云平台专用认证；各字段最多 EG800_MQTT_CONFIG_TEXT_SIZE - 1 字节。
Eg800AtResult_e Eg800_Mqtt_Set_Aliyun_Auth(const char *product_key, const char *device_name, const char *device_secret, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Set_OneNET_Auth(const char *product_id, const char *access_key, SemaphoreHandle_t completion_sem);


// 唤醒主题配置；配置唤醒主题不代替订阅。
Eg800AtResult_e Eg800_Mqtt_Clear_Wakeup_Topics(SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Add_Wakeup_Topic(const char *topic, SemaphoreHandle_t completion_sem);

// 客户端网络与 MQTT 会话控制。
// Open/Connect 等待最终结果 URC；业务失败返回 ERROR，格式错误返回 DATA_FAIL，传输错误原样返回。
// Connect 的 username 为 NULL 时必须同时省略 password；仅 password 为 NULL 时省略密码。
Eg800AtResult_e Eg800_Mqtt_Open_Network(const char *host, uint16_t port, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Close_Network(SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Connect_Server(const char *client_id, const char *username, const char *password, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Disconnect_Server(SemaphoreHandle_t completion_sem);


// 单主题订阅、退订和定长发布。
Eg800AtResult_e Eg800_Mqtt_Subscribe_Topic(uint16_t msg_id, const char *topic, uint8_t qos, SemaphoreHandle_t completion_sem);
// 扩展订阅接口可输出实际获准的 QoS；服务器拒绝订阅时返回 ERROR，granted_qos 为 NULL 时不输出。
Eg800AtResult_e Eg800_Mqtt_Subscribe_Topic_Ex(uint16_t msg_id, const char *topic, uint8_t qos, uint8_t *granted_qos, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Unsubscribe_Topic(uint16_t msg_id, const char *topic, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Mqtt_Publish_Topic(uint16_t msg_id, const char *topic, uint8_t qos, bool retain, const uint8_t *payload, size_t payload_len, SemaphoreHandle_t completion_sem);


//[TODO]:AT+QMTCFG="wakeup/topic",0,0 清除旧的唤醒topic
//[TODO]:AT+QMTCFG="wakeup/topic",0,1,"<topic>" 逐个配置唤醒topic




//AT+QMTOPEN=0,"mqtt.gmd-cloud.com",1883
//AT+QMTCONN=0,"GMD4318|<IMEI>","<用户名>","<MD5密码>"    “用户名格式 GMD4318|<IMEI>|<Tick>，密码为明文 GMD4318|<IMEI>|<Tick>|<LOGIN_KEY> 的 MD5”
// AT+QMTSUB=0,1,"<topic>",0 串行订阅



//AT+QMTPUBEX=0,<msgid>,0,0,"<topic>",<len>  发布
//AT+QMTDISC=0          断开
//AT+QMTCLOSE=0 关闭



// 初始化只创建资源、注册 URC
Result_t Eg800_Mqtt_Init(void);

// 提交连接意图；成功不代表连接或断开已经完成。
Result_t Eg800_Mqtt_Connect(void);
Result_t Eg800_Mqtt_Disconnect(void);

// 仅读取软件缓存，不发送 AT 命令
bool Eg800_Mqtt_Is_Online(void);
Result_t Eg800_Mqtt_Get_Snapshot(Eg800MqttSnapshot_t *snapshot);





#endif // EG_MQTT_H_

