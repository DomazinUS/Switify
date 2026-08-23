#include "URLParser.h"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
  const auto https = bell::URLParser::parse(
      "https://audio.example.com/audio/interactive/token?quality=high#part");
  assert(https.schema == "https");
  assert(https.host == "audio.example.com");
  assert(https.port == 443);
  assert(https.path == "/audio/interactive/token?quality=high");

  const auto customPort =
      bell::URLParser::parse("http://127.0.0.1:8080/status");
  assert(customPort.host == "127.0.0.1");
  assert(customPort.port == 8080);
  assert(customPort.path == "/status");

  const std::string longToken(4096, 'a');
  const auto longUrl = bell::URLParser::parse(
      "https://audio.example.com/audio/interactive/" + longToken);
  assert(longUrl.path.size() == longToken.size() + 19);

  bool rejected = false;
  try {
    (void)bell::URLParser::parse("not a URL");
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);

  std::cout << "Switify URL parser tests passed\n";
  return 0;
}
