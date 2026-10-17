#!/usr/bin/env python3
# =============================================================================
# end-to-end validation of Razobill AWS S3 Parquet export and query
#
# streams decomposed log columns directly to S3 Parquet, executes in-S3 filter
# predicates, and verifies byte-for-byte exact reconstruction over S3 Parquet
# ~ بنصدر لـ s3 ونقرأ تاني ونتأكد إن مافيش ولا بايت طار في السكة
# =============================================================================

import argparse
from pathlib import Path
import re
import sys

import duckdb

# setup paths
S3_TESTS_DIR = Path(__file__).resolve().parent
TESTS_DIR = S3_TESTS_DIR.parent
ROOT_DIR = TESTS_DIR.parent

sys.path.insert(0, str(S3_TESTS_DIR))
sys.path.insert(0, str(TESTS_DIR))
sys.path.insert(0, str(ROOT_DIR / "lib" / "python"))
sys.path.insert(0, str(ROOT_DIR))

from S3ClientConfig import configureDuckDbS3  # noqa: E402

try:
    import pyrazobill

    HAS_PYRAZOBILL = True
except ImportError:
    HAS_PYRAZOBILL = False

# Razobill canonical ASCII control characters
kPlaceholder = "\x12"
kTypeSeparator = "\x13"


# decomposes msg into (form, ints, strs) using pyrazobill or reference fallback
def parseRazobill(msg: str):
    """Decompose log message into form key, integers, and string tokens.

    inv: join(split(msg)) == msg
    in:  input log message string
    out: tuple (form_key, ints_list, strs_list)
    """
    if HAS_PYRAZOBILL:
        rec = pyrazobill.RzSplit(msg)
        return rec.template, rec.ints, rec.strs

    # Fallback tokenizer
    tokens = []
    delimiters = []
    last_end = 0
    for match in re.finditer(r"[A-Za-z0-9_./:-]+", msg):
        start, end = match.span()
        delimiters.append(msg[last_end:start])
        tokens.append(match.group(0))
        last_end = end
    tail = msg[last_end:]

    skeleton_parts = []
    types_list = []
    ints_list = []
    strs_list = []

    def isIntToken(tok: str) -> bool:
        if not tok or len(tok) > 18:
            return False
        if tok[0] == "0":
            return len(tok) == 1
        return tok.isdigit()

    for gap, tok in zip(delimiters, tokens):
        skeleton_parts.append(gap)
        if isIntToken(tok):
            val = int(tok)
            skeleton_parts.append(kPlaceholder)
            types_list.append("I")
            ints_list.append(val)
            continue

        if (
            re.fullmatch(r"\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}", tok)
            or re.fullmatch(r"[0-9a-fA-F]{8,}", tok)
            or re.fullmatch(r"\d{4}-\d{2}-\d{2}", tok)
            or re.fullmatch(r"\d{2}:\d{2}:\d{2}", tok)
            or re.fullmatch(r"\d+", tok)
            or "/" in tok
            or tok.startswith("blk_")
        ):
            skeleton_parts.append(kPlaceholder)
            types_list.append("S")
            strs_list.append(tok)
        else:
            skeleton_parts.append(tok)

    skeleton_parts.append(tail)
    skeleton = "".join(skeleton_parts)
    types_str = "".join(types_list)
    form = skeleton + kTypeSeparator + types_str
    return form, ints_list, strs_list


# losslessly reconstructs exact original message from form, ints, and strs
def reconstructRazobill(form: str, ints: list, strs: list) -> str:
    """Reconstruct exact original log message from form key and slot arrays.

    inv: reconstruct(form, ints, strs) == original
    in:  form template string, list of integer values, list of string values
    out: byte-exact reconstructed message string
    """
    if HAS_PYRAZOBILL:
        return pyrazobill.RzJoin(form, ints, strs)
    sep_idx = form.rfind(kTypeSeparator)
    if sep_idx == -1:
        return form
    skeleton = form[:sep_idx]
    types = form[sep_idx + 1 :]

    out = []
    ii = 0
    si = 0
    ti = 0

    for ch in skeleton:
        if ch == kPlaceholder:
            if ti < len(types):
                t = types[ti]
                ti += 1
                if t == "I":
                    out.append(str(ints[ii]))
                    ii += 1
                elif t == "S":
                    out.append(str(strs[si]))
                    si += 1
                else:
                    out.append(ch)
            else:
                out.append(ch)
        else:
            out.append(ch)

    return "".join(out)


# pins exact round-trip reconstruction directly from S3 Parquet files
def testAwsS3Export(endpoint: str = None, bucket: str = "telemetry", count: int = 2000):
    """Verify end-to-end DuckDB export to AWS S3 Parquet and lossless reconstruction.

    inv: all exported rows reconstruct byte-for-byte identically from S3 Parquet
    in:  optional S3 endpoint URL, target bucket name, event count
    out: True on complete verification; raises AssertionError on mismatch
    """
    print("=" * 78)
    print("  RAZOBILL AWS S3 PARQUET EXPORT & ROUNDTRIP VERIFICATION")
    print("=" * 78)

    mock_server = None
    try:
        con = duckdb.connect()
        active_bucket, endpoint_desc, is_real_aws, mock_server = configureDuckDbS3(
            con, bucket=bucket, endpointOverride=endpoint
        )
        print(f"[*] S3 Storage Target: s3://{active_bucket}/ ({endpoint_desc})")

        # Register Razobill scalar functions
        con.create_function("rbForm", lambda m: parseRazobill(m)[0], ["VARCHAR"], "VARCHAR")
        con.create_function("rbInts", lambda m: parseRazobill(m)[1], ["VARCHAR"], "BIGINT[]")
        con.create_function("rbStrs", lambda m: parseRazobill(m)[2], ["VARCHAR"], "VARCHAR[]")
        con.create_function(
            "rbReconstruct",
            reconstructRazobill,
            ["VARCHAR", "BIGINT[]", "VARCHAR[]"],
            "VARCHAR",
        )
        print("[*] Registered Razobill scalar functions: rbForm, rbInts, rbStrs, rbReconstruct")

        # Synthesize production logs
        templates = [
            "081109 2035{sec:02d} {node} INFO dfs.DataNode$DataXceiver: Receiving block blk_{blk} src: /10.250.19.{ip}:{port} dest: /10.250.19.102:50010 seq={seq}",
            "2026-10-07 14:02:{sec:02d} [WARN] Connection to 10.0.0.{ip}:5432 failed after {retries} retries",
            "2026-10-07 14:02:{sec:02d} [INFO] HTTP GET /api/v1/users/{uid} completed with status {status} in {lat} ms",
            "2026-10-07 14:02:{sec:02d} [ERROR] OutOfMemoryError in worker-{node}: heap exhausted limit={mem}MB used={used}MB",
        ]

        sample_logs = []
        for i in range(count):
            tmpl = templates[i % len(templates)]
            sec = i % 60
            ip = (i % 254) + 1
            node = 100 + (i % 50)
            blk = 1000000000 + i
            port = 30000 + (i % 20000)
            seq = i * 7
            retries = (i % 5) + 1
            uid = 1000 + (i % 500)
            status = 200 if (i % 10 != 0) else 500
            lat = 15 + (i % 250)
            mem = 2048
            used = 2040 + (i % 8)
            msg = tmpl.format(
                sec=sec,
                node=node,
                blk=blk,
                ip=ip,
                port=port,
                seq=seq,
                retries=retries,
                uid=uid,
                status=status,
                lat=lat,
                mem=mem,
                used=used,
            )
            sample_logs.append(msg)

        con.execute("CREATE TABLE staging_logs (id INT, msg VARCHAR);")
        con.executemany("INSERT INTO staging_logs VALUES (?, ?);", list(enumerate(sample_logs)))
        print(f"[*] Staged {count:,} log events in memory table.")

        s3_target_uri = f"s3://{active_bucket}/year=2026/month=10/logs.parquet"
        print(f"[*] Executing SQL COPY to AWS S3: {s3_target_uri} ...")

        export_sql = f"""
        COPY (
            SELECT
                id,
                rbForm(msg) AS form,
                rbInts(msg) AS ints,
                rbStrs(msg) AS strs
            FROM staging_logs
        ) TO '{s3_target_uri}' (FORMAT PARQUET, OVERWRITE TRUE);
        """
        con.execute(export_sql)
        print("[+] Parquet file successfully streamed directly to S3!")

        print("[*] Reading and querying Parquet file directly from S3...")
        cnt_row = con.execute(f"SELECT COUNT(*) FROM '{s3_target_uri}';").fetchone()
        row_count = cnt_row[0]
        print(f"[+] Total rows verified in S3 Parquet: {row_count:,} (Expected: {count:,})")
        assert row_count == count, f"Expected {count} rows in S3, found {row_count}"

        print("[*] Running analytical query over S3 integer slots (ints[1] > 200)...")
        res = con.execute(
            f"""
            SELECT
                form,
                COUNT(*) AS cnt,
                AVG(ints[1]) AS avg_val
            FROM '{s3_target_uri}'
            WHERE len(ints) >= 1 AND ints[1] > 200
            GROUP BY form
            ORDER BY cnt DESC
            LIMIT 3;
        """
        ).fetchall()
        print(f"[+] Top aggregated forms from S3:\n    {res}")

        print("[*] Verifying byte-for-byte lossless reconstruction for all rows from S3...")
        rows = con.execute(f"SELECT id, rbReconstruct(form, ints, strs) FROM '{s3_target_uri}' ORDER BY id;").fetchall()

        mismatches = 0
        for row_id, reconstructed_msg in rows:
            orig = sample_logs[row_id]
            if reconstructed_msg != orig:
                mismatches += 1
                if mismatches <= 3:
                    print(f"[-] MISMATCH at ID {row_id}:")
                    print(f"    ORIG: {orig}")
                    print(f"    RECN: {reconstructed_msg}")

        print(f"[+] Bit-level verification complete: {len(rows):,} rows evaluated.")
        print(f"[+] Total Mismatches: {mismatches} (Invertibility: {((count - mismatches) / count) * 100:.3f}%)")
        assert mismatches == 0, f"Encountered {mismatches} mismatches in S3 Parquet reconstruction!"

        print("=" * 78)
        print("  ALL AWS S3 PARQUET EXPORT & LOSSLESS TESTS PASSED (100.000% EXACT)")
        print("=" * 78)
        return True

    finally:
        if mock_server is not None:
            mock_server.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Razobill AWS S3 Parquet Export Validation")
    parser.add_argument(
        "--endpoint",
        type=str,
        default=None,
        help="Custom S3 endpoint URL (or 'aws' for live AWS)",
    )
    parser.add_argument("--bucket", type=str, default="telemetry", help="Target S3 bucket name")
    parser.add_argument("--count", type=int, default=2000, help="Number of log events to test")
    args = parser.parse_args()

    testAwsS3Export(endpoint=args.endpoint, bucket=args.bucket, count=args.count)
