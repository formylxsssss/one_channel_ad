#include "app_net_config.h"
#include <stdio.h>

AppNetConfig_t g_app_net_cfg =
{
    {
        APP_DEFAULT_LOCAL_IP_A,
        APP_DEFAULT_LOCAL_IP_B,
        APP_DEFAULT_LOCAL_IP_C,
        APP_DEFAULT_LOCAL_IP_D
    },

    {
        APP_DEFAULT_NETMASK_A,
        APP_DEFAULT_NETMASK_B,
        APP_DEFAULT_NETMASK_C,
        APP_DEFAULT_NETMASK_D
    },

    {
        APP_DEFAULT_GATEWAY_IP_A,
        APP_DEFAULT_GATEWAY_IP_B,
        APP_DEFAULT_GATEWAY_IP_C,
        APP_DEFAULT_GATEWAY_IP_D
    },

    {
        APP_DEFAULT_SERVER_IP_A,
        APP_DEFAULT_SERVER_IP_B,
        APP_DEFAULT_SERVER_IP_C,
        APP_DEFAULT_SERVER_IP_D
    },

    APP_DEFAULT_SERVER_PORT,
    APP_DEFAULT_DEVICE_ID,
    0U,

    APP_TCP_RECONNECT_INTERVAL_MS,
    0U
};

static int ip_all_zero(const uint8_t ip[4])
{
    return (ip[0] == 0U) &&
           (ip[1] == 0U) &&
           (ip[2] == 0U) &&
           (ip[3] == 0U);
}

static int ip_all_255(const uint8_t ip[4])
{
    return (ip[0] == 255U) &&
           (ip[1] == 255U) &&
           (ip[2] == 255U) &&
           (ip[3] == 255U);
}

static int ipv4_basic_valid(const uint8_t ip[4])
{
    if (ip == 0)
    {
        return 0;
    }

    if (ip_all_zero(ip) || ip_all_255(ip))
    {
        return 0;
    }

    /*
     * 对 local_ip / gateway / server_ip 做基本保护。
     * 不允许主机号为 0 或 255。
     *
     * 注意：
     * server_ip 正式部署时可以是公网 IP，不要求和 local_ip 同网段。
     */
    if ((ip[3] == 0U) || (ip[3] == 255U))
    {
        return 0;
    }

    return 1;
}

static int netmask_basic_valid(const uint8_t ip[4])
{
    if (ip == 0)
    {
        return 0;
    }

    if (ip_all_zero(ip) || ip_all_255(ip))
    {
        return 0;
    }

    /*
     * 当前项目默认使用 255.255.255.0。
     * 这里允许常见掩码，不做复杂连续性校验。
     */
    return 1;
}

AppNetConfig_t AppNetConfig_Default(void)
{
    AppNetConfig_t cfg;

    cfg.local_ip[0] = APP_DEFAULT_LOCAL_IP_A;
    cfg.local_ip[1] = APP_DEFAULT_LOCAL_IP_B;
    cfg.local_ip[2] = APP_DEFAULT_LOCAL_IP_C;
    cfg.local_ip[3] = APP_DEFAULT_LOCAL_IP_D;

    cfg.netmask[0] = APP_DEFAULT_NETMASK_A;
    cfg.netmask[1] = APP_DEFAULT_NETMASK_B;
    cfg.netmask[2] = APP_DEFAULT_NETMASK_C;
    cfg.netmask[3] = APP_DEFAULT_NETMASK_D;

    cfg.gateway[0] = APP_DEFAULT_GATEWAY_IP_A;
    cfg.gateway[1] = APP_DEFAULT_GATEWAY_IP_B;
    cfg.gateway[2] = APP_DEFAULT_GATEWAY_IP_C;
    cfg.gateway[3] = APP_DEFAULT_GATEWAY_IP_D;

    cfg.server_ip[0] = APP_DEFAULT_SERVER_IP_A;
    cfg.server_ip[1] = APP_DEFAULT_SERVER_IP_B;
    cfg.server_ip[2] = APP_DEFAULT_SERVER_IP_C;
    cfg.server_ip[3] = APP_DEFAULT_SERVER_IP_D;

    cfg.server_port = APP_DEFAULT_SERVER_PORT;
    cfg.device_id = APP_DEFAULT_DEVICE_ID;
    cfg.reserved0 = 0U;
    cfg.reconnect_ms = APP_TCP_RECONNECT_INTERVAL_MS;
    cfg.reserved1 = 0U;

    return cfg;
}

int AppNetConfig_IsValid(const AppNetConfig_t *cfg)
{
    if (cfg == 0)
    {
        return 0;
    }

    if (!ipv4_basic_valid(cfg->local_ip))
    {
        return 0;
    }

    if (!netmask_basic_valid(cfg->netmask))
    {
        return 0;
    }

    if (!ipv4_basic_valid(cfg->gateway))
    {
        return 0;
    }

    if (!ipv4_basic_valid(cfg->server_ip))
    {
        return 0;
    }

    if ((cfg->server_port == 0U) || (cfg->server_port == 65535U))
    {
        return 0;
    }

    if ((cfg->device_id == 0U) || (cfg->device_id > 247U))
    {
        return 0;
    }

    if ((cfg->reconnect_ms < 500UL) || (cfg->reconnect_ms > 60000UL))
    {
        return 0;
    }

    return 1;
}

static void format_ip4(const uint8_t ip[4], char *out, uint32_t out_size)
{
    if ((ip == 0) || (out == 0) || (out_size == 0U))
    {
        return;
    }

    (void)snprintf(out,
                   (size_t)out_size,
                   "%u.%u.%u.%u",
                   (unsigned int)ip[0],
                   (unsigned int)ip[1],
                   (unsigned int)ip[2],
                   (unsigned int)ip[3]);
}

void AppNetConfig_FormatIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size)
{
    if (cfg == 0)
    {
        return;
    }

    format_ip4(cfg->server_ip, out, out_size);
}

void AppNetConfig_FormatLocalIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size)
{
    if (cfg == 0)
    {
        return;
    }

    format_ip4(cfg->local_ip, out, out_size);
}

void AppNetConfig_FormatGatewayIp(const AppNetConfig_t *cfg, char *out, uint32_t out_size)
{
    if (cfg == 0)
    {
        return;
    }

    format_ip4(cfg->gateway, out, out_size);
}