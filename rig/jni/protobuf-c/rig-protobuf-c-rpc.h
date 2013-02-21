#ifndef __PROTOBUF_C_RPC_H_
#define __PROTOBUF_C_RPC_H_

/* Protocol is:
 *    client issues request with header:
 *         method_index              32-bit little-endian
 *         message_length            32-bit little-endian
 *         request_id                32-bit any-endian
 *    server responds with header:
 *         status_code               32-bit little-endian
 *         method_index              32-bit little-endian
 *         message_length            32-bit little-endian
 *         request_id                32-bit any-endian
 */
#include <google/protobuf-c/protobuf-c-dispatch.h>

typedef enum
{
  PROTOBUF_C_RPC_ADDRESS_LOCAL,  /* unix-domain socket */
  PROTOBUF_C_RPC_ADDRESS_TCP     /* host/port tcp socket */
} ProtobufC_RPC_AddressType;

typedef enum
{
  PROTOBUF_C_ERROR_CODE_HOST_NOT_FOUND,
  PROTOBUF_C_ERROR_CODE_CONNECTION_REFUSED,
  PROTOBUF_C_ERROR_CODE_CLIENT_TERMINATED,
  PROTOBUF_C_ERROR_CODE_BAD_REQUEST,
  PROTOBUF_C_ERROR_CODE_PROXY_PROBLEM
} ProtobufC_RPC_Error_Code;

typedef enum
{
  PROTOBUF_C_STATUS_CODE_SUCCESS,
  PROTOBUF_C_STATUS_CODE_SERVICE_FAILED,
  PROTOBUF_C_STATUS_CODE_TOO_MANY_PENDING
} ProtobufC_RPC_Status_Code;

typedef void (*ProtobufC_RPC_Error_Func)   (ProtobufC_RPC_Error_Code code,
                                            const char              *message,
                                            void                    *error_func_data);

/* --- Client API --- */
typedef struct _ProtobufC_RPC_Client ProtobufC_RPC_Client;

/* The return value (the service) may be cast to ProtobufC_RPC_Client* */
ProtobufCService *rig_protobuf_c_rpc_client_new (ProtobufC_RPC_AddressType type,
                                                 const char               *name,
                                                 const ProtobufCServiceDescriptor *descriptor,
                                                 ProtobufCDispatch       *dispatch /* or NULL */
                                                 );

/* forcing the client to connect */
typedef enum
{
  PROTOBUF_C_RPC_CLIENT_CONNECT_SUCCESS,/* also returned if already connected */
  PROTOBUF_C_RPC_CLIENT_CONNECT_ERROR_NAME_LOOKUP,
  PROTOBUF_C_RPC_CLIENT_CONNECT_ERROR_CONNECT
} ProtobufC_RPC_Client_ConnectStatus;

ProtobufC_RPC_Client_ConnectStatus
rig_protobuf_c_rpc_client_connect (ProtobufC_RPC_Client *client);

/* --- configuring the client */


/* Pluginable async dns hooks */
/* TODO: use adns library or port evdns? ugh */
typedef void (*ProtobufC_NameLookup_Found) (const uint8_t *address,
                                            void          *callback_data);
typedef void (*ProtobufC_NameLookup_Failed)(const char    *error_message,
                                            void          *callback_data);
typedef void (*ProtobufC_NameLookup_Func)  (ProtobufCDispatch *dispatch,
                                            const char        *name,
                                            ProtobufC_NameLookup_Found found_func,
                                            ProtobufC_NameLookup_Failed failed_func,
                                            void *callback_data);
void rig_protobuf_c_rpc_client_set_name_resolver (ProtobufC_RPC_Client *client,
                                                  ProtobufC_NameLookup_Func resolver);

/* Error handling */
void rig_protobuf_c_rpc_client_set_error_handler (ProtobufC_RPC_Client *client,
                                                  ProtobufC_RPC_Error_Func func,
                                                  void                 *error_func_data);

/* Configuring the autoreconnect behavior.
   If the client is disconnected, all pending requests get an error.
   If autoreconnect is set, and it is by default, try connecting again
   after a certain amount of time has elapsed. */
void rig_protobuf_c_rpc_client_disable_autoreconnect (ProtobufC_RPC_Client *client);
void rig_protobuf_c_rpc_client_set_autoreconnect_period (ProtobufC_RPC_Client *client,
                                                         unsigned              millis);

/* checking the state of the client */
protobuf_c_boolean rig_protobuf_c_rpc_client_is_connected (ProtobufC_RPC_Client *client);

/* NOTE: we don't actually start connecting til the main-loop runs,
   so you may configure the client immediately after creation */

/* --- Server API --- */
typedef struct _ProtobufC_RPC_Server ProtobufC_RPC_Server;
ProtobufC_RPC_Server *
     rig_protobuf_c_rpc_server_new    (ProtobufC_RPC_AddressType type,
                                       const char               *name,
                                       ProtobufCService         *service,
                                       ProtobufCDispatch       *dispatch /* or NULL */
                                      );

int
rig_protobuf_c_rpc_server_get_fd (ProtobufC_RPC_Server *server);

typedef struct _ProtobufC_RPC_ServerConnection ProtobufC_RPC_ServerConnection;

typedef void (*ProtobufC_RPC_Client_Connect_Func) (ProtobufC_RPC_Server *server,
                                                   ProtobufC_RPC_ServerConnection *conn,
                                                   void *user_data);

void
rig_protobuf_c_rpc_server_set_client_connect_handler (ProtobufC_RPC_Server *server,
                                                      ProtobufC_RPC_Client_Connect_Func callback,
                                                      void *user_data);

typedef void (*ProtobufC_RPC_Client_Close_Func) (ProtobufC_RPC_Server *server,
                                                 ProtobufC_RPC_ServerConnection *conn,
                                                 void *user_data);

void
rig_protobuf_c_rpc_server_set_client_close_handler (ProtobufC_RPC_Server *server,
                                                    ProtobufC_RPC_Client_Close_Func callback,
                                                    void *user_data);

typedef void (*ProtobufC_RPC_ServerConnection_Close_Func) (ProtobufC_RPC_ServerConnection *conn,
                                                           void *user_data);

void
rig_protobuf_c_rpc_server_connection_set_close_handler (ProtobufC_RPC_ServerConnection *conn,
                                                        ProtobufC_RPC_ServerConnection_Close_Func func,
                                                        void *user_data);

typedef void (*ProtobufC_RPC_ServerConnection_Error_Func) (ProtobufC_RPC_ServerConnection *conn,
                                                           ProtobufC_RPC_Error_Code error,
                                                           const char *message,
                                                           void *user_data);

void
rig_protobuf_c_rpc_server_connection_set_error_handler (ProtobufC_RPC_ServerConnection *conn,
                                                        ProtobufC_RPC_ServerConnection_Error_Func func,
                                                        void *user_data);

ProtobufCService *
     rig_protobuf_c_rpc_server_destroy (ProtobufC_RPC_Server     *server,
                                        protobuf_c_boolean        free_underlying_service);

/* NOTE: these do not have guaranteed semantics if called after there are actually
   clients connected to the server!
   NOTE 2:  The purist in me has left the default of no-autotimeout.
   The pragmatist in me knows thats going to be a pain for someone.
   Please set autotimeout, and if you really don't want it, disable it explicitly,
   because i might just go and make it the default! */
void rig_protobuf_c_rpc_server_disable_autotimeout(ProtobufC_RPC_Server *server);
void rig_protobuf_c_rpc_server_set_autotimeout (ProtobufC_RPC_Server *server,
                                                unsigned              timeout_millis);

typedef protobuf_c_boolean
          (*ProtobufC_RPC_IsRpcThreadFunc) (ProtobufC_RPC_Server *server,
                                            ProtobufCDispatch    *dispatch,
                                            void                 *is_rpc_data);
void rig_protobuf_c_rpc_server_configure_threading (ProtobufC_RPC_Server *server,
                                                    ProtobufC_RPC_IsRpcThreadFunc func,
                                                    void          *is_rpc_data);


/* Error handling */
void rig_protobuf_c_rpc_server_set_error_handler (ProtobufC_RPC_Server *server,
                                                  ProtobufC_RPC_Error_Func func,
                                                  void                 *error_func_data);

#endif
