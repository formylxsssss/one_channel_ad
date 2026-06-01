#ifndef APP_NET_CONFIG_H
#define APP_NET_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 5G主动连接模式网络参数。
 *
 * local_ip/netmask/gateway 是 STM32 在 USR-G815 LAN 侧的静态地址。
 * server_ip/server_port 是 STM32 主动连接的公网服务器地址。
 *
 * 默认用于局域网联调：
 *   STM32：      192.168.10.100
 *   USR-G815：   192.168.10.1
 *   测试服务器：192.168.10.200:9000
 * 正式部署时，通过上位机 SET_NET 把 server_ip 改为公网服务器 IP。
 */
#ifndef APP_DEFAULT_LOCAL_IP_A
#define APP_DEFAULT_LOCAL_IP_A          192U
#endif
#ifndef APP_DEFAULT_LOCAL_IP_B
#define APP_DEFAULT_LOCAL_IP_B          168U
#endif
#ifndef APP_DEFAULT_LOCAL_IP_C
#define APP_DEFAULT_LOCAL_IP_C          10U
#endif
#ifndef APP_DEFAULT_LOCAL_IP_D
#define APP_DEFAULT_LOCAL_IP_D          100U
#endif

#ifndef APP_DEFAULT_NETMASK_A
#define APP_DEFAULT_NETMASK_A           255U
#endif
#ifndef APP_DEFAULT_NETMASK_B
#define APP_DEFAULT_NETMASK_B           255U
#endif
#ifndef APP_DEFAULT_NETMASK_C
#define APP_DEFAULT_NETMASK_C           255U
#endif
#ifndef APP_DEFAULT_NETMASK_D
#define APP_DEFAULT_NETMASK_D           0U
#endif

#ifndef APP_DEFAULT_GATEWAY_IP_A
#define APP_DEFAULT_GATEWAY_IP_A        192U
#endif
#ifndef APP_DEFAULT_GATEWAY_IP_B
#define APP_DEFAULT_GATEWAY_IP_B        168U
#endif
#ifndef APP_DEFAULT_GATEWAY_IP_C
#define APP_DEFAULT_GATEWAY_IP_C        10U
#endif
#ifndef APP_DEFAULT_GATEWAY_IP_D
#define APP_DEFAULT_GATEWAY_IP_D        1U
#endif

#ifndef APP_DEFAULT_SERVER_IP_A
#define APP_DEFAULT_SERVER_IP_A         192U
#endif
#ifndef APP_DEFAULT_SERVER_IP_B
#define APP_DEFAULT_SERVER_IP_B         168U
#endif
#ifndef APP_DEFAULT_SERVER_IP_C
#define APP_DEFAULT_SERVER_IP_C         10U
#endif
#ifndef APP_DEFAULT_SERVER_IP_D
#define APP_DEFAULT_SERVER_IP_D         200U
#endif

#ifndef APP_DEFAULT_SERVER_PORT
#define APP_DEFAULT_SERVER_PORT         9000U
#endif

#ifndef APP_DEFAULT_DEVICE_ID
#define APP_DEFAULT_DEVICE_ID           1U
#endif

#ifndef APP_TCP_RECONNECT_INTERVAL_MS
#define APP_TCP_RECONNECT_INTERVAL_MS   3000UL
#endif

#ifndef APP_NET_SET_FLAG_SAVE
#define APP_NET_SET_FLAG_SAVE           0x01U
#endif
#ifndef APP_NET_SET_FLAG_RECONNECT
#define APP_NET_SET_FLAG_RECONNECT      0x02U
#endif

typedef struct
{
    uint8_t  local_ip[4];
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    uint8_t  server_ip[4];

    uint16_t server_port;
    uint8_t  device_id;
    uint8_t  reserved0;

    uint32_t reconnect_ms;
    uint32_t reserved1;
} AppNetConfig_t;

extern AppNetConfig_t g_app_net_cfg;

AppNetConfig_t AppNetConfig_Default(void);
int AppNetConfig_IsValid(const AppNetConfig_t *cfg);

/* 兼容 adc_tcp_server.c 旧调用名：格式化 server_ip。 */
void AppNetConfig_FormatIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size);
void AppNetConfig_FormatLocalIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size);
void AppNetConfig_FormatGatewayIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size);

#ifdef __cplusplus
}
#endif

#endif
