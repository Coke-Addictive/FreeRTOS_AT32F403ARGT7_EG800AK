#ifndef EG_NETWORK_H_
#define EG_NETWORK_H_

#include <stddef.h>
#include "eg_basic.h"



#define EG800_NETWORK_TASK_STACK_SIZE          256U             // Network任务栈深度
#define EG800_NETWORK_TASK_PRIORITY            11U              // 低于EG Service任务优先级


#define EG800_NETWORK_CONTEXT_ID                1U              // 当前固定使用的PDP场景ID
#define EG800_NETWORK_RETRY_MAX                 5U              // 联网失败最大尝试次数
#define EG800_NETWORK_RETRY_INTERVAL_MS         120000U         // 联网失败重试冷却时间 120秒
#define EG800_NETWORK_IP_ADDR_SIZE              48U             // IP地址缓冲区大小，可容纳IPv6地址




// EG800 PDP网络状态
typedef enum {
    EG800_NETWORK_STATE_IDLE = 0,             // 网络未激活
    EG800_NETWORK_STATE_CONFIGURING,          // 正在配置PDP场景和APN
    EG800_NETWORK_STATE_ACTIVATING,           // 正在激活PDP场景
    EG800_NETWORK_STATE_ACTIVE,               // PDP场景已激活并获得IP地址
    EG800_NETWORK_STATE_RETRY_WAIT,           // 联网失败，正在等待下一次自动重试
    EG800_NETWORK_STATE_DEACTIVATING,         // 正在去激活PDP场景
    EG800_NETWORK_STATE_FAULT                 // 网络激活失败，重试次数耗尽
} Eg800NetworkState_e;


// Network层保存的当前PDP网络信息
typedef struct {
    uint8_t PDP_ID;                                     // PDP 上下文ID
    Eg800NetworkState_e state;                          // 当前PDP网络状态
    Eg800BasicOperator_e operator_type;                 // 当前使用的运营商
    char ip_addr[EG800_NETWORK_IP_ADDR_SIZE];           // PDP 激活后的 IP 地址
} Eg800NetworkInfo_t;



typedef struct {
    Eg800NetworkInfo_t info;                            // 最近一次成功获得的数据
    TickType_t update_tick;                             // 最近一次全量刷新结束的时刻
    bool refresh_in_progress;                           // 后台是否正在刷新
} Eg800NetworkSnapshot_t;





Result_t Eg800_Network_Init(void);






Result_t Eg800_Network_Get_Info(Eg800NetworkInfo_t *info);





#endif // EG_NETWORK_H_
