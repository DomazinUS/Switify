#include "AccessKeyFetcher.h"

#include <algorithm>         // for min
#include <chrono>            // for steady_clock
#include <cstring>           // for strrchr
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <type_traits>       // for remove_extent_t
#include <vector>            // for vector

#include "BellLogger.h"    // for AbstractLogger
#include "BellUtils.h"     // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "Logger.h"            // for CSPOT_LOG
#include "MercurySession.h"    // for MercurySession, MercurySession::Res...
#include "NanoPBExtensions.h"  // for bell::nanopb::encode...
#include "NanoPBHelper.h"      // for pbEncode and pbDecode
#include "Packet.h"            // for cspot
#include "TimeProvider.h"      // for TimeProvider
#include "Utils.h"             // for string_format

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

namespace {

constexpr uint32_t InitialRetryDelayMs = 1000;
constexpr uint32_t MaximumRetryDelayMs = 30000;

int64_t monotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

AccessKeyFetcher::AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx)
    : ctx(ctx) {}

bool AccessKeyFetcher::isExpired() {
  if (accessKey.empty()) {
    return true;
  }

  if (ctx->timeProvider->getSyncedTimestamp() > expiresAt) {
    return true;
  }

  return false;
}

std::string AccessKeyFetcher::getAccessKey() {
  if (!isExpired()) {
    return accessKey;
  }

  updateAccessKey();

  return accessKey;
}

void AccessKeyFetcher::updateAccessKey() {
  if (monotonicMs() < retryAfterMonotonicMs.load()) {
    return;
  }

  bool expected = false;
  if (!keyPending.compare_exchange_strong(expected, true)) {
    // Already pending refresh request
    return;
  }

  CSPOT_LOG(info, "Access token expired, fetching new one...");
  try {
    if (ctx->config.accessTokenProvider)
      accessKey = ctx->config.accessTokenProvider();
    if (!accessKey.empty()) {
      // The host refreshes Login5 tokens before expiry. Re-check periodically
      // so a long-running player never retains a stale bearer indefinitely.
      expiresAt = ctx->timeProvider->getSyncedTimestamp() + 15 * 60 * 1000LL;
      retryAfterMonotonicMs = 0;
      retryDelayMs = InitialRetryDelayMs;
      CSPOT_LOG(info, "Access token successfully fetched");
    }
  } catch (const std::exception& error) {
    CSPOT_LOG(error, "Failed to fetch access token: %s", error.what());
    accessKey.clear();
  }

  if (accessKey.empty()) {
    const uint32_t delay = retryDelayMs.load();
    retryAfterMonotonicMs = monotonicMs() + delay;
    retryDelayMs = std::min(delay * 2, MaximumRetryDelayMs);
    CSPOT_LOG(error, "Access token unavailable; retrying in %u ms", delay);
  }
  keyPending = false;
}
