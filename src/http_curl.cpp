// HTTP over libcurl, every call bounded (sdk/src/adapters.ts fetchHttp).
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "curl_global.hpp"
#include "keygrant/adapters.hpp"
#include "utf.hpp"

namespace keygrant {

namespace detail {

namespace {

/** Whether libcurl's global set-up has run, where, and the lock it runs under. */
struct CurlGlobal {
  std::mutex mutex;
  bool done = false;
  std::thread::id thread;
};

CurlGlobal& curlGlobal() {
  // Never destroyed: a call abandoned at its bound may still be running at exit.
  static auto* global = new CurlGlobal();
  return *global;
}

/**
 * libcurl's global set-up, once for the process and never undone (a call
 * abandoned at its bound may still be running at exit). Run by the first
 * CurlHttp made, on the thread that makes it (the app's, building its
 * License), never on a worker thread of the SDK's beside the app's own:
 * before libcurl 7.84 it is not thread-safe. A set-up that failed is tried
 * again by the next call.
 */
bool ensureCurlGlobal() {
  CurlGlobal& global = curlGlobal();
  std::lock_guard<std::mutex> lock(global.mutex);
  if (!global.done && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
    global.done = true;
    global.thread = std::this_thread::get_id();
  }
  return global.done;
}

}  // namespace

std::optional<std::thread::id> curlGlobalInitThread() {
  CurlGlobal& global = curlGlobal();
  std::lock_guard<std::mutex> lock(global.mutex);
  if (!global.done) return std::nullopt;
  return global.thread;
}

}  // namespace detail

namespace {

/// How many redirects one request follows, as fetch does.
constexpr int kMaxRedirects = 20;

/** The body so far, and the most it may grow to. */
struct Sink {
  std::string body;
  std::size_t limit = 0;
};

std::size_t collect(char* data, std::size_t size, std::size_t count, void* user) {
  auto* sink = static_cast<Sink*>(user);
  const std::size_t bytes = size * count;
  // Too large to be an answer (a portal page, a hostile endpoint): abort, which is no answer.
  if (sink->body.size() + bytes > sink->limit) return 0;
  sink->body.append(data, bytes);
  return bytes;
}

struct EasyDeleter {
  void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};

struct ListDeleter {
  void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

void check(CURLcode code, const char* what) {
  if (code != CURLE_OK) throw std::runtime_error(std::string(what) + ": " + curl_easy_strerror(code));
}

/** Whether a redirect may lead here: https only, so no redirect downgrades the call. */
bool isHttps(const std::string& url) { return url.size() > 8 && detail::asciiLower(url.substr(0, 8)) == "https://"; }

}  // namespace

CurlHttp::CurlHttp() : CurlHttp(CurlHttpOptions{}) {}

CurlHttp::CurlHttp(CurlHttpOptions options) : options_(std::move(options)) {
  // Here, on the app's thread, not lazily on the worker a call runs on.
  (void)detail::ensureCurlGlobal();
}

HttpResponse CurlHttp::post(const std::string& url, const std::string& jsonBody, std::chrono::milliseconds timeout) {
  if (!detail::ensureCurlGlobal()) throw std::runtime_error("curl_global_init failed");
  const std::unique_ptr<CURL, EasyDeleter> handle(curl_easy_init());
  if (!handle) throw std::runtime_error("curl_easy_init failed");
  curl_slist* raw = curl_slist_append(nullptr, "Content-Type: application/json");
  if (raw != nullptr) raw = curl_slist_append(raw, "Accept: application/json");
  const std::unique_ptr<curl_slist, ListDeleter> headers(raw);
  if (!headers) throw std::runtime_error("curl_slist_append failed");

  // One bound for the whole call, redirects included.
  const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{1});
  Sink sink;
  sink.limit = options_.maxBodyBytes;
  CURL* h = handle.get();
  check(curl_easy_setopt(h, CURLOPT_POST, 1L), "post");
  check(curl_easy_setopt(h, CURLOPT_POSTFIELDS, jsonBody.c_str()), "body");
  check(curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(jsonBody.size())), "body size");
  check(curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.get()), "headers");
  check(curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect), "write function");
  check(curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink), "write data");
  // No signals from a resolver timeout: safe on any thread.
  check(curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L), "nosignal");
  // Redirects are followed here, not by libcurl, which would follow every 3xx.
  check(curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L), "redirects");
  check(curl_easy_setopt(h, CURLOPT_USERAGENT, options_.userAgent.c_str()), "user agent");
#if LIBCURL_VERSION_NUM >= 0x075500
  check(curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https"), "protocols");
#else
  check(curl_easy_setopt(h, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS)), "protocols");
#endif
  if (options_.caInfo) check(curl_easy_setopt(h, CURLOPT_CAINFO, options_.caInfo->c_str()), "ca bundle");
  // The chain is verified, but revocation is not checked, as fetch does not
  // check it: Schannel (libcurl on Windows) would otherwise refuse every
  // certificate whose revocation status it cannot find (a CRL host the network
  // does not let through, a TLS-inspecting proxy's leaves), and no activation
  // or renewal would ever be answered there. Other TLS backends ignore it.
  check(curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NO_REVOKE)), "ssl options");

  std::string target = url;
  for (int redirects = 0;; ++redirects) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) throw std::runtime_error("request: out of time");
    check(curl_easy_setopt(h, CURLOPT_URL, target.c_str()), "url");
    check(curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(left)), "timeout");
    check(curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(left)), "connect timeout");
    sink.body.clear();
    check(curl_easy_perform(h), "request");
    long status = 0;
    check(curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status), "status");
    // A 307 or a 308 is followed with the method and the body, as fetch does,
    // and only to https. Every other answer, a 301, 302 or 303 included, is
    // the answer as it is.
    if (status != 307 && status != 308) return HttpResponse{static_cast<int>(status), std::move(sink.body)};
    char* next = nullptr;
    check(curl_easy_getinfo(h, CURLINFO_REDIRECT_URL, &next), "redirect");
    if (next == nullptr) return HttpResponse{static_cast<int>(status), std::move(sink.body)};
    if (!isHttps(next)) throw std::runtime_error("request: redirected to a URL that is not https");
    if (redirects >= kMaxRedirects) throw std::runtime_error("request: too many redirects");
    target = next;
  }
}

}  // namespace keygrant
