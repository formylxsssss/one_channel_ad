#ifndef ADC_TCP_SERVER_H
#define ADC_TCP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 为了尽量少改 main.c，函数名仍保留 AdcTcpServer_xxx。
 * 但本版 adc_tcp_server.c 内部已经不是服务器监听，而是 TCP Client：
 *   板子主动连接 g_app_net_cfg.server_ip:g_app_net_cfg.server_port。
 */
void AdcTcpServer_Init(void);
void AdcTcpServer_Task(void);

#ifdef __cplusplus
}
#endif

#endif
