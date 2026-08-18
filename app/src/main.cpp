// Switify — native Spotify client for Nintendo Switch.

#include <borealis.hpp>
#include <borealis/core/cache_helper.hpp>

#ifdef __SWITCH__
#include <arpa/inet.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>
#endif

#include "NXConsoleLog.h"
#include "app_settings.hpp"
#include "mini_player.hpp"
#include "playback_shortcuts.hpp"
#include "platform_controls.hpp"
#include "spotify_engine.hpp"
#include "tabs.hpp"
#include "ui_helpers.hpp"

class MainActivity : public brls::Activity {
 public:
  CONTENT_FROM_XML_RES("activity/main.xml");

  void onContentAvailable() override {
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
          else if (!swplatform::isScreenOff())
            brls::Application::notify("Screen restored");
          return true;
        });
  }
};

int main(int argc, char* argv[]) {
  brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

#ifdef __SWITCH__
  int nxlinkSocket = -1;
#endif

  // bell/cspot log through a global pointer that is NULL until installed —
  // the first log call from a worker thread would otherwise crash.
  nxlog::installBellLogger();
#ifdef __SWITCH__
  // Create the destination before opening the log. The engine also creates
  // it defensively, but it starts after early Borealis/cspot diagnostics.
  mkdir("/config", 0755);
  mkdir("/config/Switify", 0755);
  nxlog::setLogFile("/config/Switify/app.log");
  // HBMenu fills __nxlink_host for NetLoader launches. Open the SD log first,
  // so a firewall or host-side live-log failure still leaves diagnostics.
  if (__nxlink_host.s_addr != 0) {
    nxlog::push("NetLoader launch detected; connecting live log channel");
    nxlinkSocket = nxlinkStdio();
  }
  nxlog::pushf("Switify 0.6.4 starting (nxlink live logs: %s)",
               nxlinkSocket >= 0 ? "connected" : "off");
#endif

  if (!brls::Application::init()) {
    brls::Logger::error("Unable to init Borealis");
    return EXIT_FAILURE;
  }

  brls::Application::createWindow("Switify");
  brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
  brls::Application::setGlobalQuit(false);
  brls::TextureCache::instance().cache.setCapacity(128);
  swplatform::initialize();

  // Borealis' stock sidebar is sized for settings-heavy apps. Switify has five
  // short destinations; a compact rail gives Now Playing and media shelves the
  // horizontal room they need at 1280x720.
  auto style = brls::Application::getStyle();
  style.addMetric("brls/tab_frame/sidebar_width", 280.0f);
  style.addMetric("brls/sidebar/padding_left", 32.0f);
  style.addMetric("brls/sidebar/padding_right", 16.0f);
  style.addMetric("brls/sidebar/item_height", 58.0f);

  brls::Application::registerXMLView("HomeTab", HomeTab::create);
  brls::Application::registerXMLView("SearchTab", SearchTab::create);
  brls::Application::registerXMLView("LibraryTab", LibraryTab::create);
  brls::Application::registerXMLView("NowPlayingTab", NowPlayingTab::create);
  brls::Application::registerXMLView("SettingsTab", SettingsTab::create);
  brls::Application::registerXMLView("MiniPlayer", MiniPlayer::create);

  swsettings::load();
  const auto settings = swsettings::get();
  SpotifyEngine::instance().setVolume(settings.outputVolume);
  SpotifyEngine::instance().setHighQuality(settings.highQuality);
  SpotifyEngine::instance().setEqualizer(
      settings.equalizerEnabled, settings.bassDb, settings.midDb,
      settings.trebleDb);
  SpotifyEngine::instance().start();

  brls::Application::pushActivity(new MainActivity());

  while (brls::Application::mainLoop()) {
    // cspot workers only enqueue messages. Draining here keeps libnx console
    // output on the main thread and also enables nxlink live diagnostics.
#ifdef __SWITCH__
    if (nxlinkSocket >= 0) nxlog::drain();
#else
    nxlog::drain();
#endif
  }

  swplatform::shutdown();
  swui::shutdownRemoteImages();
  SpotifyEngine::instance().shutdown();
#ifdef __SWITCH__
  if (nxlinkSocket >= 0) nxlog::drain();
  if (nxlinkSocket >= 0) close(nxlinkSocket);
#else
  nxlog::drain();
#endif
  return EXIT_SUCCESS;
}
