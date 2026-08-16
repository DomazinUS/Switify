#include "ZeroconfHttpServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

ZeroconfHttpServer::ZeroconfHttpServer(
    int port, std::function<std::string()> getInfoJson,
    std::function<void(std::map<std::string, std::string>)> onAddUser,
    std::function<void()> onClose)
    : port(port),
      getInfoJson(getInfoJson),
      onAddUser(onAddUser),
      onClose(onClose) {}

ZeroconfHttpServer::~ZeroconfHttpServer() {
  stop();
}

bool ZeroconfHttpServer::start() {
  listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd < 0)
    return false;

  int on = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
      listen(listenFd, 4) < 0) {
    close(listenFd);
    listenFd = -1;
    return false;
  }

  running = true;
  serverThread = std::thread([this] { serverLoop(); });
  return true;
}

void ZeroconfHttpServer::stop() {
  if (!running)
    return;
  running = false;
  if (listenFd >= 0) {
    shutdown(listenFd, SHUT_RDWR);
    close(listenFd);
    listenFd = -1;
  }
  if (serverThread.joinable())
    serverThread.join();
}

void ZeroconfHttpServer::serverLoop() {
  while (running) {
    struct sockaddr_in client;
    socklen_t clientLen = sizeof(client);
    int fd = accept(listenFd, (struct sockaddr*)&client, &clientLen);
    if (fd < 0) {
      if (!running)
        break;
      continue;
    }
    handleClient(fd);
    close(fd);
  }
}

void ZeroconfHttpServer::handleClient(int fd) {
  // Read the request head (and possibly the start of the body)
  std::string request;
  char buf[2048];
  size_t headerEnd = std::string::npos;

  while (headerEnd == std::string::npos && request.size() < 32768) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      return;
    request.append(buf, n);
    headerEnd = request.find("\r\n\r\n");
  }
  if (headerEnd == std::string::npos)
    return;

  // Request line: METHOD SP PATH SP VERSION
  size_t lineEnd = request.find("\r\n");
  std::string requestLine = request.substr(0, lineEnd);
  size_t sp1 = requestLine.find(' ');
  size_t sp2 = requestLine.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos)
    return;
  std::string method = requestLine.substr(0, sp1);
  std::string path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

  // Strip the query string; the Spotify app sends ?action=... params but
  // the meaningful data for addUser is in the POST body.
  size_t q = path.find('?');
  if (q != std::string::npos)
    path = path.substr(0, q);

  // Content-Length, case-insensitive scan of the header block
  size_t contentLength = 0;
  {
    std::string headers = request.substr(0, headerEnd);
    for (auto& c : headers)
      c = tolower(c);
    size_t p = headers.find("content-length:");
    if (p != std::string::npos)
      contentLength = strtoul(headers.c_str() + p + 15, nullptr, 10);
  }

  // Read the rest of the body if needed
  std::string body = request.substr(headerEnd + 4);
  while (body.size() < contentLength) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      break;
    body.append(buf, n);
  }

  std::string responseBody;
  if (path == "/spotify_info" && method == "GET") {
    responseBody = getInfoJson();
  } else if (path == "/spotify_info" && method == "POST") {
    onAddUser(parseUrlEncoded(body));
    responseBody = "{\"status\":101,\"spotifyError\":0,"
                   "\"statusString\":\"ERROR-OK\"}";
  } else if (path == "/close") {
    responseBody = "{}";
    onClose();
  } else {
    const char* notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
    send(fd, notFound, strlen(notFound), 0);
    return;
  }

  char header[256];
  snprintf(header, sizeof(header),
           "HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: %zu\r\n"
           "Connection: close\r\n\r\n",
           responseBody.size());
  send(fd, header, strlen(header), 0);
  send(fd, responseBody.data(), responseBody.size(), 0);
}

std::string ZeroconfHttpServer::urlDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%' && i + 2 < in.size()) {
      char hex[3] = {in[i + 1], in[i + 2], 0};
      out += (char)strtoul(hex, nullptr, 16);
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

std::map<std::string, std::string> ZeroconfHttpServer::parseUrlEncoded(
    const std::string& body) {
  std::map<std::string, std::string> result;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t amp = body.find('&', pos);
    if (amp == std::string::npos)
      amp = body.size();
    size_t eq = body.find('=', pos);
    if (eq != std::string::npos && eq < amp) {
      result[urlDecode(body.substr(pos, eq - pos))] =
          urlDecode(body.substr(eq + 1, amp - eq - 1));
    }
    pos = amp + 1;
  }
  return result;
}
