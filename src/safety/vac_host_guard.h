#pragma once

namespace vac_host_guard {

// Replaces only mowas_2.exe's SteamGameServer_Init import.  Secure host
// requests are rejected; insecure/authentication-only requests are forwarded
// to the original Steam API function unchanged.
bool install();

}  // namespace vac_host_guard
