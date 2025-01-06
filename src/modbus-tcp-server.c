/**
 * @file
 * This implementation is a copy from the PCM5.2 SDK. The source is located
 * here https://github.com/deif-wpt/libmodbus/tree/add-tcp-server
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#ifndef _MSC_VER
# include <unistd.h>
#endif

#if defined(_WIN32)
# define OS_WIN32
/* ws2_32.dll has getaddrinfo and freeaddrinfo on Windows XP and later.
 * minwg32 headers check WINVER before allowing the use of these */
# ifndef WINVER
# define WINVER 0x0501
# endif
/* Already set in modbus-tcp.h but it seems order matters in VS2005 */
# include <winsock2.h>
# include <ws2tcpip.h>
# define SHUT_RDWR 2
# define TIME_UTC 1
# define close closesocket
#else
# include <sys/select.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
#endif

#include "modbus-tcp-server.h"
#include <pthread.h>
#include <time.h>

/// scanbuild is buggy, sigh
#define USE_ASSERT 0

#if USE_ASSERT
#include <assert.h>
#endif

#define MB_TCP_SRV_IDLE_TIMEOUT 60

/* modbus client data */
struct modbus_tcp_client_t {
    int socket;
    uint64_t cycle;
    struct modbus_tcp_client_t* next;
};

/* mdbus server data */
struct _modbus_tcp_server {
    int server_socket;
    fd_set refset;
    fd_set rdset;
    int fdmax;
    unsigned fdcount;
    unsigned max_connections;
    uint16_t port;
    uint32_t idle_time_sec;
    uint32_t select_to_sec;
    uint32_t select_to_usec;
    uint8_t shutdown;

    modbus_t* ctx;
    /* Linked list of all current connections */
    /* Used as single chained list as scanbuild is broken */
    struct modbus_tcp_client_t conn;
    unsigned conn_count;
    int debug; // debug flag
    int error_count;
};

#if USE_ASSERT
static bool _cli_in_list(modbus_tcp_server_t* data, struct modbus_tcp_client_t* cli) {
    struct modbus_tcp_client_t* sp;
    for (sp = data->conn.next; sp != &data->conn; sp = sp->next) {
        if (sp == cli) {
            return true;
        }
    }
    return false;
}
#endif

static void _modbus_tcp_srv_update_fdmax(modbus_tcp_server_t* data) {
    struct modbus_tcp_client_t* sp;
    int fdmax = data->server_socket;
    unsigned fdcount = 0;
    for (sp = data->conn.next; sp != &data->conn; sp = sp->next) {
        if (sp->socket > fdmax) {
            fdmax = sp->socket;
        }
        fdcount++;
    }
    data->fdmax = fdmax;
    data->fdcount = fdcount;
}

static void _modbus_tcp_srv_rm_cli(modbus_tcp_server_t* data,
        struct modbus_tcp_client_t* cli) {
    int socket_fd = cli->socket;
    close(cli->socket);
    FD_CLR(cli->socket, &data->refset);

#if USE_ASSERT
    struct modbus_tcp_client_t* entry = cli;
#endif

    struct modbus_tcp_client_t** psp = &data->conn.next;
    while (*psp != &data->conn) {
      if (*psp == cli) {
         *psp = cli->next;
         free(cli);
         cli = NULL;
         data->conn_count--;
         break;
      }
      psp = &(*psp)->next;
    }


#if USE_ASSERT
    assert(cli == NULL);
    assert(!_cli_in_list(data, entry));
#endif    
    _modbus_tcp_srv_update_fdmax(data);
   if (data->debug)
   {
      fprintf(stderr, "MB TCP server on port %d, fd: %d close fdmax: %d conn_count: %u fdcount: %u\n",
         data->port, socket_fd, data->fdmax, data->conn_count, data->fdcount);
   }
}

static void _modbus_tcp_srv_del_oldest_cli(modbus_tcp_server_t* data) {
    struct modbus_tcp_client_t* cli_to_remove = NULL;
    uint64_t oldest_cycle = UINT64_MAX;

    /* Remove client from list of connected clients*/
    struct modbus_tcp_client_t* sp;
    for (sp = data->conn.next; sp != &data->conn; sp = sp->next) {
        if (sp->cycle < oldest_cycle) {
            cli_to_remove = sp;
            oldest_cycle = sp->cycle;
        }
    }

    /* If we found one, remove it */
    if (cli_to_remove != NULL) {
        if (data->debug) {
           fprintf(stderr,
                    "MB TCP server on port %d, fd: %d close oldest cycle %llu age %llu\n",
                       data->port, cli_to_remove->socket, (unsigned long long) (data->conn.cycle - cli_to_remove->cycle), (unsigned long long) cli_to_remove->cycle);
        }
        _modbus_tcp_srv_rm_cli(data, cli_to_remove);
    }
}

static void _modbus_tcp_srv_add_cli(modbus_tcp_server_t* data, int socket) {
    /* Modbus specification: if no available slots, remove oldest client when new connects */
    if (data->conn_count >= data->max_connections) {
       _modbus_tcp_srv_del_oldest_cli(data);
       if (data->conn_count >= data->max_connections) {         
          fprintf(stderr,
               "MB TCP server stuck at %u max connections - ** INTERNAL ERROR **\n", 
                  data->conn_count);
          data->error_count++;
          return;
       }
    }
    
    struct modbus_tcp_client_t* sp = malloc(sizeof(*sp));
    sp->cycle = data->conn.cycle;
    sp->socket = socket;
    sp->next = data->conn.next;
    data->conn.next = sp;
    data->conn_count++;

    /* add to listener*/
    FD_SET(socket, &data->refset);

    _modbus_tcp_srv_update_fdmax(data);

    if (data->debug) {
      fprintf(stderr, "MB _modbus_tcp_srv_add_cli() fd: %d cycle: %llu fdmax: %d conn_count: %u fdcount: %u\n", sp->socket, (unsigned long long) sp->cycle, data->fdmax, data->conn_count, data->fdcount);
    }
    if (data->fdcount != data->conn_count)
    {
      fprintf(stderr, "MB _modbus_tcp_srv_add_cli() ERROR conn_count: %u != fdcount: %u\n", data->conn_count, data->fdcount);
    }
}

static void _modbus_tcp_server_stop(modbus_tcp_server_t* srv_ctx) {

    if (srv_ctx != NULL) {
        while (srv_ctx->conn_count > 0) {
           _modbus_tcp_srv_rm_cli(srv_ctx, srv_ctx->conn.next);
        }

        /* Close modbus server sockets*/
        close(srv_ctx->server_socket);

        /* Release main context */
        if (srv_ctx->ctx != NULL) {
            modbus_free(srv_ctx->ctx);
            srv_ctx->ctx = NULL;
        }

        bool debug = srv_ctx->debug;
        /* Release server data */
        free(srv_ctx);

        if (debug) {
            fprintf(stderr, "MB TCP server stopped\n");
        } 
    }
}

modbus_tcp_server_t* modbus_tcp_server_start(const char* ipaddr, uint16_t port, uint16_t max_connections) {
    modbus_tcp_server_t* data;

    /* Check select can support the request connections */
    if (max_connections > FD_SETSIZE) {
        errno = EFBIG;
        return NULL;
    }

    data = calloc(1, sizeof(*data));
    if (data == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    data->max_connections = max_connections;
    data->port = port;
    data->idle_time_sec = MB_TCP_SRV_IDLE_TIMEOUT;
    data->select_to_sec = MB_TCP_SRV_BLOCKING_TIMEOUT;
    data->select_to_usec = 0;

    /* Create new server */
    data->ctx = modbus_new_tcp(ipaddr, port);
    if (data->ctx == NULL) {
        /* libmodbus has set errno */
        free(data);
        return NULL;
    }

    /* Open socket */
    data->server_socket = modbus_tcp_listen(data->ctx, 5);

    if (data->server_socket < 0) {
        modbus_free(data->ctx);
        /* libmodbus has set errno */
        free(data);
        return NULL;
    }

    /* Clear the reference set of socket */
    FD_ZERO(&data->refset);

    /* Add the server socket */
    FD_SET(data->server_socket, &data->refset);

    /* Empty single linked list, anchor 'data->conn' references itself */
    data->conn.next = &data->conn;

    /* Keep track of the max file descriptor */
    _modbus_tcp_srv_update_fdmax(data);

    return data;
}

int modbus_tcp_server_stop(modbus_tcp_server_t* srv_ctx) {

    if (srv_ctx == NULL) {
        return -1;
    }

    srv_ctx->shutdown = 1;
    /* send shutdown event to accept() in handle function */
    shutdown(srv_ctx->server_socket, SHUT_RDWR);
    return 0;
}

int modbus_tcp_server_handle(modbus_tcp_server_t* srv_ctx,
        modbus_mapping_t* mb_map) {
    int retval, rc;

    if ((srv_ctx == NULL) || (srv_ctx->ctx == NULL)) {
        errno = EBADF;
        return -1;
    }

    /* Reset select set and select timeout */
    srv_ctx->rdset = srv_ctx->refset;

    /* Blocking select waiting for connections */
    if (srv_ctx->select_to_sec == MB_TCP_SRV_BLOCKING_TIMEOUT
            || srv_ctx->select_to_usec == MB_TCP_SRV_BLOCKING_TIMEOUT) {
        retval = select(srv_ctx->fdmax + 1, &srv_ctx->rdset, NULL, NULL, NULL);
    } else {
        /* Non-blocking select with timeout waiting for connections */
        struct timeval scan_ms;
        scan_ms.tv_sec = srv_ctx->select_to_sec;
        scan_ms.tv_usec = srv_ctx->select_to_usec;
        retval = select(srv_ctx->fdmax + 1, &srv_ctx->rdset, NULL, NULL,
                &scan_ms);
    }

    /* If the context has been destroyed, bailout */
    if (srv_ctx->ctx == NULL) {
        errno = EBADF;
        return -1;
    }

    if (srv_ctx->shutdown) {
        _modbus_tcp_server_stop(srv_ctx);
        errno = ECONNRESET;
        return -1;
    }

    if (retval == 0) {
        /* timeout, this is OK */
    } else if (retval == -1) ///< Critical error on select, exit
    {
        /* select has set errno */
        modbus_tcp_server_stop(srv_ctx);
        return -1;
    } else {
        // cycle counter used as time, independent of time progression, time change
        srv_ctx->conn.cycle++;

        /* New connection request */
        if (FD_ISSET(srv_ctx->server_socket, &srv_ctx->rdset)) {
            socklen_t addrlen;
            struct sockaddr_storage clientaddr;
            int newfd;

            /* Handle new connections */
            addrlen = sizeof(clientaddr);
            memset(&clientaddr, 0, sizeof(clientaddr));
            newfd = accept(srv_ctx->server_socket,
                    (struct sockaddr *) &clientaddr, &addrlen);

            /* Debug if needed */
            if (srv_ctx->debug && newfd > 0) {
                char ipstr[INET6_ADDRSTRLEN + 1] = { 0 };
                int port = 0;

                struct sockaddr_in *s = (struct sockaddr_in *) &clientaddr;
                port = ntohs(s->sin_port);
                #ifdef USE_VERY_SLOW_NAME_RESOLUTION
                getnameinfo((struct sockaddr *) &clientaddr, sizeof(clientaddr),
                        ipstr, sizeof(ipstr), NULL, 0, 0);
                #else
                inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
                #endif
                fprintf(stderr,
                        "MB TCP server on port %d, fd: %d Incoming connection from %s -> %d\n",
                        srv_ctx->port, newfd, ipstr, port);
            }

            if (newfd == -1) {
                if (srv_ctx->debug) {
                    perror("Server accept() error");
                    fprintf(stderr, " Socket: %d on port: %d\r\n",
                            srv_ctx->server_socket, srv_ctx->port);
                }
                /* accept has set errno */
                return -1;
            } else {
                _modbus_tcp_srv_add_cli(srv_ctx, newfd);
            }
        }

        /* Data request */
         struct modbus_tcp_client_t* sp = srv_ctx->conn.next;

         while (sp != &srv_ctx->conn) {
            struct modbus_tcp_client_t* next = sp->next;
               if (FD_ISSET(sp->socket, &srv_ctx->rdset)) {
                  /* An already connected master has sent a new query */
                  uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];

                  modbus_set_socket(srv_ctx->ctx, sp->socket);
                  rc = modbus_receive(srv_ctx->ctx, query);

                  if (rc != -1) {
                     modbus_reply(srv_ctx->ctx, query, rc, mb_map);
                     sp->cycle = srv_ctx->conn.cycle;
                     /* considering implementing a callback pointer with a registration function
                        which the user can use for a 'request from IP on port has been handled' event. */
                  } else {
                     // sp will properly point to freed memory after this call.
                     if (srv_ctx->debug) {
                        fprintf(stderr,
                                 "MB TCP server on port %d, fd: %d modbus_receive error(%d,%s)\n", 
                                    srv_ctx->port, sp->socket, errno, strerror(errno));
                     }
                     _modbus_tcp_srv_rm_cli(srv_ctx, sp);
                  }
               }
               sp = next;
         }
    }
    return 0;
}

int modbus_tcp_server_set_select_timeout(modbus_tcp_server_t* srv_ctx, uint32_t to_sec, uint32_t to_usec) {

    if (srv_ctx == NULL) {
        errno = EBADF;
        return -1;
    }
    srv_ctx->select_to_sec = to_sec;
    srv_ctx->select_to_usec = to_usec;
    return 0;
}

void modbus_tcp_server_debug(modbus_tcp_server_t* mb_srv_ctx, bool enable)
{
   if (mb_srv_ctx) {
      mb_srv_ctx->debug = enable;
   }
}

int modbus_tcp_server_error_count(modbus_tcp_server_t* mb_srv_ctx)
{
   int rc = -1;
   if (mb_srv_ctx) {
      rc = mb_srv_ctx->error_count;
   }
   return rc;
}

int modbus_tcp_server_client_count(modbus_tcp_server_t* mb_srv_ctx)
{
   int rc = -1;
   if (mb_srv_ctx) {
      rc = mb_srv_ctx->conn_count;
   }
   return rc;
}
