#pragma once

namespace vac_join_guard {

// Detours the SteamUser018 secure-session authentication implementation.
// Secure authentication requests are refused without modifying Proton-owned
// interface vtables.
bool install();

}  // namespace vac_join_guard
