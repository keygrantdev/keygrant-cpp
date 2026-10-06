#include "utf.hpp"

#include <algorithm>

namespace keygrant::detail {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

/** The WHATWG UTF-8 decoder's state between bytes. */
struct Decoder {
  char32_t codePoint = 0;
  int needed = 0;
  int seen = 0;
  unsigned char lower = 0x80;
  unsigned char upper = 0xBF;

  void reset() {
    codePoint = 0;
    needed = 0;
    seen = 0;
    lower = 0x80;
    upper = 0xBF;
  }
};

}  // namespace

std::vector<CodePoint> decodeUtf8(std::string_view bytes) {
  std::vector<CodePoint> out;
  out.reserve(bytes.size());
  Decoder d;
  std::size_t start = 0;
  std::size_t i = 0;
  while (i < bytes.size()) {
    const auto byte = static_cast<unsigned char>(bytes[i]);
    if (d.needed == 0) {
      start = i;
      if (byte <= 0x7F) {
        out.push_back({byte, i, 1});
      } else if (byte >= 0xC2 && byte <= 0xDF) {
        d.needed = 1;
        d.codePoint = byte & 0x1Fu;
      } else if (byte >= 0xE0 && byte <= 0xEF) {
        if (byte == 0xE0) d.lower = 0xA0;
        if (byte == 0xED) d.upper = 0x9F;
        d.needed = 2;
        d.codePoint = byte & 0x0Fu;
      } else if (byte >= 0xF0 && byte <= 0xF4) {
        if (byte == 0xF0) d.lower = 0x90;
        if (byte == 0xF4) d.upper = 0x8F;
        d.needed = 3;
        d.codePoint = byte & 0x07u;
      } else {
        out.push_back({kReplacement, i, 1});
      }
      ++i;
      continue;
    }
    if (byte < d.lower || byte > d.upper) {
      // The sequence so far is one replacement; this byte is read again on its own.
      out.push_back({kReplacement, start, i - start});
      d.reset();
      continue;
    }
    d.lower = 0x80;
    d.upper = 0xBF;
    d.codePoint = (d.codePoint << 6) | (byte & 0x3Fu);
    ++d.seen;
    ++i;
    if (d.seen == d.needed) {
      out.push_back({d.codePoint, start, i - start});
      d.reset();
    }
  }
  if (d.needed != 0) out.push_back({kReplacement, start, bytes.size() - start});
  return out;
}

bool isValidUtf8(std::string_view bytes) {
  for (const CodePoint& cp : decodeUtf8(bytes)) {
    if (cp.value == kReplacement) {
      // A real U+FFFD in the text is three bytes EF BF BD; a replacement for bad bytes is not.
      if (cp.length != 3 || bytes.substr(cp.offset, 3) != "\xEF\xBF\xBD") return false;
    }
  }
  return true;
}

void appendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string sanitizeUtf8(std::string_view bytes) {
  if (isValidUtf8(bytes)) return std::string(bytes);
  std::string out;
  out.reserve(bytes.size() + 8);
  for (const CodePoint& cp : decodeUtf8(bytes)) appendUtf8(out, cp.value);
  return out;
}

std::string decodeUtf8Text(std::string_view bytes) {
  if (bytes.substr(0, 3) == "\xEF\xBB\xBF") bytes.remove_prefix(3);
  return sanitizeUtf8(bytes);
}

std::size_t utf16Length(std::string_view utf8) {
  std::size_t units = 0;
  for (const CodePoint& cp : decodeUtf8(utf8)) units += cp.value >= 0x10000 ? 2 : 1;
  return units;
}

std::string utf16ToUtf8(std::u16string_view utf16) {
  std::string out;
  out.reserve(utf16.size());
  for (std::size_t i = 0; i < utf16.size(); ++i) {
    const char16_t unit = utf16[i];
    if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < utf16.size()) {
      const char16_t next = utf16[i + 1];
      if (next >= 0xDC00 && next <= 0xDFFF) {
        appendUtf8(out, 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) + (static_cast<char32_t>(next) - 0xDC00));
        ++i;
        continue;
      }
    }
    appendUtf8(out, unit >= 0xD800 && unit <= 0xDFFF ? kReplacement : static_cast<char32_t>(unit));
  }
  return out;
}

std::u16string utf8ToUtf16(std::string_view utf8) {
  std::u16string out;
  out.reserve(utf8.size());
  for (const CodePoint& cp : decodeUtf8(utf8)) {
    if (cp.value >= 0x10000) {
      const char32_t v = cp.value - 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp.value));
    }
  }
  return out;
}

bool isJsSpace(char32_t cp) {
  switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
    case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
    case 0x3000: case 0xFEFF:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200A;
  }
}

std::string jsTrim(std::string_view utf8) {
  const std::string text = sanitizeUtf8(utf8);
  const std::vector<CodePoint> points = decodeUtf8(text);
  std::size_t first = 0;
  while (first < points.size() && isJsSpace(points[first].value)) ++first;
  std::size_t last = points.size();
  while (last > first && isJsSpace(points[last - 1].value)) --last;
  if (first == last) return {};
  const std::size_t from = points[first].offset;
  const std::size_t to = points[last - 1].offset + points[last - 1].length;
  return text.substr(from, to - from);
}

std::string asciiUpper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

namespace {

/** One code point toUpperCase changes, and what it becomes (up to three code points, 0 for none). */
struct UpperCase {
  char32_t from;
  char32_t to[3];
};

constexpr UpperCase kUpperCase[] = {
#include "upper_table.inc"
};

}  // namespace

std::string jsUpper(std::string_view utf8) {
  const std::string text = sanitizeUtf8(utf8);
  std::string out;
  out.reserve(text.size());
  for (const CodePoint& cp : decodeUtf8(text)) {
    if (cp.value < 0x80) {
      const char c = static_cast<char>(cp.value);
      out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
      continue;
    }
    const auto* end = kUpperCase + sizeof kUpperCase / sizeof kUpperCase[0];
    const auto* found =
        std::lower_bound(kUpperCase, end, cp.value, [](const UpperCase& row, char32_t value) { return row.from < value; });
    if (found == end || found->from != cp.value) {
      appendUtf8(out, cp.value);
      continue;
    }
    for (const char32_t mapped : found->to) {
      if (mapped != 0) appendUtf8(out, mapped);
    }
  }
  return out;
}

std::string asciiLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

}  // namespace keygrant::detail
