#ifndef APP_EG_H_
#define APP_EG_H_

#define EG800_MQTT_SERVER_PORT               1883U          // 非 TLS 服务器端口



// 测试环境选择；1 使用测试服务器，0 使用正式服务器。
#define EG800_MQTT_TEST_ENVIRONMENT          1U
#if EG800_MQTT_TEST_ENVIRONMENT

// #define EG800_MQTT_SERVER_HOST                    "broker.emqx.io"            // 测试域名
#define EG800_MQTT_SERVER_HOST                    "mqtt.gmd-cloud.com"            // 测试域名

#else 
#define EG800_MQTT_LOGIN_KEY                  "GMDULTRA4317MQTT2025MQswCQYDVGMD4317Usp3mbMGA1"        // MD5登陆密钥
#define EG800_MQTT_SERVER_HOST                  "mqtt.gmd-cloud.com"                                    // 正式服务器域名



// 产品和主题定义同时用于测试环境与正式环境。
#define EG800_MQTT_PRODUCT_ID                   "GMD4318"                                              // 产品编号

// 订阅主题需要在前缀和后缀之间拼入设备 IMEI。
#define EG800_MQTT_SUB_TOPIC_PREFIX             "/GMD/Embedded/" EG800_MQTT_PRODUCT_ID "/Device/"       // 设备订阅主题前缀
#define EG800_MQTT_SUB_LOCATION_REPLY_SUFFIX    "/Position/Report/Reply"                                // 定位上报回复主题后缀
#define EG800_MQTT_SUB_FILE_UPDATE_SUFFIX       "/File/Update"                                          // 文件更新下发主题后缀
#define EG800_MQTT_SUB_OTA_SUFFIX               "/Firmware/Update"                                      // OTA 升级下发主题后缀
#define EG800_MQTT_SUB_CONFIG_GET_SUFFIX        "/Config"                                               // 获取设备配置主题后缀
#define EG800_MQTT_SUB_CONFIG_UPDATE_SUFFIX     "/Config/Update"                                        // 更新设备配置主题后缀
#define EG800_MQTT_SUB_REGISTER_REPLY_SUFFIX    "/Register/Reply"                                       // 设备注册回复主题后缀

// 发布主题由固定字符串组成，可以直接得到完整主题。
#define EG800_MQTT_PUB_TOPIC_PREFIX               "/GMD/Embedded/" EG800_MQTT_PRODUCT_ID "/Centre"                     // 发布主题公共前缀
#define EG800_MQTT_PUB_LOCATION_TOPIC             EG800_MQTT_PUB_TOPIC_PREFIX "/Position/Report"                       // 完整的定位上报主题
#define EG800_MQTT_PUB_FILE_UPDATE_REPLY_TOPIC    EG800_MQTT_PUB_TOPIC_PREFIX "/File/Update/Reply"                     // 完整的文件更新答复主题
#define EG800_MQTT_PUB_CONFIG_REPLY_TOPIC         EG800_MQTT_PUB_TOPIC_PREFIX "/Config/Reply"                          // 完整的获取配置答复主题
#define EG800_MQTT_PUB_CONFIG_UPDATE_REPLY_TOPIC  EG800_MQTT_PUB_TOPIC_PREFIX "/Config/Update/Reply"                   // 完整的更新配置答复主题
#define EG800_MQTT_PUB_OTA_REPLY_TOPIC            EG800_MQTT_PUB_TOPIC_PREFIX "/Firmware/Update/Reply"                 // 完整的 OTA 升级答复主题
#define EG800_MQTT_PUB_REGISTER_TOPIC             EG800_MQTT_PUB_TOPIC_PREFIX "/Register"                              // 完整的设备注册主题
#define EG800_MQTT_PUB_EVENT_TOPIC                EG800_MQTT_PUB_TOPIC_PREFIX "/Report/Event"                          // 完整的设备事件上报主题

#endif













void App_EG_Init(void);

void App_EG_Launch(void);

// 持续阻塞接收 MQTT 消息并打印，接收失败时返回。
void App_EG_Mqtt_Receive(void);

#endif // APP_EG_H_

