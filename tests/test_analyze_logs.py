"""Sanitized fixtures: no downloaded reports, names, real IDs or local paths."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("analyze_logs", Path(__file__).resolve().parents[1] / "tools/analyze_logs.py")
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)

LEGACY = """[2026-10-21 12:00:00.000] mowas2-memory-guard v2.5.0-experimental.13 loaded
[2026-10-21 12:00:00.001] Session: pid=42 log=D:\\game\\mowas2-memory-guard-example.log
[2026-10-21 12:00:00.010] STEAM TEXT : native Windows; compatibility hook not needed
[2026-10-21 12:00:01.000] MEM : VA free=2132 MiB largest=1949 MiB @ 00010000 committed=1556 MiB reserved=406 MiB guard=96 MiB | private=1170 MiB working_set=1160 MiB
[2026-10-21 12:00:30.000] ALLOC PROFILE : heap alloc=1000 realloc=1 free=900 failures=0 requested=4 MiB | virtual alloc=0 free=0 failures=0 requested=0 MiB | samples=2 live=5 KiB estimate=5 MiB drops=0 sites_dropped=0 sites_saturated=0
[2026-10-21 12:01:00.000] ALLOC PROFILE : heap alloc=4000 realloc=2 free=3900 failures=1 requested=12 MiB | virtual alloc=5 free=4 failures=0 requested=3 MiB | samples=3 live=6 KiB estimate=6 MiB drops=0 sites_dropped=22 sites_saturated=1
[2026-10-21 12:01:00.100] ALLOC TOP RETAINED #1 : mowas_2.exe+0x003749C9 sample_live=1594 KiB (45 blocks, est=1594 MiB) sampled_freed=50 KiB avg_lifetime=0 ms
[2026-10-21 12:01:00.200] LOW-LEVEL ALLOC FAILURE RECOVERED : HeapAlloc size=699211 caller=mowas_2.exe+0x0093176C released=48 MiB retries=1
[2026-10-21 12:01:01.000] MEM FAILURE SNAPSHOT : api=HeapAlloc bytes=699211 site=+0093176C phase=initial thread=7 tick=123 free=36196KiB largest=480KiB committed=3799MiB reserved=261MiB guard=0MiB complete=1
[2026-10-21 12:01:01.100] LOW-LEVEL ALLOC FAILURE : HeapAlloc size=699211 caller=mowas_2.exe+0x0093176C released=0 MiB retries=0
[2026-10-21 12:01:02.000] SYSTEM MEMORY : available_phys=14079MiB total_phys=32060MiB page_faults=123 (soft+hard)
"""
MODERN = """[2026-10-22 12:00:00.000] REPORT SESSION : fmt=2 sid=example-p42 full_log=mowas2-memory-guard-example.log exp=off request_status=off
[2026-10-22 12:00:00.000] session v=2.5.0-experimental.15 sid=example-p42 env=native-windows exp=off fmt=2
[2026-10-22 12:00:01.000] ALLOC PROFILER hook : HeapAlloc IAT=00100000 original=00200000
[2026-10-22 12:00:30.000] ALLOC WATCH AGE : snap=1 site=+003749C9 sample_blocks=2 sample_B=2048 delta_B=0 oldest_ms=60000
[2026-10-22 12:01:00.000] ALLOC WATCH AGE : snap=2 site=+003749C9 sample_blocks=1 sample_B=1024 delta_B=-1024 oldest_ms=90000
[2026-10-22 12:01:00.000] ALLOC LIFECYCLE : rate=1/1024 same_thread=40 cross_thread=3 reallocated=2 retired_ms=4500 flags_or=00000008 coverage=observed_iat_only
[2026-10-22 12:01:00.000] ALLOC SHAPE : bucket=1 min_B=16 max_B=63 rate=1/1024 request_samples=300 request_sample_B=15000 retired_samples=80 retired_sample_B=4000
"""


class AnalyzerTests(unittest.TestCase):
    def parse(self, content):
        return analyzer.analyze_lines(content.splitlines(keepends=True))

    def test_legacy_separates_traffic_estimates_and_unknown_termination(self):
        result = self.parse(LEGACY)
        self.assertEqual(result["session"]["version"], "2.5.0-experimental.13")
        self.assertEqual(result["session"]["full_log"], "mowas2-memory-guard-example.log")
        self.assertEqual(result["recovered_records"], 1)
        self.assertEqual(result["unrecovered_records"], 1)
        self.assertEqual(result["latest_profile"]["heap_alloc"], 4000)
        self.assertEqual(result["latest_profile"]["heap_failures"], "1")
        self.assertEqual(result["latest_profile"]["virtual_failures"], "0")
        self.assertEqual(result["max_observed_heap_allocations_per_second"], 100)
        self.assertEqual(result["retained_by_caller"]["+003749C9"]["estimated_live_bytes"], 1594 * analyzer.MIB)
        self.assertEqual(result["crash_status"], "unknown_not_captured_or_still_running")
        self.assertTrue(any("virtual-address pressure" in line for line in result["conclusions"]))
        self.assertIn("sampling_or_site_table_coverage_incomplete", result["warnings"])
        self.assertEqual(result["allocator_assessment"]["status"], "insufficient_evidence")

    def test_modern_sampled_lifetimes_and_shapes(self):
        result = self.parse(MODERN)
        self.assertEqual(result["session"]["sid"], "example-p42")
        self.assertEqual(result["session"]["experiment"], "off")
        self.assertEqual(result["retained_by_caller"]["+003749C9"]["delta_from_first_sample_B"], -1024)
        self.assertEqual(result["hook_apis_observed"], ["HeapAlloc"])
        self.assertEqual(result["request_shape"]["1"]["request_samples"], "300")
        self.assertTrue(result["allocator_assessment"]["opportunity_hints"])
        self.assertNotIn("allocation_size_distribution_unavailable", result["warnings"])

    def test_partial_and_unknown_are_not_zero(self):
        result = self.parse("[2026-10-22 12:00:00.000] MEM FAILURE SNAPSHOT : api=HeapAlloc bytes=")
        self.assertIn("unterminated_final_line_or_fragment", result["warnings"])
        self.assertEqual(result["conclusions"], [])
        self.assertIsNone(result["private_peak_bytes"])
        self.assertEqual(result["allocator_assessment"]["status"], "insufficient_evidence")

    def test_failed_process_query_and_cpu_availability(self):
        report = self.parse("[2026-10-22 12:00:00.000] MEM : VA free=5 MiB largest=1 MiB private=0 MiB working_set=0 MiB process_available=no\n[2026-10-22 12:00:00.000] PROCESS CPU : available=no core_pct=unknown\n")
        self.assertIsNone(report["private_peak_bytes"])
        self.assertEqual(report["latest_process_cpu"]["core_pct"], "unknown")

    def test_incomplete_scan_or_recovered_snapshot_not_used_as_failure_proof(self):
        for change in (("complete=1", "complete=0"), ("phase=initial", "phase=retry_ok")):
            result = self.parse(LEGACY.replace(*change))
            self.assertFalse(any("virtual-address pressure" in line for line in result["conclusions"]))

    def test_exception_requires_an_actual_exception_record(self):
        result = self.parse(MODERN + "[2026-10-22 12:02:00.000] DLL CRASH REPORT : UNHANDLED EXCEPTION code=0xC0000005\n")
        self.assertEqual(result["crash_status"], "captured_unhandled_exception")

    def test_compact_reports_and_session_comparison(self):
        compact = "session v=2.5.0-experimental.15 sid=example-p42 env=native-windows exp=off fmt=2\nfull_log=mowas2-memory-guard-example.log\nfailure=HeapAlloc bytes=699211 result=failed\nmem_fail free=36196KiB largest=480KiB\n"
        result = self.parse(compact)
        self.assertEqual(result["memory"][0]["largest_bytes"], 480 * 1024)
        comparison = analyzer.compare([result, self.parse(MODERN)])
        self.assertEqual(comparison["status"], "matching_session")
        self.assertFalse(comparison["controlled_comparison"])
        self.assertEqual(analyzer.compare([result, self.parse(LEGACY)])["status"], "incompatible_or_unknown_metadata")
        wrong = self.parse(MODERN.replace("full_log=mowas2-memory-guard-example.log", "full_log=wrong.log"))
        self.assertEqual(analyzer.compare([result, wrong])["status"], "session_correlation_conflict")

    def test_bounded_history(self):
        result = self.parse("[2026-10-22 12:00:00.000] MEM FAILURE SNAPSHOT : bytes=1\n" * 1000)
        self.assertEqual(len(result["timeline"]), 256)
        self.assertEqual(len(result["memory"]), 256)
        self.assertIn("analysis_history_capped_latest_records_only", result["warnings"])

    def test_markdown_has_triage_sections(self):
        output = analyzer.markdown({"reports": [self.parse(MODERN)], "comparison": {"status": "not_requested"}})
        for text in ("insufficient_evidence", "Timeline", "Unresolved", "Allocator leads"):
            self.assertIn(text, output)

    def test_explicit_files_cli_and_limits(self):
        with tempfile.TemporaryDirectory(prefix="mowas2-analyzer-test-") as directory:
            path = Path(directory) / "selected.log"
            path.write_text(MODERN, encoding="utf-8")
            report = analyzer.analyze_file(path)
            self.assertEqual(len(report["sha256"]), 64)
            with self.assertRaises(ValueError):
                analyzer.analyze_file(path, max_bytes=10)
            with self.assertRaises(ValueError):
                analyzer.analyze_file(directory)
            command = [sys.executable, str(SPEC.origin), "--format", "json", str(path)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10, check=True)
            self.assertIn('"analysis_format": 1', result.stdout)


if __name__ == "__main__":
    unittest.main()
