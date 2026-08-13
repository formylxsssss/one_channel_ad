#ifndef APP_NET_CONFIG_H
#define APP_NET_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 5G 主动连接模式网络参数。
 *
 * local_ip/netmask/gateway 是 STM32 在局域网/USR-G815 LAN 侧的静态地址。
 * server_ip/server_port 是 STM32 主动连接的服务器地址。
 *
 * 当前默认值按你的局域网测试环境设置：
 *
 *   电脑服务器 IP：192.168.19.16
 *   STM32 本机 IP：192.168.19.100
 *   STM32 网关：   192.168.19.16
 *   服务器端口：   9000
 *
 * 如果后续接 USR-G815 正式测试，通常要改成：
 *
 *   STM32 本机 IP：192.168.10.100
 *   网关：         192.168.10.1
 *   服务器 IP：    公网服务器 IP
 *   服务器端口：   9000
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

/*
 * 局域网直连电脑测试时，网关可以填电脑 IP。
 * 如果你是通过路由器/交换机，网关也可以填真实路由器地址，例如 192.168.19.1。
 */
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

/*
 * 这里是下位机主动连接的服务器 IP。
 * 你的局域网测试中，电脑运行服务器脚本，所以填电脑 IP：192.168.19.16。
 */
#ifndef APP_DEFAULT_SERVER_IP_A
#define APP_DEFAULT_SERVER_IP_A         49U
#endif

#ifndef APP_DEFAULT_SERVER_IP_B
#define APP_DEFAULT_SERVER_IP_B         239U
#endif

#ifndef APP_DEFAULT_SERVER_IP_C
#define APP_DEFAULT_SERVER_IP_C         193U
#endif

#ifndef APP_DEFAULT_SERVER_IP_D
#define APP_DEFAULT_SERVER_IP_D         149U
#endif

#ifndef APP_DEFAULT_SERVER_PORT
#define APP_DEFAULT_SERVER_PORT         9000U
#endif

#ifndef APP_DEFAULT_DEVICE_ID
#define APP_DEFAULT_DEVICE_ID           3U
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

/*
 * 全工程统一使用这个变量名。
 * 不要再使用 g_net_cfg。
 */
extern AppNetConfig_t g_app_net_cfg;

AppNetConfig_t AppNetConfig_Default(void);
int AppNetConfig_IsValid(const AppNetConfig_t *cfg);

void AppNetConfig_FormatIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size);
void AppNetConfig_FormatLocalIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size);
void AppNetConfig_FormatGatewayIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size);

#ifdef __cplusplus
}
#endif

#endif