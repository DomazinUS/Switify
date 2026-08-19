#include "track_list_activity.hpp"

#include "platform_controls.hpp"
#include "playback_shortcuts.hpp"
#include "tabs.hpp"
#include "ui_helpers.hpp"

TrackListActivity::TrackListActivity(SpotifyEngine::Collection collection)
    : collection(std::move(collection)),
      alive(std::make_shared<std::atomic_bool>(true)) {}

TrackListActivity::~TrackListActivity() { alive->store(false); }

void TrackListActivity::onContentAvailable() {
  registerPlaybackShortcuts(this);
  registerAction(
      "Player", brls::BUTTON_START,
      [](brls::View*) {
        showTheaterMode();
        return true;
      });
  registerAction(
      "Screen off", brls::BUTTON_BACK,
      [](brls::View*) {
        if (!swplatform::toggleScreenOff())
          brls::Application::notify(
              "Screen-off mode is unavailable on this console");
        return true;
      });

  title->setText(collection.name);
  subtitle->setText(collection.subtitle.empty() ? "Spotify collection"
                                                 : collection.subtitle);
  description->setText(collection.description);
  description->setVisibility(collection.description.empty()
                                 ? brls::Visibility::GONE
                                 : brls::Visibility::VISIBLE);
  if (!collection.imageUrl.empty())
    swui::loadRemoteImage(artwork, collection.imageUrl);

  playAll->setFocusable(false);
  playAll->setBackgroundColor(swui::surfaceRaised());
  playAll->registerClickAction([this](brls::View*) {
    if (!currentTracks.empty())
      SpotifyEngine::instance().playTracks(currentTracks, 0, collection.name);
    return true;
  });
  playAll->addGestureRecognizer(new brls::TapGestureRecognizer(playAll));
  playAll->getFocusEvent()->subscribe([this](brls::View*) {
    playAllLabel->setTextColor(swui::textPrimary());
  });
  playAll->getFocusLostEvent()->subscribe([this](brls::View*) {
    playAllLabel->setTextColor(nvgRGB(8, 20, 12));
  });

  std::vector<SpotifyEngine::Track> cachedTracks;
  if (SpotifyEngine::instance().cachedPlaylistTracks(collection,
                                                     cachedTracks)) {
    SpotifyEngine::Result<std::vector<SpotifyEngine::Track>> cached;
    cached.value = std::move(cachedTracks);
    showTracks(std::move(cached));
    return;
  }

  const bool podcast = collection.kind == SpotifyEngine::Collection::Kind::SHOW;
  list->addView(swui::makeStatePanel(
      podcast ? "Loading episodes" : "Loading tracks",
      podcast ? "Fetching episodes from Spotify."
              : "Fetching this collection from Spotify."));

  auto token = alive;
  SpotifyEngine::instance().fetchPlaylistTracks(
      collection,
      [this, token](SpotifyEngine::Result<std::vector<SpotifyEngine::Track>> result) mutable {
        brls::sync([this, token, result = std::move(result)]() mutable {
          if (!token->load()) return;
          showTracks(std::move(result));
        });
      });
}

void TrackListActivity::showTracks(
    SpotifyEngine::Result<std::vector<SpotifyEngine::Track>> result) {
  list->clearViews();
  if (!result.ok()) {
    subtitle->setText("Could not load this collection");
    list->addView(swui::makeStatePanel("Spotify returned an error", result.error,
                                       nvgRGB(55, 31, 36)));
    return;
  }
  currentTracks = std::move(result.value);
  if (currentTracks.empty()) {
    const bool podcast =
        collection.kind == SpotifyEngine::Collection::Kind::SHOW;
    subtitle->setText(podcast ? "No playable episodes"
                              : "No playable tracks");
    list->addView(swui::makeStatePanel(
        podcast ? "This podcast has no episodes"
                : "This collection is empty",
        podcast ? "Spotify did not return any playable episodes for this show."
                : "Spotify did not return any playable items for this collection."));
    return;
  }

  const bool podcast = collection.kind == SpotifyEngine::Collection::Kind::SHOW;
  std::string detail = std::to_string(currentTracks.size()) +
                       (podcast ? " episodes" : " songs");
  if (!collection.subtitle.empty()) detail += "  |  " + collection.subtitle;
  subtitle->setText(detail);
  playAllLabel->setText(podcast ? "Play episodes" : "Play all");
  playAll->setFocusable(true);
  playAll->setBackgroundColor(swui::accent());
  playAllLabel->setTextColor(playAll->isFocused() ? swui::textPrimary()
                                                  : nvgRGB(8, 20, 12));

  renderedTrackCount = 0;
  loadMoreButton = nullptr;
  appendTrackRows();
}

void TrackListActivity::appendTrackRows(bool focusFirstNew) {
  constexpr size_t RenderBatchSize = 100;
  const size_t end =
      std::min(currentTracks.size(), renderedTrackCount + RenderBatchSize);
  brls::View* firstNew = nullptr;

  for (size_t index = renderedTrackCount; index < end; ++index) {
    auto* row = new swui::TrackRow();
    row->setTrack(currentTracks[index], index, [this, index] {
      SpotifyEngine::instance().playTracks(currentTracks, index,
                                           collection.name);
    });
    list->addView(row);
    if (!firstNew) firstNew = row;
  }
  renderedTrackCount = end;

  if (renderedTrackCount < currentTracks.size()) {
    const size_t remaining = currentTracks.size() - renderedTrackCount;
    loadMoreButton = swui::makeActionButton(
        std::string("Load more ") +
            (collection.kind == SpotifyEngine::Collection::Kind::SHOW
                 ? "episodes ("
                 : "songs (") +
            std::to_string(remaining) + " remaining)",
        [this, token = alive](brls::View*) {
          if (!token->load() || !loadMoreButton) return true;
          auto* previousButton = loadMoreButton;
          loadMoreButton = nullptr;
          previousButton->setFocusable(false);
          brls::sync([this, token, previousButton] {
            if (!token->load()) return;
            list->removeView(previousButton);
            appendTrackRows(true);
          });
          return true;
        });
    loadMoreButton->setMarginTop(10);
    list->addView(loadMoreButton);
  }

  if (focusFirstNew && firstNew) brls::Application::giveFocus(firstNew);
}
