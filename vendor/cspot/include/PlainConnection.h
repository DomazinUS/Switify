#ifndef PLAINCONNECTION_H
#define PLAINCONNECTION_H
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include "win32shim.h"
#else
#include <unistd.h>  // for size_t
#endif
#include <cstdint>     // for uint8_t
#include <functional>  // for function
#include <mutex>       // for mutex
#include <string>      // for string
#include <vector>      // for vector

typedef std::function<bool()> timeoutCallback;

namespace cspot {
class PlainConnection {
 public:
  PlainConnection();
  ~PlainConnection();

  /**
   * @brief Connect to the given AP address
   * 
   * @param apAddress The AP url to connect to
   */
  void connect(const std::string& apAddress);
  void close();
  // Shut down the socket WITHOUT closing the fd: unblocks a recv() blocked
  // on another thread while keeping the descriptor valid. On libnx, closing
  // an fd another thread is recv()ing on frees the descriptor entry and the
  // reader crashes with a null deref (confirmed by crash report).
  void shutdownSocket();

  timeoutCallback timeoutHandler;
  std::vector<uint8_t> sendPrefixPacket(const std::vector<uint8_t>& prefix,
                                        const std::vector<uint8_t>& data);
  std::vector<uint8_t> recvPacket();

  void readBlock(uint8_t* dst, size_t size);
  size_t writeBlock(const std::vector<uint8_t>& data);

 private:
  int apSock;
  // libnx's descriptor table is not safe when two teardown paths operate on
  // the same fd concurrently. Serialize shutdown/close and retire the public
  // fd before releasing it so a second close becomes a harmless no-op.
  std::mutex socketLifecycleMutex;
};
}  // namespace cspot

#endif
