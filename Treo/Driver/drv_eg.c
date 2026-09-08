// RX DMA 采用循环模式，USART IDLE 与 RX DMA HDT/FDT 中断仅置位接收检查请求。
// Drv_EG_Read() 在任务上下文中读取 DMA 当前写入位置，并将新增数据搬运到 RX 软件环形缓冲区。
// TX 由任务写入软件环形缓冲区，DMA FDT 中断释放已发送区段并启动下一段发送。
#include "drv_eg.h"

#include "board_eg.h"           // EG800AK 板级引脚和外设资源

#include "lwrb.h"               // 软件环形缓冲区实现

#define LOG_TAG "DRV_EG"        // 驱动日志模块标签
#include "log.h"



#define EG_RX_DMA_BUF_SIZE          2048U // RX DMA 硬件循环缓冲区大小，单位字节

#define EG_RX_RING_BUF_SIZE         4096U // RX 软件环形缓冲区大小，单位字节
#define EG_TX_RING_BUF_SIZE         2048U // TX 软件环形缓冲区大小，单位字节



static uint8_t s_eg_rx_dma_buf[EG_RX_DMA_BUF_SIZE];       // RX DMA 循环缓冲区，由 DMA1 Channel1 连续写入
static uint8_t s_eg_rx_ring_buf[EG_RX_RING_BUF_SIZE];     // RX 软件环形缓冲区数据区，由任务上下文写入
static size_t s_eg_rx_dma_old_pos = 0;                    // RX DMA 上一次处理位置，用于计算新增数据长度
static lwrb_t s_eg_rx_ring;                               // RX 软件环形缓冲区控制块



static uint8_t s_eg_tx_ring_buf[EG_TX_RING_BUF_SIZE];     // TX 软件环形缓冲区数据区，由任务写入、DMA1 Channel2 读取
static volatile size_t s_eg_tx_dma_current_len = 0;       // 当前 DMA 发送长度，0 表示 TX DMA 空闲
static lwrb_t s_eg_tx_ring;                               // TX 软件环形缓冲区控制块


// RX 检查请求标志，由 USART IDLE、RX DMA HDT/FDT 中断置位，并由任务上下文取走。多次中断会合并为一次检查请求；该标志不记录中断次数。
static volatile bool s_eg_rx_check_pending = false; 


static volatile Drv_EG_Error_t s_eg_error;                // 驱动内部故障统计，复位后清零，当前未提供读取接口


/**
 * @brief 配置 EG800AK 的 Launch、Power、DTR 输出引脚和 RI 输入引脚。
 * @note 输出引脚在切换为输出模式前先写入低电平，避免初始化期间出现高电平毛刺。
 */
static void EG_GPIO_Init(void) {
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);

    // 初始化EG_Launch,默认输出低电平
    gpio_bits_reset(BOARD_EG_LAUNCH_GPIO_PORT, BOARD_EG_LAUNCH_PIN);
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;                       // 输出模式
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;              // 推挽输出
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE; // 中等驱动能力
    gpio_init_struct.gpio_pins = BOARD_EG_LAUNCH_PIN;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE; // 无上下拉
    gpio_init(BOARD_EG_LAUNCH_GPIO_PORT, &gpio_init_struct);

    // 初始化EG_Power,默认输出低电平
    gpio_bits_reset(BOARD_EG_POWER_GPIO_PORT, BOARD_EG_POWER_PIN);
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;                       // 输出模式
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;              // 推挽输出
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE; // 中等驱动能力
    gpio_init_struct.gpio_pins = BOARD_EG_POWER_PIN;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE; // 无上下拉
    gpio_init(BOARD_EG_POWER_GPIO_PORT, &gpio_init_struct);

    // 初始化EG_DTR,默认输出低电平
    gpio_bits_reset(BOARD_EG_DTR_GPIO_PORT, BOARD_EG_DTR_PIN);
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;                       // 输出模式
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;              // 推挽输出
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE; // 中等驱动能力
    gpio_init_struct.gpio_pins = BOARD_EG_DTR_PIN;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE; // 无上下拉
    gpio_init(BOARD_EG_DTR_GPIO_PORT, &gpio_init_struct);

    // 初始化EG_RI,默认输入----4G模块唤醒MCU
    gpio_init_struct.gpio_mode = GPIO_MODE_INPUT; // 输入模式
    gpio_init_struct.gpio_pull = GPIO_PULL_DOWN;  // 下拉
    gpio_init_struct.gpio_pins = BOARD_EG_RI_PIN; // 选择引脚
    gpio_init(BOARD_EG_RI_GPIO_PORT, &gpio_init_struct);
}


/**
 * @brief 初始化 EG800AK 使用的 USART3 及其 DMA 收发请求和中断。
 */
static void EG_USART_Init(void) {
    BOARD_EG_USART_INIT();
}



/**
 * @brief 初始化 RX 循环 DMA 和 TX 单次传输 DMA 通道。
 * @note RX DMA 初始化后立即启动，TX DMA 保持关闭并在有数据待发送时启动。
 */
static void EG_DMA_Init(void) {
    // DMA1 Channel1 用于 USART2 RX，循环模式，传输完成/半传输完成/传输错误中断
    BOARD_EG_RX_DMA_INIT();
    wk_dma_channel_config(BOARD_EG_RX_DMA_CHANNEL, BOARD_EG_USART_DATA_REG_ADDR, (uint32_t)s_eg_rx_dma_buf, EG_RX_DMA_BUF_SIZE);
    dma_flag_clear(BOARD_EG_RX_DMA_GL_FLAG); // 清除可能的旧标志
    dma_channel_enable(BOARD_EG_RX_DMA_CHANNEL, TRUE);

    // DMA1 Channel2 用于 USART2 TX，正常模式，传输完成/传输错误中断
    BOARD_EG_TX_DMA_INIT();
    dma_flag_clear(BOARD_EG_TX_DMA_GL_FLAG);
    // wk_dma_channel_config tx的配置在发送数据时动态配置，默认先不启动
    dma_channel_enable(BOARD_EG_TX_DMA_CHANNEL, FALSE);
}

/**
 * @brief 初始化 RX/TX 软件环形缓冲区及其 DMA 处理状态。
 */
static void EG_LWRB_Init(void) {
    // 初始化 RX 软件环形缓冲区
    lwrb_init(&s_eg_rx_ring, s_eg_rx_ring_buf, sizeof(s_eg_rx_ring_buf));
    s_eg_rx_dma_old_pos = 0;
    s_eg_rx_check_pending = false;

    // 初始化 TX 软件环形缓冲区
    lwrb_init(&s_eg_tx_ring, s_eg_tx_ring_buf, sizeof(s_eg_tx_ring_buf));
    s_eg_tx_dma_current_len = 0;
}



/**
 * @brief 将 RX DMA 新增数据写入 RX 软件环形缓冲区，并统计溢出和丢弃字节数。
 * @param data 待写入数据地址，仅在本次调用期间访问。
 * @param len 待写入数据长度，单位字节。
 */
static void EG_RX_RingWrite(const uint8_t *data, size_t len) {
    size_t written;

    if ((data == NULL) || (len == 0U)) {
        return;
    }

    written = lwrb_write(&s_eg_rx_ring, data, len);

    if (written != len) {
        s_eg_error.rx_ring_overflow_count++;                        // RX 环形缓冲区溢出次数+1
        s_eg_error.rx_dropped_bytes += (uint32_t)(len-written);     // RX 掉落的字节数累计
    }
}



/**
 * @brief 根据 RX DMA 当前写入位置计算新增数据，并搬运到 RX 软件环形缓冲区。
 * @note 本函数必须在任务上下文中调用。
 * @note 必须在 DMA 覆盖一整圈数据前完成处理；位置差值无法识别已经被覆盖的整圈数据。
 */
static void EG_USART_RxCheck(void) {
    size_t pos;         // DMA 当前写指针指向位置
    size_t data_len;

    pos = ARRAY_SIZE(s_eg_rx_dma_buf) - dma_data_number_get(BOARD_EG_RX_DMA_CHANNEL); // 当前DMA写入位置

    if (pos > ARRAY_SIZE(s_eg_rx_dma_buf)) {
        s_eg_error.rx_dma_error_count++;            //RX DMA 错误计数+1
        LOG_ERROR("EG RX DMA position error");
        NVIC_SystemReset();
    }

    if (pos == s_eg_rx_dma_old_pos) {
        // 没有新数据
        return;
    }

    if (pos > s_eg_rx_dma_old_pos) {
        /* DMA未回卷，新增数据连续 */
        data_len = pos - s_eg_rx_dma_old_pos;                             // 新数据长度
        EG_RX_RingWrite(&s_eg_rx_dma_buf[s_eg_rx_dma_old_pos], data_len); // 写入环形缓冲区

    } else {
        /* DMA已回卷，新增数据分为 尾部+头部 两段数据 */
        data_len = ARRAY_SIZE(s_eg_rx_dma_buf) - s_eg_rx_dma_old_pos;     // 尾部新数据长度
        EG_RX_RingWrite(&s_eg_rx_dma_buf[s_eg_rx_dma_old_pos], data_len); // 写入环形缓冲区
        if (pos > 0U) {
            // 头部新数据长度
            EG_RX_RingWrite(&s_eg_rx_dma_buf[0], pos);
        }
    }

    // 更新DMA写入位置
    s_eg_rx_dma_old_pos = pos;
}

 


/**
 * @brief 标记 RX DMA 写入位置需要在任务上下文中检查。
 * @note 应在 USART IDLE、RX DMA HDT 和 RX DMA FDT 中断中调用。
 * @note 多次调用只保留一个待处理标志，不累计中断次数。
 */
static void EG_USART_RxSchedule(void) {
    s_eg_rx_check_pending = true;
}






/**
 * @brief 原子地取走 RX 检查请求，并在存在请求时处理 RX DMA 新增数据。
 * @note 当前仅由 Drv_EG_Read() 在任务上下文中调用。
 */
static void EG_USART_RxProcess(void) {
    uint32_t primask;
    bool pending;

    // 进入临界区保护（因为中断中也操作了该标志位）
    primask = __get_PRIMASK();
    __disable_irq();

    pending = s_eg_rx_check_pending;        // 取走请求
    s_eg_rx_check_pending = false;          // 清除标志位

    // 退出临界区保护
    __set_PRIMASK(primask);

    if (pending == true) {
        EG_USART_RxCheck();
    }
}




/**
 * @brief 在 TX DMA 空闲时取得环形缓冲区的连续数据块并启动一次 DMA 发送。
 * @return 1 表示启动了新的 DMA 发送，0 表示 DMA 忙或没有待发送数据。
 * @note 本函数可从任务或 TX DMA 中断调用，通过保存和恢复 PRIMASK 保护共享状态。
 */
static uint8_t EG_USART_TxDmaStart(void) {
    uint32_t primask;
    void *send_addr;
    uint8_t started = 0U;

    // 进入临界区保护
    primask = __get_PRIMASK();
    __disable_irq();

    // 当前DMA空闲，并且 TX ring 中有一段连续可发送数据，准备启动新的 DMA 发送
    if ((s_eg_tx_dma_current_len == 0U) && ((s_eg_tx_dma_current_len = lwrb_get_linear_block_read_length(&s_eg_tx_ring)) > 0U)) {
        send_addr = lwrb_get_linear_block_read_address(&s_eg_tx_ring);
        dma_channel_enable(BOARD_EG_TX_DMA_CHANNEL, FALSE); // 先关闭DMA通道
        dma_flag_clear(BOARD_EG_TX_DMA_GL_FLAG);            // 清除可能存在的标志位

        // 重新配置并启动一次定长的DMA发送
        wk_dma_channel_config(BOARD_EG_TX_DMA_CHANNEL, BOARD_EG_USART_DATA_REG_ADDR, (uint32_t)send_addr, (uint16_t)s_eg_tx_dma_current_len);

        dma_channel_enable(BOARD_EG_TX_DMA_CHANNEL, TRUE);
        started = 1U;
    }

    __set_PRIMASK(primask);
    // 退出临界区保护

    return started;
}











//--------------------------------------------------公开API---------------------------------------------------------
//--------------------------------------------------公开API---------------------------------------------------------
//--------------------------------------------------公开API---------------------------------------------------------

/**
 * @brief 读取 EG 接收环形缓冲区中的数据。
 * @param data 接收数据存放地址，至少可容纳 len 个字节。
 * @param len 期望读取的最大长度，单位字节。
 * @return 实际读取字节数；参数无效或当前无数据时返回 0。
 * @note USART 和 RX DMA 中断只置位检查请求，调用方必须周期调用本接口完成数据搬运。
 */
size_t Drv_EG_Read(uint8_t *data, size_t len) {
    if ((data == NULL) || (len == 0U)) {
        return 0U;
    }
    EG_USART_RxProcess();
    return lwrb_read(&s_eg_rx_ring, data, len);
}


/**
 * @brief 将 EG 串口原始数据写入 TX ring，并尝试启动 DMA 发送。
 * @param data 待发送数据地址，仅在本次调用期间访问。
 * @param len 待发送数据长度，单位字节。
 * @return 成功时返回 len；参数无效或 TX ring 空间不足时返回 0。
 * @note 仅允许一个任务调用，驱动内部未提供多任务互斥保护。
 */
size_t Drv_EG_SendData(const uint8_t *data, size_t len) {
    size_t write_len;

    if ((data == NULL) || (len == 0U)) {
        return 0U;
    }

    
    // 检查 TX ring 空间是否足够
    if (lwrb_get_free(&s_eg_tx_ring) >= len) {
        write_len = lwrb_write(&s_eg_tx_ring, data, len);
        (void)EG_USART_TxDmaStart();        
        
    } else {
        LOG_ERROR("EG TX 发送缓冲区剩余空间不足, 丢弃本次内容 %lu 字节", (unsigned long)len);
        s_eg_error.tx_ring_overflow_count++;            // TX 环形缓冲区溢出次数+1      
        return 0U;
    }

    return write_len;
}




/**
 * @brief 将字符串写入 TX ring，并尝试启动 DMA 发送。
 * @param str 待发送的空字符结尾字符串地址。
 * @return 成功时返回字符串长度；参数无效或 TX ring 空间不足时返回 0。
 * @note 本接口面向 AT 字符串，不适合发送包含 '\0' 的二进制数据。
 */
size_t Drv_EG_Send(const char *str) {
    if (str == NULL) {
        return 0U;
    }

    return Drv_EG_SendData((const uint8_t *)str, strlen(str));
}



/**
 * @brief 设置 EG800AK 电源控制引脚电平。
 * @param state PWR_ON 输出高电平，PWR_OFF 输出低电平。
 */
void Drv_EG_Power_SW(PowerState_t state) {
    gpio_bits_write(BOARD_EG_POWER_GPIO_PORT, BOARD_EG_POWER_PIN, (state == PWR_ON) ? TRUE : FALSE);
}



/**
 * @brief 设置 EG800AK 启动控制引脚电平。
 * @param state PWR_ON 输出高电平，PWR_OFF 输出低电平。
 */
void Drv_EG_Launch_SW(PowerState_t state) {
    gpio_bits_write(BOARD_EG_LAUNCH_GPIO_PORT, BOARD_EG_LAUNCH_PIN, (state == PWR_ON) ? TRUE : FALSE);
}



/**
 * @brief 设置 EG800AK DTR 控制引脚电平。
 * @param state PWR_ON 输出高电平，PWR_OFF 输出低电平。
 */
void Drv_EG_DTR_SW(PowerState_t state) {
    gpio_bits_write(BOARD_EG_DTR_GPIO_PORT, BOARD_EG_DTR_PIN, (state == PWR_ON) ? TRUE : FALSE);
}



/**
 * @brief 读取 EG800AK RI 引脚的原始输入电平。
 * @return 0 表示低电平，非 0 表示高电平。
 */
uint8_t Drv_EG_RI_Read(void) {
    return gpio_input_data_bit_read(BOARD_EG_RI_GPIO_PORT, BOARD_EG_RI_PIN);
}



/**
 * @brief 初始化 EG800AK 控制 GPIO、软件环形缓冲区、DMA 和 USART。
 * @note 先初始化软件缓冲区和 DMA，再使能 USART，避免接收器启动时数据无处存放。
 */
void Drv_EG_Init(void) {
    // 配置GPIO
    EG_GPIO_Init();
    Drv_EG_Power_SW(PWR_OFF);
    Drv_EG_Launch_SW(PWR_OFF);

    // 环形缓冲器初始化（在DMA和USART之前）
    EG_LWRB_Init();

    // DMA(在前)
    EG_DMA_Init();

    // USART(在后)
    EG_USART_Init();
}





//--------------------------------------------------中断---------------------------------------------------------
//--------------------------------------------------中断---------------------------------------------------------
//--------------------------------------------------中断---------------------------------------------------------
/**
 * @brief 处理 EG800AK USART IDLE 事件，标记 RX DMA 数据需要在任务上下文中检查。
 * @note 由 USART3 中断入口清除 IDLE 标志后调用，本函数不直接搬运数据。
 */
void Drv_EG_USART_IRQHandler(void) {
    EG_USART_RxSchedule();
}

/**
 * @brief 处理 EG800AK RX DMA 半传输或全传输事件，标记接收数据需要检查。
 * @note 由 DMA1 Channel1 中断入口清除 HDT/FDT 标志后调用，本函数不直接搬运数据。
 */
void Drv_EG_RX_DMA_Channel_IRQHandler(void) {
    EG_USART_RxSchedule();
}


/**
 * @brief 处理 EG800AK TX DMA 全传输完成事件，并继续发送环形缓冲区中的后续数据。
 * @note 由 DMA1 Channel2 中断入口清除 FDT 标志后调用。
 */
void Drv_EG_TX_DMA_Channel_IRQHandler(void) {
    // 刚才 DMA 已经发完 current_len 字节了，把这些字节从 TX ring 里移除
    if (s_eg_tx_dma_current_len > 0U) {
        lwrb_skip(&s_eg_tx_ring, s_eg_tx_dma_current_len);
    }


    // 清零当前 DMA 发送长度，表示 DMA 空闲
    s_eg_tx_dma_current_len = 0U;
    // 尝试启动下一段 DMA 发送任务（如果 TX ring 里还有数据）
    (void)EG_USART_TxDmaStart();
}





// 现有 LOG 机制可能在立即复位前来不及输出，因此错误中断暂时使用 printf 输出简短诊断信息。



/**
 * @brief 处理 RX DMA 传输错误，输出诊断信息后立即复位 MCU。
 */
void Drv_EG_RX_DMA_Error_IRQHandler(void) {
    printf("EG RX DMA error\r\n");
    NVIC_SystemReset();
}

/**
 * @brief 处理 TX DMA 传输错误，输出诊断信息后立即复位 MCU。
 */
void Drv_EG_TX_DMA_Error_IRQHandler(void) {
    printf("EG TX DMA error\r\n");
    NVIC_SystemReset();
}

/**
 * @brief 处理 USART 接收错误，输出诊断信息后立即复位 MCU。
 */
void Drv_EG_USART_Error_IRQHandler(void) {
    printf("EG USART error\r\n");
    NVIC_SystemReset();
}



