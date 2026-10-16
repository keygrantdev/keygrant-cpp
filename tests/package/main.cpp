// The smallest app: a License with the default adapters but storage in a
// scratch folder, asked its status (unlicensed, with nothing asked online).
#include <keygrant/keygrant.hpp>

#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
  const std::string folder = argc > 1 ? argv[1] : "keygrant-consumer";
  keygrant::LicenseConfig config;
  config.product = "consumer-check";
  config.apiBaseUrl = "https://api.keygrant.dev";
  config.publicJwk = R"({"kty":"OKP","crv":"Ed25519","x":"Ohg4h8SrYQ0m1su_zVacJLLwf_6rNsvOtV5oWY6CzJs"})";
  config.major = 1;
  config.adapters.storage = std::make_shared<keygrant::FileStorage>(folder + "/consumer-check.json");
  keygrant::License license(config);
  const keygrant::LicenseStatus status = license.status();
  std::cout << status << '\n';
  // The metered-uses call through the installed headers: a licence that was never activated holds no meter to spend from.
  const keygrant::UseResult spent = license.use("exports");
  std::cout << spent << '\n';
  if (spent.ok || spent.error != keygrant::UseError::NotLicensed) return 1;
  return status.state == keygrant::LicenseState::Unlicensed ? 0 : 1;
}
