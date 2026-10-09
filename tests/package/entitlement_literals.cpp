// What an app's literals make of a keygrant::Entitlement, checked as it
// compiles: as C++17, and as C++20, where char8_t and the u8 literal it types
// exist. A narrow string literal, a number and a bool are entitlements; a
// character, a wide, UTF-16, UTF-32 or u8 literal, an enum and a null pointer
// are not (each would otherwise be the flag true or a number).
#include <keygrant/keygrant.hpp>

#include <cstddef>
#include <string>
#include <type_traits>

#if KEYGRANT_CONSUMER_CXX_STANDARD >= 20 && !defined(__cpp_char8_t)
#error "asked for C++20, built without char8_t: the C++20 checks below would not run"
#endif

namespace {

template <class T>
constexpr bool kMade = std::is_convertible_v<T, keygrant::Entitlement>;

template <class T>
constexpr bool kRefused = !std::is_constructible_v<keygrant::Entitlement, T> && !std::is_convertible_v<T, keygrant::Entitlement>;

enum PlainEnum { kPlainEnum };
enum class ScopedEnum { kScopedEnum };

static_assert(kMade<decltype("pro")> && kMade<const char*> && kMade<std::string> && kMade<int> && kMade<double> &&
                  kMade<bool>,
              "a narrow literal, a number and a flag are entitlements");
static_assert(kRefused<char> && kRefused<wchar_t> && kRefused<char16_t> && kRefused<char32_t>,
              "a character is no entitlement");
static_assert(kRefused<decltype(L"pro")> && kRefused<decltype(u"pro")> && kRefused<decltype(U"pro")>,
              "a wide or UTF literal is no entitlement");
static_assert(kRefused<PlainEnum> && kRefused<ScopedEnum> && kRefused<std::nullptr_t>,
              "an enum or a null pointer is no entitlement");
#if defined(__cpp_char8_t)
static_assert(kRefused<char8_t> && kRefused<decltype(u8"pro")> && kRefused<const char8_t*>,
              "a C++20 u8 literal is no entitlement");
#endif

}  // namespace
