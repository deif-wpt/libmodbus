/* DEIF_LEGAL_HEADER - DO NOT REMOVE THIS LINE! */


#ifndef MODBUS_TCP_SERVER_H
#define MODBUS_TCP_SERVER_H
#include <stdint.h>
#include <stdbool.h>

// MODBUS_TCP_SERVER_H: Prevent SDK modbus-tcp-server.h from being parsed by compiler
#define MODBUS_TCP_SERVER_H
#include "modbus.h"

#define MB_TCP_SRV_BLOCKING_TIMEOUT 0xFFFFFFFF // Use with modbus_tcp_server_set_select_timeout()

typedef struct _modbus_tcp_server modbus_tcp_server_t;

MODBUS_BEGIN_DECLS

#ifndef MODBUS_API
#define MODBUS_API
#endif

MODBUS_API modbus_tcp_server_t* modbus_tcp_server_start(const char* ipaddr, uint16_t port, uint16_t max_connections);
MODBUS_API int modbus_tcp_server_stop(modbus_tcp_server_t* mb_srv_ctx);
MODBUS_API int modbus_tcp_server_handle(modbus_tcp_server_t* mb_srv_ctx, modbus_mapping_t* mb_map);

MODBUS_API int modbus_tcp_server_set_select_timeout(modbus_tcp_server_t* mb_srv_ctx, uint32_t to_sec, uint32_t to_usec);

#define MODBUS_TCP_HAS_DEBUG
MODBUS_API void modbus_tcp_server_debug(modbus_tcp_server_t* mb_srv_ctx, bool enable);
MODBUS_API int modbus_tcp_server_error_count(modbus_tcp_server_t* mb_srv_ctx);
MODBUS_API int modbus_tcp_server_client_count(modbus_tcp_server_t* mb_srv_ctx);

MODBUS_END_DECLS

#endif /* MODBUS_TCP_H */
