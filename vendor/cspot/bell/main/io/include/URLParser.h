#pragma once

#include <cstdlib>    // for strtol, size_t
#include <regex>      // for match_results, match_results<>::value_type, sub...
#include <stdexcept>  // for invalid_argument
#include <string>     // for string, allocator, operator+, char_traits, oper...
#include <vector>     // for vector

namespace bell {
class URLParser {
 public:
  static std::string urlEncode(const std::string& value) {
    std::string new_str = "";
    static auto hex_digt = "0123456789ABCDEF";

    std::string result;
    result.reserve(value.size() << 1);

    for (auto ch : value) {
      if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_' || ch == '!' ||
          ch == '\'' || ch == '(' || ch == ')' || ch == '*' || ch == '~' ||
          ch == '.')  // !'()*-._~
      {
        result.push_back(ch);
      } else {
        result += std::string("%") +
                  hex_digt[static_cast<unsigned char>(ch) >> 4] +
                  hex_digt[static_cast<unsigned char>(ch) & 15];
      }
    }

    return result;
  }
  static std::string urlDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
      auto ch = value[i];

      if (ch == '%' && (i + 2) < value.size()) {
        auto hex = value.substr(i + 1, 2);
        auto dec = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
        result.push_back(dec);
        i += 2;
      } else if (ch == '+') {
        result.push_back(' ');
      } else {
        result.push_back(ch);
      }
    }

    return result;
  }

  std::string host;
  int port = -1;

  std::string schema = "http";
  std::string path;
#ifdef BELL_DISABLE_REGEX
  void parse(const char* url, std::vector<std::string>& match);
#else
  static const std::regex urlParseRegex;
#endif

#ifndef BELL_DISABLE_REGEX
  // Iterative equivalent of
  // ^(?:([^:/?#]+):)?(?://([^/?#]*))?([^?#]*)(\?(?:[^#]*))?(#(?:.*))?
  // Returns {url, schema, host, path, query-with-'?', fragment-with-'#'}.
  static std::vector<std::string> splitUrl(const std::string& url) {
    std::vector<std::string> match(6);
    match[0] = url;
    size_t pos = 0;
    const size_t schemaEnd = url.find_first_of(":/?#");
    if (schemaEnd != std::string::npos && schemaEnd > 0 &&
        url[schemaEnd] == ':') {
      match[1] = url.substr(0, schemaEnd);
      pos = schemaEnd + 1;
    }
    if (url.compare(pos, 2, "//") == 0) {
      pos += 2;
      size_t end = url.find_first_of("/?#", pos);
      if (end == std::string::npos) end = url.size();
      match[2] = url.substr(pos, end - pos);
      pos = end;
    }
    size_t pathEnd = url.find_first_of("?#", pos);
    if (pathEnd == std::string::npos) pathEnd = url.size();
    match[3] = url.substr(pos, pathEnd - pos);
    pos = pathEnd;
    if (pos < url.size() && url[pos] == '?') {
      size_t end = url.find('#', pos);
      if (end == std::string::npos) end = url.size();
      match[4] = url.substr(pos, end - pos);
      pos = end;
    }
    if (pos < url.size() && url[pos] == '#') match[5] = url.substr(pos);
    return match;
  }
#endif

  static URLParser parse(const std::string& url) {
    URLParser parser;

    // apply parser.urlParseRegex to url
#ifdef BELL_DISABLE_REGEX
    std::vector<std::string> match(6);
    parser.parse(url.c_str(), match);
#else
    // libstdc++'s std::regex_match recurses once per input character and
    // overflows bell's bounded task stacks (36 KiB on the Switch service) on
    // Spotify storage-resolve/CDN URLs. Split the URL iteratively with the
    // same grammar as urlParseRegex instead.
    std::vector<std::string> match = splitUrl(url);
#endif

    if (match.size() < 3) {
      throw std::invalid_argument("Invalid URL");
    }

    parser.schema = match[1];
    parser.host = match[2];
    parser.path = match[3];

    if (match[4] != "") {
      parser.path += match[4];
    }

    // check if parser.host contains ':'
    if (parser.host.find(':') != std::string::npos) {
      auto port = std::stoi(
          parser.host.substr(parser.host.find(':') + 1, parser.host.size()));
      auto host = parser.host.substr(0, parser.host.find(':'));
      parser.port = port;
      parser.host = host;
    }

    if (parser.port == -1) {
      parser.port = parser.schema == "http" ? 80 : 443;
    }

    return parser;
  }
};
}  // namespace bell