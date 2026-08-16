#include "HTTPClient.h"

#include <string.h>   // for memcpy
#include <algorithm>  // for transform
#include <cassert>    // for assert
#include <cctype>     // for tolower
#include <ostream>    // for operator<<, basic_ostream
#include <stdexcept>  // for runtime_error

#include "BellSocket.h"  // for bell

using namespace bell;

void HTTPClient::Response::connect(const std::string& url) {
  urlParser = bell::URLParser::parse(url);

  // Open socket of type
  this->socketStream.open(urlParser.host, urlParser.port,
                          urlParser.schema == "https");
}

HTTPClient::Response::~Response() {
  if (this->socketStream.isOpen()) {
    this->socketStream.close();
  }
}

void HTTPClient::Response::rawRequest(const std::string& url,
                                      const std::string& method,
                                      const std::vector<uint8_t>& content,
                                      Headers& headers) {
  urlParser = bell::URLParser::parse(url);

  // Prepare a request
  const char* reqEnd = "\r\n";

  for (int attempt = 0;; attempt++) {
    // Reconnect when the socket was never opened, was closed (for example
    // a keep-alive connection dropped by the server), or the stream is in
    // a failed state left over from a previous request
    if (!socketStream.isOpen() || !socketStream.good()) {
      socketStream.close();
      socketStream.clear();
      socketStream.open(urlParser.host, urlParser.port,
                        urlParser.schema == "https");
      if (!socketStream.good()) {
        throw std::runtime_error("Cannot connect to " + urlParser.host);
      }
    }

    socketStream << method << " " << urlParser.path << " HTTP/1.1" << reqEnd;
    socketStream << "Host: " << urlParser.host << ":" << urlParser.port
                 << reqEnd;
    bool hasConnectionHeader = false;
    for (const auto& header : headers) {
      std::string headerName = header.first;
      std::transform(headerName.begin(), headerName.end(), headerName.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (headerName == "connection") {
        hasConnectionHeader = true;
        break;
      }
    }
    if (!hasConnectionHeader)
      socketStream << "Connection: keep-alive" << reqEnd;
    socketStream << "Accept: */*" << reqEnd;

    // Write content
    if (content.size() > 0) {
      socketStream << "Content-Length: " << content.size() << reqEnd;
    }

    // Write headers
    for (auto& header : headers) {
      socketStream << header.first << ": " << header.second << reqEnd;
    }

    socketStream << reqEnd;

    // Write request body
    if (content.size() > 0) {
      socketStream.write((const char*)content.data(), content.size());
    }

    socketStream.flush();

    // Parse response
    try {
      readResponseHeaders();
      return;
    } catch (const std::runtime_error&) {
      // Only retry when the connection itself died (stale keep-alive
      // socket closed by the server); genuine protocol errors leave the
      // stream in good state and are rethrown immediately
      if (attempt >= 1 || socketStream.good()) {
        throw;
      }
      socketStream.close();
    }
  }
}

void HTTPClient::Response::readResponseHeaders() {
  char *method, *path;
  const char* msgPointer;

  size_t msgLen;
  int pret, minorVersion, status;

  size_t prevbuflen = 0, numHeaders;
  this->contentSize = 0;
  this->hasContentSize = false;
  this->isChunked = false;
  this->bodyRead = false;
  this->rawBody.clear();
  this->httpBufferAvailable = 0;

  while (1) {
    socketStream.getline((char*)httpBuffer.data() + httpBufferAvailable,
                         httpBuffer.size() - httpBufferAvailable);

    // A connection closed by the peer or a socket error leaves the stream
    // in eof/fail state with nothing extracted. Without this check the
    // loop spins forever at 100% CPU, as getline() keeps returning
    // immediately with gcount() == 0 once the stream has failed.
    if (socketStream.eof() || socketStream.bad() ||
        socketStream.gcount() == 0) {
      throw std::runtime_error("Connection closed while reading HTTP headers");
    }

    // getline() hit the buffer limit before finding '\n'; the stream is in
    // fail state and no further progress is possible
    if (socketStream.fail()) {
      throw std::runtime_error("Response too large");
    }

    prevbuflen = httpBufferAvailable;
    httpBufferAvailable += socketStream.gcount();

    // Restore delimiters: getline() consumed the '\n' and wrote a '\0'
    // terminator, put "\r\n" back for the parser. Guarded so a 1-byte
    // first line cannot write before the start of the buffer.
    if (httpBufferAvailable >= 2)
      memcpy(httpBuffer.data() + httpBufferAvailable - 2, "\r\n", 2);

    // Parse the request
    numHeaders = sizeof(phResponseHeaders) / sizeof(phResponseHeaders[0]);

    pret =
        phr_parse_response((const char*)httpBuffer.data(), httpBufferAvailable,
                           &minorVersion, &status, &msgPointer, &msgLen,
                           phResponseHeaders, &numHeaders, prevbuflen);

    if (pret > 0) {
      this->responseStatus = status;
      break; /* successfully parsed the request */
    } else if (pret == -1)
      throw std::runtime_error("Cannot parse http response");

    /* request is incomplete, continue the loop */
    assert(pret == -2);
    if (httpBufferAvailable == httpBuffer.size())
      throw std::runtime_error("Response too large");
  }

  this->responseHeaders = {};

  // Headers have benen read
  for (int headerIndex = 0; headerIndex < numHeaders; headerIndex++) {
    this->responseHeaders.push_back(
        ValueHeader{std::string(phResponseHeaders[headerIndex].name,
                                phResponseHeaders[headerIndex].name_len),
                    std::string(phResponseHeaders[headerIndex].value,
                                phResponseHeaders[headerIndex].value_len)});
  }

  std::string contentLengthValue = std::string(header("content-length"));
  if (contentLengthValue.size() > 0) {
    this->hasContentSize = true;
    this->contentSize = std::stoi(contentLengthValue);
  }
  std::string transferEncoding = std::string(header("transfer-encoding"));
  std::transform(transferEncoding.begin(), transferEncoding.end(),
                 transferEncoding.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  this->isChunked = transferEncoding.find("chunked") != std::string::npos;
}

void HTTPClient::Response::get(const std::string& url, Headers headers) {
  std::string method = "GET";
  return this->rawRequest(url, method, {}, headers);
}

void HTTPClient::Response::post(const std::string& url, Headers headers,
                                const std::vector<uint8_t>& body) {
  std::string method = "POST";
  return this->rawRequest(url, method, body, headers);
}

size_t HTTPClient::Response::contentLength() {
  return contentSize;
}

std::string_view HTTPClient::Response::header(const std::string& headerName) {
  for (auto& header : this->responseHeaders) {
    std::string headerValue = header.first;
    std::transform(headerValue.begin(), headerValue.end(), headerValue.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (headerName == headerValue) {
      return header.second;
    }
  }

  return "";
}

size_t HTTPClient::Response::totalLength() {
  auto rangeHeader = header("content-range");

  if (rangeHeader.find("/") != std::string::npos) {
    return std::stoi(
        std::string(rangeHeader.substr(rangeHeader.find("/") + 1)));
  }

  return this->contentLength();
}

void HTTPClient::Response::readRawBody() {
  if (bodyRead) return;
  bodyRead = true;

  if (hasContentSize && contentSize > 0) {
    rawBody = std::vector<uint8_t>(contentSize);
    socketStream.read((char*)rawBody.data(), contentSize);
    if (static_cast<size_t>(socketStream.gcount()) != contentSize)
      throw std::runtime_error("Connection closed while reading HTTP body");
    return;
  }

  if (isChunked) {
    while (true) {
      std::string sizeLine;
      if (!std::getline(socketStream, sizeLine))
        throw std::runtime_error("Connection closed while reading HTTP chunk");
      if (!sizeLine.empty() && sizeLine.back() == '\r') sizeLine.pop_back();
      const auto extension = sizeLine.find(';');
      if (extension != std::string::npos) sizeLine.resize(extension);
      if (sizeLine.empty())
        throw std::runtime_error("Invalid HTTP chunk size");

      size_t parsed = 0;
      const unsigned long long chunkSize = std::stoull(sizeLine, &parsed, 16);
      if (parsed != sizeLine.size())
        throw std::runtime_error("Invalid HTTP chunk size");
      if (chunkSize == 0) {
        // Consume optional trailers and their terminating blank line.
        std::string trailer;
        while (std::getline(socketStream, trailer)) {
          if (trailer == "\r" || trailer.empty()) break;
        }
        return;
      }
      if (chunkSize > 16 * 1024 * 1024)
        throw std::runtime_error("HTTP chunk is too large");

      const size_t oldSize = rawBody.size();
      rawBody.resize(oldSize + static_cast<size_t>(chunkSize));
      socketStream.read(reinterpret_cast<char*>(rawBody.data() + oldSize),
                        static_cast<std::streamsize>(chunkSize));
      if (static_cast<unsigned long long>(socketStream.gcount()) != chunkSize)
        throw std::runtime_error("Connection closed while reading HTTP chunk");
      char delimiter[2] = {};
      socketStream.read(delimiter, 2);
      if (socketStream.gcount() != 2 || delimiter[0] != '\r' ||
          delimiter[1] != '\n')
        throw std::runtime_error("Invalid HTTP chunk delimiter");
    }
  }

  // HTTP/1.1 responses without Content-Length or chunked framing are delimited
  // by connection close. Spotify's client-token edge currently uses this path
  // on libnx even though the same endpoint exposes Content-Length on desktop.
  // Statuses that never carry a body must not wait for a close.
  if (responseStatus < 100 ||
      (responseStatus >= 200 && responseStatus != 204 &&
       responseStatus != 304)) {
    char byte = 0;
    while (socketStream.get(byte))
      rawBody.push_back(static_cast<uint8_t>(byte));
  }
}

std::string_view HTTPClient::Response::body() {
  readRawBody();
  return std::string_view((char*)rawBody.data(), rawBody.size());
}

std::vector<uint8_t> HTTPClient::Response::bytes() {
  readRawBody();
  return rawBody;
}
