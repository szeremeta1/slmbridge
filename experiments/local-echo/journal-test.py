#!/usr/bin/env python3
"""Offline attribution faults and real C-generated reference reports."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("echo_journal", HERE / "journal.py")
j = importlib.util.module_from_spec(spec); spec.loader.exec_module(j)
EXE, UNIT = "/reviewed/slmbridge", "reviewed-answer.service"


def fixture(message, rx=3, tx=3):
    identity = {"_PID": "123", "_BOOT_ID": "a" * 32, "_EXE": EXE,
                "_SYSTEMD_UNIT": UNIT, "_TRANSPORT": "stdout", "_STREAM_ID": "b" * 32}
    rows = [dict(identity, MESSAGE="slmbridge helper: exit (PCM EOF) tx=3 rx=3", __REALTIME_TIMESTAMP="200"),
            dict(identity, MESSAGE=message, __REALTIME_TIMESTAMP="201")]
    transport = {"verified": True, "enabled": True, "boot_id": "a" * 32,
                 "roles": {"helper": {"pid": "123", "start_realtime_us": "100", "end_realtime_us": "202"}},
                 "helper_pcm_counts": {"rx": rx, "tx": tx}, "transport_error_observed": False,
                 "pending_output_observed": False, "partial_input_observed": False}
    return rows, transport


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(); root = Path(cls.temp.name)
        source = root / "report.c"
        source.write_text('''#include "local_echo.h"
int main(void) {
  int16_t rx[160]={0},tx[160]={100};
  struct local_echo s={.mode=LE_OBSERVE};
  for(int i=0;i<3;i++){le_receive(&s,rx,sizeof rx);le_stage(&s,tx);le_commit(&s);}
  le_report(&s);
  s=(struct local_echo){.mode=LE_MIX};
  for(int i=0;i<3;i++){le_receive(&s,rx,sizeof rx);le_stage(&s,tx);le_commit(&s);}
  le_report(&s);
  le_stage(&s,tx); le_report(&s);
  s=(struct local_echo){.mode=LE_MIX}; le_receive(&s,rx,sizeof rx);le_stage(&s,tx);le_report(&s);
  return 0;
}''')
        subprocess.run(["cc", "-Wall", "-Wextra", "-Wno-unused-function", "-I", str(HERE),
                        str(source), "-o", str(root / "report")], check=True, capture_output=True, timeout=30)
        cls.reports = subprocess.run([str(root / "report")], check=True, capture_output=True,
                                     text=True, timeout=5).stderr.splitlines()
        assert len(cls.reports) == 4

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def call(self, rows, proof, mode="mix", rehash=True):
        raw = "".join(json.dumps(r) + "\n" for r in rows)
        if rehash: proof["journal_sha256"] = hashlib.sha256(raw.encode()).hexdigest()
        return j.analyze(raw, mode, proof, EXE, UNIT)

    def test_real_observe_and_mix(self):
        for i, mode in enumerate(("observe", "mix")):
            result = self.call(*fixture(self.reports[i]), mode)
            self.assertTrue(result["usable_treatment"])
            self.assertEqual(result["counters"]["mixed"], 2 if mode == "mix" else 0)

    def test_real_invalid_epoch_stays_invalid(self):
        result = self.call(*fixture(self.reports[2], 99, 100))
        self.assertFalse(result["reference_valid"])
        self.assertFalse(result["delivery_complete"])
        self.assertFalse(result["usable_treatment"])
        self.assertEqual(result["reason"], "unpaired_output")

    def test_real_partial_reference_is_not_accepted(self):
        result = self.call(*fixture(self.reports[3], 1, 0))
        self.assertFalse(result["usable_treatment"])
        self.assertIn("reference_output_incomplete", result["issues"])

    def test_off_means_absent_counters(self):
        rows, proof = fixture(self.reports[1]); rows.pop()
        result = self.call(rows, proof, "off")
        self.assertIsNone(result["counters"])
        self.assertFalse(result["usable_treatment"])

    def test_attribution_and_line_faults_refuse(self):
        mutations = {"_PID": "124", "_BOOT_ID": "c" * 32, "_EXE": "/other",
                     "_SYSTEMD_UNIT": "other.service", "_TRANSPORT": "syslog", "_STREAM_ID": "c" * 32,
                     "_LINE_BREAK": "pid-change", "__REALTIME_TIMESTAMP": "203"}
        for key, value in mutations.items():
            with self.subTest(key=key), self.assertRaises(ValueError):
                rows, proof = fixture(self.reports[1]); rows[1][key] = value; self.call(rows, proof)

    def test_duplicate_missing_wrong_mode_and_disabled_record_refuse(self):
        for mutation, mode in ((lambda r: r.append(copy.deepcopy(r[1])), "mix"),
                               (lambda r: r.pop(), "mix"), (lambda r: None, "observe"),
                               (lambda r: None, "off")):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                rows, proof = fixture(self.reports[1]); mutation(rows); self.call(rows, proof, mode)

    def test_impossible_and_overflow_counters_refuse(self):
        for old, new in (("received=3", "received=0"), ("committed=3", "committed=4"),
                         ("warmup=1", "warmup=2"), ("mixed=2", "mixed=0"),
                         ("clipped=0", "clipped=321"), ("pending=0", "pending=2"),
                         ("received=3", "received=" + str(2**64)), ("reason=none", "reason=unpaired_output")):
            with self.subTest(old=old, new=new), self.assertRaises(ValueError):
                self.call(*fixture(self.reports[1].replace(old, new)))

    def test_incomplete_transport_is_separate_from_reference_validity(self):
        for field in ("transport_error_observed", "pending_output_observed", "partial_input_observed"):
            rows, proof = fixture(self.reports[1]); proof[field] = True
            result = self.call(rows, proof)
            self.assertTrue(result["reference_valid"])
            self.assertFalse(result["delivery_complete"])
            self.assertFalse(result["usable_treatment"])

    def test_extra_fallback_volume_cannot_pass(self):
        result = self.call(*fixture(self.reports[1], 3, 4))
        self.assertTrue(result["reference_valid"])
        self.assertFalse(result["delivery_complete"])

    def test_report_before_exit_refuses(self):
        for reorder in (False, True):
            with self.subTest(reorder=reorder), self.assertRaises(ValueError):
                rows, proof = fixture(self.reports[1])
                if reorder: rows.reverse()
                else: rows[1]["__REALTIME_TIMESTAMP"] = "199"
                self.call(rows, proof)

    def test_wrong_journal_or_unverified_transport_refuse(self):
        for field, value in (("journal_sha256", "0" * 64), ("verified", False), ("enabled", False)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                rows, proof = fixture(self.reports[1]); proof[field] = value
                self.call(rows, proof, rehash=field != "journal_sha256")


if __name__ == "__main__": unittest.main()
