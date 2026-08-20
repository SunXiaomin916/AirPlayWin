# Cryptography boundary

`ISessionAuthenticator` keeps protocol authorization separate from credentials and derived
keys. `OpenSessionAuthenticator` is the minimal implementation for the explicitly advertised
unencrypted development profile.

Pairing, key derivation, secure storage, and AirPlay encryption remain absent. Pairing routes
are rejected explicitly and the repository contains no long-term or test key material. Windows
CNG remains preferred when the pairing phase adds protected keys.
