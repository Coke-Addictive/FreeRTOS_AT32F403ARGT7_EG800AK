#ifndef EG_HTTP_H_
#define EG_HTTP_H_


#include "eg_service.h"


// 任务资源
#define EG800_HTPP_TASK_STACK_SIZE              768U            // 栈深度，单位 StackType_t，当前平台共 3072 字节
#define EG800_HTPP_TASK_PRIORITY                10U             // 低于 Service 和 Network 任务

// 队列资源
#define EG800_HTPP_RECEIVE_QUEUE_LENGTH         2U              // 接收队列最多缓存的消息条数
#define EG800_HTPP_RECEIVE_MAX_LENGTH           2048U           // 单条消息内容最大长度，单位字节

#define EG800_HTTP_SSL_CONTEXT_ID               1U              // HTTP 使用的 SSL 上下文编号
#define EG800_MQTT_SSL_VERSION                  4U              // HTTP V4 （全部）


typedef struct {
    uint8_t responseheader;                                     // 临时占位
} Eg800HttpConfig_t;

// HTTP 接收消息
typedef struct {
    uint8_t responseheader;                                     // 临时占位
} Eg800HttpReceiveMessage_t;


// ----------------------------------------------------基础命令封装----------------------------------------------------
Eg800AtResult_e Eg800_Http_Set_PDP_Context(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_Context(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_Version(uint8_t version, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_Version(uint8_t version, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_Security_Level(uint8_t level, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_SSL_SIN(uint8_t version, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_CipherSuite(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_RequestHeader(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_ResponseHeader(SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Set_URL(uint16_t url_lengt, uint32_t time_out, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Send_Get(uint8_t rsptime_s, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Send_Get_Range(uint8_t rsptime_s, int32_t start_position, int32_t read_len, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Http_Get_Read(uint8_t rsptime_s, SemaphoreHandle_t completion_sem);



// ----------------------------------------------------通用 API-------------------------------------------------------
Result_t Eg800_Http_Init(void);





#endif














