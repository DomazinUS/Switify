#include "tabs.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

#include "app_settings.hpp"
#include "platform_controls.hpp"
#include "playback_shortcuts.hpp"
#include "track_list_activity.hpp"
#include "ui_helpers.hpp"

namespace {
using Collection = SpotifyEngine::Collection;
using Track = SpotifyEngine::Track;
constexpr size_t kNowPlayingPageSize = 50;

float equalizerDb(float progress) {
  return std::round((progress * 24.0f) - 12.0f);
}

std::string equalizerDbText(float db) {
  const int rounded = static_cast<int>(std::round(db));
  if (rounded > 0) return "+" + std::to_string(rounded) + " dB";
  return std::to_string(rounded) + " dB";
}

brls::Box* makeHeading(const std::string& title, const std::string& subtitle,
                       brls::Label** titleOut = nullptr,
                       brls::Label** subtitleOut = nullptr) {
  auto* heading = new brls::Box();
  heading->setAxis(brls::Axis::COLUMN);
  heading->setGrow(1.0f);
  auto* titleLabel = swui::makeLabel(title, 30, swui::textPrimary());
  auto* subtitleLabel = swui::makeLabel(subtitle, 15, swui::textSecondary());
  subtitleLabel->setMarginTop(4);
  heading->addView(titleLabel);
  heading->addView(subtitleLabel);
  if (titleOut) *titleOut = titleLabel;
  if (subtitleOut) *subtitleOut = subtitleLabel;
  return heading;
}

void openCollection(const Collection& collection) {
  brls::Application::pushActivity(new TrackListActivity(collection));
}

void addTrackSection(brls::Box* parent, const std::string& title,
                     const std::string& subtitle,
                     const std::vector<Track>& tracks) {
  auto* section = new swui::HorizontalSection(title, subtitle);
  if (tracks.empty()) {
    section->showEmpty("Listen on Spotify and this section will fill in.");
  } else {
    auto sharedTracks = std::make_shared<std::vector<Track>>(tracks);
    for (size_t index = 0; index < sharedTracks->size(); ++index) {
      const auto& item = sharedTracks->at(index);
      auto* card = new swui::MediaCard();
      card->setContent(item.name, item.artist, item.imageUrl);
      card->setAction([sharedTracks, index, title] {
        SpotifyEngine::instance().playTracks(*sharedTracks, index, title);
      });
      section->addCard(card);
    }
  }
  parent->addView(section);
}

void addCollectionSection(brls::Box* parent, const std::string& title,
                          const std::string& subtitle,
                          const std::vector<Collection>& collections) {
  auto* section = new swui::HorizontalSection(title, subtitle);
  if (collections.empty()) {
    section->showEmpty("No collections are available for this account yet.");
  } else {
    for (const auto& collection : collections) {
      auto* card = new swui::MediaCard();
      card->setContent(collection.name, collection.subtitle,
                       collection.imageUrl);
      card->setAction([collection] { openCollection(collection); });
      section->addCard(card);
    }
  }
  parent->addView(section);
}

void addResumeSection(
    brls::Box* parent,
    const std::vector<SpotifyEngine::ResumeContext>& contexts) {
  auto* section = new swui::HorizontalSection(
      "Jump back in", "Resume the playlist or album where you left it");
  if (contexts.empty()) {
    section->showEmpty("Your recent listening contexts will appear here.");
  } else {
    auto shared =
        std::make_shared<std::vector<SpotifyEngine::ResumeContext>>(contexts);
    for (size_t index = 0; index < shared->size(); ++index) {
      const auto& item = shared->at(index);
      auto* card = new swui::MediaCard();
      card->setContent(item.collection.name, item.collection.subtitle,
                       item.collection.imageUrl);
      card->setAction([shared, index] {
        const auto context = shared->at(index);
        brls::Application::notify("Loading " + context.collection.name + "...");
        SpotifyEngine::instance().fetchPlaylistTracks(
            context.collection,
            [context](SpotifyEngine::Result<std::vector<Track>> result) mutable {
              brls::sync([context, result = std::move(result)]() mutable {
                if (!result.ok() || result.value.empty()) {
                  brls::Application::notify(
                      result.ok() ? "This context has no playable tracks"
                                  : result.error);
                  return;
                }
                size_t start = 0;
                for (size_t track = 0; track < result.value.size(); ++track) {
                  if (result.value[track].uri == context.lastTrack.uri) {
                    start = track;
                    break;
                  }
                }
                SpotifyEngine::instance().playTracks(
                    std::move(result.value), start, context.collection.name);
              });
            });
      });
      section->addCard(card);
    }
  }
  parent->addView(section);
}

brls::Box* makeRetryPanel(const std::string& title, const std::string& detail,
                          std::function<void()> retry) {
  auto* wrapper = new brls::Box();
  wrapper->setAxis(brls::Axis::COLUMN);
  wrapper->addView(swui::makeStatePanel(title, detail, nvgRGB(55, 31, 36)));
  auto* retryButton = swui::makeActionButton(
      "Try again",
      [retry = std::move(retry)](brls::View*) {
        retry();
        return true;
      },
      true);
  retryButton->setMarginTop(12);
  retryButton->setWidth(150);
  wrapper->addView(retryButton);
  return wrapper;
}

bool isInside(brls::View* view, brls::View* ancestor) {
  while (view) {
    if (view == ancestor) return true;
    if (!view->hasParent()) return false;
    view = view->getParent();
  }
  return false;
}

void clearDynamicContent(brls::Box* content, brls::View* stableFocus) {
  // Borealis may keep currentFocus after a dynamic child is queued for
  // deletion. Move focus to a persistent control before replacing a screen.
  // This prevents both the detached highlight and the use-after-free seen in
  // the 0.2.1 hardware crash.
  if (content && stableFocus &&
      isInside(brls::Application::getCurrentFocus(), content))
    brls::Application::giveFocus(stableFocus);
  if (content) content->clearViews();
}

std::string stateLabel(SpotifyEngine::State state) {
  switch (state) {
    case SpotifyEngine::State::WaitingForPhone:
      return "Waiting for first-time login";
    case SpotifyEngine::State::Connecting:
      return "Connecting";
    case SpotifyEngine::State::Ready:
      return "Online";
    case SpotifyEngine::State::Error:
      return "Needs attention";
    default:
      return "Starting";
  }
}

void showQueuePage() {
  auto queue = SpotifyEngine::instance().currentQueue();
  auto sharedQueue = std::make_shared<std::vector<Track>>(std::move(queue));

  auto* page = new brls::Box();
  page->setAxis(brls::Axis::COLUMN);
  page->setDimensions(brls::Application::contentWidth,
                      brls::Application::contentHeight);
  page->setBackgroundColor(swui::background());
  page->registerAction(
      "Back", brls::BUTTON_B,
      [](brls::View*) { return brls::Application::popActivity(); });
  page->registerAction(
      "Player", brls::BUTTON_START,
      [](brls::View*) {
        showTheaterMode();
        return true;
      });
  registerPlaybackShortcuts(page);
  page->registerAction(
      "Screen off", brls::BUTTON_BACK,
      [](brls::View*) {
        if (!swplatform::toggleScreenOff())
          brls::Application::notify(
              "Screen-off mode is unavailable on this console");
        return true;
      });

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(22, swui::kPagePadding, 16, swui::kPagePadding);
  top->addView(makeHeading(
      "Playback queue",
      sharedQueue->empty()
          ? "There are no tracks after the current song"
          : std::to_string(sharedQueue->size()) + " tracks coming next"));
  auto* back = swui::makeActionButton(
      "Back",
      [](brls::View*) { return brls::Application::popActivity(); });
  top->addView(back);
  page->addView(top);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  auto* list = new brls::Box();
  list->setAxis(brls::Axis::COLUMN);
  list->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  if (sharedQueue->empty()) {
    list->addView(swui::makeStatePanel(
        "Nothing up next",
        "The current song is the final track in this playback order."));
  } else {
    for (size_t index = 0; index < sharedQueue->size(); ++index) {
      auto* row = new swui::TrackRow();
      row->setTrack(sharedQueue->at(index), index, [sharedQueue, index] {
        SpotifyEngine::instance().playQueueTrack(index);
        brls::Application::notify("Queue moved to " +
                                  sharedQueue->at(index).name);
      });
      list->addView(row);
    }
  }
  scroll->setContentView(list);
  page->addView(scroll);
  brls::Application::pushActivity(new brls::Activity(page));
}

void showInformationPage(
    const std::string& title, const std::string& subtitle,
    const std::vector<std::pair<std::string, std::string>>& sections) {
  auto* page = new brls::Box();
  page->setAxis(brls::Axis::COLUMN);
  page->setDimensions(brls::Application::contentWidth,
                      brls::Application::contentHeight);
  page->setBackgroundColor(swui::background());
  page->registerAction(
      "Back", brls::BUTTON_B,
      [](brls::View*) { return brls::Application::popActivity(); });

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(22, swui::kPagePadding, 16, swui::kPagePadding);
  top->addView(makeHeading(title, subtitle));
  auto* back = swui::makeActionButton(
      "Back",
      [](brls::View*) { return brls::Application::popActivity(); });
  back->setWidth(130);
  top->addView(back);
  page->addView(top);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  auto* content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 44, swui::kPagePadding);
  for (size_t index = 0; index < sections.size(); ++index) {
    auto* panel =
        swui::makeStatePanel(sections[index].first, sections[index].second);
    panel->setFocusable(true);
    panel->setHighlightCornerRadius(10);
    if (index != 0) panel->setMarginTop(14);
    content->addView(panel);
  }
  scroll->setContentView(content);
  page->addView(scroll);
  brls::Application::pushActivity(new brls::Activity(page));
}

class TheaterView : public brls::Box {
 public:
  TheaterView() : alive(std::make_shared<std::atomic_bool>(true)) {
    setAxis(brls::Axis::ROW);
    setDimensions(brls::Application::contentWidth,
                  brls::Application::contentHeight);
    setAlignItems(brls::AlignItems::CENTER);
    setPadding(52, 74, 52, 74);
    setBackgroundColor(nvgRGB(7, 9, 12));

    auto* artPanel = new brls::Box();
    artPanel->setDimensions(500, 500);
    artPanel->setPadding(14, 14, 14, 14);
    artPanel->setCornerRadius(18);
    artPanel->setBackgroundColor(swui::surface());
    artwork = new brls::Image();
    artwork->setDimensions(472, 472);
    artwork->setCornerRadius(12);
    artwork->setBackgroundColor(swui::surfaceRaised());
    artwork->setScalingType(brls::ImageScalingType::FIT);
    artPanel->addView(artwork);
    addView(artPanel);

    auto* details = new brls::Box();
    details->setAxis(brls::Axis::COLUMN);
    details->setGrow(1.0f);
    details->setMarginLeft(58);
    status = swui::makeLabel("FULL PLAYER", 15, swui::accent(), true);
    title = swui::makeLabel("Choose something to play", 40,
                            swui::textPrimary(), true);
    title->setMarginTop(16);
    artist = swui::makeLabel("Switify", 24, swui::textSecondary(), true);
    artist->setMarginTop(10);
    album = swui::makeLabel("Native playback on Nintendo Switch", 18,
                            swui::textSecondary(), true);
    album->setMarginTop(5);
    details->addView(status);
    details->addView(title);
    details->addView(artist);
    details->addView(album);

    progress = new brls::Slider();
    progress->setStep(0.18f);
    progress->setMarginTop(26);
    details->addView(progress);
    auto* timeRow = new brls::Box();
    timeRow->setAxis(brls::Axis::ROW);
    elapsed = swui::makeLabel("0:00", 14, swui::textSecondary(), true);
    duration = swui::makeLabel("0:00", 14, swui::textSecondary(), true);
    timeRow->addView(elapsed);
    timeRow->addView(new brls::Padding());
    timeRow->addView(duration);
    details->addView(timeRow);

    auto* transport = new brls::Box();
    transport->setAxis(brls::Axis::ROW);
    transport->setAlignItems(brls::AlignItems::CENTER);
    transport->setMarginTop(22);
    auto* shuffle = swui::makeShuffleButton(
        [](brls::View*) {
          SpotifyEngine::instance().toggleShuffle();
          return true;
        },
        &shuffleIcon);
    shuffle->setMarginRight(12);
    brls::Label* previousLabel = nullptr;
    auto* previous = swui::makeActionButton(
        "|◀",
        [](brls::View*) {
          SpotifyEngine::instance().previous();
          return true;
        },
        false, &previousLabel);
    previousLabel->setFontSize(34);
    previous->setMarginRight(16);
    playPauseButton = swui::makeActionButton(
        "▶",
        [](brls::View*) {
          SpotifyEngine::instance().playPause();
          return true;
        },
        true, &playPauseLabel);
    playPauseLabel->setFontSize(36);
    playPauseButton->setMarginRight(16);
    brls::Label* nextLabel = nullptr;
    auto* next = swui::makeActionButton(
        "▶|",
        [](brls::View*) {
          SpotifyEngine::instance().next();
          return true;
        },
        false, &nextLabel);
    nextLabel->setFontSize(34);
    next->setMarginRight(12);
    auto* repeat = swui::makeRepeatButton(
        [](brls::View*) {
          SpotifyEngine::instance().cycleRepeatMode();
          return true;
        },
        &repeatIcon);
    for (auto* button : {shuffle, previous, playPauseButton, next, repeat}) {
      button->setDimensions(86, 66);
      button->setPadding(8, 14, 8, 14);
    }
    transport->addView(shuffle);
    transport->addView(previous);
    transport->addView(playPauseButton);
    transport->addView(next);
    transport->addView(repeat);
    details->addView(transport);
    addView(details);

    // Enter theater mode on transport controls. Seeking remains deliberate:
    // press Up from Previous, Play/Pause, or Next to reach the timeline.
    if (auto* progressFocus = progress->getDefaultFocus()) {
      previous->setCustomNavigationRoute(brls::FocusDirection::UP,
                                         progressFocus);
      shuffle->setCustomNavigationRoute(brls::FocusDirection::UP, previous);
      playPauseButton->setCustomNavigationRoute(brls::FocusDirection::UP,
                                                progressFocus);
      next->setCustomNavigationRoute(brls::FocusDirection::UP, progressFocus);
      repeat->setCustomNavigationRoute(brls::FocusDirection::UP, next);
      progressFocus->setCustomNavigationRoute(brls::FocusDirection::DOWN,
                                              playPauseButton);
    }

    progress->getProgressEvent()->subscribe([this](float value) {
      if (updatingProgress) return;
      const auto now = std::chrono::steady_clock::now();
      if (now - lastSeek < std::chrono::milliseconds(250)) return;
      const auto snapshot = SpotifyEngine::instance().nowPlaying();
      if (snapshot.durationMs == 0) return;
      lastSeek = now;
      SpotifyEngine::instance().seekToMs(
          static_cast<uint32_t>(snapshot.durationMs * value));
    });

    registerAction(
        "Close player", brls::BUTTON_START,
        [](brls::View*) {
          return brls::Application::popActivity(
              brls::TransitionAnimation::NONE);
        });
    registerAction(
        "Back", brls::BUTTON_B,
        [](brls::View*) { return brls::Application::popActivity(); }, true);
    registerAction(
        "Screen off", brls::BUTTON_BACK,
        [](brls::View*) {
          if (!swplatform::toggleScreenOff())
            brls::Application::notify(
                "Screen-off mode is unavailable on this console");
          return true;
        });
    registerPlaybackShortcuts(this);

    auto token = alive;
    observerId = SpotifyEngine::instance().subscribe([this, token] {
      brls::sync([this, token] {
        if (token->load()) refresh();
      });
    });
    refresh();
  }

  ~TheaterView() override {
    alive->store(false);
    SpotifyEngine::instance().unsubscribe(observerId);
  }

  brls::View* getDefaultFocus() override { return playPauseButton; }

  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override {
    if (std::chrono::steady_clock::now() - lastRefresh >
        std::chrono::milliseconds(250))
      refresh();
    brls::Box::draw(vg, x, y, width, height, style, ctx);
  }

 private:
  void refresh() {
    const auto snapshot = SpotifyEngine::instance().nowPlaying();
    status->setText(snapshot.hasTrack
                        ? (snapshot.paused ? "PAUSED" : "NOW PLAYING")
                        : "FULL PLAYER");
    if (snapshot.hasTrack) {
      title->setText(snapshot.name);
      artist->setText(snapshot.artist);
      album->setText(snapshot.album);
      if (!snapshot.imageUrl.empty() && snapshot.imageUrl != loadedArtworkUrl) {
        loadedArtworkUrl = snapshot.imageUrl;
        swui::loadRemoteImage(artwork, snapshot.imageUrl);
      }
    }
    playPauseLabel->setText(snapshot.paused ? "▶" : "II");
    elapsed->setText(swui::formatDuration(snapshot.positionMs));
    duration->setText(swui::formatDuration(snapshot.durationMs));
    shuffleIcon->setActive(snapshot.shuffle);
    repeatIcon->setMode(snapshot.repeatMode);
    updatingProgress = true;
    auto* progressFocus = progress->getDefaultFocus();
    if (!progressFocus || !progressFocus->isFocused()) {
      progress->setProgress(snapshot.durationMs > 0
                                ? static_cast<float>(snapshot.positionMs) /
                                      static_cast<float>(snapshot.durationMs)
                                : 0.0f);
    }
    updatingProgress = false;
    lastRefresh = std::chrono::steady_clock::now();
  }

  brls::Image* artwork = nullptr;
  brls::Label* status = nullptr;
  brls::Label* title = nullptr;
  brls::Label* artist = nullptr;
  brls::Label* album = nullptr;
  brls::Label* elapsed = nullptr;
  brls::Label* duration = nullptr;
  brls::Label* playPauseLabel = nullptr;
  swui::ShuffleIcon* shuffleIcon = nullptr;
  swui::RepeatIcon* repeatIcon = nullptr;
  brls::Box* playPauseButton = nullptr;
  brls::Slider* progress = nullptr;
  bool updatingProgress = false;
  std::string loadedArtworkUrl;
  SpotifyEngine::ObserverId observerId = 0;
  std::shared_ptr<std::atomic_bool> alive;
  std::chrono::steady_clock::time_point lastSeek{};
  std::chrono::steady_clock::time_point lastRefresh{};
};
}  // namespace

void showTheaterMode() {
  brls::Application::pushActivity(new brls::Activity(new TheaterView()),
                                  brls::TransitionAnimation::NONE);
}

// ---- Home ---------------------------------------------------------------

HomeTab::HomeTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(20, swui::kPagePadding, 12, swui::kPagePadding);
  top->addView(makeHeading("Home", "Your listening, ready on Switch", &title,
                           &account));
  refreshButton = swui::makeActionButton(
      "Refresh",
      [this](brls::View*) {
        auto& engine = SpotifyEngine::instance();
        if (engine.state() == SpotifyEngine::State::Error)
          engine.retryConnection();
        else if (engine.state() == SpotifyEngine::State::Ready)
          load(true);
        else
          brls::Application::notify("Spotify is still connecting");
        return true;
      });
  top->addView(refreshButton);
  addView(top);

  status = swui::makeLabel("Starting Spotify...", 15, swui::textSecondary());
  status->setMarginLeft(swui::kPagePadding);
  status->setMarginRight(swui::kPagePadding);
  status->setMarginBottom(8);
  addView(status);

  auto* filters = new brls::Box();
  filters->setAxis(brls::Axis::ROW);
  filters->setAlignItems(brls::AlignItems::CENTER);
  filters->setMarginLeft(swui::kPagePadding);
  filters->setMarginRight(swui::kPagePadding);
  filters->setMarginBottom(10);
  const struct {
    const char* title;
    Filter value;
    float width;
  } homeFilters[] = {{"All", Filter::All, 86},
                     {"Music", Filter::Music, 112},
                     {"Podcasts", Filter::Podcasts, 142}};
  for (const auto& item : homeFilters) {
    auto* chip = new swui::FilterChip(
        item.title, [this, value = item.value] { selectFilter(value); });
    chip->setWidth(item.width);
    if (!filterChips.empty()) chip->setMarginLeft(10);
    filters->addView(chip);
    filterChips.push_back(chip);
  }
  updateFilterChips();
  addView(filters);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  scroll->setContentView(content);
  addView(scroll);

  registerAction(
      "Refresh / retry", brls::BUTTON_X,
      [this](brls::View*) {
        auto& engine = SpotifyEngine::instance();
        if (engine.state() == SpotifyEngine::State::Error)
          engine.retryConnection();
        else if (engine.state() == SpotifyEngine::State::Ready)
          load(true);
        else
          brls::Application::notify("Spotify is still connecting");
        return true;
      });

  content->addView(swui::makeStatePanel(
      "Preparing your music", "Switify is establishing a native Spotify session."));

  auto token = alive;
  observerId = SpotifyEngine::instance().subscribe([this, token] {
    brls::sync([this, token] {
      if (!token->load()) return;
      refreshStatus();
    });
  });
  refreshStatus();
}

HomeTab::~HomeTab() {
  alive->store(false);
  SpotifyEngine::instance().unsubscribe(observerId);
}

brls::View* HomeTab::getDefaultFocus() {
  return !filterChips.empty() ? filterChips[static_cast<size_t>(filter)]
                             : refreshButton ? refreshButton->getDefaultFocus()
                       : brls::Box::getDefaultFocus();
}

void HomeTab::refreshStatus() {
  auto& engine = SpotifyEngine::instance();
  const auto state = engine.state();
  const bool stateChanged = state != displayedState;
  displayedState = state;
  status->setText(stateLabel(state) + "  |  " + engine.statusText());
  const auto user = engine.profile();
  if (!user.displayName.empty()) account->setText("Welcome, " + user.displayName);
  if (state == SpotifyEngine::State::WaitingForPhone && stateChanged) {
    loaded = false;
    loading = false;
    hasDashboardData = false;
    ++requestGeneration;
    clearDynamicContent(content, refreshButton);
    content->addView(swui::makeStatePanel(
        "Finish first-time setup",
        "1. Connect this Switch and your phone to the same Wi-Fi.\n"
        "2. Open Spotify on the phone.\n"
        "3. Open the device picker and choose Switify.\n\n"
        "Your encrypted login is then saved locally; the phone is not needed again.",
        nvgRGB(25, 55, 38)));
  } else if (state == SpotifyEngine::State::Connecting && stateChanged) {
    loaded = false;
    loading = false;
    hasDashboardData = false;
    ++requestGeneration;
    clearDynamicContent(content, refreshButton);
    content->addView(swui::makeStatePanel(
        "Connecting to Spotify",
        "Switify is trying each available Spotify access point."));
  } else if (state == SpotifyEngine::State::Error && stateChanged) {
    loaded = false;
    loading = false;
    hasDashboardData = false;
    ++requestGeneration;
    clearDynamicContent(content, refreshButton);
    content->addView(makeRetryPanel(
        "Spotify needs attention",
        engine.statusText() +
            "\n\nDetails are in sdmc:/config/Switify/app.log.",
        [] { SpotifyEngine::instance().retryConnection(); }));
  } else if (state == SpotifyEngine::State::Ready && !loaded && !loading) {
    load();
  }
}

void HomeTab::load(bool forceRefresh) {
  const uint64_t requestId = ++requestGeneration;
  if (!forceRefresh) {
    SpotifyEngine::HomeData dashboard;
    if (SpotifyEngine::instance().cachedDashboard(dashboard)) {
      SpotifyEngine::Result<SpotifyEngine::HomeData> cached;
      cached.value = std::move(dashboard);
      show(std::move(cached), requestId);
      return;
    }
  }
  loading = true;
  clearDynamicContent(content, refreshButton);
  content->addView(swui::makeStatePanel(
      "Loading Home", "Recently played, top tracks, and playlists are on the way."));
  auto token = alive;
  SpotifyEngine::instance().fetchDashboard(
      [this, token, requestId](SpotifyEngine::Result<SpotifyEngine::HomeData> result) mutable {
        brls::sync([this, token, requestId, result = std::move(result)]() mutable {
          if (!token->load()) return;
          show(std::move(result), requestId);
        });
      },
      forceRefresh);
}

void HomeTab::show(SpotifyEngine::Result<SpotifyEngine::HomeData> result,
                   uint64_t requestId) {
  if (requestId != requestGeneration.load()) return;
  loading = false;
  clearDynamicContent(content, refreshButton);
  if (!result.ok()) {
    // The error panel is a completed load state. Retrying is explicit via its
    // button; unrelated engine notifications must not auto-launch another
    // dashboard worker.
    loaded = true;
    content->addView(makeRetryPanel("Home could not be loaded", result.error,
                                    [this] {
                                      auto& engine = SpotifyEngine::instance();
                                      if (engine.state() == SpotifyEngine::State::Error)
                                        engine.retryConnection();
                                      else
                                        load(true);
                                    }));
    return;
  }
  loaded = true;
  dashboardData = std::move(result.value);
  hasDashboardData = true;
  if (!dashboardData.profile.displayName.empty())
    account->setText("Welcome, " + dashboardData.profile.displayName);
  if (!dashboardData.warning.empty()) {
    // Partial/transient failures should not push the useful Home shelves down
    // the screen. Preserve details in app.log and keep the status compact.
    status->setText("Online  |  Home loaded; some details can be retried");
  }
  renderDashboard();
}

void HomeTab::selectFilter(Filter nextFilter) {
  if (filter == nextFilter) return;
  filter = nextFilter;
  updateFilterChips();
  if (hasDashboardData) renderDashboard();
}

void HomeTab::updateFilterChips() {
  for (size_t index = 0; index < filterChips.size(); ++index)
    filterChips[index]->setSelected(index == static_cast<size_t>(filter));
}

void HomeTab::renderDashboard() {
  if (!hasDashboardData) return;
  brls::View* stable = !filterChips.empty()
                           ? filterChips[static_cast<size_t>(filter)]
                           : refreshButton;
  clearDynamicContent(content, stable);
  const bool showMusic = filter != Filter::Podcasts;
  const bool showPodcasts = filter != Filter::Music;

  std::vector<SpotifyEngine::ResumeContext> contexts;
  for (const auto& item : dashboardData.resumeContexts) {
    const bool podcast = item.collection.kind == Collection::Kind::SHOW ||
                         item.lastTrack.podcast;
    if ((podcast && showPodcasts) || (!podcast && showMusic))
      contexts.push_back(item);
  }
  if (!contexts.empty()) {
    addResumeSection(content, contexts);
  } else {
    std::vector<Track> recent;
    for (const auto& track : dashboardData.recentlyPlayed) {
      if ((track.podcast && showPodcasts) || (!track.podcast && showMusic))
        recent.push_back(track);
    }
    if (!recent.empty())
      addTrackSection(content,
                      filter == Filter::Podcasts ? "Recent episodes"
                                                  : "Recently played",
                      "Pick up where you left off", recent);
  }

  for (const auto& shelf : dashboardData.shelves) {
    std::vector<Collection> items;
    for (const auto& item : shelf.items) {
      const bool podcast = item.kind == Collection::Kind::SHOW;
      if ((podcast && showPodcasts) || (!podcast && showMusic))
        items.push_back(item);
    }
    if (!items.empty())
      addCollectionSection(content, shelf.title, shelf.subtitle, items);
  }
  if (showMusic) {
    addTrackSection(content, "Your top tracks", "Based on the last four weeks",
                    dashboardData.topTracks);
    addCollectionSection(content, "Your playlists", "Saved and created by you",
                         dashboardData.playlists);
  }
  if (showPodcasts)
    addCollectionSection(content, "Your podcasts",
                         "Saved shows and episodes", dashboardData.podcasts);

  if (filter == Filter::Podcasts && contexts.empty() &&
      dashboardData.podcasts.empty()) {
    content->addView(swui::makeStatePanel(
        "No podcasts yet",
        "Save a show or play an episode on Spotify, then refresh Home."));
  }
}

// ---- Search -------------------------------------------------------------

SearchTab::SearchTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(20, swui::kPagePadding, 12, swui::kPagePadding);
  top->addView(makeHeading(
      "Search", "Songs, artists, albums, playlists, and podcasts"));
  searchButton = swui::makeActionButton(
      "Search Spotify",
      [this](brls::View*) {
        openKeyboard();
        return true;
      },
      true, &queryLabel);
  searchButton->setWidth(260);
  top->addView(searchButton);
  addView(top);

  status = swui::makeLabel("Choose Search Spotify and enter a title or artist.",
                           15, swui::textSecondary());
  status->setMarginLeft(swui::kPagePadding);
  status->setMarginBottom(8);
  addView(status);

  auto* filters = new brls::Box();
  filters->setAxis(brls::Axis::ROW);
  filters->setAlignItems(brls::AlignItems::CENTER);
  filters->setMarginLeft(swui::kPagePadding);
  filters->setMarginRight(swui::kPagePadding);
  filters->setMarginBottom(10);
  const struct {
    const char* title;
    Filter value;
    float width;
  } searchFilters[] = {{"All", Filter::All, 72},
                       {"Songs", Filter::Songs, 102},
                       {"Artists", Filter::Artists, 105},
                       {"Albums", Filter::Albums, 110},
                       {"Playlists", Filter::Playlists, 125},
                       {"Podcasts", Filter::Podcasts, 130}};
  for (const auto& item : searchFilters) {
    auto* chip = new swui::FilterChip(
        item.title, [this, value = item.value] { selectFilter(value); });
    chip->setWidth(item.width);
    if (!filterChips.empty()) chip->setMarginLeft(8);
    filters->addView(chip);
    filterChips.push_back(chip);
  }
  updateFilterChips();
  addView(filters);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  scroll->setContentView(content);
  addView(scroll);
  content->addView(swui::makeStatePanel(
      "Find your next track",
      "Search the Spotify catalog with the controller or touchscreen keyboard."));
}

SearchTab::~SearchTab() { alive->store(false); }

brls::View* SearchTab::getDefaultFocus() {
  return !filterChips.empty() ? filterChips[static_cast<size_t>(filter)]
                             : searchButton ? searchButton->getDefaultFocus()
                      : brls::Box::getDefaultFocus();
}

void SearchTab::openKeyboard() {
  brls::Application::getImeManager()->openForText(
      [this](std::string text) {
        if (!text.empty()) searchFor(text);
      },
      "Search Spotify");
}

void SearchTab::searchFor(const std::string& query) {
  const uint64_t requestId = ++requestGeneration;
  queryLabel->setText(query);
  status->setText("Searching Spotify...");
  clearDynamicContent(content, searchButton);
  content->addView(swui::makeStatePanel("Searching", "Looking for " + query));
  auto token = alive;
  SpotifyEngine::instance().search(
      query, [this, token, query, requestId](SpotifyEngine::Result<SpotifyEngine::SearchResults> result) mutable {
        brls::sync([this, token, query, requestId, result = std::move(result)]() mutable {
          if (!token->load()) return;
          show(query, std::move(result), requestId);
        });
      });
}

void SearchTab::show(
    const std::string& query,
    SpotifyEngine::Result<SpotifyEngine::SearchResults> result,
    uint64_t requestId) {
  if (requestId != requestGeneration.load()) return;
  clearDynamicContent(content, searchButton);
  if (!result.ok()) {
    hasSearchResults = false;
    status->setText("Search needs attention");
    content->addView(makeRetryPanel("Search failed", result.error,
                                    [this, query] {
                                      auto& engine = SpotifyEngine::instance();
                                      if (engine.state() == SpotifyEngine::State::Error)
                                        engine.retryConnection();
                                      else
                                        searchFor(query);
                                    }));
    return;
  }
  currentQuery = query;
  searchResults = std::move(result.value);
  hasSearchResults = true;
  renderResults();
}

void SearchTab::selectFilter(Filter nextFilter) {
  if (filter == nextFilter) return;
  filter = nextFilter;
  updateFilterChips();
  if (hasSearchResults) renderResults();
}

void SearchTab::updateFilterChips() {
  for (size_t index = 0; index < filterChips.size(); ++index)
    filterChips[index]->setSelected(index == static_cast<size_t>(filter));
}

void SearchTab::renderResults() {
  if (!hasSearchResults) return;
  brls::View* stable = !filterChips.empty()
                           ? filterChips[static_cast<size_t>(filter)]
                           : searchButton;
  clearDynamicContent(content, stable);

  size_t count = 0;
  switch (filter) {
    case Filter::Songs:
      count = searchResults.tracks.size();
      break;
    case Filter::Artists:
      count = searchResults.artists.size();
      break;
    case Filter::Albums:
      count = searchResults.albums.size();
      break;
    case Filter::Playlists:
      count = searchResults.playlists.size();
      break;
    case Filter::Podcasts:
      count = searchResults.podcasts.size() + searchResults.episodes.size();
      break;
    case Filter::All:
      count = searchResults.tracks.size() + searchResults.artists.size() +
              searchResults.albums.size() + searchResults.playlists.size() +
              searchResults.podcasts.size() + searchResults.episodes.size();
      break;
  }
  status->setText(std::to_string(count) + " results for " + currentQuery);
  if (count == 0) {
    content->addView(swui::makeStatePanel(
        "No matches", "Try a different spelling or another content filter."));
    return;
  }

  const auto addRows = [this](const std::string& heading,
                              const std::vector<Track>& tracks,
                              const std::string& source) {
    if (tracks.empty()) return;
    auto* header = new brls::Header();
    header->setTitle(heading);
    header->setSubtitle(std::to_string(tracks.size()));
    content->addView(header);
    auto sharedTracks = std::make_shared<std::vector<Track>>(tracks);
    for (size_t index = 0; index < sharedTracks->size(); ++index) {
      auto* row = new swui::TrackRow();
      row->setTrack(sharedTracks->at(index), index,
                    [sharedTracks, index, source] {
        SpotifyEngine::instance().playTracks(
            *sharedTracks, index, source);
      });
      content->addView(row);
    }
  };

  if (filter == Filter::All || filter == Filter::Songs)
    addRows("Songs", searchResults.tracks, "Search: " + currentQuery);
  if ((filter == Filter::All || filter == Filter::Artists) &&
      !searchResults.artists.empty())
    addCollectionSection(content, "Artists", "Top matching tracks",
                         searchResults.artists);
  if ((filter == Filter::All || filter == Filter::Albums) &&
      !searchResults.albums.empty())
    addCollectionSection(content, "Albums", "Open an album to see its tracks",
                         searchResults.albums);
  if ((filter == Filter::All || filter == Filter::Playlists) &&
      !searchResults.playlists.empty())
    addCollectionSection(content, "Playlists", "Matching Spotify playlists",
                         searchResults.playlists);
  if (filter == Filter::All || filter == Filter::Podcasts) {
    if (!searchResults.podcasts.empty())
      addCollectionSection(content, "Podcasts", "Shows matching your search",
                           searchResults.podcasts);
    addRows("Episodes", searchResults.episodes,
            "Podcast search: " + currentQuery);
  }
}

// ---- Library ------------------------------------------------------------

LibraryTab::LibraryTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(20, swui::kPagePadding, 12, swui::kPagePadding);
  top->addView(makeHeading(
      "Your Library", "Liked songs, playlists, albums, and podcasts"));
  refreshButton = swui::makeActionButton(
      "Refresh",
      [this](brls::View*) {
        auto& engine = SpotifyEngine::instance();
        if (engine.state() == SpotifyEngine::State::Error)
          engine.retryConnection();
        else if (engine.state() == SpotifyEngine::State::Ready)
          load(true);
        else
          brls::Application::notify("Spotify is still connecting");
        return true;
      });
  top->addView(refreshButton);
  addView(top);

  status = swui::makeLabel("Preparing your library...", 15,
                           swui::textSecondary());
  status->setMarginLeft(swui::kPagePadding);
  status->setMarginBottom(8);
  addView(status);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  scroll->setContentView(content);
  addView(scroll);
  content->addView(swui::makeStatePanel(
      "Preparing your library", "Private Spotify collections load after login."));

  auto token = alive;
  observerId = SpotifyEngine::instance().subscribe([this, token] {
    brls::sync([this, token] {
      if (!token->load()) return;
      refreshStatus();
    });
  });
  refreshStatus();
}

LibraryTab::~LibraryTab() {
  alive->store(false);
  SpotifyEngine::instance().unsubscribe(observerId);
}

brls::View* LibraryTab::getDefaultFocus() {
  return refreshButton ? refreshButton->getDefaultFocus()
                       : brls::Box::getDefaultFocus();
}

void LibraryTab::refreshStatus() {
  auto& engine = SpotifyEngine::instance();
  const auto state = engine.state();
  const bool stateChanged = state != displayedState;
  displayedState = state;
  status->setText(stateLabel(state) + "  |  " + engine.statusText());
  if (state == SpotifyEngine::State::Error && stateChanged) {
    loaded = false;
    loading = false;
    ++requestGeneration;
    clearDynamicContent(content, refreshButton);
    content->addView(makeRetryPanel(
        "Spotify needs attention", engine.statusText(),
        [] { SpotifyEngine::instance().retryConnection(); }));
  } else if (state == SpotifyEngine::State::Connecting && stateChanged) {
    loaded = false;
    loading = false;
    ++requestGeneration;
    clearDynamicContent(content, refreshButton);
    content->addView(swui::makeStatePanel(
        "Connecting to Spotify",
        "Your library will load as soon as the session is ready."));
  } else if (state == SpotifyEngine::State::Ready && !loaded && !loading) {
    load();
  }
}

void LibraryTab::load(bool forceRefresh) {
  const uint64_t requestId = ++requestGeneration;
  if (!forceRefresh) {
    SpotifyEngine::LibraryData library;
    if (SpotifyEngine::instance().cachedLibrary(library)) {
      SpotifyEngine::Result<SpotifyEngine::LibraryData> cached;
      cached.value = std::move(library);
      show(std::move(cached), requestId);
      return;
    }
  }
  loading = true;
  clearDynamicContent(content, refreshButton);
  content->addView(swui::makeStatePanel(
      "Loading Your Library",
      "Fetching private playlists, saved albums, and podcasts."));
  auto token = alive;
  SpotifyEngine::instance().fetchLibrary(
      [this, token, requestId](SpotifyEngine::Result<SpotifyEngine::LibraryData> result) mutable {
        brls::sync([this, token, requestId, result = std::move(result)]() mutable {
          if (!token->load()) return;
          show(std::move(result), requestId);
        });
      },
      forceRefresh);
}

void LibraryTab::show(
    SpotifyEngine::Result<SpotifyEngine::LibraryData> result,
    uint64_t requestId) {
  if (requestId != requestGeneration.load()) return;
  loading = false;
  clearDynamicContent(content, refreshButton);
  if (!result.ok()) {
    loaded = true;
    content->addView(makeRetryPanel("Library could not be loaded", result.error,
                                    [this] {
                                      auto& engine = SpotifyEngine::instance();
                                      if (engine.state() == SpotifyEngine::State::Error)
                                        engine.retryConnection();
                                      else
                                        load(true);
                                    }));
    return;
  }
  loaded = true;
  status->setText(std::to_string(result.value.playlists.size()) +
                  " collections  |  " +
                  std::to_string(result.value.albums.size()) +
                  " albums  |  " +
                  std::to_string(result.value.podcasts.size()) +
                  " podcasts");
  addCollectionSection(content, "Playlists and Liked Songs",
                       "Everything you have saved or followed",
                       result.value.playlists);
  addCollectionSection(content, "Saved albums", "Your album library",
                       result.value.albums);
  addCollectionSection(content, "Saved podcasts", "Shows you follow",
                       result.value.podcasts);
}

// ---- Now Playing --------------------------------------------------------

NowPlayingTab::NowPlayingTab()
    : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  auto* body = new brls::Box();
  body->setAxis(brls::Axis::COLUMN);
  body->setPadding(24, 52, 36, 52);

  auto* player = new brls::Box();
  player->setAxis(brls::Axis::ROW);
  player->setHeight(390);
  player->setAlignItems(brls::AlignItems::CENTER);

  auto* artPanel = new brls::Box();
  artPanel->setDimensions(350, 350);
  artPanel->setPadding(12, 12, 12, 12);
  artPanel->setCornerRadius(16);
  artPanel->setBackgroundColor(swui::surface());
  artwork = new brls::Image();
  artwork->setDimensions(326, 326);
  artwork->setBackgroundColor(swui::surfaceRaised());
  artwork->setCornerRadius(10);
  artwork->setScalingType(brls::ImageScalingType::FIT);
  artPanel->addView(artwork);
  player->addView(artPanel);

  auto* info = new brls::Box();
  info->setAxis(brls::Axis::COLUMN);
  info->setGrow(1.0f);
  info->setMarginLeft(44);
  status = swui::makeLabel("Nothing playing", 15, swui::accent(), true);
  track = swui::makeLabel("Choose something from Home", 34,
                          swui::textPrimary(), true);
  track->setMarginTop(12);
  artist = swui::makeLabel("Switify", 21, swui::textSecondary(), true);
  artist->setMarginTop(7);
  album = swui::makeLabel("Native playback on Nintendo Switch", 16,
                          swui::textSecondary(), true);
  album->setMarginTop(4);
  info->addView(status);
  info->addView(track);
  info->addView(artist);
  info->addView(album);

  progress = new brls::Slider();
  progress->setStep(0.18f);
  progress->setMarginTop(25);
  info->addView(progress);
  auto* timeRow = new brls::Box();
  timeRow->setAxis(brls::Axis::ROW);
  elapsed = swui::makeLabel("0:00", 13, swui::textSecondary(), true);
  duration = swui::makeLabel("0:00", 13, swui::textSecondary(), true);
  auto* spacer = new brls::Padding();
  timeRow->addView(elapsed);
  timeRow->addView(spacer);
  timeRow->addView(duration);
  info->addView(timeRow);

  auto* transport = new brls::Box();
  transport->setAxis(brls::Axis::ROW);
  transport->setMarginTop(18);
  transport->setAlignItems(brls::AlignItems::CENTER);
  auto* shuffle = swui::makeShuffleButton(
      [](brls::View*) {
        SpotifyEngine::instance().toggleShuffle();
        return true;
      },
      &shuffleIcon);
  shuffle->setMarginRight(8);
  brls::Label* previousLabel = nullptr;
  auto* previous = swui::makeActionButton(
      "|◀",
      [](brls::View*) {
        SpotifyEngine::instance().previous();
        return true;
      },
      false, &previousLabel);
  previousLabel->setFontSize(24);
  previous->setMarginRight(8);
  playPauseButton = swui::makeActionButton(
      "▶",
      [](brls::View*) {
        SpotifyEngine::instance().playPause();
        return true;
      },
      true, &playPauseLabel);
  playPauseLabel->setFontSize(25);
  playPauseButton->setMarginRight(8);
  brls::Label* nextLabel = nullptr;
  auto* next = swui::makeActionButton(
      "▶|",
      [](brls::View*) {
        SpotifyEngine::instance().next();
        return true;
      },
      false, &nextLabel);
  nextLabel->setFontSize(24);
  next->setMarginRight(8);
  auto* repeat = swui::makeRepeatButton(
      [](brls::View*) {
        SpotifyEngine::instance().cycleRepeatMode();
        return true;
      },
      &repeatIcon);
  for (auto* button : {shuffle, previous, playPauseButton, next, repeat}) {
    button->setWidth(76);
    button->setGrow(1.0f);
    button->setPadding(8, 12, 8, 12);
  }
  transport->addView(shuffle);
  transport->addView(previous);
  transport->addView(playPauseButton);
  transport->addView(next);
  transport->addView(repeat);
  info->addView(transport);

  auto* modes = new brls::Box();
  modes->setAxis(brls::Axis::ROW);
  modes->setMarginTop(12);
  auto* save = swui::makeActionButton(
      "♡",
      [this](brls::View*) {
        const auto snapshot = SpotifyEngine::instance().nowPlaying();
        if (snapshot.uri.empty()) {
          brls::Application::notify(
              "This externally started track has no library URI yet");
          return true;
        }
        if (snapshot.savePending) {
          brls::Application::notify("Library update already pending");
          return true;
        }
        const bool target = !snapshot.saved;
        brls::Application::notify(target ? "Saving to Your Library…"
                                         : "Removing from Your Library…");
        auto token = alive;
        SpotifyEngine::instance().setTrackSaved(
            snapshot.uri, target,
            [token, target](SpotifyEngine::Result<bool> result) {
              brls::sync([token, target, result = std::move(result)] {
                if (!token->load()) return;
                brls::Application::notify(
                    result.ok() ? (target ? "Saved to Your Library"
                                           : "Removed from Your Library")
                                : result.error);
              });
            });
        return true;
      },
      false, &saveLabel);
  saveLabel->setFontSize(28);
  auto* queue = swui::makeActionButton(
      "Queue",
      [](brls::View*) {
        showQueuePage();
        return true;
      });
  save->setMarginRight(8);
  save->setWidth(82);
  queue->setGrow(1.0f);
  for (auto* button : {save, queue}) {
    button->setPadding(10, 12, 10, 12);
  }
  modes->addView(save);
  modes->addView(queue);
  info->addView(modes);

  auto* volumeRow = new brls::Box();
  volumeRow->setAxis(brls::Axis::ROW);
  volumeRow->setAlignItems(brls::AlignItems::CENTER);
  volumeRow->setMarginTop(16);
  volumeLabel = swui::makeLabel("Volume 80%", 15, swui::textSecondary(), true);
  volumeLabel->setWidth(110);
  volume = new brls::Slider();
  volume->setGrow(1.0f);
  volume->setStep(0.45f);
  volumeRow->addView(volumeLabel);
  volumeRow->addView(volume);
  info->addView(volumeRow);
  player->addView(info);
  body->addView(player);

  playlistHeader = new brls::Header();
  playlistHeader->setTitle("Playlist");
  playlistHeader->setSubtitle("Start something to build this session");
  playlistHeader->setHeight(48);
  playlistHeader->setMarginTop(10);
  body->addView(playlistHeader);
  playlistList = new brls::Box();
  playlistList->setAxis(brls::Axis::COLUMN);
  body->addView(playlistList);
  scroll->setContentView(body);
  addView(scroll);

  // Entering this tab from the sidebar must land on transport controls, not
  // on the seek bar. The seek bar is reachable deliberately by pressing Up
  // from Previous, Play/Pause, or Next.
  // Slider is a container; its pointer child owns focus in Borealis. Routes
  // registered on the container trigger a fatal error when this tab is first
  // constructed, so always route through the actual focusable child.
  if (auto* progressFocus = progress->getDefaultFocus()) {
    shuffle->setCustomNavigationRoute(brls::FocusDirection::UP, previous);
    previous->setCustomNavigationRoute(brls::FocusDirection::UP,
                                       progressFocus);
    playPauseButton->setCustomNavigationRoute(brls::FocusDirection::UP,
                                              progressFocus);
    next->setCustomNavigationRoute(brls::FocusDirection::UP, progressFocus);
    repeat->setCustomNavigationRoute(brls::FocusDirection::UP, next);
    progressFocus->setCustomNavigationRoute(brls::FocusDirection::DOWN,
                                            playPauseButton);
    progressFocus->setCustomNavigationRoute(brls::FocusDirection::LEFT,
                                            playPauseButton);
  }

  progress->getProgressEvent()->subscribe([this](float value) {
    if (updatingSliders) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSeek < std::chrono::milliseconds(250)) return;
    const auto snapshot = SpotifyEngine::instance().nowPlaying();
    if (snapshot.durationMs == 0) return;
    lastSeek = now;
    SpotifyEngine::instance().seekToMs(
        static_cast<uint32_t>(snapshot.durationMs * value));
  });
  volume->getProgressEvent()->subscribe([this](float value) {
    if (!updatingSliders) {
      swsettings::setOutputVolume(value);
      SpotifyEngine::instance().setVolume(value);
    }
  });

  auto token = alive;
  observerId = SpotifyEngine::instance().subscribe([this, token] {
    brls::sync([this, token] {
      if (token->load() && getVisibility() == brls::Visibility::VISIBLE)
        refresh();
    });
  });
  refresh();
}

NowPlayingTab::~NowPlayingTab() {
  alive->store(false);
  SpotifyEngine::instance().unsubscribe(observerId);
}

brls::View* NowPlayingTab::getDefaultFocus() {
  return playPauseButton ? playPauseButton : brls::Box::getDefaultFocus();
}

void NowPlayingTab::requestPlaylistPage(size_t offset) {
  if (isInside(brls::Application::getCurrentFocus(), playlistList))
    brls::Application::giveFocus(playPauseButton);
  auto token = alive;
  brls::sync([this, token, offset] {
    if (!token->load()) return;
    const auto session = SpotifyEngine::instance().playbackSessionInfo();
    if (session.trackCount == 0) return;
    playlistPageStart =
        std::min(offset, ((session.trackCount - 1) / kNowPlayingPageSize) *
                             kNowPlayingPageSize);
    rebuildPlaylistPage(session, true);
  });
}

void NowPlayingTab::rebuildPlaylistPage(
    const SpotifyEngine::PlaybackSessionInfo& session,
    bool focusFirstTrack) {
  if (isInside(brls::Application::getCurrentFocus(), playlistList))
    brls::Application::giveFocus(playPauseButton);
  playlistList->clearViews();
  playlistRows.clear();
  playlistCurrentIndex = session.hasCurrent
                             ? session.currentIndex
                             : static_cast<size_t>(-1);

  const std::string source =
      session.name.empty() ? "Current playback session" : session.name;
  playlistHeader->setTitle(session.podcast ? "Episodes" : "Playlist");
  if (session.trackCount == 0) {
    playlistPageStart = 0;
    playlistPageEnd = 0;
    playlistHeader->setSubtitle("Start something to build this session");
    playlistList->addView(swui::makeStatePanel(
        "Nothing playing yet",
        "Choose a playlist, album, or group of tracks from Switify."));
    return;
  }

  playlistPageStart =
      std::min(playlistPageStart,
               ((session.trackCount - 1) / kNowPlayingPageSize) *
                   kNowPlayingPageSize);
  playlistPageEnd = std::min(session.trackCount,
                             playlistPageStart + kNowPlayingPageSize);
  const std::string itemName = session.podcast ? "Episodes" : "Songs";
  const std::string itemNameLower = session.podcast ? "episodes" : "songs";
  playlistHeader->setSubtitle(
      source + "  |  " + itemName + " " +
      std::to_string(playlistPageStart + 1) +
      "-" + std::to_string(playlistPageEnd) + " of " +
      std::to_string(session.trackCount));

  if (playlistPageStart > 0) {
    auto* previousPage = swui::makeActionButton(
        "Previous " + std::to_string(kNowPlayingPageSize) + " " +
            itemNameLower,
        [this](brls::View*) {
          requestPlaylistPage(playlistPageStart >= kNowPlayingPageSize
                                  ? playlistPageStart - kNowPlayingPageSize
                                  : 0);
          return true;
        });
    previousPage->setMarginBottom(8);
    playlistList->addView(previousPage);
  }

  auto tracks = SpotifyEngine::instance().playbackSessionPage(
      playlistPageStart, kNowPlayingPageSize);
  brls::View* firstTrack = nullptr;
  playlistRows.reserve(tracks.size());
  for (size_t localIndex = 0; localIndex < tracks.size(); ++localIndex) {
    const size_t trackIndex = playlistPageStart + localIndex;
    auto* row = new swui::TrackRow();
    row->setCompact(true);
    row->setTrack(tracks[localIndex], trackIndex, [trackIndex] {
      SpotifyEngine::instance().playPlaylistTrack(trackIndex);
    });
    row->setCurrent(session.hasCurrent &&
                    trackIndex == session.currentIndex);
    playlistRows.push_back(row);
    playlistList->addView(row);
    if (!firstTrack) firstTrack = row;
  }

  if (playlistPageEnd < session.trackCount) {
    auto* nextPage = swui::makeActionButton(
        "Next " + std::to_string(std::min(
                      kNowPlayingPageSize,
                      session.trackCount - playlistPageEnd)) +
            " " + itemNameLower,
        [this](brls::View*) {
          requestPlaylistPage(playlistPageEnd);
          return true;
        });
    nextPage->setMarginTop(8);
    playlistList->addView(nextPage);
  }

  if (focusFirstTrack && firstTrack)
    brls::Application::giveFocus(firstTrack);
}

void NowPlayingTab::refreshPlaylist(
    const SpotifyEngine::PlaybackSessionInfo& session) {
  if (session.revision != playlistRevision) {
    playlistRevision = session.revision;
    playlistPageStart = session.hasCurrent
                            ? (session.currentIndex / kNowPlayingPageSize) *
                                  kNowPlayingPageSize
                            : 0;
    rebuildPlaylistPage(session);
    return;
  }

  const size_t current = session.hasCurrent
                             ? session.currentIndex
                             : static_cast<size_t>(-1);
  if (current == playlistCurrentIndex) return;

  // Follow automatic track changes only while the user is viewing the page
  // that contained the previous current track. Manual page browsing remains
  // stable instead of snapping back every refresh.
  const bool followedCurrent =
      playlistCurrentIndex >= playlistPageStart &&
      playlistCurrentIndex < playlistPageEnd;
  if (session.hasCurrent && followedCurrent &&
      (current < playlistPageStart || current >= playlistPageEnd)) {
    playlistPageStart =
        (current / kNowPlayingPageSize) * kNowPlayingPageSize;
    rebuildPlaylistPage(session);
    return;
  }

  if (playlistCurrentIndex >= playlistPageStart &&
      playlistCurrentIndex < playlistPageEnd) {
    const size_t local = playlistCurrentIndex - playlistPageStart;
    if (local < playlistRows.size()) playlistRows[local]->setCurrent(false);
  }
  if (current >= playlistPageStart && current < playlistPageEnd) {
    const size_t local = current - playlistPageStart;
    if (local < playlistRows.size()) playlistRows[local]->setCurrent(true);
  }
  playlistCurrentIndex = current;
}

void NowPlayingTab::refresh() {
  auto& engine = SpotifyEngine::instance();
  const auto snapshot = engine.nowPlaying();
  status->setText(snapshot.hasTrack
                      ? (snapshot.paused ? "PAUSED" : "NOW PLAYING")
                      : stateLabel(engine.state()) + "  |  " + engine.statusText());
  if (snapshot.hasTrack) {
    track->setText(snapshot.name);
    artist->setText(snapshot.artist);
    album->setText(snapshot.album);
    if (!snapshot.imageUrl.empty() && snapshot.imageUrl != loadedArtworkUrl) {
      loadedArtworkUrl = snapshot.imageUrl;
      swui::loadRemoteImage(artwork, snapshot.imageUrl);
    }
  }
  playPauseLabel->setText(snapshot.paused ? "▶" : "II");
  shuffleIcon->setActive(snapshot.shuffle);
  repeatIcon->setMode(snapshot.repeatMode);
  saveLabel->setText(snapshot.saved ? "♥" : "♡");
  saveLabel->setTextColor(snapshot.saved
                              ? swui::accent()
                              : (snapshot.savedKnown ? swui::textPrimary()
                                                     : swui::textSecondary()));
  elapsed->setText(swui::formatDuration(snapshot.positionMs));
  duration->setText(swui::formatDuration(snapshot.durationMs));
  volumeLabel->setText("Volume " +
                       std::to_string(static_cast<int>(snapshot.volume * 100)) +
                       "%");
  updatingSliders = true;
  if (!progress->getDefaultFocus()->isFocused()) {
    progress->setProgress(snapshot.durationMs > 0
                              ? static_cast<float>(snapshot.positionMs) /
                                    static_cast<float>(snapshot.durationMs)
                              : 0.0f);
  }
  if (!volume->getDefaultFocus()->isFocused()) volume->setProgress(snapshot.volume);
  updatingSliders = false;
  refreshPlaylist(engine.playbackSessionInfo());
  lastRefresh = std::chrono::steady_clock::now();
}

void NowPlayingTab::willAppear(bool resetState) {
  brls::Box::willAppear(resetState);
  refresh();
}

void NowPlayingTab::draw(NVGcontext* vg, float x, float y, float width,
                         float height, brls::Style style,
                         brls::FrameContext* ctx) {
  if (std::chrono::steady_clock::now() - lastRefresh >
      std::chrono::milliseconds(250))
    refresh();
  brls::Box::draw(vg, x, y, width, height, style, ctx);
}

// ---- Settings -----------------------------------------------------------

SettingsTab::SettingsTab()
    : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* top = new brls::Box();
  top->setPadding(20, swui::kPagePadding, 12, swui::kPagePadding);
  top->addView(makeHeading("Settings", "Playback, account, and diagnostics"));
  addView(top);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  auto* content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  scroll->setContentView(content);
  addView(scroll);

  auto* accountPanel = new brls::Box();
  accountPanel->setAxis(brls::Axis::COLUMN);
  accountPanel->setPadding(18, 20, 18, 20);
  accountPanel->setCornerRadius(10);
  accountPanel->setBackgroundColor(swui::surface());
  connection = swui::makeLabel("Starting", 20, swui::textPrimary());
  account = swui::makeLabel("Spotify account not loaded yet", 15,
                            swui::textSecondary());
  account->setMarginTop(5);
  accountPanel->addView(connection);
  accountPanel->addView(account);
  content->addView(accountPanel);

  auto* signOut = swui::makeActionButton(
      "Sign out",
      [](brls::View*) {
        auto* dialog = new brls::Dialog(
            "Sign out of Spotify on this Switch?\n\nSwitify will close and "
            "will ask for the one-time phone connection again next launch.");
        dialog->addButton("Cancel", [] {});
        dialog->addButton("Sign out", [] {
          if (!SpotifyEngine::instance().forgetCredentials()) {
            brls::Application::notify("Could not remove the saved login");
            return;
          }
          brls::Application::quit();
        });
        dialog->open();
        return true;
      });
  signOut->setMarginTop(12);
  signOut->setWidth(140);
  signOut->setBackgroundColor(nvgRGB(65, 34, 39));
  content->addView(signOut);

  auto* controlsHeader = new brls::Header();
  controlsHeader->setTitle("Playback controls");
  controlsHeader->setMarginTop(16);
  content->addView(controlsHeader);
  const auto settings = swsettings::get();
  auto* shortcuts = new brls::BooleanCell();
  shortcuts->init("Controller shortcuts", settings.playbackShortcuts,
                  [](bool enabled) {
                    swsettings::setPlaybackShortcutsEnabled(enabled);
                    brls::Application::notify(
                        enabled ? "Playback shortcuts enabled"
                                : "Playback shortcuts disabled");
                  });
  content->addView(shortcuts);
  auto* shortcutHelp = swui::makeStatePanel(
      "Available everywhere in Switify",
      "Y  Play / Pause     L  Previous     R  Next\n"
      "ZL  Rewind 15 seconds     ZR  Fast-forward 15 seconds");
  shortcutHelp->setMarginTop(8);
  content->addView(shortcutHelp);

  auto* audioHeader = new brls::Header();
  audioHeader->setTitle("Audio");
  audioHeader->setMarginTop(16);
  content->addView(audioHeader);
  auto* volumePanel = new brls::Box();
  volumePanel->setAxis(brls::Axis::ROW);
  volumePanel->setAlignItems(brls::AlignItems::CENTER);
  volumePanel->setPadding(14, 20, 14, 20);
  volumePanel->setCornerRadius(10);
  volumePanel->setBackgroundColor(swui::surface());
  volumeValue = swui::makeLabel("Output volume 80%", 17,
                                swui::textPrimary(), true);
  volumeValue->setWidth(190);
  volume = new brls::Slider();
  volume->setGrow(1.0f);
  volume->setStep(0.45f);
  volumePanel->addView(volumeValue);
  volumePanel->addView(volume);
  content->addView(volumePanel);

  auto* quality = new brls::SelectorCell();
  quality->init(
      "Streaming quality", {"Normal · 160 kbps", "High · 320 kbps"},
      settings.highQuality ? 1 : 0,
      [](int selected) {
        const bool high = selected == 1;
        swsettings::setHighQuality(high);
        SpotifyEngine::instance().setHighQuality(high);
        brls::Application::notify(
            high ? "High quality selected; applies next launch"
                 : "Normal quality selected; applies next launch");
      });
  quality->setMarginTop(10);
  content->addView(quality);
  auto* qualityHelp = swui::makeStatePanel(
      "Spotify stream quality",
      "Normal uses 160 kbps and less bandwidth. High uses 320 kbps.\n"
      "A quality change takes effect the next time Switify starts.");
  qualityHelp->setMarginTop(8);
  content->addView(qualityHelp);

  equalizerEnabled = new brls::BooleanCell();
  equalizerEnabled->init("Three-band equalizer", settings.equalizerEnabled,
                         [this](bool) { commitEqualizer(); });
  equalizerEnabled->setMarginTop(10);
  content->addView(equalizerEnabled);

  bass = new brls::SliderCell();
  bass->init("Bass · 120 Hz", (settings.bassDb + 12.0f) / 24.0f,
             [this](float) { commitEqualizer(); });
  bass->setDetailText(equalizerDbText(settings.bassDb));
  content->addView(bass);
  mid = new brls::SliderCell();
  mid->init("Mid · 1 kHz", (settings.midDb + 12.0f) / 24.0f,
            [this](float) { commitEqualizer(); });
  mid->setDetailText(equalizerDbText(settings.midDb));
  content->addView(mid);
  treble = new brls::SliderCell();
  treble->init("Treble · 8 kHz", (settings.trebleDb + 12.0f) / 24.0f,
               [this](float) { commitEqualizer(); });
  treble->setDetailText(equalizerDbText(settings.trebleDb));
  content->addView(treble);

  auto* resetEqualizer = swui::makeActionButton(
      "Reset equalizer",
      [this](brls::View*) {
        bass->slider->setProgress(0.5f);
        mid->slider->setProgress(0.5f);
        treble->slider->setProgress(0.5f);
        commitEqualizer();
        return true;
      });
  resetEqualizer->setMarginTop(8);
  resetEqualizer->setWidth(190);
  content->addView(resetEqualizer);

  auto* aboutHeader = new brls::Header();
  aboutHeader->setTitle("Application");
  aboutHeader->setMarginTop(16);
  content->addView(aboutHeader);
  content->addView(swui::makeStatePanel(
      "Switify 0.6.4",
      "Created by HiroshiYamauchi\nA controller-first native Spotify client "
      "for Nintendo Switch homebrew."));

  auto* about = swui::makeActionButton(
      "About and credits",
      [](brls::View*) {
        showInformationPage(
            "About Switify", "Version 0.6.4",
            {{"Created by HiroshiYamauchi",
              "A controller-first native Spotify client for Nintendo Switch "
              "homebrew."},
             {"Technology",
              "Spotify sessions, streaming, decryption, Vorbis decoding, and "
              "audout playback use the bundled cspot fork. The standalone "
              "interface uses Borealis; background playback uses libtesla, "
              "Ultrahand, and libnx."},
             {"Acknowledgements",
              "Thanks to librespot, psst, Spotix, Spotube, Streamfin, and "
              "sys-tune for research and inspiration."},
             {"License and status",
              "GPL-3.0-or-later. Provided without warranty. Switify is an "
              "independent, unofficial project and is not affiliated with "
              "Spotify or Nintendo."}});
        return true;
      });
  about->setMarginTop(12);
  about->setWidth(210);
  content->addView(about);

  auto* changelog = swui::makeActionButton(
      "Changelog",
      [](brls::View*) {
        showInformationPage(
            "Changelog", "Complete Switify development history",
            {{"Standalone 0.6.4 · complete cached collections",
              "Restored complete Liked Songs and playlists with Play All, "
              "multi-thousand-track session caching, and 100-row lazy UI "
              "batches. Added followed podcasts through Spotify's exact "
              "native Podcasts & Shows filter, reliable deferred Shuffle, "
              "and removed unused non-Vorbis decoders."},
             {"Overlay 0.4.4 · compatibility and paging",
              "Reduced the resident service and artwork footprint, fixed "
              "Nintendo 64 Nintendo Switch Online compatibility, added "
              "bounded automatic startup retries, demand-loaded collection "
              "pages, centered artwork, and a clear volume layout."},
             {"Standalone 0.6.3 · playback modes",
              "Made Shuffle reorder the queue that is already playing. Moved "
              "Repeat All and Repeat One into the native queue/player path so "
              "all repeats are immediate, deterministic, and race-free."},
             {"Standalone 0.6.2 · audio and efficiency",
              "Added persistent shared volume, Normal 160 kbps and High 320 "
              "kbps quality selection, a higher-quality fixed-ratio audio "
              "converter, stronger playback buffering, and underrun/CDN "
              "diagnostics."},
             {"Standalone 0.6.1 · search and information pages",
              "Restored the authenticated Pathfinder search used by the "
              "Spotify web player. Search no longer depends on the heavily "
              "rate-limited public Web API. Replaced overflowing About and "
              "Changelog dialogs with controller-friendly scrolling pages."},
             {"Standalone 0.6.0 · release interface",
              "Added Home content filters; Search filters for songs, artists, "
              "albums, playlists, and podcasts; podcast browsing; improved "
              "library-save state; the dark Switify icon; creator credit; and "
              "public release documentation."},
             {"Standalone 0.5.3 · large libraries",
              "Paged Liked Songs and long Now Playing sessions, restored "
              "responsive controller navigation and retained loaded content "
              "between tabs."},
             {"Standalone 0.5.0–0.5.2 · player controls",
              "Added drawn shuffle/previous/play-pause/next/repeat controls, "
              "Off/All/One repeat modes, live liked-state UI, optional "
              "Y/L/R/ZL/ZR shortcuts, and a three-band equalizer."},
             {"Standalone 0.4.9 · paused restoration",
              "Restored the last track, playlist, and position as paused "
              "metadata. Switify no longer starts playing merely because the "
              "app was opened."},
             {"Standalone 0.4.x · persistence and theater mode",
              "Added persistent artwork caching, an inline playlist below "
              "Now Playing, theater mode on Plus, screen-off playback on "
              "Minus, and reliable B-button navigation."},
             {"Standalone 0.3.x · complete browsing",
              "Expanded Home, Search, Your Library, albums, playlists, queue, "
              "resume contexts, pagination, and local playback-session "
              "tracking."},
             {"Standalone 0.2.2 · focus and connection recovery",
              "Fixed Borealis stale-focus crashes, added safe retry focus, "
              "bounded Spotify AP failover, and restored missing system UI "
              "assets."},
             {"Standalone 0.2.1 · cspot concurrency",
              "Serialized connection replacement and packet writes, protected "
              "Mercury callbacks, and prevented reconnect-time null-pointer "
              "crashes."},
             {"Standalone 0.2.0 · professional shell",
              "Introduced the five-section Borealis interface, mini-player, "
              "personalized Home, private library access, artwork workers, "
              "loading/error states, and diagnostics."},
             {"Standalone 0.1.0 · native playback proof",
              "Established phone-assisted Spotify authentication, cached "
              "credentials, native CDN streaming/decryption/Vorbis decoding, "
              "audout playback, transport controls, search, and clean exit."},
             {"Overlay 0.4.3 · playback modes",
              "Shares the corrected live Shuffle queue and native Repeat All "
              "and Repeat One implementation with the standalone player."},
             {"Overlay 0.4.2 · audio and lightweight profile",
              "Added shared persistent volume and quality selection, 25-item "
              "pages, smaller browse/catalog caches, lighter artwork memory, "
              "stronger PCM/CDN buffering, and playback diagnostics."},
             {"Overlay 0.4.1 · catalog correction",
              "Shares the corrected catalog search and updated release "
              "metadata while retaining the stable playback and volume "
              "interface."},
             {"Overlay 0.4.0 · public baseline",
              "Combined the full release browse model with the hardware-tested "
              "0.3.9 resource and playback profile."},
             {"Overlay 0.3.9 · visible audio controls",
              "Added the numeric volume slider and Off/All/One repeat modes."},
             {"Overlay 0.3.8 · network profile",
              "Reduced the background service to two TCP-only BSD sessions "
              "and removed unused UDP reservations."},
             {"Overlay 0.3.7 · applet-pool audio",
              "Moved the service to the Applet memory pool and made audout "
              "acquisition lazy until the first PCM frame."},
             {"Overlay 0.3.6 · recovery baseline",
              "Restored the proven 4 MiB heap, playback buffers, caches, and "
              "local B escape from loading views after the withdrawn 0.3.5 "
              "resource experiment."},
             {"Overlay 0.3.5 · withdrawn",
              "Withdrawn after hardware tests found silent playback, infinite "
              "collection loading, and a trapped Ultrahand navigation state."},
             {"Overlay 0.3.0–0.3.4 · background playback",
              "Established the independent Ultrahand UI, versioned CMIF IPC, "
              "dormant Atmosphere service, native playback over games, "
              "artwork, Spotify browsing, queue controls, and large-library "
              "pages."},
             {"Phase 2 probes 0.1.x–0.2.x",
              "Validated service discovery, launch behavior, IPC health, "
              "Atmosphere packaging, and the first background audio path "
              "before the production overlay was introduced."}});
        return true;
      });
  changelog->setMarginTop(10);
  changelog->setWidth(210);
  content->addView(changelog);

  auto* diagnostics = swui::makeStatePanel(
      "Diagnostics",
      "Runtime log: sdmc:/config/Switify/app.log\nCrash reports: "
      "sdmc:/atmosphere/crash_reports/");
  diagnostics->setMarginTop(16);
  content->addView(diagnostics);

  volume->getProgressEvent()->subscribe([this](float value) {
    if (!updatingVolume) {
      swsettings::setOutputVolume(value);
      SpotifyEngine::instance().setVolume(value);
    }
  });
  auto token = alive;
  observerId = SpotifyEngine::instance().subscribe([this, token] {
    brls::sync([this, token] {
      if (token->load()) refresh();
    });
  });
  refresh();
}

SettingsTab::~SettingsTab() {
  alive->store(false);
  SpotifyEngine::instance().unsubscribe(observerId);
}

void SettingsTab::commitEqualizer() {
  if (!equalizerEnabled || !bass || !mid || !treble) return;
  const float bassDb = equalizerDb(bass->slider->getProgress());
  const float midDb = equalizerDb(mid->slider->getProgress());
  const float trebleDb = equalizerDb(treble->slider->getProgress());
  bass->setDetailText(equalizerDbText(bassDb));
  mid->setDetailText(equalizerDbText(midDb));
  treble->setDetailText(equalizerDbText(trebleDb));
  const bool enabled = equalizerEnabled->isOn();
  swsettings::setEqualizer(enabled, bassDb, midDb, trebleDb);
  SpotifyEngine::instance().setEqualizer(enabled, bassDb, midDb, trebleDb);
}

void SettingsTab::refresh() {
  auto& engine = SpotifyEngine::instance();
  connection->setText(stateLabel(engine.state()) + "  |  " +
                      engine.statusText());
  const auto user = engine.profile();
  account->setText(user.displayName.empty()
                       ? "Spotify account details load after connection"
                       : "Signed in as " + user.displayName);
  const auto snapshot = engine.nowPlaying();
  volumeValue->setText(
      "Output volume " +
      std::to_string(static_cast<int>(snapshot.volume * 100)) + "%");
  if (!volume->getDefaultFocus()->isFocused()) {
    updatingVolume = true;
    volume->setProgress(snapshot.volume);
    updatingVolume = false;
  }
}
