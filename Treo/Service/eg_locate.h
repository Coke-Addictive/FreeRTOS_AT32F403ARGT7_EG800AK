#ifndef EG_LOCATE_H_
#define EG_LOCATE_H_

#include "stdbool.h"
#include "types.h"
#include "eg_service.h"

#define EG800_GNSS_TASK_STACK_SIZE              384U    // Basic 后台刷新任务栈大小
#define EG800_GNSS_TASK_PRIORITY                7U      // 低于 EG800 Service 任务优先级

#define EG800_LOCATE_GNSS_QUERY_MODE            2U      // 本查询接口固定使用模式 2，输出十进制度坐标 （暂时没启用，先固定写死了）

// 定位默认开关，统一通过 Eg800_Locate_Read_Config() 读取；true 开启，false 关闭。
#define EG800_LOCATE_DEFAULT_ENABLED          true      // 定位系统默认总开关
#define EG800_LOCATE_BDS_DEFAULT_ENABLED      true      // 北斗定位默认开关
#define EG800_LOCATE_WIFI_DEFAULT_ENABLED     true      // Wi-Fi 定位默认开关
#define EG800_LOCATE_LBS_DEFAULT_ENABLED      true      // 基站定位默认开关

// 定位结果来源
typedef enum {
    LOCATE_SOURCE_NONE = 0U,                            // 无定位来源，或当前未执行定位
    LOCATE_SOURCE_BDS,                                  // 北斗卫星定位
    LOCATE_SOURCE_WIFI,                                 // Wi-Fi 辅助定位
    LOCATE_SOURCE_LBS,                                  // 基站辅助定位
    LOCATE_SOURCE_COUNT                                 // 来源枚举上限，不作为实际来源
} Eg800LocationSource_e;

// 定位开关读取结果；实际允许使用某种方式时，总开关和对应子开关必须同时开启。
typedef struct {
    bool locate_sw;                                     // 定位系统总开关
    bool bds_sw;                                        // 北斗定位独立开关
    bool wifi_sw;                                       // Wi-Fi 定位独立开关
    bool lbs_sw;                                        // 基站定位独立开关
} Eg800LocateConfig_t;

// 定位服务运行阶段，与配置开关和定位结果是否有效分别维护。
typedef enum {
    EG800_LOCATE_STATE_IDLE = 0U,                       // 当前空闲，没有正在执行的定位或收尾操作
    EG800_LOCATE_STATE_LOCATING,                        // 正在启动、等待或查询本轮定位
    EG800_LOCATE_STATE_STOPPING,                        // 正在停止本轮定位并清理相关资源
    EG800_LOCATE_STATE_FAULT                            // 故障
} Eg800LocateState_e;




// 定位记录的真实时间
typedef struct {
    uint16_t year;                                      // 年
    uint8_t month;                                      // 月
    uint8_t day;                                        // 日
    uint8_t hour;                                       // 时
    uint8_t minute;                                     // 分
} Eg800LocationTime_t;




// 单种定位方式的结果
typedef struct {
    bool is_valid;                                              // 成功/失败
    double longitude;                                           // 经度，单位,十进制度
    double latitude;                                            // 纬度，单位,十进制度
    double altitude;                                            // 海拔，单位,m
    int32_t error_code;                                         // 失败错误码(负数)
    Eg800LocationSource_e location_source;                      // 定位来源：北斗、Wi-Fi、基站
    Eg800LocationTime_t success_time;                           // 定位的真实时间，尚未记录时全为 0
} Eg800LocationInfo_t;

// 定位结果更新字段，可按位或组合；未选中的成员保持原值。
#define EG800_LOCATE_UPDATE_IS_VALID           (1UL << 0)       // 更新本次定位是否成功
#define EG800_LOCATE_UPDATE_LONGITUDE          (1UL << 1)       // 更新经度
#define EG800_LOCATE_UPDATE_LATITUDE           (1UL << 2)       // 更新纬度
#define EG800_LOCATE_UPDATE_ALTITUDE           (1UL << 3)       // 更新海拔
#define EG800_LOCATE_UPDATE_ERROR_CODE         (1UL << 4)       // 更新错误码，允许写入 0
#define EG800_LOCATE_UPDATE_LOCATION_SOURCE    (1UL << 5)       // 更新定位来源
#define EG800_LOCATE_UPDATE_SUCCESS_TIME       (1UL << 6)       // 更新完整的成功时间成员
#define EG800_LOCATE_UPDATE_ALL                0x7FUL           // 更新以上全部 7 个成员

// 定位服务快照
typedef struct {
    bool refresh_in_progress;                           // 后台是否正在进行定位或收尾操作
    Eg800LocationInfo_t bds_info;                       // 北斗-定位信息
    Eg800LocationInfo_t wifi_info;                      // WIFI-定位信息
    Eg800LocationInfo_t lbs_info;                       // 基站-定位信息
} Eg800LocationSnapshot_t;











//----------------------------------------------------基础命令封装-------------------------------------------------------------------

Eg800AtResult_e Eg800_Locate_Gnss_Set_Enabled(bool enabled, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Locate_Gnss_Query(double *longitude, double *latitude, double *altitude, SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Locate_Gnss_Stop(SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Locate_WifiScan(SemaphoreHandle_t completion_sem);
Eg800AtResult_e Eg800_Locate_Lbs(SemaphoreHandle_t completion_sem);


//----------------------------------------------------通用API接口-------------------------------------------------------------------


// 初始化接口
Result_t Eg800_Locate_Init(void);


void Eg800_Locate_Config_SW_Update(bool locate_sw, bool bds_sw, bool wifi_sw, bool lbs_sw);


void Eg800_Locate_Test(void);


#endif // EG_LOCATE_H_

