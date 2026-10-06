// The libcurl adapter against servers on this machine, plain and TLS, on every
// platform (sdk/src/adapters.test.ts fetchHttp): the status and body whatever
// the status, TLS verified, redirects as fetch follows them, every call bounded.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <thread>

#include "curl_global.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

// --- sockets, on POSIX and Winsock alike ----------------------------------------------

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
#else
using Socket = int;
const Socket kNoSocket = -1;
#endif

void socketsUp() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
  });
#endif
}

void closeSocket(Socket s) {
#ifdef _WIN32
  ::closesocket(s);
#else
  ::close(s);
#endif
}

void shutdownSocket(Socket s) {
#ifdef _WIN32
  ::shutdown(s, SD_BOTH);
#else
  ::shutdown(s, SHUT_RDWR);
#endif
}

void receiveTimeout(Socket s, int seconds) {
#ifdef _WIN32
  const DWORD ms = static_cast<DWORD>(seconds) * 1000;
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
  timeval wait{seconds, 0};
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
#ifdef SO_NOSIGPIPE
  int yes = 1;
  ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
#endif
}

long receive(Socket s, char* buffer, std::size_t size) {
#ifdef _WIN32
  return ::recv(s, buffer, static_cast<int>(size), 0);
#else
  return static_cast<long>(::recv(s, buffer, size, 0));
#endif
}

long transmit(Socket s, const char* data, std::size_t size) {
#ifdef _WIN32
  return ::send(s, data, static_cast<int>(size), 0);
#elif defined(MSG_NOSIGNAL)
  return static_cast<long>(::send(s, data, size, MSG_NOSIGNAL));
#else
  return static_cast<long>(::send(s, data, size, 0));
#endif
}

// --- TLS identities no system trusts ------------------------------------------------------

/** A certificate for `cn`, signed by `issuerKey` as `issuer` (itself, when none), with these extensions. */
X509* makeCertificate(EVP_PKEY* key, const char* cn, X509* issuer, EVP_PKEY* issuerKey, long serial,
                      const std::vector<std::pair<int, std::string>>& extensions) {
  X509* cert = X509_new();
  if (cert == nullptr) throw std::runtime_error("cannot make a test certificate");
  X509_set_version(cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);
  X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
  X509_gmtime_adj(X509_getm_notAfter(cert), 7 * 24 * 3600);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
  X509_set_issuer_name(cert, issuer != nullptr ? X509_get_subject_name(issuer) : name);
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer != nullptr ? issuer : cert, cert, nullptr, nullptr, 0);
  for (const auto& [nid, value] : extensions) {
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
    if (extension == nullptr) throw std::runtime_error("cannot make a test certificate's extension");
    X509_add_ext(cert, extension, -1);
    X509_EXTENSION_free(extension);
  }
  if (X509_sign(cert, issuerKey != nullptr ? issuerKey : key, EVP_sha256()) == 0) {
    throw std::runtime_error("cannot sign a test certificate");
  }
  return cert;
}

/**
 * A server certificate for localhost and 127.0.0.1, the key it serves with,
 * and a CA file that trusts it (CurlHttpOptions::caInfo): self-signed, or a
 * leaf a test CA issued, which carries no revocation information at all, or
 * names a CRL at a URL nothing answers (a network that lets the API through
 * but not the CA's hosts, or a TLS-inspecting proxy's leaves).
 */
class TlsIdentity {
 public:
  enum class Kind { SelfSigned, IssuedNoRevocationInfo, IssuedUnreachableCrl };

  explicit TlsIdentity(Kind kind) {
    key_ = EVP_EC_gen("P-256");
    if (key_ == nullptr) throw std::runtime_error("cannot make a test key");
    const std::string altNames = "DNS:localhost,IP:127.0.0.1";
    if (kind == Kind::SelfSigned) {
      cert_ = makeCertificate(key_, "localhost", nullptr, nullptr, 1,
                              {{NID_basic_constraints, "critical,CA:TRUE"},
                               {NID_key_usage, "critical,digitalSignature,keyCertSign"},
                               {NID_ext_key_usage, "serverAuth"},
                               {NID_subject_key_identifier, "hash"},
                               {NID_subject_alt_name, altNames}});
      trusted_ = cert_;
    } else {
      caKey_ = EVP_EC_gen("P-256");
      if (caKey_ == nullptr) throw std::runtime_error("cannot make a test key");
      ca_ = makeCertificate(caKey_, "KeyGrant test CA", nullptr, nullptr, 1,
                            {{NID_basic_constraints, "critical,CA:TRUE"},
                             {NID_key_usage, "critical,keyCertSign,cRLSign"},
                             {NID_subject_key_identifier, "hash"}});
      std::vector<std::pair<int, std::string>> leaf = {{NID_basic_constraints, "critical,CA:FALSE"},
                                                       {NID_key_usage, "critical,digitalSignature"},
                                                       {NID_ext_key_usage, "serverAuth"},
                                                       {NID_subject_key_identifier, "hash"},
                                                       {NID_authority_key_identifier, "keyid:always"},
                                                       {NID_subject_alt_name, altNames}};
      // Port 1 on this machine: nothing answers there.
      if (kind == Kind::IssuedUnreachableCrl) leaf.emplace_back(NID_crl_distribution_points, "URI:http://127.0.0.1:1/keygrant-test.crl");
      cert_ = makeCertificate(key_, "localhost", ca_, caKey_, 2, leaf);
      trusted_ = ca_;
    }

    std::random_device random;
    caFile_ = (std::filesystem::temp_directory_path() /
               ("keygrant-test-ca-" + std::to_string(random()) + std::to_string(random()) + ".pem"))
                  .string();
    BIO* file = BIO_new_file(caFile_.c_str(), "w");
    if (file == nullptr) throw std::runtime_error("cannot write the test CA");
    const int written = PEM_write_bio_X509(file, trusted_);
    BIO_free(file);
    if (written != 1) throw std::runtime_error("cannot write the test CA");

    context_ = SSL_CTX_new(TLS_server_method());
    if (context_ == nullptr || SSL_CTX_use_certificate(context_, cert_) != 1 ||
        SSL_CTX_use_PrivateKey(context_, key_) != 1) {
      throw std::runtime_error("cannot make a TLS server context");
    }
    // The whole chain is sent, as a real server sends it.
    if (ca_ != nullptr && SSL_CTX_add1_chain_cert(context_, ca_) != 1) {
      throw std::runtime_error("cannot add the test CA to the chain");
    }
  }
  ~TlsIdentity() {
    SSL_CTX_free(context_);
    X509_free(cert_);
    X509_free(ca_);
    EVP_PKEY_free(key_);
    EVP_PKEY_free(caKey_);
    std::error_code ignored;
    std::filesystem::remove(caFile_, ignored);
  }
  TlsIdentity(const TlsIdentity&) = delete;
  TlsIdentity& operator=(const TlsIdentity&) = delete;

  SSL_CTX* context() const { return context_; }
  /// A PEM file that trusts this identity (CurlHttpOptions::caInfo).
  const std::string& caFile() const { return caFile_; }

 private:
  EVP_PKEY* key_ = nullptr;
  X509* cert_ = nullptr;
  EVP_PKEY* caKey_ = nullptr;
  X509* ca_ = nullptr;
  X509* trusted_ = nullptr;
  SSL_CTX* context_ = nullptr;
  std::string caFile_;
};

/** The self-signed identity most tests serve. */
const TlsIdentity& tls() {
  static const TlsIdentity identity(TlsIdentity::Kind::SelfSigned);
  return identity;
}

const TlsIdentity& issuedNoRevocationInfo() {
  static const TlsIdentity identity(TlsIdentity::Kind::IssuedNoRevocationInfo);
  return identity;
}

const TlsIdentity& issuedUnreachableCrl() {
  static const TlsIdentity identity(TlsIdentity::Kind::IssuedUnreachableCrl);
  return identity;
}

// --- the server ---------------------------------------------------------------------------

/** One connection's reads and writes: over the socket, or over TLS on it. */
class Connection {
 public:
  Connection(Socket socket, SSL* ssl) : socket_(socket), ssl_(ssl) {}
  long read(char* buffer, std::size_t size) {
    if (ssl_ == nullptr) return receive(socket_, buffer, size);
    return SSL_read(ssl_, buffer, static_cast<int>(size));
  }
  void write(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const long n = ssl_ == nullptr ? transmit(socket_, data.data() + sent, data.size() - sent)
                                     : SSL_write(ssl_, data.data() + sent, static_cast<int>(data.size() - sent));
      if (n <= 0) return;
      sent += static_cast<std::size_t>(n);
    }
  }

 private:
  Socket socket_;
  SSL* ssl_;
};

/**
 * One-request-at-a-time HTTP on 127.0.0.1, or HTTPS on localhost with a test
 * identity: the handler gives the whole raw response, or "" to never answer.
 */
class LocalServer {
 public:
  using Handler = std::function<std::string(const std::string& request)>;

  explicit LocalServer(Handler handler, const TlsIdentity* identity = nullptr)
      : handler_(std::move(handler)), identity_(identity) {
    socketsUp();
    listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_ == kNoSocket) throw std::runtime_error("cannot open a socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(listen_, 8) != 0) {
      closeSocket(listen_);
      throw std::runtime_error("cannot listen on 127.0.0.1");
    }
    socklen_t length = sizeof address;
    ::getsockname(listen_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { serve(); });
  }

  ~LocalServer() {
    stopping_ = true;
    gate_.open();
    shutdownSocket(listen_);
    closeSocket(listen_);
    thread_.join();
  }

  LocalServer(const LocalServer&) = delete;
  LocalServer& operator=(const LocalServer&) = delete;

  int port() const { return port_; }

  std::string url(const std::string& path = "/v1/products/sluice/validate") const {
    return (identity_ != nullptr ? "https://localhost:" : "http://127.0.0.1:") + std::to_string(port_) + path;
  }

  std::string lastRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.empty() ? std::string() : requests_.back();
  }

  int requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(requests_.size());
  }

 private:
  void serve() {
    for (;;) {
      const Socket client = ::accept(listen_, nullptr, nullptr);
      if (client == kNoSocket) return;
      receiveTimeout(client, 5);
      SSL* ssl = nullptr;
      if (identity_ != nullptr) {
        ssl = SSL_new(identity_->context());
        SSL_set_fd(ssl, static_cast<int>(client));
        if (SSL_accept(ssl) <= 0) {
          // The client would not trust it, or went away: no request was made.
          ERR_clear_error();
          SSL_free(ssl);
          closeSocket(client);
          if (stopping_) return;
          continue;
        }
      }
      Connection connection(client, ssl);
      const std::string request = readRequest(connection);
      if (request.empty()) {
        // The client went away without sending a byte: no request was made either. A client that checks the
        // certificate once the handshake is done refuses this way, after SSL_accept has succeeded here
        // (libcurl 8.17 over OpenSSL 3.6 on macOS); one that checks during the handshake fails SSL_accept above.
        if (ssl != nullptr) {
          SSL_free(ssl);
          ERR_clear_error();
        }
        closeSocket(client);
        if (stopping_) return;
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
      }
      const std::string answer = handler_(request);
      if (answer.empty()) {
        gate_.wait();
      } else {
        connection.write(answer);
      }
      if (ssl != nullptr) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ERR_clear_error();
      }
      closeSocket(client);
      if (stopping_) return;
    }
  }

  static std::string readRequest(Connection& connection) {
    std::string request;
    char buffer[4096];
    std::size_t bodyAt = std::string::npos;
    std::size_t length = 0;
    for (;;) {
      if (bodyAt != std::string::npos && request.size() >= bodyAt + length) return request;
      const long got = connection.read(buffer, sizeof buffer);
      if (got <= 0) return request;
      request.append(buffer, static_cast<std::size_t>(got));
      if (bodyAt == std::string::npos) {
        const std::size_t end = request.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        bodyAt = end + 4;
        std::string headers = request.substr(0, end);
        for (char& c : headers) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::size_t at = headers.find("content-length:");
        if (at != std::string::npos) length = std::stoul(headers.substr(at + 15));
      }
    }
  }

  Handler handler_;
  const TlsIdentity* identity_ = nullptr;
  Socket listen_ = kNoSocket;
  int port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::vector<std::string> requests_;
  Gate gate_;
};

std::string response(int status, const std::string& body, const std::string& extraHeaders = "") {
  return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
}

/** A request's first line, and its body. */
std::string requestLine(const std::string& request) { return request.substr(0, request.find("\r\n")); }
std::string bodyOf(const std::string& request) {
  const std::size_t at = request.find("\r\n\r\n");
  return at == std::string::npos ? std::string() : request.substr(at + 4);
}

/** A CurlHttp that trusts the test identity. */
keygrant::CurlHttp trusting() {
  keygrant::CurlHttpOptions options;
  options.caInfo = tls().caFile();
  return keygrant::CurlHttp(options);
}

TEST(CurlHttp, ReturnsTheStatusAndTheBodyWhateverTheStatus) {
  LocalServer server([](const std::string&) { return response(403, R"({"error":"revoked"})"); });
  const keygrant::HttpResponse res = keygrant::CurlHttp().post(server.url(), R"({"key":"K"})", Millis{5'000});
  EXPECT_EQ(res.status, 403);
  EXPECT_EQ(res.body, R"({"error":"revoked"})");
}

TEST(CurlHttp, PostsTheJsonBodyAsJson) {
  LocalServer server([](const std::string&) { return response(200, "{}"); });
  keygrant::CurlHttp().post(server.url(), R"({"key":"K","major":3})", Millis{5'000});
  const std::string request = server.lastRequest();
  EXPECT_EQ(requestLine(request), "POST /v1/products/sluice/validate HTTP/1.1") << request;
  EXPECT_NE(request.find("Content-Type: application/json\r\n"), std::string::npos) << request;
  EXPECT_EQ(bodyOf(request), R"({"key":"K","major":3})");
}

TEST(CurlHttp, AbandonsACallThatOutlivesItsBound) {
  LocalServer server([](const std::string&) { return std::string(); });
  const Millis took = timed([&] {
    EXPECT_THROW(keygrant::CurlHttp().post(server.url(), "{}", Millis{300}), std::runtime_error);
  });
  EXPECT_GE(took, Millis{250});
  EXPECT_LT(took, Millis{3'000});
}

TEST(CurlHttp, IsNoAnswerWhenNothingListens) {
  int port = 0;
  {
    LocalServer gone([](const std::string&) { return response(200, "{}"); });
    port = gone.port();
  }
  EXPECT_THROW(keygrant::CurlHttp().post("http://127.0.0.1:" + std::to_string(port) + "/x", "{}", Millis{2'000}),
               std::runtime_error);
}

TEST(CurlHttp, RefusesABodyLargerThanItsBound) {
  LocalServer server([](const std::string&) { return response(200, std::string(4096, 'x')); });
  keygrant::CurlHttpOptions options;
  options.maxBodyBytes = 1024;
  EXPECT_THROW(keygrant::CurlHttp(options).post(server.url(), "{}", Millis{5'000}), std::runtime_error);
}

TEST(CurlHttp, IsNoAnswerOverAConnectionItCannotVerify) {
  // A refusal is unsigned JSON: only TLS stops a network attacker forging
  // {"error":"revoked"}. A certificate no system trusts is no answer at all.
  LocalServer server([](const std::string&) { return response(403, R"({"error":"revoked"})"); }, &tls());
  EXPECT_THROW(keygrant::CurlHttp().post(server.url(), "{}", Millis{5'000}), std::runtime_error);
  EXPECT_EQ(server.requests(), 0);
}

TEST(CurlHttp, AnswersWhateverACertificatesRevocationStatusCanBeFound) {
  // Revocation is not checked, as Node's fetch does not check it: a network
  // that lets the API through but not the CA's CRL hosts, or a TLS-inspecting
  // proxy whose leaves carry no revocation information, must not cost every
  // activation and renewal (Schannel would refuse both, strictly, by default).
  for (const TlsIdentity* identity : {&issuedUnreachableCrl(), &issuedNoRevocationInfo()}) {
    LocalServer server([](const std::string&) { return response(200, R"({"lease":"h.p.s"})"); }, identity);
    keygrant::CurlHttpOptions options;
    options.caInfo = identity->caFile();
    keygrant::HttpResponse res;
    EXPECT_NO_THROW(res = keygrant::CurlHttp(options).post(server.url(), "{}", Millis{10'000}));
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body, R"({"lease":"h.p.s"})");
    // Still verified: the same leaf from a CA it was not told to trust is no answer.
    EXPECT_THROW(keygrant::CurlHttp().post(server.url(), "{}", Millis{10'000}), std::runtime_error);
  }
}

TEST(CurlHttp, AnswersOverAConnectionVerifiedByItsOwnCaBundle) {
  LocalServer server([](const std::string&) { return response(200, R"({"lease":"h.p.s"})"); }, &tls());
  const keygrant::HttpResponse res = trusting().post(server.url(), R"({"key":"K"})", Millis{5'000});
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.body, R"({"lease":"h.p.s"})");
  EXPECT_EQ(bodyOf(server.lastRequest()), R"({"key":"K"})");
}

TEST(CurlHttp, FollowsA307Or308ToHttpsWithTheMethodAndTheBodyAsFetchDoes) {
  for (const int code : {307, 308}) {
    LocalServer moved([](const std::string&) { return response(200, R"({"lease":"h.p.s"})"); }, &tls());
    const std::string location = moved.url("/v2/products/sluice/validate");
    LocalServer api([code, location](const std::string&) { return response(code, "", "Location: " + location + "\r\n"); },
                    &tls());
    const keygrant::HttpResponse res = trusting().post(api.url(), R"({"key":"K","major":3})", Millis{5'000});
    EXPECT_EQ(res.status, 200) << code;
    EXPECT_EQ(res.body, R"({"lease":"h.p.s"})") << code;
    EXPECT_EQ(requestLine(moved.lastRequest()), "POST /v2/products/sluice/validate HTTP/1.1") << code;
    EXPECT_EQ(bodyOf(moved.lastRequest()), R"({"key":"K","major":3})") << code;
  }
}

TEST(CurlHttp, FollowsARelativeLocationOnTheSameHttpsServer) {
  LocalServer api(
      [](const std::string& request) {
        if (requestLine(request).rfind("POST /v1/moved ", 0) == 0) return response(200, R"({"ok":true})");
        return response(308, "", "Location: /v1/moved\r\n");
      },
      &tls());
  const keygrant::HttpResponse res = trusting().post(api.url(), R"({"key":"K"})", Millis{5'000});
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(api.requests(), 2);
  EXPECT_EQ(bodyOf(api.lastRequest()), R"({"key":"K"})");
}

TEST(CurlHttp, NeverFollowsARedirectThatIsNotHttps) {
  // A 307 to plain http would hand the call to anyone on the network: no answer.
  LocalServer target([](const std::string&) { return response(200, R"({"lease":"h.p.s"})"); });
  const std::string location = target.url();
  LocalServer api([location](const std::string&) { return response(307, "", "Location: " + location + "\r\n"); });
  EXPECT_THROW(keygrant::CurlHttp().post(api.url(), "{}", Millis{5'000}), std::runtime_error);
  EXPECT_EQ(target.requests(), 0);
}

TEST(CurlHttp, AnswersA301302Or303AsItIsFollowingNothing) {
  // fetch would turn the POST into a GET for these: an answer the API never gives.
  LocalServer target([](const std::string&) { return response(200, R"({"lease":"h.p.s"})"); }, &tls());
  const std::string location = target.url();
  for (const int code : {301, 302, 303}) {
    LocalServer api([code, location](const std::string&) { return response(code, "", "Location: " + location + "\r\n"); },
                    &tls());
    EXPECT_EQ(trusting().post(api.url(), "{}", Millis{5'000}).status, code);
  }
  EXPECT_EQ(target.requests(), 0);
}

TEST(CurlHttp, GivesUpAfterTwentyRedirectsAsFetchDoes) {
  LocalServer api([](const std::string&) { return response(307, "", "Location: /v1/again\r\n"); }, &tls());
  EXPECT_THROW(trusting().post(api.url(), "{}", Millis{20'000}), std::runtime_error);
  EXPECT_EQ(api.requests(), 21);
}

TEST(CurlHttp, KeepsOneBoundForTheWholeCallRedirectsIncluded) {
  LocalServer silent([](const std::string&) { return std::string(); }, &tls());
  const std::string location = silent.url();
  LocalServer api([location](const std::string&) { return response(307, "", "Location: " + location + "\r\n"); }, &tls());
  const Millis took = timed([&] { EXPECT_THROW(trusting().post(api.url(), "{}", Millis{500}), std::runtime_error); });
  EXPECT_LT(took, Millis{3'000});
}

TEST(CurlHttp, SetsLibcurlUpOnTheThreadThatMakesItNotOnAWorker) {
  // Before 7.84 curl_global_init is not thread-safe: never run beside the
  // app's threads. Checked in a process of its own, where nothing has set
  // libcurl up yet: making a CurlHttp does it, on the making thread.
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        if (keygrant::detail::curlGlobalInitThread().has_value()) std::exit(2);
        const keygrant::CurlHttp http;
        std::exit(keygrant::detail::curlGlobalInitThread() == std::this_thread::get_id() ? 0 : 1);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(CurlHttp, CarriesALicenceFromActivationToStatus) {
  // The whole path over real HTTP: the URL, the body, the answer, the lease verified.
  Harness h;
  const std::string lease = h.sign(KEY, "perpetual", 30 * 24 * 3600);
  LocalServer server([&](const std::string& request) {
    if (request.find("POST /v1/products/sluice/activate ") != 0) return response(404, R"({"error":"unknown"})");
    return response(200, json({{"lease", lease}, {"activationId", "act_1"}}).dump());
  });
  keygrant::LicenseConfig config = h.config;
  config.apiBaseUrl = server.url("/");
  config.adapters.http = nullptr;  // the default: CurlHttp
  keygrant::License license(config);
  EXPECT_EQ(license.activate(" sluice-aaaa-bbbb-cccc-dddd\n"), ok());
  const json sent = json::parse(bodyOf(server.lastRequest()));
  EXPECT_EQ(sent["key"], KEY);
  EXPECT_EQ(sent["fingerprint"], "fp-test");
  EXPECT_EQ(sent["major"], 1);
  EXPECT_EQ(license.status(), licensed(KEY));
}

}  // namespace
}  // namespace kgtest
