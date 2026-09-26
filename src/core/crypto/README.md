# Cryptography boundary

`ISessionAuthenticator` keeps protocol authorization separate from credentials and derived
keys. `OpenSessionAuthenticator` is the v1.0 implementation for the trusted-Private-network
Classic RAOP profile.

`IRaopCryptoProvider` is the separate classic-media boundary. The Windows adapter answers the
legacy Apple challenge, recovers the per-session AES key with RSA-OAEP, and decrypts
AES-128-CBC audio with CNG. Session key/scratch material is cleared during teardown.

Modern AirPlay pairing/FairPlay, user PIN authorization, credential storage, and protected
long-term identity remain absent. Pairing routes are rejected explicitly. Consequently, v1.0
must be used only on a trusted Private network as documented in `SECURITY.md`.
