// Switify POC — Phase 0 gate.
//
// Foreground console homebrew that proves the whole chain on real hardware:
//   TLS + sockets under libnx -> Spotify zeroconf auth from the phone app ->
//   session/auth -> CDN fetch + AES decrypt -> Vorbis decode -> audout.
//
// Controls:
//   A     play/pause
//   L / R previous / next track
//   +     exit
//
// Files on SD:
//   /config/switify-poc/config.json       {"clientId": "...", "clientSecret": "..."}
//   /config/switify-poc/credentials.json  written automatically after first auth

#include <switch.h>

#include <sys/stat.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "BellLogger.h"
#include "CSpotContext.h"
#include "Logger.h"
#include "LoginBlob.h"
#include "MDNSService.h"
#include "NXAudioSink.h"
#include "NXConsoleLog.h"
#include "NXPlayer.h"
#include "SpircHandler.h"
#include "WrappedSemaphore.h"
#include "ZeroconfHttpServer.h"
#include "nlohmann/json.hpp"

static const char* CONFIG_DIR = "/config/switify-poc";
static const char* CONFIG_FILE = "/config/switify-poc/config.json";
static const char* CREDENTIALS_FILE = "/config/switify-poc/credentials.json";
static const int ZEROCONF_PORT = 7864;

static std::string readFile(const char* path) {
  std::ifstream file(path);
  if (!file.good())
    return "";
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  consoleInit(NULL);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  socketInitializeDefault();

  printf("Switify POC - Spotify Connect receiver\n");
  printf("--------------------------------------\n\n");
  consoleUpdate(NULL);

  // Route ALL bell/cspot logging through a thread-safe queue: the libnx
  // console is not thread-safe and worker-thread printf crashes the app.
  nxlog::installBellLogger();

  mkdir("/config", 0755);
  mkdir(CONFIG_DIR, 0755);

  // --- Load developer-app credentials (required by the 2026 protocol path)
  std::string clientId, clientSecret;
  {
    std::string cfg = readFile(CONFIG_FILE);
    if (!cfg.empty()) {
      try {
        auto json = nlohmann::json::parse(cfg);
        clientId = json.value("clientId", "");
        clientSecret = json.value("clientSecret", "");
      } catch (...) {}
    }
  }
  if (clientId.empty() || clientSecret.empty()) {
    printf("ERROR: missing %s\n\n", CONFIG_FILE);
    printf("Create it with your Spotify developer app credentials:\n");
    printf("  {\"clientId\": \"...\", \"clientSecret\": \"...\"}\n\n");
    printf("(developer.spotify.com -> Dashboard -> Create app)\n\n");
    printf("Press + to exit.\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
        break;
      consoleUpdate(NULL);
      svcSleepThread(16'000'000);
    }
    socketExit();
    consoleExit(NULL);
    return 0;
  }

  auto loggedInSemaphore = std::make_shared<bell::WrappedSemaphore>(1);
  auto loginBlob = std::make_shared<cspot::LoginBlob>("Switify (Switch)");
  std::atomic<bool> isRunning = true;

  // --- Auth: cached credentials if present, zeroconf otherwise
  std::unique_ptr<ZeroconfHttpServer> zeroconfServer;
  std::unique_ptr<bell::MDNSService> mdnsService;

  std::string cachedCredentials = readFile(CREDENTIALS_FILE);
  if (!cachedCredentials.empty()) {
    printf("Using cached credentials from %s\n", CREDENTIALS_FILE);
    loginBlob->loadJson(cachedCredentials);
    loggedInSemaphore->give();
  } else {
    zeroconfServer = std::make_unique<ZeroconfHttpServer>(
        ZEROCONF_PORT,
        [loginBlob]() { return loginBlob->buildZeroconfInfo(); },
        [loginBlob, loggedInSemaphore](std::map<std::string, std::string> query) {
          nxlog::pushf("Received zeroconf credentials from the Spotify app");
          // Runs on the HTTP server thread: an uncaught exception here
          // terminates the whole process. Catch and report instead.
          try {
            loginBlob->loadZeroconfQuery(query);
            loggedInSemaphore->give();
          } catch (const std::exception& e) {
            nxlog::pushf("ERROR decoding credential blob: %s", e.what());
          } catch (...) {
            nxlog::pushf("ERROR decoding credential blob (unknown)");
          }
        },
        [&isRunning]() { isRunning = false; });

    if (!zeroconfServer->start()) {
      printf("ERROR: could not start the zeroconf HTTP server\n");
    }

    mdnsService = bell::MDNSService::registerService(
        loginBlob->getDeviceName(), "_spotify-connect", "_tcp", "",
        ZEROCONF_PORT,
        {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});

    printf("Waiting for the Spotify app...\n");
    printf("Open Spotify on your phone (same Wi-Fi) and pick\n");
    printf("\"Switify (Switch)\" in the Connect device list.\n\n");
    consoleUpdate(NULL);
  }

  // Wait for auth on a worker thread so the console stays responsive
  std::atomic<bool> authDone = false;
  std::thread authWaiter([&]() {
    loggedInSemaphore->wait();
    authDone = true;
  });

  while (appletMainLoop() && !authDone && isRunning) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
      isRunning = false;
      loggedInSemaphore->give();  // unblock the waiter
    }
    nxlog::drain();
    consoleUpdate(NULL);
    svcSleepThread(16'000'000);
  }
  authWaiter.join();

  if (!isRunning || !appletMainLoop()) {
    if (zeroconfServer)
      zeroconfServer->stop();
    socketExit();
    consoleExit(NULL);
    return 0;
  }

  printf("Authenticated. Connecting to Spotify...\n");
  consoleUpdate(NULL);

  int exitCode = 0;
  try {
    auto ctx = cspot::Context::createFromBlob(loginBlob);
    ctx->config.clientId = clientId;
    ctx->config.clientSecret = clientSecret;

    ctx->session->connectWithRandomAp();
    ctx->config.authData = ctx->session->authenticate(loginBlob);

    if (ctx->config.authData.size() > 0) {
      // Persist reusable credentials for the next launch
      {
        std::ofstream file(CREDENTIALS_FILE);
        file << ctx->getCredentialsJson();
      }

      auto handler = std::make_shared<cspot::SpircHandler>(ctx);
      ctx->session->startTask();

      auto audioSink = std::make_unique<NXAudioSink>();
      if (!audioSink->ok()) {
        printf("ERROR: audout init failed\n");
      }
      audioSink->setParams(44100, 2, 16);

      auto player = std::make_shared<NXPlayer>(std::move(audioSink), handler);

      // Session packet pump on a worker thread; input on the main thread
      std::thread sessionThread([&]() {
        while (isRunning) {
          try {
            ctx->session->handlePacket();
          } catch (const std::exception& e) {
            nxlog::pushf("Session error: %s", e.what());
            isRunning = false;
          }
        }
      });

      nxlog::drain();
      printf("\nReady. A: play/pause  L/R: prev/next  +: exit\n\n");
      consoleUpdate(NULL);

      bool paused = false;
      while (appletMainLoop() && isRunning) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) {
          isRunning = false;
        }
        if (down & HidNpadButton_A) {
          paused = !paused;
          handler->setPause(paused);
        }
        if (down & HidNpadButton_L) {
          handler->previousSong();
        }
        if (down & HidNpadButton_R) {
          handler->nextSong();
        }

        nxlog::drain();
        consoleUpdate(NULL);
        svcSleepThread(16'000'000);
      }

      handler->disconnect();
      player->disconnect();
      // handlePacket() may be blocked on the socket; closing the session
      // via disconnect above unblocks it on the next packet/teardown.
      if (sessionThread.joinable())
        sessionThread.join();
    } else {
      printf("ERROR: authentication against Spotify AP failed\n");
      exitCode = 1;
    }
  } catch (const std::exception& e) {
    printf("FATAL: %s\n", e.what());
    exitCode = 1;
  }

  if (zeroconfServer)
    zeroconfServer->stop();
  if (mdnsService)
    mdnsService->unregisterService();

  printf("\nExiting in 3 seconds...\n");
  consoleUpdate(NULL);
  svcSleepThread(3'000'000'000);

  socketExit();
  consoleExit(NULL);
  return exitCode;
}
