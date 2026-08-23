#include "URLParser.h"

namespace bell {

#ifdef BELL_DISABLE_REGEX
void URLParser::parse(const char* url, std::vector<std::string>& match) {
  if (!url || match.size() < 6) {
    match.clear();
    return;
  }

  const std::string value(url);
  match.assign(6, {});
  match[0] = value;

  const size_t schemeEnd = value.find("://");
  if (schemeEnd == std::string::npos || schemeEnd == 0) {
    match.clear();
    return;
  }

  const size_t authorityStart = schemeEnd + 3;
  const size_t authorityEnd = value.find_first_of("/?#", authorityStart);
  match[1] = value.substr(0, schemeEnd);
  match[2] = value.substr(
      authorityStart, authorityEnd == std::string::npos
                          ? std::string::npos
                          : authorityEnd - authorityStart);
  if (match[2].empty()) {
    match.clear();
    return;
  }

  if (authorityEnd == std::string::npos) return;
  size_t cursor = authorityEnd;
  if (value[cursor] == '/') {
    const size_t pathEnd = value.find_first_of("?#", cursor);
    match[3] = value.substr(cursor, pathEnd == std::string::npos
                                       ? std::string::npos
                                       : pathEnd - cursor);
    if (pathEnd == std::string::npos) return;
    cursor = pathEnd;
  }
  if (value[cursor] == '?') {
    const size_t queryEnd = value.find('#', cursor);
    match[4] = value.substr(cursor, queryEnd == std::string::npos
                                       ? std::string::npos
                                       : queryEnd - cursor);
    if (queryEnd == std::string::npos) return;
    cursor = queryEnd;
  }
  if (value[cursor] == '#') match[5] = value.substr(cursor);
}
#else
const std::regex URLParser::urlParseRegex = std::regex(
    "^(?:([^:/?#]+):)?(?://([^/?#]*))?([^?#]*)(\\?(?:[^#]*))?(#(?:.*))?");
#endif
}  // namespace bell
