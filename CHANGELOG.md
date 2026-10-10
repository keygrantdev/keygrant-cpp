# Changelog

## 0.4.0

- `License::purchaseUrl` is removed, and the SDK no longer asks for a bought key at `status()`, `refresh()` or
  `startTrial()`. To sell from the app, open your Payment Link from your own Buy button; the customer types the key
  from the delivery email (`activate`). A purchase secret an earlier version stored is ignored, and left out the next
  time the SDK saves its state.
- `StoredState` no longer has `purchaseToken`, `purchaseTokenAt` or `claimAskedAt`: a storage adapter or test code
  that sets them stops compiling, and the same names among its `otherFields` are never written. The shared library's
  SONAME carries the minor version while it is 0.x (`libkeygrant.so.0.4`), as `StoredState`'s layout changed.

## 0.3.0

- Key sets: `LicenseConfig::publicJwk` takes the product's key set (`{"keys":[...]}`, as `/pubkey` answers
  it) or a list of JWKs, and one key as before (a set of one). A lease naming its key (`kid`) is verified by
  that key, one naming none by each key in turn.
- A lease signed by a key the build does not carry is `Invalid` with the new reason
  `LicenseReason::UnknownKey`, rather than `Tampered`.
- Each activation, validate and claim sends the ids of the keys the build carries (`kids`).
- `License::has(code)` and `License::require(code)` answer, offline from the stored lease, whether the licence
  grants an entitlement; `require` throws `keygrant::MissingEntitlement`. `keygrant::grants` answers it of a
  status's entitlements.
- A public key's `x` is canonical base64url, as every KeyGrant SDK reads it: trailing padding is allowed;
  white space, the standard alphabet and stray bits are refused.
