#ifndef EG_HTTP_H_
#define EG_HTTP_H_


#include "eg_service.h"


// 任务资源
#define EG800_HTPP_TASK_STACK_SIZE              768U            // 栈深度，单位 StackType_t，当前平台共 3072 字节
#define EG800_HTPP_TASK_PRIORITY                10U             // 低于 Service 和 Network 任务

#define EG800_HTTP_SSL_CONTEXT_ID               1U              // HTTPS 使用的 SSL 上下文编号
#define EG800_HTTP_SSL_VERSION                  4U              // 允许模组支持的全部 SSL/TLS 版本
#define EG800_HTTP_SSL_SECURITY_LEVEL           0U              // 不验证对端证书
#define EG800_HTTP_SSL_SNI                      1U              // TLS 服务器名称指示，1 启用，0 禁用
#define EG800_HTTP_REQUESTHEADER                0U              // 由模组生成 HTTP 请求头
#define EG800_HTTP_RESPONSEHEADER               0U              // 读取响应时只输出响应体
#define EG800_HTTP_CIPHERSUITE                  0xFFFFU         // 允许模组支持的全部加密套件

#define EG800_HTTP_URL_MAX_LENGTH               2047U           // URL 最大长度

#define EG800_HTTP_GET_RESPONE_DEFAULT_TIME     60U             // QHTTPGET 默认响应超时，单位 s
#define EG800_HTTP_GET_MAX_RESPONSE_TIME_S      300U            // QHTTPGET 允许的最大响应超时，单位 s
#define EG800_HTTP_DOWNLOAD_AT_TIMEOUT_S        60U             // 下载时 URL 输入、GETEX 响应和 READ 包间隔超时，单位 s

#define EG800_HTTP_DOWNLOAD_EVENT_QUEUE_LENGTH  1U              // 单槽事件队列，DATA 归还或撤销后才会投递下一事件




#define EG800_HTTP_DOWNLOAD_NOTIFY_START        (1UL << 0)      // 通知下载任务读取刚提交的请求
#define EG800_HTTP_DOWNLOAD_NOTIFY_RELEASE      (1UL << 1)      // 接收者已处理完上一包，可以复用缓冲区
#define EG800_HTTP_DOWNLOAD_NOTIFY_CANCEL       (1UL << 2)      // 通知下载任务在当前 AT 返回后取消本轮下载


typedef struct {
    uint16_t status_code;          // HTTP 状态码，如 200、206、404
    bool content_length_valid;     // 模组是否上报了响应体长度
    uint32_t content_length;       // 本次响应体长度，单位字节
} Eg800HttpResponseInfo_t;

// QHTTPREAD 的响应体缓冲区及读取长度
typedef struct {
    uint8_t *buffer;                // 调用方提供的响应体接收缓冲区
    size_t buffer_size;             // 缓冲区容量，单位字节
    size_t expected_len;            // 本次完整响应体长度，来自 GET 或 GETEX，单位字节
    size_t received_len;            // 本次实际写入的字节数，由读取函数填写
} Eg800HttpReadBuffer_t;


// 单次文件下载请求
typedef struct {
    const char *url;                // 以 '\0' 结尾的 URL，Start 接受请求时复制
    uint32_t file_size;             // 文件总长度，单位字节，末字节偏移须能用 int32_t 表示
    size_t chunk_size;             // 每段请求长度，范围 1~EG800_AT_MAX_RAW_SIZE，单位字节
} Eg800HttpDownloadRequest_t;


// 下载请求的提交结果
typedef enum {
    EG800_HTTP_DOWNLOAD_START_OK = 0,               // 已接受请求
    EG800_HTTP_DOWNLOAD_START_INVALID_PARAM,        // 请求参数无效
    EG800_HTTP_DOWNLOAD_START_NOT_READY,            // 下载任务尚未初始化
    EG800_HTTP_DOWNLOAD_START_BUSY,                 // 正在下载或上一轮终态尚未取走
    EG800_HTTP_DOWNLOAD_START_RECOVERY_REQUIRED     // 模组 HTTP 状态待确认
} Eg800HttpDownloadStartResult_e;

// 下载任务交付给调用者的事件
typedef enum {
    EG800_HTTP_DOWNLOAD_EVENT_DATA = 0,             // 一段响应体已完整接收
    EG800_HTTP_DOWNLOAD_EVENT_DONE,                 // 全部数据已交付，会话已收尾
    EG800_HTTP_DOWNLOAD_EVENT_FAILED                // 本轮已结束，包含失败或取消原因
} Eg800HttpDownloadEventType_e;



// 下载失败所在步骤
typedef enum {
    EG800_HTTP_DOWNLOAD_STAGE_NONE = 0,             // 无具体失败指令
    EG800_HTTP_DOWNLOAD_STAGE_SET_URL,              // 设置 URL
    EG800_HTTP_DOWNLOAD_STAGE_GET_RANGE,            // 请求分段或检查分段响应
    EG800_HTTP_DOWNLOAD_STAGE_READ,                 // 读取分段响应体
    EG800_HTTP_DOWNLOAD_STAGE_STOP                  // 关闭 HTTP 会话
} Eg800HttpDownloadStage_e;


typedef struct {
    Eg800HttpDownloadEventType_e type;              // 事件类型
    uint32_t offset;                                // 数据起始偏移、失败位置或完成时的总字节数
    size_t len;                                     // DATA 的有效字节数，终态为 0
    const uint8_t *data;                            // DATA 的只读内容，终态为 NULL
    Eg800HttpDownloadStage_e stage;                 // FAILED 的失败步骤
    Eg800AtResult_e result;                         // FAILED 的失败或取消原因
    uint16_t http_status;                           // 本段 HTTP 状态码，0 表示未取得
} Eg800HttpDownloadEvent_t;


// ----------------------------------------------------基础命令封装----------------------------------------------------
Eg800AtResult_e Eg800_Http_Set_PDP_Context(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_Context(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_Version(uint8_t version, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_Security_Level(uint8_t seclevel, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_SNI(uint8_t sni, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_CipherSuite(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_RequestHeader(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_ResponseHeader(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_URL(const char *url, int32_t url_length, uint8_t input_timeout_s, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Send_Get(Eg800HttpResponseInfo_t *response, uint16_t rsptime_s, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Send_Get_Range(Eg800HttpResponseInfo_t *response, uint8_t rsptime_s, int32_t start_position, int32_t read_len, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Get_Read(Eg800HttpReadBuffer_t *read_buffer, uint8_t rsptime_s, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_STOP(SemaphoreHandle_t completion_sem);


// ----------------------------------------------------公开  API-------------------------------------------------------
Result_t Eg800_Http_Init(void);

Eg800AtResult_e Eg800_Http_Configure_Basic(void);

Result_t Eg800_Http_Download_Init(void);

Eg800HttpDownloadStartResult_e Eg800_Http_Download_Start(const Eg800HttpDownloadRequest_t *request);

Result_t Eg800_Http_Download_Receive(Eg800HttpDownloadEvent_t *event, TickType_t wait_ticks);

Result_t Eg800_Http_Download_Cancel(void);

#endif // EG_HTTP_H_
