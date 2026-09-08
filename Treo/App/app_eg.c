#include "app_eg.h"

#include "drv_eg.h"
#include "eg_service.h"
#include "eg800_basic.h"


#include "FreeRTOS.h"
#include "task.h"


#define LOG_TAG "EG"
#include "log.h"

void App_EG_Init(void) {
    Drv_EG_Init();
    Eg800_Basic_Init();
    Eg800_Service_Init();


    
}

void App_EG_Launch(void) {

    Drv_EG_Power_SW(PWR_ON);
    vTaskDelay(1000);

    Drv_EG_Launch_SW(PWR_ON);
    vTaskDelay(1500);
    Drv_EG_Launch_SW(PWR_OFF);
}
