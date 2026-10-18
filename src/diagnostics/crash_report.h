#pragma once

namespace crash_report {

// Installs a top-level exception recorder while preserving and chaining the
// filter that was present before Memory Guard loaded.
bool install();

}  // namespace crash_report
