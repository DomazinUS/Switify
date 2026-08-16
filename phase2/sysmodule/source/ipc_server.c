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

#include "ipc_server.h"

#include <stdio.h>
#include <string.h>

#define SWITIFY_IPC_TLS_BUFFER_SIZE 0x100u
#define SWITIFY_IPC_TRACE_PATH \
  "sdmc:/config/switify-poc/phase2-service.log"

typedef struct SwitifyIpcRawHeader {
  uint64_t magic;
  union {
    uint64_t command_id;
    uint64_t result;
  };
} SwitifyIpcRawHeader;

static FILE* g_trace_file;

static void traceOpen(void) {
  g_trace_file = fopen(SWITIFY_IPC_TRACE_PATH, "w");
  if (g_trace_file) {
    fputs("start\n", g_trace_file);
    fflush(g_trace_file);
  }
}

static void traceResult(const char* stage, Result result) {
  if (!g_trace_file) return;
  fprintf(g_trace_file, "%s 0x%08X\n", stage, result);
  fflush(g_trace_file);
}

static void traceClose(void) {
  if (!g_trace_file) return;
  fclose(g_trace_file);
  g_trace_file = NULL;
}

static void clearMessageBuffer(void) {
  memset(armGetTls(), 0, SWITIFY_IPC_TLS_BUFFER_SIZE);
}

static Result addSession(SwitifyIpcServer* server, Handle session) {
  if (server->handle_count >= server->max_handles)
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  server->handles[server->handle_count++] = session;
  return 0;
}

static Result removeSession(SwitifyIpcServer* server, uint32_t index) {
  if (index == 0 || index >= server->handle_count)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);

  svcCloseHandle(server->handles[index]);
  for (uint32_t i = index; i + 1 < server->handle_count; ++i)
    server->handles[i] = server->handles[i + 1];
  --server->handle_count;
  return 0;
}

Result switifyIpcServerInitialize(SwitifyIpcServer* server, const char* name,
                                  uint32_t max_sessions) {
  if (!server || !name || max_sessions < 1 ||
      max_sessions > MAX_WAIT_OBJECTS - 1)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);

  memset(server, 0, sizeof(*server));
  traceOpen();
  server->service_name = smEncodeName(name);
  server->max_handles = max_sessions + 1;

  /*
   * Publish through the Horizon service manager. The old health builds used
   * a raw kernel named port on both ends; that bypassed the service ACL in the
   * NPDM and repeatedly failed at the first synchronous request on hardware.
   * Explicitly select CMIF because the commands below use CMIF framing.
   */
  Result result = smRegisterServiceCmif(&server->handles[0],
                                        server->service_name, false,
                                        (int32_t)max_sessions);
  if (R_SUCCEEDED(result)) server->handle_count = 1;
  traceResult("sm-register", result);
  return result;
}

Result switifyIpcServerExit(SwitifyIpcServer* server) {
  if (!server) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

  const Result result = smUnregisterServiceCmif(server->service_name);
  for (uint32_t i = 0; i < server->handle_count; ++i)
    svcCloseHandle(server->handles[i]);
  server->handle_count = 0;
  traceResult("sm-unregister", result);
  traceClose();
  return result;
}

static Result parseRequest(SwitifyIpcServerRequest* request) {
  void* tls = armGetTls();
  request->hipc = hipcParseRequest(tls);
  request->command_id = 0;
  request->data = NULL;
  request->data_size = 0;

  if (request->hipc.meta.type != CmifCommandType_Request &&
      request->hipc.meta.type != CmifCommandType_Control)
    return 0;

  const size_t raw_size = request->hipc.meta.num_data_words * sizeof(uint32_t);
  const SwitifyIpcRawHeader* header =
      (const SwitifyIpcRawHeader*)cmifGetAlignedDataStart(
          request->hipc.data.data_words, tls);
  if (!header || raw_size < sizeof(*header) ||
      (uint32_t)header->magic != CMIF_IN_HEADER_MAGIC)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);

  request->command_id = (uint32_t)header->command_id;
  if (raw_size > sizeof(*header)) {
    request->data = header + 1;
    request->data_size = raw_size - sizeof(*header);
  }
  return 0;
}

static void prepareResponse(Result result, const void* data, size_t data_size) {
  void* tls = armGetTls();
  /*
   * A server reply is itself a new HIPC message. Do not leave any descriptor
   * or receive-list bits from the request in the TLS command buffer: current
   * Horizon kernels reject that combination before CMIF is parsed.
   */
  clearMessageBuffer();
  HipcRequest hipc = hipcMakeRequestInline(
      tls, .type = CmifCommandType_Request,
      .num_data_words =
          (0x10u + sizeof(SwitifyIpcRawHeader) + data_size + 3u) / 4u);
  SwitifyIpcRawHeader* header =
      (SwitifyIpcRawHeader*)cmifGetAlignedDataStart(hipc.data_words, tls);
  header->magic = CMIF_OUT_HEADER_MAGIC;
  header->result = result;
  if (R_SUCCEEDED(result) && data_size != 0)
    memcpy(header + 1, data, data_size);
}

static Result acceptSession(SwitifyIpcServer* server) {
  Handle session = INVALID_HANDLE;
  Result result = svcAcceptSession(&session, server->handles[0]);
  if (R_SUCCEEDED(result)) {
    result = addSession(server, session);
    if (R_FAILED(result)) svcCloseHandle(session);
  }
  traceResult("accept", result);
  return result;
}

static Result processSession(SwitifyIpcServer* server,
                             SwitifyIpcServerHandler handler, void* context,
                             uint32_t index) {
  s32 unused = -1;
  SwitifyIpcServerRequest request;
  size_t output_size = 0;
  uint8_t output[SWITIFY_IPC_RESPONSE_MAX_DATA_SIZE] = {0};
  bool close_session = false;

  /*
   * svcReplyAndReceive treats the existing TLS message as the receive
   * template. A prior libnx service call can leave descriptor metadata there,
   * which makes even a descriptor-free Ping fail with Kernel
   * InvalidCombination (0xE801). Start every receive from a blank template.
   */
  clearMessageBuffer();
  Result result = svcReplyAndReceive(&unused, &server->handles[index], 1, 0,
                                     UINT64_MAX);
  if (R_FAILED(result)) {
    traceResult("receive", result);
  } else {
    result = parseRequest(&request);
    if (R_FAILED(result)) traceResult("parse", result);
  }

  if (R_SUCCEEDED(result)) {
    if (request.hipc.meta.type == CmifCommandType_Request) {
      Result command_result = handler(context, &request, output, &output_size);
      if (output_size > sizeof(output)) {
        output_size = 0;
        command_result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
      }
      prepareResponse(command_result, output, output_size);
    } else if (request.hipc.meta.type == CmifCommandType_Control) {
      if (request.command_id == 3u) {
        /* libnx Service initialization queries the server pointer-buffer size. */
        const uint16_t pointer_buffer_size = 0;
        prepareResponse(0, &pointer_buffer_size,
                        sizeof(pointer_buffer_size));
      } else {
        prepareResponse(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL,
                        0);
      }
    } else if (request.hipc.meta.type == CmifCommandType_Close) {
      prepareResponse(0, NULL, 0);
      close_session = true;
    } else {
      prepareResponse(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
    }

    result = svcReplyAndReceive(&unused, &server->handles[index], 0,
                                server->handles[index], 0);
    if (result == KERNELRESULT(TimedOut)) result = 0;
    traceResult("reply", result);
  }

  if (R_FAILED(result) || close_session) removeSession(server, index);
  return result;
}

Result switifyIpcServerProcess(SwitifyIpcServer* server,
                               SwitifyIpcServerHandler handler,
                               void* context) {
  if (!server || !handler || server->handle_count == 0)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);

  s32 index = -1;
  Result result = svcWaitSynchronization(&index, server->handles,
                                         server->handle_count, UINT64_MAX);
  if (R_FAILED(result)) return result;
  if (index < 0 || (uint32_t)index >= server->handle_count)
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
  return index == 0 ? acceptSession(server)
                    : processSession(server, handler, context, (uint32_t)index);
}
