#include "MercurySession.h"

#include <string.h>     // for memcpy
#include <memory>       // for shared_ptr
#include <mutex>        // for scoped_lock
#include <stdexcept>    // for runtime_error
#include <type_traits>  // for remove_extent_t, __underlying_type_impl<>:...
#include <utility>      // for pair
#ifndef _WIN32
#include <arpa/inet.h>  // for htons, ntohs, htonl, ntohl
#endif
#include "BellLogger.h"         // for AbstractLogger
#include "BellTask.h"           // for Task
#include "BellUtils.h"          // for BELL_SLEEP_MS
#include "Logger.h"             // for CSPOT_LOG
#include "NanoPBHelper.h"       // for pbPutString, pbDecode, pbEncode
#include "PlainConnection.h"    // for PlainConnection
#include "ShannonConnection.h"  // for ShannonConnection
#include "TimeProvider.h"       // for TimeProvider
#include "Utils.h"              // for extract, pack, hton64

using namespace cspot;

MercurySession::MercurySession(std::shared_ptr<TimeProvider> timeProvider)
    : bell::Task("mercury_dispatcher", 4 * 1024, 3, 1) {
  this->timeProvider = timeProvider;
}

MercurySession::~MercurySession() {
  std::scoped_lock lock(this->isRunningMutex);
}

void MercurySession::runTask() {
  isRunning = true;
  std::scoped_lock lock(this->isRunningMutex);

  this->executeEstabilishedCallback = true;
  while (isRunning) {
    cspot::Packet packet = {};
    try {
      auto activeConnection = currentShannonConnection();
      if (!activeConnection)
        throw std::runtime_error("Spotify encrypted connection is unavailable");
      packet = activeConnection->recvPacket();
      CSPOT_LOG(info, "Received packet, command: %d", packet.command);
      // Any valid encrypted packet proves that the AP is alive. Restricting
      // this timestamp to PING alone can force a reconnect during otherwise
      // active metadata, keymaster, or audio-key traffic.
      this->lastPingTimestamp = timeProvider->getSyncedTimestamp();

      if (static_cast<RequestType>(packet.command) == RequestType::PING) {
        timeProvider->syncWithPingPacket(packet.data);

        this->lastPingTimestamp = timeProvider->getSyncedTimestamp();
        this->sendPacketSafe(0x49, packet.data);
      } else {
        this->packetQueue.push(packet);
      }
    } catch (const std::runtime_error& e) {
      CSPOT_LOG(error, "Error while receiving packet: %s", e.what());
      failAllPending();

      if (!isRunning)
        return;

      reconnect();
      continue;
    }
  }
}

void MercurySession::reconnect() {
  isReconnecting = true;

  while (isRunning) {
    try {
      // sendPacketSafe() blocks new writes while this replacement is in
      // flight. Keep the replacement atomic from every request producer.
      {
        std::lock_guard<std::mutex> connectionLock(connectionMutex);
        this->conn = nullptr;
        this->shanConn = nullptr;

        this->connectWithRandomAp();
        if (this->authenticate(this->authBlob).empty())
          throw std::runtime_error("Spotify reauthentication failed");
      }

      CSPOT_LOG(info, "Reconnection successful");
      BELL_SLEEP_MS(100);
      lastPingTimestamp = timeProvider->getSyncedTimestamp();
      isReconnecting = false;
      this->executeEstabilishedCallback = true;
      return;
    } catch (const std::exception& error) {
      CSPOT_LOG(error, "Cannot reconnect: %s; retrying in 1s", error.what());
      // Keep shutdown latency bounded instead of sleeping for five seconds or
      // recursing once per failed reconnect.
      for (int elapsed = 0; elapsed < 1000 && isRunning; elapsed += 100)
        BELL_SLEEP_MS(100);
    }
  }
  isReconnecting = false;
}

void MercurySession::setConnectedHandler(
    ConnectionEstabilishedCallback callback) {
  this->connectionReadyCallback = callback;
}

bool MercurySession::triggerTimeout() {
  if (!isRunning)
    return true;
  auto currentTimestamp = timeProvider->getSyncedTimestamp();

  if (currentTimestamp - this->lastPingTimestamp > PING_TIMEOUT_MS) {
    CSPOT_LOG(debug, "Reconnection required, no ping received");
    return true;
  }

  return false;
}

void MercurySession::unregister(uint64_t sequenceId) {
  std::lock_guard<std::mutex> lock(requestStateMutex);
  auto callback = this->callbacks.find(sequenceId);

  if (callback != this->callbacks.end()) {
    this->callbacks.erase(callback);
  }
}

void MercurySession::unregisterAudioKey(uint32_t sequenceId) {
  std::lock_guard<std::mutex> lock(requestStateMutex);
  auto callback = this->audioKeyCallbacks.find(sequenceId);

  if (callback != this->audioKeyCallbacks.end()) {
    this->audioKeyCallbacks.erase(callback);
  }
}

void MercurySession::disconnect() {
  CSPOT_LOG(info, "Disconnecting mercury session");
  this->isRunning = false;

  // Order matters (crash fix): shutdown() first — it unblocks the reader
  // task's recv() while the fd stays valid. Then wait for the reader to
  // exit (it holds isRunningMutex for its whole lifetime). Only then close
  // the fd. Closing while the reader is inside recv() crashes on libnx.
  std::shared_ptr<PlainConnection> activeConnection;
  {
    std::lock_guard<std::mutex> lock(connectionMutex);
    activeConnection = conn;
    if (activeConnection) activeConnection->shutdownSocket();
  }
  {
    std::scoped_lock lock(this->isRunningMutex);
  }
  {
    std::lock_guard<std::mutex> lock(connectionMutex);
    if (activeConnection) activeConnection->close();
  }
}

std::string MercurySession::getCountryCode() {
  return this->countryCode;
}

void MercurySession::handlePacket() {
  Packet packet = {};

  this->packetQueue.wtpop(packet, 200);

  if (executeEstabilishedCallback && this->connectionReadyCallback != nullptr) {
    executeEstabilishedCallback = false;
    this->connectionReadyCallback();
  }

  switch (static_cast<RequestType>(packet.command)) {
    case RequestType::COUNTRY_CODE_RESPONSE: {
      this->countryCode = std::string();
      this->countryCode.resize(2);
      memcpy(this->countryCode.data(), packet.data.data(), 2);
      CSPOT_LOG(debug, "Received country code %s", this->countryCode.c_str());
      break;
    }
    case RequestType::AUDIO_KEY_FAILURE_RESPONSE:
    case RequestType::AUDIO_KEY_SUCCESS_RESPONSE: {
      // this->lastRequestTimestamp = -1;

      // First four bytes mark the sequence id
      auto seqId = ntohl(extract<uint32_t>(packet.data, 0));

      AudioKeyCallback callback;
      {
        std::lock_guard<std::mutex> lock(requestStateMutex);
        const auto found = audioKeyCallbacks.find(seqId);
        if (found != audioKeyCallbacks.end()) callback = found->second;
      }
      if (callback) {
        const bool success = static_cast<RequestType>(packet.command) ==
                             RequestType::AUDIO_KEY_SUCCESS_RESPONSE;
        callback(success, packet.data);
      }

      break;
    }
    case RequestType::SEND:
    case RequestType::SUB:
    case RequestType::UNSUB: {
      CSPOT_LOG(debug, "Received mercury packet");

      auto response = this->decodeResponse(packet.data);
      ResponseCallback callback;
      {
        std::lock_guard<std::mutex> lock(requestStateMutex);
        const auto found = callbacks.find(response.sequenceId);
        if (found != callbacks.end()) {
          callback = found->second;
          callbacks.erase(found);
        }
      }
      CSPOT_LOG(debug,
                "Mercury response seq=%llu status=%d parts=%u callback=%s",
                static_cast<unsigned long long>(response.sequenceId),
                response.mercuryHeader.has_status_code
                    ? response.mercuryHeader.status_code
                    : 0,
                static_cast<unsigned>(response.parts.size()),
                callback ? "yes" : "no");
      // Callbacks may immediately send another Mercury request (SPIRC does
      // this after subscribing), so never invoke one while holding the map.
      if (callback) callback(response);
      break;
    }
    case RequestType::SUBRES: {
      auto response = decodeResponse(packet.data);

      auto uri = std::string(response.mercuryHeader.uri);
      ResponseCallback callback;
      {
        std::lock_guard<std::mutex> lock(requestStateMutex);
        const auto found = subscriptions.find(uri);
        if (found != subscriptions.end()) callback = found->second;
      }
      if (callback) callback(response);
      break;
    }
    default:
      break;
  }
}

void MercurySession::failAllPending() {
  Response response = {};
  response.fail = true;

  std::vector<ResponseCallback> pending;
  {
    std::lock_guard<std::mutex> lock(requestStateMutex);
    pending.reserve(callbacks.size() + subscriptions.size());
    for (auto& item : callbacks) pending.push_back(item.second);
    for (auto& item : subscriptions) pending.push_back(item.second);
    callbacks.clear();
    subscriptions.clear();
  }

  if (!pending.empty())
    CSPOT_LOG(info, "Failing %u pending Mercury callback(s)",
              static_cast<unsigned>(pending.size()));

  // A failure callback can issue another request. Invoke only after releasing
  // requestStateMutex to keep this path re-entrant and deterministic.
  for (auto& callback : pending) {
    if (callback) callback(response);
  }
}

MercurySession::Response MercurySession::decodeResponse(
    const std::vector<uint8_t>& data) {
  Response response = {};
  response.parts = {};

  auto sequenceLength = ntohs(extract<uint16_t>(data, 0));
  response.sequenceId = hton64(extract<uint64_t>(data, 2));

  auto partsNumber = ntohs(extract<uint16_t>(data, 11));

  auto headerSize = ntohs(extract<uint16_t>(data, 13));
  auto headerBytes =
      std::vector<uint8_t>(data.begin() + 15, data.begin() + 15 + headerSize);

  auto pos = 15 + headerSize;
  while (pos < data.size()) {
    auto partSize = ntohs(extract<uint16_t>(data, pos));

    response.parts.push_back(std::vector<uint8_t>(
        data.begin() + pos + 2, data.begin() + pos + 2 + partSize));
    pos += 2 + partSize;
  }

  pbDecode(response.mercuryHeader, Header_fields, headerBytes);
  response.fail = response.mercuryHeader.has_status_code &&
                  (response.mercuryHeader.status_code < 200 ||
                   response.mercuryHeader.status_code >= 300);

  return response;
}

uint64_t MercurySession::executeSubscription(RequestType method,
                                             const std::string& uri,
                                             ResponseCallback callback,
                                             ResponseCallback subscription,
                                             DataParts& payload) {
  const std::string requestName = RequestTypeMap[method];
  // Encode header
  Header requestHeader = {};
  if (uri.size() >= sizeof(requestHeader.uri) ||
      requestName.size() >= sizeof(requestHeader.method)) {
    CSPOT_LOG(error,
              "Mercury request header is too large (uri=%u/%u, method=%u/%u)",
              static_cast<unsigned>(uri.size()),
              static_cast<unsigned>(sizeof(requestHeader.uri) - 1),
              static_cast<unsigned>(requestName.size()),
              static_cast<unsigned>(sizeof(requestHeader.method) - 1));
    if (callback) {
      Response failure = {};
      failure.fail = true;
      callback(failure);
    }
    return 0;
  }
  pbPutString(uri, requestHeader.uri);
  pbPutString(requestName, requestHeader.method);

  requestHeader.has_method = true;
  requestHeader.has_uri = true;

  // GET and SEND are actually the same. Therefore the override
  // The difference between them is only in header's method
  if (method == RequestType::GET) {
    method = RequestType::SEND;
  }

  auto headerBytes = pbEncode(Header_fields, &requestHeader);

  uint64_t requestSequence = 0;
  {
    std::lock_guard<std::mutex> lock(requestStateMutex);
    requestSequence = sequenceId++;
    callbacks.insert_or_assign(requestSequence, callback);
    if (method == RequestType::SUB)
      subscriptions.insert_or_assign(uri, subscription);
  }
  CSPOT_LOG(debug, "Mercury request %s seq=%llu uri=%s",
            requestName.c_str(),
            static_cast<unsigned long long>(requestSequence), uri.c_str());

  // Structure: [Sequence size] [SequenceId] [0x1] [Payloads number]
  // [Header size] [Header] [Payloads (size + data)]

  // Pack sequenceId
  auto sequenceIdBytes = pack<uint64_t>(hton64(requestSequence));
  auto sequenceSizeBytes = pack<uint16_t>(htons(sequenceIdBytes.size()));

  sequenceIdBytes.insert(sequenceIdBytes.begin(), sequenceSizeBytes.begin(),
                         sequenceSizeBytes.end());
  sequenceIdBytes.push_back(0x01);

  auto payloadNum = pack<uint16_t>(htons(payload.size() + 1));
  sequenceIdBytes.insert(sequenceIdBytes.end(), payloadNum.begin(),
                         payloadNum.end());

  auto headerSizePayload = pack<uint16_t>(htons(headerBytes.size()));
  sequenceIdBytes.insert(sequenceIdBytes.end(), headerSizePayload.begin(),
                         headerSizePayload.end());
  sequenceIdBytes.insert(sequenceIdBytes.end(), headerBytes.begin(),
                         headerBytes.end());

  // Encode all the payload parts
  for (int x = 0; x < payload.size(); x++) {
    headerSizePayload = pack<uint16_t>(htons(payload[x].size()));
    sequenceIdBytes.insert(sequenceIdBytes.end(), headerSizePayload.begin(),
                           headerSizePayload.end());
    sequenceIdBytes.insert(sequenceIdBytes.end(), payload[x].begin(),
                           payload[x].end());
  }

  try {
    this->sendPacketSafe(
        static_cast<std::underlying_type<RequestType>::type>(method),
        sequenceIdBytes);
  } catch (const std::exception& error) {
    {
      std::lock_guard<std::mutex> lock(requestStateMutex);
      callbacks.erase(requestSequence);
      if (method == RequestType::SUB) subscriptions.erase(uri);
    }
    CSPOT_LOG(error, "Mercury request send failed: %s", error.what());
    if (callback) {
      Response failure = {};
      failure.fail = true;
      callback(failure);
    }
  }

  return requestSequence;
}

uint32_t MercurySession::requestAudioKey(const std::vector<uint8_t>& trackId,
                                         const std::vector<uint8_t>& fileId,
                                         AudioKeyCallback audioCallback) {
  auto buffer = fileId;

  uint32_t requestSequence = 0;
  {
    std::lock_guard<std::mutex> lock(requestStateMutex);
    requestSequence = audioKeySequence++;
    audioKeyCallbacks.insert_or_assign(requestSequence, audioCallback);
  }

  // Structure: [FILEID] [TRACKID] [4 BYTES SEQUENCE ID] [0x00, 0x00]
  buffer.insert(buffer.end(), trackId.begin(), trackId.end());
  auto audioKeySequenceBuffer = pack<uint32_t>(htonl(requestSequence));
  buffer.insert(buffer.end(), audioKeySequenceBuffer.begin(),
                audioKeySequenceBuffer.end());
  auto suffix = std::vector<uint8_t>({0x00, 0x00});
  buffer.insert(buffer.end(), suffix.begin(), suffix.end());

  // Used for broken connection detection
  // this->lastRequestTimestamp = timeProvider->getSyncedTimestamp();
  try {
    this->sendPacketSafe(
        static_cast<uint8_t>(RequestType::AUDIO_KEY_REQUEST_COMMAND), buffer);
  } catch (const std::exception& error) {
    {
      std::lock_guard<std::mutex> lock(requestStateMutex);
      audioKeyCallbacks.erase(requestSequence);
    }
    CSPOT_LOG(error, "Audio key request send failed: %s", error.what());
    if (audioCallback) audioCallback(false, buffer);
  }
  return requestSequence;
}
