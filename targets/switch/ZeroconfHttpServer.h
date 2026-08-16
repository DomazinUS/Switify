#pragma once

// Minimal blocking HTTP server for the Spotify zeroconf handshake.
//
// Replaces bell's civetweb-based BellHTTPServer on the Switch so civetweb
// does not need to be ported (bell is built with BELL_DISABLE_WEBSERVER).
// It serves exactly the three endpoints the Spotify app talks to:
//   GET  /spotify_info   -> zeroconf getInfo JSON
//   POST /spotify_info   -> addUser (urlencoded credentials blob)
//   GET  /close          -> shut down
//
// Runs its own accept thread; handlers are invoked on that thread.

#include <functional>
#include <map>
#include <string>
#include <thread>

class ZeroconfHttpServer {
 public:
  // getInfoJson: returns the zeroconf info document
  // onAddUser: receives the parsed+urldecoded form fields from the POST
  // onClose: called on GET /close
  ZeroconfHttpServer(int port, std::function<std::string()> getInfoJson,
                     std::function<void(std::map<std::string, std::string>)>
                         onAddUser,
                     std::function<void()> onClose);
  ~ZeroconfHttpServer();

  bool start();  // returns false if the socket could not be bound
  void stop();

 private:
  int port;
  int listenFd = -1;
  bool running = false;
  std::thread serverThread;

  std::function<std::string()> getInfoJson;
  std::function<void(std::map<std::string, std::string>)> onAddUser;
  std::function<void()> onClose;

  void serverLoop();
  void handleClient(int fd);

  static std::string urlDecode(const std::string& in);
  static std::map<std::string, std::string> parseUrlEncoded(
      const std::string& body);
};
