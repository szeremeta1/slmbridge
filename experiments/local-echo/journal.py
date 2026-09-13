#!/usr/bin/env python3
"""Pure attribution and reference checks; no modem, process or service actions."""
import hashlib
import json
import re

REASONS = frozenset(("input_frame_size", "input_before_reference_commit", "counter_overflow",
                     "unpaired_output", "unpaired_commit"))
COUNTERS = ("received", "committed", "warmup", "mixed", "clipped", "pending")


def require(condition, message):
    if not condition: raise ValueError(message)


def analyze(raw, mode, transport, executable, unit):
    """Bind one reference record to an already checked whole-call transport.

    The caller must independently verify the actual executable bytes, process
    generation and requested mode. A log pathname is not a binary fingerprint.
    Fragmented records deliberately refuse until their precise journal framing
    is supported; guessing an unmarked continuation would weaken attribution.
    """
    require(mode in ("off", "observe", "mix"), "unsupported exact reference mode")
    require(isinstance(raw, str) and len(raw.encode()) <= 16 * 1024 * 1024, "bounded journal required")
    require(isinstance(executable, str) and executable.startswith("/") and
            isinstance(unit, str) and unit.endswith(".service"), "explicit executable and unit required")
    require(transport.get("verified") is True and transport.get("enabled") is True and
            transport.get("journal_sha256") == hashlib.sha256(raw.encode()).hexdigest(),
            "complete enabled transport proof for this exact journal required")
    rows = [json.loads(line) for line in raw.splitlines() if line.strip()]
    require(rows and all(isinstance(r, dict) and isinstance(r.get("MESSAGE"), str) for r in rows),
            "journal records malformed")
    reports = [r for r in rows if r["MESSAGE"].startswith("bridge_local_echo:")]
    if mode == "off":
        require(not reports, "disabled reference unexpectedly reported activity")
        return {"mode": mode, "attribution_verified": True, "reference_valid": None,
                "delivery_complete": None, "usable_treatment": False, "counters": None,
                "issues": ["reference_disabled"], "journal_sha256": transport["journal_sha256"]}
    require(len(reports) == 1, "one complete local-reference record required")
    row = reports[0]
    helper = transport["roles"]["helper"]
    expected = {"_PID": helper["pid"], "_BOOT_ID": transport["boot_id"],
                "_EXE": executable, "_SYSTEMD_UNIT": unit, "_TRANSPORT": "stdout"}
    require(all(row.get(k) == v for k, v in expected.items()), "reference process attribution differs")
    require(re.fullmatch(r"[0-9a-f]{32}", str(row.get("_STREAM_ID", ""))) and
            row.get("_LINE_BREAK") is None, "complete attributed reference line required")
    stamp = str(row.get("__REALTIME_TIMESTAMP", ""))
    require(re.fullmatch(r"[0-9]+", stamp) and
            int(helper["start_realtime_us"]) <= int(stamp) <= int(helper["end_realtime_us"]),
            "reference record lies outside helper lifetime")
    # Bind the stream as well as the PID to a helper lifecycle event. The
    # supplied transport parser already checks exact lifecycle grammar/order.
    anchors = [r for r in rows if r["MESSAGE"].startswith("slmbridge helper: exit (")]
    require(len(anchors) == 1 and all(anchors[0].get(k) == v for k, v in expected.items()) and
            anchors[0].get("_STREAM_ID") == row["_STREAM_ID"], "reference stream differs from helper exit")
    exit_stamp = str(anchors[0].get("__REALTIME_TIMESTAMP", ""))
    require(re.fullmatch(r"[0-9]+", exit_stamp) and int(exit_stamp) <= int(stamp) and
            rows.index(anchors[0]) < rows.index(row), "reference report precedes helper exit")
    pattern = (r"bridge_local_echo: mode=(observe|mix) invalid=([01]) reason=([a-z_]+) " +
               " ".join(name + r"=([0-9]+)" for name in COUNTERS))
    match = re.fullmatch(pattern, row["MESSAGE"])
    require(match is not None and match[1] == mode, "reference mode or complete counter grammar differs")
    invalid, reason = match[2] == "1", match[3]
    counts = dict(zip(COUNTERS, map(int, match.groups()[3:])))
    require(all(n < 2**64 for n in counts.values()) and counts["pending"] <= 1,
            "reference counter exceeds ABI range")
    received, committed = counts["received"], counts["committed"]
    require(0 <= received - committed <= 1 and
            (not counts["pending"] or received == committed + 1), "impossible reference commit prefix")
    require(counts["warmup"] == int(received > 0) and
            counts["mixed"] == (max(0, received - 1) if mode == "mix" else 0) and
            counts["clipped"] <= counts["mixed"] * 160, "impossible reference sample counts")
    require((invalid and reason in REASONS) or (not invalid and reason == "none"),
            "invalid flag and reason disagree")
    issues = []
    if invalid: issues.append("reference_epoch_invalid:" + reason)
    if counts["pending"] or received != committed: issues.append("reference_output_incomplete")
    if received < 2: issues.append("no_post_warmup_reference")
    # Invalid epochs freeze reference counters; they cannot be compared with
    # whole-call volume as if the experiment had continued after invalidation.
    pcm = transport["helper_pcm_counts"]
    complete_counts = not invalid and pcm["rx"] == received and pcm["tx"] == committed
    if not complete_counts: issues.append("reference_does_not_cover_whole_call")
    if transport.get("transport_error_observed") is not False: issues.append("transport_error_or_unknown")
    if transport.get("pending_output_observed") is not False: issues.append("transport_output_pending_or_unknown")
    if transport.get("partial_input_observed") is not False: issues.append("transport_input_partial_or_unknown")
    reference_valid = not invalid and received == committed and not counts["pending"]
    delivery_complete = complete_counts and all(transport.get(k) is False for k in
        ("transport_error_observed", "pending_output_observed", "partial_input_observed"))
    return {"mode": mode, "attribution_verified": True, "reference_valid": reference_valid,
            "delivery_complete": delivery_complete, "usable_treatment": not issues,
            "counters": counts, "invalid": invalid, "reason": reason, "issues": issues,
            "journal_sha256": transport["journal_sha256"], "report_realtime_us": stamp,
            "scope": "Attributed reference and delivery only; no physical echo delay, speed or call-reliability claim."}
