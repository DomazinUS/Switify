#pragma once

#include <string>  // for string
#include <vector>  // for vector
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#endif

namespace cspot {
class ApResolve {
 public:
  ApResolve(std::string apOverride);

  /**
   * @brief Resolves Spotify's currently advertised access points.
   * @returns Addresses in host:port form, in Spotify's preferred order.
   */
  std::vector<std::string> fetchApAddresses();

  // Retained for callers that explicitly need the first advertised endpoint.
  std::string fetchFirstApAddress();

 private:
  std::string apOverride;
};
}  // namespace cspot
