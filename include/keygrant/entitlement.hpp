// One thing a licence grants beyond the base product: a flag, a number or a text.
#pragma once

#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace keygrant {

namespace detail {

/**
 * The character types (char, signed and unsigned char, wchar_t, char8_t where
 * the language has it, char16_t and char32_t): a character, never a number, so
 * never an entitlement's number either (Entitlement).
 */
template <class T>
struct IsCharacter : std::false_type {};
template <>
struct IsCharacter<char> : std::true_type {};
template <>
struct IsCharacter<signed char> : std::true_type {};
template <>
struct IsCharacter<unsigned char> : std::true_type {};
template <>
struct IsCharacter<wchar_t> : std::true_type {};
#if defined(__cpp_char8_t)
template <>
struct IsCharacter<char8_t> : std::true_type {};
#endif
template <>
struct IsCharacter<char16_t> : std::true_type {};
template <>
struct IsCharacter<char32_t> : std::true_type {};

/// A pointer to narrow text (`const char*`, `char*`): a C string, which makes a text.
template <class T>
inline constexpr bool kNarrowText = std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>;

/**
 * What would otherwise slip into an Entitlement as a flag, through the
 * conversion to bool every pointer, member pointer and unscoped enum has, or
 * as a number: a character; an enum; a null pointer; and a pointer that is not
 * narrow text, which takes in the wide (`L"pro"`, `TEXT("pro")` on a UNICODE
 * Win32 build), UTF-16, UTF-32 and C++20 `u8` string literals. None of them
 * is an entitlement, so none compiles as one.
 */
template <class T>
inline constexpr bool kNeverAnEntitlement = IsCharacter<T>::value || std::is_enum_v<T> || std::is_null_pointer_v<T> ||
                                            std::is_member_pointer_v<T> || (std::is_pointer_v<T> && !kNarrowText<T>);

}  // namespace detail

/**
 * One thing a licence grants beyond the base product, as its lease signs it
 * (a member of the `ent` claim): a flag (`true`), a number (`5`) or a text
 * (`"pro"`), exactly one of them. A number is a double, as JSON and the
 * reference read one, so 5 and 5.0 are the same entitlement.
 *
 * A class over std::variant<bool, double, std::string> rather than that bare
 * variant, whose converting constructor reads against the app: built from an
 * int it does not compile (bool and double are as good as each other), and
 * built from a string literal it holds a bool wherever the standard library
 * keeps C++17's rule (a pointer converts to bool before it converts to
 * std::string; P0608 changed that, and not every library applies the change
 * in C++17 mode). Here a narrow string literal or C string is a text, any
 * number is a double, and a flag is only a bool, so `{"edition", "pro"}` and
 * `{"seats", 5}` mean what they say on every compiler, and the typed accessors
 * read a value without std::get or std::visit. What would otherwise turn into
 * the flag true or a number does not compile at all, as an entitlement or
 * compared with one: a character ('p'), an enum, a null pointer, and a wide,
 * UTF-16, UTF-32 or C++20 u8 literal (`L"pro"`, `u"pro"`, `U"pro"`, `u8"pro"`;
 * an entitlement's text is UTF-8 in a std::string). Two entitlements are
 * equal when they are the same kind and the same value: a flag never equals a
 * number or a text.
 */
class Entitlement {
 public:
  /// Which of the three it is.
  enum class Kind { Flag, Number, Text };

  Entitlement(bool flag) noexcept : value_(std::in_place_type<bool>, flag) {}
  /// Any number (an int, a double, ...), held as a double. Not a character (below).
  template <class Number, std::enable_if_t<std::is_arithmetic_v<Number> && !std::is_same_v<Number, bool> &&
                                               !detail::IsCharacter<Number>::value,
                                           int> = 0>
  Entitlement(Number number) noexcept : value_(std::in_place_type<double>, static_cast<double>(number)) {}
  /**
   * Not from a character, an enum, a null pointer, or a pointer that is not
   * narrow text (detail::kNeverAnEntitlement): each would otherwise be read as
   * a number (a character's code, 112 for 'p') or, through bool, as the flag
   * true, so it does not compile; nor does comparing an entitlement with one.
   * std::int8_t and std::uint8_t are character types too: cast one to int.
   */
  template <class T, std::enable_if_t<detail::kNeverAnEntitlement<T>, int> = 0>
  Entitlement(T) = delete;
  Entitlement(std::string text) : value_(std::in_place_type<std::string>, std::move(text)) {}
  Entitlement(const char* text) : value_(std::in_place_type<std::string>, text) {}

  Kind kind() const noexcept { return static_cast<Kind>(value_.index()); }
  bool isFlag() const noexcept { return kind() == Kind::Flag; }
  bool isNumber() const noexcept { return kind() == Kind::Number; }
  bool isText() const noexcept { return kind() == Kind::Text; }

  /// The flag; none when it is a number or a text.
  std::optional<bool> flag() const noexcept;
  /// The number; none when it is a flag or a text.
  std::optional<double> number() const noexcept;
  /// The text; none when it is a flag or a number.
  std::optional<std::string> text() const;

  friend bool operator==(const Entitlement& a, const Entitlement& b) { return a.value_ == b.value_; }
  friend bool operator!=(const Entitlement& a, const Entitlement& b) { return !(a == b); }

 private:
  std::variant<bool, double, std::string> value_;
};

/// As JSON writes it: `true`, `5.0`, `"pro"`.
std::ostream& operator<<(std::ostream& out, const Entitlement& entitlement);

}  // namespace keygrant
