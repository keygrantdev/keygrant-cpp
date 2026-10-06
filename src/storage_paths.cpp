#include <initializer_list>
#include <stdexcept>
#include <string>

#include "keygrant/fingerprint.hpp"
#include "keygrant/storage.hpp"
#include "sys.hpp"

namespace keygrant {

namespace {

bool separator(char c, bool windows) { return c == '/' || (windows && c == '\\'); }

/** Node's path.win32.isAbsolute: a leading separator, or a drive letter, a colon and a separator. */
bool win32Absolute(const std::string& path) {
  if (path.empty()) return false;
  if (separator(path[0], true)) return true;
  const char drive = path[0];
  const bool letter = (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
  return path.size() > 2 && letter && path[1] == ':' && separator(path[2], true);
}

bool posixAbsolute(const std::string& path) { return !path.empty() && path[0] == '/'; }

/** Parts joined by the platform's separator, repeated separators collapsed (a leading UNC pair kept). */
std::string join(std::initializer_list<std::string> parts, bool windows) {
  const char sep = windows ? '\\' : '/';
  std::string joined;
  for (const std::string& part : parts) {
    if (part.empty()) continue;
    if (!joined.empty()) joined.push_back(sep);
    joined += part;
  }
  std::string out;
  for (std::size_t i = 0; i < joined.size(); ++i) {
    const bool isSep = separator(joined[i], windows);
    const char c = isSep ? sep : joined[i];
    const bool uncLead = windows && i == 1 && !out.empty() && out[0] == sep;
    if (isSep && !out.empty() && out.back() == sep && !uncLead) continue;
    out.push_back(c);
  }
  return out;
}

std::string envOf(const SystemPaths& system, const char* name) {
  const auto it = system.env.find(name);
  return it == system.env.end() ? std::string() : it->second;
}

/// Why there is no default storage: never a path relative to the working directory.
constexpr const char* kNoHome =
    "no home folder to keep the licence state in (HOME/USERPROFILE unset, empty or relative): give "
    "LicenseConfig::adapters.storage a FileStorage at a path of your own";

}  // namespace

std::string defaultStoragePath(const std::string& product, const SystemPaths& system) {
  const std::string file = product + ".json";
  if (system.platform == "win32") {
    const std::string localAppData = envOf(system, "LOCALAPPDATA");
    if (win32Absolute(localAppData)) return join({localAppData, "KeyGrant", file}, true);
    if (!win32Absolute(system.homedir)) throw std::invalid_argument(kNoHome);
    return join({system.homedir, "AppData", "Local", "KeyGrant", file}, true);
  }
  if (system.platform == "darwin") {
    if (!posixAbsolute(system.homedir)) throw std::invalid_argument(kNoHome);
    return join({system.homedir, "Library", "Application Support", "KeyGrant", file}, false);
  }
  const std::string xdg = envOf(system, "XDG_CONFIG_HOME");
  if (posixAbsolute(xdg)) return join({xdg, "keygrant", file}, false);
  if (!posixAbsolute(system.homedir)) throw std::invalid_argument(kNoHome);
  return join({system.homedir, ".config", "keygrant", file}, false);
}

std::string defaultStoragePath(const std::string& product) { return defaultStoragePath(product, SystemPaths::current()); }

SystemPaths SystemPaths::current() {
  SystemPaths system;
  system.platform = platformName();
  system.homedir = detail::homeDirectory();
  for (const char* name : {"LOCALAPPDATA", "XDG_CONFIG_HOME"}) {
    if (const auto value = detail::environment(name)) system.env[name] = *value;
  }
#ifdef _WIN32
  // Where the environment does not say (a service, a stripped environment), the shell does.
  if (system.env.find("LOCALAPPDATA") == system.env.end()) {
    if (const auto local = detail::localAppDataFolder()) system.env["LOCALAPPDATA"] = *local;
  }
#endif
  return system;
}

}  // namespace keygrant
