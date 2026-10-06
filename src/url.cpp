#include "url.hpp"

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "utf.hpp"

namespace keygrant::detail {

namespace {

bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }

int hexValue(char c) {
  if (digit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** The URL as the WHATWG parser first cleans it: no leading or trailing C0/space, no tab or newline. */
std::string cleaned(const std::string& url) {
  std::size_t from = 0;
  std::size_t to = url.size();
  while (from < to && static_cast<unsigned char>(url[from]) <= 0x20) ++from;
  while (to > from && static_cast<unsigned char>(url[to - 1]) <= 0x20) --to;
  std::string out;
  for (std::size_t i = from; i < to; ++i) {
    if (url[i] != '\t' && url[i] != '\n' && url[i] != '\r') out.push_back(url[i]);
  }
  return out;
}

/** application/x-www-form-urlencoded decoding of one name or value. */
std::string formDecode(const std::string& text) {
  std::string bytes;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '+') {
      bytes.push_back(' ');
    } else if (c == '%' && i + 2 < text.size() && hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0) {
      bytes.push_back(static_cast<char>(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2])));
      i += 2;
    } else {
      bytes.push_back(c);
    }
  }
  return sanitizeUtf8(bytes);
}

/** application/x-www-form-urlencoded encoding of one name or value. */
std::string formEncode(const std::string& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : sanitizeUtf8(text)) {
    const auto byte = static_cast<unsigned char>(c);
    if (alpha(c) || digit(c) || c == '*' || c == '-' || c == '.' || c == '_') {
      out.push_back(c);
    } else if (c == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    }
  }
  return out;
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

Pairs parseQuery(const std::string& query) {
  Pairs pairs;
  std::size_t start = 0;
  while (start <= query.size()) {
    const std::size_t end = query.find('&', start);
    const std::string part = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!part.empty()) {
      const std::size_t eq = part.find('=');
      pairs.emplace_back(formDecode(part.substr(0, eq)), eq == std::string::npos ? "" : formDecode(part.substr(eq + 1)));
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return pairs;
}

}  // namespace

std::string withQueryParam(const std::string& url, const std::string& name, const std::string& value) {
  const std::string input = cleaned(url);
  std::size_t colon = 0;
  while (colon < input.size() && input[colon] != ':') ++colon;
  bool scheme = colon > 0 && colon < input.size() && alpha(input[0]);
  for (std::size_t i = 1; scheme && i < colon; ++i) {
    const char c = input[i];
    scheme = alpha(c) || digit(c) || c == '+' || c == '-' || c == '.';
  }
  if (!scheme) throw std::invalid_argument("not an absolute URL: " + url);
  const std::string schemeName = asciiLower(input.substr(0, colon));
  std::string rest = input.substr(colon + 1);

  std::optional<std::string> fragment;
  if (const std::size_t hash = rest.find('#'); hash != std::string::npos) {
    fragment = rest.substr(hash + 1);
    rest.resize(hash);
  }
  std::string query;
  if (const std::size_t question = rest.find('?'); question != std::string::npos) {
    query = rest.substr(question + 1);
    rest.resize(question);
  }
  if (schemeName == "http" || schemeName == "https") {
    if (rest.size() < 3 || rest[0] != '/' || rest[1] != '/') throw std::invalid_argument("not an absolute URL: " + url);
    const std::size_t pathAt = rest.find('/', 2);
    const std::string authority = rest.substr(2, pathAt == std::string::npos ? std::string::npos : pathAt - 2);
    const std::size_t at = authority.rfind('@');
    const std::string userinfo = at == std::string::npos ? "" : authority.substr(0, at + 1);
    const std::string host = at == std::string::npos ? authority : authority.substr(at + 1);
    if (host.empty() || host[0] == ':') throw std::invalid_argument("no host in the URL: " + url);
    const std::string path = pathAt == std::string::npos ? "/" : rest.substr(pathAt);
    rest = "//" + userinfo + asciiLower(host) + path;
  }

  Pairs pairs = parseQuery(query);
  bool found = false;
  Pairs updated;
  for (auto& pair : pairs) {
    if (pair.first != name) {
      updated.push_back(std::move(pair));
    } else if (!found) {
      found = true;
      updated.emplace_back(name, value);
    }
  }
  if (!found) updated.emplace_back(name, value);
  std::string serialized;
  for (const auto& [key, val] : updated) {
    if (!serialized.empty()) serialized.push_back('&');
    serialized += formEncode(key) + "=" + formEncode(val);
  }
  std::string out = schemeName + ":" + rest + "?" + serialized;
  if (fragment) out += "#" + *fragment;
  return out;
}

}  // namespace keygrant::detail
