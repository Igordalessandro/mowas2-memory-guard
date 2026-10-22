param(
    [string]$ToolchainRoot = $env:LLVM_MINGW_ROOT,
    [string]$OutputDirectory,
    [switch]$CompileOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ToolchainRoot) { throw 'Set LLVM_MINGW_ROOT or pass -ToolchainRoot for the pinned llvm-mingw toolchain.' }
$compiler = Join-Path $ToolchainRoot 'bin/i686-w64-mingw32-clang++.exe'
if (-not (Test-Path $compiler)) { throw "Compiler missing: $compiler" }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'build-tests-windows' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$sources = @(
    'memory_guard_recovery_state_test',
    'compact_report_test',
    'retry_policy_test',
    'allocator_profiler_tests',
    'long_path_guard_tests',
    'vac_guard_tests',
    'crash_report_tests',
    'dbghelp_proxy_tests',
    'logger_tests',
    'address_space_stats_test',
    'large_allocation_history_test',
    'memory_guard_diagnostics_tests',
    'engine_alloc_tests',
    'stable_steam_text_test',
    'steam_text_guard_tests',
    'profiling_policy_test',
    'watch_diagnostics_test',
    'allocator_assessment_test'
)
foreach ($name in $sources) {
    $source = Join-Path $PSScriptRoot "$name.cpp"
    $executable = Join-Path $OutputDirectory "$name.exe"
    & $compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wconversion' '-Werror' '-fms-extensions' '-D_WIN32_WINNT=0x0601' "-I$projectRoot/src" "-I$projectRoot/thirdparty/minhook/include" '-static' $source '-o' $executable '-lpsapi'
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    if (-not $CompileOnly) {
        & $executable
        if ($LASTEXITCODE -ne 0) { throw "Regression failed: $name" }
        Write-Host "Passed: $name"
    }
}
if ($CompileOnly) { Write-Host 'Windows regression executables compiled; not executed.' }
