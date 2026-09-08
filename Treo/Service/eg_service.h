#ifndef EG_SERVICE_H_
#define EG_SERVICE_H_

#include "types.h"      // for Result_t
#include "stdint.h"
#include "stdbool.h"
#include "stddef.h"
#include "string.h"

/* 请求字段为 0 时使用的默认超时 */
#define EG800_AT_DEFAULT_FINAL_TIMEOUT_MS           3000U                   // 默认等待 OK/ERROR 的超时时间
#define EG800_AT_DEFAULT_URC_TIMEOUT_MS             5000U                   // 默认等待异步 URC 的超时时间
#define EG800_AT_DEFAULT_DATA_TIMEOUT_MS            30000U                  // 默认等待原始二进制数据的超时时间

/* 调用者可以为每条请求指定超时，但服务层会限制在以下最大值内 */
#define EG800_AT_MAX_FINAL_TIMEOUT_MS               60000U                  // 单次最大等待 OK/ERROR 的超时时间
#define EG800_AT_MAX_URC_TIMEOUT_MS                 130000U                 // 单次最大等待异步 URC 的超时时间
#define EG800_AT_MAX_DATA_TIMEOUT_MS                120000U                 // 单次等待原始二进制数据的最大超时时间


#define EG800_AT_MAX_RAW_SIZE                       1050U                   // 单次原始数据接收最大长度1KB，再给点余量


// 通知
#define EG800_SERVICE_NOTIFY_INDEX                  0U                      // EG800服务任务的工作事件
#define EG800_AT_COMPLETION_NOTIFY_INDEX            1U                      // AT调用任务的完成事件


#define EG800_AT_SYNC_MAX_WAIT_MS                   400000U                 // Eg800_AtExec成功入队后，等待整条请求完成的最大全局保护时间





typedef enum {
    EG800_AT_OWNER_NONE = 0U,                   // 未指定归属
    EG800_AT_OWNER_BASIC,                       // 基础命令
    EG800_AT_OWNER_NETWORK,                     // 网络注册/PDP
    EG800_AT_OWNER_MQTT,                        // MQTT 通讯
    EG800_AT_OWNER_HTTP,                        // HTTP 请求/下载
    EG800_AT_OWNER_LOCATION,                    // 定位服务（BDS、LBS、WIFI_SCAN）
    EG800_AT_OWNER_SYSTEM                       // 模块系统事件
} EG800AtOwner_e;


// AT执行流枚举
typedef enum {
    EG800_AT_FLOW_TEXT = 0U,                    // 普通文本 AT，按行接收，等待 OK/ERROR 结束
    EG800_AT_FLOW_TEXT_URC,                     // 文本 AT 返回 OK 后，继续等待指定 URC
    EG800_AT_FLOW_SEND_DATA,                    // 等待提示符 > 后发送原始数据，再等待 OK/ERROR
    EG800_AT_FLOW_RECV_RAW                      // 等待 CONNECT 后接收固定长度原始数据，再等待 OK/ERROR
} Eg800AtFlow_e;



// AT执行结果枚举
typedef enum {
    EG800_AT_RESULT_OK = 0U,                    // 命令成功
    EG800_AT_RESULT_ERROR,                      // 模块返回 ERROR
    EG800_AT_RESULT_CME_ERROR,                  // 模块返回 +CME ERROR
    EG800_AT_RESULT_CMS_ERROR,                  // 模块返回 +CMS ERROR
    EG800_AT_RESULT_TIMEOUT,                    // 等待响应、URC 或原始数据超时
    EG800_AT_RESULT_SEND_FAIL,                  // 底层发送失败
    EG800_AT_RESULT_DATA_FAIL,                  // 原始数据处理失败
    EG800_AT_RESULT_CANCELLED,                  // 会话切换或关机取消（命令被取消）
    EG800_AT_RESULT_QUEUE_FULL,                 // AT 请求队列已满，没发送出去
    EG800_AT_RESULT_PARAM_ERROR                 // 参数错误或服务未初始化
} Eg800AtResult_e;


// AT 命令结构化身份标识
typedef struct {
    uint8_t owner;  // 命令所属模块，取值参考 Eg800AtOwner_e
    uint8_t id;     // 所属模块内部定义的命令编号
} Eg800AtCmd_t;




// -------------------回调函数-------------------------
// URC处理函数
typedef void (*Eg800AtUrcHandler_t)(const char *line);                          

// 模块提交给 Service 的 URC 注册项
typedef struct {
    const char *prefix;                 // URC 匹配前缀，字符串必须长期有效
    EG800AtOwner_e owner;               // URC 归属模块
    Eg800AtUrcHandler_t handler;        // 匹配后在 Service 任务中执行的处理函数
} Eg800UrcRegistration_t;






//一次AT请求执行完成后的详细信息
typedef struct {
    Eg800AtResult_e result;         // 执行结果
    Eg800AtCmd_t cmd;               // 逻辑命令编号
    size_t rx_len;                  // 写入 rx_buf 的文本长度
    size_t raw_len;                 // 已接收原始数据长度
    int module_error;               // CME/CMS 错误码，未知时为 -1
} Eg800AtResultInfo_t;





// AT 请求结构体（调用者填写该结构体后传给Eg800_AtExec）
typedef struct {
    Eg800AtCmd_t cmd;                    // 逻辑命令编号
    Eg800AtFlow_e flow;                  // 采用的AT通信流程
    const char *name;                    // 日志名，可为 NULL
    const char *tx;                      // 实际发送字符串，必须带 "\r\n"
    const char *rsp_prefix;              // 需要保存的文本响应前缀，NULL 表示保存非 URC 文本
    const char *expect_urc_prefix;       // 需要等待的异步URC前缀；SEND_DATA/RECV_RAW填写后也会在OK后继续等待该URC
    const char *prompt_prefix;           // 数据提示符，NULL 时由 flow 决定默认提示

    uint32_t final_timeout_ms;           // 等待 OK/ERROR 的超时时间，0 使用默认值
    uint32_t urc_timeout_ms;             // 等待异步 URC 的超时时间，0 使用默认值
    uint32_t data_timeout_ms;            // 等待原始数据的超时时间，0 使用默认值

    char *rx_buf;                        // 调用者提供的文本响应缓冲区，可为 NULL
    size_t rx_buf_size;                  // 调用者提供的文本响应缓冲区大小

    //仅SEND_DATA流程使用
    const uint8_t *tx_data;              // 提示符后发送的原始数据，
    size_t tx_data_len;                  // 提示符后发送的原始数据长度

    //仅RECV_RAW流程使用
    uint8_t *raw_buf;                    // 调用者提供的用于保存模块返回原始数据的缓冲区，可为 NULL
    size_t raw_buf_size;                 // 调用者提供的用于保存模块返回原始数据的缓冲区大小
    size_t raw_expect_len;               // 期望接收的原始数据长度(raw_expect_len：> 0 且 <= raw_buf_size 且 <= EG800_AT_MAX_RAW_SIZE)
    bool allow_during_shutdown;          // Service停止接收普通请求时，是否仍允许执行该请求(一般命令为false,关机、断开网络等收尾命令可设为true)
} Eg800AtRequest_t;






Result_t Eg800_Service_Init(void);


// 同步提交并执行一条AT请求。
Eg800AtResult_e Eg800_AtExec(const Eg800AtRequest_t *request,Eg800AtResultInfo_t *result_info);




// URC 路由注册
Result_t Eg800_Urc_Register(const char *prefix, uint8_t owner, Eg800AtUrcHandler_t handler);
Result_t Eg800_Urc_Register_Table(const Eg800UrcRegistration_t *routes, size_t route_count);




void Eg800_Service_Rx_Notify_From_ISR(void);





#endif // EG_SERVICE_H_


