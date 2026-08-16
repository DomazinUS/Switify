/*
 * --------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <p-sam@d3vs.net>, <natinusala@gmail.com>, <m4x@m4xw.net>
 * wrote the original sys-clk IPC server this is based on. As long as you
 * retain this notice you can do whatever you want with this stuff. If you
 * meet any of them some day, and you think this stuff is worth it, you can
 * buy them a beer in return.
 * --------------------------------------------------------------------------
 */

#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITIFY_IPC_RESPONSE_MAX_DATA_SIZE \
  (0x100u - sizeof(HipcHeader) - 0x10u - sizeof(CmifOutHeader))

typedef struct SwitifyIpcServer {
  SmServiceName service_name;
  Handle handles[MAX_WAIT_OBJECTS];
  uint32_t max_handles;
  uint32_t handle_count;
} SwitifyIpcServer;

typedef struct SwitifyIpcServerRequest {
  HipcParsedRequest hipc;
  uint32_t command_id;
  const void* data;
  size_t data_size;
} SwitifyIpcServerRequest;

typedef Result (*SwitifyIpcServerHandler)(
    void* context, const SwitifyIpcServerRequest* request, uint8_t* output,
    size_t* output_size);

Result switifyIpcServerInitialize(SwitifyIpcServer* server, const char* name,
                                  uint32_t max_sessions);
Result switifyIpcServerExit(SwitifyIpcServer* server);
Result switifyIpcServerProcess(SwitifyIpcServer* server,
                               SwitifyIpcServerHandler handler,
                               void* context);

#ifdef __cplusplus
}
#endif
