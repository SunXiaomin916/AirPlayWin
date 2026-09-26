# Discovery core

This directory owns the platform-independent discovery contract, AirPlay/RAOP DNS-SD
record construction, stable device-ID formatting, service-instance name normalization,
conflict suffixing, and network-interface eligibility policy.

`IDiscoveryService` is the only lifecycle API consumed by the application. Core code
does not include `windns.h`, IP Helper headers, or Winsock headers.

`DiscoveryConfig::classic_raop` selects a deliberately narrow compatibility profile:
only the RAOP service is normally published by the application, and its TXT record
advertises PCM/Apple Lossless (`cn=0,1`) plus legacy RSA-AES (`et=0,1`) over UDP with
the classic RAOP server version. The default profile remains the phase-12 dual-service
profile.
