// Nintendo Switch (libnx) implementation of bell::WrappedSemaphore.
// Uses std::mutex + std::condition_variable, which devkitA64's gcc supports,
// instead of POSIX semaphores (not provided by newlib on this target).

#include "WrappedSemaphore.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace bell;

namespace {
struct NxSemaphore {
  std::mutex mtx;
  std::condition_variable cv;
  int count = 0;
};
}  // namespace

WrappedSemaphore::WrappedSemaphore(int maxVal) {
  this->semaphoreHandle = new NxSemaphore();
}

WrappedSemaphore::~WrappedSemaphore() {
  delete static_cast<NxSemaphore*>(this->semaphoreHandle);
}

int WrappedSemaphore::wait() {
  auto* sem = static_cast<NxSemaphore*>(this->semaphoreHandle);
  std::unique_lock<std::mutex> lock(sem->mtx);
  sem->cv.wait(lock, [sem] { return sem->count > 0; });
  sem->count--;
  return 0;
}

int WrappedSemaphore::twait(long milliseconds) {
  auto* sem = static_cast<NxSemaphore*>(this->semaphoreHandle);
  std::unique_lock<std::mutex> lock(sem->mtx);
  bool ok = sem->cv.wait_for(lock, std::chrono::milliseconds(milliseconds),
                             [sem] { return sem->count > 0; });
  if (!ok)
    return -1;  // timeout, same contract as sem_timedwait
  sem->count--;
  return 0;
}

void WrappedSemaphore::give() {
  auto* sem = static_cast<NxSemaphore*>(this->semaphoreHandle);
  {
    std::lock_guard<std::mutex> lock(sem->mtx);
    sem->count++;
  }
  sem->cv.notify_one();
}
