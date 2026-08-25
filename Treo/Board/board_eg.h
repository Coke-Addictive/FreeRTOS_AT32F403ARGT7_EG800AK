#ifndef BOARD_EG_H_
#define BOARD_EG_H_

// for GPIO、USART、DMA
#include "at32f403a_407_wk_config.h"
#include "wk_usart.h"
#include "wk_dma.h"


/* EG800AK 4G 模块控制引脚。 */
#define BOARD_EG_LAUNCH_PIN             Eg_Launch_PIN
#define BOARD_EG_LAUNCH_GPIO_PORT       Eg_Launch_GPIO_PORT

#define BOARD_EG_POWER_PIN              Eg_Power_PIN
#define BOARD_EG_POWER_GPIO_PORT        Eg_Power_GPIO_PORT

/* RI 当前暂不使用，WorkBench 未生成专用别名，按硬件文档保留 PB1 资源。 */
#define BOARD_EG_RI_PIN                 GPIO_PINS_1
#define BOARD_EG_RI_GPIO_PORT           GPIOB

#define BOARD_EG_DTR_PIN                Eg_DTR_PIN
#define BOARD_EG_DTR_GPIO_PORT          Eg_DTR_GPIO_PORT


/* EG800AK 串口资源：USART3，115200 8N1。 */
#define BOARD_EG_USART                  USART3
#define BOARD_EG_USART_BAUDRATE         115200U
#define BOARD_EG_USART_DATA_REG_ADDR    ((uint32_t)&BOARD_EG_USART->dt)
#define BOARD_EG_USART_INIT()           wk_usart3_init()


/* EG800AK DMA 资源：DMA1 Channel1 接收，DMA1 Channel2 发送。 */
#define BOARD_EG_RX_DMA_CHANNEL         DMA1_CHANNEL1
#define BOARD_EG_RX_DMA_GL_FLAG         DMA1_GL1_FLAG
#define BOARD_EG_RX_DMA_INIT()          wk_dma1_channel1_init()
#define BOARD_EG_TX_DMA_CHANNEL         DMA1_CHANNEL2
#define BOARD_EG_TX_DMA_GL_FLAG         DMA1_GL2_FLAG
#define BOARD_EG_TX_DMA_INIT()          wk_dma1_channel2_init()





#endif // BOARD_EG_H_
