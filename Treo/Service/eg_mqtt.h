#ifndef EG_MQTT_H_
#define EG_MQTT_H_


// 测试宏开关
#define MQTT_TEST_ENVIRONMENT 0




#if MQTT_TEST_ENVIRONMENT

#define MQTT_SERVER_HOST                    "broker.emqx.io"            // 测试域名

#else 
#define MQTT_SUB_Product_ID                 "GMD4318"                                               // 项目编号
#define MQTT_LOGIN_KEY                      "GMDULTRA4317MQTT2025MQswCQYDVGMD4317Usp3mbMGA1"        // MD5登陆密钥
#define MQTT_SERVER_HOST                    "mqtt.gmd-cloud.com"        // 服务器域名

//需要订阅的话题
#define MQTT_SUB_COMMON_PART                "/GMD/Embedded/" MQTT_SUB_Product_ID "/Device/"
#define MQTT_SUB_Location_Reply             "/Position/Report/Reply"                            //设备位置上报回复
#define MQTT_SUB_File_Update                "/File/Update"                                      //设备文件更新下发
#define MQTT_SUB_OTA                        "/Firmware/Update"                                  //设备OTA升级下发
#define MQTT_SUB_Config_Get                 "/Config"                                           //获取设备配置的主题
#define MQTT_SUB_Config_Update              "/Config/Update"                                    //更新设备配置的主题
#define MQTT_SUB_Register_Reply             "/Register/Reply"                                   //设备注册回复

//需要发布的话题
#define MQTT_PUB_COMMON_PART                "/GMD/Embedded/" MQTT_SUB_Product_ID "/Centre"
#define MQTT_PUB_Location_Topic             MQTT_PUB_COMMON_PART "/Position/Report"             //设备上报位置主题
#define MQTT_PUB_File_Update_Topic          MQTT_PUB_COMMON_PART "/File/Update/Reply"           //设备文件更新答复主题
#define MQTT_PUB_Config_Reply_Topic         MQTT_PUB_COMMON_PART "/Config/Reply"                //获取设备配置的，回复主题
#define MQTT_PUB_Config_Update_Reply_Topic  MQTT_PUB_COMMON_PART "/Config/Update/Reply"         //更新设备配置的，回复主题
#define MQTT_PUB_OTA_Reply_Topic            MQTT_PUB_COMMON_PART "/Firmware/Update/Reply"       //设备OTA升级答复主题
#define MQTT_PUB_Register_Topic             MQTT_PUB_COMMON_PART "/Register"                    //设备注册主题回复
#define MQTT_PUB_EVENT_Topic                MQTT_PUB_COMMON_PART "/Report/Event"                //设备事件上报主题

#endif



// MQTT 配置项
#define EG800_MQTT_CLIENT_INDEX             0U                          // MQTT客户端编号
#define MQTT_SERVER_PORT                    1883U                       // 非TLS MQTT服务器端口
#define MQTT_VERSION                        4U                          // MQTT协议版本4，对应MQTT v3.1.1
#define MQTT_DEFAULT_QOS                    0U                          // MQTT QoS等级
#define MQTT_DEFAULT_RETAIN                 0U                          // 服务器不保留该消息












#endif // EG_MQTT_H_
