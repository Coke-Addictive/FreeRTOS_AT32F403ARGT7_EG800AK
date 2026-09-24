#ifndef EG_NETWORK_H_
#define EG_NETWORK_H_

#include <stddef.h>
#include "eg_basic.h"



#define EG800_NETWORK_TASK_STACK_SIZE          768U             // Network任务栈深度
#define EG800_NETWORK_TASK_PRIORITY            11U              // 低于EG Service任务优先级


#define EG800_NETWORK_RETRY_MAX                 5U              // 联网失败最大尝试次数
#define EG800_NETWORK_RETRY_INTERVAL_MS         120000U         // 联网失败重试冷却时间 120秒
#define EG800_NETWORK_IP_ADDR_SIZE              48U             // IP地址缓冲区大小，可容纳IPv6地址


// APN 配置情况
#define EG800_NETWORK_CONTEXT_ID                1U              // 当前固定使用的PDP场景ID
#define EG800_NETWORK_CONTEXT_TYPE              1U              // 当前固定使用的PDP类型IPV4


#define EG800_NETWORK_EVENT_CONNECT             (1UL << 0)      // 请求建立连接并维持PDP网络
#define EG800_NETWORK_EVENT_DISCONNECT          (1UL << 1)      // 请求停止重连并断开PDP
#define EG800_NETWORK_EVENT_PDP_DEACT           (1UL << 2)      // 收到PDP异常断开URC



// Network 服务运行状态
typedef enum {
    EG800_NETWORK_STATE_NONE = 0U,                      // 未知PDP状态
    EG800_NETWORK_STATE_IDLE,                           // 已确认 PDP 清理完成，当前未联网
    EG800_NETWORK_STATE_CONNECTING,                     // 正在执行单次联网流程：配置、激活和查询
    EG800_NETWORK_STATE_ACTIVE,                         // 目标 PDP 已激活并获得 IP 地址
    EG800_NETWORK_STATE_DEACTIVATING,                   // 正在去激活 PDP，清理连接资源
    EG800_NETWORK_STATE_RETRY_WAIT,                     // 本次联网失败且已完成清理，等待下一次重试
    EG800_NETWORK_STATE_FAULT                           // 联网尝试耗尽或 PDP 清理失败，停止自动重试
} Eg800NetworkState_e;


// Network层保存的当前PDP网络信息
typedef struct {
    uint8_t pdp_id;                                     // PDP 上下文ID
    uint8_t pdp_type;                                   // PDP 网络类型
    Eg800NetworkState_e state;                          // 当前PDP网络状态
    Eg800BasicOperator_e operator_type;                 // 当前使用的运营商
    char ip_addr[EG800_NETWORK_IP_ADDR_SIZE];           // PDP 激活后的 IP 地址
} Eg800NetworkInfo_t;


// PDP 场景查询结果
typedef struct {
    uint8_t context_id;                                 // PDP 场景 ID
    bool active;                                        // PDP 场景是否已激活
    uint8_t context_type;                               // PDP 协议类型，未获得有效类型时为 0
    char ip_addr[EG800_NETWORK_IP_ADDR_SIZE];           // PDP IP 地址，未获得时为空字符串
} Eg800NetworkPdpContext_t;



typedef struct {
    Eg800NetworkInfo_t info;                            // 当前网络状态及 PDP 信息，IP 仅在 ACTIVE 状态有效
    TickType_t update_tick;                             // 最近一次软件快照更新时刻，单位 tick
    bool refresh_in_progress;                           // 是否正在配置、激活或去激活 PDP
} Eg800NetworkSnapshot_t;





Result_t Eg800_Network_Init(void);


//----------------------------------------------------基础命令封装-------------------------------------------------------------------
Eg800AtResult_e Eg800_Network_Configure_PDP_Context(uint8_t context_id, uint8_t context_type, const char *apn, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Network_Activate_PDP(uint8_t context_id, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Network_Query_PDP_Context(uint8_t context_id, Eg800NetworkPdpContext_t *context, SemaphoreHandle_t completion_sem);

Eg800AtResult_e Eg800_Network_Deactivate_PDP(uint8_t context_id, SemaphoreHandle_t completion_sem);




//----------------------------------------------------命令封装-------------------------

Result_t Eg800_Network_Connect(void);

Result_t Eg800_Network_Disconnect(void);

bool Eg800_Network_Is_Active(void);

Result_t Eg800_Network_Get_Snapshot(Eg800NetworkSnapshot_t *snapshot);

#endif // EG_NETWORK_H_


