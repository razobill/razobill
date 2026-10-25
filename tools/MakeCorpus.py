#!/usr/bin/env python3
"""Deterministic SYNTHETIC test corpus for tests/IntegrationDuckdb.cpp.

Writes /tmp/corpus.parquet (or argv[1]) with exactly 134,815 rows and columns
(msg VARCHAR, level VARCHAR), modelled on three public LogHub templates
(HDFS, OpenSSH, Linux auth). It is generated with integer hashing only, so it is
bit-identical on every machine. It is NOT real production data: it exists so the
integration suite executes everywhere instead of skipping.
"""
import os
import sys
import tempfile

import duckdb

if len(sys.argv) > 1:
    OUT = sys.argv[1]
else:
    OUT = "/tmp/corpus.parquet" if os.path.exists("/tmp") else os.path.join(tempfile.gettempdir(), "corpus.parquet")
OUT = OUT.replace("\\", "/")
ROWS = 134815
con = duckdb.connect()
con.execute(
    f"""
COPY (
  WITH n AS (SELECT i, hash(i) AS h FROM range({ROWS}) t(i))
  SELECT
    CASE i % 10
      WHEN 0 THEN 'Failed password for ' || (['root','admin','test','guest','oracle'])[1 + (h % 5)::INT]
                  || ' from 10.' || (h % 250) || '.' || ((h >> 8) % 250) || '.' || ((h >> 16) % 250)
                  || ' port ' || (1024 + (h >> 24) % 60000) || ' ssh2'
      WHEN 1 THEN 'authentication failure; logname= uid=0 euid=0 tty=NODEVssh ruser= rhost=10.'
                  || (h % 250) || '.' || ((h >> 8) % 250) || '.1  user=' || (['root','admin','bob'])[1 + (h % 3)::INT]
      WHEN 2 THEN 'Accepted publickey for ' || (['alice','bob','carol'])[1 + (h % 3)::INT]
                  || ' from 192.168.' || (h % 250) || '.' || ((h >> 8) % 250) || ' port ' || (2000 + (h >> 20) % 50000) || ' ssh2'
      WHEN 3 THEN 'Connection closed by 10.' || (h % 250) || '.' || ((h >> 8) % 250) || '.7 port ' || (1024 + (h >> 24) % 60000) || ' [preauth]'
      WHEN 4 THEN 'Received disconnect from 10.' || (h % 250) || '.' || ((h >> 8) % 250) || '.9: 11: Bye Bye'
      WHEN 5 THEN 'pam_unix(sshd:session): session opened for user ' || (['alice','bob','carol'])[1 + (h % 3)::INT] || ' by (uid=0)'
      ELSE 'Receiving block blk_' || (CASE WHEN h % 2 = 0 THEN '-' ELSE '' END) || ((h >> 4) % 9000000000000000000)
           || ' src: /10.250.' || (h % 250) || '.' || ((h >> 8) % 250) || ':' || (50000 + (h >> 12) % 10000)
           || ' dest: /10.250.' || ((h >> 3) % 250) || '.' || ((h >> 11) % 250) || ':50010'
    END AS msg,
    CASE WHEN i % 10 = 0 THEN 'WARN' WHEN i % 10 = 1 THEN 'ERROR' WHEN i % 10 = 2 THEN 'INFO' WHEN h % 97 = 0 THEN 'ERROR' ELSE 'INFO' END AS level
  FROM n ORDER BY i
) TO '{OUT}' (FORMAT PARQUET)
"""
)
print(f"wrote {OUT}: {con.execute(f'SELECT count(*) FROM read_parquet({chr(39)}{OUT}{chr(39)})').fetchone()[0]} rows")
