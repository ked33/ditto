#pragma once

#include "sqlite/CppSQLite3.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

constexpr UINT WM_CLIP_PREVIEW_READY = WM_APP + 0x320;
constexpr UINT WM_CLIP_PREVIEW_FORMAT = WM_APP + 0x321;
constexpr UINT WM_CLIP_PREVIEW_CANCEL = WM_APP + 0x322;

enum class ClipPreviewFormat { Automatic, Text, RichText, Html, Image };

struct ClipPreviewResult
{
    UINT_PTR generation = 0;
    int clipId = 0;
    CString databasePath;
    bool found = false;
    bool hasText = false;
    bool hasRichText = false;
    bool hasHtml = false;
    bool hasImage = false;
    ClipPreviewFormat format = ClipPreviewFormat::Text;
    CString text;
    int textWordCount = 0;
    CStringA richText;
    CString html;
    std::unique_ptr<Gdiplus::Bitmap> image;
    __int64 added = 0;
    __int64 lastUsed = 0;
    bool dontAutoDelete = false;
    CString quickPaste;
    int shortcut = 0;
    bool globalShortcut = false;
    bool sticky = false;
    CString folderPath;
};

// Owns both queued requests and results. Window messages never carry owning
// pointers, so cancelled requests and destroyed HWNDs cannot leak a result.
class CAsyncClipPreview
{
public:
    ~CAsyncClipPreview();
    UINT_PTR Request(HWND notifyWindow, const CString& databasePath, int clipId,
        ClipPreviewFormat format, bool inGroup, int busyTimeout);
    void Cancel();
    void Stop();
    std::unique_ptr<ClipPreviewResult> TakeResult();

private:
    struct RequestData
    {
        HWND notifyWindow = nullptr;
        CString databasePath;
        int clipId = 0;
        ClipPreviewFormat format = ClipPreviewFormat::Automatic;
        bool inGroup = false;
        int busyTimeout = 0;
        UINT_PTR generation = 0;
    };

    void Run();
    std::unique_ptr<ClipPreviewResult> Load(const RequestData& request);
    bool IsCurrent(UINT_PTR generation) const;
    static int Progress(void* context);
    static int Busy(void* context, int attempts);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    RequestData m_request;
    bool m_pending = false;
    std::unique_ptr<ClipPreviewResult> m_result;
    std::atomic<UINT_PTR> m_generation{0};
    std::atomic<bool> m_stopping{false};
    std::mutex m_connectionMutex;
    CppSQLite3DB* m_connection = nullptr;
    UINT_PTR m_runningGeneration = 0;
    int m_busyTimeout = 0;
};
