#ifndef EG_SERVICE_CFG_H_
#define EG_SERVICE_CFG_H_

// Service 层日志开关，1 为开启，0 为关闭。
#define EG_SERVICE_LOG_ENABLE                       (1)    // Service   模块日志开关
#define EG_BASIC_LOG_ENABLE                         (0)    // Basic     模块日志开关
#define EG_NETWORK_LOG_ENABLE                       (0)    // Network   模块日志开关
#define EG_MQTT_LOG_ENABLE                          (0)    // MQTT      模块日志开关
#define EG_HTTP_LOG_ENABLE                          (0)    // HTTP      模块日志开关
#define EG_TIMER_LOG_ENABLE                         (0)    // Timer     模块日志开关
#define EG_LOCATE_LOG_ENABLE                        (0)    // Locate    模块日志开关



// ↓↓↓↓↓↓ Service各层所使用资源 ↓↓↓↓↓↓


// Service 层
#define EG800_SERVICE_TASK_STACK_SIZE               256U                    // 任务任务栈大小
#define EG800_SERVICE_TASK_PRIORITY                 12U                     // 任务任务优先级
#define EG800_AT_QUEUE_LENGTH                       8U                      // AT 命令队列长度
#define EG800_SERVICE_EVENT_WAIT_MS                 1000U                   // 无事件时的最长阻塞时间，同时保证周期检查AT超时



#define EG800_SERVICE_RX_CHUNK_SIZE                 2048U                   // 单次从驱动读取的字节数上限
#define EG800_SERVICE_LINE_BUF_SIZE                 2048U                   // AT 单行缓冲区容量，单位字节
#define EG800_AT_MAX_RAW_SIZE                       2048U                   // 单次 RAW 接收长度上限，单位字节

#define EG800_SERVICE_URC_ROUTE_MAX                 64U                     // 注册URC前缀的个数上限


// Basic
#define EG800_BASIC_TASK_STACK_SIZE                 384U                    // Basic 后台刷新任务栈大小
#define EG800_BASIC_TASK_PRIORITY                   8U                      // 低于 EG800 Service 任务优先级


// NetWork
#define EG800_NETWORK_TASK_STACK_SIZE               768U                    // Network任务栈深度
#define EG800_NETWORK_TASK_PRIORITY                 11U                     // 低于EG Service任务优先级


// Mqtt
#define EG800_MQTT_TASK_STACK_SIZE                  768U                    // 栈深度，单位 StackType_t，当前平台共 3072 字节
#define EG800_MQTT_TASK_PRIORITY                    10U                     // 低于 Service 和 Network 任务


// Http
#define EG800_HTPP_TASK_STACK_SIZE                  768U                    // 栈深度，单位 StackType_t，当前平台共 3072 字节
#define EG800_HTPP_TASK_PRIORITY                    10U                     // 低于 Service 和 Network 任务


// Locate
#define EG800_GNSS_TASK_STACK_SIZE                  384U                    // Basic 后台刷新任务栈大小
#define EG800_GNSS_TASK_PRIORITY                    7U                      // 低于 EG800 Service 任务优先级

#endif // EG_SERVICE_CFG_H_






















/*

Service 串口接收缓冲区

数据先从驱动的 RX 软件环形缓冲区读到 s_eg800_rx_chunk，再按接收状态送入
s_eg800_line_buf（AT 文本）或 raw_buf（RAW 正文）。驱动的 DMA 循环缓冲区和
RX 软件环形缓冲区分别由 EG_RX_DMA_BUF_SIZE 和 EG_RX_RING_BUF_SIZE 配置。

EG800_SERVICE_RX_CHUNK_SIZE
- Service 用本宏分配临时缓冲区 s_eg800_rx_chunk。每次 Drv_EG_Read() 最多读取此字节数，
  读到多少就处理多少。读满一批时，Service 会继续调用 Drv_EG_Read()。
- 调大增加静态 RAM 占用，底层已有足量数据时可用更少的调用读完。调小节省 RAM，
  读完相同的数据可能需要更多次调用。若改为 512 字节，已就绪的 2048 字节会分 4 批处理。
  完整的 AT 文本行和 RAW 正文都可以跨多批接收。

EG800_SERVICE_LINE_BUF_SIZE
- Service 用本宏分配 s_eg800_line_buf。每次拼接一行 AT 响应或 URC，收到换行后解析。
  缓冲区需留 1 字节存放字符串终止符，设置为 500 时最多容纳 499 字节的单行文本。
- 调大增加静态 RAM 占用，可接收更长的 AT 单行。调小节省 RAM，但单行超过容量减 1 字节时，
  当前实现会输出 EG line overflow 并复位 MCU。
- 容量按所有启用功能的最长单行设置。MQTT 的 +QMTRECV 如果在同一行携带主题和正文，
  要把前缀、主题、其他字段和正文一起计入。

EG800_AT_MAX_RAW_SIZE
- Service 用本宏校验单次 RAW 请求的长度。调用方通过 raw_buf 提供实际存储空间，
  raw_expect_len 必须同时小于或等于本宏和 raw_buf_size。
- HTTP 下载用本宏分配 s_eg800_http_download_data。启用 HTTP 下载后，该数组的静态 RAM
  占用随宏值增减。实际单段长度由下载请求的 chunk_size 指定，并受本宏限制。
- 调大后可配置更大的单段。调小到 1024 时，原来配置为 2048 字节的单段请求会被拒绝，
  将 chunk_size 改为 1024 后，同样的 2048 字节正文可分两段下载。
- QHTTPREAD 识别到 CONNECT 行后，正文按预期长度从 s_eg800_rx_chunk 拷到 raw_buf。
  收满后回到文本解析，OK 和结果 URC 继续进入 s_eg800_line_buf。



*/








