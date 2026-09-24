#ifndef EG_SERVICE_CFG_H_
#define EG_SERVICE_CFG_H_




// Service 层所使用资源
#define EG800_SERVICE_TASK_STACK_SIZE               256U                    // 任务任务栈大小
#define EG800_SERVICE_TASK_PRIORITY                 12U                     // 任务任务优先级
#define EG800_AT_QUEUE_LENGTH                       8U                      // AT 命令队列长度
#define EG800_SERVICE_EVENT_WAIT_MS                 1000U                   // 无事件时的最长阻塞时间，同时保证周期检查AT超时

#define EG800_SERVICE_RX_CHUNK_SIZE                 1024U                   // 单次读取底层 RX 环形缓冲区的最大字节数
#define EG800_SERVICE_LINE_BUF_SIZE                 2048U                   // AT 文本行缓冲区大小，应足够大以容纳一行完整的 AT 响应

#define EG800_SERVICE_URC_ROUTE_MAX                 64U                     // 注册URC前缀的个数上限




/*

我建议先按下面这套分配：
任务	建议优先级	分配理由
EG Service	12	及时处理串口接收、解析 AT 回复、分发 URC、唤醒等待者
EG Network	11	处理网络状态变化、连接维护、断线恢复
EG MQTT	10	处理 MQTT 收发事件和连接维护
预留	9	后续有更及时响应需求的通信任务
EG Basic	8	基础信息后台查询，允许一定延迟
EG HTTP	8	普通请求、下载任务，通常允许等待
EG GNSS	8	按普通周期定位需求分配
普通业务任务	3～7	业务控制、数据组织、定期上报等
更严格的实时任务预留	13～15	后续根据实际响应时限使用


这里 Network、MQTT 的先后属于初始分配建议，不是协议本身规定的顺序。例如 GNSS 如果承担严格时限的定位处理，也可以提高。
有几个原则需要把握：
1. Service 优先，是因为其他任务都依赖它完成 AT 交互。
你的 Service 不仅解析数据，还通过 Drv_EG_Read() 在任务上下文中处理 RX DMA 新数据。如果它长期得不到运行，存在 DMA 数据被覆盖的风险。因此，它需要及时运行，并尽快完成这一轮处理后重新阻塞。
Basic、Network 等任务发送 AT 后，会阻塞等待完成信号，不需要一直占着 CPU 等模块返回。
2. 优先级高，任务每次处理的工作量更要受控。
你现在的 URC 路由直接调用 handler(line)。所以即使回调属于 Network 或 MQTT，它仍然运行在 Service 任务的优先级下。
回调里适合做短小的解析、状态记录、投递消息或通知对应任务。耗时业务处理应该交给对应任务；尤其不能在这里调用阻塞等待 Service 完成的 AT 接口，否则会形成自己等待自己的问题。
3. 任务优先级不会改变 AT 命令的排队顺序。
现在 Service 使用普通 FIFO 队列。
即便 Network 是 11、HTTP 是 8，Network 后提交的命令也不会自动插到 HTTP 前面，更不能打断已经执行中的 AT 命令。以后如果要解决“长时间 HTTP 操作影响其他命令”的问题，需要从 AT 请求调度和超时设计处理。
4. System Main 也需要一起考虑。
现在 [System Main 的优先级是 10 (line 27)](C:/My_Project/Crucial_Project/FreeRTOS/AT32F403ARGT7_EG800AK/Treo/System/system_main.c:27)，比当前 Service 高很多。
按它目前主要负责初始化、测试流程和周期日志的用途，建议降到 6。以后实际业务分别放进对应任务，根据响应需求分配优先级，不必让所有业务都继承 Main 的高优先级。
5. 这次调整任务优先级，不需要跟着提高串口和 DMA 中断优先级。
FreeRTOS 任务优先级与 NVIC 中断优先级是两套不同机制。任务数值越大越优先，Cortex-M 中断通常是数值越小越优先；中断调用 FromISR 接口还受到单独的配置约束。FreeRTOS 官方说明
就你当前项目而言，Service 12、Network 11、MQTT 10、Basic/HTTP/GNSS 8、Main 6，可以作为后续扩展的起点。建议把这些任务优先级集中放在一个配置头文件里，便于整体调整。目前代码尚未修改。







*/




#endif // EG_SERVICE_CFG_H_
