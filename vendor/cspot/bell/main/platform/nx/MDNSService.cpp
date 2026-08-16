// Nintendo Switch (libnx) implementation of bell::MDNSService.
// Always uses the vendored mdnssvc responder (no avahi on this platform).
// The host IP is obtained from libnx's gethostid(), which returns the
// console's current IP address in network byte order (same source ftpd uses).

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "BellLogger.h"
#include "MDNSService.h"
#include "mdnssvc.h"

using namespace bell;

class implMDNSService : public MDNSService {
 private:
  struct mdns_service* service;

 public:
  static struct mdnsd* mdnsServer;
  static std::atomic<size_t> instances;

  implMDNSService(struct mdns_service* service) : service(service) {
    instances++;
  }
  void unregisterService();
};

struct mdnsd* implMDNSService::mdnsServer = NULL;
std::atomic<size_t> implMDNSService::instances = 0;
static std::mutex registerMutex;

void implMDNSService::unregisterService() {
  mdns_service_remove(implMDNSService::mdnsServer, service);
  if (!--instances && implMDNSService::mdnsServer) {
    mdnsd_stop(implMDNSService::mdnsServer);
    implMDNSService::mdnsServer = nullptr;
  }
}

std::unique_ptr<MDNSService> MDNSService::registerService(
    const std::string& serviceName, const std::string& serviceType,
    const std::string& serviceProto, const std::string& serviceHost,
    int servicePort, const std::map<std::string, std::string> txtData) {
  std::lock_guard lock(registerMutex);

  if (!implMDNSService::mdnsServer) {
    struct in_addr addr;
    // libnx: gethostid() returns the console's current IPv4 address
    // in network byte order (0 / INADDR_LOOPBACK when not connected).
    addr.s_addr = gethostid();

    if (addr.s_addr == 0 || addr.s_addr == INADDR_NONE ||
        addr.s_addr == htonl(INADDR_LOOPBACK)) {
      BELL_LOG(error, "MDNS", "no network connection, cannot start mDNS");
      return nullptr;
    }

    char hostname[64] = "switify";

    implMDNSService::mdnsServer = mdnsd_start(addr, false);
    if (implMDNSService::mdnsServer) {
      mdnsd_set_hostname(implMDNSService::mdnsServer, hostname, addr);
    }
  }

  if (implMDNSService::mdnsServer) {
    std::vector<const char*> txt;
    std::vector<std::unique_ptr<std::string>> txtStr;

    for (auto& [key, value] : txtData) {
      auto str = std::make_unique<std::string>(key + "=" + value);
      txtStr.push_back(std::move(str));
      txt.push_back(txtStr.back()->c_str());
    }

    txt.push_back(NULL);
    std::string type(serviceType + "." + serviceProto + ".local");

    BELL_LOG(info, "MDNS", "using built-in mDNS for %s", serviceName.c_str());
    auto service =
        mdnsd_register_svc(implMDNSService::mdnsServer, serviceName.c_str(),
                           type.c_str(), servicePort, NULL, txt.data());

    if (service)
      return std::make_unique<implMDNSService>(service);
  }

  BELL_LOG(error, "MDNS", "cannot start mDNS listener for %s",
           serviceName.c_str());
  return nullptr;
}
