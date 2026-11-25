#ifndef SYS_VERSION_H_
#define SYS_VERSION_H_


#define PROJECT_NAME "EG800AK_CN11LG"


#define AT32F403ARGT7_EG_VER_MAJOR     0   // 主版本(Major)
#define AT32F403ARGT7_EG_VER_MINOR     0   // 次版本(Minor)
#define AT32F403ARGT7_EG_VER_PATCH     2   // 修订号(Patch)

/**
 * 版本号整数代码 (用于代码逻辑中的大小比较)
 * 计算方式：Major * 10000 + Minor * 100 + Patch
 * 示例：1.0.3 -> 10003, 1.2.15 -> 10215
 */
#define AT32F403ARGT7_APP_VERSION_CODE              ((AT32F403ARGT7_EG_VER_MAJOR * 10000) + (AT32F403ARGT7_EG_VER_MINOR * 100) + AT32F403ARGT7_EG_VER_PATCH)

/* 版本号字符串 (用于日志打印、生成 JSON)
 * 示例："1.0.3"
 */
#define _STR_HELPER(x)                              #x
#define _STR(x)                                     _STR_HELPER(x)
#define AT32F403ARGT7_APP_VERSION_STR               _STR(AT32F403ARGT7_EG_VER_MAJOR) "." _STR(AT32F403ARGT7_EG_VER_MINOR) "." _STR(AT32F403ARGT7_EG_VER_PATCH)


//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<版本更新日志 (Version Changelog)>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>

/*
//--------------------------------------V0.0.2--------------------------------------
已知问题：
service层还是存在问题，联网成功，断网，MQTT断开没有通知上层app的能力，app要控制连接网络，连接MQTT，需要不断轮询查询网络连接状态。应该加入某种上报的机制。
第二个问题，这个模块自己APP可以实现，MCU不休眠，MQTT没有通讯任务，就自动休眠。MQTT消息接收本身就设置了150毫秒延迟，所以不论接收都是满足自主休眠唤醒的需求的。








*/
#endif // SYS_VERSION_H_
