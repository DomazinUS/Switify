// Switify — native Spotify client for Nintendo Switch.

#include <borealis.hpp>
#include <borealis/core/cache_helper.hpp>

#include <atomic>
#include <cstring>

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

namespace {

std::atomic_bool runtimeShutdownStarted{false};

bool requiresSphairaApplicationSafeExit() {
#ifdef __SWITCH__
  // Sphaira identifies its HBL through EntryType_EndOfList. Returning a
  // service-heavy NRO through that loader while it is hosted by an application
  // forwarder can leave HBL's appletOE self-exit request failing with 0xD401.
  // Applet-mode Sphaira returns normally and must keep its regular handoff.
  const char* loader = envGetLoaderInfo();
  return loader && std::strncmp(loader, "sphaira ", 8) == 0 &&
         appletGetAppletType() == AppletType_Application;
#else
  return false;
#endif
}

void shutdownRuntime() {
  if (runtimeShutdownStarted.exchange(true)) return;

  // Borealis fires its exit event before deleting the activity tree, stopping
  // its sync worker, and destroying the Switch platform. Stop every Switify
  // worker while those dependencies are still alive. Doing this only after
  // mainLoop() returned left quick exits racing live dashboard/artwork work.
  nxlog::push("Runtime shutdown begin");
  SpotifyEngine::instance().shutdown();
  swui::shutdownRemoteImages();
  swplatform::shutdown();
  nxlog::push("Runtime shutdown complete");
}

}  // namespace

int main(int argc, char* argv[]) {
  brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

#ifdef __SWITCH__
  int nxlinkSocket = -1;
  const bool sphairaApplicationSafeExit =
      requiresSphairaApplicationSafeExit();
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
  nxlog::pushf("Switify 0.7.1 starting (nxlink live logs: %s)",
               nxlinkSocket >= 0 ? "connected" : "off");
  if (sphairaApplicationSafeExit)
    nxlog::push("Sphaira application loader detected; safe HOME exit enabled");
#endif

  if (!brls::Application::init()) {
    brls::Logger::error("Unable to init Borealis");
    return EXIT_FAILURE;
  }

  brls::Application::createWindow("Switify");
  brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
  brls::Application::setGlobalQuit(false);
  // Subscribe before TextureCache installs its own exit callback, ensuring
  // remote image workers cannot touch the cache after Borealis cleans it.
  brls::Application::getExitEvent()->subscribe([] { shutdownRuntime(); });
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

  // Non-Borealis platforms or an initialization edge case may return without
  // firing the event. Keep cleanup idempotent as a final safety net.
  shutdownRuntime();
#ifdef __SWITCH__
  if (nxlinkSocket >= 0) nxlog::drain();
  if (sphairaApplicationSafeExit) {
    // Ask Horizon to close the application while libnx's valid applet proxy
    // and ISelfController are still alive. Sphaira HBL attempts the same Exit
    // command only after the NRO and its applet sessions are gone; obtaining a
    // fresh proxy at that point is the request that fails with 0xD401.
    fsdevCommitDevice("sdmc");
    Service* self = appletGetServiceSession_SelfController();
    const Result result =
        serviceIsActive(self) ? serviceDispatch(self, 0)
                              : KERNELRESULT(InvalidState);
    nxlog::pushf("Sphaira safe HOME exit request: 0x%x", result);
    nxlog::drain();
    if (nxlinkSocket >= 0) {
      close(nxlinkSocket);
      nxlinkSocket = -1;
    }

    if (R_SUCCEEDED(result)) {
      // This mirrors Sphaira HBL's successful selfExit path: the applet
      // manager owns termination now, so do not return to HBL and race it.
      while (true) svcSleepThread(86400000000000ULL);
    }

    nxlog::push("Sphaira safe HOME exit request failed; returning to loader");
    nxlog::drain();
  }
  if (nxlinkSocket >= 0) close(nxlinkSocket);
#else
  nxlog::drain();
#endif
  return EXIT_SUCCESS;
}
