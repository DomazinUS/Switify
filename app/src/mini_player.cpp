#include "mini_player.hpp"

#include "ui_helpers.hpp"

MiniPlayer::MiniPlayer()
    : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::ROW);
  setAlignItems(brls::AlignItems::CENTER);
  setHeight(82);
  setPadding(9, 34, 9, 34);
  setBackgroundColor(nvgRGB(17, 20, 24));

  artwork = new brls::Image();
  artwork->setDimensions(62, 62);
  artwork->setCornerRadius(6);
  artwork->setBackgroundColor(swui::surfaceRaised());
  artwork->setScalingType(brls::ImageScalingType::FIT);
  addView(artwork);

  auto* text = new brls::Box();
  text->setAxis(brls::Axis::COLUMN);
  text->setGrow(1.0f);
  text->setMarginLeft(14);
  title = swui::makeLabel("Switify is starting", 18, swui::textPrimary(), true);
  subtitle = swui::makeLabel("Native Spotify playback", 14,
                             swui::textSecondary(), true);
  subtitle->setMarginTop(3);
  text->addView(title);
  text->addView(subtitle);
  addView(text);

  state = swui::makeLabel("STARTING", 13, swui::accent(), true);
  state->setMarginRight(18);
  addView(state);

  brls::Label* previousLabel = nullptr;
  auto* previous = swui::makeActionButton(
      "|◀",
      [](brls::View*) {
        SpotifyEngine::instance().previous();
        return true;
      },
      false, &previousLabel);
  previousLabel->setFontSize(21);
  previous->setWidth(64);
  previous->setMarginRight(8);
  auto* play = swui::makeActionButton(
      "▶",
      [](brls::View*) {
        SpotifyEngine::instance().playPause();
        return true;
      },
      true, &playPauseLabel);
  playPauseLabel->setFontSize(22);
  play->setWidth(66);
  play->setMarginRight(8);
  brls::Label* nextLabel = nullptr;
  auto* next = swui::makeActionButton(
      "▶|",
      [](brls::View*) {
        SpotifyEngine::instance().next();
        return true;
      },
      false, &nextLabel);
  nextLabel->setFontSize(21);
  next->setWidth(64);
  addView(previous);
  addView(play);
  addView(next);

  auto token = alive;
  observerId = SpotifyEngine::instance().subscribe([this, token] {
    brls::sync([this, token] {
      if (token->load()) refresh();
    });
  });
  refresh();
}

MiniPlayer::~MiniPlayer() {
  alive->store(false);
  SpotifyEngine::instance().unsubscribe(observerId);
}

void MiniPlayer::refresh() {
  auto& engine = SpotifyEngine::instance();
  const auto snapshot = engine.nowPlaying();
  if (snapshot.hasTrack) {
    title->setText(snapshot.name);
    std::string detail = snapshot.artist;
    if (!snapshot.album.empty()) detail += "  |  " + snapshot.album;
    subtitle->setText(detail);
    state->setText(snapshot.paused ? "PAUSED" : "PLAYING");
    playPauseLabel->setText(snapshot.paused ? "▶" : "II");
    if (!snapshot.imageUrl.empty() && snapshot.imageUrl != loadedArtworkUrl) {
      loadedArtworkUrl = snapshot.imageUrl;
      swui::loadRemoteImage(artwork, snapshot.imageUrl);
    }
  } else {
    title->setText(engine.statusText().empty() ? "Switify" : engine.statusText());
    subtitle->setText("Browse Home, Search, or Your Library");
    switch (engine.state()) {
      case SpotifyEngine::State::Ready:
        state->setText("READY");
        state->setTextColor(swui::accent());
        break;
      case SpotifyEngine::State::Error:
        state->setText("ERROR");
        state->setTextColor(swui::danger());
        break;
      case SpotifyEngine::State::WaitingForPhone:
        state->setText("PAIRING");
        state->setTextColor(swui::textSecondary());
        break;
      default:
        state->setText("CONNECTING");
        state->setTextColor(swui::accent());
        break;
    }
    playPauseLabel->setText("▶");
  }
}
