#ifndef EG_SERVICE_CFG_H_
#define EG_SERVICE_CFG_H_




// Service 层所使用资源
#define EG800_SERVICE_TASK_STACK_SIZE               256U                    // 任务任务栈大小
#define EG800_SERVICE_TASK_PRIORITY                 4U                      // 任务任务优先级
#define EG800_AT_QUEUE_LENGTH                       5U                      // AT 命令队列长度
#define EG800_SERVICE_EVENT_WAIT_MS                 1000U                   // 无事件时的最长阻塞时间，同时保证周期检查AT超时

#define EG800_SERVICE_RX_CHUNK_SIZE                 1024U                   // 单次读取底层 RX 环形缓冲区的最大字节数
#define EG800_SERVICE_LINE_BUF_SIZE                 2048U                   // AT 文本行缓冲区大小，应足够大以容纳一行完整的 AT 响应

#define EG800_SERVICE_URC_ROUTE_MAX                 64U                     // 注册URC前缀的个数上限









#endif // EG_SERVICE_CFG_H_
