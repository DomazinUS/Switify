#include "platform_controls.hpp"

#include "NXConsoleLog.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace swplatform {
namespace {
bool initialized = false;
bool backlightAvailable = false;
bool appletBacklightAvailable = false;
bool screenOff = false;
}  // namespace

void initialize() {
  if (initialized) return;
  initialized = true;
#ifdef __SWITCH__
  const Result result = lblInitialize();
  backlightAvailable = R_SUCCEEDED(result);
  Result appletResult = 0;
  if (!backlightAvailable) {
    appletResult = appletSetLcdBacklightOffEnabled(false);
    appletBacklightAvailable = R_SUCCEEDED(appletResult);
  }
  nxlog::pushf("Backlight service: %s (0x%x)",
               backlightAvailable
                   ? "lbl ready"
                   : (appletBacklightAvailable ? "applet fallback ready"
                                               : "unavailable"),
               backlightAvailable ? result : appletResult);
#endif
}

bool toggleScreenOff() {
  initialize();
#ifdef __SWITCH__
  if (!backlightAvailable && !appletBacklightAvailable) return false;
  const bool target = !screenOff;
  // Keep Horizon from dimming or suspending playback while Switify has
  // deliberately blanked the built-in panel.
  appletSetMediaPlaybackState(target);
  const Result result = backlightAvailable
                            ? (target ? lblSwitchBacklightOff(250000000ULL)
                                      : lblSwitchBacklightOn(250000000ULL))
                            : appletSetLcdBacklightOffEnabled(target);
  if (R_FAILED(result)) {
    if (target) appletSetMediaPlaybackState(false);
    nxlog::pushf("Backlight %s failed: 0x%x", target ? "off" : "on",
                 result);
    return false;
  }
  screenOff = target;
  nxlog::pushf("Backlight switched %s", screenOff ? "off" : "on");
  return true;
#else
  return false;
#endif
}

bool isScreenOff() { return screenOff; }

void shutdown() {
  if (!initialized) return;
#ifdef __SWITCH__
  if (backlightAvailable && screenOff) lblSwitchBacklightOn(0);
  if (appletBacklightAvailable && screenOff)
    appletSetLcdBacklightOffEnabled(false);
  appletSetMediaPlaybackState(false);
  if (backlightAvailable) lblExit();
#endif
  screenOff = false;
  backlightAvailable = false;
  appletBacklightAvailable = false;
  initialized = false;
}

}  // namespace swplatform
