#include "ui_helpers.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "HTTPClient.h"
#include "NXConsoleLog.h"
#include <borealis/core/cache_helper.hpp>

namespace swui {
namespace {
std::mutex imageCacheMutex;
std::unordered_map<std::string, std::string> imageCache;
std::deque<std::string> imageCacheOrder;
std::deque<std::string> persistentImageCacheOrder;
bool persistentImageCacheLoaded = false;
std::mutex imageRequestMutex;
std::unordered_map<brls::Image*, std::string> imageRequests;
constexpr size_t kImageCacheEntries = 48;
constexpr size_t kPersistentImageCacheEntries = 128;
constexpr size_t kMaxEncodedImageBytes = 1024 * 1024;

#ifdef __SWITCH__
constexpr const char* kPersistentImageCacheDirectory =
    "/config/Switify/artwork-cache";
#else
constexpr const char* kPersistentImageCacheDirectory =
    "switify-artwork-cache";
#endif

std::string persistentCacheKey(const std::string& url) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : url) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  char encoded[17] = {};
  std::snprintf(encoded, sizeof(encoded), "%016llx",
                static_cast<unsigned long long>(hash));
  return encoded;
}

std::string persistentCachePath(const std::string& key) {
  return std::string(kPersistentImageCacheDirectory) + "/" + key + ".img";
}

std::string persistentCacheIndexPath() {
  return std::string(kPersistentImageCacheDirectory) + "/index";
}

void ensurePersistentCacheDirectory() {
#ifdef __SWITCH__
  mkdir("/config", 0755);
  mkdir("/config/Switify", 0755);
  mkdir(kPersistentImageCacheDirectory, 0755);
#elif defined(_WIN32)
  _mkdir(kPersistentImageCacheDirectory);
#else
  mkdir(kPersistentImageCacheDirectory, 0755);
#endif
}

bool isSupportedImageData(const std::string& data) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  const bool jpeg = data.size() >= 3 && bytes[0] == 0xff &&
                    bytes[1] == 0xd8 && bytes[2] == 0xff;
  const bool png = data.size() >= 8 && bytes[0] == 0x89 &&
                   bytes[1] == 0x50 && bytes[2] == 0x4e &&
                   bytes[3] == 0x47 && bytes[4] == 0x0d &&
                   bytes[5] == 0x0a && bytes[6] == 0x1a &&
                   bytes[7] == 0x0a;
  return jpeg || png;
}

void savePersistentCacheIndexLocked() {
  if (!persistentImageCacheLoaded) return;
  ensurePersistentCacheDirectory();
  const std::string path = persistentCacheIndexPath();
  const std::string temporary = path + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output.good()) return;
  for (const auto& key : persistentImageCacheOrder) output << key << '\n';
  output.close();
  if (!output.good()) {
    std::remove(temporary.c_str());
    return;
  }
  std::remove(path.c_str());
  if (std::rename(temporary.c_str(), path.c_str()) != 0)
    std::remove(temporary.c_str());
}

void loadPersistentCacheIndexLocked() {
  if (persistentImageCacheLoaded) return;
  persistentImageCacheLoaded = true;
  ensurePersistentCacheDirectory();
  std::ifstream input(persistentCacheIndexPath());
  std::string key;
  while (std::getline(input, key)) {
    if (!key.empty() && key.back() == '\r') key.pop_back();
    if (key.size() != 16 ||
        key.find_first_not_of("0123456789abcdef") != std::string::npos)
      continue;
    if (std::find(persistentImageCacheOrder.begin(),
                  persistentImageCacheOrder.end(), key) !=
        persistentImageCacheOrder.end())
      continue;
    std::ifstream file(persistentCachePath(key), std::ios::binary);
    if (file.good()) persistentImageCacheOrder.push_back(key);
  }
  while (persistentImageCacheOrder.size() > kPersistentImageCacheEntries) {
    const std::string oldest = persistentImageCacheOrder.front();
    persistentImageCacheOrder.pop_front();
    std::remove(persistentCachePath(oldest).c_str());
  }
}

void touchPersistentCacheKeyLocked(const std::string& key) {
  const auto found = std::find(persistentImageCacheOrder.begin(),
                               persistentImageCacheOrder.end(), key);
  if (found != persistentImageCacheOrder.end())
    persistentImageCacheOrder.erase(found);
  persistentImageCacheOrder.push_back(key);
}

void removePersistentImageLocked(const std::string& url) {
  loadPersistentCacheIndexLocked();
  const std::string key = persistentCacheKey(url);
  const auto found = std::find(persistentImageCacheOrder.begin(),
                               persistentImageCacheOrder.end(), key);
  if (found != persistentImageCacheOrder.end())
    persistentImageCacheOrder.erase(found);
  std::remove(persistentCachePath(key).c_str());
  imageCache.erase(url);
  const auto memoryOrder =
      std::find(imageCacheOrder.begin(), imageCacheOrder.end(), url);
  if (memoryOrder != imageCacheOrder.end()) imageCacheOrder.erase(memoryOrder);
  savePersistentCacheIndexLocked();
}

std::string readPersistentImageLocked(const std::string& url) {
  loadPersistentCacheIndexLocked();
  const std::string key = persistentCacheKey(url);
  const std::string path = persistentCachePath(key);
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input.good()) return {};
  const std::streamoff length = input.tellg();
  if (length <= 0 ||
      length > static_cast<std::streamoff>(kMaxEncodedImageBytes)) {
    removePersistentImageLocked(url);
    return {};
  }
  std::string data(static_cast<size_t>(length), '\0');
  input.seekg(0);
  input.read(data.data(), length);
  if (!input.good() || !isSupportedImageData(data)) {
    removePersistentImageLocked(url);
    return {};
  }
  touchPersistentCacheKeyLocked(key);
  return data;
}

void writePersistentImageLocked(const std::string& url,
                                const std::string& data) {
  if (data.empty() || data.size() > kMaxEncodedImageBytes ||
      !isSupportedImageData(data))
    return;
  loadPersistentCacheIndexLocked();
  const std::string key = persistentCacheKey(url);
  const std::string path = persistentCachePath(key);
  const std::string temporary = path + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output.good()) return;
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  output.close();
  if (!output.good()) {
    std::remove(temporary.c_str());
    return;
  }
  std::remove(path.c_str());
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    return;
  }
  touchPersistentCacheKeyLocked(key);
  while (persistentImageCacheOrder.size() > kPersistentImageCacheEntries) {
    const std::string oldest = persistentImageCacheOrder.front();
    persistentImageCacheOrder.pop_front();
    std::remove(persistentCachePath(oldest).c_str());
  }
  savePersistentCacheIndexLocked();
}

void storeMemoryImageLocked(const std::string& url, const std::string& data) {
  if (data.empty() || imageCache.find(url) != imageCache.end()) return;
  while (imageCacheOrder.size() >= kImageCacheEntries) {
    imageCache.erase(imageCacheOrder.front());
    imageCacheOrder.pop_front();
  }
  imageCacheOrder.push_back(url);
  imageCache.emplace(url, data);
}

class ImageWorkerPool {
 public:
  ImageWorkerPool() {
    for (int index = 0; index < 4; ++index)
      workers.emplace_back([this] { run(); });
  }

  void submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopping) return;
      tasks.push(std::move(task));
    }
    wake.notify_one();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopping) return;
      stopping = true;
      std::queue<std::function<void()>> empty;
      tasks.swap(empty);
    }
    wake.notify_all();
    for (auto& worker : workers) {
      if (worker.joinable()) worker.join();
    }
    workers.clear();
  }

 private:
  void run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [this] { return stopping || !tasks.empty(); });
        if (stopping && tasks.empty()) return;
        task = std::move(tasks.front());
        tasks.pop();
      }
      task();
    }
  }

  std::mutex mutex;
  std::condition_variable wake;
  std::queue<std::function<void()>> tasks;
  std::vector<std::thread> workers;
  bool stopping = false;
};

std::mutex poolMutex;
std::unique_ptr<ImageWorkerPool> pool;

ImageWorkerPool* imageWorkers() {
  std::lock_guard<std::mutex> lock(poolMutex);
  if (!pool) pool = std::make_unique<ImageWorkerPool>();
  return pool.get();
}
}  // namespace

NVGcolor background() { return nvgRGB(10, 12, 15); }
NVGcolor surface() { return nvgRGB(22, 25, 30); }
NVGcolor surfaceRaised() { return nvgRGB(31, 35, 42); }
NVGcolor textPrimary() { return nvgRGB(246, 247, 249); }
NVGcolor textSecondary() { return nvgRGB(168, 175, 186); }
NVGcolor accent() { return nvgRGB(30, 215, 96); }
NVGcolor danger() { return nvgRGB(225, 90, 100); }

std::string formatDuration(uint32_t milliseconds) {
  const uint32_t seconds = milliseconds / 1000;
  return std::to_string(seconds / 60) + ":" +
         (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
}

void loadRemoteImage(brls::Image* image, const std::string& url,
                     int preferredSize) {
  if (!image || url.empty()) return;
  std::string resolvedUrl = url;
  const auto imagePath = url.find("/image/");
  // Small cards use Spotify's 300 px mosaic endpoint to avoid oversized
  // downloads. Large player/detail artwork must retain the original selected
  // CDN source; forcing it through /300 was the reason podcast banners stayed
  // visibly pixelated even after selecting a larger source URL.
  if (preferredSize <= 300 && imagePath != std::string::npos &&
      (url.find("spotifycdn.com") != std::string::npos ||
       url.find("i.scdn.co") != std::string::npos)) {
    const std::string imageId = url.substr(imagePath + 7);
    if (!imageId.empty() && imageId.find_first_not_of(
                                "0123456789abcdefABCDEF") == std::string::npos)
      resolvedUrl = "https://mosaic.scdn.co/300/" + imageId;
  }
  image->setScalingType(brls::ImageScalingType::FIT);
  const int cachedTexture =
      brls::TextureCache::instance().getCache(resolvedUrl);
  if (cachedTexture > 0) {
    // Invalidate an older in-flight request for this view before showing the
    // cache hit, otherwise the older download could overwrite the new art.
    {
      std::lock_guard<std::mutex> lock(imageRequestMutex);
      imageRequests.erase(image);
    }
    image->setFreeTexture(false);
    image->innerSetImage(cachedTexture);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(imageRequestMutex);
    imageRequests[image] = resolvedUrl;
  }
  image->ptrLock();
  imageWorkers()->submit([image, url = std::move(resolvedUrl)]() mutable {
      std::string data;
      {
        std::lock_guard<std::mutex> lock(imageCacheMutex);
        const auto cached = imageCache.find(url);
        if (cached != imageCache.end()) data = cached->second;
        if (data.empty()) {
          data = readPersistentImageLocked(url);
          if (!data.empty()) storeMemoryImageLocked(url, data);
        }
      }
      if (data.empty()) {
        try {
          // Borealis' stb_image build handles JPEG/PNG but not negotiated
          // WebP/AVIF. Spotify's image CDN honors this explicit Accept list.
          auto response = bell::HTTPClient::get(
              url, {{"Accept", "image/jpeg, image/png;q=0.9"}});
          if (response->statusCode() >= 200 && response->statusCode() < 300) {
            const auto bytes = response->bytes();
            data.assign(bytes.begin(), bytes.end());
            if (!data.empty() && data.size() <= kMaxEncodedImageBytes &&
                isSupportedImageData(data)) {
              std::lock_guard<std::mutex> lock(imageCacheMutex);
              storeMemoryImageLocked(url, data);
              writePersistentImageLocked(url, data);
            } else {
              data.clear();
            }
          }
        } catch (const std::exception& error) {
          // Borealis writes to stdout; libnx's console output is not safe from
          // worker threads. Route failures through the app's queued logger.
          nxlog::pushf("Artwork request failed: %s", error.what());
        }
      }
      brls::sync([image, url, data = std::move(data)] {
        bool currentRequest = false;
        {
          std::lock_guard<std::mutex> lock(imageRequestMutex);
          const auto request = imageRequests.find(image);
          currentRequest = request != imageRequests.end() && request->second == url;
          if (currentRequest) imageRequests.erase(request);
        }
        if (currentRequest && !data.empty()) {
          int texture = brls::TextureCache::instance().getCache(url);
          if (texture == 0) {
            texture = nvgCreateImageMem(
                brls::Application::getNVGContext(), 0,
                reinterpret_cast<unsigned char*>(
                    const_cast<char*>(data.data())),
                static_cast<int>(data.size()));
            if (texture > 0)
              brls::TextureCache::instance().addCache(url, texture);
            else
              nxlog::pushf("Artwork decode failed (%d bytes): %s",
                           static_cast<int>(data.size()), url.c_str());
          }
          if (texture > 0) {
            image->setFreeTexture(false);
            image->innerSetImage(texture);
          } else {
            std::lock_guard<std::mutex> lock(imageCacheMutex);
            removePersistentImageLocked(url);
          }
        }
        image->ptrUnlock();
      });
  });
}

void shutdownRemoteImages() {
  std::unique_ptr<ImageWorkerPool> active;
  {
    std::lock_guard<std::mutex> lock(poolMutex);
    active = std::move(pool);
  }
  if (active) active->stop();
  {
    std::lock_guard<std::mutex> lock(imageCacheMutex);
    savePersistentCacheIndexLocked();
  }
}

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color,
                       bool singleLine) {
  auto* label = new brls::Label();
  label->setText(text);
  label->setFontSize(size);
  label->setTextColor(color);
  label->setSingleLine(singleLine);
  return label;
}

brls::Box* makeActionButton(const std::string& text,
                            std::function<bool(brls::View*)> action,
                            bool primary, brls::Label** labelOut) {
  auto* button = new brls::Box();
  button->setFocusable(true);
  button->setAxis(brls::Axis::ROW);
  button->setAlignItems(brls::AlignItems::CENTER);
  button->setJustifyContent(brls::JustifyContent::CENTER);
  button->setPadding(10, 18, 10, 18);
  button->setCornerRadius(9);
  button->setHighlightCornerRadius(9);
  button->setBackgroundColor(primary ? accent() : surfaceRaised());
  button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
  auto* label = makeLabel(text, 18, primary ? nvgRGB(8, 20, 12) : textPrimary(),
                          true);
  button->addView(label);
  if (primary) {
    // Borealis draws a dark focus fill over custom primary buttons. Switch the
    // label to white while focused, then restore the high-contrast dark text
    // on Spotify green when focus leaves.
    button->getFocusEvent()->subscribe(
        [label](brls::View*) { label->setTextColor(textPrimary()); });
    button->getFocusLostEvent()->subscribe([label](brls::View*) {
      label->setTextColor(nvgRGB(8, 20, 12));
    });
  }
  button->registerClickAction(std::move(action));
  if (labelOut) *labelOut = label;
  return button;
}

FilterChip::FilterChip(const std::string& text, std::function<void()> action) {
  setFocusable(true);
  setAxis(brls::Axis::ROW);
  setAlignItems(brls::AlignItems::CENTER);
  setJustifyContent(brls::JustifyContent::CENTER);
  setHeight(44);
  setPadding(8, 20, 8, 20);
  setCornerRadius(22);
  setHighlightCornerRadius(22);
  addGestureRecognizer(new brls::TapGestureRecognizer(this));
  label = makeLabel(text, 17, textPrimary(), true);
  addView(label);
  registerClickAction([action = std::move(action)](brls::View*) {
    action();
    return true;
  });
  getFocusEvent()->subscribe(
      [this](brls::View*) { applyStyle(true); });
  getFocusLostEvent()->subscribe(
      [this](brls::View*) { applyStyle(false); });
  applyStyle(false);
}

void FilterChip::setSelected(bool nextSelected) {
  selected = nextSelected;
  applyStyle(isFocused());
}

void FilterChip::applyStyle(bool focused) {
  setBackgroundColor(selected ? accent() : surfaceRaised());
  label->setTextColor(selected && !focused ? nvgRGB(8, 20, 12)
                                           : textPrimary());
}

RepeatIcon::RepeatIcon() {
  setDimensions(34, 34);
  setFocusable(false);
}

void RepeatIcon::setMode(SpotifyEngine::RepeatMode nextMode) {
  if (mode == nextMode) return;
  mode = nextMode;
}

void RepeatIcon::draw(NVGcontext* vg, float x, float y, float width,
                      float height, brls::Style, brls::FrameContext*) {
  const NVGcolor color = mode == SpotifyEngine::RepeatMode::Off
                             ? textPrimary()
                             : accent();
  const float left = x + 4.0f;
  const float right = x + width - 4.0f;
  const float top = y + 10.0f;
  const float bottom = y + height - 10.0f;

  nvgSave(vg);
  nvgStrokeColor(vg, color);
  nvgStrokeWidth(vg, 2.6f);
  nvgLineCap(vg, NVG_ROUND);
  nvgLineJoin(vg, NVG_ROUND);

  nvgBeginPath(vg);
  nvgMoveTo(vg, left, top);
  nvgLineTo(vg, right - 2.0f, top);
  nvgMoveTo(vg, right - 8.0f, top - 5.0f);
  nvgLineTo(vg, right - 2.0f, top);
  nvgLineTo(vg, right - 8.0f, top + 5.0f);
  nvgStroke(vg);

  nvgBeginPath(vg);
  nvgMoveTo(vg, right, bottom);
  nvgLineTo(vg, left + 2.0f, bottom);
  nvgMoveTo(vg, left + 8.0f, bottom - 5.0f);
  nvgLineTo(vg, left + 2.0f, bottom);
  nvgLineTo(vg, left + 8.0f, bottom + 5.0f);
  nvgStroke(vg);

  if (mode == SpotifyEngine::RepeatMode::One) {
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFontSize(vg, 13.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, color);
    nvgText(vg, x + width / 2.0f, y + height / 2.0f + 0.5f, "1", nullptr);
  }
  nvgRestore(vg);
}

brls::Box* makeRepeatButton(std::function<bool(brls::View*)> action,
                            RepeatIcon** iconOut) {
  auto* button = makeActionButton("", std::move(action));
  button->clearViews();
  auto* icon = new RepeatIcon();
  button->addView(icon);
  if (iconOut) *iconOut = icon;
  return button;
}

ShuffleIcon::ShuffleIcon() {
  setDimensions(34, 34);
  setFocusable(false);
}

void ShuffleIcon::setActive(bool nextActive) { active = nextActive; }

void ShuffleIcon::draw(NVGcontext* vg, float x, float y, float width,
                       float height, brls::Style, brls::FrameContext*) {
  const NVGcolor color = active ? accent() : textPrimary();
  const float left = x + 4.0f;
  const float right = x + width - 4.0f;
  const float top = y + 9.0f;
  const float bottom = y + height - 9.0f;
  const float center = x + width / 2.0f;

  nvgSave(vg);
  nvgStrokeColor(vg, color);
  nvgStrokeWidth(vg, 2.6f);
  nvgLineCap(vg, NVG_ROUND);
  nvgLineJoin(vg, NVG_ROUND);

  // Upper input crosses down to the lower output.
  nvgBeginPath(vg);
  nvgMoveTo(vg, left, top);
  nvgBezierTo(vg, center - 5.0f, top, center + 3.0f, bottom,
              right - 2.0f, bottom);
  nvgMoveTo(vg, right - 8.0f, bottom - 5.0f);
  nvgLineTo(vg, right - 2.0f, bottom);
  nvgLineTo(vg, right - 8.0f, bottom + 5.0f);
  nvgStroke(vg);

  // Lower input crosses up to the upper output.
  nvgBeginPath(vg);
  nvgMoveTo(vg, left, bottom);
  nvgBezierTo(vg, center - 5.0f, bottom, center + 3.0f, top,
              right - 2.0f, top);
  nvgMoveTo(vg, right - 8.0f, top - 5.0f);
  nvgLineTo(vg, right - 2.0f, top);
  nvgLineTo(vg, right - 8.0f, top + 5.0f);
  nvgStroke(vg);
  nvgRestore(vg);
}

brls::Box* makeShuffleButton(std::function<bool(brls::View*)> action,
                             ShuffleIcon** iconOut) {
  auto* button = makeActionButton("", std::move(action));
  button->clearViews();
  auto* icon = new ShuffleIcon();
  button->addView(icon);
  if (iconOut) *iconOut = icon;
  return button;
}

brls::Box* makeStatePanel(const std::string& title,
                          const std::string& detail, NVGcolor tint) {
  auto* panel = new brls::Box();
  panel->setAxis(brls::Axis::COLUMN);
  panel->setPadding(18, 20, 18, 20);
  panel->setCornerRadius(10);
  panel->setBackgroundColor(tint);
  auto* heading = makeLabel(title, 20, textPrimary(), false);
  auto* body = makeLabel(detail, 16, textSecondary(), false);
  body->setMarginTop(6);
  panel->addView(heading);
  panel->addView(body);
  return panel;
}

MediaCard::MediaCard() {
  setAxis(brls::Axis::COLUMN);
  setFocusable(true);
  setDimensions(178, 232);
  setCornerRadius(10);
  setHighlightCornerRadius(10);
  setBackgroundColor(surface());
  setPadding(0, 0, 12, 0);
  addGestureRecognizer(new brls::TapGestureRecognizer(this));

  artwork = new brls::Image();
  artwork->setDimensions(178, 166);
  artwork->setBackgroundColor(surfaceRaised());
  artwork->setCornerRadius(8);
  artwork->setScalingType(brls::ImageScalingType::FIT);
  addView(artwork);

  titleLabel = makeLabel("", 17, textPrimary(), true);
  titleLabel->setMarginTop(10);
  titleLabel->setMarginLeft(11);
  titleLabel->setMarginRight(11);
  addView(titleLabel);

  subtitleLabel = makeLabel("", 14, textSecondary(), true);
  subtitleLabel->setMarginTop(3);
  subtitleLabel->setMarginLeft(11);
  subtitleLabel->setMarginRight(11);
  addView(subtitleLabel);

  registerClickAction([this](brls::View*) {
    if (onSelected) onSelected();
    return true;
  });
}

void MediaCard::setContent(const std::string& title,
                           const std::string& subtitle,
                           const std::string& imageUrl) {
  titleLabel->setText(title);
  subtitleLabel->setText(subtitle);
  if (!imageUrl.empty()) loadRemoteImage(artwork, imageUrl);
}

void MediaCard::setAction(std::function<void()> action) {
  onSelected = std::move(action);
}

TrackRow::TrackRow() {
  setAxis(brls::Axis::ROW);
  setFocusable(true);
  setAlignItems(brls::AlignItems::CENTER);
  setHeight(72);
  setPadding(7, 12, 7, 12);
  setCornerRadius(8);
  setHighlightCornerRadius(8);
  addGestureRecognizer(new brls::TapGestureRecognizer(this));

  indexLabel = makeLabel("", 15, textSecondary(), true);
  indexLabel->setWidth(30);
  indexLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
  addView(indexLabel);

  artwork = new brls::Image();
  artwork->setDimensions(56, 56);
  artwork->setBackgroundColor(surfaceRaised());
  artwork->setCornerRadius(5);
  artwork->setScalingType(brls::ImageScalingType::FIT);
  addView(artwork);

  auto* text = new brls::Box();
  text->setAxis(brls::Axis::COLUMN);
  text->setGrow(1.0f);
  text->setMarginLeft(14);
  titleLabel = makeLabel("", 18, textPrimary(), true);
  subtitleLabel = makeLabel("", 14, textSecondary(), true);
  subtitleLabel->setMarginTop(3);
  text->addView(titleLabel);
  text->addView(subtitleLabel);
  addView(text);

  durationLabel = makeLabel("", 15, textSecondary(), true);
  durationLabel->setMarginLeft(14);
  addView(durationLabel);

  registerClickAction([this](brls::View*) {
    if (onSelected) onSelected();
    return true;
  });
}

void TrackRow::setTrack(const SpotifyEngine::Track& track, size_t index,
                        std::function<void()> action) {
  indexLabel->setText(std::to_string(index + 1));
  titleLabel->setText(track.name);
  std::string detail = track.artist;
  if (!track.album.empty()) detail += "  |  " + track.album;
  if (track.explicitContent) detail += "  |  Explicit";
  subtitleLabel->setText(detail);
  durationLabel->setText(formatDuration(track.durationMs));
  if (artworkEnabled && !track.imageUrl.empty())
    loadRemoteImage(artwork, track.imageUrl);
  onSelected = std::move(action);
}

void TrackRow::setCompact(bool compact) {
  artworkEnabled = !compact;
  artwork->setVisibility(compact ? brls::Visibility::GONE
                                 : brls::Visibility::VISIBLE);
  setHeight(compact ? 58 : 72);
}

void TrackRow::setCurrent(bool current) {
  setBackgroundColor(current ? nvgRGB(18, 45, 30) : nvgRGBA(0, 0, 0, 0));
  indexLabel->setTextColor(current ? accent() : textSecondary());
  titleLabel->setTextColor(current ? accent() : textPrimary());
}

HorizontalSection::HorizontalSection(const std::string& title,
                                     const std::string& subtitle) {
  setAxis(brls::Axis::COLUMN);
  setMarginBottom(22);
  auto* header = new brls::Header();
  header->setTitle(title);
  header->setSubtitle(subtitle);
  header->setHeight(42);
  addView(header);

  scroller = new brls::HScrollingFrame();
  scroller->setHeight(244);
  scroller->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  scroller->setScrollingIndicatorVisible(false);
  row = new brls::Box();
  row->setAxis(brls::Axis::ROW);
  row->setPadding(4, 8, 4, 4);
  scroller->setContentView(row);
  addView(scroller);
}

void HorizontalSection::addCard(MediaCard* card) {
  card->setMarginRight(14);
  row->addView(card);
}

void HorizontalSection::showEmpty(const std::string& text) {
  auto* panel = makeStatePanel("Nothing here yet", text);
  panel->setWidth(430);
  panel->setMarginRight(14);
  row->addView(panel);
}

}  // namespace swui
