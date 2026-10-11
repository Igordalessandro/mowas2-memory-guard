#pragma once

namespace vac_join_guard {

// Hooks both the SteamMatchmaking009 lobby boundary and the later SteamUser018
// authentication boundary. Only lobbies explicitly marked insecure are
// admitted, and any later secure authentication request is also refused.
bool install();

}  // namespace vac_join_guard
