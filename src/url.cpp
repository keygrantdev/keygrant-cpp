#include "url.hpp"

#include <stdexcept>

#include "utf.hpp"

namespace keygrant::detail {

namespace {

/** The query parameter a payment link names its checkout's buyer by. */
constexpr const char* kReference = "client_reference_id";

bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }

/**
 * The URL as the WHATWG parser first cleans it: no leading or trailing C0
 * control or space, no tab or newline, and ill-formed UTF-8 read as a
 * JavaScript string holds it (each bad sequence one U+FFFD).
 */
std::string cleaned(const std::string& url) {
  const std::string text = sanitizeUtf8(url);
  std::size_t from = 0;
  std::size_t to = text.size();
  while (from < to && static_cast<unsigned char>(text[from]) <= 0x20) ++from;
  while (to > from && static_cast<unsigned char>(text[to - 1]) <= 0x20) --to;
  std::string out;
  for (std::size_t i = from; i < to; ++i) {
    if (text[i] != '\t' && text[i] != '\n' && text[i] != '\r') out.push_back(text[i]);
  }
  return out;
}

void percentEncode(std::string& out, unsigned char byte) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  out.push_back('%');
  out.push_back(kHex[byte >> 4]);
  out.push_back(kHex[byte & 0x0F]);
}

/** Whether a byte is in the WHATWG C0 control percent-encode set: a C0 control, DEL, or any byte of a non-ASCII character. */
bool c0Control(unsigned char byte) { return byte < 0x20 || byte > 0x7E; }

/**
 * Text as the WHATWG parser writes it in a query (the query percent-encode
 * set, and `'` for an http(s) URL) or a fragment (the fragment percent-encode
 * set). A `%` is left as it is: what is already encoded is not encoded again.
 */
std::string asParsed(const std::string& text, bool fragment, bool special) {
  std::string out;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    const bool both = c0Control(byte) || c == ' ' || c == '"' || c == '<' || c == '>';
    const bool encode = both || (fragment ? c == '`' : special && c == '\'');
    if (encode) {
      percentEncode(out, byte);
    } else {
      out.push_back(c);
    }
  }
  return out;
}

/** encodeURIComponent: everything but letters, digits and `-_.!~*'()` percent-encoded as UTF-8. */
std::string uriComponent(const std::string& text) {
  std::string out;
  for (const char c : sanitizeUtf8(text)) {
    const bool unreserved = alpha(c) || digit(c) || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' ||
                            c == '*' || c == '\'' || c == '(' || c == ')';
    if (unreserved) {
      out.push_back(c);
    } else {
      percentEncode(out, static_cast<unsigned char>(c));
    }
  }
  return out;
}

bool isHttp(const std::string& base) { return base.rfind("http:", 0) == 0 || base.rfind("https:", 0) == 0; }

}  // namespace

Link absoluteLink(const std::string& url) {
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

  Link link;
  if (const std::size_t hash = rest.find('#'); hash != std::string::npos) {
    link.fragment = rest.substr(hash + 1);
    rest.resize(hash);
  }
  if (const std::size_t question = rest.find('?'); question != std::string::npos) {
    link.query = rest.substr(question + 1);
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
  link.base = schemeName + ":" + rest;
  return link;
}

std::string referencedLink(const std::string& link, const std::string& reference) {
  const Link url = absoluteLink(link);
  const bool special = isHttp(url.base);
  std::string query;
  const std::string written = url.query.value_or("");
  std::size_t start = 0;
  while (start <= written.size()) {
    const std::size_t end = written.find('&', start);
    const std::string part =
        asParsed(written.substr(start, end == std::string::npos ? std::string::npos : end - start), false, special);
    // Its name: the text before the first `=`, or all of it.
    if (!part.empty() && part.substr(0, part.find('=')) != kReference) query += part + "&";
    if (end == std::string::npos) break;
    start = end + 1;
  }
  // The whole query as the parser writes it (setting a URL's search): an http(s) URL's `'` encoded, the reference's too.
  query = asParsed(query + kReference + "=" + uriComponent(reference), false, special);
  std::string out = url.base + "?" + query;
  if (url.fragment) out += "#" + asParsed(*url.fragment, true, special);
  return out;
}

}  // namespace keygrant::detail
