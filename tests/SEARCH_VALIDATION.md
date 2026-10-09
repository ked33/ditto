# Search regression checks

Run `python tests/search_sql_regression.py` with Python 3.12+ and SQLite built
with FTS5/trigram support. Set `DITTO_ICU_EXTENSION` to the freshly compiled
`ICU_Loader.dll` to test the complete v3 schema and real character tokenizer.
Without that DLL, the script explicitly skips the five literal-index tests
and exercises only the cache/trigram portion of the schema. GitHub Actions
runs all 18 schema/search tests and the 6 filter tests with the compiled DLL before packaging.
Tests use only generated in-memory or temporary
databases. They extract the production SQL factories from
`src/SearchIndexSql.cpp`; they never open the user's clipboard database.

Run `python tests/search_filter_regression.py` with the same DLL to check
the production `!!link` and `!!rtf` SQL templates. Link cases cover whole-body
HTTP/HTTPS/www URLs, whitespace, Unicode URLs, ANSI-only clips, Unicode format
precedence, misleading/truncated descriptions, both index-ready and fallback
paths, combined keyword/group filters, counts and pagination. Decoder tests use
both UTF-8 and UTF-16 databases. RTF checks reject HTML-only clips, `.rtf` file
lists and ordinary text mentioning RTF, and keep duplicate formats from adding
duplicate results. Without the DLL, only RTF SQL and source wiring checks run.

`!!link` ignores leading/trailing whitespace and accepts only a whole web URL;
prose containing a URL does not match. Recognition checks saved text, not the
description or HTML markup. It does not connect to the URL or check availability.
The indexed path reads the complete Unicode cache; the fallback reads Unicode
clipboard BLOBs. Both paths also handle ANSI-only text. `!!rtf` requires an actual
`Rich Text Format` data row and does not include HTML-only data. These prefixes
are case insensitive and require end-of-input or whitespace: `!!linkage` and
`!!rtf123` retain their original quick-paste-name meaning. Trailing keywords use
the existing search scope and mode, e.g. `!!link /f github` or `!!rtf report`.
Whole-URL recognition uses a fixed anchored ICU regex, not the literal FTS index.

Run `python tests/search_static_checks.py` for source delimiter, project
registration and search-message checks. These are not compiler checks.

Coverage includes v1/v2-to-v3 migration and rollback, preservation of complete
long text and raw clipboard data, index synchronization, rebuilding a missing
index, description-only reads, Unicode/short queries, combined format filtering,
and cancellation of an isolated read connection while a writer remains usable.
Literal-index tests compare against the actual ICU LIKE function, including
one/two-character terms, ASCII/Unicode case folding, accents, emoji, whitespace,
quotes, punctuation, random substrings, and text past a NUL. A multi-megabyte
article fixture checks its final words while denying reads from the external
text cache, proving that MATCH does not silently fall back to a content scan.
The SQLite cancellation test validates the database behavior, not the Windows
message loop or the C++ worker implementation.

The MFC application is compiled by the existing GitHub Actions workflows.
After installing that build, check these UI flows:

- Type and delete quickly while a full-text search/count is running; only the
  latest query may update the rows or total. Clearing the box restores history.
- Search one/two Chinese characters in descriptions, then use `/f ` for full
  text. Test simple, Boolean and regular-expression modes, including quotes,
  literal percent signs and Windows paths.
- Combine `!!img` / `!!file` / `!!link` / `!!rtf` with text; verify grouping, sticky ordering,
  pagination, select-all and delayed tooltips.
- Hide and reopen a preserved search view while its first page or total is
  loading; scrolling must still load later pages.
- Switch the database, then search again. Clipboard capture and pasting must
  continue while searches are cancelled.

The first v3 launch builds character positions from the existing v2 full-text
cache in one transaction. This is a one-time startup cost, independent of search
terms. It does not reload the original clipboard BLOBs. New/edited/deleted clips
update the index through cache triggers. Original `Main` / `Data` records and
journal-mode settings are not changed. Install the entire portable package,
including the matching `ICU_Loader.dll` that registers the tokenizer on every
database connection.

Simple contains queries of 1-256 UTF-16 code units use an FTS quoted character
phrase. All characters, including punctuation and whitespace, occupy a position;
ICU simple case folding matches the existing LIKE implementation. The query
returns row IDs without fetching large cached articles. Each quote is escaped
for both the FTS phrase and SQL string. Empty/longer queries, the existing `_`
LIKE wildcard, Boolean/wildcard mode and regex retain their previous path and
may remain slow. No article or fallback query is truncated.

The index stores character positions, so disk space and text-update work grow
with total text length. An initial model over about 91 million cached characters
used about 114 MiB of additional disk space and built in 19 seconds; model
queries for two Chinese characters, `no`, and `demo` took about 1, 43, and 93 ms.
These are exploratory model timings, not app UI latency or native DLL timings.
Cold storage, concurrent clipboard writes, initial migration, and unusually
common/long phrases still need observation in the installed application.
