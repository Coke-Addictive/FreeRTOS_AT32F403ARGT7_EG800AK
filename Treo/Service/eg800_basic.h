#ifndef EG800_BASIC_H_
#define EG800_BASIC_H_


#include <stdint.h>
#include "types.h"


#define EG800_BASIC_IMEI_SIZE                 24U  // IMEI缓冲区
#define EG800_BASIC_IMSI_SIZE                 24U  // IMSI缓冲区
#define EG800_BASIC_ICCID_SIZE                24U  // ICCID缓冲区
#define EG800_BASIC_PHONE_NUMBER_SIZE         32U  // SIM本机号码缓冲区
#define EG800_BASIC_OPERATOR_NAME_SIZE        24U  // 运营商名称缓冲区
#define EG800_BASIC_FIRMWARE_VERSION_SIZE     64U  // 模块固件版本缓冲区



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





Result_t Eg800_Basic_Init(void);








#endif // EG800_BASIC_H_
