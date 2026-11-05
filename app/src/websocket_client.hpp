#pragma once

// Minimal RFC 6455 client on top of mbedtls, used by the background service
// to talk to Spotify's dealer. devkitPro's libcurl (7.69) predates curl's
// WebSocket support, so this keeps a single TLS connection with bounded
// buffers instead of pulling in another networking stack.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class WebSocketClient {
 public:
  enum class ReadResult { Message, Timeout, Closed, Error };

  // Messages larger than maxMessageBytes (cluster pushes, mostly) are
  // discarded while streaming so they never sit in the fixed heap.
  explicit WebSocketClient(size_t maxMessageBytes = 64 * 1024);
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  // Opens TLS to host:port and performs the HTTP/1.1 Upgrade handshake for
  // `path` (which may carry a query string). On failure `error` is set and
  // the connection is closed. `httpStatus` receives the handshake status.
  bool connect(const std::string& host, uint16_t port, const std::string& path,
               std::string& error, int& httpStatus);

  // Waits up to timeoutMs for one complete text/binary message. Control
  // frames (ping/pong) are handled internally.
  ReadResult read(std::string& message, uint32_t timeoutMs);

  bool sendText(const std::string& text);
  void close();
  bool isOpen() const { return open; }
  size_t droppedMessages() const { return dropped; }

 private:
  struct Tls;

  bool writeAll(const uint8_t* data, size_t size);
  bool sendFrame(uint8_t opcode, const uint8_t* data, size_t size);
  // Appends available bytes to rx. Returns >0 bytes read, 0 on timeout and
  // -1 on a closed or failed connection.
  int fill(uint32_t timeoutMs);
  // Parses as much of rx as possible. Returns true when `message` holds a
  // complete data message.
  bool parse(std::string& message, bool& closed, bool& failed);

  std::unique_ptr<Tls> tls;
  bool open = false;
  size_t maxMessage;
  size_t dropped = 0;
  std::vector<uint8_t> rx;
  std::string partial;
  bool inMessage = false;
  bool dropMessage = false;
  uint64_t skipBytes = 0;
};
