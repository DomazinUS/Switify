#pragma once

#include <stdint.h>  // for uint8_t
#include <memory>    // for shared_ptr, unique_ptr
#include <mutex>     // for mutex
#include <string>    // for string
#include <vector>    // for vector

namespace cspot {
class AuthChallenges;
class LoginBlob;
class PlainConnection;
class ShannonConnection;
}  // namespace cspot

#define LOGIN_REQUEST_COMMAND 0xAB
#define AUTH_SUCCESSFUL_COMMAND 0xAC
#define AUTH_DECLINED_COMMAND 0xAD

namespace cspot {
class Session {
 protected:
  std::unique_ptr<cspot::AuthChallenges> challenges;
  std::shared_ptr<cspot::PlainConnection> conn;
  std::shared_ptr<LoginBlob> authBlob;
  // Reconnect replaces conn/shanConn while the UI and playback workers may
  // issue Mercury requests. Serialize replacement and encrypted writes so no
  // caller can dereference the temporary null connection.
  std::mutex connectionMutex;

  std::shared_ptr<cspot::ShannonConnection> currentShannonConnection();
  void sendPacketSafe(uint8_t command, std::vector<uint8_t>& data);

  std::string deviceId = "142137fd329622137a14901634264e6f332e2411";

 public:
  Session();
  ~Session();

  std::shared_ptr<cspot::ShannonConnection> shanConn;

  void connect(std::unique_ptr<cspot::PlainConnection> connection);
  void connectWithRandomAp();
  void close();
  virtual bool triggerTimeout() = 0;
  std::vector<uint8_t> authenticate(std::shared_ptr<LoginBlob> blob);
  // The device id is part of the authenticated AP identity and must match the
  // id later supplied to keymaster. A stale built-in id authenticates, but
  // makes scoped token requests fail with 403 "Invalid client".
  void setDeviceId(const std::string& id) { deviceId = id; }
};
}  // namespace cspot
