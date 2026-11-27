#include "websocket_client.hpp"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>

#include "X509Bundle.h"

namespace {
constexpr size_t READ_CHUNK = 4096;
constexpr size_t MAX_HANDSHAKE_RESPONSE = 16 * 1024;
constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 10000;
}  // namespace

struct WebSocketClient::Tls {
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_context entropy;

  Tls() {
    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
  }
  ~Tls() {
    mbedtls_net_free(&net);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }
};

WebSocketClient::WebSocketClient(size_t maxMessageBytes)
    : maxMessage(maxMessageBytes) {}

WebSocketClient::~WebSocketClient() { close(); }

bool WebSocketClient::connect(const std::string& host, uint16_t port,
                              const std::string& path, std::string& error,
                              int& httpStatus) {
  close();
  httpStatus = 0;
  inMessage = dropMessage = false;
  skipBytes = 0;
  tls = std::make_unique<Tls>();

  const char* pers = "switify-dealer";
  int ret = mbedtls_ctr_drbg_seed(&tls->drbg, mbedtls_entropy_func,
                                  &tls->entropy,
                                  reinterpret_cast<const unsigned char*>(pers),
                                  strlen(pers));
  if (ret != 0) {
    error = "ctr_drbg_seed " + std::to_string(ret);
    tls.reset();
    return false;
  }
  ret = mbedtls_net_connect(&tls->net, host.c_str(),
                            std::to_string(port).c_str(),
                            MBEDTLS_NET_PROTO_TCP);
  if (ret != 0) {
    error = "connect " + std::to_string(ret);
    tls.reset();
    return false;
  }
  ret = mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    error = "ssl_config_defaults " + std::to_string(ret);
    tls.reset();
    return false;
  }
  // Same certificate policy as bell::TLSSocket: verify against the bundled
  // roots when the bundle is present, otherwise behave like the rest of the
  // Switch transport (libcurl runs with peer verification disabled).
  if (bell::X509Bundle::shouldVerify()) {
    bell::X509Bundle::attach(&tls->conf);
    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
  }
  mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->drbg);
  mbedtls_ssl_conf_read_timeout(&tls->conf, HANDSHAKE_TIMEOUT_MS);
  ret = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
  if (ret == 0) ret = mbedtls_ssl_set_hostname(&tls->ssl, host.c_str());
  if (ret != 0) {
    error = "ssl_setup " + std::to_string(ret);
    tls.reset();
    return false;
  }
  mbedtls_ssl_set_bio(&tls->ssl, &tls->net, mbedtls_net_send, nullptr,
                      mbedtls_net_recv_timeout);
  while ((ret = mbedtls_ssl_handshake(&tls->ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      error = "tls handshake " + std::to_string(ret);
      tls.reset();
      return false;
    }
  }
  open = true;

  unsigned char nonce[16];
  mbedtls_ctr_drbg_random(&tls->drbg, nonce, sizeof(nonce));
  unsigned char key[32] = {};
  size_t keyLength = 0;
  mbedtls_base64_encode(key, sizeof(key), &keyLength, nonce, sizeof(nonce));

  const std::string request =
      "GET " + path + " HTTP/1.1\r\n"
      "Host: " + host + "\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: " +
      std::string(reinterpret_cast<char*>(key), keyLength) +
      "\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "User-Agent: Spotify/127700358 Linux/0 (Switify)\r\n"
      "\r\n";
  if (!writeAll(reinterpret_cast<const uint8_t*>(request.data()),
                request.size())) {
    error = "handshake write failed";
    close();
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(HANDSHAKE_TIMEOUT_MS);
  size_t headerEnd = std::string::npos;
  while (headerEnd == std::string::npos) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline || rx.size() > MAX_HANDSHAKE_RESPONSE) {
      error = "handshake response timeout";
      close();
      return false;
    }
    const int got = fill(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count()));
    if (got < 0) {
      error = "handshake connection closed";
      close();
      return false;
    }
    const std::string_view view(reinterpret_cast<const char*>(rx.data()),
                                rx.size());
    headerEnd = view.find("\r\n\r\n");
  }

  const std::string head(reinterpret_cast<const char*>(rx.data()), headerEnd);
  rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(headerEnd + 4));
  const size_t space = head.find(' ');
  if (space != std::string::npos)
    httpStatus = std::atoi(head.c_str() + space + 1);
  if (httpStatus != 101) {
    const size_t lineEnd = head.find("\r\n");
    error = "handshake rejected: " + head.substr(0, lineEnd);
    close();
    return false;
  }
  return true;
}

void WebSocketClient::close() {
  if (tls) {
    if (open) {
      sendFrame(0x8, nullptr, 0);
      mbedtls_ssl_close_notify(&tls->ssl);
    }
    tls.reset();
  }
  open = false;
  rx.clear();
  rx.shrink_to_fit();
  partial.clear();
  partial.shrink_to_fit();
}

bool WebSocketClient::writeAll(const uint8_t* data, size_t size) {
  if (!tls) return false;
  while (size > 0) {
    const int ret = mbedtls_ssl_write(&tls->ssl, data, size);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
      continue;
    if (ret <= 0) {
      open = false;
      return false;
    }
    data += ret;
    size -= static_cast<size_t>(ret);
  }
  return true;
}

bool WebSocketClient::sendFrame(uint8_t opcode, const uint8_t* data,
                                size_t size) {
  if (!tls || !open) return false;
  std::vector<uint8_t> frame;
  frame.reserve(size + 14);
  frame.push_back(static_cast<uint8_t>(0x80 | opcode));
  if (size < 126) {
    frame.push_back(static_cast<uint8_t>(0x80 | size));
  } else if (size <= 0xffff) {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>(size >> 8));
    frame.push_back(static_cast<uint8_t>(size));
  } else {
    frame.push_back(0x80 | 127);
    for (int shift = 56; shift >= 0; shift -= 8)
      frame.push_back(
          static_cast<uint8_t>(static_cast<uint64_t>(size) >> shift));
  }
  uint8_t mask[4];
  mbedtls_ctr_drbg_random(&tls->drbg, mask, sizeof(mask));
  frame.insert(frame.end(), mask, mask + 4);
  for (size_t i = 0; i < size; ++i) frame.push_back(data[i] ^ mask[i & 3]);
  return writeAll(frame.data(), frame.size());
}

bool WebSocketClient::sendText(const std::string& text) {
  return sendFrame(0x1, reinterpret_cast<const uint8_t*>(text.data()),
                   text.size());
}

int WebSocketClient::fill(uint32_t timeoutMs) {
  if (!tls || !open) return -1;
  // The service only owns two bsd:u sessions. A blocking select()/recv here
  // would pin one of them for the whole timeout and stall the AP, CDN and
  // HTTP traffic queued behind it. Poll with a zero timeout and wait in user
  // space instead; only call into mbedtls once bytes are actually pending.
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  for (;;) {
    if (mbedtls_ssl_get_bytes_avail(&tls->ssl) > 0) break;
    const int ready =
        mbedtls_net_poll(&tls->net, MBEDTLS_NET_POLL_READ, 0);
    if (ready < 0) {
      open = false;
      return -1;
    }
    if (ready & MBEDTLS_NET_POLL_READ) break;
    if (std::chrono::steady_clock::now() >= deadline) return 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // Data is pending: the rest of a TLS record normally follows immediately.
  mbedtls_ssl_conf_read_timeout(&tls->conf, 200);
  uint8_t buffer[READ_CHUNK];
  const int ret = mbedtls_ssl_read(&tls->ssl, buffer, sizeof(buffer));
  if (ret > 0) {
    rx.insert(rx.end(), buffer, buffer + ret);
    return ret;
  }
  if (ret == MBEDTLS_ERR_SSL_TIMEOUT || ret == MBEDTLS_ERR_SSL_WANT_READ ||
      ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    return 0;
  open = false;
  return -1;
}

bool WebSocketClient::parse(std::string& message, bool& closed, bool& failed) {
  for (;;) {
    if (skipBytes > 0) {
      const size_t n =
          static_cast<size_t>(std::min<uint64_t>(skipBytes, rx.size()));
      rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(n));
      skipBytes -= n;
      if (skipBytes > 0) return false;
      continue;
    }
    if (rx.size() < 2) return false;
    const bool fin = (rx[0] & 0x80) != 0;
    const uint8_t opcode = rx[0] & 0x0f;
    const bool masked = (rx[1] & 0x80) != 0;
    uint64_t length = rx[1] & 0x7f;
    size_t pos = 2;
    if (length == 126) {
      if (rx.size() < 4) return false;
      length = (static_cast<uint64_t>(rx[2]) << 8) | rx[3];
      pos = 4;
    } else if (length == 127) {
      if (rx.size() < 10) return false;
      length = 0;
      for (int i = 0; i < 8; ++i) length = (length << 8) | rx[2 + i];
      pos = 10;
    }
    uint8_t mask[4] = {};
    if (masked) {
      if (rx.size() < pos + 4) return false;
      std::memcpy(mask, &rx[pos], 4);
      pos += 4;
    }

    if (opcode >= 0x8) {
      if (length > 125 || !fin) {
        failed = true;
        return false;
      }
      if (rx.size() < pos + length) return false;
      std::vector<uint8_t> payload(
          rx.begin() + static_cast<std::ptrdiff_t>(pos),
          rx.begin() + static_cast<std::ptrdiff_t>(pos + length));
      if (masked)
        for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];
      rx.erase(rx.begin(),
               rx.begin() + static_cast<std::ptrdiff_t>(pos + length));
      if (opcode == 0x8) {
        closed = true;
        return false;
      }
      if (opcode == 0x9) sendFrame(0xA, payload.data(), payload.size());
      continue;
    }

    if (opcode == 0x1 || opcode == 0x2) {
      inMessage = true;
      dropMessage = false;
      partial.clear();
    } else if (opcode != 0x0 || !inMessage) {
      failed = true;
      return false;
    }

    if (dropMessage || partial.size() + length > maxMessage) {
      if (!dropMessage) {
        dropMessage = true;
        ++dropped;
        partial.clear();
        partial.shrink_to_fit();
      }
      rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(pos));
      skipBytes = length;
      if (fin) inMessage = dropMessage = false;
      continue;
    }

    if (rx.size() < pos + length) return false;
    const size_t start = partial.size();
    partial.append(reinterpret_cast<const char*>(&rx[pos]),
                   static_cast<size_t>(length));
    if (masked)
      for (size_t i = 0; i < length; ++i) partial[start + i] ^= mask[i & 3];
    rx.erase(rx.begin(),
             rx.begin() + static_cast<std::ptrdiff_t>(pos + length));
    if (fin) {
      inMessage = false;
      message.swap(partial);
      partial.clear();
      return true;
    }
  }
}

WebSocketClient::ReadResult WebSocketClient::read(std::string& message,
                                                  uint32_t timeoutMs) {
  if (!open) return ReadResult::Closed;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  for (;;) {
    bool closed = false;
    bool failed = false;
    if (parse(message, closed, failed)) return ReadResult::Message;
    if (closed) {
      close();
      return ReadResult::Closed;
    }
    if (failed) {
      close();
      return ReadResult::Error;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return ReadResult::Timeout;
    const int got = fill(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count()));
    if (got < 0) {
      close();
      return ReadResult::Closed;
    }
    if (got == 0 && std::chrono::steady_clock::now() >= deadline)
      return ReadResult::Timeout;
  }
}
