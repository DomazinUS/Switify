#include "ApResolve.h"

#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <memory>            // for allocator, unique_ptr
#include <stdexcept>         // for runtime_error
#include <string_view>       // for string_view
#include <vector>            // for vector

#include "HTTPClient.h"  // for HTTPClient, HTTPClient::Response
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

ApResolve::ApResolve(std::string apOverride) {
  this->apOverride = apOverride;
}

std::string ApResolve::fetchFirstApAddress() {
  auto addresses = fetchApAddresses();
  if (addresses.empty())
    throw std::runtime_error("Spotify AP resolver returned no endpoints");
  return addresses.front();
}

std::vector<std::string> ApResolve::fetchApAddresses() {
  if (apOverride != "") {
    return {apOverride};
  }

  auto request = bell::HTTPClient::get("https://apresolve.spotify.com/");
  std::string_view responseStr = request->body();

  // parse json with nlohmann
#ifdef BELL_ONLY_CJSON
  cJSON* json = cJSON_Parse(responseStr.data());
  if (!json)
    throw std::runtime_error("Spotify AP resolver returned invalid JSON");
  cJSON* list = cJSON_GetObjectItem(json, "ap_list");
  std::vector<std::string> addresses;
  if (cJSON_IsArray(list)) {
    const int count = cJSON_GetArraySize(list);
    addresses.reserve(count);
    for (int index = 0; index < count; ++index) {
      cJSON* item = cJSON_GetArrayItem(list, index);
      if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
        addresses.emplace_back(item->valuestring);
    }
  }
  cJSON_Delete(json);
  if (addresses.empty())
    throw std::runtime_error("Spotify AP resolver returned no endpoints");
  return addresses;
#else
  auto json = nlohmann::json::parse(responseStr);
  if (!json.contains("ap_list") || !json["ap_list"].is_array())
    throw std::runtime_error("Spotify AP resolver returned no endpoint list");
  std::vector<std::string> addresses;
  for (const auto& item : json["ap_list"]) {
    if (item.is_string() && !item.get_ref<const std::string&>().empty())
      addresses.push_back(item.get<std::string>());
  }
  if (addresses.empty())
    throw std::runtime_error("Spotify AP resolver returned no endpoints");
  return addresses;
#endif
}
