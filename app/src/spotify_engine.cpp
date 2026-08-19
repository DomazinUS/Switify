#include "spotify_engine.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#ifdef __SWITCH__
#include <curl/curl.h>
#endif

#include "BellLogger.h"
#include "CSpotContext.h"
#include "HTTPClient.h"
#include "LoginBlob.h"
#include "MDNSService.h"
#include "MercurySession.h"
#include "NanoPBHelper.h"
#include "NXConsoleLog.h"
#include "NXPlayer.h"
#include "SpircHandler.h"
#include "TimeProvider.h"
#include "TrackPlayer.h"
#include "TrackQueue.h"
#include "WrappedSemaphore.h"
#include "ZeroconfHttpServer.h"
#include "app_settings.hpp"
#include "nlohmann/json.hpp"
#include "protobuf/login5.pb.h"

#ifdef __SWITCH__
#include "NXAudioSink.h"
#else
class NullAudioSink : public AudioSink {
 public:
  void feedPCMFrames(const uint8_t*, size_t) override {}
  bool setParams(uint32_t, uint8_t, uint8_t) override { return true; }
};
#endif

namespace {
constexpr const char* CONFIG_DIR = "/config/Switify";
constexpr const char* CREDENTIALS_FILE =
    "/config/Switify/credentials.json";
constexpr const char* CREDENTIALS_FILE_POC =
    "/config/switify-poc/credentials.json";
constexpr const char* NO_MIGRATION_FILE =
    "/config/Switify/do-not-import-poc-credentials";
constexpr const char* PLAYBACK_SESSION_FILE =
    "/config/Switify/playback-session.json";
constexpr int ZEROCONF_PORT = 7864;
constexpr const char* DEVICE_NAME = "Switify";
constexpr const char* WEB_CLIENT_ID =
    "65b708073fc0480ea92a077233ca87bd";
constexpr const char* ANDROID_CLIENT_ID =
    "9a8d2f0ce77a4e248bb71fefcb557637";
constexpr const char* IOS_CLIENT_ID =
    "58bd3c95768941ea9eb4350aaa033eb3";
constexpr const char* SPOTIFY_APP_VERSION = "1.2.52.442";
#ifdef SWITIFY_BACKGROUND_SERVICE
constexpr size_t MAX_CACHED_COLLECTIONS = 1;
constexpr size_t MAX_CACHED_COLLECTION_TRACKS = 300;
// A large collection remains fully available to the overlay browser, but the
// fixed-heap background process must not duplicate all of it into the player,
// queue, cspot state, and persisted session at once. Keep a useful amount of
// history before the selected track and fill the rest of the window with the
// upcoming tracks.
constexpr size_t MAX_BACKGROUND_PLAYBACK_TRACKS = 100;
constexpr size_t BACKGROUND_PLAYBACK_HISTORY = 20;
#else
constexpr size_t MAX_CACHED_COLLECTIONS = 10;
// The standalone NRO owns the foreground application resources. Retain full
// multi-thousand-track collections so revisiting a section is instantaneous;
// Borealis still creates only 100 track rows at a time.
constexpr size_t MAX_CACHED_COLLECTION_TRACKS = 30000;
#endif

bool sameTrack(const SpotifyEngine::Track& left,
               const SpotifyEngine::Track& right) {
  if (!left.uri.empty() && !right.uri.empty()) return left.uri == right.uri;
  if (!left.id.empty() && !right.id.empty()) return left.id == right.id;
  return left.name == right.name && left.artist == right.artist;
}

#ifdef SWITIFY_BACKGROUND_SERVICE
size_t boundBackgroundPlaybackWindow(
    std::vector<SpotifyEngine::Track>& tracks, size_t selectedIndex,
    size_t* windowStart = nullptr, size_t* originalCount = nullptr) {
  if (tracks.empty()) return 0;
  if (selectedIndex >= tracks.size()) selectedIndex = 0;
  if (originalCount) *originalCount = tracks.size();
  if (windowStart) *windowStart = 0;
  if (tracks.size() <= MAX_BACKGROUND_PLAYBACK_TRACKS) return selectedIndex;

  size_t first = selectedIndex > BACKGROUND_PLAYBACK_HISTORY
                     ? selectedIndex - BACKGROUND_PLAYBACK_HISTORY
                     : 0;
  if (first + MAX_BACKGROUND_PLAYBACK_TRACKS > tracks.size())
    first = tracks.size() - MAX_BACKGROUND_PLAYBACK_TRACKS;
  const size_t end = first + MAX_BACKGROUND_PLAYBACK_TRACKS;

  std::vector<SpotifyEngine::Track> window;
  window.reserve(MAX_BACKGROUND_PLAYBACK_TRACKS);
  window.insert(window.end(),
                std::make_move_iterator(
                    tracks.begin() + static_cast<std::ptrdiff_t>(first)),
                std::make_move_iterator(
                    tracks.begin() + static_cast<std::ptrdiff_t>(end)));
  tracks = std::move(window);
  if (windowStart) *windowStart = first;
  return selectedIndex - first;
}
#endif

cspot::SpircHandler::LocalRepeatMode localRepeatMode(
    SpotifyEngine::RepeatMode mode) {
  switch (mode) {
    case SpotifyEngine::RepeatMode::All:
      return cspot::SpircHandler::LocalRepeatMode::All;
    case SpotifyEngine::RepeatMode::One:
      return cspot::SpircHandler::LocalRepeatMode::One;
    case SpotifyEngine::RepeatMode::Off:
    default:
      return cspot::SpircHandler::LocalRepeatMode::Off;
  }
}

std::string collectionCacheKey(const SpotifyEngine::Collection& collection) {
  if (!collection.uri.empty()) return collection.uri;
  return std::to_string(static_cast<int>(collection.kind)) + ":" +
         collection.id + ":" + collection.name;
}

// Resolved from Spotify's current web-player bundles. The accompanying
// resolve-spotify-hashes.py utility refreshes this small table when Spotify
// republishes a persisted query.
const std::unordered_map<std::string, std::string> PATHFINDER_HASHES = {
    {"fetchEntitiesForRecentlyPlayed",
     "cf5d2e94ffd82788470788ae1f6090cc3e9e774fb8fd383580634c6e6f50f7be"},
    {"fetchLibraryTracks",
     "087278b20b743578a6262c2b0b4bcd20d879c503cc359a2285baf083ef944240"},
    {"fetchPlaylist",
     "86dde7b9d9356e2369414647cf6950cfed96e778e129cfdfc99aea6c1613b3b0"},
    {"getAlbum",
     "b9bfabef66ed756e5e13f68a942deb60bd4125ec1f1be8cc42769dc0259b4b10"},
    {"queryHomeSection",
     "aa46a47bd094b8abb08bb30fc26b66adba6417463be7345bdb05c9c69faabd5b"},
    {"libraryV3",
     "390c78e5b951029bad359785e69b07b536a509c581cbcd0aded5e5067f187455"},
    {"profileAttributes",
     "08ffb4730af3746e04a8301396f20875dbbce10c75243803091a9274eacc8ac0"},
    {"queryArtistOverview",
     "ae0e2958a4ab645b35ca19ac04d0495ae12d9c5d7b7286217674801a9aab281a"},
    {"queryPodcastEpisodes",
     "06046f9b939d56c8eb7cdbb687da938de1164c006871aec91dc26e4dc7d8eb08"},
    {"searchDesktop",
     "db61238974d27839a136c9dc02bfdbe3fab7635f21cf85976ebff9a1ee281345"},
    {"userTopContent",
     "49ee15704de4a7fdeac65a02db20604aa11e46f02e809c55d9a89f6db9754356"},
};

using Json = nlohmann::json;

std::string readFile(const char* path) {
  std::ifstream file(path);
  if (!file.good()) return "";
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

std::string urlEncode(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// Minimal protobuf wire helpers for Spotify's client-token request. Keeping
// this small avoids importing the very large current Spotify protocol tree;
// unknown response fields remain forward-compatible by construction.
void protoVarint(std::vector<uint8_t>& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>(value) | 0x80);
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

void protoBytes(std::vector<uint8_t>& out, uint32_t field,
                const uint8_t* data, size_t size) {
  protoVarint(out, (static_cast<uint64_t>(field) << 3) | 2);
  protoVarint(out, size);
  if (size > 0) out.insert(out.end(), data, data + size);
}

void protoBytes(std::vector<uint8_t>& out, uint32_t field,
                const std::vector<uint8_t>& data) {
  protoBytes(out, field, data.data(), data.size());
}

void protoString(std::vector<uint8_t>& out, uint32_t field,
                 const std::string& value) {
  protoBytes(out, field, reinterpret_cast<const uint8_t*>(value.data()),
             value.size());
}

bool readProtoVarint(const std::vector<uint8_t>& data, size_t& pos,
                     uint64_t& value) {
  value = 0;
  for (int shift = 0; shift < 64 && pos < data.size(); shift += 7) {
    const uint8_t byte = data[pos++];
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) return true;
  }
  return false;
}

bool findProtoVarint(const std::vector<uint8_t>& data, uint32_t wanted,
                     uint64_t& value) {
  size_t pos = 0;
  while (pos < data.size()) {
    uint64_t key = 0;
    if (!readProtoVarint(data, pos, key)) return false;
    const uint32_t field = static_cast<uint32_t>(key >> 3);
    const uint32_t wire = static_cast<uint32_t>(key & 7);
    if (wire == 0) {
      uint64_t current = 0;
      if (!readProtoVarint(data, pos, current)) return false;
      if (field == wanted) {
        value = current;
        return true;
      }
    } else if (wire == 1) {
      if (pos + 8 > data.size()) return false;
      pos += 8;
    } else if (wire == 2) {
      uint64_t length = 0;
      if (!readProtoVarint(data, pos, length) ||
          length > data.size() - pos)
        return false;
      pos += static_cast<size_t>(length);
    } else if (wire == 5) {
      if (pos + 4 > data.size()) return false;
      pos += 4;
    } else {
      return false;
    }
  }
  return false;
}

bool findProtoBytes(const std::vector<uint8_t>& data, uint32_t wanted,
                    std::vector<uint8_t>& value) {
  size_t pos = 0;
  while (pos < data.size()) {
    uint64_t key = 0;
    if (!readProtoVarint(data, pos, key)) return false;
    const uint32_t field = static_cast<uint32_t>(key >> 3);
    const uint32_t wire = static_cast<uint32_t>(key & 7);
    if (wire == 0) {
      uint64_t ignored = 0;
      if (!readProtoVarint(data, pos, ignored)) return false;
    } else if (wire == 1) {
      if (pos + 8 > data.size()) return false;
      pos += 8;
    } else if (wire == 2) {
      uint64_t length = 0;
      if (!readProtoVarint(data, pos, length) ||
          length > data.size() - pos)
        return false;
      if (field == wanted) {
        value.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                     data.begin() + static_cast<std::ptrdiff_t>(pos + length));
        return true;
      }
      pos += static_cast<size_t>(length);
    } else if (wire == 5) {
      if (pos + 4 > data.size()) return false;
      pos += 4;
    } else {
      return false;
    }
  }
  return false;
}

std::string joinArtists(const Json& artists) {
  if (!artists.is_array()) return "";
  std::string result;
  for (const auto& artist : artists) {
    const std::string name = artist.value("name", "");
    if (name.empty()) continue;
    if (!result.empty()) result += ", ";
    result += name;
  }
  return result;
}

constexpr int DefaultArtworkWidth = 300;
#ifdef SWITIFY_BACKGROUND_SERVICE
constexpr int PodcastArtworkWidth = DefaultArtworkWidth;
#else
// Podcast art is displayed in a large detail panel by the full NRO. The
// background service deliberately retains the smaller choice above so the
// overlay does not download or cache larger files.
constexpr int PodcastArtworkWidth = 640;
#endif

std::string pickImage(const Json& images,
                      int targetWidth = DefaultArtworkWidth) {
  if (!images.is_array() || images.empty()) return "";
  int bestDistance = std::numeric_limits<int>::max();
  std::string best;
  for (const auto& image : images) {
    const std::string url = image.value("url", "");
    if (url.empty()) continue;
    const int width = image.value("width", 300);
    const int distance = std::abs(width - targetWidth);
    if (best.empty() || distance < bestDistance) {
      best = url;
      bestDistance = distance;
    }
  }
  return best;
}

SpotifyEngine::Track parseTrack(const Json& item,
                                const std::string& defaultAlbum = "",
                                const std::string& defaultArtist = "",
                                const std::string& defaultImage = "") {
  SpotifyEngine::Track track;
  if (!item.is_object()) return track;
  track.id = item.value("id", "");
  track.name = item.value("name", "");
  track.uri = item.value("uri", "");
  track.durationMs = item.value("duration_ms", 0u);
  track.explicitContent = item.value("explicit", false);
  track.artist = item.contains("artists")
                     ? joinArtists(item["artists"])
                     : defaultArtist;
  if (track.artist.empty()) track.artist = defaultArtist;
  track.album = defaultAlbum;
  track.imageUrl = defaultImage;
  if (item.contains("album") && item["album"].is_object()) {
    track.album = item["album"].value("name", defaultAlbum);
    if (item["album"].contains("images"))
      track.imageUrl = pickImage(item["album"]["images"]);
  }
  return track;
}

SpotifyEngine::Track parseEpisode(const Json& item,
                                  const std::string& defaultShow = "",
                                  const std::string& defaultPublisher = "",
                                  const std::string& defaultImage = "") {
  SpotifyEngine::Track episode;
  if (!item.is_object()) return episode;
  episode.id = item.value("id", "");
  episode.name = item.value("name", "");
  episode.uri = item.value("uri", "");
  episode.durationMs = item.value("duration_ms", 0u);
  episode.explicitContent = item.value("explicit", false);
  episode.album = defaultShow;
  episode.artist = defaultPublisher;
  episode.imageUrl = defaultImage;
  episode.podcast = true;
  if (item.contains("images"))
    episode.imageUrl = pickImage(item["images"], PodcastArtworkWidth);
  if (item.contains("show") && item["show"].is_object()) {
    const auto& show = item["show"];
    episode.album = show.value("name", episode.album);
    episode.artist = show.value("publisher", episode.artist);
    if (episode.imageUrl.empty() && show.contains("images"))
      episode.imageUrl = pickImage(show["images"], PodcastArtworkWidth);
  }
  if (episode.artist.empty()) episode.artist = episode.album;
  return episode;
}

SpotifyEngine::Collection parsePlaylist(const Json& item) {
  SpotifyEngine::Collection collection;
  collection.kind = SpotifyEngine::Collection::Kind::PLAYLIST;
  if (!item.is_object()) return collection;
  collection.id = item.value("id", "");
  collection.uri = item.value("uri", "");
  collection.name = item.value("name", "");
  collection.description = item.value("description", "");
  if (item.contains("owner") && item["owner"].is_object())
    collection.subtitle = item["owner"].value("display_name", "");
  if (item.contains("images"))
    collection.imageUrl = pickImage(item["images"]);
  const Json* countObject = nullptr;
  if (item.contains("items") && item["items"].is_object())
    countObject = &item["items"];
  else if (item.contains("tracks") && item["tracks"].is_object())
    countObject = &item["tracks"];
  if (countObject) collection.itemCount = countObject->value("total", 0);
  if (collection.itemCount > 0) {
    if (!collection.subtitle.empty()) collection.subtitle += "  |  ";
    collection.subtitle += std::to_string(collection.itemCount) + " songs";
  }
  return collection;
}

SpotifyEngine::Collection parseAlbum(const Json& item) {
  SpotifyEngine::Collection collection;
  collection.kind = SpotifyEngine::Collection::Kind::ALBUM;
  if (!item.is_object()) return collection;
  collection.id = item.value("id", "");
  collection.uri = item.value("uri", "");
  collection.name = item.value("name", "");
  collection.subtitle = item.contains("artists")
                            ? joinArtists(item["artists"])
                            : std::string();
  collection.itemCount = item.value("total_tracks", 0);
  if (item.contains("images"))
    collection.imageUrl = pickImage(item["images"]);
  return collection;
}

SpotifyEngine::Collection parseArtist(const Json& item) {
  SpotifyEngine::Collection collection;
  collection.kind = SpotifyEngine::Collection::Kind::ARTIST;
  if (!item.is_object()) return collection;
  collection.id = item.value("id", "");
  collection.uri = item.value("uri", "");
  collection.name = item.value("name", "");
  collection.subtitle = "Artist";
  if (item.contains("images"))
    collection.imageUrl = pickImage(item["images"]);
  return collection;
}

SpotifyEngine::Collection parseShow(const Json& item) {
  SpotifyEngine::Collection collection;
  collection.kind = SpotifyEngine::Collection::Kind::SHOW;
  if (!item.is_object()) return collection;
  collection.id = item.value("id", "");
  collection.uri = item.value("uri", "");
  collection.name = item.value("name", "");
  collection.description = item.value("description", "");
  collection.subtitle = item.value("publisher", "Podcast");
  collection.itemCount = item.value("total_episodes", 0);
  if (item.contains("images"))
    collection.imageUrl = pickImage(item["images"], PodcastArtworkWidth);
  return collection;
}

std::string jsonString(const Json& object, const char* key) {
  if (!object.is_object()) return "";
  const auto found = object.find(key);
  return found != object.end() && found->is_string()
             ? found->get<std::string>()
             : std::string();
}

uint32_t jsonUint(const Json& object, const char* key) {
  if (!object.is_object()) return 0;
  const auto found = object.find(key);
  if (found == object.end() || !found->is_number()) return 0;
  const auto value = found->get<long long>();
  return static_cast<uint32_t>(
      std::clamp<long long>(value, 0, std::numeric_limits<uint32_t>::max()));
}

const Json* jsonPath(const Json& root,
                     std::initializer_list<const char*> path) {
  const Json* current = &root;
  for (const char* key : path) {
    if (!current->is_object()) return nullptr;
    const auto found = current->find(key);
    if (found == current->end()) return nullptr;
    current = &*found;
  }
  return current;
}

bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

std::string idFromUri(const std::string& uri) {
  const auto separator = uri.rfind(':');
  return separator == std::string::npos ? uri : uri.substr(separator + 1);
}

struct PartnerNode {
  const Json* data = nullptr;
  std::string uri;
};

bool findPartnerNode(const Json& value, const std::string& uriPrefix,
                     PartnerNode& result, int depth = 0) {
  if (depth > 16) return false;
  if (value.is_object()) {
    std::string uri = jsonString(value, "uri");
    if (uri.empty()) uri = jsonString(value, "_uri");
    if (startsWith(uri, uriPrefix)) {
      const auto data = value.find("data");
      result.data = data != value.end() && data->is_object() ? &*data : &value;
      result.uri = std::move(uri);
      return true;
    }
    // Prefer the normal Pathfinder wrapper keys before walking metadata.
    for (const char* key : {"itemV2", "item", "track", "data"}) {
      const auto child = value.find(key);
      if (child != value.end() &&
          findPartnerNode(*child, uriPrefix, result, depth + 1))
        return true;
    }
    for (auto child = value.begin(); child != value.end(); ++child) {
      if (child.key() == "itemV2" || child.key() == "item" ||
          child.key() == "track" || child.key() == "data")
        continue;
      if (findPartnerNode(child.value(), uriPrefix, result, depth + 1))
        return true;
    }
  } else if (value.is_array()) {
    for (const auto& child : value) {
      if (findPartnerNode(child, uriPrefix, result, depth + 1)) return true;
    }
  }
  return false;
}

std::string nestedName(const Json& value, int depth = 0) {
  if (depth > 6 || !value.is_object()) return "";
  std::string name = jsonString(value, "name");
  if (name.empty()) name = jsonString(value, "displayName");
  if (name.empty()) name = jsonString(value, "title");
  if (!name.empty()) return name;
  for (const char* key : {"profile", "data", "owner", "ownerV2", "user"}) {
    const auto child = value.find(key);
    if (child != value.end()) {
      name = nestedName(*child, depth + 1);
      if (!name.empty()) return name;
    }
  }
  return "";
}

void appendPartnerArtist(const Json& value, std::vector<std::string>& names,
                         int depth = 0) {
  if (depth > 5 || !value.is_object()) return;
  std::string name;
  const auto profile = value.find("profile");
  if (profile != value.end()) name = nestedName(*profile);
  if (name.empty()) name = jsonString(value, "name");
  if (!name.empty() &&
      std::find(names.begin(), names.end(), name) == names.end()) {
    names.push_back(std::move(name));
    return;
  }
  for (const char* key : {"node", "artist", "data"}) {
    const auto child = value.find(key);
    if (child != value.end()) appendPartnerArtist(*child, names, depth + 1);
  }
}

std::string partnerArtists(const Json& item) {
  std::vector<std::string> names;
  for (const char* groupKey : {"artists", "firstArtist", "otherArtists"}) {
    const auto group = item.find(groupKey);
    if (group == item.end()) continue;
    if (group->is_array()) {
      for (const auto& entry : *group) appendPartnerArtist(entry, names);
    } else if (group->is_object()) {
      bool usedList = false;
      for (const char* listKey : {"items", "nodes", "edges"}) {
        const auto list = group->find(listKey);
        if (list != group->end() && list->is_array()) {
          usedList = true;
          for (const auto& entry : *list) appendPartnerArtist(entry, names);
        }
      }
      if (!usedList) appendPartnerArtist(*group, names);
    }
  }
  std::string joined;
  for (const auto& name : names) {
    if (!joined.empty()) joined += ", ";
    joined += name;
  }
  return joined;
}

void findPartnerImage(const Json& value, std::string& best,
                      int& bestDistance,
                      int targetWidth = DefaultArtworkWidth, int depth = 0) {
  if (depth > 10) return;
  if (value.is_object()) {
    const std::string url = jsonString(value, "url");
    if (!url.empty()) {
      const int width = static_cast<int>(jsonUint(value, "width"));
      const int distance =
          width > 0 ? std::abs(width - targetWidth) : 1000;
      if (best.empty() || distance < bestDistance) {
        best = url;
        bestDistance = distance;
      }
    }
    for (auto child = value.begin(); child != value.end(); ++child)
      findPartnerImage(child.value(), best, bestDistance, targetWidth,
                       depth + 1);
  } else if (value.is_array()) {
    for (const auto& child : value)
      findPartnerImage(child, best, bestDistance, targetWidth, depth + 1);
  }
}

std::string partnerImage(const Json& item,
                         int targetWidth = DefaultArtworkWidth) {
  std::string best;
  int bestDistance = std::numeric_limits<int>::max();
  // Album art is the most useful track image. Search it first and retain it
  // when available instead of accidentally selecting an artist avatar.
  for (const char* key : {"albumOfTrack", "album"}) {
    const auto album = item.find(key);
    if (album != item.end())
      findPartnerImage(*album, best, bestDistance, targetWidth);
  }
  if (best.empty())
    findPartnerImage(item, best, bestDistance, targetWidth);
  return best;
}

SpotifyEngine::Track parsePartnerTrack(const Json& value) {
  SpotifyEngine::Track track;
  PartnerNode node;
  if (!findPartnerNode(value, "spotify:track:", node) || !node.data)
    return track;
  const Json& item = *node.data;
  track.uri = node.uri;
  track.id = idFromUri(track.uri);
  track.name = jsonString(item, "name");
  if (track.name.empty()) track.name = jsonString(item, "title");
  track.artist = partnerArtists(item);
  for (const char* key : {"albumOfTrack", "album"}) {
    const auto album = item.find(key);
    if (album != item.end() && album->is_object()) {
      track.album = nestedName(*album);
      if (!track.album.empty()) break;
    }
  }
  track.durationMs = jsonUint(item, "duration_ms");
  if (track.durationMs == 0) track.durationMs = jsonUint(item, "durationMs");
  for (const char* key : {"duration", "trackDuration"}) {
    if (track.durationMs != 0) break;
    const auto duration = item.find(key);
    if (duration != item.end())
      track.durationMs = jsonUint(*duration, "totalMilliseconds");
  }
  const auto explicitValue = item.find("explicit");
  if (explicitValue != item.end() && explicitValue->is_boolean())
    track.explicitContent = explicitValue->get<bool>();
  const auto rating = item.find("contentRating");
  if (rating != item.end()) {
    std::string label = jsonString(*rating, "label");
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    if (label == "EXPLICIT") track.explicitContent = true;
  }
  track.imageUrl = partnerImage(item);
  return track;
}

SpotifyEngine::Track parsePartnerEpisode(const Json& value) {
  SpotifyEngine::Track episode;
  PartnerNode node;
  if (!findPartnerNode(value, "spotify:episode:", node) || !node.data)
    return episode;
  const Json& item = *node.data;
  episode.uri = node.uri;
  episode.id = idFromUri(episode.uri);
  episode.name = jsonString(item, "name");
  if (episode.name.empty()) episode.name = jsonString(item, "title");
  episode.durationMs = jsonUint(item, "duration_ms");
  if (episode.durationMs == 0) episode.durationMs = jsonUint(item, "durationMs");
  for (const char* key : {"duration", "episodeDuration"}) {
    if (episode.durationMs != 0) break;
    const auto duration = item.find(key);
    if (duration != item.end())
      episode.durationMs = jsonUint(*duration, "totalMilliseconds");
  }
  for (const char* key : {"podcast", "show"}) {
    const auto show = item.find(key);
    if (show != item.end() && show->is_object()) {
      episode.album = nestedName(*show);
      episode.artist = jsonString(*show, "publisher");
      if (episode.artist.empty()) episode.artist = episode.album;
      if (!episode.album.empty()) break;
    }
  }
  episode.imageUrl = partnerImage(item, PodcastArtworkWidth);
  episode.podcast = true;
  return episode;
}

SpotifyEngine::Collection parsePartnerCollection(
    const Json& value, SpotifyEngine::Collection::Kind kind) {
  SpotifyEngine::Collection collection;
  collection.kind = kind;
  std::string prefix;
  switch (kind) {
    case SpotifyEngine::Collection::Kind::PLAYLIST:
      prefix = "spotify:playlist:";
      break;
    case SpotifyEngine::Collection::Kind::ALBUM:
      prefix = "spotify:album:";
      break;
    case SpotifyEngine::Collection::Kind::ARTIST:
      prefix = "spotify:artist:";
      break;
    case SpotifyEngine::Collection::Kind::SHOW:
      prefix = "spotify:show:";
      break;
    default:
      return collection;
  }
  PartnerNode node;
  if (!findPartnerNode(value, prefix, node) || !node.data) return collection;
  const Json& item = *node.data;
  collection.uri = node.uri;
  collection.id = idFromUri(collection.uri);
  collection.name = nestedName(item);
  collection.description = jsonString(item, "description");
  collection.imageUrl = partnerImage(
      item, kind == SpotifyEngine::Collection::Kind::SHOW
                ? PodcastArtworkWidth
                : DefaultArtworkWidth);
  collection.itemCount = static_cast<int>(jsonUint(item, "totalTracks"));
  if (collection.itemCount == 0) {
    const auto content = item.find("content");
    if (content != item.end())
      collection.itemCount = static_cast<int>(jsonUint(*content, "totalCount"));
  }
  if (kind == SpotifyEngine::Collection::Kind::PLAYLIST) {
    for (const char* key : {"ownerV2", "owner"}) {
      const auto owner = item.find(key);
      if (owner != item.end()) {
        collection.subtitle = nestedName(*owner);
        if (!collection.subtitle.empty()) break;
      }
    }
    if (collection.itemCount > 0) {
      if (!collection.subtitle.empty()) collection.subtitle += "  |  ";
      collection.subtitle += std::to_string(collection.itemCount) + " songs";
    }
  } else if (kind == SpotifyEngine::Collection::Kind::ALBUM) {
    collection.subtitle = partnerArtists(item);
  } else if (kind == SpotifyEngine::Collection::Kind::ARTIST) {
    collection.subtitle = "Artist";
  } else {
    collection.subtitle = jsonString(item, "publisher");
    if (collection.subtitle.empty()) collection.subtitle = "Podcast";
  }
  return collection;
}

template <typename Parsed, typename Parser>
void appendPartnerItems(const Json* container, std::vector<Parsed>& output,
                        Parser parser) {
  if (!container || !container->is_object()) return;
  const auto items = container->find("items");
  if (items == container->end() || !items->is_array()) return;
  std::unordered_set<std::string> seen;
  for (const auto& item : *items) {
    auto parsed = parser(item);
    if (!parsed.uri.empty() && seen.insert(parsed.uri).second)
      output.push_back(std::move(parsed));
  }
}

size_t appendPartnerTrackPage(
    const Json* container, std::vector<SpotifyEngine::Track>& output,
    std::unordered_set<std::string>& seen) {
  if (!container || !container->is_object()) return 0;
  const auto items = container->find("items");
  if (items == container->end() || !items->is_array()) return 0;
  for (const auto& item : *items) {
    auto parsed = parsePartnerTrack(item);
    if (!parsed.uri.empty() && seen.insert(parsed.uri).second)
      output.push_back(std::move(parsed));
  }
  return items->size();
}

size_t appendPartnerEpisodePage(
    const Json* container, std::vector<SpotifyEngine::Track>& output,
    std::unordered_set<std::string>& seen, const std::string& showName,
    const std::string& publisher, const std::string& showImage) {
  if (!container || !container->is_object()) return 0;
  const auto items = container->find("items");
  if (items == container->end() || !items->is_array()) return 0;
  for (const auto& item : *items) {
    auto episode = parsePartnerEpisode(item);
    if (episode.album.empty()) episode.album = showName;
    if (episode.artist.empty())
      episode.artist = publisher.empty() ? showName : publisher;
    if (episode.imageUrl.empty()) episode.imageUrl = showImage;
    if (!episode.uri.empty() && seen.insert(episode.uri).second)
      output.push_back(std::move(episode));
  }
  return items->size();
}

void collectPartnerTracks(const Json& value,
                          std::vector<SpotifyEngine::Track>& output,
                          std::unordered_set<std::string>& seen,
                          int depth = 0) {
  if (depth > 16) return;
  if (value.is_object()) {
    std::string uri = jsonString(value, "uri");
    if (uri.empty()) uri = jsonString(value, "_uri");
    if (startsWith(uri, "spotify:track:")) {
      auto track = parsePartnerTrack(value);
      if (!track.uri.empty() && seen.insert(track.uri).second)
        output.push_back(std::move(track));
      return;
    }
    for (auto child = value.begin(); child != value.end(); ++child)
      collectPartnerTracks(child.value(), output, seen, depth + 1);
  } else if (value.is_array()) {
    for (const auto& child : value)
      collectPartnerTracks(child, output, seen, depth + 1);
  }
}

std::vector<SpotifyEngine::HomeShelf> parseHomeShelves(const Json& payload) {
  std::vector<SpotifyEngine::HomeShelf> shelves;
  const Json* sections = jsonPath(payload, {"data", "homeSections", "sections"});
  if (!sections || !sections->is_array()) return shelves;
  for (const auto& sectionValue : *sections) {
    SpotifyEngine::HomeShelf shelf;
    const Json* title = jsonPath(sectionValue, {"data", "title"});
    if (title) shelf.title = jsonString(*title, "text");
    const Json* items = jsonPath(sectionValue, {"sectionItems", "items"});
    if (!items || !items->is_array()) continue;
    std::unordered_set<std::string> seen;
    for (const auto& wrapper : *items) {
      const Json* content = jsonPath(wrapper, {"content", "data"});
      if (!content) content = &wrapper;
      std::string uri = jsonString(*content, "uri");
      SpotifyEngine::Collection collection;
      if (startsWith(uri, "spotify:playlist:")) {
        collection = parsePartnerCollection(
            *content, SpotifyEngine::Collection::Kind::PLAYLIST);
      } else if (startsWith(uri, "spotify:album:")) {
        collection = parsePartnerCollection(
            *content, SpotifyEngine::Collection::Kind::ALBUM);
      } else if (startsWith(uri, "spotify:artist:")) {
        collection = parsePartnerCollection(
            *content, SpotifyEngine::Collection::Kind::ARTIST);
      } else if (startsWith(uri, "spotify:show:")) {
        collection = parsePartnerCollection(
            *content, SpotifyEngine::Collection::Kind::SHOW);
      }
      if (!collection.uri.empty() && seen.insert(collection.uri).second)
        shelf.items.push_back(std::move(collection));
    }
    if (shelf.title.empty()) shelf.title = "Recommended for you";
    if (!shelf.items.empty()) shelves.push_back(std::move(shelf));
  }
  return shelves;
}

void appendWarning(std::string& warnings, const std::string& text) {
  if (!warnings.empty()) warnings += "  |  ";
  warnings += text;
}

Json libraryRequestVariables(const char* filter, size_t limit) {
  return Json{{"filters", Json::array({filter})},
              {"order", nullptr},
              {"textFilter", ""},
              {"features", Json::array({"LIKED_SONGS", "YOUR_EPISODES"})},
              {"limit", limit},
              {"offset", 0},
              {"flatten", false},
              {"expandedFolders", Json::array()},
              {"folderUri", nullptr},
              {"includeFoldersWhenFlattening", true},
              {"withCuration", false}};
}

void appendSavedPodcasts(const Json& json,
                         std::vector<SpotifyEngine::Collection>& output) {
  appendPartnerItems(
      jsonPath(json, {"data", "me", "libraryV3"}), output,
      [](const Json& item) {
        return parsePartnerCollection(
            item, SpotifyEngine::Collection::Kind::SHOW);
      });
}

using WebHeaders = std::vector<std::pair<std::string, std::string>>;

struct WebResponse {
  int statusCode = 0;
  std::vector<uint8_t> body;
  std::unordered_map<std::string, std::string> headers;

  std::string header(const std::string& name) const {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const auto found = headers.find(lower);
    return found == headers.end() ? std::string() : found->second;
  }
};

#ifdef __SWITCH__
size_t curlBodyCallback(char* data, size_t size, size_t count, void* userData) {
  const size_t bytes = size * count;
  auto& body = *static_cast<std::vector<uint8_t>*>(userData);
  const auto* begin = reinterpret_cast<const uint8_t*>(data);
  body.insert(body.end(), begin, begin + bytes);
  return bytes;
}

size_t curlHeaderCallback(char* data, size_t size, size_t count,
                          void* userData) {
  const size_t bytes = size * count;
  auto& headers =
      *static_cast<std::unordered_map<std::string, std::string>*>(userData);
  std::string line(data, bytes);
  const auto colon = line.find(':');
  if (colon == std::string::npos) return bytes;

  std::string name = line.substr(0, colon);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  size_t valueStart = colon + 1;
  while (valueStart < line.size() &&
         std::isspace(static_cast<unsigned char>(line[valueStart])))
    ++valueStart;
  size_t valueEnd = line.size();
  while (valueEnd > valueStart &&
         std::isspace(static_cast<unsigned char>(line[valueEnd - 1])))
    --valueEnd;
  headers[name] = line.substr(valueStart, valueEnd - valueStart);
  return bytes;
}
#endif

WebResponse webRequest(const std::string& url, const std::string& method,
                       const std::vector<uint8_t>& requestBody,
                       const WebHeaders& requestHeaders) {
#ifdef __SWITCH__
  static std::once_flag curlInit;
  static CURLcode curlInitResult = CURLE_FAILED_INIT;
  std::call_once(curlInit, [] { curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (curlInitResult != CURLE_OK)
    throw std::runtime_error("libcurl global initialization failed");

  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("libcurl request initialization failed");

  curl_slist* headerList = nullptr;
  bool hasContentLength = false;
  for (const auto& [name, value] : requestHeaders) {
    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lowerName == "content-length") hasContentLength = true;
    const std::string line = name + ": " + value;
    curl_slist* appended = curl_slist_append(headerList, line.c_str());
    if (!appended) {
      curl_slist_free_all(headerList);
      curl_easy_cleanup(curl);
      throw std::runtime_error("libcurl header allocation failed");
    }
    headerList = appended;
  }
  if (requestBody.empty() && method != "GET" && method != "HEAD" &&
      !hasContentLength) {
    curl_slist* appended = curl_slist_append(headerList, "Content-Length: 0");
    if (!appended) {
      curl_slist_free_all(headerList);
      curl_easy_cleanup(curl);
      throw std::runtime_error("libcurl header allocation failed");
    }
    headerList = appended;
  }

  WebResponse response;
  char errorBuffer[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlBodyCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlHeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
  curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
  curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
  // Bell's current Switch TLS path also operates without a CA bundle. Keep
  // transport behavior equivalent until Switify ships its own root bundle.
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  if (!requestBody.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
                     reinterpret_cast<const char*>(requestBody.data()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(requestBody.size()));
  } else if (method != "GET" && method != "HEAD") {
    // CURLOPT_CUSTOMREQUEST alone does not emit Content-Length for an empty
    // body. Spotify requires explicit framing for zero-length POST, PUT, and
    // DELETE requests; omitting it returns HTTP 411 Length Required.
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
  }

  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  response.statusCode = static_cast<int>(status);
  curl_slist_free_all(headerList);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) {
    const char* detail = errorBuffer[0] ? errorBuffer : curl_easy_strerror(result);
    throw std::runtime_error(std::string("HTTP transport failed: ") + detail);
  }
  return response;
#else
  bell::HTTPClient::Headers headers = requestHeaders;
  auto raw = std::make_unique<bell::HTTPClient::Response>();
  raw->connect(url);
  raw->rawRequest(url, method, requestBody, headers);
  WebResponse response;
  response.statusCode = raw->statusCode();
  response.body = raw->bytes();
  for (const char* name : {"content-length", "transfer-encoding", "connection",
                           "retry-after"}) {
    const std::string value(raw->header(name));
    if (!value.empty()) response.headers[name] = value;
  }
  return response;
#endif
}

std::string responseText(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}
}  // namespace

SpotifyEngine& SpotifyEngine::instance() {
  static SpotifyEngine engine;
  return engine;
}

SpotifyEngine::ObserverId SpotifyEngine::subscribe(std::function<void()> cb) {
  const ObserverId id = nextObserverId++;
  std::lock_guard<std::mutex> lock(observerMutex);
  observers.emplace(id, std::move(cb));
  return id;
}

void SpotifyEngine::unsubscribe(ObserverId id) {
  std::lock_guard<std::mutex> lock(observerMutex);
  observers.erase(id);
}

void SpotifyEngine::notifyUpdate() {
  std::vector<std::function<void()>> callbacks;
  {
    std::lock_guard<std::mutex> lock(observerMutex);
    callbacks.reserve(observers.size());
    for (const auto& [id, callback] : observers) callbacks.push_back(callback);
  }
  for (const auto& callback : callbacks) {
    if (callback) callback();
  }
}

void SpotifyEngine::setState(State newState, const std::string& text) {
  currentState = newState;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    status = text;
  }
  notifyUpdate();
}

SpotifyEngine::State SpotifyEngine::state() const { return currentState; }

std::string SpotifyEngine::statusText() {
  std::lock_guard<std::mutex> lock(dataMutex);
  return status;
}

SpotifyEngine::Profile SpotifyEngine::profile() {
  std::lock_guard<std::mutex> lock(dataMutex);
  return profileData;
}

uint32_t SpotifyEngine::computedPositionLocked() const {
  uint64_t position = positionBaseMs;
  if (!pausedState && nowPlayingData.hasTrack &&
      positionMeasuredAt.time_since_epoch().count() != 0) {
    position += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - positionMeasuredAt)
            .count());
  }
  if (nowPlayingData.durationMs > 0)
    position = std::min<uint64_t>(position, nowPlayingData.durationMs);
  return static_cast<uint32_t>(position);
}

void SpotifyEngine::savePlaybackSession() {
  Json session;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    if (activePlaylist.empty() || !activePlaylistHasCurrent ||
        activePlaylistIndex >= activePlaylist.size())
      return;

    size_t savedIndex = activePlaylistIndex;
    uint32_t savedPosition = computedPositionLocked();
    const uint32_t duration = activePlaylist[savedIndex].durationMs;
    if (duration > 0 && savedPosition + 2000 >= duration &&
        savedIndex + 1 < activePlaylist.size()) {
      ++savedIndex;
      savedPosition = 0;
    }

    session["version"] = 1;
    session["source"] = activeSourceName;
    session["index"] = savedIndex;
    session["position_ms"] = savedPosition;
    session["tracks"] = Json::array();
    for (const auto& track : activePlaylist) {
      if (track.uri.empty()) continue;
      session["tracks"].push_back(
          {{"id", track.id},
           {"uri", track.uri},
           {"name", track.name},
           {"artist", track.artist},
           {"album", track.album},
           {"image", track.imageUrl},
           {"duration_ms", track.durationMs},
           {"explicit", track.explicitContent},
           {"podcast", track.podcast}});
    }
  }

  if (session["tracks"].empty()) return;
  std::lock_guard<std::mutex> persistenceLock(playbackPersistenceMutex);
  const std::string temporary = std::string(PLAYBACK_SESSION_FILE) + ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    if (!file.good()) {
      nxlog::push("Could not write local playback session");
      return;
    }
    file << session.dump();
    if (!file.good()) {
      nxlog::push("Local playback session write was incomplete");
      return;
    }
  }
  std::remove(PLAYBACK_SESSION_FILE);
  if (std::rename(temporary.c_str(), PLAYBACK_SESSION_FILE) != 0)
    nxlog::push("Could not commit local playback session");
}

bool SpotifyEngine::restorePlaybackSession() {
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    if (!activePlaylist.empty()) return false;
  }

  std::string serialized;
  {
    std::lock_guard<std::mutex> persistenceLock(playbackPersistenceMutex);
    serialized = readFile(PLAYBACK_SESSION_FILE);
  }
  if (serialized.empty()) return false;

  try {
    const Json session = Json::parse(serialized);
    if (session.value("version", 0) != 1 ||
        !session.contains("tracks") || !session["tracks"].is_array() ||
        session["tracks"].empty() || session["tracks"].size() > 1000)
      throw std::runtime_error("invalid session structure");

    std::vector<Track> tracks;
    tracks.reserve(session["tracks"].size());
    for (const auto& item : session["tracks"]) {
      Track track;
      track.id = item.value("id", "");
      track.uri = item.value("uri", "");
      track.name = item.value("name", "Unknown track");
      track.artist = item.value("artist", "");
      track.album = item.value("album", "");
      track.imageUrl = item.value("image", "");
      track.durationMs = item.value("duration_ms", 0U);
      track.explicitContent = item.value("explicit", false);
      track.podcast = item.value("podcast", false);
      if (!track.uri.empty()) tracks.push_back(std::move(track));
    }
    if (tracks.empty()) throw std::runtime_error("session has no tracks");

    size_t index = session.value("index", static_cast<size_t>(0));
    if (index >= tracks.size()) index = 0;
#ifdef SWITIFY_BACKGROUND_SERVICE
    size_t restoredWindowStart = 0;
    size_t restoredTrackCount = 0;
    index = boundBackgroundPlaybackWindow(tracks, index, &restoredWindowStart,
                                          &restoredTrackCount);
    if (restoredTrackCount > tracks.size()) {
      nxlog::pushf("Restored playback window: tracks %u-%u of %u",
                   static_cast<unsigned>(restoredWindowStart + 1),
                   static_cast<unsigned>(restoredWindowStart + tracks.size()),
                   static_cast<unsigned>(restoredTrackCount));
    }
#endif
    uint32_t positionMs = session.value("position_ms", 0U);
    if (tracks[index].durationMs > 0)
      positionMs = std::min(positionMs, tracks[index].durationMs);
    const std::string source = session.value("source", "Playback session");

    const Track selected = tracks[index];
    const size_t trackCount = tracks.size();
    {
      std::lock_guard<std::mutex> lock(dataMutex);
      activeSourceName = source;
      activePlaylist = tracks;
      activePlaylistIndex = index;
      activePlaylistHasCurrent = true;
      ++activePlaylistRevision;
      activeQueue = std::move(tracks);
      activeQueueIndex = index;
      activeQueueHasCurrent = true;
      nowPlayingData.id = selected.id;
      nowPlayingData.uri = selected.uri;
      nowPlayingData.name = selected.name;
      nowPlayingData.artist = selected.artist;
      nowPlayingData.album = selected.album;
      nowPlayingData.imageUrl = selected.imageUrl;
      nowPlayingData.durationMs = selected.durationMs;
      nowPlayingData.hasTrack = true;
      nowPlayingData.saved = false;
      nowPlayingData.savedKnown = false;
      nowPlayingData.savePending = false;
      positionBaseMs = positionMs;
      positionMeasuredAt = std::chrono::steady_clock::now();
      restoredSessionPendingStart = true;
    }
    pausedState = true;
    notifyUpdate();
    refreshTrackSavedState(selected.uri);
    nxlog::pushf("Restored paused session \"%s\" at track %u/%u, %u ms",
                 source.c_str(), static_cast<unsigned>(index + 1),
                 static_cast<unsigned>(trackCount), positionMs);
    return true;
  } catch (const std::exception& error) {
    nxlog::pushf("Ignoring invalid local playback session: %s", error.what());
    std::lock_guard<std::mutex> persistenceLock(playbackPersistenceMutex);
    std::remove(PLAYBACK_SESSION_FILE);
    return false;
  }
}

SpotifyEngine::NowPlaying SpotifyEngine::nowPlaying() {
  std::lock_guard<std::mutex> lock(dataMutex);
  NowPlaying snapshot = nowPlayingData;
  snapshot.paused = pausedState;
  snapshot.positionMs = computedPositionLocked();
  return snapshot;
}

SpotifyEngine::PlaybackSession SpotifyEngine::playbackSession() {
  std::lock_guard<std::mutex> lock(dataMutex);
  PlaybackSession session;
  session.name = activeSourceName;
  session.tracks = activePlaylist;
  session.currentIndex = activePlaylistIndex;
  session.hasCurrent = activePlaylistHasCurrent;
  return session;
}

SpotifyEngine::PlaybackSessionInfo SpotifyEngine::playbackSessionInfo() {
  std::lock_guard<std::mutex> lock(dataMutex);
  PlaybackSessionInfo info;
  info.name = activeSourceName;
  info.trackCount = activePlaylist.size();
  info.currentIndex = activePlaylistIndex;
  info.hasCurrent = activePlaylistHasCurrent;
  if (!activePlaylist.empty()) {
    const size_t typeIndex =
        activePlaylistHasCurrent && activePlaylistIndex < activePlaylist.size()
            ? activePlaylistIndex
            : 0;
    info.podcast = activePlaylist[typeIndex].podcast;
  }
  info.revision = activePlaylistRevision;
  return info;
}

std::vector<SpotifyEngine::Track> SpotifyEngine::playbackSessionPage(
    size_t offset, size_t count) {
  std::lock_guard<std::mutex> lock(dataMutex);
  if (offset >= activePlaylist.size() || count == 0) return {};
  const size_t end = std::min(activePlaylist.size(), offset + count);
  return {activePlaylist.begin() + static_cast<std::ptrdiff_t>(offset),
          activePlaylist.begin() + static_cast<std::ptrdiff_t>(end)};
}

bool SpotifyEngine::cachedDashboard(HomeData& dashboard) {
  std::lock_guard<std::mutex> lock(dataMutex);
  if (!hasDashboardCache) return false;
  dashboard = dashboardCache;
  return true;
}

bool SpotifyEngine::cachedLibrary(LibraryData& library) {
  std::lock_guard<std::mutex> lock(dataMutex);
  if (!hasLibraryCache) return false;
  library = libraryCache;
  return true;
}

bool SpotifyEngine::cachedPlaylistTracks(const Collection& collection,
                                         std::vector<Track>& tracks) {
  const std::string key = collectionCacheKey(collection);
  std::lock_guard<std::mutex> lock(dataMutex);
  const auto cached = collectionTracksCache.find(key);
  if (cached == collectionTracksCache.end()) return false;
  tracks = cached->second;
  const auto order = std::find(collectionTrackCacheOrder.begin(),
                               collectionTrackCacheOrder.end(), key);
  if (order != collectionTrackCacheOrder.end())
    collectionTrackCacheOrder.erase(order);
  collectionTrackCacheOrder.push_back(key);
  return true;
}

std::vector<SpotifyEngine::Track> SpotifyEngine::currentQueue() {
  std::lock_guard<std::mutex> lock(dataMutex);
  const size_t first = activeQueueHasCurrent ? activeQueueIndex + 1 : 0;
  if (first >= activeQueue.size()) return {};
  return {activeQueue.begin() + static_cast<std::ptrdiff_t>(first),
          activeQueue.end()};
}

void SpotifyEngine::playPause() {
  if (currentState != State::Ready || !handler) return;
  std::vector<Track> restoredTracks;
  std::string restoredSource;
  size_t restoredIndex = 0;
  uint32_t restoredPosition = 0;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    if (pausedState && restoredSessionPendingStart &&
        activePlaylistHasCurrent &&
        activePlaylistIndex < activePlaylist.size()) {
      restoredTracks = activePlaylist;
      restoredSource = activeSourceName;
      restoredIndex = activePlaylistIndex;
      restoredPosition = computedPositionLocked();
    }
    positionBaseMs = computedPositionLocked();
    positionMeasuredAt = std::chrono::steady_clock::now();
  }
  if (!restoredTracks.empty()) {
    playTracks(std::move(restoredTracks), restoredIndex, restoredSource,
               restoredPosition);
    return;
  }
  const bool pause = !pausedState;
  pausedState = pause;
  handler->setPause(pause);
  notifyUpdate();
  savePlaybackSession();
}

void SpotifyEngine::next() {
  if (currentState != State::Ready || !handler) return;
  std::vector<Track> restoredTracks;
  std::string restoredSource;
  size_t restoredIndex = 0;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    if (restoredSessionPendingStart && !activePlaylist.empty()) {
      restoredTracks = activePlaylist;
      restoredSource = activeSourceName;
      restoredIndex = std::min(activePlaylistIndex + 1,
                               activePlaylist.size() - 1);
    }
  }
  if (!restoredTracks.empty())
    playTracks(std::move(restoredTracks), restoredIndex, restoredSource);
  else
    handler->nextSong();
}

void SpotifyEngine::previous() {
  if (currentState != State::Ready || !handler) return;
  std::vector<Track> restoredTracks;
  std::string restoredSource;
  size_t restoredIndex = 0;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    if (restoredSessionPendingStart && !activePlaylist.empty()) {
      restoredTracks = activePlaylist;
      restoredSource = activeSourceName;
      restoredIndex = activePlaylistIndex > 0 ? activePlaylistIndex - 1 : 0;
    }
  }
  if (!restoredTracks.empty())
    playTracks(std::move(restoredTracks), restoredIndex, restoredSource);
  else
    handler->previousSong();
}

void SpotifyEngine::seekToMs(uint32_t positionMs) {
  if (currentState != State::Ready || !handler) return;
  bool pendingStart = false;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    if (nowPlayingData.durationMs > 0)
      positionMs = std::min(positionMs, nowPlayingData.durationMs);
    positionBaseMs = positionMs;
    positionMeasuredAt = std::chrono::steady_clock::now();
    pendingStart = restoredSessionPendingStart;
  }
  if (!pendingStart) {
    handler->getTrackPlayer()->seekMs(positionMs);
    handler->updatePositionMs(positionMs);
  }
  notifyUpdate();
  savePlaybackSession();
}

void SpotifyEngine::setVolume(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  bool advertise = false;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    nowPlayingData.volume = value;
    if (std::abs(value - advertisedVolume) >= 0.02f || value == 0.0f ||
        value == 1.0f) {
      advertisedVolume = value;
      advertise = true;
    }
  }
#ifdef __SWITCH__
  if (switchAudioSink) switchAudioSink->setVolume(value);
#endif
  if (advertise && currentState == State::Ready && handler)
    handler->setRemoteVolume(static_cast<int>(value * 65535.0f));
  notifyUpdate();
}

void SpotifyEngine::setHighQuality(bool enabled) {
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    highQuality = enabled;
  }
  notifyUpdate();
}

bool SpotifyEngine::highQualityEnabled() {
  std::lock_guard<std::mutex> lock(dataMutex);
  return highQuality;
}

void SpotifyEngine::toggleShuffle() {
  bool enabled = false;
  bool reordered = false;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    nowPlayingData.shuffle = !nowPlayingData.shuffle;
    enabled = nowPlayingData.shuffle;

    if (!restoredSessionPendingStart && handler && activeQueueHasCurrent &&
        activeQueueIndex < activeQueue.size() && !activePlaylist.empty()) {
      const Track current = activeQueue[activeQueueIndex];
      std::vector<Track> replacement;
      size_t replacementIndex = 0;

      if (enabled) {
        replacement.reserve(activePlaylist.size());
        bool removedCurrent = false;
        for (const auto& track : activePlaylist) {
          if (!removedCurrent && sameTrack(track, current)) {
            removedCurrent = true;
            continue;
          }
          if (!track.uri.empty()) replacement.push_back(track);
        }
        if (removedCurrent && !current.uri.empty()) {
          std::mt19937 generator(std::random_device{}());
          std::shuffle(replacement.begin(), replacement.end(), generator);
          replacement.insert(replacement.begin(), current);
        } else {
          replacement.clear();
        }
      } else {
        replacement.reserve(activePlaylist.size());
        bool foundCurrent = false;
        for (const auto& track : activePlaylist) {
          if (track.uri.empty()) continue;
          if (!foundCurrent && sameTrack(track, current)) {
            replacementIndex = replacement.size();
            foundCurrent = true;
          }
          replacement.push_back(track);
        }
        if (!foundCurrent) replacement.clear();
      }

      std::vector<std::string> uris;
      uris.reserve(replacement.size());
      for (const auto& track : replacement) uris.push_back(track.uri);
      if (!uris.empty() &&
          handler->replaceLocalTrackList(uris, replacementIndex)) {
        activeQueue = std::move(replacement);
        activeQueueIndex = replacementIndex;
        activeQueueHasCurrent = true;
        reordered = true;
      }
    }
  }
  nxlog::pushf("Shuffle %s%s", enabled ? "enabled" : "disabled",
               reordered ? "; active queue reordered" : "");
  notifyUpdate();
}

void SpotifyEngine::cycleRepeatMode() {
  RepeatMode mode = RepeatMode::Off;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    switch (nowPlayingData.repeatMode) {
      case RepeatMode::Off:
        nowPlayingData.repeatMode = RepeatMode::All;
        break;
      case RepeatMode::All:
        nowPlayingData.repeatMode = RepeatMode::One;
        break;
      case RepeatMode::One:
        nowPlayingData.repeatMode = RepeatMode::Off;
        break;
    }
    mode = nowPlayingData.repeatMode;
  }
  if (handler) handler->setLocalRepeatMode(localRepeatMode(mode));
  nxlog::pushf("Repeat mode: %s", mode == RepeatMode::One
                                        ? "one"
                                        : (mode == RepeatMode::All ? "all"
                                                                    : "off"));
  notifyUpdate();
}

void SpotifyEngine::setRepeatMode(RepeatMode mode) {
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    nowPlayingData.repeatMode = mode;
  }
  if (handler) handler->setLocalRepeatMode(localRepeatMode(mode));
  nxlog::pushf("Repeat mode: %s", mode == RepeatMode::One
                                        ? "one"
                                        : (mode == RepeatMode::All ? "all"
                                                                    : "off"));
  notifyUpdate();
}

void SpotifyEngine::setEqualizer(bool enabled, float bassDb, float midDb,
                                 float trebleDb) {
  bassDb = std::clamp(bassDb, -12.0f, 12.0f);
  midDb = std::clamp(midDb, -12.0f, 12.0f);
  trebleDb = std::clamp(trebleDb, -12.0f, 12.0f);
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    equalizerEnabled = enabled;
    equalizerBassDb = bassDb;
    equalizerMidDb = midDb;
    equalizerTrebleDb = trebleDb;
  }
#ifdef __SWITCH__
  if (switchAudioSink)
    switchAudioSink->setEqualizer(enabled, bassDb, midDb, trebleDb);
#endif
}

bool SpotifyEngine::forgetCredentials() {
  // Keep the old POC credential intact, but leave a marker so the standalone
  // app does not silently import it again after the user explicitly signs out.
  const int removed = std::remove(CREDENTIALS_FILE);
  std::remove(PLAYBACK_SESSION_FILE);
  std::remove((std::string(PLAYBACK_SESSION_FILE) + ".tmp").c_str());
  std::ofstream marker(NO_MIGRATION_FILE);
  marker << "Switify sign-out marker\n";
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    dashboardCache = {};
    libraryCache = {};
    hasDashboardCache = false;
    hasLibraryCache = false;
    collectionTracksCache.clear();
    collectionTrackCacheOrder.clear();
    cachedCollectionTrackCount = 0;
    activeSourceName.clear();
    activePlaylist.clear();
    activePlaylistIndex = 0;
    activePlaylistHasCurrent = false;
    ++activePlaylistRevision;
    activeQueue.clear();
    activeQueueIndex = 0;
    activeQueueHasCurrent = false;
    restoredSessionPendingStart = false;
    nowPlayingData = {};
  }
  return (removed == 0 || !std::ifstream(CREDENTIALS_FILE).good()) &&
         marker.good();
}

void SpotifyEngine::launchWorker(std::function<void()> work) {
  // Reap completed joinable threads during normal operation. Keeping every
  // finished std::thread until shutdown retained per-thread resources and
  // amplified an earlier UI retry loop into a GPU/heap OOM. Join reaped
  // threads before constructing the replacement: if std::thread creation
  // fails under memory pressure, destroying a local joinable thread would
  // call std::terminate and kill the entire background service.
  std::vector<std::thread> finished;
  {
    std::lock_guard<std::mutex> lock(workerMutex);
    auto worker = dataWorkers.begin();
    while (worker != dataWorkers.end()) {
      if (worker->completed && worker->completed->load()) {
        finished.emplace_back(std::move(worker->thread));
        worker = dataWorkers.erase(worker);
      } else {
        ++worker;
      }
    }
  }
  for (auto& thread : finished) {
    if (thread.joinable()) thread.join();
  }

  auto completed = std::make_shared<std::atomic_bool>(false);
  auto task = std::make_shared<std::function<void()>>(std::move(work));
  const auto runTask = [task, completed] {
    try {
      (*task)();
    } catch (const std::exception& error) {
      nxlog::pushf("Unhandled data worker error: %s", error.what());
    } catch (...) {
      nxlog::push("Unhandled data worker error");
    }
    completed->store(true);
  };

  DataWorker next;
  next.completed = completed;
  try {
    next.thread = std::thread(
        [runTask] { runTask(); });
  } catch (const std::exception& error) {
    // A browse request is more useful running synchronously than leaving the
    // overlay permanently Loading or terminating the service. This path is
    // exceptional; normal requests still use a worker thread.
    nxlog::pushf("Data worker start failed; running inline: %s", error.what());
    runTask();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(workerMutex);
    dataWorkers.emplace_back(std::move(next));
  }
}

// ---- Token and Web API ---------------------------------------------------

std::string SpotifyEngine::spotifyClientToken() {
  auto context = ctx;
  if (!context || !running) return "";
  const long long now = context->timeProvider->getSyncedTimestamp();
  if (!cachedClientToken.empty() && now < clientTokenExpiresAt)
    return cachedClientToken;

  try {
    // ClientTokenRequest.request_type = REQUEST_CLIENT_DATA_REQUEST.
    // The Linux platform submessage is intentionally empty, matching the
    // maintained go-librespot implementation for non-mobile clients.
    std::vector<uint8_t> linuxData;
    std::vector<uint8_t> platformData;
    protoBytes(platformData, 5, linuxData);
    std::vector<uint8_t> connectivity;
    protoBytes(connectivity, 1, platformData);
    protoString(connectivity, 2, context->config.deviceId);
    std::vector<uint8_t> clientData;
    protoString(clientData, 1, "0.0.0");
    protoString(clientData, 2, WEB_CLIENT_ID);
    protoBytes(clientData, 3, connectivity);
    std::vector<uint8_t> request;
    protoVarint(request, (1u << 3) | 0u);
    protoVarint(request, 1);
    protoBytes(request, 2, clientData);

    const std::string url =
        "https://clienttoken.spotify.com/v1/clienttoken";
    WebHeaders headers = {
        {"Accept", "application/x-protobuf"},
        {"Content-Type", "application/x-protobuf"},
        {"User-Agent", "Spotify/127700358 Linux/0 (Switify)"}};
    const auto response = webRequest(url, "POST", request, headers);
    nxlog::pushf(
        "Client token HTTP response (status=%d, bytes=%d, length=%s, "
        "transfer=%s)",
        response.statusCode, static_cast<int>(response.body.size()),
        response.header("content-length").c_str(),
        response.header("transfer-encoding").c_str());
    const auto& body = response.body;
    if (response.statusCode != 200) {
      nxlog::pushf("Client token request failed (HTTP %d)",
                   response.statusCode);
      return "";
    }

    uint64_t responseType = 0;
    std::vector<uint8_t> granted;
    std::vector<uint8_t> tokenBytes;
    if (!findProtoVarint(body, 1, responseType) || responseType != 1 ||
        !findProtoBytes(body, 2, granted) ||
        !findProtoBytes(granted, 1, tokenBytes) || tokenBytes.empty()) {
      nxlog::pushf(
          "Client token response unsupported (type=%d, bytes=%d)",
                   static_cast<int>(responseType),
                   static_cast<int>(body.size()));
      return "";
    }
    uint64_t refreshSeconds = 7200;
    findProtoVarint(granted, 3, refreshSeconds);
    cachedClientToken.assign(tokenBytes.begin(), tokenBytes.end());
    clientTokenExpiresAt = now +
        static_cast<long long>(std::max<uint64_t>(60, refreshSeconds -
                                                        std::min<uint64_t>(60, refreshSeconds))) *
            1000LL;
    nxlog::push("Spotify client token acquired");
    return cachedClientToken;
  } catch (const std::exception& error) {
    nxlog::pushf("Client token request failed: %s", error.what());
    cachedClientToken.clear();
    clientTokenExpiresAt = 0;
    return "";
  }
}

std::string SpotifyEngine::login5AccessToken() {
  auto context = ctx;
  if (!context || !running) return "";
  const long long now = context->timeProvider->getSyncedTimestamp();
  if (!cachedLogin5Token.empty() && now < login5TokenExpiresAt)
    return cachedLogin5Token;
  if (now < login5RetryAfter) return "";

  try {
    const std::string clientToken = spotifyClientToken();
    if (clientToken.empty()) {
      login5RetryAfter = now + 5000;
      return "";
    }
    if (context->config.username.empty() || context->config.authData.empty()) {
      nxlog::push("Login5 skipped: stored Spotify credential unavailable");
      login5RetryAfter = now + 5000;
      return "";
    }

    std::vector<uint8_t> clientInfo;
    protoString(clientInfo, 1, WEB_CLIENT_ID);
    protoString(clientInfo, 2, context->config.deviceId);
    std::vector<uint8_t> storedCredential;
    protoString(storedCredential, 1, context->config.username);
    protoBytes(storedCredential, 2, context->config.authData);
    std::vector<uint8_t> request;
    protoBytes(request, 1, clientInfo);
    protoBytes(request, 100, storedCredential);

    const std::string url = "https://login5.spotify.com/v3/login";
    WebHeaders headers = {
        {"Accept", "application/x-protobuf"},
        {"Content-Type", "application/x-protobuf"},
        {"User-Agent", "Spotify/127700358 Linux/0 (Switify)"},
        {"Client-Token", clientToken}};
    auto response = webRequest(url, "POST", request, headers);
    auto& body = response.body;
    if (response.statusCode != 200 || body.empty()) {
      nxlog::pushf("Login5 request failed (HTTP %d, bytes=%d)",
                   response.statusCode, static_cast<int>(body.size()));
      login5RetryAfter = now + 5000;
      return "";
    }

    LoginResponse loginResponse = LoginResponse_init_zero;
    pbDecode(loginResponse, LoginResponse_fields, body);
    if (loginResponse.which_response != LoginResponse_ok_tag ||
        !loginResponse.response.ok.access_token ||
        loginResponse.response.ok.access_token[0] == '\0') {
      const int errorCode =
          loginResponse.which_response == LoginResponse_error_tag
              ? static_cast<int>(loginResponse.response.error)
              : -1;
      nxlog::pushf("Login5 rejected stored credential (code=%d)", errorCode);
      pb_release(LoginResponse_fields, &loginResponse);
      login5RetryAfter = now + 5000;
      return "";
    }

    cachedLogin5Token = loginResponse.response.ok.access_token;
    const int expiresIn = loginResponse.response.ok.has_access_token_expires_in
                              ? loginResponse.response.ok.access_token_expires_in
                              : 3600;
    pb_release(LoginResponse_fields, &loginResponse);
    login5TokenExpiresAt = now + std::max(60, expiresIn - 60) * 1000LL;
    login5RetryAfter = 0;
    nxlog::push("Login5 Web API token acquired");
    return cachedLogin5Token;
  } catch (const std::exception& error) {
    nxlog::pushf("Login5 token request failed: %s", error.what());
    cachedLogin5Token.clear();
    login5TokenExpiresAt = 0;
    login5RetryAfter = now + 5000;
    return "";
  }
}

std::string SpotifyEngine::partnerRequest(
    const std::string& operation, const std::string& variablesJson) {
  const auto hash = PATHFINDER_HASHES.find(operation);
  if (hash == PATHFINDER_HASHES.end())
    throw std::runtime_error("Unknown Spotify partner operation: " +
                             operation);

  const Json extensions = {
      {"persistedQuery", {{"version", 1}, {"sha256Hash", hash->second}}}};

  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string accessToken;
    std::string clientToken;
    {
      // The two token refresh paths share cached state and must be serialized,
      // but the network data request itself must not hold the token mutex.
      std::lock_guard<std::mutex> lock(tokenMutex);
      accessToken = login5AccessToken();
      clientToken = spotifyClientToken();
    }
    if (accessToken.empty() || clientToken.empty())
      throw std::runtime_error("Spotify partner session unavailable");

    std::string requestVariables = variablesJson;
    if (operation == "queryHomeSection") {
      Json variables = Json::parse(variablesJson);
      variables["sp_t"] = accessToken;
      requestVariables = variables.dump();
    }
    const std::string url =
        "https://api-partner.spotify.com/pathfinder/v1/query?operationName=" +
        urlEncode(operation) + "&variables=" + urlEncode(requestVariables) +
        "&extensions=" + urlEncode(extensions.dump());

    const WebHeaders headers = {
        {"Authorization", "Bearer " + accessToken},
        {"Client-Token", clientToken},
        {"Spotify-App-Version", SPOTIFY_APP_VERSION},
        {"Accept", "application/json"},
        {"Accept-Language", "en-US"},
        {"app-platform", "WebPlayer"},
        {"User-Agent", "Spotify/127700358 Linux/0 (Switify)"}};
    const auto started = std::chrono::steady_clock::now();
    WebResponse response;
    try {
      response = webRequest(url, "POST", {}, headers);
    } catch (const std::exception& error) {
      nxlog::pushf("Pathfinder %s transport error: %s", operation.c_str(),
                   error.what());
      if (attempt == 0) continue;
      throw;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const std::string body = responseText(response.body);
    nxlog::pushf("Pathfinder %s -> HTTP %d, %d bytes, %lld ms",
                 operation.c_str(), response.statusCode,
                 static_cast<int>(response.body.size()),
                 static_cast<long long>(elapsed));

    if (response.statusCode == 401 && attempt == 0) {
      std::lock_guard<std::mutex> lock(tokenMutex);
      cachedLogin5Token.clear();
      login5TokenExpiresAt = 0;
      login5RetryAfter = 0;
      continue;
    }

    std::string detail;
    try {
      const Json payload = Json::parse(body);
      const auto errors = payload.find("errors");
      if (errors != payload.end() && errors->is_array() && !errors->empty()) {
        detail = jsonString(errors->front(), "message");
        if (detail.empty()) detail = "Pathfinder query failed";
      }
      if (response.statusCode >= 200 && response.statusCode < 300 &&
          detail.empty())
        return body;
      if (detail.empty()) {
        const auto error = payload.find("error");
        if (error != payload.end()) {
          detail = error->is_string() ? error->get<std::string>()
                                      : jsonString(*error, "message");
        }
      }
    } catch (const std::exception& error) {
      if (response.statusCode >= 200 && response.statusCode < 300)
        throw std::runtime_error("Spotify partner returned invalid JSON: " +
                                 std::string(error.what()));
    }
    if (response.statusCode == 429)
      detail = "Spotify partner service is temporarily busy";
    throw std::runtime_error(
        "Spotify partner " + std::to_string(response.statusCode) +
        (detail.empty() ? std::string() : ": " + detail));
  }
  throw std::runtime_error("Spotify partner authentication failed");
}

std::string SpotifyEngine::connectRequest(const std::string& url) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string accessToken;
    std::string clientToken;
    {
      std::lock_guard<std::mutex> lock(tokenMutex);
      accessToken = login5AccessToken();
      clientToken = spotifyClientToken();
    }
    if (accessToken.empty() || clientToken.empty())
      throw std::runtime_error("Spotify partner session unavailable");
    const WebHeaders headers = {
        {"Authorization", "Bearer " + accessToken},
        {"Client-Token", clientToken},
        {"Spotify-App-Version", SPOTIFY_APP_VERSION},
        {"Accept", "application/json"},
        {"Accept-Language", "en-US"},
        {"app-platform", "WebPlayer"},
        {"User-Agent", "Spotify/127700358 Linux/0 (Switify)"}};
    const auto started = std::chrono::steady_clock::now();
    const auto response = webRequest(url, "GET", {}, headers);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    nxlog::pushf("Spotify internal GET -> HTTP %d, %d bytes, %lld ms",
                 response.statusCode, static_cast<int>(response.body.size()),
                 static_cast<long long>(elapsed));
    if (response.statusCode == 401 && attempt == 0) {
      std::lock_guard<std::mutex> lock(tokenMutex);
      cachedLogin5Token.clear();
      login5TokenExpiresAt = 0;
      login5RetryAfter = 0;
      continue;
    }
    const std::string body = responseText(response.body);
    if (response.statusCode >= 200 && response.statusCode < 300) return body;
    std::string detail;
    try {
      const auto payload = Json::parse(body);
      const auto error = payload.find("error");
      if (error != payload.end())
        detail = error->is_string() ? error->get<std::string>()
                                    : jsonString(*error, "message");
    } catch (...) {
    }
    throw std::runtime_error(
        "Spotify internal " + std::to_string(response.statusCode) +
        (detail.empty() ? std::string() : ": " + detail));
  }
  throw std::runtime_error("Spotify internal authentication failed");
}

std::string SpotifyEngine::userToken(const std::string& scopes) {
  std::lock_guard<std::mutex> lock(tokenMutex);
  auto context = ctx;
  const long long now = context
                            ? context->timeProvider->getSyncedTimestamp()
                            : 0;
  if (!context || !running) return "";

  auto& cache = userTokenCache[scopes];
  if (!cache.token.empty() && now < cache.expiresAt) return cache.token;
  if (now < cache.retryAfter) return "";

  // login5 is the current credential-to-token bridge. It uses the same stored
  // credential already produced by Switify's Connect setup and replaces the
  // retired keymaster path without requiring a second user login.
  const std::string loginToken = login5AccessToken();
  if (!loginToken.empty()) {
    cache.token = loginToken;
    cache.expiresAt = login5TokenExpiresAt;
    cache.retryAfter = 0;
    return cache.token;
  }

  struct TokenClient {
    const char* label;
    const char* id;
  };
  // Keymaster allows different scope sets for different Spotify clients. The
  // desktop identity no longer accepts every Web API scope on all accounts,
  // so negotiate the three current identities used by librespot instead of
  // tying the whole application to a single brittle client/scope bundle.
  constexpr TokenClient clients[] = {
      {"desktop", WEB_CLIENT_ID},
      {"android", ANDROID_CLIENT_ID},
      {"ios", IOS_CLIENT_ID},
  };

  struct TokenResponse {
    std::shared_ptr<bell::WrappedSemaphore> semaphore =
        std::make_shared<bell::WrappedSemaphore>(1);
    std::string body;
    bool failed = false;
    int statusCode = 0;
  };

  for (size_t candidate = 0; candidate < std::size(clients) && running;
       ++candidate) {
    const size_t clientIndex =
        (preferredTokenClient + candidate) % std::size(clients);
    const auto& client = clients[clientIndex];
    const std::string uri =
        std::string("hm://keymaster/token/authenticated?scope=") + scopes +
        "&client_id=" + client.id +
        "&device_id=" + context->config.deviceId;
    auto result = std::make_shared<TokenResponse>();
    const uint64_t sequence = context->session->execute(
        cspot::MercurySession::RequestType::GET, uri,
        [result](cspot::MercurySession::Response& response) {
          result->failed = response.fail;
          if (response.mercuryHeader.has_status_code)
            result->statusCode = response.mercuryHeader.status_code;
          if (!response.parts.empty())
            result->body.assign(response.parts[0].begin(),
                                response.parts[0].end());
          result->semaphore->give();
        });

    bool completed = false;
    for (int waitedMs = 0; waitedMs < 8000 && running; waitedMs += 200) {
      if (result->semaphore->twait(200) == 0) {
        completed = true;
        break;
      }
    }

    if (!running) {
      context->session->unregister(sequence);
      return "";
    }
    if (!completed) {
      context->session->unregister(sequence);
      nxlog::pushf("User token request timed out (client=%s)", client.label);
      continue;
    }
    if (result->body.empty()) {
      nxlog::pushf("User token request interrupted (client=%s)", client.label);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    try {
      const auto json = Json::parse(result->body);
      // Keymaster traditionally uses camelCase (accessToken/expiresIn), but
      // some current AP responses use the OAuth snake_case spelling. Accept
      // both and never treat an error JSON object as a valid empty token.
      std::string token =
          json.value("accessToken", json.value("access_token", ""));
      const int expiresIn =
          json.value("expiresIn", json.value("expires_in", 3600));
      if (result->failed || token.empty()) {
        std::string detail = json.value(
            "errorDescription",
            json.value("error_description", json.value("error", "")));
        std::string fields;
        if (json.is_object()) {
          for (auto it = json.begin(); it != json.end(); ++it) {
            if (!fields.empty()) fields += ",";
            fields += it.key();
          }
        }
        nxlog::pushf(
            "Keymaster rejected user token (client=%s status=%d)%s%s "
            "(fields: %s)",
            client.label, result->statusCode, detail.empty() ? "" : ": ",
            detail.c_str(), fields.c_str());
        continue;
      }
      cache.token = std::move(token);
      cache.expiresAt = context->timeProvider->getSyncedTimestamp() +
                        std::max(60, expiresIn - 60) * 1000LL;
      cache.retryAfter = 0;
      preferredTokenClient = clientIndex;
      int scopeCount = 0;
      if (json.contains("scope") && json["scope"].is_array()) {
        scopeCount = static_cast<int>(json["scope"].size());
      } else if (json.contains("scope") && json["scope"].is_string()) {
        const auto scopes = json["scope"].get<std::string>();
        scopeCount = scopes.empty()
                         ? 0
                         : 1 + static_cast<int>(std::count(scopes.begin(),
                                                          scopes.end(), ' '));
      }
      nxlog::pushf("User token acquired (client=%s, %d scopes)",
                   client.label, scopeCount);
      return cache.token;
    } catch (const std::exception& error) {
      nxlog::pushf("Token parse failed (body=%d bytes): %s",
                   static_cast<int>(result->body.size()), error.what());
      cache.token.clear();
    }
  }
  cache.retryAfter = context->timeProvider->getSyncedTimestamp() + 5000;
  return "";
}

std::string SpotifyEngine::apiRequest(const std::string& method,
                                      const std::string& path,
                                      const std::string& jsonBody) {
  const std::string url = "https://api.spotify.com" + path;
  const auto context = ctx;
  const long long now = context
                            ? context->timeProvider->getSyncedTimestamp()
                            : 0;
  const long long retryAt = apiRetryAfter.load();
  if (now > 0 && now < retryAt) {
    const long long seconds = std::max(1LL, (retryAt - now + 999) / 1000);
    throw std::runtime_error("Spotify rate limit active; try again in " +
                             std::to_string(seconds) + " seconds");
  }
  std::string scopes = "user-read-private";
  if (path.rfind("/v1/me/player/recently-played", 0) == 0) {
    scopes = "user-read-recently-played";
  } else if (path.rfind("/v1/me/top/", 0) == 0) {
    scopes = "user-top-read";
  } else if (path.rfind("/v1/me/playlists", 0) == 0 ||
             path.rfind("/v1/playlists/", 0) == 0) {
    scopes = "playlist-read-private,playlist-read-collaborative";
  } else if (path.rfind("/v1/me/albums", 0) == 0 ||
             path.rfind("/v1/me/tracks", 0) == 0 ||
             path.rfind("/v1/me/shows", 0) == 0 ||
             path.rfind("/v1/me/episodes", 0) == 0) {
    scopes = "user-library-read";
  } else if (path.rfind("/v1/me/library", 0) == 0) {
    scopes = method == "GET" ? "user-library-read" : "user-library-modify";
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::string token = userToken(scopes);
    if (token.empty()) throw std::runtime_error("Spotify user token unavailable");

    WebHeaders headers = {
        {"Authorization", "Bearer " + token},
        {"Accept", "application/json"},
        {"User-Agent", "Spotify/127700358 Linux/0 (Switify)"}};
    std::vector<uint8_t> requestBody(jsonBody.begin(), jsonBody.end());
    if (!requestBody.empty())
      headers.emplace_back("Content-Type", "application/json");
    {
      std::lock_guard<std::mutex> lock(tokenMutex);
      if (!cachedClientToken.empty())
        headers.emplace_back("Client-Token", cachedClientToken);
    }
    const auto requestStarted = std::chrono::steady_clock::now();
    WebResponse response;
    try {
      response = webRequest(url, method, requestBody, headers);
    } catch (const std::exception& error) {
      nxlog::pushf("Spotify API %s %s transport error: %s", method.c_str(),
                   path.c_str(), error.what());
      throw;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - requestStarted)
                               .count();
    const int statusCode = response.statusCode;
    const std::string body = responseText(response.body);
    nxlog::pushf(
        "Spotify API %s %s -> HTTP %d, %d bytes, %lld ms, retry-after=%s",
                 method.c_str(), path.c_str(), statusCode,
                 static_cast<int>(response.body.size()),
                 static_cast<long long>(elapsedMs),
                 response.header("retry-after").c_str());

    if (statusCode == 401 && attempt == 0) {
      std::lock_guard<std::mutex> lock(tokenMutex);
      userTokenCache.erase(scopes);
      cachedLogin5Token.clear();
      login5TokenExpiresAt = 0;
      login5RetryAfter = 0;
      continue;
    }
    if (statusCode == 429) {
      int waitSeconds = 60;
      try {
        const std::string retryAfter(response.header("retry-after"));
        if (!retryAfter.empty()) waitSeconds = std::stoi(retryAfter);
      } catch (...) {
      }
      waitSeconds = std::clamp(waitSeconds, 1, 24 * 60 * 60);
      const long long limitedAt = context
                                      ? context->timeProvider->getSyncedTimestamp()
                                      : now;
      const long long nextAttempt = limitedAt + waitSeconds * 1000LL;
      long long previous = apiRetryAfter.load();
      while (previous < nextAttempt &&
             !apiRetryAfter.compare_exchange_weak(previous, nextAttempt)) {
      }
      throw std::runtime_error(
          "Spotify rate limit active; try again in " +
          std::to_string(waitSeconds) + " seconds");
    }
    if (statusCode < 200 || statusCode >= 300) {
      std::string detail;
      try {
        const auto json = Json::parse(body);
        if (json.contains("error")) {
          if (json["error"].is_object())
            detail = json["error"].value("message", "");
          else if (json["error"].is_string())
            detail = json["error"].get<std::string>();
        }
      } catch (...) {
      }
      throw std::runtime_error("Spotify API " + std::to_string(statusCode) +
                               (detail.empty() ? "" : ": " + detail));
    }
    const long long completedAt = context
                                      ? context->timeProvider->getSyncedTimestamp()
                                      : 0;
    const long long activeRetryAt = apiRetryAfter.load();
    // A successful request that was already in flight must not erase a newer
    // 429 cooldown reported by another worker.
    if (activeRetryAt <= 0 || completedAt <= 0 || completedAt >= activeRetryAt)
      apiRetryAfter = 0;
    return body;
  }
  throw std::runtime_error("Spotify API request failed");
}

bool SpotifyEngine::waitForApiRateLimitWindow() {
  while (running.load()) {
    const long long retryAt = apiRetryAfter.load();
    if (retryAt <= 0) return true;

    const auto context = ctx;
    const long long now = context
                              ? context->timeProvider->getSyncedTimestamp()
                              : 0;
    if (now <= 0 || now >= retryAt) return true;

    const long long remainingMs = retryAt - now;
    std::this_thread::sleep_for(std::chrono::milliseconds(
        std::min(remainingMs, 250LL)));
  }
  return false;
}

// ---- Data layer ----------------------------------------------------------

void SpotifyEngine::fetchDashboard(
    std::function<void(Result<HomeData>)> cb, bool forceRefresh) {
#ifndef SWITIFY_BACKGROUND_SERVICE
  if (!forceRefresh) {
    Result<HomeData> cached;
    {
      std::lock_guard<std::mutex> lock(dataMutex);
      if (hasDashboardCache) cached.value = dashboardCache;
      else cached.error = "cache miss";
    }
    if (cached.error.empty()) {
      nxlog::push("Dashboard cache hit");
      launchWorker([cb = std::move(cb), cached = std::move(cached)]() mutable {
        cb(std::move(cached));
      });
      return;
    }
  }
#else
  (void)forceRefresh;
#endif
  launchWorker([this, cb = std::move(cb)] {
    Result<HomeData> result;
    if (currentState != State::Ready) {
      result.error = "Spotify is still connecting";
      cb(std::move(result));
      return;
    }
    int successfulSections = 0;
    std::string username;
    try {
      const auto json = Json::parse(partnerRequest("profileAttributes", "{}"));
      const Json* profile = jsonPath(json, {"data", "me", "profile"});
      if (!profile || !profile->is_object())
        throw std::runtime_error("Profile response was incomplete");
      username = jsonString(*profile, "username");
      const std::string profileUri = jsonString(*profile, "uri");
      if (username.empty()) username = idFromUri(profileUri);
      result.value.profile.id = username;
      result.value.profile.displayName = jsonString(*profile, "name");
      if (result.value.profile.displayName.empty())
        result.value.profile.displayName = jsonString(*profile, "displayName");
      if (result.value.profile.displayName.empty())
        result.value.profile.displayName = username;
      result.value.profile.imageUrl = partnerImage(*profile);
      {
        std::lock_guard<std::mutex> lock(dataMutex);
        profileData = result.value.profile;
      }
      ++successfulSections;
    } catch (const std::exception& error) {
      appendWarning(result.value.warning,
                    "Profile: " + std::string(error.what()));
    }

    try {
      if (username.empty()) {
        const auto context = ctx;
        if (context) username = context->config.username;
      }
      if (username.empty()) throw std::runtime_error("Spotify username unavailable");
      const std::string historyUrl =
          "https://spclient.wg.spotify.com/recently-played/v3/user/" +
          urlEncode(username) +
          "/recently-played?format=json&offset=0&limit=50&filter=" +
          urlEncode("default,collection-new-episodes");
      const auto history = Json::parse(connectRequest(historyUrl));
      Json uris = Json::array();
      std::unordered_set<std::string> requested;
      std::unordered_set<std::string> seenContexts;
      std::vector<std::pair<std::string, std::string>> historyItems;
      const auto contexts = history.find("playContexts");
      if (contexts != history.end() && contexts->is_array()) {
        for (const auto& entry : *contexts) {
          const std::string trackUri =
              jsonString(entry, "lastPlayedTrackUri");
          const std::string contextUri = jsonString(entry, "uri");
          if (trackUri.empty()) continue;
          if (!contextUri.empty() && !startsWith(contextUri, "spotify:track:") &&
              !seenContexts.insert(contextUri).second)
            continue;
          historyItems.emplace_back(contextUri, trackUri);
          if (requested.insert(trackUri).second) uris.push_back(trackUri);
          if (!contextUri.empty() && requested.insert(contextUri).second)
            uris.push_back(contextUri);
          if (historyItems.size() >= 10) break;
        }
      }
      if (!uris.empty()) {
        const auto lookup = Json::parse(partnerRequest(
            "fetchEntitiesForRecentlyPlayed", Json{{"uris", uris}}.dump()));
        std::unordered_map<std::string, Track> byUri;
        std::unordered_map<std::string, Collection> collectionsByUri;
        const Json* entries = jsonPath(lookup, {"data", "lookup"});
        if (entries && entries->is_array()) {
          for (const auto& entry : *entries) {
            std::string key = jsonString(entry, "_uri");
            if (startsWith(key, "spotify:track:")) {
              auto track = parsePartnerTrack(entry);
              if (!track.uri.empty()) byUri[key] = std::move(track);
            } else if (startsWith(key, "spotify:episode:")) {
              auto episode = parsePartnerEpisode(entry);
              if (!episode.uri.empty()) byUri[key] = std::move(episode);
            } else if (startsWith(key, "spotify:playlist:")) {
              auto collection = parsePartnerCollection(
                  entry, Collection::Kind::PLAYLIST);
              if (!collection.uri.empty())
                collectionsByUri[key] = std::move(collection);
            } else if (startsWith(key, "spotify:album:")) {
              auto collection =
                  parsePartnerCollection(entry, Collection::Kind::ALBUM);
              if (!collection.uri.empty())
                collectionsByUri[key] = std::move(collection);
            } else if (startsWith(key, "spotify:artist:")) {
              auto collection =
                  parsePartnerCollection(entry, Collection::Kind::ARTIST);
              if (!collection.uri.empty())
                collectionsByUri[key] = std::move(collection);
            } else if (startsWith(key, "spotify:show:")) {
              auto collection =
                  parsePartnerCollection(entry, Collection::Kind::SHOW);
              if (!collection.uri.empty())
                collectionsByUri[key] = std::move(collection);
            }
          }
        }
        std::unordered_set<std::string> recentTracks;
        for (const auto& [contextUri, trackUri] : historyItems) {
          const auto track = byUri.find(trackUri);
          if (track != byUri.end() && recentTracks.insert(trackUri).second)
            result.value.recentlyPlayed.push_back(track->second);

          Collection collection;
          if (contextUri == "spotify:collection:tracks") {
            collection.uri = contextUri;
            collection.name = "Liked Songs";
            collection.kind = Collection::Kind::LIKED;
          } else {
            const auto found = collectionsByUri.find(contextUri);
            if (found != collectionsByUri.end()) collection = found->second;
          }
          // Some lookup payloads omit context metadata. Keep the context
          // playable and derive a useful card from its last track.
          if (collection.uri.empty() && !contextUri.empty()) {
            collection.uri = contextUri;
            collection.id = idFromUri(contextUri);
            if (startsWith(contextUri, "spotify:playlist:")) {
              collection.kind = Collection::Kind::PLAYLIST;
              collection.name = "Continue playlist";
            } else if (startsWith(contextUri, "spotify:album:")) {
              collection.kind = Collection::Kind::ALBUM;
              collection.name = track != byUri.end() ? track->second.album
                                                     : "Continue album";
            } else if (startsWith(contextUri, "spotify:artist:")) {
              collection.kind = Collection::Kind::ARTIST;
              collection.name = track != byUri.end() ? track->second.artist
                                                     : "Continue artist";
            } else if (startsWith(contextUri, "spotify:show:")) {
              collection.kind = Collection::Kind::SHOW;
              collection.name = track != byUri.end() ? track->second.album
                                                     : "Continue podcast";
            }
          }
          if (!collection.uri.empty() && track != byUri.end()) {
            collection.subtitle = "Continue from " + track->second.name;
            if (collection.imageUrl.empty())
              collection.imageUrl = track->second.imageUrl;
            result.value.resumeContexts.push_back(
                ResumeContext{std::move(collection), track->second});
          }
        }
      }
      ++successfulSections;
    } catch (const std::exception& error) {
      appendWarning(result.value.warning,
                    "Recently played: " + std::string(error.what()));
    }

#ifndef SWITIFY_BACKGROUND_SERVICE
    try {
      const Json variables = {
          {"includeTopArtists", false},
          {"topArtistsInput",
           {{"offset", 0},
            {"limit", 0},
            {"sortBy", "AFFINITY"},
            {"timeRange", "SHORT_TERM"}}},
          {"includeTopTracks", true},
          {"topTracksInput",
           {{"offset", 0},
            {"limit", 10},
            {"sortBy", "AFFINITY"},
            {"timeRange", "SHORT_TERM"}}}};
      const auto json = Json::parse(
          partnerRequest("userTopContent", variables.dump()));
      appendPartnerItems(jsonPath(
                             json, {"data", "me", "profile", "topTracks"}),
                         result.value.topTracks,
                         [](const Json& item) { return parsePartnerTrack(item); });
      ++successfulSections;
    } catch (const std::exception& error) {
      appendWarning(result.value.warning,
                    "Top tracks: " + std::string(error.what()));
    }
#endif

    try {
      const auto json = Json::parse(partnerRequest(
          "libraryV3", libraryRequestVariables("Playlists", 10).dump()));
      appendPartnerItems(
          jsonPath(json, {"data", "me", "libraryV3"}),
          result.value.playlists, [](const Json& item) {
            return parsePartnerCollection(item, Collection::Kind::PLAYLIST);
          });
      ++successfulSections;
    } catch (const std::exception& error) {
      appendWarning(result.value.warning,
                    "Playlists: " + std::string(error.what()));
    }

#ifndef SWITIFY_BACKGROUND_SERVICE
    try {
      appendSavedPodcasts(
          Json::parse(partnerRequest(
              "libraryV3",
              libraryRequestVariables("Podcasts & Shows", 50).dump())),
          result.value.podcasts);
      nxlog::pushf("Dashboard podcast library: %d followed shows",
                   static_cast<int>(result.value.podcasts.size()));
      ++successfulSections;
    } catch (const std::exception& error) {
      appendWarning(result.value.warning,
                    "Podcasts: " + std::string(error.what()));
    }

    const struct {
      const char* uri;
      const char* fallbackTitle;
    } homeSections[] = {
        {"spotify:section:0JQ5DAUnp4wcj0bCb3wh3S", "Made for you"},
        {"spotify:section:0JQ5DAnM3wGh0gz1MXnu89", "Your top mixes"},
        {"spotify:section:0JQ5DAnM3wGh0gz1MXnu3R",
         "Recommended stations"}};
    for (const auto& section : homeSections) {
      try {
        const auto context = ctx;
        const std::string country =
            context && !context->config.countryCode.empty()
                ? context->config.countryCode
                : "US";
        const std::string timeZone =
            country == "BR" ? "America/Sao_Paulo" : "Etc/UTC";
        const Json variables = {{"sectionItemsLimit", 12},
                                {"sectionItemsOffset", 0},
                                {"timeZone", timeZone},
                                {"country", country},
                                {"uri", section.uri}};
        auto shelves = parseHomeShelves(Json::parse(
            partnerRequest("queryHomeSection", variables.dump())));
        for (auto& shelf : shelves) {
          if (shelf.title.empty()) shelf.title = section.fallbackTitle;
          result.value.shelves.push_back(std::move(shelf));
        }
        ++successfulSections;
      } catch (const std::exception& error) {
        appendWarning(result.value.warning,
                      std::string(section.fallbackTitle) + ": " + error.what());
      }
    }
#endif

    if (successfulSections == 0)
      result.error = result.value.warning.empty()
                         ? "Could not load Spotify Home"
                         : result.value.warning;
    nxlog::pushf(
                 "Dashboard: %d contexts, %d recent, %d top, %d playlists, "
                 "%d podcasts, "
                 "%d curated shelves%s%s",
                 static_cast<int>(result.value.resumeContexts.size()),
                 static_cast<int>(result.value.recentlyPlayed.size()),
                 static_cast<int>(result.value.topTracks.size()),
                 static_cast<int>(result.value.playlists.size()),
                 static_cast<int>(result.value.podcasts.size()),
                 static_cast<int>(result.value.shelves.size()),
                 result.value.warning.empty() ? "" : " | ",
                 result.value.warning.c_str());
#ifndef SWITIFY_BACKGROUND_SERVICE
    if (result.error.empty()) {
      std::lock_guard<std::mutex> lock(dataMutex);
      dashboardCache = result.value;
      hasDashboardCache = true;
    }
#endif
    // The callback owns this request's UI update. Broadcasting another engine
    // update here used to make Home interpret an error/empty result as a fresh
    // first load and launch an unbounded refresh loop.
    cb(std::move(result));
  });
}

void SpotifyEngine::fetchLibrary(
    std::function<void(Result<LibraryData>)> cb, bool forceRefresh) {
#ifndef SWITIFY_BACKGROUND_SERVICE
  if (!forceRefresh) {
    Result<LibraryData> cached;
    {
      std::lock_guard<std::mutex> lock(dataMutex);
      if (hasLibraryCache) cached.value = libraryCache;
      else cached.error = "cache miss";
    }
    if (cached.error.empty()) {
      nxlog::push("Library cache hit");
      launchWorker([cb = std::move(cb), cached = std::move(cached)]() mutable {
        cb(std::move(cached));
      });
      return;
    }
  }
#else
  (void)forceRefresh;
#endif
  launchWorker([this, cb = std::move(cb)] {
    Result<LibraryData> result;
    if (currentState != State::Ready) {
      result.error = "Spotify is still connecting";
      cb(std::move(result));
      return;
    }
    Collection liked;
    liked.name = "Liked Songs";
    liked.subtitle = "Your saved tracks";
    liked.kind = Collection::Kind::LIKED;
    liked.uri = "spotify:collection:tracks";
    result.value.playlists.push_back(std::move(liked));

    int successfulSections = 0;
    std::string errors;
    try {
      const auto playlists = Json::parse(
          partnerRequest("libraryV3",
                         libraryRequestVariables("Playlists", 50).dump()));
      appendPartnerItems(
          jsonPath(playlists, {"data", "me", "libraryV3"}),
          result.value.playlists, [](const Json& item) {
            return parsePartnerCollection(item, Collection::Kind::PLAYLIST);
          });
      ++successfulSections;
    } catch (const std::exception& error) {
      appendWarning(errors, "Playlists: " + std::string(error.what()));
    }
#ifndef SWITIFY_BACKGROUND_SERVICE
    try {
      const auto albums = Json::parse(
          partnerRequest("libraryV3",
                         libraryRequestVariables("Albums", 50).dump()));
      appendPartnerItems(
          jsonPath(albums, {"data", "me", "libraryV3"}),
          result.value.albums, [](const Json& item) {
            return parsePartnerCollection(item, Collection::Kind::ALBUM);
          });
      ++successfulSections;
    } catch (const std::exception& error) {
      appendWarning(errors, "Albums: " + std::string(error.what()));
    }
#endif
    try {
      appendSavedPodcasts(
          Json::parse(partnerRequest(
              "libraryV3",
              libraryRequestVariables("Podcasts & Shows", 50).dump())),
          result.value.podcasts);
      nxlog::pushf("Library podcast filter: %d followed shows",
                   static_cast<int>(result.value.podcasts.size()));
      ++successfulSections;
    } catch (const std::exception& error) {
      appendWarning(errors, "Podcasts: " + std::string(error.what()));
    }
    if (successfulSections == 0) result.error = errors;
    nxlog::pushf("Library: %d collections, %d albums, %d podcasts%s%s",
                 static_cast<int>(result.value.playlists.size()),
                 static_cast<int>(result.value.albums.size()),
                 static_cast<int>(result.value.podcasts.size()),
                 errors.empty() ? "" : " | ", errors.c_str());
#ifndef SWITIFY_BACKGROUND_SERVICE
    if (result.error.empty()) {
      std::lock_guard<std::mutex> lock(dataMutex);
      libraryCache = result.value;
      hasLibraryCache = true;
    }
#endif
    cb(std::move(result));
  });
}

void SpotifyEngine::fetchPlaylistTrackPage(
    const Collection& collection, size_t offset, size_t limit,
    std::function<void(Result<TrackPage>)> cb) {
  limit = std::clamp<size_t>(limit, 1, 50);

  if (collection.kind != Collection::Kind::LIKED &&
      collection.kind != Collection::Kind::PLAYLIST &&
      collection.kind != Collection::Kind::SHOW) {
    fetchPlaylistTracks(
        collection,
        [offset, limit, cb = std::move(cb)](
            Result<std::vector<Track>> complete) mutable {
          Result<TrackPage> result;
          if (!complete.ok()) {
            result.error = std::move(complete.error);
          } else {
            result.value.offset = offset;
            const size_t first = std::min(offset, complete.value.size());
            const size_t last =
                std::min(complete.value.size(), first + limit);
            result.value.tracks.insert(
                result.value.tracks.end(),
                std::make_move_iterator(
                    complete.value.begin() +
                    static_cast<std::ptrdiff_t>(first)),
                std::make_move_iterator(
                    complete.value.begin() +
                    static_cast<std::ptrdiff_t>(last)));
            result.value.nextOffset = last;
            result.value.hasMore = last < complete.value.size();
          }
          cb(std::move(result));
        });
    return;
  }

  launchWorker([this, collection, offset, limit, cb = std::move(cb)] {
    Result<TrackPage> result;
    result.value.offset = offset;
    if (currentState != State::Ready) {
      result.error = "Spotify is still connecting";
      cb(std::move(result));
      return;
    }

    try {
      std::unordered_set<std::string> seen;
      size_t sourceCount = 0;
      if (collection.kind == Collection::Kind::LIKED) {
        const Json variables = {
            {"uri", "spotify:collection:tracks"},
            {"offset", offset},
            {"limit", limit}};
        const auto json = Json::parse(
            partnerRequest("fetchLibraryTracks", variables.dump()));
        sourceCount = appendPartnerTrackPage(
            jsonPath(json, {"data", "me", "library", "tracks"}),
            result.value.tracks, seen);
      } else if (collection.kind == Collection::Kind::PLAYLIST) {
        const Json variables = {
            {"uri", "spotify:playlist:" + collection.id},
            {"offset", offset},
            {"limit", limit},
            {"enableWatchFeedEntrypoint", false}};
        const auto json =
            Json::parse(partnerRequest("fetchPlaylist", variables.dump()));
        sourceCount = appendPartnerTrackPage(
            jsonPath(json, {"data", "playlistV2", "content"}),
            result.value.tracks, seen);
      } else {
        const std::string showUri =
            collection.uri.empty() ? "spotify:show:" + collection.id
                                   : collection.uri;
        const Json variables = {
            {"uri", showUri},
            {"offset", offset},
            {"limit", limit},
            {"includeEpisodeContentRatingsV2", false}};
        const auto json = Json::parse(
            partnerRequest("queryPodcastEpisodes", variables.dump()));
        sourceCount = appendPartnerEpisodePage(
            jsonPath(json, {"data", "podcastUnionV2", "episodesV2"}),
            result.value.tracks, seen, collection.name,
            collection.subtitle, collection.imageUrl);
      }
      result.value.nextOffset = offset + sourceCount;
      result.value.hasMore = sourceCount == limit;
      nxlog::pushf("Collection page \"%s\": source %u-%u, %u tracks%s",
                   collection.name.c_str(), static_cast<unsigned>(offset + 1),
                   static_cast<unsigned>(result.value.nextOffset),
                   static_cast<unsigned>(result.value.tracks.size()),
                   result.value.hasMore ? ", more available" : "");
    } catch (const std::exception& error) {
      result.error = error.what();
      nxlog::pushf("Collection page fetch failed: %s", error.what());
    }
    cb(std::move(result));
  });
}

void SpotifyEngine::fetchPlaylistTracks(
    const Collection& collection,
    std::function<void(Result<std::vector<Track>>)> cb) {
  std::vector<Track> cachedTracks;
  if (cachedPlaylistTracks(collection, cachedTracks)) {
    nxlog::pushf("Collection cache hit: %s", collection.name.c_str());
    launchWorker([cb = std::move(cb),
                  cachedTracks = std::move(cachedTracks)]() mutable {
      Result<std::vector<Track>> cached;
      cached.value = std::move(cachedTracks);
      cb(std::move(cached));
    });
    return;
  }

  launchWorker([this, collection, cb = std::move(cb)] {
    Result<std::vector<Track>> result;
    if (currentState != State::Ready) {
      result.error = "Spotify is still connecting";
      cb(std::move(result));
      return;
    }
    try {
      if (collection.kind == Collection::Kind::ALBUM) {
        const auto album = Json::parse(partnerRequest(
            "getAlbum", Json{{"uri", "spotify:album:" + collection.id}}.dump()));
        std::unordered_set<std::string> seen;
        collectPartnerTracks(album, result.value, seen);
      } else if (collection.kind == Collection::Kind::ARTIST) {
        const Json variables = {{"uri", "spotify:artist:" + collection.id},
                                {"locale", "en-US"}};
        const auto artist = Json::parse(
            partnerRequest("queryArtistOverview", variables.dump()));
        std::unordered_set<std::string> seen;
        collectPartnerTracks(artist, result.value, seen);
        if (result.value.size() > 25) result.value.resize(25);
      } else if (collection.kind == Collection::Kind::SHOW) {
        constexpr int PageSize = 50;
        constexpr int MaximumEpisodes = 500;
        std::unordered_set<std::string> seen;
        std::string partnerError;
        try {
          const std::string showUri =
              collection.uri.empty()
                  ? "spotify:show:" + collection.id
                  : collection.uri;
          for (int offset = 0; offset < MaximumEpisodes;
               offset += PageSize) {
            const Json variables = {
                {"uri", showUri},
                {"offset", offset},
                {"limit", PageSize},
                {"includeEpisodeContentRatingsV2", false}};
            const auto response = Json::parse(
                partnerRequest("queryPodcastEpisodes", variables.dump()));
            const Json* page = jsonPath(
                response, {"data", "podcastUnionV2", "episodesV2"});
            const size_t before = result.value.size();
            const size_t sourceCount = appendPartnerEpisodePage(
                page, result.value, seen, collection.name,
                collection.subtitle, collection.imageUrl);
            if (sourceCount < PageSize || result.value.size() == before)
              break;
          }
          nxlog::pushf("Podcast partner episodes: %d",
                       static_cast<int>(result.value.size()));
        } catch (const std::exception& error) {
          partnerError = error.what();
          result.value.clear();
          seen.clear();
          nxlog::pushf("Podcast partner episode request failed: %s",
                       partnerError.c_str());
        }

        // Retain the public endpoint only as compatibility fallback. Normal
        // episode browsing no longer shares its global rate-limit cooldown.
        if (result.value.empty()) {
          try {
            for (int offset = 0; offset < MaximumEpisodes;
                 offset += PageSize) {
              const auto page = Json::parse(apiRequest(
                  "GET", "/v1/shows/" + urlEncode(collection.id) +
                             "/episodes?limit=" +
                             std::to_string(PageSize) + "&offset=" +
                             std::to_string(offset)));
              const auto items = page.find("items");
              if (items == page.end() || !items->is_array()) break;
              const size_t before = result.value.size();
              for (const auto& item : *items) {
                auto episode = parseEpisode(item, collection.name,
                                            collection.subtitle,
                                            collection.imageUrl);
                if (!episode.uri.empty() && seen.insert(episode.uri).second)
                  result.value.push_back(std::move(episode));
              }
              if (items->size() < PageSize ||
                  result.value.size() == before)
                break;
            }
          } catch (const std::exception& apiError) {
            throw std::runtime_error(
                "Podcast episodes unavailable" +
                (partnerError.empty() ? std::string()
                                      : ": partner " + partnerError) +
                "; fallback " + apiError.what());
          }
        }
      } else {
        constexpr int PageSize = 50;
#ifdef SWITIFY_BACKGROUND_SERVICE
        constexpr int MaximumCollectionTracks = 1000;
#else
        // The foreground NRO is not a fixed-heap sysmodule. Fetch a complete
        // practical library and retain it in the session cache; the UI itself
        // remains cheap because TrackListActivity materializes 100 rows at a
        // time. This preserves Play All and multi-thousand-track Liked Songs.
        constexpr int MaximumCollectionTracks = 30000;
#endif
        std::unordered_set<std::string> seen;
        if (collection.kind == Collection::Kind::LIKED) {
          for (int offset = 0; offset < MaximumCollectionTracks;
               offset += PageSize) {
            const Json variables = {{"uri", "spotify:collection:tracks"},
                                    {"offset", offset},
                                    {"limit", PageSize}};
            const auto json = Json::parse(
                partnerRequest("fetchLibraryTracks", variables.dump()));
            const Json* tracks =
                jsonPath(json, {"data", "me", "library", "tracks"});
            const size_t before = result.value.size();
            const size_t sourceCount =
                appendPartnerTrackPage(tracks, result.value, seen);
            // Pathfinder's totalCount can describe the returned window rather
            // than the complete library. Stop only when the source page is
            // short/empty or an ignored offset yields no new tracks.
            if (sourceCount < PageSize || result.value.size() == before)
              break;
          }
        } else {
          for (int offset = 0; offset < MaximumCollectionTracks;
               offset += PageSize) {
            const Json variables = {
                {"uri", "spotify:playlist:" + collection.id},
                {"offset", offset},
                {"limit", PageSize},
                {"enableWatchFeedEntrypoint", false}};
            const auto json = Json::parse(
                partnerRequest("fetchPlaylist", variables.dump()));
            const Json* content =
                jsonPath(json, {"data", "playlistV2", "content"});
            const size_t before = result.value.size();
            const size_t sourceCount =
                appendPartnerTrackPage(content, result.value, seen);
            if (sourceCount < PageSize || result.value.size() == before)
              break;
          }
        }
      }
      nxlog::pushf("Collection \"%s\": %d tracks", collection.name.c_str(),
                   static_cast<int>(result.value.size()));
    } catch (const std::exception& error) {
      result.error = error.what();
      nxlog::pushf("Collection fetch failed: %s", error.what());
    }
    if (result.ok() && result.value.size() <= MAX_CACHED_COLLECTION_TRACKS) {
      const std::string key = collectionCacheKey(collection);
      std::lock_guard<std::mutex> lock(dataMutex);
      const auto existing = collectionTracksCache.find(key);
      if (existing != collectionTracksCache.end()) {
        cachedCollectionTrackCount -= existing->second.size();
        collectionTracksCache.erase(existing);
      }
      const auto oldOrder = std::find(collectionTrackCacheOrder.begin(),
                                      collectionTrackCacheOrder.end(), key);
      if (oldOrder != collectionTrackCacheOrder.end())
        collectionTrackCacheOrder.erase(oldOrder);
      while (!collectionTrackCacheOrder.empty() &&
             (collectionTracksCache.size() >= MAX_CACHED_COLLECTIONS ||
              cachedCollectionTrackCount + result.value.size() >
                  MAX_CACHED_COLLECTION_TRACKS)) {
        const std::string oldest = collectionTrackCacheOrder.front();
        collectionTrackCacheOrder.pop_front();
        const auto stale = collectionTracksCache.find(oldest);
        if (stale != collectionTracksCache.end()) {
          cachedCollectionTrackCount -= stale->second.size();
          collectionTracksCache.erase(stale);
        }
      }
      cachedCollectionTrackCount += result.value.size();
      collectionTracksCache.emplace(key, result.value);
      collectionTrackCacheOrder.push_back(key);
    }
    cb(std::move(result));
  });
}

void SpotifyEngine::search(const std::string& query,
                           std::function<void(Result<SearchResults>)> cb) {
  launchWorker([this, query, cb = std::move(cb)] {
    Result<SearchResults> result;
    if (currentState != State::Ready) {
      result.error = "Spotify is still connecting";
      cb(std::move(result));
      return;
    }
    if (query.empty()) {
      result.error = "Enter something to search for";
      cb(std::move(result));
      return;
    }
    try {
      // api.spotify.com/v1/search is aggressively rate limited for the
      // web-player user token. searchDesktop is the authenticated query that
      // is already proven on Switch hardware and returns every filter group
      // in one response without consuming the public Web API rate bucket.
      const Json variables = {
          {"searchTerm", query},
          {"offset", 0},
          {"limit", 10},
          {"numberOfTopResults", 5},
          {"includeAudiobooks", true},
          {"includePreReleases", true},
          {"includeLocalConcertsField", false},
          {"includeArtistHasConcertsField", false}};
      const auto json =
          Json::parse(partnerRequest("searchDesktop", variables.dump()));
      const Json* search = jsonPath(json, {"data", "searchV2"});
      if (!search || !search->is_object())
        throw std::runtime_error("Spotify search returned no results payload");

      appendPartnerItems(jsonPath(*search, {"tracksV2"}),
                         result.value.tracks, parsePartnerTrack);
      appendPartnerItems(jsonPath(*search, {"episodes"}),
                         result.value.episodes, parsePartnerEpisode);
      const Json* albums = jsonPath(*search, {"albumsV2"});
      if (!albums) albums = jsonPath(*search, {"albums"});
      appendPartnerItems(albums, result.value.albums, [](const Json& item) {
        return parsePartnerCollection(
            item, SpotifyEngine::Collection::Kind::ALBUM);
      });
      appendPartnerItems(
          jsonPath(*search, {"artists"}), result.value.artists,
          [](const Json& item) {
            return parsePartnerCollection(
                item, SpotifyEngine::Collection::Kind::ARTIST);
          });
      appendPartnerItems(
          jsonPath(*search, {"playlists"}), result.value.playlists,
          [](const Json& item) {
            return parsePartnerCollection(
                item, SpotifyEngine::Collection::Kind::PLAYLIST);
          });
      appendPartnerItems(
          jsonPath(*search, {"podcasts"}), result.value.podcasts,
          [](const Json& item) {
            return parsePartnerCollection(
                item, SpotifyEngine::Collection::Kind::SHOW);
          });
#ifdef SWITIFY_BACKGROUND_SERVICE
      // The in-game overlay intentionally retains only the two quick-search
      // surfaces it exposes. The full NRO still keeps every result group.
      result.value.episodes.clear();
      result.value.albums.clear();
      result.value.artists.clear();
      result.value.podcasts.clear();
#endif
      nxlog::pushf(
          "Search \"%s\": %d songs, %d artists, %d albums, %d playlists, "
          "%d shows, %d episodes",
          query.c_str(), static_cast<int>(result.value.tracks.size()),
          static_cast<int>(result.value.artists.size()),
          static_cast<int>(result.value.albums.size()),
          static_cast<int>(result.value.playlists.size()),
          static_cast<int>(result.value.podcasts.size()),
          static_cast<int>(result.value.episodes.size()));
    } catch (const std::exception& error) {
      result.error = error.what();
      nxlog::pushf("Search failed: %s", error.what());
    }
    cb(std::move(result));
  });
}

void SpotifyEngine::setTrackSaved(const std::string& uri, bool saved,
                                  std::function<void(Result<bool>)> cb) {
  bool previousSaved = false;
  bool previousKnown = false;
  std::string immediateError;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    if (currentState != State::Ready || uri.empty()) {
      immediateError = "No playable track selected";
    } else if (!likedMutationPending.insert(uri).second) {
      immediateError = "Library update already pending";
    } else {
      ++likedStateGeneration[uri];
      const auto cached = likedStateCache.find(uri);
      if (cached != likedStateCache.end()) {
        previousSaved = cached->second;
        previousKnown = true;
      } else if (nowPlayingData.uri == uri) {
        previousSaved = nowPlayingData.saved;
        previousKnown = nowPlayingData.savedKnown;
      }
      likedStateCache[uri] = saved;
      if (nowPlayingData.uri == uri) {
        nowPlayingData.saved = saved;
        nowPlayingData.savedKnown = true;
        nowPlayingData.savePending = true;
      }
    }
  }
  if (!immediateError.empty()) {
    launchWorker([cb = std::move(cb), error = std::move(immediateError)]() mutable {
      Result<bool> result;
      result.error = std::move(error);
      cb(std::move(result));
    });
    return;
  }

  notifyUpdate();
  launchWorker([this, uri, saved, previousSaved, previousKnown,
                cb = std::move(cb)] {
    Result<bool> result;
    try {
      bool committed = false;
      for (int attempt = 0; attempt < 2 && !committed; ++attempt) {
        if (!waitForApiRateLimitWindow())
          throw std::runtime_error("Library update cancelled");
        try {
          const std::string body = Json{{"uris", Json::array({uri})}}.dump();
          apiRequest(saved ? "PUT" : "DELETE",
                     "/v1/me/library?uris=" + urlEncode(uri), body);
          committed = true;
        } catch (...) {
          const auto context = ctx;
          const long long now = context
                                    ? context->timeProvider->getSyncedTimestamp()
                                    : 0;
          if (attempt == 0 && now > 0 && now < apiRetryAfter.load()) continue;
          throw;
        }
      }
      if (!committed)
        throw std::runtime_error("Spotify did not accept the library update");

      bool verified = false;
      for (int attempt = 0; attempt < 3 && !verified; ++attempt) {
        if (attempt > 0)
          std::this_thread::sleep_for(
              std::chrono::milliseconds(400 * attempt));
        if (!waitForApiRateLimitWindow())
          throw std::runtime_error("Library verification cancelled");
        try {
          const Json response = Json::parse(apiRequest(
              "GET", "/v1/me/library/contains?uris=" + urlEncode(uri)));
          if (!response.is_array() || response.empty() ||
              !response.front().is_boolean())
            throw std::runtime_error("Spotify returned an invalid liked state");
          const bool actual = response.front().get<bool>();
          verified = actual == saved;
          if (!verified)
            nxlog::pushf("Library verification not visible yet (attempt %d)",
                         attempt + 1);
        } catch (...) {
          const auto context = ctx;
          const long long now = context
                                    ? context->timeProvider->getSyncedTimestamp()
                                    : 0;
          if (attempt < 2 && now > 0 && now < apiRetryAfter.load()) continue;
          throw;
        }
      }
      if (!verified)
        throw std::runtime_error(
            "Spotify did not persist the library update");
      {
        std::lock_guard<std::mutex> lock(dataMutex);
        likedMutationPending.erase(uri);
        likedStateCache[uri] = saved;
        if (nowPlayingData.uri == uri) {
          nowPlayingData.saved = saved;
          nowPlayingData.savedKnown = true;
          nowPlayingData.savePending = false;
        }
        const std::string likedKey = "spotify:collection:tracks";
        const auto liked = collectionTracksCache.find(likedKey);
        if (liked != collectionTracksCache.end()) {
          cachedCollectionTrackCount -= liked->second.size();
          collectionTracksCache.erase(liked);
          const auto order = std::find(collectionTrackCacheOrder.begin(),
                                       collectionTrackCacheOrder.end(),
                                       likedKey);
          if (order != collectionTrackCacheOrder.end())
            collectionTrackCacheOrder.erase(order);
        }
      }
      result.value = saved;
      notifyUpdate();
    } catch (const std::exception& error) {
      result.error = error.what();
      {
        std::lock_guard<std::mutex> lock(dataMutex);
        likedMutationPending.erase(uri);
        if (previousKnown)
          likedStateCache[uri] = previousSaved;
        else
          likedStateCache.erase(uri);
        if (nowPlayingData.uri == uri) {
          nowPlayingData.saved = previousSaved;
          nowPlayingData.savedKnown = previousKnown;
          nowPlayingData.savePending = false;
        }
      }
      notifyUpdate();
      refreshTrackSavedState(uri);
      nxlog::pushf("Library update failed: %s", error.what());
    }
    cb(std::move(result));
  });
}

void SpotifyEngine::refreshTrackSavedState(const std::string& uri) {
#ifdef SWITIFY_BACKGROUND_SERVICE
  // The overlay does not expose library editing. Avoid spending a Web API
  // request and token refresh on every background track transition merely to
  // populate a field that no background UI consumes.
  (void)uri;
  return;
#endif
  if (uri.empty()) return;
  bool cached = false;
  bool cachedValue = false;
  uint64_t requestGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    const auto found = likedStateCache.find(uri);
    if (found != likedStateCache.end()) {
      cached = true;
      cachedValue = found->second;
      if (nowPlayingData.uri == uri) {
        nowPlayingData.saved = cachedValue;
        nowPlayingData.savedKnown = true;
        nowPlayingData.savePending =
            likedMutationPending.find(uri) != likedMutationPending.end();
      }
    } else {
      if (!likedStatePending.insert(uri).second) return;
      requestGeneration = likedStateGeneration[uri];
      if (nowPlayingData.uri == uri) nowPlayingData.savedKnown = false;
    }
  }
  if (cached) {
    notifyUpdate();
    return;
  }

  launchWorker([this, uri, requestGeneration] {
    try {
      Json response;
      bool received = false;
      for (int attempt = 0; attempt < 2 && !received; ++attempt) {
        if (!waitForApiRateLimitWindow()) {
          std::lock_guard<std::mutex> lock(dataMutex);
          likedStatePending.erase(uri);
          return;
        }
        try {
          response = Json::parse(apiRequest(
              "GET", "/v1/me/library/contains?uris=" + urlEncode(uri)));
          received = true;
        } catch (...) {
          const auto context = ctx;
          const long long now = context
                                    ? context->timeProvider->getSyncedTimestamp()
                                    : 0;
          if (attempt == 0 && now > 0 && now < apiRetryAfter.load()) continue;
          throw;
        }
      }
      if (!received)
        throw std::runtime_error("Spotify did not return the liked state");
      if (!response.is_array() || response.empty() ||
          !response.front().is_boolean())
        throw std::runtime_error("Spotify returned an invalid liked state");
      const bool saved = response.front().get<bool>();
      bool refreshAgain = false;
      {
        std::lock_guard<std::mutex> lock(dataMutex);
        likedStatePending.erase(uri);
        const bool stale = likedStateGeneration[uri] != requestGeneration;
        const bool mutationPending =
            likedMutationPending.find(uri) != likedMutationPending.end();
        if (!stale && !mutationPending) likedStateCache[uri] = saved;
        if (nowPlayingData.uri == uri && !stale && !mutationPending) {
          nowPlayingData.saved = saved;
          nowPlayingData.savedKnown = true;
          nowPlayingData.savePending = false;
        }
        refreshAgain = stale && !mutationPending &&
                       likedStateCache.find(uri) == likedStateCache.end() &&
                       nowPlayingData.uri == uri;
      }
      notifyUpdate();
      if (refreshAgain) refreshTrackSavedState(uri);
    } catch (const std::exception& error) {
      bool refreshAgain = false;
      {
        std::lock_guard<std::mutex> lock(dataMutex);
        likedStatePending.erase(uri);
        refreshAgain = likedStateGeneration[uri] != requestGeneration &&
                       likedMutationPending.find(uri) ==
                           likedMutationPending.end() &&
                       likedStateCache.find(uri) == likedStateCache.end() &&
                       nowPlayingData.uri == uri;
      }
      nxlog::pushf("Liked-state lookup failed: %s", error.what());
      if (refreshAgain) refreshTrackSavedState(uri);
    }
  });
}

void SpotifyEngine::playTracks(std::vector<Track> tracks, size_t startIndex,
                               const std::string& sourceName,
                               uint32_t positionMs) {
  if (currentState != State::Ready || !handler || tracks.empty()) return;
  if (startIndex >= tracks.size()) startIndex = 0;

  // Keep Switify's metadata queue exactly aligned with the URI vector handed
  // to cspot. A skipped empty URI used to shift every later index, which made
  // live Shuffle and queue selection appear intermittent.
  std::vector<Track> playableTracks;
  playableTracks.reserve(tracks.size());
  size_t playableStartIndex = 0;
  bool selectedPlayable = false;
  for (size_t index = 0; index < tracks.size(); ++index) {
    if (tracks[index].uri.empty()) continue;
    if (index == startIndex) {
      playableStartIndex = playableTracks.size();
      selectedPlayable = true;
    }
    playableTracks.push_back(std::move(tracks[index]));
  }
  if (playableTracks.empty()) return;
  tracks = std::move(playableTracks);
  startIndex = selectedPlayable ? playableStartIndex : 0;

#ifdef SWITIFY_BACKGROUND_SERVICE
  size_t playbackWindowStart = 0;
  size_t sourceTrackCount = 0;
  startIndex = boundBackgroundPlaybackWindow(
      tracks, startIndex, &playbackWindowStart, &sourceTrackCount);
  if (sourceTrackCount > tracks.size()) {
    nxlog::pushf("Playback window: tracks %u-%u of %u",
                 static_cast<unsigned>(playbackWindowStart + 1),
                 static_cast<unsigned>(playbackWindowStart + tracks.size()),
                 static_cast<unsigned>(sourceTrackCount));
  }
#endif

  const std::vector<Track> sourceTracks = tracks;
  const size_t sourceIndex = startIndex;
  std::string resolvedSourceName = sourceName;
  if (resolvedSourceName.empty()) {
    resolvedSourceName = tracks[startIndex].album;
    if (resolvedSourceName.empty()) resolvedSourceName = "Playback session";
  }

  bool shuffle = false;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    shuffle = nowPlayingData.shuffle;
  }
  if (shuffle && tracks.size() > 1) {
    Track selected = tracks[startIndex];
    tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(startIndex));
    std::mt19937 generator(std::random_device{}());
    std::shuffle(tracks.begin(), tracks.end(), generator);
    tracks.insert(tracks.begin(), std::move(selected));
    startIndex = 0;
  }

  std::vector<std::string> uris;
  uris.reserve(tracks.size());
  for (const auto& track : tracks) {
    if (!track.uri.empty()) uris.push_back(track.uri);
  }
  if (uris.empty()) return;

  {
    std::lock_guard<std::mutex> lock(dataMutex);
    activeSourceName = std::move(resolvedSourceName);
    activePlaylist = sourceTracks;
    activePlaylistIndex = sourceIndex;
    activePlaylistHasCurrent = true;
    ++activePlaylistRevision;
    activeQueue = tracks;
    activeQueueIndex = startIndex;
    activeQueueHasCurrent = true;
    restoredSessionPendingStart = false;
    const auto& selected = tracks[startIndex];
    nowPlayingData.id = selected.id;
    nowPlayingData.uri = selected.uri;
    nowPlayingData.name = selected.name;
    nowPlayingData.artist = selected.artist;
    nowPlayingData.album = selected.album;
    nowPlayingData.imageUrl = selected.imageUrl;
    nowPlayingData.durationMs = selected.durationMs;
    nowPlayingData.hasTrack = true;
    nowPlayingData.saved = false;
    nowPlayingData.savedKnown = false;
    nowPlayingData.savePending = false;
    positionBaseMs = std::min(positionMs, selected.durationMs);
    positionMeasuredAt = std::chrono::steady_clock::now();
  }
  pausedState = false;
  notifyUpdate();
  refreshTrackSavedState(tracks[startIndex].uri);
  savePlaybackSession();
  nxlog::pushf("Local play: %d tracks from %d at %u ms",
               static_cast<int>(uris.size()), static_cast<int>(startIndex),
               positionMs);

  launchWorker([this, uris = std::move(uris), startIndex, positionMs] {
    try {
      handler->playLocalTrackList(uris, startIndex, positionMs);
    } catch (const std::exception& error) {
      nxlog::pushf("Local play failed: %s", error.what());
    }
  });
}

void SpotifyEngine::playPlaylistTrack(size_t index) {
  std::vector<Track> tracks;
  std::string sourceName;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    tracks = activePlaylist;
    sourceName = activeSourceName;
  }
  if (!tracks.empty()) playTracks(std::move(tracks), index, sourceName);
}

void SpotifyEngine::playQueueTrack(size_t upcomingIndex) {
  if (currentState != State::Ready || !handler) return;

  std::vector<std::string> uris;
  size_t targetIndex = 0;
  std::string selectedUri;
  {
    std::lock_guard<std::mutex> lock(dataMutex);
    const size_t first = activeQueueHasCurrent ? activeQueueIndex + 1 : 0;
    targetIndex = first + upcomingIndex;
    if (targetIndex >= activeQueue.size()) return;

    uris.reserve(activeQueue.size());
    for (const auto& track : activeQueue) {
      if (!track.uri.empty()) uris.push_back(track.uri);
    }
    if (uris.size() != activeQueue.size()) return;

    activeQueueIndex = targetIndex;
    activeQueueHasCurrent = true;
    restoredSessionPendingStart = false;
    const auto& selected = activeQueue[targetIndex];
    selectedUri = selected.uri;
    for (size_t index = 0; index < activePlaylist.size(); ++index) {
      if ((!selected.uri.empty() &&
           activePlaylist[index].uri == selected.uri) ||
          (activePlaylist[index].name == selected.name &&
           activePlaylist[index].artist == selected.artist)) {
        activePlaylistIndex = index;
        activePlaylistHasCurrent = true;
        break;
      }
    }
    nowPlayingData.id = selected.id;
    nowPlayingData.uri = selected.uri;
    nowPlayingData.name = selected.name;
    nowPlayingData.artist = selected.artist;
    nowPlayingData.album = selected.album;
    nowPlayingData.imageUrl = selected.imageUrl;
    nowPlayingData.durationMs = selected.durationMs;
    nowPlayingData.hasTrack = true;
    nowPlayingData.saved = false;
    nowPlayingData.savedKnown = false;
    nowPlayingData.savePending = false;
    positionBaseMs = 0;
    positionMeasuredAt = std::chrono::steady_clock::now();
  }
  pausedState = false;
  notifyUpdate();
  refreshTrackSavedState(selectedUri);

  launchWorker([this, uris = std::move(uris), targetIndex] {
    try {
      handler->playLocalTrackList(uris, targetIndex);
    } catch (const std::exception& error) {
      nxlog::pushf("Queue jump failed: %s", error.what());
    }
  });
}

// ---- Session lifecycle --------------------------------------------------

void SpotifyEngine::setInteractiveAuthentication(bool enabled) {
  if (!running.load()) interactiveAuthentication.store(enabled);
}

void SpotifyEngine::start() {
  if (running) return;
  running = true;
  engineThread = std::thread([this] { run(); });
}

void SpotifyEngine::retryConnection() {
  if (!running || currentState != State::Error) return;
  {
    std::lock_guard<std::mutex> lock(retryMutex);
    if (retryRequested) return;
    retryRequested = true;
  }
  nxlog::push("Manual Spotify connection retry requested");
  setState(State::Connecting, "Retrying Spotify connection...");
  retryCondition.notify_one();
}

void SpotifyEngine::run() {
  mkdir("/config", 0755);
  mkdir(CONFIG_DIR, 0755);

  loggedInSemaphore = std::make_shared<bell::WrappedSemaphore>(1);
  loginBlob = std::make_shared<cspot::LoginBlob>(DEVICE_NAME);

  std::string cached = readFile(CREDENTIALS_FILE);
  if (cached.empty() && readFile(NO_MIGRATION_FILE).empty())
    cached = readFile(CREDENTIALS_FILE_POC);
  if (!cached.empty()) {
    try {
      loginBlob->loadJson(cached);
      loggedInSemaphore->give();
    } catch (...) {
      cached.clear();
    }
  }

  if (cached.empty()) {
    if (!interactiveAuthentication.load()) {
      setState(State::NoConfig,
               "Open the Switify app and sign in before using the overlay");
      return;
    }

    zeroconfServer = std::make_unique<ZeroconfHttpServer>(
        ZEROCONF_PORT, [this]() { return loginBlob->buildZeroconfInfo(); },
        [this](std::map<std::string, std::string> query) {
          try {
            loginBlob->loadZeroconfQuery(query);
            loggedInSemaphore->give();
          } catch (const std::exception& error) {
            setState(State::Error,
                     std::string("Credential decode failed: ") + error.what());
          }
        },
        [] {});
    if (!zeroconfServer->start()) {
      setState(State::Error, "Could not start the Spotify login service");
      return;
    }
    mdnsService = bell::MDNSService::registerService(
        loginBlob->getDeviceName(), "_spotify-connect", "_tcp", "",
        ZEROCONF_PORT,
        {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});
    setState(State::WaitingForPhone,
             "Open Spotify on a phone on this Wi-Fi and select Switify once");
  }

  loggedInSemaphore->wait();
  if (!running) return;

  while (running) {
  setState(State::Connecting, "Connecting securely to Spotify...");
  try {
    ctx = cspot::Context::createFromBlob(loginBlob);
    ctx->config.clientId = WEB_CLIENT_ID;
    {
      std::lock_guard<std::mutex> lock(dataMutex);
      ctx->config.audioFormat =
          highQuality ? AudioFormat_OGG_VORBIS_320
                      : AudioFormat_OGG_VORBIS_160;
      ctx->config.volume = static_cast<int>(
          std::clamp(nowPlayingData.volume, 0.0f, 1.0f) * 65535.0f);
      nxlog::pushf("Spotify audio quality: %s (%d kbps)",
                   highQuality ? "High" : "Normal",
                   highQuality ? 320 : 160);
    }
    ctx->session->connectWithRandomAp();
    ctx->config.authData = ctx->session->authenticate(loginBlob);
    if (ctx->config.authData.empty()) {
      throw std::runtime_error("Spotify authentication failed");
    }
    ctx->config.accessTokenProvider =
        [this]() { return userToken("streaming"); };
    {
      std::ofstream file(CREDENTIALS_FILE);
      file << ctx->getCredentialsJson();
    }
    std::remove(NO_MIGRATION_FILE);

    // Switify owns its queue locally. Do not start cspot's legacy SPIRC
    // receiver subscription; current Spotify APs repeatedly close that
    // deprecated channel and invalidate concurrent keymaster requests.
    handler = std::make_shared<cspot::SpircHandler>(ctx, false);
    {
      std::lock_guard<std::mutex> lock(dataMutex);
      handler->setLocalRepeatMode(localRepeatMode(nowPlayingData.repeatMode));
    }
    ctx->session->startTask();

#ifdef __SWITCH__
    auto sink = std::make_unique<NXAudioSink>();
    if (!sink->ok())
      throw std::runtime_error("Nintendo Switch audio output is unavailable");
    sink->setParams(44100, 2, 16);
    sink->setVolume(nowPlayingData.volume);
    sink->setEqualizer(equalizerEnabled, equalizerBassDb, equalizerMidDb,
                       equalizerTrebleDb);
    switchAudioSink = sink.get();
    std::unique_ptr<AudioSink> audioSink = std::move(sink);
#else
    std::unique_ptr<AudioSink> audioSink = std::make_unique<NullAudioSink>();
#endif

    player = std::make_shared<NXPlayer>(std::move(audioSink), handler);
    player->setUiEventSink([this](cspot::SpircHandler::Event* event) {
      switch (event->eventType) {
        case cspot::SpircHandler::EventType::TRACK_INFO: {
          const auto info = std::get<cspot::TrackInfo>(event->data);
          std::string resolvedUri;
          {
            std::lock_guard<std::mutex> lock(dataMutex);
            nowPlayingData.id = info.trackId;
            nowPlayingData.uri.clear();
            nowPlayingData.name = info.name;
            nowPlayingData.artist = info.artist;
            nowPlayingData.album = info.album;
            nowPlayingData.imageUrl = info.imageUrl;
            nowPlayingData.durationMs = info.duration;
            nowPlayingData.hasTrack = true;
            nowPlayingData.saved = false;
            nowPlayingData.savedKnown = false;
            nowPlayingData.savePending = false;
            restoredSessionPendingStart = false;
            activeQueueHasCurrent = false;
            for (size_t index = 0; index < activeQueue.size(); ++index) {
              const auto& track = activeQueue[index];
              if (track.name == info.name && track.artist == info.artist) {
                activeQueueIndex = index;
                activeQueueHasCurrent = true;
                nowPlayingData.id = track.id;
                nowPlayingData.uri = track.uri;
                resolvedUri = track.uri;
                if (!track.imageUrl.empty())
                  nowPlayingData.imageUrl = track.imageUrl;
                break;
              }
            }
            activePlaylistHasCurrent = false;
            for (size_t index = 0; index < activePlaylist.size(); ++index) {
              const auto& track = activePlaylist[index];
              if (track.name == info.name && track.artist == info.artist) {
                activePlaylistIndex = index;
                activePlaylistHasCurrent = true;
                break;
              }
            }
            positionBaseMs = 0;
            positionMeasuredAt = std::chrono::steady_clock::now();
          }
          refreshTrackSavedState(resolvedUri);
          savePlaybackSession();
          break;
        }
        case cspot::SpircHandler::EventType::PLAY_PAUSE: {
          const bool paused = std::get<bool>(event->data);
          {
            std::lock_guard<std::mutex> lock(dataMutex);
            positionBaseMs = computedPositionLocked();
            positionMeasuredAt = std::chrono::steady_clock::now();
          }
          pausedState = paused;
          savePlaybackSession();
          break;
        }
        case cspot::SpircHandler::EventType::PLAYBACK_START: {
          const int position = std::get<int>(event->data);
          {
            std::lock_guard<std::mutex> lock(dataMutex);
            positionBaseMs = std::max(0, position);
            positionMeasuredAt = std::chrono::steady_clock::now();
          }
          pausedState = false;
          savePlaybackSession();
          break;
        }
        case cspot::SpircHandler::EventType::SEEK: {
          const int position = std::get<int>(event->data);
          {
            std::lock_guard<std::mutex> lock(dataMutex);
            positionBaseMs = std::max(0, position);
            positionMeasuredAt = std::chrono::steady_clock::now();
          }
          savePlaybackSession();
          break;
        }
        case cspot::SpircHandler::EventType::VOLUME: {
          const float volume =
              std::clamp(std::get<int>(event->data) / 65535.0f, 0.0f, 1.0f);
          {
            std::lock_guard<std::mutex> lock(dataMutex);
            nowPlayingData.volume = volume;
            advertisedVolume = volume;
          }
#ifdef __SWITCH__
          if (switchAudioSink) switchAudioSink->setVolume(volume);
#endif
          swsettings::setOutputVolume(volume);
          break;
        }
        case cspot::SpircHandler::EventType::DEPLETED: {
          {
            std::lock_guard<std::mutex> lock(dataMutex);
            positionBaseMs = nowPlayingData.durationMs;
            positionMeasuredAt = std::chrono::steady_clock::now();
          }
          pausedState = true;
          savePlaybackSession();
          break;
        }
        default:
          break;
      }
      notifyUpdate();
    });

    setState(State::Ready, "Connected");
    restorePlaybackSession();
    while (running) {
      try {
        ctx->session->handlePacket();
      } catch (const std::exception& error) {
        throw std::runtime_error(std::string("Spotify session error: ") +
                                 error.what());
      }
    }
  } catch (const std::exception& error) {
    if (running)
      setState(State::Error,
               std::string("Spotify engine error: ") + error.what());
  }

  if (!running) break;
  nxlog::push("Spotify engine waiting for an on-screen retry");
  {
    std::unique_lock<std::mutex> lock(retryMutex);
    retryCondition.wait(lock,
                        [this] { return !running || retryRequested; });
    retryRequested = false;
  }
  if (!running) break;

  // A retry starts with fresh connection/session objects. This is normally
  // the pre-authentication path; the defensive teardown also supports a
  // terminal error after a session had already reached Ready.
  try {
    if (handler) handler->disconnect();
  } catch (const std::exception& error) {
    nxlog::pushf("Spotify retry disconnect warning: %s", error.what());
  }
  if (player) player->disconnect();
  switchAudioSink = nullptr;
  player.reset();
  handler.reset();
  ctx.reset();
  {
    std::lock_guard<std::mutex> lock(tokenMutex);
    userTokenCache.clear();
    preferredTokenClient = 0;
    cachedClientToken.clear();
    clientTokenExpiresAt = 0;
    cachedLogin5Token.clear();
    login5TokenExpiresAt = 0;
    login5RetryAfter = 0;
  }
  nxlog::push("Spotify connection retry starting");
  }
}

void SpotifyEngine::shutdown() {
  if (!running) return;
  savePlaybackSession();
  running = false;
  if (loggedInSemaphore) loggedInSemaphore->give();
  retryCondition.notify_all();
  if (zeroconfServer) zeroconfServer->stop();
  if (mdnsService) mdnsService->unregisterService();
  if (handler) handler->disconnect();
  if (player) player->disconnect();
  if (engineThread.joinable()) engineThread.join();

  std::vector<DataWorker> workers;
  {
    std::lock_guard<std::mutex> lock(workerMutex);
    workers.swap(dataWorkers);
  }
  for (auto& worker : workers) {
    if (worker.thread.joinable()) worker.thread.join();
  }

  switchAudioSink = nullptr;
  player.reset();
  handler.reset();
  ctx.reset();
  loginBlob.reset();
  zeroconfServer.reset();
  mdnsService.reset();
  loggedInSemaphore.reset();
  userTokenCache.clear();
  preferredTokenClient = 0;
  cachedClientToken.clear();
  clientTokenExpiresAt = 0;
  cachedLogin5Token.clear();
  login5TokenExpiresAt = 0;
  login5RetryAfter = 0;
  currentState = State::NoConfig;
}
