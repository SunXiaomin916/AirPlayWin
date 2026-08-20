# Discovery core

This directory owns the platform-independent discovery contract, AirPlay/RAOP DNS-SD
record construction, stable device-ID formatting, service-instance name normalization,
conflict suffixing, and network-interface eligibility policy.

`IDiscoveryService` is the only lifecycle API consumed by the application. Core code
does not include `windns.h`, IP Helper headers, or Winsock headers.
