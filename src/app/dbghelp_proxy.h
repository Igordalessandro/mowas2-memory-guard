#pragma once

namespace dbghelp_proxy {

// Resolve the real system crash-dump functions while address space is healthy.
// Call only outside DllMain/the loader lock. Editors retain lazy forwarding.
bool prewarm();

}
