# Storage, search and preview validation

These checks use synthetic data. They never open the user's clipboard database.

Run with Python 3.12+:

```text
python -B tests/search_static_checks.py
python -B tests/database_optimization_regression.py
python -B tests/search_paging_regression.py
python -B tests/preview_regression.py
python -B tests/copy_source_regression.py
python -B tests/search_sql_regression.py
python -B tests/search_filter_regression.py
python -B tests/theme_package_checks.py
```

Set `DITTO_ICU_EXTENSION` to a matching cloud-built `ICU_Loader.dll` for the
complete literal-search and web-link cases. Both portable and PR workflows
compile the MFC application, then run search tests with that build's DLL.
The workflows also run `Test-WorkerUtilities.ps1`, which compiles and executes
the actual sparse-cache template and cancelable WinSock helpers with MSVC
`/W4 /WX`. Socket checks use only localhost and loopback connections.

The changes cover these eight areas:

1. `DittoSchemaMigrations` records the sticky-order conversion once. Existing
   databases with the newer sorting indexes preserve legitimate zero positions.
2. PNG and DIB thumbnails share UI-owned LRU eviction: at most 50 items, trimmed
   to 30 after overflow, with a 32 MiB image budget and an 8 MiB RTF budget.
   Thumbnails also respect the available display width. Full saved data remains
   intact, including when an oversized RTF row falls back to its description.
3. A sparse cache holds at most 512 complete rows. Bulk actions resolve IDs
   without fetching descriptions or BLOBs. Changed row-to-ID anchors abort the
   action and refresh the view instead of mixing old and new result positions.
4. Saves, edits and copies use serialized transactions and nested savepoints;
   format or full-text update failure rolls back the record. Clipboard identity
   caches update only after an outer commit. No transaction spans an editor dialog.
5. Format-first and deletion-queue indexes serve exact clipboard type filters
   and deferred cleanup. Parent-first and existing web-link indexes are retained.
6. Full previews use a separate reader, request generations and background image
   decoding. Only the selected format's BLOB is loaded. Text word counts are
   calculated in the worker; RTF uses the rendered text; HTML uses saved bounds.
   Visible pinned previews can finish while the main window is hidden.
7. Exact counts use a separate worker and connection. A short first page gives
   the exact count immediately; otherwise the list supports progressive browsing.
8. Maintenance uses 25-row Main batches, a 100 ms budget checked between batches,
   and yields to pending clipboard saves. Deferred BLOB deletion retains idle
   checks and small batches. Individual SQL statements can exceed the budget.

Cancellation discards stale results; it cannot interrupt an individual GDI+
decode already in progress. Shutdown joins workers before freeing their data.
Automatic network sends use cancelable DNS, connect and send operations so a
blocked receiver cannot prevent that join indefinitely.

After installing the complete cloud-built package, exercise these UI cases:

- Browse more than 50 PNG-only images, switch searches rapidly, and hide/reopen
  the window during loading; inspect memory and ensure new visible rows update.
- Jump to a distant page, select all, and exercise copy/export/delete on a test
  database. Insert a new clip while selection is pending; stale bulk actions
  must refresh without touching unintended IDs.
- Change sticky/group order, delete selected rows, and paste with and without
  the preserved-search option. Selection must follow clip identity.
- Search frequent, short, regex and no-match terms; scroll before count completion.
- Open large text, RTF, HTML and image previews; switch formats and selection,
  keep a pinned preview visible while hiding the main window, and switch databases.
  Also verify format menus in the Delete Clip Data dialog.
- Save from the internal and external editors; failed saves must retain edits
  and must not be announced as successful. Cancel a new clip's property dialog.
- Exit during copying, image decoding, cleanup and automatic network sending.

Python SQL models and source checks do not validate the Windows message loop,
MFC object ownership, actual disk throughput or GUI latency. The native build
and helper smoke tests add compilation and helper behavior coverage; installed
application interaction still requires the UI checks above.
