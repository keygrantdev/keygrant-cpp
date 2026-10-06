// Path pieces for the storage file, by the separators of the platform built for.
#pragma once

#include <string>

namespace keygrant::detail {

#ifdef _WIN32
inline constexpr char kSeparator = '\\';
inline bool isSeparator(char c) { return c == '\\' || c == '/'; }
#else
inline constexpr char kSeparator = '/';
inline bool isSeparator(char c) { return c == '/'; }
#endif

/** The folder a path is in (Node's path.dirname). */
inline std::string dirOf(const std::string& path) {
  std::size_t cut = std::string::npos;
  for (std::size_t i = path.size(); i-- > 0;) {
    if (isSeparator(path[i])) {
      cut = i;
      break;
    }
  }
  if (cut == std::string::npos) return ".";
  if (cut == 0) return path.substr(0, 1);
#ifdef _WIN32
  if (cut == 2 && path[1] == ':') return path.substr(0, 3);
#endif
  return path.substr(0, cut);
}

/** The last part of a path (Node's path.basename). */
inline std::string baseOf(const std::string& path) {
  for (std::size_t i = path.size(); i-- > 0;) {
    if (isSeparator(path[i])) return path.substr(i + 1);
  }
  return path;
}

/** A name in a folder. */
inline std::string joinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  return isSeparator(dir.back()) ? dir + name : dir + kSeparator + name;
}

}  // namespace keygrant::detail
