"""Read-only clipboard benchmark; all index writes stay in a temporary database.

Example:
  python tests/search_literal_benchmark.py --database path/to/Ditto.db \
      --icu-extension path/to/ICU_Loader.dll

Prints only aggregate timings/counts for fixed public example terms. Clipboard
content and row IDs are never printed. The temporary copy is deleted on exit.
"""

import argparse
import json
import sqlite3
import tempfile
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--icu-extension", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="ditto-literal-benchmark-") as directory:
        path = Path(directory) / "benchmark.db"
        target = sqlite3.connect(path)
        try:
            target.enable_load_extension(True)
            # Use the native API: ICU cannot replace LIKE during an active SQL
            # SELECT load_extension(...) statement.
            target.load_extension(str(args.icu_extension.resolve()),
                                  entrypoint="sqlite3_icu_init")
            target.enable_load_extension(False)
            target.execute("CREATE TABLE cache(clipID INTEGER PRIMARY KEY, fulltext TEXT NOT NULL)")
            source = sqlite3.connect(args.database.resolve().as_uri() + "?mode=ro",
                                     uri=True, timeout=1)
            try:
                source.execute("PRAGMA query_only=ON")
                ids = source.execute("SELECT clipID FROM MainFullTextCache").fetchall()
                # Release each read statement before writing to the temporary
                # database, so index construction cannot hold the live DB lock.
                for (clip_id,) in ids:
                    row = source.execute(
                        "SELECT fulltext FROM MainFullTextCache WHERE clipID=?", (clip_id,)).fetchone()
                    if row is not None:
                        target.execute("INSERT INTO cache VALUES(?,?)", (clip_id, row[0]))
            finally:
                source.close()
            target.commit()
            original_bytes = path.stat().st_size
            target.execute("CREATE VIRTUAL TABLE literal USING fts5("
                           "fulltext,content='cache',content_rowid='clipID',"
                           "tokenize='ditto_char',detail='full')")
            start = time.perf_counter()
            target.execute("INSERT INTO literal(literal) VALUES('rebuild')")
            target.commit()
            print(json.dumps({"sqlite": sqlite3.sqlite_version,
                              "rows": len(ids), "build_seconds": round(time.perf_counter() - start, 3),
                              "index_MiB": round((path.stat().st_size - original_bytes) / 1048576, 2)}),
                  flush=True)
            target.execute("CREATE VIRTUAL TABLE previous USING fts5("
                           "fulltext,content='cache',content_rowid='clipID',"
                           "tokenize='trigram',detail='none')")
            target.execute("INSERT INTO previous(previous) VALUES('rebuild')")
            target.commit()
            for term in ("补充", "demo", "no", "补", "n"):
                rowsets = []
                for label, sql, parameter in (
                    ("previous", "SELECT rowid FROM previous WHERE fulltext<>'' AND fulltext LIKE ?", "%" + term + "%"),
                    ("literal", "SELECT rowid FROM literal WHERE literal MATCH ?", '"' + term + '"'),
                ):
                    start = time.perf_counter()
                    rows = {row[0] for row in target.execute(sql, (parameter,))}
                    rowsets.append(rows)
                    print(json.dumps({"term": term, "path": label, "hits": len(rows),
                                      "ms": round((time.perf_counter() - start) * 1000, 2)}), flush=True)
                if rowsets[0] != rowsets[1]:
                    raise RuntimeError("Search result mismatch; row IDs suppressed")
        finally:
            target.close()


if __name__ == "__main__":
    main()
