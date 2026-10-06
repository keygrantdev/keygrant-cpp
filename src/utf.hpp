// Text as the reference's JavaScript sees it: UTF-8 decoded the WHATWG way
// (every ill-formed sequence one U+FFFD), lengths in UTF-16 code units, and
// String.prototype.trim's idea of white space.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace keygrant::detail {

/** One code point decoded from UTF-8, and where its bytes were. */
struct CodePoint {
  char32_t value;
  std::size_t offset;
  std::size_t length;
};

/// UTF-8 decoded as the WHATWG decoder does (TextDecoder, Buffer.toString): each ill-formed sequence one U+FFFD.
std::vector<CodePoint> decodeUtf8(std::string_view bytes);

/// Whether the bytes are well-formed UTF-8.
bool isValidUtf8(std::string_view bytes);

/// The bytes with every ill-formed sequence replaced by U+FFFD (themselves when well-formed).
std::string sanitizeUtf8(std::string_view bytes);

/// As sanitizeUtf8, without a leading byte-order mark (TextDecoder, fetch's text()).
std::string decodeUtf8Text(std::string_view bytes);

void appendUtf8(std::string& out, char32_t cp);

/// How many UTF-16 code units the text is: a JavaScript string's length.
std::size_t utf16Length(std::string_view utf8);

/// UTF-16 as UTF-8, a lone surrogate replaced by U+FFFD.
std::string utf16ToUtf8(std::u16string_view utf16);

/// UTF-8 (sanitized) as UTF-16.
std::u16string utf8ToUtf16(std::string_view utf8);

/// ECMAScript WhiteSpace and LineTerminator: what String.prototype.trim removes, and what a regex's \s matches.
bool isJsSpace(char32_t cp);

/// String.prototype.trim, on sanitized UTF-8.
std::string jsTrim(std::string_view utf8);

/// ASCII letters upper-cased, every other byte as it is.
std::string asciiUpper(std::string_view text);

/**
 * String.prototype.toUpperCase: the locale-independent full case mapping
 * ("é" is "É", "ß" is "SS"), from a table generated from Node's own answers
 * (scripts/gen-uppercase.mjs). On sanitized UTF-8.
 */
std::string jsUpper(std::string_view utf8);

/// ASCII letters lower-cased, every other byte as it is.
std::string asciiLower(std::string_view text);

}  // namespace keygrant::detail
