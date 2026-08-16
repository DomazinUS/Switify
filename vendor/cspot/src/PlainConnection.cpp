#include "PlainConnection.h"

#ifndef _WIN32
#include <arpa/inet.h>  // for ntohl, htonl (not transitively provided by newlib)
#include <fcntl.h>      // for fcntl, O_NONBLOCK
#include <netdb.h>  // for addrinfo, freeaddrinfo, getaddrinfo
#include <netinet/in.h>   // for IPPROTO_IP, IPPROTO_TCP
#include <netinet/tcp.h>  // for TCP_NODELAY
#include <sys/errno.h>    // for EAGAIN, EINTR, ETIMEDOUT, errno
#include <sys/select.h>   // for select, fd_set
#include <sys/socket.h>   // for setsockopt, connect, recv, send, shutdown
#include <sys/time.h>     // for timeval
#include <cstring>        // for memset
#include <stdexcept>      // for runtime_error
#else
#include <ws2tcpip.h>
#endif
#include "BellLogger.h"  // for AbstractLogger
#include "Logger.h"      // for CSPOT_LOG
#include "Packet.h"      // for cspot
#include "Utils.h"       // for extract, pack

using namespace cspot;

static int getErrno() {
#ifdef _WIN32
  int code = WSAGetLastError();
  if (code == WSAETIMEDOUT)
    return ETIMEDOUT;
  if (code == WSAEINTR)
    return EINTR;
  return code;
#else
  return errno;
#endif
}

PlainConnection::PlainConnection() {
  this->apSock = -1;
};

PlainConnection::~PlainConnection() {
  this->close();
};

void PlainConnection::connect(const std::string& apAddress) {
  struct addrinfo h, *airoot = nullptr, *ai;
  std::string hostname = apAddress.substr(0, apAddress.find(":"));
  std::string portStr =
      apAddress.substr(apAddress.find(":") + 1, apAddress.size());
  memset(&h, 0, sizeof(h));
  h.ai_family = AF_INET;
  h.ai_socktype = SOCK_STREAM;
  h.ai_protocol = IPPROTO_IP;

  // Lookup host
  const int lookupResult =
      getaddrinfo(hostname.c_str(), portStr.c_str(), &h, &airoot);
  if (lookupResult != 0 || !airoot)
    throw std::runtime_error("DNS lookup failed for Spotify access point");

  // find the right ai, connect to server
  bool connected = false;
  for (ai = airoot; ai; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
      continue;

    this->apSock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (this->apSock < 0)
      continue;

#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(this->apSock, FIONBIO, &nonBlocking);
#else
    const int oldFlags = fcntl(this->apSock, F_GETFL, 0);
    if (oldFlags >= 0) fcntl(this->apSock, F_SETFL, oldFlags | O_NONBLOCK);
#endif

    int connectResult = ::connect(this->apSock,
                                  (struct sockaddr*)ai->ai_addr,
                                  ai->ai_addrlen);
    if (connectResult != 0) {
      const int error = getErrno();
#ifdef _WIN32
      const bool pending = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
      const bool pending = error == EINPROGRESS || error == EWOULDBLOCK;
#endif
      if (pending) {
        fd_set writeSet;
        fd_set errorSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(this->apSock, &writeSet);
        FD_SET(this->apSock, &errorSet);
        struct timeval connectTimeout;
        connectTimeout.tv_sec = 6;
        connectTimeout.tv_usec = 0;
        const int selected = select(this->apSock + 1, nullptr, &writeSet,
                                    &errorSet, &connectTimeout);
        if (selected > 0 && FD_ISSET(this->apSock, &writeSet)) {
          int socketError = 0;
#ifdef _WIN32
          int socketErrorSize = sizeof(socketError);
#else
          socklen_t socketErrorSize = sizeof(socketError);
#endif
          if (getsockopt(this->apSock, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&socketError),
                         &socketErrorSize) == 0 && socketError == 0)
            connectResult = 0;
        }
      }
    }

#ifdef _WIN32
    nonBlocking = 0;
    ioctlsocket(this->apSock, FIONBIO, &nonBlocking);
#else
    if (oldFlags >= 0) fcntl(this->apSock, F_SETFL, oldFlags);
#endif

    if (connectResult == 0) {
#ifdef _WIN32
      uint32_t tv = 3000;
#else
      struct timeval tv;
      tv.tv_sec = 3;
      tv.tv_usec = 0;
#endif
      setsockopt(this->apSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv,
                 sizeof tv);
      setsockopt(this->apSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv,
                 sizeof tv);

      int flag = 1;
      setsockopt(this->apSock, /* socket affected */
                 IPPROTO_TCP,  /* set option at TCP level */
                 TCP_NODELAY,  /* name of option */
                 (char*)&flag, /* the cast is historical cruft */
                 sizeof(int)); /* length of option value */
      connected = true;
      break;
    }

#ifdef _WIN32
    closesocket(this->apSock);
#else
    ::close(this->apSock);
#endif
    apSock = -1;
  }

  freeaddrinfo(airoot);
  if (!connected)
    throw std::runtime_error("TCP connection to Spotify access point failed");
  CSPOT_LOG(debug, "Connected to spotify server");
}

std::vector<uint8_t> PlainConnection::recvPacket() {
  // Read packet size
  std::vector<uint8_t> packetBuffer(4);
  readBlock(packetBuffer.data(), 4);
  uint32_t packetSize = ntohl(extract<uint32_t>(packetBuffer, 0));

  packetBuffer.resize(packetSize, 0);

  // Read actual data
  readBlock(packetBuffer.data() + 4, packetSize - 4);

  return packetBuffer;
}

std::vector<uint8_t> PlainConnection::sendPrefixPacket(
    const std::vector<uint8_t>& prefix, const std::vector<uint8_t>& data) {
  // Calculate full packet length
  uint32_t actualSize = prefix.size() + data.size() + sizeof(uint32_t);

  // Packet structure [PREFIX] + [SIZE] +  [DATA]
  auto sizeRaw = pack<uint32_t>(htonl(actualSize));
  sizeRaw.insert(sizeRaw.begin(), prefix.begin(), prefix.end());
  sizeRaw.insert(sizeRaw.end(), data.begin(), data.end());

  // Actually write it to the server
  writeBlock(sizeRaw);

  return sizeRaw;
}

void PlainConnection::readBlock(uint8_t* dst, size_t size) {
  size_t idx = 0;

  while (idx < size) {
    const ssize_t n =
        recv(this->apSock, reinterpret_cast<char*>(dst + idx), size - idx, 0);
    if (n > 0) {
      idx += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      CSPOT_LOG(error, "Spotify access point closed the connection (recv=0)");
      throw std::runtime_error("Spotify access point closed the connection");
    }

    const int error = getErrno();
    if (error == EAGAIN
#ifdef EWOULDBLOCK
        || error == EWOULDBLOCK
#endif
        || error == ETIMEDOUT) {
      if (timeoutHandler()) {
        CSPOT_LOG(error, "Connection idle timeout requires reconnection");
        throw std::runtime_error("Reconnection required");
      }
      continue;
    }
    if (error == EINTR)
      continue;

    CSPOT_LOG(error, "Spotify socket read failed (errno=%d: %s)", error,
              strerror(error));
    throw std::runtime_error("Error in read");
  }
}

size_t PlainConnection::writeBlock(const std::vector<uint8_t>& data) {
  size_t idx = 0;

  while (idx < data.size()) {
    const ssize_t n =
        send(this->apSock, reinterpret_cast<const char*>(data.data() + idx),
             data.size() - idx, 0);
    if (n > 0) {
      idx += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      CSPOT_LOG(error, "Spotify socket send returned zero");
      throw std::runtime_error("Spotify access point closed the connection");
    }

    const int error = getErrno();
    if (error == EAGAIN
#ifdef EWOULDBLOCK
        || error == EWOULDBLOCK
#endif
        || error == ETIMEDOUT) {
      if (timeoutHandler())
        throw std::runtime_error("Reconnection required");
      continue;
    }
    if (error == EINTR)
      continue;

    CSPOT_LOG(error, "Spotify socket write failed (errno=%d: %s)", error,
              strerror(error));
    throw std::runtime_error("Error in write");
  }

  return data.size();
}

void PlainConnection::shutdownSocket() {
  std::lock_guard<std::mutex> lock(socketLifecycleMutex);
  if (this->apSock < 0)
    return;
  shutdown(this->apSock, SHUT_RDWR);
}

void PlainConnection::close() {
  int socketToClose = -1;
  {
    std::lock_guard<std::mutex> lock(socketLifecycleMutex);
    if (this->apSock < 0)
      return;
    socketToClose = this->apSock;
    // Retire the descriptor before calling into libnx. Another owner or the
    // destructor can now call close() safely without touching the same fd.
    this->apSock = -1;
  }

  CSPOT_LOG(info, "Closing socket...");
  // Do not call shutdown() here. MercurySession::disconnect() explicitly
  // shuts down its live reader before joining it. Reconnect reaches close()
  // only after recv() has already returned. The redundant shutdown used to
  // race another teardown path in libnx's devoptab and caused a null data
  // abort in _socketGetFd (Atmosphere 2168-0002).
#ifdef _WIN32
  closesocket(socketToClose);
#else
  ::close(socketToClose);
#endif
}
