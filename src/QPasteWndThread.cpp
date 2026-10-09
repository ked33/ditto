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
    m_request.countNeeded = true;
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
        m_request.countNeeded = false;
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
        FireSetListCount();
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
    return !thread->IsCurrentSearch(thread->m_runningGeneration);
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

    // Visible pages take precedence over an exact total.
    OnLoadItems(param);
    if (!IsCurrentSearch(request.generation))
        return;

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
            const int rowCount = max(range.y - range.x, 0);
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
                if (!rows.empty())
                {
                    const size_t required = static_cast<size_t>(firstRow) + rows.size();
                    if (pasteWnd->m_listItems.size() < required)
                        pasteWnd->m_listItems.resize(required);
                    std::copy(rows.begin(), rows.end(), pasteWnd->m_listItems.begin() + firstRow);
                }
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
                    FireSetListCount();
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


void ReduceMapItems(CF_DibTypeMap &mapItem, CCriticalSection &critSection, CString mapName)
{
	ATL::CCritSecLock csLock(critSection.m_sect);

	int maxSize = 50;
	int reduceToSize = 30;

	if (mapItem.size() > maxSize)
	{
		//create a vector so we can sort and keep the last x number of events
		vector<INT64> counterArray;
		for (CF_DibTypeMap::iterator iterDib = mapItem.begin(); iterDib != mapItem.end(); iterDib++)
		{
			counterArray.push_back(iterDib->second.m_counter);
		}
		std::sort(counterArray.begin(), counterArray.end());
		counterArray.erase(counterArray.begin(), counterArray.end() - reduceToSize);

		//remove the oldest x number if bitmaps
		for (CF_DibTypeMap::iterator iterDib = mapItem.begin(); iterDib != mapItem.end();)
		{
			if (std::binary_search(counterArray.begin(), counterArray.end(), iterDib->second.m_counter) == false)
			{
				Log(StrF(_T("reduced size of %s cache, Id: %d, Row: %d"), mapName, iterDib->second.m_parentId, iterDib->second.m_clipRow));

				mapItem.erase(iterDib++);
			}
			else
			{
				++iterDib;
			}
		}

		Log(StrF(_T("reduced size of %s cache, count: %d"), mapName, mapItem.size()));
	}
}

void CQPasteWndThread::OnLoadExtraData(void *param)
{
    ResetEvent(m_SearchingEvent);

    CQPasteWnd *pasteWnd = (CQPasteWnd*)param;

    Log(_T("Start of load extra data, Bitmaps/rtf"));

    std::list<CClipFormatQListCtrl> localFormats;
	{
		ATL::CCritSecLock csLock(pasteWnd->m_CritSection.m_sect);

		for (std::list<CClipFormatQListCtrl>::iterator it = pasteWnd->m_ExtraDataLoadItems.begin(); it != pasteWnd->m_ExtraDataLoadItems.end(); it++)
		{
			localFormats.push_back(*it);
		}
	    pasteWnd->m_ExtraDataLoadItems.clear();
	}
	
	for (std::list<CClipFormatQListCtrl>::iterator it = localFormats.begin(); it != localFormats.end(); it++)
    {
		bool loadClip = true;

		if (it->m_cfType == CF_DIB)
		{
			ATL::CCritSecLock csLock(pasteWnd->m_CritSection.m_sect);

			CF_DibTypeMap::iterator iterDib = pasteWnd->m_cf_dibCache.find(it->m_parentId);
			if (iterDib != pasteWnd->m_cf_dibCache.end())
			{
				loadClip = false;
			}
			else
			{
				CF_NoDibTypeMap::iterator iterNoDib = pasteWnd->m_cf_NO_dibCache.find(it->m_parentId);
				if (iterNoDib != pasteWnd->m_cf_NO_dibCache.end())
				{
					loadClip = false;
				}
			}
		}
		else if (it->m_cfType == theApp.m_RTFFormat)
		{
			ATL::CCritSecLock csLock(pasteWnd->m_CritSection.m_sect);

			CF_DibTypeMap::iterator iterDib = pasteWnd->m_cf_rtfCache.find(it->m_parentId);
			if (iterDib != pasteWnd->m_cf_rtfCache.end())
			{
				loadClip = false;
			}
			else
			{
				CF_NoDibTypeMap::iterator iterNoRtf = pasteWnd->m_cf_NO_rtfCache.find(it->m_parentId);
				if (iterNoRtf != pasteWnd->m_cf_NO_rtfCache.end())
				{
					loadClip = false;
				}
			}
		}

		if (loadClip)
		{
			DWORD startLoadClipData = GetTickCount();

			BOOL foundClipData = theApp.GetClipData(it->m_parentId, *it);
			if (foundClipData == false &&
				it->m_cfType == CF_DIB)
			{
				it->Free();
				it->m_cfType = theApp.m_PNG_Format;

				foundClipData = theApp.GetClipData(it->m_parentId, *it);
			}

			if (foundClipData)
			{
				DWORD timeTook = GetTickCount() - startLoadClipData;
				if (timeTook > 20)
				{
					Log(StrF(_T("GetClipData for clip %d, took: %d"), it->m_parentId, timeTook));
				}

				if (it->m_cfType == CF_DIB ||
					it->m_cfType == theApp.m_PNG_Format)
				{
					DWORD startConvertImage = GetTickCount();

					HDC dc = GetDC(NULL);

					it->GetDibFittingToHeight(CDC::FromHandle(dc), m_rowHeight);

					ReleaseDC(NULL, dc);

					DWORD timeTook = GetTickCount() - startConvertImage;
					if (timeTook > 20)
					{
						Log(StrF(_T("GetDibFittingToHeight for clip %d, took: %d"), it->m_parentId, GetTickCount() - startConvertImage));
					}

					{
						ATL::CCritSecLock csLock(pasteWnd->m_CritSection.m_sect);

						pasteWnd->m_cf_dibCache[it->m_parentId] = *it;
						//the cache now owns the format data, set it to delete the data in the destructor
						pasteWnd->m_cf_dibCache[it->m_parentId].m_autoDeleteData = true;

						Log(StrF(_T("Loaded, extra data for clipId: %d, Row: %d image cache count: %d"), it->m_parentId, it->m_clipRow, pasteWnd->m_cf_dibCache.size()));
					}
				}
				else if (it->m_cfType == theApp.m_RTFFormat)
				{
					ATL::CCritSecLock csLock(pasteWnd->m_CritSection.m_sect);

					pasteWnd->m_cf_rtfCache[it->m_parentId] = *it;
					it->m_autoDeleteData = false;
					//the cache now owns the format data, set it to delete the data in the destructor
					pasteWnd->m_cf_rtfCache[it->m_parentId].m_autoDeleteData = true;

					Log(StrF(_T("Loaded, extra data for clip %d, rtf cache count: %d"), it->m_parentId, pasteWnd->m_cf_rtfCache.size()));
				}

				::PostMessage(pasteWnd->m_hWnd, NM_REFRESH_ROW, it->m_parentId, it->m_clipRow);
			}
			else
			{
				ATL::CCritSecLock csLock(pasteWnd->m_CritSection.m_sect);

				if (it->m_cfType == CF_DIB ||
					it->m_cfType == theApp.m_PNG_Format)
				{
					pasteWnd->m_cf_NO_dibCache[it->m_parentId] = true;
				}
				else if (it->m_cfType == theApp.m_RTFFormat)
				{
					pasteWnd->m_cf_NO_rtfCache[it->m_parentId] = true;
				}
			}
		}

		if (it->m_cfType == CF_DIB)
		{
			ReduceMapItems(pasteWnd->m_cf_dibCache, pasteWnd->m_CritSection, _T("image"));
		}
		else if (it->m_cfType == theApp.m_RTFFormat)
		{
			ReduceMapItems(pasteWnd->m_cf_rtfCache, pasteWnd->m_CritSection, _T("rtf"));
		}
    }

    SetEvent(m_SearchingEvent);
    Log(_T("End of load extra data, Bitmaps/rtf"));
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
