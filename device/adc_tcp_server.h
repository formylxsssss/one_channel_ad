#ifndef ADC_TCP_SERVER_H
#define ADC_TCP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 为了尽量少改 main.c，函数名仍保留 AdcTcpServer_xxx。
 * udp_client branch: all commands and data use UDP to the configured server.
 * Legacy public function names are retained; no TCP socket is opened.
 */
void AdcTcpServer_Init(void);
void AdcTcpServer_Task(void);

#ifdef __cplusplus
}
#endif

#endif
