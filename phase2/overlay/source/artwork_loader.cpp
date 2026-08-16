#include "artwork_loader.hpp"

#include <curl/curl.h>
#include <switch.h>
#include <turbojpeg.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace switify::artwork {
namespace {

constexpr size_t MaxEncodedBytes = 384 * 1024;
constexpr size_t MaxCacheEntries = 128;
constexpr uint64_t RetryDelayNs = UINT64_C(5000000000);
constexpr const char* CacheDirectory =
    "sdmc:/config/Switify/artwork-cache";
constexpr const char* CacheIndex =
    "sdmc:/config/Switify/artwork-cache/index";

constexpr SocketInitConfig SocketConfig = {
    .tcp_tx_buf_size = 0x800,
    .tcp_rx_buf_size = 0x2000,
    .tcp_tx_buf_max_size = 0x2EE0,
    .tcp_rx_buf_max_size = 0x8000,
    .udp_tx_buf_size = 0,
    .udp_rx_buf_size = 0,
    .sb_efficiency = 2,
    .num_bsd_sessions = 1,
    .bsd_service_type = BsdServiceType_User,
};

std::atomic_bool running{false};
bool threadCreated = false;
bool networkReady = false;
bool socketReady = false;
bool curlReady = false;
Thread workerThread{};
Mutex stateMutex{};
CondVar stateChanged{};

std::string wantedUrl;
int wantedSize = 176;
std::string failedUrl;
uint64_t retryAt = 0;

uint8_t* shownPixels = nullptr;
int shownWidth = 0;
int shownHeight = 0;
std::string shownUrl;

uint8_t* pendingPixels = nullptr;
int pendingWidth = 0;
int pendingHeight = 0;
std::string pendingUrl;

std::string cacheKey(const std::string& url) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (const unsigned char byte : url) {
    hash ^= byte;
    hash *= UINT64_C(1099511628211);
  }
  char encoded[17]{};
  std::snprintf(encoded, sizeof(encoded), "%016llx",
                static_cast<unsigned long long>(hash));
  return encoded;
}

std::string cachePath(const std::string& url) {
  return std::string(CacheDirectory) + "/" + cacheKey(url) + ".img";
}

std::string cacheKeyPath(const std::string& key) {
  return std::string(CacheDirectory) + "/" + key + ".img";
}

bool validCacheKey(const std::string& key) {
  if (key.size() != 16) return false;
  return std::all_of(key.begin(), key.end(), [](unsigned char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f');
  });
}

void touchCache(const std::string& url) {
  std::vector<std::string> order;
  FILE* input = std::fopen(CacheIndex, "r");
  if (input) {
    char line[64]{};
    while (std::fgets(line, sizeof(line), input)) {
      std::string key(line);
      while (!key.empty() && (key.back() == '\r' || key.back() == '\n'))
        key.pop_back();
      if (validCacheKey(key) &&
          std::find(order.begin(), order.end(), key) == order.end()) {
        order.push_back(std::move(key));
        if (order.size() > MaxCacheEntries) order.erase(order.begin());
      }
    }
    std::fclose(input);
  }

  const std::string key = cacheKey(url);
  const auto previous = std::find(order.begin(), order.end(), key);
  if (previous != order.end()) order.erase(previous);
  order.push_back(key);
  while (order.size() > MaxCacheEntries) {
    std::remove(cacheKeyPath(order.front()).c_str());
    order.erase(order.begin());
  }

  const std::string temporary = std::string(CacheIndex) + ".overlay.tmp";
  FILE* output = std::fopen(temporary.c_str(), "w");
  if (!output) return;
  bool written = true;
  for (const auto& item : order)
    if (std::fprintf(output, "%s\n", item.c_str()) < 0) written = false;
  const bool closed = std::fclose(output) == 0;
  if (!written || !closed) {
    std::remove(temporary.c_str());
    return;
  }
  std::remove(CacheIndex);
  if (std::rename(temporary.c_str(), CacheIndex) != 0)
    std::remove(temporary.c_str());
}

bool supportedJpeg(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 &&
         bytes[2] == 0xFF;
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::vector<uint8_t> bytes;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return bytes;
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return bytes;
  }
  const long length = std::ftell(file);
  if (length <= 0 || static_cast<size_t>(length) > MaxEncodedBytes ||
      std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return bytes;
  }
  bytes.resize(static_cast<size_t>(length));
  if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
    bytes.clear();
  std::fclose(file);
  return bytes;
}

void saveFile(const std::string& url, const std::vector<uint8_t>& bytes) {
  if (!supportedJpeg(bytes)) return;
  mkdir("sdmc:/config", 0755);
  mkdir("sdmc:/config/Switify", 0755);
  mkdir(CacheDirectory, 0755);
  const std::string destination = cachePath(url);
  const std::string temporary = destination + ".overlay.tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return;
  const bool written =
      std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool closed = std::fclose(file) == 0;
  if (!written || !closed) {
    std::remove(temporary.c_str());
    return;
  }
  std::remove(destination.c_str());
  if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
    std::remove(temporary.c_str());
    return;
  }
  touchCache(url);
}

size_t receiveBytes(char* data, size_t size, size_t count, void* userData) {
  const size_t length = size * count;
  auto& bytes = *static_cast<std::vector<uint8_t>*>(userData);
  if (length > MaxEncodedBytes || bytes.size() > MaxEncodedBytes - length)
    return 0;
  const auto* begin = reinterpret_cast<const uint8_t*>(data);
  bytes.insert(bytes.end(), begin, begin + length);
  return length;
}

int transferProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return running.load() ? 0 : 1;
}

std::vector<uint8_t> download(const std::string& url) {
  std::vector<uint8_t> bytes;
  if (!networkReady || !running.load()) return bytes;
  CURL* curl = curl_easy_init();
  if (!curl) return bytes;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveBytes);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transferProgress);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK || status < 200 || status >= 300 ||
      !supportedJpeg(bytes))
    bytes.clear();
  return bytes;
}

uint8_t* decodeJpeg(const std::vector<uint8_t>& bytes, int targetSize,
                    int* width, int* height) {
  *width = 0;
  *height = 0;
  if (!supportedJpeg(bytes)) return nullptr;

  tjhandle decoder = tjInitDecompress();
  if (!decoder) return nullptr;
  int sourceWidth = 0;
  int sourceHeight = 0;
  int subsampling = 0;
  int colorspace = 0;
  if (tjDecompressHeader3(decoder, bytes.data(), bytes.size(), &sourceWidth,
                          &sourceHeight, &subsampling, &colorspace) != 0) {
    tjDestroy(decoder);
    return nullptr;
  }

  int factorCount = 0;
  const tjscalingfactor* factors = tjGetScalingFactors(&factorCount);
  int outputWidth = sourceWidth;
  int outputHeight = sourceHeight;
  int bestPixels = 0;
  int smallestPixels = sourceWidth * sourceHeight;
  int smallestWidth = sourceWidth;
  int smallestHeight = sourceHeight;
  for (int index = 0; index < factorCount; ++index) {
    const int scaledWidth = TJSCALED(sourceWidth, factors[index]);
    const int scaledHeight = TJSCALED(sourceHeight, factors[index]);
    const int pixels = scaledWidth * scaledHeight;
    if (pixels < smallestPixels) {
      smallestPixels = pixels;
      smallestWidth = scaledWidth;
      smallestHeight = scaledHeight;
    }
    if (scaledWidth > targetSize || scaledHeight > targetSize) continue;
    if (pixels > bestPixels) {
      bestPixels = pixels;
      outputWidth = scaledWidth;
      outputHeight = scaledHeight;
    }
  }
  if (bestPixels == 0) {
    outputWidth = smallestWidth;
    outputHeight = smallestHeight;
  }

  const size_t outputBytes =
      static_cast<size_t>(outputWidth) * outputHeight * 4;
  auto* pixels = static_cast<uint8_t*>(std::malloc(outputBytes));
  if (!pixels) {
    tjDestroy(decoder);
    return nullptr;
  }
  if (tjDecompress2(decoder, bytes.data(), bytes.size(), pixels, outputWidth,
                    0, outputHeight, TJPF_RGBA, 0) != 0) {
    std::free(pixels);
    pixels = nullptr;
  } else {
    *width = outputWidth;
    *height = outputHeight;
  }
  tjDestroy(decoder);
  return pixels;
}

void worker(void*) {
  while (running.load()) {
    std::string job;
    int targetSize = 176;
    mutexLock(&stateMutex);
    while (running.load()) {
      const uint64_t now = armTicksToNs(armGetSystemTick());
      const bool alreadyAvailable =
          !wantedUrl.empty() &&
          (wantedUrl == shownUrl || wantedUrl == pendingUrl);
      const bool coolingDown =
          wantedUrl == failedUrl && now < retryAt;
      if (!wantedUrl.empty() && !alreadyAvailable && !coolingDown) break;
      if (coolingDown)
        condvarWaitTimeout(&stateChanged, &stateMutex, retryAt - now);
      else
        condvarWait(&stateChanged, &stateMutex);
    }
    if (!running.load()) {
      mutexUnlock(&stateMutex);
      return;
    }
    job = wantedUrl;
    targetSize = wantedSize;
    mutexUnlock(&stateMutex);

    std::vector<uint8_t> encoded = readFile(cachePath(job));
    if (!supportedJpeg(encoded)) {
      encoded = download(job);
      if (!encoded.empty()) saveFile(job, encoded);
    } else {
      touchCache(job);
    }
    int width = 0;
    int height = 0;
    uint8_t* decoded = decodeJpeg(encoded, targetSize, &width, &height);

    mutexLock(&stateMutex);
    if (job == wantedUrl && decoded) {
      std::free(pendingPixels);
      pendingPixels = decoded;
      pendingWidth = width;
      pendingHeight = height;
      pendingUrl = job;
      failedUrl.clear();
      retryAt = 0;
    } else {
      std::free(decoded);
      if (job == wantedUrl) {
        failedUrl = job;
        retryAt = armTicksToNs(armGetSystemTick()) + RetryDelayNs;
      }
    }
    mutexUnlock(&stateMutex);
  }
}

}  // namespace

bool initialize() {
  if (threadCreated) return true;
  mutexInit(&stateMutex);
  condvarInit(&stateChanged);
  socketReady = R_SUCCEEDED(socketInitialize(&SocketConfig));
  if (socketReady)
    curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  networkReady = socketReady && curlReady;
  running = true;
  Result result = threadCreate(&workerThread, worker, nullptr, nullptr, 0x6000,
                               0x3B, -2);
  const bool created = R_SUCCEEDED(result);
  if (R_SUCCEEDED(result)) result = threadStart(&workerThread);
  if (R_FAILED(result)) {
    running = false;
    if (created) threadClose(&workerThread);
    if (curlReady) {
      curl_global_cleanup();
      curlReady = false;
    }
    if (socketReady) {
      socketExit();
      socketReady = false;
    }
    networkReady = false;
    return false;
  }
  threadCreated = true;
  return true;
}

void shutdown() {
  if (!threadCreated) return;
  running = false;
  mutexLock(&stateMutex);
  condvarWakeAll(&stateChanged);
  mutexUnlock(&stateMutex);
  threadWaitForExit(&workerThread);
  threadClose(&workerThread);
  threadCreated = false;
  std::free(shownPixels);
  std::free(pendingPixels);
  shownPixels = nullptr;
  pendingPixels = nullptr;
  shownUrl.clear();
  pendingUrl.clear();
  wantedUrl.clear();
  if (curlReady) {
    curl_global_cleanup();
    curlReady = false;
  }
  if (socketReady) {
    socketExit();
    socketReady = false;
  }
  networkReady = false;
}

void request(const std::string& url, int targetSize) {
  if (!threadCreated) return;
  mutexLock(&stateMutex);
  const bool changed = wantedUrl != url;
  wantedUrl = url;
  wantedSize = std::clamp(targetSize, 64, 256);
  if (changed) {
    failedUrl.clear();
    retryAt = 0;
    condvarWakeAll(&stateChanged);
  }
  mutexUnlock(&stateMutex);
}

void borrow(const uint8_t** pixels, int* width, int* height) {
  if (pixels) *pixels = nullptr;
  if (width) *width = 0;
  if (height) *height = 0;
  if (!threadCreated) return;

  mutexLock(&stateMutex);
  if (!pendingUrl.empty() && pendingUrl == wantedUrl) {
    std::free(shownPixels);
    shownPixels = pendingPixels;
    shownWidth = pendingWidth;
    shownHeight = pendingHeight;
    shownUrl = pendingUrl;
    pendingPixels = nullptr;
    pendingWidth = 0;
    pendingHeight = 0;
    pendingUrl.clear();
  }
  if (!wantedUrl.empty() && shownUrl == wantedUrl) {
    if (pixels) *pixels = shownPixels;
    if (width) *width = shownWidth;
    if (height) *height = shownHeight;
  }
  mutexUnlock(&stateMutex);
}

}  // namespace switify::artwork
