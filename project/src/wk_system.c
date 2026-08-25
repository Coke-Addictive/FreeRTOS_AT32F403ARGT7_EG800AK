/* add user code begin Header */
/**
  **************************************************************************
  * @file     wk_system.c
  * @brief    work bench config program
  **************************************************************************
  * Copyright (c) 2025, Artery Technology, All rights reserved.
  *
  * The software Board Support Package (BSP) that is made available to
  * download from Artery official website is the copyrighted work of Artery.
  * Artery authorizes customers to use, copy, and distribute the BSP
  * software and its related documentation for the purpose of design and
  * development in conjunction with Artery microcontrollers. Use of the
  * software is governed by this copyright notice and the following disclaimer.
  *
  * THIS SOFTWARE IS PROVIDED ON "AS IS" BASIS WITHOUT WARRANTIES,
  * GUARANTEES OR REPRESENTATIONS OF ANY KIND. ARTERY EXPRESSLY DISCLAIMS,
  * TO THE FULLEST EXTENT PERMITTED BY LAW, ALL EXPRESS, IMPLIED OR
  * STATUTORY OR OTHER WARRANTIES, GUARANTEES OR REPRESENTATIONS,
  * INCLUDING BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
  *
  **************************************************************************
  */
/* add user code end Header */

#include "wk_system.h"

#define STEP_DELAY_MS                    (uint32_t)(50)
#define TICK_COUNT_MAX                   (uint32_t)(0xFFFF)
#define TICK_COUNT_VALUE                 (TMR6->cval)

/* global variable */
volatile uint32_t ticks_count_us;

/**
  * @brief  this function provides minimum delay (in microsecond).
  * @param  delay: specifies the delay time length, in microsecond.
  * @retval none
  */
__WEAK void wk_delay_us(uint32_t delay)
{
  uint32_t delay_ticks, pre_ticks, cur_ticks, delta;
  delay_ticks = delay * ticks_count_us;

  pre_ticks = TICK_COUNT_VALUE;
  do
  {
    cur_ticks = TICK_COUNT_VALUE;
    /* count up */
    delta = (cur_ticks >= pre_ticks) ? (cur_ticks - pre_ticks) : ((TICK_COUNT_MAX - pre_ticks) + cur_ticks + 1);
  } while(delta < delay_ticks);
}

/**
  * @brief  this function provides minimum delay (in milliseconds).
  * @param  delay variable specifies the delay time length, in milliseconds.
  * @retval none
  */
__WEAK void wk_delay_ms(uint32_t delay)
{
  while(delay)
  {
    if(delay > STEP_DELAY_MS)
    {
      wk_delay_us(STEP_DELAY_MS * 1000);
      delay -= STEP_DELAY_MS;
    }
    else
    {
      wk_delay_us(delay * 1000);
      delay = 0;
    }
  }
}

/**
  * @brief  this function configures the source of the time base.
  * @param  none
  * @retval none
  */
__WEAK void wk_timebase_init(void)
{
  crm_clocks_freq_type crm_clocks;
  uint32_t frequency = 0;

  /* get crm_clocks */
  crm_clocks_freq_get(&crm_clocks);

  frequency = crm_clocks.apb1_freq * 2;
  /* time base configuration */
  tmr_base_init(TMR6, TICK_COUNT_MAX, ((frequency / 1000000U) - 1));
  ticks_count_us = 1U;
  /* config timer count up */
  tmr_cnt_dir_set(TMR6, TMR_COUNT_UP);
  /* enable timer */
  tmr_counter_enable(TMR6, TRUE);
}

/* support printf function, usemicrolib is unnecessary */
#if (__ARMCC_VERSION >= 6010050)
  int _sys_open(const char *path, int mode)
  {
    UNUSED(path);
    UNUSED(mode);
    return 0;  
  }
  int _sys_close(int fd)
  {
    UNUSED(fd);
    return 0;
  }
  int _sys_write(int fd, const unsigned char *buf,
                 unsigned len, int mode)
  {
    UNUSED(fd);
    UNUSED(buf);
    UNUSED(len);
    UNUSED(mode);
    return 0;   
  }
  int _sys_read(int fd, unsigned char *buf,
                unsigned len, int mode)
  {
    UNUSED(fd);
    UNUSED(buf);
    UNUSED(len);
    UNUSED(mode);
    return 0;       
  }
  int _sys_istty(int fd)
  {
    UNUSED(fd);
    return 1;
  }
  int _sys_seek(int fd, long pos)
  {
    UNUSED(fd);
    UNUSED(pos);
    return -1;
  }
  int _sys_ensure(int fd)
  {
    UNUSED(fd);
    return 0;
  }
  long _sys_flen(int fd)
  {
    UNUSED(fd);
    return 0;
  }
#else
  #ifdef __CC_ARM
  #pragma import(__use_no_semihosting)
  struct __FILE
  {
    int handle;
  };
  FILE __stdout;
  #endif
#endif

void _sys_exit(int fd)
{
  UNUSED(fd);
}
void _ttywrch(int ch)
{
  UNUSED(ch);
}

#if defined (__GNUC__) && !defined (__clang__)
  #define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
  #define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif

/**
  * @brief  retargets the c library printf function to the usart.
  * @param  none
  * @retval none
  */
PUTCHAR_PROTOTYPE
{
#if !defined (__GNUC__) || defined (__clang__)
  UNUSED(f);
#endif
  while(usart_flag_get(UART4, USART_TDBE_FLAG) == RESET);
  usart_data_transmit(UART4, (uint16_t)ch);
  while(usart_flag_get(UART4, USART_TDC_FLAG) == RESET);
  return ch;
}

#if (defined (__GNUC__) && !defined (__clang__)) || (defined (__ICCARM__))
#if defined (__GNUC__) && !defined (__clang__)
int _write(int fd, char *pbuffer, int size)
#elif defined ( __ICCARM__ )
#pragma module_name = "?__write"
int __write(int fd, char *pbuffer, int size)
#endif
{
  UNUSED(fd);
  for(int i = 0; i < size; i ++)
  {
    while(usart_flag_get(UART4, USART_TDBE_FLAG) == RESET);
    usart_data_transmit(UART4, (uint16_t)(*pbuffer++));
    while(usart_flag_get(UART4, USART_TDC_FLAG) == RESET);
  }

  return size;
}
#endif

