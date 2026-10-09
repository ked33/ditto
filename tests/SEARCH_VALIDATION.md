# Search regression checks

Run `python tests/search_sql_regression.py` with Python 3.12+ and SQLite built
with FTS5/trigram support. Tests use only generated in-memory or temporary
databases. They extract the production SQL factories from
`src/SearchIndexSql.cpp`; they never open the user's clipboard database.

Run `python tests/search_static_checks.py` for source delimiter, project
registration and search-message checks. These are not compiler checks.

Coverage includes v1-to-v2 migration and rollback, preservation of complete
long text and raw clipboard data, index synchronization, rebuilding a missing
index, description-only reads, Unicode/short queries, combined format filtering,
and cancellation of an isolated read connection while a writer remains usable.
The SQLite cancellation test validates the database behavior, not the Windows
message loop or the C++ worker implementation.

The MFC application is compiled by the existing GitHub Actions workflows.
After installing that build, check these UI flows:

- Type and delete quickly while a full-text search/count is running; only the
  latest query may update the rows or total. Clearing the box restores history.
- Search one/two Chinese characters in descriptions, then use `/f ` for full
  text. Test simple, Boolean and regular-expression modes, including quotes,
  literal percent signs and Windows paths.
- Combine `!!img` / `!!file` with text; verify grouping, sticky ordering,
  pagination, select-all and delayed tooltips.
- Hide and reopen a preserved search view while its first page or total is
  loading; scrolling must still load later pages.
- Switch the database, then search again. Clipboard capture and pasting must
  continue while searches are cancelled.

The first v2 launch migrates the cached full text and removes the old combined
search cache/index in one transaction. Original `Main` / `Data` records are
not changed. No journal-mode setting is changed. Full-text queries without a
three-character literal run (including one/two-character terms) still require
scanning the full-text cache; description searches use only the small Main table.
