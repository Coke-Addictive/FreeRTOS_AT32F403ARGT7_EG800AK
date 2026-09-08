#ifndef DRV_EG_H_
#define DRV_EG_H_

#include "types.h"

#include "stdint.h"
#include "stddef.h"     // for size_t
#include "stdbool.h"


// 通信故障统计
typedef struct {
    uint32_t rx_ring_overflow_count;            // RX 软件环形缓冲区溢出的次数
    uint32_t rx_dropped_bytes;                  // 因 RX 环形缓冲区满而丢弃的总字节数
    uint32_t tx_ring_overflow_count;            // TX 环形缓冲区溢出次数
    uint32_t rx_dma_error_count;                // RX DMA 位置异常次数，硬件传输错误当前不累计
    uint32_t tx_dma_error_count;                // 预留的 TX DMA 错误计数，当前未累计
    uint32_t usart_error_count;                 // 预留的 USART 错误计数，当前未累计
} Drv_EG_Error_t;



// 初始化和模块引脚控制接口
// 使用其他接口前必须先调用 Drv_EG_Init()；PWR_ON/PWR_OFF 分别输出高/低电平。
void Drv_EG_Init(void);
void Drv_EG_Power_SW(PowerState_t state);
void Drv_EG_Launch_SW(PowerState_t state);
void Drv_EG_DTR_SW(PowerState_t state);
uint8_t Drv_EG_RI_Read(void);



// 数据接收接口；调用时在任务上下文中处理 RX DMA 新增数据。
size_t Drv_EG_Read(uint8_t *data, size_t len);



// 数据发送接口；仅允许一个任务调用，驱动内部未提供多任务互斥。
size_t Drv_EG_SendData(const uint8_t *data, size_t len);
size_t Drv_EG_Send(const char *str);



// 中断桥接接口；由芯片中断入口在清除对应硬件标志后调用。
void Drv_EG_USART_IRQHandler(void);
void Drv_EG_RX_DMA_Channel_IRQHandler(void);
void Drv_EG_TX_DMA_Channel_IRQHandler(void);



// 致命错误处理接口；输出故障信息后立即复位 MCU。
void Drv_EG_RX_DMA_Error_IRQHandler(void);
void Drv_EG_TX_DMA_Error_IRQHandler(void);
void Drv_EG_USART_Error_IRQHandler(void);

#endif // DRV_EG_H_





