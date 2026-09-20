#ifndef EG_BASIC_H_
#define EG_BASIC_H_



#include <stddef.h>
#include "types.h"
#include "eg_service.h"

#define EG800_BASIC_TASK_STACK_SIZE           256U // Basic 后台刷新任务栈大小
#define EG800_BASIC_TASK_PRIORITY             8U   // 低于 EG800 Service 任务优先级






#define EG800_BASIC_IMEI_SIZE                 24U  // IMEI缓冲区
#define EG800_BASIC_IMSI_SIZE                 24U  // IMSI缓冲区
#define EG800_BASIC_ICCID_SIZE                24U  // ICCID缓冲区
#define EG800_BASIC_PHONE_NUMBER_SIZE         32U  // SIM本机号码缓冲区
#define EG800_BASIC_OPERATOR_NAME_SIZE        24U  // 运营商名称缓冲区
#define EG800_BASIC_FIRMWARE_VERSION_SIZE     64U  // 模块固件版本缓冲区

#define EG800_BASIC_REFRESH_INTERVAL_MS       60000U // Basic 信息全量刷新间隔，单位 ms





#define EG800_BASIC_CME_SIM_NOT_INSERTED      10   // +CME ERROR: 10，表示SIM卡未插入

// SIM卡状态
typedef enum {
    EG800_BASIC_SIM_STATE_UNKNOWN = 0,      /* SIM 状态未知 */
    EG800_BASIC_SIM_STATE_NOT_INSERTED,     /* SIM 未插入 */
    EG800_BASIC_SIM_STATE_READY,            /* SIM 已就绪 */
    EG800_BASIC_SIM_STATE_NOT_READY         /* SIM 未就绪 */
} Eg800BasicSimState_e;



// 运营商类型，Network层可根据该类型选择APN
typedef enum {
    EG800_BASIC_OPERATOR_UNKNOWN = 0,        /* 运营商未知 */
    EG800_BASIC_OPERATOR_CHINA_MOBILE,       /* 中国移动 */
    EG800_BASIC_OPERATOR_CHINA_TELECOM,      /* 中国电信 */
    EG800_BASIC_OPERATOR_CHINA_UNICOM        /* 中国联通 */
} Eg800BasicOperator_e;




// EG800基础信息，由Basic层查询并保存
typedef struct {
    Eg800BasicSimState_e sim_state;                             // SIM状态
    uint8_t csq;                                                // 信号强度：0~31，99表示未知
    uint8_t ber;                                                // 误码率：0~7，99表示未知
    char imei[EG800_BASIC_IMEI_SIZE];                           // 模块IMEI
    char imsi[EG800_BASIC_IMSI_SIZE];                           // SIM卡IMSI
    char iccid[EG800_BASIC_ICCID_SIZE];                         // SIM卡ICCID
    char phone_number[EG800_BASIC_PHONE_NUMBER_SIZE];           // SIM本机号码
    Eg800BasicOperator_e operator_type;                         // 运营商类型枚举
    char operator_name[EG800_BASIC_OPERATOR_NAME_SIZE];         // 运营商原始名称
    char firmware_version[EG800_BASIC_FIRMWARE_VERSION_SIZE];   // 模块固件版本
} Eg800BasicInfo_t;


typedef struct {
    Eg800BasicInfo_t info;                                      // 最近一次成功获得的数据
    TickType_t update_tick;                                     // 最近一次全量刷新结束的时刻
    bool refresh_in_progress;                                   // 后台是否正在刷新
} Eg800BasicSnapshot_t;


Result_t Eg800_Basic_Init(void);



//----------------------------------------------------基础命令封装-------------------------------------------------------------------
Eg800AtResult_e Eg800_Basic_Set_Echo_Close(void);

Eg800AtResult_e Eg800_Basic_Set_Urc_Port_Uart1(void);

Eg800AtResult_e Eg800_Basic_Set_Ri_Physical(void);

Eg800AtResult_e Eg800_Basic_Query_Sim(Eg800BasicSimState_e *sim_state);

Eg800AtResult_e Eg800_Basic_Query_Signal(uint8_t *csq, uint8_t *ber);

Eg800AtResult_e Eg800_Basic_Query_Imei(char *imei, size_t imei_size);

Eg800AtResult_e Eg800_Basic_Query_Imsi(char *imsi, size_t imsi_size);

Eg800AtResult_e Eg800_Basic_Query_Iccid(char *iccid, size_t iccid_size);

Eg800AtResult_e Eg800_Basic_Query_Phone_Number(char *phone_number, size_t phone_number_size);

Eg800AtResult_e Eg800_Basic_Query_Firmware_Version(char *version, size_t version_size);

Eg800AtResult_e Eg800_Basic_Query_Operator(Eg800BasicOperator_e *operator_type, char *operator_name, size_t operator_name_size);




//---------------------------------------------------------信息查询获取-------------------------------------------
bool Eg800_Basic_Is_Boot_Ready(void);

void Eg800_Basic_Boot_Ready_Clear(void);

Result_t Eg800_Basic_Get_Snapshot(Eg800BasicSnapshot_t *snapshot);
Result_t Eg800_Basic_Request_Refresh(void);





#endif // EG_BASIC_H_
