// libcurl's global set-up, as CurlHttp makes it (http_curl.cpp).
#pragma once

#include <optional>
#include <thread>

namespace keygrant::detail {

/// The thread libcurl's global set-up ran on, once a CurlHttp has been made; none before.
std::optional<std::thread::id> curlGlobalInitThread();

}  // namespace keygrant::detail
