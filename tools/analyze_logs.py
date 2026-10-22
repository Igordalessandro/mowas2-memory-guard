#!/usr/bin/env python3
"""Offline, explicit-file Memory Guard triage. Standard library only; no uploads.

Units in JSON are bytes unless named otherwise. Estimates never establish
ownership, allocator slack, or a leak. This tool is not a background service.
"""
import argparse
from collections import deque
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import sys

MIB = 1024 * 1024
STAMP = re.compile(r"^\[([^]]+)\]\s*(.*)$")
PAIR = re.compile(r"([A-Za-z_][\w/]*)=([^\s|]+)")
QUANTITY = re.compile(r"([A-Za-z_]+)=([0-9]+)\s*(MiB|KiB|B)\b")


def values(message):
    result = dict(PAIR.findall(message))
    for key, amount, unit in QUANTITY.findall(message):
        result[key + "_bytes"] = int(amount) * {"MiB": MIB, "KiB": 1024, "B": 1}[unit]
    return result


def integer(data, key):
    try:
        return int(data[key])
    except (KeyError, TypeError, ValueError):
        return None


def moment(timestamp):
    try:
        return datetime.fromisoformat(timestamp)
    except (TypeError, ValueError):
        return None


def analyze_lines(lines, name="selected.log"):
    events = deque(maxlen=256)
    memory = deque(maxlen=256)
    failures = deque(maxlen=64)
    retained = {}
    shape = {}
    scan_cost = {}
    warnings = set()
    session = {"version": None, "sid": None, "environment": None, "experiment": None,
               "format": None, "full_log": None}
    profile, lifecycle, system, cpu = {}, {}, {}, {}
    previous_profile = None
    max_allocation_rate = None
    private_first = private_last = private_peak = None
    first_time = last_time = None
    crash = "unknown_not_captured_or_still_running"
    event_count = memory_count = failure_count = 0
    recovered = unrecovered = 0
    lines_seen = 0
    hook_apis = set()
    steam_text = set()
    terminal_seen = False

    for raw in lines:
        lines_seen += 1
        if len(raw) > 65536:
            warnings.add("oversized_line_skipped")
            continue
        if not raw.endswith(("\n", "\r")):
            warnings.add("unterminated_final_line_or_fragment")
        line = raw.rstrip("\r\n")
        match = STAMP.match(line)
        timestamp, message = match.groups() if match else (None, line)
        if timestamp:
            first_time = first_time or timestamp
            last_time = timestamp
        data = values(message)
        version = re.search(r"mowas2-memory-guard v([^ ]+) loaded", message)
        if version:
            if session["version"] and session["version"] != version[1]:
                warnings.add("multiple_sessions_or_versions_in_input")
            session["version"] = version[1]
        if message.startswith("session "):
            for source, target in (("v", "version"), ("sid", "sid"), ("env", "environment"),
                                   ("exp", "experiment"), ("fmt", "format")):
                if source in data:
                    if target == "sid" and session[target] and session[target] != data[source]:
                        warnings.add("multiple_sessions_or_versions_in_input")
                    session[target] = data[source]
        if message.startswith("REPORT SESSION"):
            for source, target in (("sid", "sid"), ("fmt", "format"),
                                   ("exp", "experiment"), ("full_log", "full_log")):
                if source in data:
                    session[target] = data[source]
        if message.startswith("full_log="):
            session["full_log"] = data.get("full_log")
        if message.startswith("ENVIRONMENT"):
            session["environment"] = data.get("env")
        if "STEAM TEXT : native Windows" in message and session["environment"] is None:
            session["environment"] = "native-windows"
        if message.startswith("Session:") and not session["full_log"]:
            filename = re.search(r"(mowas2-memory-guard[^\\/]*\.log)", message)
            if filename:
                session["full_log"] = filename[1]
        if "ALLOC PROFILER hook :" in message:
            api = re.search(r"hook : (\w+)", message)
            if api:
                hook_apis.add(api[1])
        if message.startswith("STEAM TEXT"):
            # No names, paths, IDs, or arbitrary string payloads in output.
            if "native Windows" in message:
                steam_text.add("native_windows_skipped")
            if "stale_cache=confirmed" in message:
                steam_text.add("stale_cache_confirmed")
            if "fallback" in message or "unsupported" in message or "exhaust" in message:
                steam_text.add("fallback_or_unsupported")

        kind = None
        if message.startswith(("MEM :", "MEM WARNING :")):
            point = {"timestamp": timestamp, "kind": "periodic", **data}
            memory.append(point)
            memory_count += 1
            private = data.get("private_bytes") if data.get("process_available") != "no" else None
            if private is not None:
                private_first = private if private_first is None else private_first
                private_last = private
                private_peak = max(private_peak or 0, private)
        elif message.startswith("MEM FAILURE SNAPSHOT"):
            point = {"timestamp": timestamp, "kind": "failure_snapshot", **data}
            memory.append(point)
            memory_count += 1
            kind = "failure_snapshot"
        elif message.startswith(("mem ", "mem_fail ")):
            memory.append({"timestamp": timestamp, "kind": "compact_snapshot", **data})
            memory_count += 1
        elif message.startswith("SYSTEM MEMORY"):
            system = data
        elif message.startswith("PROCESS CPU"):
            cpu = data
        elif message.startswith("VA SCAN COST"):
            scan_cost[data.get("kind", "unknown")] = data
        elif message.startswith("ALLOC PROFILE"):
            ambiguous = {"alloc", "realloc", "free", "failures", "requested", "requested_bytes"}
            profile = {key: value for key, value in data.items() if key not in ambiguous}
            for segment in message.split("|"):
                prefix = "heap" if "heap alloc=" in segment else "virtual" if "virtual alloc=" in segment else None
                if prefix:
                    for key, value in values(segment).items():
                        profile[prefix + "_" + key] = value
            # The same line contains a virtual alloc count: do not confuse it
            # with the first, heap alloc count when parsing key=value tokens.
            heap = re.search(r"heap alloc=(\d+) realloc=(\d+) free=(\d+)", message)
            if heap:
                profile.update(heap_alloc=int(heap[1]), heap_realloc=int(heap[2]), heap_free=int(heap[3]))
                now = moment(timestamp)
                if previous_profile and now:
                    before, count = previous_profile
                    seconds = (now - before).total_seconds()
                    delta = int(heap[1]) - count
                    if seconds > 0 and delta >= 0:
                        rate = delta / seconds
                        max_allocation_rate = max(max_allocation_rate or 0, rate)
                    elif delta < 0:
                        warnings.add("counter_reset_or_mixed_session")
                if now:
                    previous_profile = (now, int(heap[1]))
            if integer(data, "drops") or integer(data, "sites_dropped") or data.get("sites_saturated") == "1":
                warnings.add("sampling_or_site_table_coverage_incomplete")
        elif message.startswith("ALLOC LIFECYCLE"):
            lifecycle = data
        elif message.startswith("ALLOC SHAPE"):
            if integer(data, "bucket") is not None:
                shape[str(data["bucket"])] = data
        elif message.startswith("ALLOC WATCH AGE"):
            site = data.get("site") or data.get("caller")
            if site:
                item = retained.setdefault(site, {"source": "watch_age", "first_sample_B": None})
                current = integer(data, "sample_B")
                if item["first_sample_B"] is None:
                    item["first_sample_B"] = current
                item.update(data)
                item["delta_from_first_sample_B"] = (
                    current - item["first_sample_B"] if current is not None and item["first_sample_B"] is not None else None)
        elif message.startswith("ALLOC RETAINED AGE"):
            if integer(data, "metadata_wall_ms") is not None:
                scan_cost["retention_metadata"] = data
        elif message.startswith("ALLOC TOP RETAINED"):
            site = re.search(r"mowas_2\.exe\+0x([0-9A-Fa-f]+)", message)
            if site:
                key = "+" + site[1].upper()
                item = retained.setdefault(key, {"source": "legacy_sample_estimate"})
                item["sample_live_bytes"] = data.get("sample_live_bytes")
                item["estimated_live_bytes"] = data.get("est_bytes")
        elif message.startswith("LOW-LEVEL") and "FAILURE" in message:
            kind = "allocation_failure"
            result = "recovered" if "RECOVERED" in message else "unrecovered"
            recovered += result == "recovered"
            unrecovered += result == "unrecovered"
            failures.append({"timestamp": timestamp, "result": result, **data})
            failure_count += 1
        elif message.startswith("failure=") and data.get("failure") != "none_observed":
            kind = "compact_failure"
            failures.append({"timestamp": timestamp, **data})
            failure_count += 1
        elif "DLL CRASH REPORT : UNHANDLED EXCEPTION" in message or message.startswith("crash=unhandled"):
            crash = "captured_unhandled_exception"
            terminal_seen = True
            kind = "unhandled_exception"
        elif "MEM GUARD RELEASED" in message or "event=reserve_release" in message:
            kind = "reserve_release"
        elif message.startswith("ALLOC WATCH TRACE"):
            kind = "watch_trace"
        elif message.startswith("ACTION MARKER"):
            kind = "reported_action"
        if kind:
            event_count += 1
            # Keep machine fields, not arbitrary message text or private paths.
            events.append({"timestamp": timestamp, "kind": kind, **data})

    if session["sid"] is None:
        warnings.add("legacy_or_missing_session_id")
    if not shape:
        warnings.add("allocation_size_distribution_unavailable")
    if not lifecycle:
        warnings.add("thread_and_resize_evidence_unavailable")
    if not any(item.get("source") == "watch_age" for item in retained.values()):
        warnings.add("retained_age_evidence_unavailable")
    if event_count > len(events) or memory_count > len(memory) or failure_count > len(failures):
        warnings.add("analysis_history_capped_latest_records_only")
    warnings.add("observed_iat_is_not_complete_process_ownership")
    conclusions = []
    unresolved = ["Allocation ownership and size-query/free coverage are not audited.",
                  "Live game bytes and allocator slack are unknown; sample estimates cannot establish either.",
                  "Load/menu phases, mods and settings are unknown unless independently supplied."]
    pressure = [point for point in memory if point.get("kind") == "failure_snapshot"
                and point.get("phase") != "retry_ok" and point.get("complete") == "1"
                and point.get("free_bytes", 2**64) <= 64 * MIB
                and integer(point, "bytes") is not None
                and point.get("largest_bytes", 2**64) < int(point["bytes"])]
    if pressure:
        conclusions.append("A complete failure-time scan shows severe virtual-address pressure and no free region large enough for the request; internal heap reuse is not measured.")
    if recovered:
        conclusions.append(f"{recovered} low-level failure record(s) explicitly report recovery; these are not additional logical allocation totals.")
    if unrecovered:
        conclusions.append(f"{unrecovered} low-level failure record(s) report no recovery; this alone does not prove process termination.")
    if not terminal_seen:
        unresolved.append("Termination is not captured; log ending is not proof of a crash or clean exit.")
    if system.get("available_phys_bytes", 0) >= 1024 * MIB and pressure:
        conclusions.append("The latest physical-RAM snapshot has at least 1 GiB available; that snapshot is not necessarily simultaneous with the failure.")
    opportunity = []
    samples = sum(integer(bucket, "request_samples") or 0 for bucket in shape.values())
    small = sum(integer(bucket, "request_samples") or 0 for bucket in shape.values()
                if (integer(bucket, "max_B") or 2**64) < 16384)
    if samples:
        opportunity.append(f"Observed request sample: {samples} attempts, {small} below 16 KiB; cohorts are incomplete and not proof of whole-game ownership or performance.")
    if pressure:
        opportunity.append("Investigate placement/reuse versus retained resources; fragmentation does not establish allocator waste.")
    if lifecycle:
        opportunity.append("Sampled thread/resize observations are available; use them to design an isolated allocator benchmark, not certify integration safety.")
    evidence_checks = {
        "request_size_distribution": "sampled" if shape else "missing",
        "retained_age_by_caller": "sampled" if any(item.get("source") == "watch_age" for item in retained.values()) else "missing",
        "retirement_thread_and_resize": "sampled" if lifecycle else "missing",
        "whole_process_allocation_ownership": "not_audited",
        "size_queries_alignment_and_other_module_frees": "not_audited",
        "controlled_alternative_allocator_benchmark": "missing",
        "exact_live_bytes_and_allocator_slack": "unknown",
    }
    next_steps = (["Obtain a .15 full log for allocation-size and retirement-thread observations."]
                  if not shape or not lifecycle else
                  ["Use the sampled distribution and lifetimes to design an isolated workload benchmark; aggregate samples are not a replayable exact allocation trace."])
    next_steps += ["Audit matching free/resize/size-query paths and ownership before any allocator substitution.",
                   "Identify retained resource owners before interpreting sample retention as reclaimable memory."]
    return {
        "analysis_format": 1, "input_name": name, "lines_seen": lines_seen,
        "session": session, "first_timestamp": first_time, "last_timestamp": last_time,
        "timestamp_basis": "logger_local_time_not_verified_UTC", "crash_status": crash,
        "failures": list(failures), "failure_records": failure_count,
        "recovered_records": recovered, "unrecovered_records": unrecovered,
        "memory": list(memory), "private_first_bytes": private_first,
        "private_last_bytes": private_last, "private_peak_bytes": private_peak,
        "max_observed_heap_allocations_per_second": max_allocation_rate,
        "retained_by_caller": retained, "request_shape": shape, "sampled_lifecycle": lifecycle,
        "latest_profile": profile, "latest_system_memory": system, "latest_process_cpu": cpu, "scan_costs": scan_cost,
        "hook_apis_observed": sorted(hook_apis), "steam_text_observations": sorted(steam_text),
        "timeline": list(events), "warnings": sorted(warnings), "conclusions": conclusions,
        "unresolved": unresolved,
        "allocator_assessment": {"status": "insufficient_evidence", "integration_safety": "not_audited",
                                 "opportunity_hints": opportunity,
                                 "evidence_checks": evidence_checks, "next_steps": next_steps,
                                 "reason": "Neither sampled retention nor allocation volume proves replaceable allocator waste; no controlled allocator comparison is supplied."},
    }


def analyze_file(path, max_bytes=32 * MIB):
    path = Path(path)
    if not path.is_file():
        raise ValueError(f"Select an explicit regular log file: {path.name}")
    # Read a bounded snapshot, not an endlessly growing live stream.
    with path.open("rb") as handle:
        raw = handle.read(max_bytes + 1)
    if len(raw) > max_bytes:
        raise ValueError(f"{path.name}: input exceeds limit; raise --max-input-mib explicitly")
    result = analyze_lines(raw.decode("utf-8-sig", errors="replace").splitlines(keepends=True), path.name)
    result["sha256"] = hashlib.sha256(raw).hexdigest()
    result["input_bytes"] = len(raw)
    return result


def compare(reports):
    if len(reports) < 2:
        return {"status": "not_requested"}
    sessions = [report["session"] for report in reports]
    ids, logs = [item["sid"] for item in sessions], [item["full_log"] for item in sessions]
    if all(ids) and all(logs):
        if (len(set(ids)) == 1) != (len(set(logs)) == 1):
            return {"status": "session_correlation_conflict", "controlled_comparison": False,
                    "warning": "Session identifiers and referenced full-log filenames disagree; do not combine these records."}
        if len(set(ids)) == 1:
            return {"status": "matching_session", "controlled_comparison": False,
                    "warning": "These files identify the same session, not independent comparison runs; content equality is not certified."}
    metadata = [(r["session"]["version"], r["session"]["environment"], r["session"]["experiment"])
                for r in reports]
    known = all(all(value is not None for value in key) for key in metadata)
    status = "matching_build_environment_only" if known and len(set(metadata)) == 1 else "incompatible_or_unknown_metadata"
    return {"status": status, "controlled_comparison": False,
            "warning": "Identical build/environment does not establish identical content, settings or actions; do not infer a treatment effect.",
            "private_peak_bytes": [r["private_peak_bytes"] for r in reports]}


def markdown(document):
    output = ["# Memory Guard evidence report", "", "Offline observations, not a leak diagnosis or allocator compatibility certificate."]
    for report in document["reports"]:
        session = report["session"]
        output.extend(["", f"## {report['input_name'].replace('`', '')}", "",
                       f"Version: {session['version'] or 'unknown'}; session: {session['sid'] or 'unknown'}; environment: {session['environment'] or 'unknown'}.",
                       f"Allocator assessment: **{report['allocator_assessment']['status']}**; safety: not audited.",
                       f"Crash status: {report['crash_status']}; recovered/unrecovered records: {report['recovered_records']}/{report['unrecovered_records']}.",
                       "", "Conclusions:", ""])
        output.extend("- " + item for item in report["conclusions"] or ["No conclusive failure evidence parsed."])
        output.extend(["", "Allocator leads:", ""])
        output.extend("- " + item for item in report["allocator_assessment"]["opportunity_hints"] or ["Additional allocator evidence is needed."])
        output.extend(["", "Next evidence needed:", ""])
        output.extend("- " + item for item in report["allocator_assessment"]["next_steps"])
        output.extend(["", "Memory trend (bytes; unknown is not zero): " +
                       f"private first={report['private_first_bytes']}, last={report['private_last_bytes']}, peak={report['private_peak_bytes']}.",
                       "", "Retention by caller (sample observations, not owned/freeable buffers):", ""])
        for site, item in report["retained_by_caller"].items():
            output.append(f"- {site}: " + " ".join(f"{key}={value}" for key, value in item.items()))
        output.extend(["", "Latest allocation-size sample buckets:", ""])
        for bucket, item in report["request_shape"].items():
            output.append(f"- Bucket {bucket}: " + " ".join(f"{key}={value}" for key, value in item.items()))
        output.extend(["", "Unresolved / coverage:", ""])
        output.extend("- " + item for item in report["unresolved"] + report["warnings"])
        output.extend(["", "Timeline (latest bounded records):", ""])
        output.extend(f"- {item['timestamp'] or 'unknown time'}: {item['kind']} " +
                      " ".join(f"{key}={value}" for key, value in item.items()
                               if key not in {"timestamp", "kind"}) for item in report["timeline"])
    output.extend(["", "Comparison: " + document["comparison"]["status"], ""])
    return "\n".join(output)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", help="Explicit full or compact log files (no directory scanning)")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument("--max-input-mib", type=int, default=32)
    args = parser.parse_args(argv)
    if not 1 <= args.max_input_mib <= 256:
        parser.error("--max-input-mib must be between 1 and 256")
    try:
        reports = [analyze_file(path, args.max_input_mib * MIB) for path in args.logs]
    except (OSError, ValueError) as error:
        parser.error(str(error))
    document = {"reports": reports, "comparison": compare(reports)}
    print(json.dumps(document, indent=2, ensure_ascii=True) if args.format == "json" else markdown(document))
    return 0


if __name__ == "__main__":
    sys.exit(main())
