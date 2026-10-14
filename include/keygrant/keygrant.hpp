// The KeyGrant licensing SDK for C++: everything an app includes.
//
//   #include <keygrant/keygrant.hpp>
//
//   keygrant::LicenseConfig config;
//   config.product = "sluice";
//   config.apiBaseUrl = "https://api.keygrant.dev";
//   config.publicJwk = R"({"kty":"OKP","crv":"Ed25519","x":"..."})";
//   config.major = 3;
//   keygrant::License license(config);
//   keygrant::LicenseStatus status = license.status();   // off the UI thread
#pragma once

#include "keygrant/adapters.hpp"
#include "keygrant/fingerprint.hpp"
#include "keygrant/license.hpp"
#include "keygrant/license_file.hpp"
#include "keygrant/storage.hpp"
#include "keygrant/types.hpp"
