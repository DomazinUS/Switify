#pragma once

#include <switch.h>

namespace switify::phase2::ipc_self_test {

// Starts a one-shot client inside the sysmodule. The main server loop services
// this request, proving service registration and CMIF marshalling without any
// dependency on Ultrahand or the overlay process.
Result start();

}  // namespace switify::phase2::ipc_self_test
