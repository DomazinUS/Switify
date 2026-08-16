#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

#include "spotify_engine.hpp"

namespace swui {

constexpr float kPagePadding = 42.0f;

NVGcolor background();
NVGcolor surface();
NVGcolor surfaceRaised();
NVGcolor textPrimary();
NVGcolor textSecondary();
NVGcolor accent();
NVGcolor danger();

std::string formatDuration(uint32_t milliseconds);
void loadRemoteImage(brls::Image* image, const std::string& url);
void shutdownRemoteImages();

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color,
                       bool singleLine = false);
brls::Box* makeActionButton(const std::string& text,
                            std::function<bool(brls::View*)> action,
                            bool primary = false,
                            brls::Label** labelOut = nullptr);

class FilterChip : public brls::Box {
 public:
  FilterChip(const std::string& text, std::function<void()> action);
  void setSelected(bool selected);
  bool isSelected() const { return selected; }

 private:
  void applyStyle(bool focused);

  brls::Label* label = nullptr;
  bool selected = false;
};

// Font-independent repeat symbol. The Switch system font does not contain the
// Unicode repeat-arrow glyph, so this view draws the two arrows directly.
class RepeatIcon : public brls::View {
 public:
  RepeatIcon();
  void setMode(SpotifyEngine::RepeatMode mode);
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  SpotifyEngine::RepeatMode mode = SpotifyEngine::RepeatMode::Off;
};

brls::Box* makeRepeatButton(std::function<bool(brls::View*)> action,
                            RepeatIcon** iconOut = nullptr);

class ShuffleIcon : public brls::View {
 public:
  ShuffleIcon();
  void setActive(bool active);
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  bool active = false;
};

brls::Box* makeShuffleButton(std::function<bool(brls::View*)> action,
                             ShuffleIcon** iconOut = nullptr);
brls::Box* makeStatePanel(const std::string& title,
                          const std::string& detail,
                          NVGcolor tint = surfaceRaised());

class MediaCard : public brls::Box {
 public:
  MediaCard();

  void setContent(const std::string& title, const std::string& subtitle,
                  const std::string& imageUrl);
  void setAction(std::function<void()> action);

 private:
  brls::Image* artwork = nullptr;
  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  std::function<void()> onSelected;
};

class TrackRow : public brls::Box {
 public:
  TrackRow();

  void setTrack(const SpotifyEngine::Track& track, size_t index,
                std::function<void()> action);
  void setCompact(bool compact);
  void setCurrent(bool current);

 private:
  brls::Label* indexLabel = nullptr;
  brls::Image* artwork = nullptr;
  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  brls::Label* durationLabel = nullptr;
  bool artworkEnabled = true;
  std::function<void()> onSelected;
};

class HorizontalSection : public brls::Box {
 public:
  explicit HorizontalSection(const std::string& title,
                             const std::string& subtitle = "");

  void addCard(MediaCard* card);
  void showEmpty(const std::string& text);

 private:
  brls::HScrollingFrame* scroller = nullptr;
  brls::Box* row = nullptr;
};

}  // namespace swui
