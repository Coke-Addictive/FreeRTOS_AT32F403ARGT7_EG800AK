#include "eg_network.h"


#define LOG_TAG "EG_NET"
#include "log.h"


#define EG800_NETWORK_CONFIG_TIMEOUT_MS         5000U       // QICSGP配置超时
#define EG800_NETWORK_ACTIVATE_TIMEOUT_MS       60000U      // QIACT激活超时
#define EG800_NETWORK_QUERY_TIMEOUT_MS          5000U       // QIACT查询超时
#define EG800_NETWORK_DEACTIVATE_TIMEOUT_MS     40000U      // QIDEACT去激活超时

// Network层内部AT命令编号
typedef enum {
    EG800_NETWORK_CMD_QICSGP = 0U,             // 配置PDP场景和APN
    EG800_NETWORK_CMD_QIACT,                   // 激活PDP场景
    EG800_NETWORK_CMD_QIACT_QUERY,             // 查询PDP场景状态和IP
    EG800_NETWORK_CMD_QIDEACT                  // 去激活PDP场景
} Eg800NetworkCmd_e;




// 单个运营商对应的APN配置
typedef struct {
    Eg800BasicOperator_e operator_type;        // 运营商类型
    const char *apn;                           // 运营商对应的APN
    uint8_t auth_type;                         // QICSGP鉴权方式
} Eg800NetworkApnConfig_t;



// 当前支持的运营商APN表
static const Eg800NetworkApnConfig_t s_eg800_network_apn_configs[] = {
    {EG800_BASIC_OPERATOR_CHINA_MOBILE,  "CMNET",  0U},
    {EG800_BASIC_OPERATOR_CHINA_TELECOM, "CTNET",  0U},
    {EG800_BASIC_OPERATOR_CHINA_UNICOM,  "UNINET", 0U}
};




static TaskHandle_t s_eg800_network_task_handle = NULL;             // 任务句柄

static bool s_eg800_network_initialized = false;                    // 初始化标志位

static bool s_eg800_network_should_connect = false;                 // 上层是否希望网络保持连接（目标状态）

static uint8_t s_eg800_network_retry_count = 0U;                    // 当前一轮联网流程已经尝试的次数

static Eg800NetworkInfo_t s_eg800_network_info;                     // Network层保存的当前网络信息






