// The one piece of URL handling the SDK needs: a payment link with a reference
// as its client_reference_id, which upgradeUrl() and purchaseUrl() both build.
#pragma once

#include <optional>
#include <string>

namespace keygrant::detail {

/** A payment link read as an absolute URL: everything before its query, its query, and its fragment. */
struct Link {
  /// The scheme, `:` and the rest up to the query, the scheme and an http(s) host lower-cased.
  std::string base;
  /// The text after `?`, as written; none when there is no `?`.
  std::optional<std::string> query;
  /// The text after `#`, as written; none when there is no `#`.
  std::optional<std::string> fragment;
};

/**
 * `link` read as an absolute URL: a scheme, `:` and the rest, an http(s) URL
 * with `//` and a host (leading and trailing white space and control
 * characters dropped, and tabs and newlines, as the WHATWG parser drops them).
 * Throws std::invalid_argument for anything else: a path such as `/buy`, or a
 * bare host such as `buy.stripe.com/x`.
 */
Link absoluteLink(const std::string& link);

/**
 * `link` with `reference` as its one `client_reference_id`. The link is read
 * as an absolute URL (absoluteLink), and its query is then edited as text:
 * split on `&`, every empty part and every part whose name (its text before
 * the first `=`, or all of it) is exactly `client_reference_id` is left out,
 * the others are kept as written and in their order (with the characters the
 * WHATWG parser percent-encodes in a query encoded), and
 * `client_reference_id=<reference>` (percent-encoded as encodeURIComponent
 * encodes it, and its `'` too in an http(s) URL, as setting a URL's search
 * writes it) is added last. The fragment is kept after the query, with the
 * characters the WHATWG parser percent-encodes in a fragment encoded. Throws
 * std::invalid_argument as absoluteLink does.
 */
std::string referencedLink(const std::string& link, const std::string& reference);

}  // namespace keygrant::detail
