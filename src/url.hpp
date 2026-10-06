// The one piece of URL handling the SDK needs: a payment link with a query
// parameter set, as `new URL(link).searchParams.set(name, value)` sets it.
#pragma once

#include <string>

namespace keygrant::detail {

/**
 * `url` with the query parameter `name` set to `value`: the first one of that
 * name takes the value and any others are removed, or it is appended. The
 * query is re-serialized as URLSearchParams serializes it
 * (application/x-www-form-urlencoded); the fragment is kept. Throws
 * std::invalid_argument for a URL with no scheme, or an http(s) URL with no host.
 */
std::string withQueryParam(const std::string& url, const std::string& name, const std::string& value);

}  // namespace keygrant::detail
