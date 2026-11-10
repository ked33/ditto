#include "stdafx.h"
#include "QPasteWndThread.h"
#include "Misc.h"
#include "Options.h"
#include "QPasteWnd.h"
#include "cp_main.h"
#include <vector>
#include <algorithm>

CQPasteWndThread::CQPasteWndThread(void)
{
	m_rowHeight = 0;
	m_threadName = "CQPasteWndThread";
    m_waitTimeout = ONE_HOUR * 12;

    m_SearchingEvent = CreateEvent(NULL, TRUE, FALSE, _T(""));

    for(int eventEnum = 0; eventEnum < ECQPASTEWNDTHREADEVENTS_COUNT; eventEnum++)
    {
        AddEvent(eventEnum);
    }
}

CQPasteWndThread::~CQPasteWndThread(void)
{
    CancelSearch();
    CancelExtraData();
    Stop();
    CloseHandle(m_SearchingEvent);
}

void CQPasteWndThread::OnTimeOut(void *param)
{
}

void CQPasteWndThread::OnEvent(int eventId, void *param)
{
	DWORD startTick = GetTickCount();
	Log(StrF(_T("Start of OnEvent, eventId: %s"), EnumName((eCQPasteWndThreadEvents)eventId)));

    switch((eCQPasteWndThreadEvents)eventId)
    {
        case DO_SET_LIST_COUNT:
            OnSetListCount(param);
            break;
        case LOAD_ACCELERATORS:
            OnLoadAccelerators(param);
            break;
        case UNLOAD_ACCELERATORS:
            OnUnloadAccelerators(param);
            break;
        case LOAD_ITEMS:
            OnLoadItems(param);
            break;
        case LOAD_EXTRA_DATA:
            OnLoadExtraData(param);
            break;
    }

	DWORD length = GetTickCount() - startTick;
	Log(StrF(_T("End of OnEvent, eventId: %s, Time: %d(ms)"), EnumName((eCQPasteWndThreadEvents)eventId), length));
}

void CQPasteWndThread::CancelSearch()
{
    {
        ATL::CCritSecLock lock(m_requestLock.m_sect);
        ++m_generation;
        // Keep the query for reopening a preserved search view; its old
        // generation remains invalid until ResumeSearch is called explicitly.
        m_countGeneration = 0;
        SetEvent(m_SearchingEvent);
    }
    {
        // Interrupt only the dedicated reader, never clipboard writes.
        ATL::CCritSecLock lock(m_connectionLock.m_sect);
        m_searchDb.interrupt();
    }
}

void CQPasteWndThread::SetSearchSql(const CString& sql, const CString& countSql, const CString& databasePath)
{
    ATL::CCritSecLock lock(m_requestLock.m_sect);
    m_request.sql = sql;
    m_request.countSql = countSql;
    m_request.databasePath = databasePath;
    m_request.generation = m_generation.load();
    m_request.busyTimeout = CGetSetOptions::GetDbTimeout();
    m_request.countNeeded = !countSql.IsEmpty();
    m_countGeneration = 0;
}

void CQPasteWndThread::FireLoadItemsRequest()
{
    ATL::CCritSecLock lock(m_requestLock.m_sect);
    if (!m_request.sql.IsEmpty() && IsCurrentSearch(m_request.generation))
    {
        ResetEvent(m_SearchingEvent);
        FireEvent(LOAD_ITEMS);
    }
}

void CQPasteWndThread::AcknowledgeListCount(UINT_PTR generation)
{
    ATL::CCritSecLock lock(m_requestLock.m_sect);
    if (IsCurrentSearch(generation))
    {
        m_request.countNeeded = false;
        m_countGeneration = 0;
        SetEvent(m_SearchingEvent);
    }
}

void CQPasteWndThread::ResumeSearch()
{
    ATL::CCritSecLock lock(m_requestLock.m_sect);
    if (m_request.sql.IsEmpty() || IsCurrentSearch(m_request.generation))
        return;
    m_request.generation = m_generation.load();
    if (m_request.countNeeded)
    {
        m_countGeneration = m_request.generation;
        ResetEvent(m_SearchingEvent);
        if (m_countOnly)
            FireSetListCount();
    }
}

void CQPasteWndThread::FireSetListCount()
{
    ATL::CCritSecLock lock(m_requestLock.m_sect);
    if (!m_request.countSql.IsEmpty() && IsCurrentSearch(m_request.generation))
    {
        m_countGeneration = m_request.generation;
        ResetEvent(m_SearchingEvent);
        FireEvent(DO_SET_LIST_COUNT);
    }
}

CQPasteWndThread::SearchRequest CQPasteWndThread::GetSearchRequest()
{
    ATL::CCritSecLock lock(m_requestLock.m_sect);
    return m_request;
}

CQPasteWndThread::SearchConnection::SearchConnection(CQPasteWndThread& owner, const SearchRequest& request) :
    m_owner(owner), m_open(owner.BeginSearchConnection(request))
{
}

CQPasteWndThread::SearchConnection::~SearchConnection()
{
    if (m_open)
        m_owner.EndSearchConnection();
}

bool CQPasteWndThread::BeginSearchConnection(const SearchRequest& request)
{
    ATL::CCritSecLock lock(m_connectionLock.m_sect);
    if (!IsCurrentSearch(request.generation))
        return false;
    m_runningGeneration = request.generation;
    m_busyTimeout = request.busyTimeout;
    try
    {
        // Reopen per batch to release read locks and follow database replacement.
        m_searchDb.open(request.databasePath, true);
        m_searchDb.setProgressHandler(1000, SearchProgress, this);
        m_searchDb.setBusyHandler(SearchBusy, this);
    }
    catch (...)
    {
        m_searchDb.close();
        throw;
    }
    return true;
}

void CQPasteWndThread::EndSearchConnection()
{
    ATL::CCritSecLock lock(m_connectionLock.m_sect);
    m_searchDb.setProgressHandler(0, nullptr, nullptr);
    m_searchDb.setBusyHandler(nullptr, nullptr);
    m_searchDb.close();
}

int CQPasteWndThread::SearchProgress(void* context)
{
    auto thread = static_cast<CQPasteWndThread*>(context);
    return thread->IsCancelled() || !thread->IsCurrentSearch(thread->m_runningGeneration);
}

int CQPasteWndThread::SearchBusy(void* context, int attempts)
{
    auto thread = static_cast<CQPasteWndThread*>(context);
    if (SearchProgress(context) || static_cast<ULONGLONG>(attempts) * 10 >= thread->m_busyTimeout)
        return 0;
    Sleep(10);
    return !SearchProgress(context);
}

void CQPasteWndThread::FinishSearch(UINT_PTR generation)
{
    ATL::CCritSecLock lock(m_requestLock.m_sect);
    if (IsCurrentSearch(generation) && m_countGeneration != generation)
        SetEvent(m_SearchingEvent);
}

void CQPasteWndThread::OnSetListCount(void *param)
{
    CQPasteWnd* pasteWnd = static_cast<CQPasteWnd*>(param);
    const SearchRequest request = GetSearchRequest();
    {
        ATL::CCritSecLock lock(m_requestLock.m_sect);
        if (!IsCurrentSearch(request.generation) || m_countGeneration != request.generation)
            return;
    }

    // This runs on the separate count worker/connection. Page requests can
    // continue while a slow regex or a common search term is being counted.

    static CEvent updateTimeEvent(TRUE, TRUE, _T("Ditto_Update_Clip_Time"), NULL);
    const DWORD waitStart = GetTickCount();
    while (IsCurrentSearch(request.generation) &&
        WaitForSingleObject(updateTimeEvent, 20) == WAIT_TIMEOUT &&
        GetTickCount() - waitStart < 2000) {}
    if (!IsCurrentSearch(request.generation))
        return;

    const DWORD start = GetTickCount();
    try
    {
        SearchConnection connection(*this, request);
        if (connection)
        {
            const int count = m_searchDb.execScalar(request.countSql);
            ATL::CCritSecLock lock(m_requestLock.m_sect);
            if (IsCurrentSearch(request.generation))
            {
                ::PostMessage(pasteWnd->m_hWnd, NM_SET_LIST_COUNT, count, static_cast<LPARAM>(request.generation));
            }
        }
    }
    catch (CppSQLite3Exception& e)
    {
        if (IsCurrentSearch(request.generation) && e.errorCode() != SQLITE_INTERRUPT)
            Log(StrF(_T("Search count failed, SQLite code: %d"), e.errorCode()));
    }
    {
        ATL::CCritSecLock lock(m_requestLock.m_sect);
        if (m_countGeneration == request.generation)
            m_countGeneration = 0;
    }
    FinishSearch(request.generation);
    Log(StrF(_T("Search count completed in %d ms"), GetTickCount() - start));
}

void CQPasteWndThread::OnLoadItems(void *param)
{
    CQPasteWnd* pasteWnd = static_cast<CQPasteWnd*>(param);
    const SearchRequest request = GetSearchRequest();
    if (request.sql.IsEmpty() || !IsCurrentSearch(request.generation))
        return;

    const DWORD start = GetTickCount();
    try
    {
        SearchConnection connection(*this, request);
        if (!connection)
            return;
        while (IsCurrentSearch(request.generation))
        {
            CPoint range;
            {
                ATL::CCritSecLock lock(pasteWnd->m_CritSection.m_sect);
                if (!IsCurrentSearch(request.generation) || pasteWnd->m_loadItems.empty())
                    break;
                range = pasteWnd->m_loadItems.front();
            }
            const bool firstLoad = range.x == -1;
            const int firstRow = max(range.x, 0);
            const int rowCount = min(max(range.y - firstRow, 1), 128);
            CString sql = request.sql;
            sql += StrF(_T(" LIMIT %d OFFSET %d"), rowCount, firstRow);

            // Publish only complete batches belonging to the current request.
            std::vector<CMainTable> rows;
            {
                CppSQLite3Query q = m_searchDb.execQuery(sql);
                while (!q.eof() && IsCurrentSearch(request.generation))
                {
                    CMainTable row;
                    CQPasteWnd::FillMainTable(row, q);
                    rows.push_back(row);
                    q.nextRow();
                }
            }
            {
                ATL::CCritSecLock lock(pasteWnd->m_CritSection.m_sect);
                if (!IsCurrentSearch(request.generation))
                    break;
                for (size_t i = 0; i < rows.size(); ++i)
                    pasteWnd->m_listItems.put(firstRow + static_cast<int>(i), rows[i]);
                if (static_cast<int>(rows.size()) == rowCount && firstRow + rowCount < range.y)
                    pasteWnd->m_loadItems.front().x = firstRow + rowCount;
                else
                    pasteWnd->m_loadItems.pop_front();
            }
            ::PostMessage(pasteWnd->m_hWnd, NM_SEARCH_RESULTS_READY,
                request.generation, firstLoad ? 1 : 0);
            if (firstLoad)
            {
                ATL::CCritSecLock lock(m_requestLock.m_sect);
                if (IsCurrentSearch(request.generation))
                {
                    m_countGeneration = request.generation;
                    if (static_cast<int>(rows.size()) < rowCount)
                        ::PostMessage(pasteWnd->m_hWnd, NM_SET_LIST_COUNT, rows.size(),
                            static_cast<LPARAM>(request.generation));
                    else
                        pasteWnd->m_countThread.FireSetListCount();
                }
            }
        }
    }
    catch (CppSQLite3Exception& e)
    {
        if (IsCurrentSearch(request.generation) && e.errorCode() != SQLITE_INTERRUPT)
            Log(StrF(_T("Search page failed, SQLite code: %d"), e.errorCode()));
    }
    FinishSearch(request.generation);
    Log(StrF(_T("Search pages completed in %d ms"), GetTickCount() - start));
}


namespace
{
    const SIZE_T ImageCacheBudget = 32 * 1024 * 1024;
    const SIZE_T RtfCacheBudget = 8 * 1024 * 1024;

    void TrimFormatCache(CF_DibTypeMap& cache, SIZE_T budget)
    {
        SIZE_T bytes = 0;
        for (const auto& item : cache)
            bytes += item.second.m_hgData ? GlobalSize(item.second.m_hgData) : 0;
        const size_t targetCount = cache.size() > 50 ? 30 : 50;
        while (!cache.empty() && (cache.size() > targetCount || bytes > budget))
        {
            auto oldest = cache.begin();
            for (auto item = cache.begin(); item != cache.end(); ++item)
                if (item->second.m_counter < oldest->second.m_counter)
                    oldest = item;
            bytes -= oldest->second.m_hgData ? GlobalSize(oldest->second.m_hgData) : 0;
            cache.erase(oldest);
        }
    }

    bool ReadExtraFormat(CppSQLite3DB& db, CClipFormatQListCtrl& format)
    {
        CppSQLite3Statement statement = db.compileStatement(
            _T("SELECT ooData FROM Data WHERE lParentID=? AND strClipBoardFormat=? LIMIT 1"));
        statement.bind(1, format.m_parentId);
        statement.bind(2, GetFormatName(format.m_cfType));
        CppSQLite3Query query = statement.execQuery();
        if (query.eof())
            return false;
        int length = 0;
        const unsigned char* data = query.getBlobField(0, length);
        if (data == nullptr || length <= 0)
            return false;
        format.m_hgData = NewGlobal(length);
        if (format.m_hgData == nullptr)
            return false;
        void* target = GlobalLock(format.m_hgData);
        if (target == nullptr)
        {
            format.Free();
            return false;
        }
        memcpy(target, data, static_cast<size_t>(length));
        GlobalUnlock(format.m_hgData);
        return true;
    }
}

void CQPasteWndThread::TrimExtraDataCache(CQPasteWnd* pasteWnd)
{
    // Called only on the UI thread: DrawItem can safely use the cached format
    // pointer until it returns, without a worker evicting it concurrently.
    ATL::CCritSecLock lock(pasteWnd->m_CritSection.m_sect);
    TrimFormatCache(pasteWnd->m_cf_dibCache, ImageCacheBudget);
    TrimFormatCache(pasteWnd->m_cf_rtfCache, RtfCacheBudget);
    while (pasteWnd->m_cf_NO_dibCache.size() > 2048)
        pasteWnd->m_cf_NO_dibCache.erase(pasteWnd->m_cf_NO_dibCache.begin());
    while (pasteWnd->m_cf_NO_rtfCache.size() > 2048)
        pasteWnd->m_cf_NO_rtfCache.erase(pasteWnd->m_cf_NO_rtfCache.begin());
}

void CQPasteWndThread::OnLoadExtraData(void* param)
{
    auto pasteWnd = static_cast<CQPasteWnd*>(param);
    const UINT_PTR generation = ExtraDataGeneration();
    CString databasePath;
    {
        ATL::CCritSecLock lock(pasteWnd->m_CritSection.m_sect);
        databasePath = pasteWnd->m_listDatabasePath;
    }
    if (databasePath.IsEmpty())
        return;
    struct ExtraContext
    {
        CQPasteWndThread* worker;
        UINT_PTR generation;
        DWORD busyTimeout;
    } context{ this, generation, CGetSetOptions::GetDbTimeout() };
    auto cancelled = [&]() { return IsCancelled() || generation != ExtraDataGeneration(); };
    auto currentRow = [&](const CClipFormatQListCtrl& format)
    {
        ATL::CCritSecLock lock(pasteWnd->m_CritSection.m_sect);
        return !cancelled() && format.m_generation == generation &&
            format.m_clipRow >= pasteWnd->m_visibleFirstRow &&
            format.m_clipRow <= pasteWnd->m_visibleLastRow &&
            pasteWnd->m_listItems.contains(format.m_clipRow) &&
            pasteWnd->m_listItems[format.m_clipRow].m_lID == format.m_parentId;
    };
    try
    {
        CppSQLite3DB db;
        db.open(databasePath, true);
        db.setProgressHandler(1000, [](void* value) -> int
        {
            const auto query = static_cast<ExtraContext*>(value);
            return query->worker->IsCancelled() ||
                query->generation != query->worker->ExtraDataGeneration();
        }, &context);
        db.setBusyHandler([](void* value, int attempts) -> int
        {
            const auto query = static_cast<ExtraContext*>(value);
            if (query->worker->IsCancelled() ||
                query->generation != query->worker->ExtraDataGeneration() ||
                static_cast<ULONGLONG>(attempts) * 10 >= query->busyTimeout)
                return 0;
            Sleep(10);
            return 1;
        }, &context);
        while (!cancelled())
        {
            // Limit decoded data queued for the UI even while a dialog is busy.
            while (m_pendingExtraResults.load() >= 2 && !cancelled())
                Sleep(10);
            if (cancelled())
                break;
            CClipFormatQListCtrl format;
            {
                ATL::CCritSecLock lock(pasteWnd->m_CritSection.m_sect);
                if (pasteWnd->m_ExtraDataLoadItems.empty())
                    break;
                format = pasteWnd->m_ExtraDataLoadItems.front();
                pasteWnd->m_ExtraDataLoadItems.pop_front();
                const bool image = format.m_cfType == CF_DIB;
                const auto& cache = image ? pasteWnd->m_cf_dibCache : pasteWnd->m_cf_rtfCache;
                const auto& absent = image ? pasteWnd->m_cf_NO_dibCache : pasteWnd->m_cf_NO_rtfCache;
                if (cache.find(format.m_parentId) != cache.end() || absent.find(format.m_parentId) != absent.end())
                    continue;
            }
            if (!currentRow(format))
                continue;
            const bool image = format.m_cfType == CF_DIB;
            bool found = ReadExtraFormat(db, format);
            if (!found && image && !cancelled())
            {
                format.m_cfType = theApp.m_PNG_Format;
                found = ReadExtraFormat(db, format);
            }
            if (!currentRow(format))
                continue;
            if (found && image)
            {
                HDC dc = GetDC(nullptr);
                if (dc != nullptr)
                {
                    format.GetDibFittingToHeight(CDC::FromHandle(dc), m_rowHeight.load(), m_maxImageWidth.load());
                    ReleaseDC(nullptr, dc);
                }
                else
                    format.Free();
                format.m_autoDeleteData = true;
            }
            // An unusually large rich-text preview falls back to the normal
            // description; its full saved data and full preview are untouched.
            const SIZE_T budget = image ? ImageCacheBudget : RtfCacheBudget;
            if (format.m_hgData != nullptr && GlobalSize(format.m_hgData) > budget)
                format.Free();
            if (!currentRow(format))
                continue;
            auto result = new CClipFormatQListCtrl(format);
            result->m_autoDeleteData = true;
            format.m_autoDeleteData = false;
            ++m_pendingExtraResults;
            if (!::PostMessage(pasteWnd->m_hWnd, NM_EXTRA_DATA_READY,
                reinterpret_cast<WPARAM>(result), 0))
            {
                --m_pendingExtraResults;
                delete result;
            }
        }
    }
    catch (CppSQLite3Exception& e)
    {
        if (!cancelled() && e.errorCode() != SQLITE_INTERRUPT)
            Log(StrF(_T("Loading thumbnail/RTF failed, SQLite code: %d"), e.errorCode()));
    }
}

void CQPasteWndThread::OnLoadAccelerators(void *param)
{
    CQPasteWnd *pasteWnd = (CQPasteWnd*)param;
    pasteWnd->m_lstHeader.DestroyAndCreateAccelerator(TRUE, theApp.m_db);
}

void CQPasteWndThread::OnUnloadAccelerators(void *param)
{
    CQPasteWnd *pasteWnd = (CQPasteWnd*)param;
    pasteWnd->m_lstHeader.DestroyAndCreateAccelerator(FALSE, theApp.m_db);
}

CString CQPasteWndThread::EnumName(eCQPasteWndThreadEvents e)
{
	switch(e)
	{
	case DO_SET_LIST_COUNT:
		return _T("Load List Count");
	case LOAD_ACCELERATORS:
		return _T("Load Accelerators");
	case UNLOAD_ACCELERATORS:
		return _T("Unload Accelerators");
	case LOAD_ITEMS:
		return _T("Load clips");
	case LOAD_EXTRA_DATA:
		return _T("Load Extra Data (rtf/bitmaps)");
	}

	return _T("");
}
