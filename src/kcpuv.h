#ifndef KCPUV_H
#define KCPUV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ikcp.h"

#define KCPUV_DEBUG 0

#define KCPUV_TIMER_INTERVAL 10
#define KCPUV_MUX_CONN_TIMEOUT 0
#define KCPUV_STATE_CREATED 0
#define KCPUV_STATE_ACK 10
#define KCPUV_STATE_WAIT_ACK 20
#define KCPUV_STATE_READY 30
#define KCPUV_STATE_CLOSED 40
#define KCPUV_CMD_ACK 10
#define KCPUV_CMD_PUSH 20
#define KCPUV_CMD_NOO 30
#define KCPUV_CMD_CLS 40
#define KCPUV_NONCE_LENGTH 8
#define KCPUV_PROTOCOL_OVERHEAD 1
#define KCPUV_OVERHEAD KCPUV_NONCE_LENGTH + KCPUV_PROTOCOL_OVERHEAD
#define INIT_WND_SIZE 2048
#define MTU_DEF 1400 - KCPUV_PROTOCOL_OVERHEAD - KCPUV_NONCE_LENGTH
// #define IKCP_OVERHEAD  24
#define MAX_SENDING_LEN 65536
#define BUFFER_LEN MAX_SENDING_LEN
#define DEFAULT_TIMEOUT 30000
#define IP4_ADDR_LENTH 17
#define IP6_ADDR_LENGTH 68
#define KCPUV_MUX_CMD_PUSH 10
#define KCPUV_MUX_CMD_CLS 40
// one byte for cmd,
// four byte for id,
// two byte for length,
#define KCPUV_MUX_PROTOCOL_OVERHEAD 7
#define MAX_MUX_CONTENT_LEN MAX_SENDING_LEN - KCPUV_MUX_PROTOCOL_OVERHEAD

#ifdef __cplusplus
}
#endif
#endif
