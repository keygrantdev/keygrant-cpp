// The adapters: all platform I/O the SDK does lives behind these interfaces,
// each with a default. An app replaces any of them, and tests replace all of
// them.
//
// Adapters may be called from more than one thread: a call behind one that is
// stuck in an adapter runs read-only beside it, and the SDK bounds the HTTP
// adapter and the fingerprinter by running them on a thread of their own,
// abandoned (left to finish) when they outlive their bound. The defaults are
// all safe to call so.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "keygrant/types.hpp"

namespace keygrant {

/** Where the state is kept. */
class StorageAdapter {
 public:
  virtual ~StorageAdapter() = default;

  /**
   * The stored state, or std::nullopt for none. Throw when it cannot be read
   * just now (only a MISSING state is none: a call built on a read that failed
   * would write what it read over the real state). `readOnly` (a call behind
   * one that has not finished): read and write nothing, not even to repair.
   */
  virtual std::optional<StoredState> read(bool readOnly) = 0;

  /** Save a whole state. Throw when it could not be saved. */
  virtual void write(const StoredState& state) = 0;

  /**
   * Optional: save `merge` of the state as stored at the moment of saving,
   * merged again onto a newer one when another process saved in between, and
   * return true. The default returns false, and the SDK then reads, merges and
   * writes itself.
   */
  virtual bool update(const std::function<StoredState(const StoredState&)>& merge) {
    (void)merge;
    return false;
  }
};

/** Survives app uninstall (the Windows registry, say), to keep a trial's start. */
class RegistryStash {
 public:
  virtual ~RegistryStash() = default;
  virtual std::optional<std::int64_t> readTrialStart() = 0;
  virtual void writeTrialStart(std::int64_t ms) = 0;
};

/** The wall clock, in milliseconds since the epoch. */
class Clock {
 public:
  virtual ~Clock() = default;
  virtual std::int64_t now() = 0;
};

/** Who this device is, as far as can be told this time (Fingerprinter::identify). */
struct DeviceIdentity {
  /// What a normal run gives, or std::nullopt when it cannot be told this time.
  std::optional<std::string> fingerprint;
  /// What `fingerprint` hashes. Only a seat keyed by a machine id is held to its device.
  std::optional<DeviceKind> kind;
  /// Other fingerprints this device answers to (such as the host-name
  /// fingerprint).
  std::vector<std::string> alternates;

  bool operator==(const DeviceIdentity& other) const {
    return fingerprint == other.fingerprint && kind == other.kind && alternates == other.alternates;
  }
};

/** Who this device is. */
class Fingerprinter {
 public:
  virtual ~Fingerprinter() = default;

  /**
   * This device's fingerprint. Throw when it cannot be read this time: that
   * is "cannot tell", never a stand-in. A fingerprinter with only get() says
   * nothing of what its fingerprint hashes, so its seats are NEVER held to
   * their device: implement identify() and report DeviceKind::Machine for a
   * stable machine id that should be.
   */
  virtual std::string get() = 0;

  /**
   * Optional, and what the SDK uses when given: this device's fingerprint
   * (none when it cannot be read this time), what it hashes, and any other
   * fingerprints this device has been activated under. Return std::nullopt
   * (the default) to have the SDK use get() instead. A wrapper around the
   * default fingerprinter should delegate this, to keep its kind and its
   * host-name alternate.
   */
  virtual std::optional<DeviceIdentity> identify(std::chrono::milliseconds timeout) {
    (void)timeout;
    return std::nullopt;
  }
};

/** An HTTP answer: its status and its body, whatever the status. */
struct HttpResponse {
  int status = 0;
  std::string body;
};

/** A minimal HTTP surface: one JSON POST. */
class HttpAdapter {
 public:
  virtual ~HttpAdapter() = default;

  /**
   * POST `jsonBody` (`Content-Type: application/json`) to `url`, and return
   * the status and the body whatever the status: a 4xx or a 5xx is an answer.
   * Throw when there is no answer at all (no network, a TLS failure, the
   * time running out). `timeout` is how long the call may take; the SDK holds
   * it to that bound itself as well, so an adapter that ignores it cannot
   * hold a call open.
   */
  virtual HttpResponse post(const std::string& url, const std::string& jsonBody, std::chrono::milliseconds timeout) = 0;
};

/** The adapters a License uses. Any left empty is the default. */
struct Adapters {
  /// Default: FileStorage at defaultStoragePath(product).
  std::shared_ptr<StorageAdapter> storage;
  /// Default: NoopRegistryStash.
  std::shared_ptr<RegistryStash> registry;
  /// Default: SystemClock.
  std::shared_ptr<Clock> clock;
  /// Default: SystemFingerprinter (the machine id, else the host name).
  std::shared_ptr<Fingerprinter> fingerprint;
  /// Default: CurlHttp.
  std::shared_ptr<HttpAdapter> http;
};

/** The system's wall clock. */
class SystemClock final : public Clock {
 public:
  std::int64_t now() override;
};

/** The default registry stash: keeps nothing. */
class NoopRegistryStash final : public RegistryStash {
 public:
  std::optional<std::int64_t> readTrialStart() override { return std::nullopt; }
  void writeTrialStart(std::int64_t ms) override { (void)ms; }
};

#ifdef _WIN32
namespace detail {
/**
 * The key under HKEY_CURRENT_USER that the trial stash keeps each product's
 * key below. Only the SDK's own tests give another (one of their own, so they
 * never touch a real app's stash); not a stable API.
 */
struct StashRoot {
  std::string subkey = "Software\\KeyGrant";
};
}  // namespace detail

/**
 * Opt-in (Windows): a trial's start kept in the user's registry, where an
 * uninstall that removes the app's own files and keys leaves it, in the one
 * format every KeyGrant SDK shares, so a trial one SDK stashed is seen by
 * another for the same product: HKEY_CURRENT_USER\Software\KeyGrant\<product>,
 * value `TrialStart`, the start in ms since the epoch as a REG_QWORD.
 *
 * Reads a REG_QWORD, or a REG_SZ of decimal digits (what a script or a hand
 * writes); anything else is no start: another type, 0, or 2^53 or more.
 * Best-effort: a read that fails is no start, and a write that fails is
 * dropped; neither throws.
 */
class WindowsRegistryStash final : public RegistryStash {
 public:
  /// The stash for this product (its slug, as LicenseConfig::product). Throws std::invalid_argument for one that is not a slug.
  explicit WindowsRegistryStash(const std::string& product);
  /// The same stash below another root (the SDK's tests); not a stable API.
  WindowsRegistryStash(const std::string& product, const detail::StashRoot& root);
  std::optional<std::int64_t> readTrialStart() override;
  void writeTrialStart(std::int64_t ms) override;
  /// The key the start is kept under, below HKEY_CURRENT_USER (UTF-8). Making the stash writes nothing.
  std::string key() const { return keyUtf8_; }

 private:
  std::string keyUtf8_;
  std::wstring key_;
};
#endif

/** Options for CurlHttp. */
struct CurlHttpOptions {
  /// Sent as `User-Agent`.
  std::string userAgent = "keygrant-cpp/0.7";
  /// A body larger than this is no answer (a portal page, a hostile endpoint).
  std::size_t maxBodyBytes = 1024 * 1024;
  /// A CA bundle to verify TLS with, instead of the system's.
  std::optional<std::string> caInfo;
};

/**
 * HTTP over libcurl, every call bounded by its `timeout`, redirects included.
 * TLS is verified against the system's CAs (or `caInfo`): an answer over a
 * connection that cannot be verified is no answer. A 307 or 308 is followed
 * as fetch follows it, with the method and the body, to https only and at
 * most 20 times; any other 3xx is the answer as it is.
 *
 * Safe to call from several threads. libcurl's global set-up runs once, when
 * the first CurlHttp is made, on the thread making it (never on a worker of
 * the SDK's), and is never undone (a call abandoned at its bound may still be
 * running). Before libcurl 7.84 that set-up is not thread-safe: with an older
 * libcurl, call curl_global_init(CURL_GLOBAL_DEFAULT) in main() before the app
 * starts any thread.
 */
class CurlHttp final : public HttpAdapter {
 public:
  CurlHttp();
  explicit CurlHttp(CurlHttpOptions options);
  HttpResponse post(const std::string& url, const std::string& jsonBody, std::chrono::milliseconds timeout) override;

 private:
  CurlHttpOptions options_;
};

}  // namespace keygrant
