#ifndef DRV_EG_H_
#define DRV_EG_H_

#include "types.h"

#include "stdint.h"




void Drv_EG_Init(void);
void Drv_EG_Power_SW(PowerState_t state);
void Drv_EG_Launch_SW(PowerState_t state);
void Drv_EG_DTR_SW(PowerState_t state);
uint8_t Drv_EG_RI_Read(void);

size_t Drv_EG_Read(uint8_t *data, size_t len);




// 发送相关接口
size_t Drv_EG_SendData(const uint8_t *data, size_t len);
size_t Drv_EG_Send(const char *str);



// 中断桥接接口
void Drv_EG_USART3_IRQHandler(void);
void Drv_EG_DMA1_Channel1_IRQHandler(void);
void Drv_EG_DMA1_Channel2_IRQHandler(void);



#endif // DRV_EG_H_


