#ifndef TYPES_H_
#define TYPES_H_



// 通用开关状态
typedef enum {
    SWITCH_OFF = 0, // 关闭
    SWITCH_ON = 1   // 开启
} SwitchState_t;

// 通用供电状态
typedef enum {
    PWR_OFF = 0, // 断电
    PWR_ON = 1   // 上电
} PowerState_t;




// 通用函数执行结果
typedef enum {
    RESULT_SUCCESS = 0, // 执行成功
    RESULT_FAIL = 1     // 执行失败
} Result_t;










#define VALUE_MAX(a, b)     (((a) > (b)) ? (a) : (b))               // 获取两个值中的较大值
#define VALUE_MIN(a, b)     (((a) < (b)) ? (a) : (b))               // 获取两个值中的较小值
#define ARRAY_SIZE(arr)     (sizeof(arr) / sizeof((arr)[0]))        // 获取静态数组元素数量




#endif // TYPES_H_
