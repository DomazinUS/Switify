#pragma once

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#elif __APPLE__
#include <dispatch/dispatch.h>  // for dispatch_semaphore_t
#elif _WIN32
#include <winsock2.h>
#elif defined(__SWITCH__)
// Nintendo Switch (libnx/devkitA64): implemented with std::mutex +
// std::condition_variable in platform/nx/WrappedSemaphore.cpp to avoid
// depending on POSIX semaphores in newlib.
#else
#include <semaphore.h>
#include <time.h>
#endif

namespace bell {

class WrappedSemaphore {
 private:
#ifdef ESP_PLATFORM
  SemaphoreHandle_t semaphoreHandle;
#elif __APPLE__
  dispatch_semaphore_t semaphoreHandle;
#elif _WIN32
  HANDLE semaphoreHandle;
#elif defined(__SWITCH__)
  void* semaphoreHandle;  // opaque, see platform/nx/WrappedSemaphore.cpp
#else
  sem_t semaphoreHandle;
#endif

 public:
  WrappedSemaphore(int maxVal = 200);
  ~WrappedSemaphore();

  int wait();
  int twait(long milliseconds = 10);
  void give();
};

}  // namespace bell
