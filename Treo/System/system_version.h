#ifndef SYS_VERSION_H_
#define SYS_VERSION_H_


#define PROJECT_NAME "EG800AK_CN11LG"


#define AT32F403ARGT7_EG_VER_MAJOR     0   // 主版本(Major)
#define AT32F403ARGT7_EG_VER_MINOR     0   // 次版本(Minor)
#define AT32F403ARGT7_EG_VER_PATCH     1   // 修订号(Patch)

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

//--------------------------------------V0.0.0--------------------------------------




#endif // SYS_VERSION_H_
