#pragma once

#include "switify/service_core.hpp"

#include <switch.h>

namespace switify::phase2::switch_service {

Result initialize(ServiceCore& core);
Result exit();
void loop();

}  // namespace switify::phase2::switch_service

