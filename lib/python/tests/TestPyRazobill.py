#!/usr/bin/env python3
# ==============================================================================
# pyrazobill unit and integration test suite
#
# validates exact invertibility, template extraction, xxHash64 bit parity,
# edge cases, actionable error model, and public surface budget compliance
# ==============================================================================

"""unit and integration test suite for pyrazobill Python library"""

import sys
import unittest
from pathlib import Path

# add lib/python directory to sys.path
pkg_dir = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(pkg_dir))

import pyrazobill as rz
import pyrazobill._fallback as _fallback


class TestPyRazobill(unittest.TestCase):
    """unit and integration test cases for pyrazobill public API and fallback"""

    def test_split_and_join_str(self):
        # pins exact lossless invertibility for unicode str inputs
        msg = "Failed password for root from 192.168.1.100 port 22 ssh2"
        rec = rz.RzSplit(msg)

        self.assertIsInstance(rec, rz.RzRecord)
        self.assertIsInstance(rec.template, str)
        self.assertEqual(rec.template, "Failed password for {s} from {s} port {i} {s}")
        self.assertEqual(rec.ints, [22])
        self.assertEqual(rec.strs, ["root", "192.168.1.100", "ssh2"])

        # invertibility via tuple unpacking and direct record pass
        self.assertEqual(rz.RzJoin(*rec), msg)
        self.assertEqual(rz.RzJoin(rec), msg)

    def test_split_and_join_bytes(self):
        # pins exact lossless invertibility for binary bytes inputs
        b_msg = b"Failed password for admin from 10.0.0.1 port 2222"
        rec = rz.RzSplit(b_msg)

        self.assertIsInstance(rec.template, bytes)
        self.assertEqual(rec.template, b"Failed password for {s} from {s} port {i}")
        self.assertEqual(rec.ints, [2222])
        self.assertEqual(rec.strs, [b"admin", b"10.0.0.1"])

        self.assertEqual(rz.RzJoin(*rec), b_msg)
        self.assertEqual(rz.RzJoin(rec), b_msg)

    def test_template_fast_path(self):
        # pins template extraction parity without allocating token containers
        msg = "Accepted publickey for ubuntu from 10.1.2.3 port 54322 ssh2"
        tmpl_str = rz.RzTemplate(msg)
        self.assertEqual(tmpl_str, "Accepted publickey for {s} from {s} port {i} {s}")

        b_msg = b"Accepted publickey for ubuntu from 10.1.2.3 port 54322 ssh2"
        tmpl_bytes = rz.RzTemplate(b_msg)
        self.assertEqual(tmpl_bytes, b"Accepted publickey for {s} from {s} port {i} {s}")

    def test_hash_xxh64(self):
        # pins xxHash64 bit-identity (seed 0) against DuckDB and ClickHouse
        tmpl = "Failed password for {s} from {s} port {i}"
        expected_hash = 54855824174769941
        self.assertEqual(rz.RzHash(tmpl), expected_hash)
        self.assertEqual(rz.RzHash(tmpl.encode("utf-8")), expected_hash)
        self.assertEqual(rz.hash(tmpl), expected_hash)

        # invalid type check
        with self.assertRaises(rz.RzError):
            rz.RzHash(12345)  # type: ignore

    def test_split_many_batch(self):
        # pins multi-line batch decomposition and record invertibility
        lines = [f"Receiving block blk_{i} src: /10.0.0.1:{50000 + i} dest: /10.0.0.2:50010" for i in range(500)]
        records = rz.RzSplitMany(lines)
        self.assertEqual(len(records), 500)
        for i, rec in enumerate(records):
            self.assertEqual(rz.RzJoin(rec), lines[i])

    def test_modes_semantic_vs_exact(self):
        # pins semantic template normalization and exact mode recovery
        line1 = "Failed password for root from 10.0.0.1 port 22"
        line2 = "Failed password for admin from 10.0.0.1 port 22"

        rec1_sem = rz.RzSplit(line1, mode="semantic")
        rec2_sem = rz.RzSplit(line2, mode="semantic")
        self.assertEqual(rec1_sem.template, rec2_sem.template)
        self.assertEqual(rz.RzJoin(rec1_sem), line1)
        self.assertEqual(rz.RzJoin(rec2_sem), line2)

        rec1_ex = rz.RzSplit(line1, mode="exact")
        self.assertEqual(rz.RzJoin(rec1_ex), line1)

    def test_edge_cases_empty(self):
        # pins empty string and empty byte sequence round-trip
        rec_str = rz.RzSplit("")
        self.assertEqual(rec_str.template, "")
        self.assertEqual(rec_str.ints, [])
        self.assertEqual(rec_str.strs, [])
        self.assertEqual(rz.RzJoin(rec_str), "")

        rec_bytes = rz.RzSplit(b"")
        self.assertEqual(rec_bytes.template, b"")
        self.assertEqual(rec_bytes.ints, [])
        self.assertEqual(rec_bytes.strs, [])
        self.assertEqual(rz.RzJoin(rec_bytes), b"")

    def test_edge_cases_quoted_backslashes(self):
        # pins quote parsing, escaped backslashes, and unclosed quote fallback
        cases = [
            '"\\"',
            "'\\'",
            '"\\""',
            "'\\''",
            '"\\\\"',
            '"\\\\\\"',
            'opened "C:\\Users\\bob\\" ok',
            "opened 'C:\\Users\\bob\\' ok",
            'foo "bar\\" baz',
            '"""',
            "'''",
            '"\\\\\\""',
        ]
        for s in cases:
            # native c++ engine
            rec = rz.RzSplit(s)
            self.assertEqual(rz.RzJoin(*rec), s)
            self.assertEqual(rz.RzJoin(rec), s)

            # pure-python fallback engine
            fb_rec = _fallback.split(s, "semantic")
            self.assertEqual(_fallback.join(fb_rec[0], fb_rec[1], fb_rec[2]), s)

    def test_edge_cases_invalid_utf8_and_nul(self):
        # pins binary payload with invalid UTF-8 and embedded NUL bytes
        raw = b"Log \xff\xfe with \x00 NUL and 12345 value \x80\x81\x82"
        rec = rz.RzSplit(raw)
        self.assertIsInstance(rec.template, bytes)
        self.assertEqual(rz.RzJoin(*rec), raw)
        self.assertEqual(rz.RzJoin(rec), raw)

    def test_edge_cases_leading_zeros(self):
        # pins string promotion for tokens with leading zeros to prevent loss
        msg = "Padded IDs: 00123, 000, 0, and 456"
        rec = rz.RzSplit(msg)
        # leading zeros are preserved as strings to prevent precision distortion
        self.assertEqual(rz.RzJoin(rec), msg)

    def test_edge_cases_oversized_integers(self):
        # pins string promotion for integers > 19 digits or > INT64_MAX to prevent overflow
        msg = "Large token 9999999999999999999999999999999999999999 end"
        rec = rz.RzSplit(msg)
        self.assertEqual(rz.RzJoin(rec), msg)

        # 19 digits up to INT64_MAX extracted as integer slot {i}
        max_int_msg = "Max: 9223372036854775807"
        rec_max = rz.RzSplit(max_int_msg)
        self.assertEqual(rec_max.ints, [9223372036854775807])
        self.assertEqual(rec_max.template, "Max: {i}")
        self.assertEqual(rz.RzJoin(rec_max), max_int_msg)
        fb_max = _fallback.split(max_int_msg, "exact")
        self.assertEqual(fb_max[1], [9223372036854775807])
        self.assertEqual(fb_max[0], "Max: {i}")
        self.assertEqual(_fallback.join(*fb_max), max_int_msg)

        # 19 digits exceeding INT64_MAX extracted as string slot {s}
        overflow_msg = "Overflow: 9223372036854775808"
        rec_ovf = rz.RzSplit(overflow_msg)
        self.assertEqual(rec_ovf.ints, [])
        self.assertEqual(rec_ovf.strs, ["9223372036854775808"])
        self.assertEqual(rec_ovf.template, "Overflow: {s}")
        self.assertEqual(rz.RzJoin(rec_ovf), overflow_msg)
        fb_ovf = _fallback.split(overflow_msg, "exact")
        self.assertEqual(fb_ovf[1], [])
        self.assertEqual(fb_ovf[2], ["9223372036854775808"])
        self.assertEqual(fb_ovf[0], "Overflow: {s}")
        self.assertEqual(_fallback.join(*fb_ovf), overflow_msg)

    def test_multiline_keyword_reset(self):
        # newline stops context keyword from bleeding to next line
        msg = "for\n\nalice"
        rec = rz.RzSplit(msg, mode="semantic")
        self.assertEqual(rec.template, "for\n\nalice")
        self.assertEqual(rec.strs, [])
        fb_rec = _fallback.split(msg, "semantic")
        self.assertEqual(fb_rec[0], "for\n\nalice")
        self.assertEqual(fb_rec[2], [])

    def test_edge_cases_literal_braces(self):
        # pins lossless round-trip of literal single and doubled curly braces
        msg = "Payload: {json_key: 123} and {{escaped}} braces {single"
        rec = rz.RzSplit(msg)
        self.assertEqual(rz.RzJoin(rec), msg)

    def test_error_handling(self):
        # pins actionable four-part error message formatting on invalid inputs
        # unsupported type
        with self.assertRaises(rz.RzError) as ctx:
            rz.RzSplit(12345)
        self.assertIn("Unsupported input type", str(ctx.exception))
        self.assertIn("What to do", str(ctx.exception))

        # invalid mode
        with self.assertRaises(rz.RzError) as ctx:
            rz.RzSplit("test", mode="unsupported")
        self.assertIn("Invalid scan mode", str(ctx.exception))

        # join count mismatch: too few ints
        with self.assertRaises(rz.RzError) as ctx:
            rz.RzJoin("Port {i} and {i}", [80], [])
        self.assertIn("Count mismatch", str(ctx.exception))
        self.assertIn("Offending template", str(ctx.exception))

        # join count mismatch: too many strs
        with self.assertRaises(rz.RzError) as ctx:
            rz.RzJoin("User {s}", [], ["root", "extra"])
        self.assertIn("Count mismatch", str(ctx.exception))

        # malformed template: unclosed brace
        with self.assertRaises(rz.RzError) as ctx:
            rz.RzJoin("Unclosed { brace", [], [])
        self.assertIn("Malformed template syntax", str(ctx.exception))

    def test_surface_budget(self):
        # pins strict API surface budget of <= 8 exported names in __all__
        self.assertLessEqual(len(rz.__all__), 8)
        expected_names = {
            "RzSplit",
            "RzJoin",
            "RzTemplate",
            "RzHash",
            "RzSplitMany",
            "RzRecord",
            "RzError",
            "__version__",
        }
        self.assertEqual(set(rz.__all__), expected_names)

        # ergonomic aliases available
        self.assertIs(rz.split, rz.RzSplit)
        self.assertIs(rz.join, rz.RzJoin)
        self.assertIs(rz.template, rz.RzTemplate)
        self.assertIs(rz.hash, rz.RzHash)
        self.assertIs(rz.split_many, rz.RzSplitMany)
        self.assertIs(rz.Record, rz.RzRecord)
        self.assertIs(rz.Error, rz.RzError)

    def test_zero_banned_terms(self):
        # pins complete absence of banned internal jargon in public docstrings
        banned = [
            "entropy",
            "ledger",
            "latitude",
            "doubt",
            "conveyance",
            "leakage",
            "channel capacity",
            "miller-madow",
            "certified interval",
            "shannon",
            "arena",
            "slot",
            "form key",
        ]
        doc = (rz.__doc__ or "").lower()
        for term in banned:
            self.assertNotIn(term, doc, f"Banned term '{term}' found in module docstring")

        for name in rz.__all__:
            obj = getattr(rz, name)
            obj_doc = (getattr(obj, "__doc__", "") or "").lower()
            for term in banned:
                self.assertNotIn(term, obj_doc, f"Banned term '{term}' found in docstring of {name}")

    def test_split_many_ephemeral_sequence(self):
        # verifies that ephemeral Python bytes objects generated on-the-fly
        # remain valid throughout parallel worker thread execution
        class EphemeralBytesSequence:
            def __init__(self, count):
                self._count = count

            def __len__(self):
                return self._count

            def __getitem__(self, idx):
                if idx < 0 or idx >= self._count:
                    raise IndexError(idx)
                return f"event-{idx} timestamp={idx} msg=test".encode("utf-8")

        seq = EphemeralBytesSequence(300)
        results = rz.RzSplitMany(seq)
        self.assertEqual(len(results), 300)
        for i, rec in enumerate(results):
            self.assertEqual(rec.template, b"{s} timestamp={i} msg={s}")
            self.assertEqual(rec.ints, [i])
            self.assertEqual(rec.strs, [f"event-{i}".encode("utf-8"), b"test"])
            self.assertEqual(rz.RzJoin(rec.template, rec.ints, rec.strs), f"event-{i} timestamp={i} msg=test".encode("utf-8"))

    def test_join_ephemeral_bytes_sequence(self):
        # verifies that ephemeral bytes yielded during join iteration are kept alive
        # and do not cause use-after-free or memory corruption
        def ephemeral_gen():
            yield f"ephemeral-val-{42}".encode("utf-8")

        res = rz.RzJoin("prefix {s} suffix", [], list(ephemeral_gen()))
        self.assertEqual(res, "prefix ephemeral-val-42 suffix")

        res_bytes = rz.RzJoin(b"prefix {s} suffix", [], list(ephemeral_gen()))
        self.assertEqual(res_bytes, b"prefix ephemeral-val-42 suffix")

    def test_join_negative_int_rejected(self):
        # verifies negative integer in slot raises RzError in C++ and fallback
        with self.assertRaises(rz.RzError):
            rz.RzJoin("slot {i}", [-1], [])
        with self.assertRaises(ValueError):
            _fallback.join("slot {i}", [-1], [])

    def test_native_and_fallback_hash_parity(self):
        # verifies that native C++ RzHash matches pure-Python fallback bitwise across diverse inputs
        samples = [
            "",
            "a",
            "hello world",
            "Failed password for root from 192.168.1.100 port 22",
            "1234567890",
            "Special chars: !@#$%^&*()_+-=[]{}|;':\",./<>?",
            "Multi-byte UTF-8: 🚀 \u2603 \U0001f600 \u4e16\u754c",
            "A" * 1024,
            "0" * 8192,
        ]
        for s in samples:
            native_val = rz.RzHash(s)
            fallback_val = _fallback.hash(s)
            self.assertEqual(
                native_val,
                fallback_val,
                f"Hash mismatch on sample {s!r}: native={native_val} fallback={fallback_val}",
            )
            s_bytes = s.encode("utf-8")
            self.assertEqual(
                rz.RzHash(s_bytes),
                _fallback.hash(s_bytes),
                f"Hash mismatch on bytes {s_bytes!r}",
            )


if __name__ == "__main__":
    unittest.main()


