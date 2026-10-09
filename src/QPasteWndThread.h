#pragma once
#include "EventThread.h"
#include "sqlite/CppSQLite3.h"
#include <atomic>

class CQPasteWndThread: public CEventThread
{
public:
    CQPasteWndThread(void);
    ~CQPasteWndThread(void);

    enum eCQPasteWndThreadEvents
    {
		DO_SET_LIST_COUNT, 
		LOAD_ACCELERATORS, 
		UNLOAD_ACCELERATORS, 
		LOAD_ITEMS, 
		LOAD_EXTRA_DATA, 

        ECQPASTEWNDTHREADEVENTS_COUNT  //must be last

    };

    void FireSetListCount()
    {
        FireEvent(DO_SET_LIST_COUNT);
    }
    void FireLoadItems(bool firstLoad)
    {
        FireLoadItemsRequest();
    }
    void FireLoadExtraData(int rowHeight)
    {
		m_rowHeight = rowHeight;
        FireEvent(LOAD_EXTRA_DATA);
    }
    void FireLoadAccelerators()
    {
        FireEvent(LOAD_ACCELERATORS);
    }
    void FireUnloadAccelerators()
    {
        FireEvent(UNLOAD_ACCELERATORS);
    }

    HANDLE m_SearchingEvent;

	void SetRowHeight(int height) { m_rowHeight = height; }
    void SetSearchSql(const CString& sql, const CString& countSql, const CString& databasePath);
    void CancelSearch();
    void ResumeSearch();
    void AcknowledgeListCount(UINT_PTR generation);
    bool IsCurrentSearch(UINT_PTR generation) const { return generation == m_generation.load(); }

protected:
    virtual void OnEvent(int eventId, void *param);
    virtual void OnTimeOut(void *param);

    void OnSetListCount(void *param);
    void OnLoadItems(void *param);
    void OnLoadExtraData(void *param);
	//void ReduceMapItems(CF_DibTypeMap mapItem, CCriticalSection &critSection, CString mapName);
    void OnLoadAccelerators(void *param);
    void OnUnloadAccelerators(void *param);

	CString EnumName(eCQPasteWndThreadEvents e);

	int m_rowHeight;

private:
    struct SearchRequest
    {
        CString sql;
        CString countSql;
        CString databasePath;
        UINT_PTR generation = 0;
        DWORD busyTimeout = 0;
        bool countNeeded = false;
    };

    class SearchConnection
    {
    public:
        SearchConnection(CQPasteWndThread& owner, const SearchRequest& request);
        ~SearchConnection();
        explicit operator bool() const { return m_open; }
    private:
        CQPasteWndThread& m_owner;
        bool m_open;
    };

    SearchRequest GetSearchRequest();
    void FireLoadItemsRequest();
    bool BeginSearchConnection(const SearchRequest& request);
    void EndSearchConnection();
    void FinishSearch(UINT_PTR generation);
    static int SearchProgress(void* context);
    static int SearchBusy(void* context, int attempts);

    CCriticalSection m_requestLock;
    CCriticalSection m_connectionLock;
    std::atomic<UINT_PTR> m_generation{1};
    SearchRequest m_request;
    UINT_PTR m_countGeneration = 0;
    CppSQLite3DB m_searchDb;
    UINT_PTR m_runningGeneration = 0;
    DWORD m_busyTimeout = 0;
};
