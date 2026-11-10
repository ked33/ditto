#include "stdafx.h"
#include "AsyncClipPreview.h"
#include "ImageHelper.h"
#include "..\Shared\TextConvert.h"
#include <limits>
#include <set>
#include <vector>

namespace
{
    // Keep metadata separate from BLOB reads: discovering alternate views must
    // not load large formats that the user has not asked to display.
    const TCHAR* PreviewMetadataSql = _T("SELECT mText, lDate, lastPasteDate, lDontAutoDelete, QuickPasteText, lShortCut, globalShortCut, stickyClipOrder, stickyClipGroupOrder, lParentID FROM Main WHERE lID = %d");
    const TCHAR* PreviewFormatsSql = _T("SELECT strClipBoardFormat FROM Data WHERE lParentID = %d");
    const TCHAR* PreviewDataSql = _T("SELECT ooData FROM Data WHERE lParentID = %d AND strClipBoardFormat = '%s' ORDER BY lID LIMIT 1");
    const TCHAR* PreviewFolderSql = _T("SELECT mText, lParentID FROM Main WHERE lID = %d");

    std::vector<unsigned char> ReadFormat(CppSQLite3DB& db, int clipId, const TCHAR* format)
    {
        auto query = db.execQueryEx(PreviewDataSql, clipId, format);
        if (query.eof())
            return {};
        int length = 0;
        const auto data = query.getBlobField(0, length);
        return data != nullptr && length > 0 ?
            std::vector<unsigned char>(data, data + length) : std::vector<unsigned char>();
    }

    CString DecodeUnicode(const std::vector<unsigned char>& data)
    {
        const int length = static_cast<int>(data.size() / sizeof(wchar_t));
        CString text;
        if (length > 0)
        {
            auto buffer = text.GetBuffer(length);
            memcpy(buffer, data.data(), length * sizeof(wchar_t));
            text.ReleaseBufferSetLength(length);
            if (text[length - 1] == L'\0')
                text.Truncate(length - 1);
        }
        return text;
    }

    CStringA DecodeBytes(const std::vector<unsigned char>& data)
    {
        if (data.empty())
            return CStringA();
        int length = static_cast<int>(data.size());
        if (data.back() == 0)
            --length;
        return CStringA(reinterpret_cast<const char*>(data.data()), length);
    }

    std::unique_ptr<Gdiplus::Bitmap> DecodeImage(const std::vector<unsigned char>& data, bool dib)
    {
        if (data.empty() || (dib && data.size() < sizeof(BITMAPINFOHEADER)))
            return nullptr;
        CComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
            return nullptr;
        if (dib)
            DIBImageHelper::prependStream(stream, const_cast<unsigned char*>(data.data()), static_cast<ULONG>(data.size()));
        ULONG written = 0;
        if (FAILED(stream->Write(data.data(), static_cast<ULONG>(data.size()), &written)) || written != data.size())
            return nullptr;
        LARGE_INTEGER start = {};
        if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)))
            return nullptr;
        Gdiplus::Bitmap source(stream);
        if (source.GetLastStatus() != Gdiplus::Ok || source.GetWidth() == 0 || source.GetHeight() == 0 ||
            source.GetWidth() > static_cast<UINT>((std::numeric_limits<int>::max)()) ||
            source.GetHeight() > static_cast<UINT>((std::numeric_limits<int>::max)()))
            return nullptr;

        // GDI+ requires the source stream to outlive its bitmap. Materialize all
        // pixels here, while the stream is alive, before handing the image to UI.
        const int width = static_cast<int>(source.GetWidth());
        const int height = static_cast<int>(source.GetHeight());
        auto image = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppARGB);
        if (image->GetLastStatus() != Gdiplus::Ok)
            return nullptr;
        Gdiplus::Graphics graphics(image.get());
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        if (graphics.DrawImage(&source, 0, 0, width, height) != Gdiplus::Ok)
            return nullptr;
        return image;
    }
}

CAsyncClipPreview::~CAsyncClipPreview()
{
    Stop();
}

UINT_PTR CAsyncClipPreview::Request(HWND notifyWindow, const CString& databasePath, int clipId,
    ClipPreviewFormat format, bool inGroup, int busyTimeout)
{
    Cancel();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping.load())
        return 0;
    m_request.notifyWindow = notifyWindow;
    m_request.databasePath = databasePath;
    m_request.clipId = clipId;
    m_request.format = format;
    m_request.inGroup = inGroup;
    m_request.busyTimeout = busyTimeout;
    m_request.generation = m_generation.load();
    m_pending = true;
    if (!m_thread.joinable())
    {
        try
        {
            m_thread = std::thread(&CAsyncClipPreview::Run, this);
        }
        catch (...)
        {
            m_pending = false;
            Log(_T("Could not start preview reader"));
            return 0;
        }
    }
    m_wake.notify_one();
    return m_request.generation;
}

void CAsyncClipPreview::Cancel()
{
    ++m_generation;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending = false;
        m_result.reset();
    }
    std::lock_guard<std::mutex> connectionLock(m_connectionMutex);
    if (m_connection != nullptr)
        m_connection->interrupt();
}

void CAsyncClipPreview::Stop()
{
    m_stopping.store(true);
    Cancel();
    m_wake.notify_one();
    if (m_thread.joinable())
        m_thread.join();
}

std::unique_ptr<ClipPreviewResult> CAsyncClipPreview::TakeResult()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::move(m_result);
}

bool CAsyncClipPreview::IsCurrent(UINT_PTR generation) const
{
    return !m_stopping.load() && m_generation.load() == generation;
}

int CAsyncClipPreview::Progress(void* context)
{
    auto owner = static_cast<CAsyncClipPreview*>(context);
    return !owner->IsCurrent(owner->m_runningGeneration);
}

int CAsyncClipPreview::Busy(void* context, int attempts)
{
    auto owner = static_cast<CAsyncClipPreview*>(context);
    if (Progress(context) || static_cast<ULONGLONG>(attempts) * 10 >= owner->m_busyTimeout)
        return 0;
    Sleep(10);
    return !Progress(context);
}

void CAsyncClipPreview::Run()
{
    const HRESULT comStatus = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!m_stopping.load())
    {
        RequestData request;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping.load() || m_pending; });
            if (m_stopping.load())
                break;
            request = m_request;
            m_pending = false;
        }
        try
        {
            auto result = Load(request);
            std::lock_guard<std::mutex> lock(m_mutex);
            if (result && IsCurrent(request.generation))
            {
                m_result = std::move(result);
                if (!::PostMessage(request.notifyWindow, WM_CLIP_PREVIEW_READY, 0, 0))
                    m_result.reset();
            }
        }
        catch (CppSQLite3Exception& error)
        {
            if (IsCurrent(request.generation))
            {
                Log(StrF(_T("Preview read failed: %s"), error.errorMessage()));
                // Failure notification owns no allocation, including on OOM.
                ::PostMessage(request.notifyWindow, WM_CLIP_PREVIEW_READY, request.generation, 1);
            }
        }
        catch (...)
        {
            if (IsCurrent(request.generation))
            {
                Log(_T("Preview loading failed"));
                ::PostMessage(request.notifyWindow, WM_CLIP_PREVIEW_READY, request.generation, 1);
            }
        }
    }
    if (SUCCEEDED(comStatus))
        CoUninitialize();
}

std::unique_ptr<ClipPreviewResult> CAsyncClipPreview::Load(const RequestData& request)
{
    if (!IsCurrent(request.generation))
        return nullptr;
    auto result = std::make_unique<ClipPreviewResult>();
    result->generation = request.generation;
    result->clipId = request.clipId;
    result->databasePath = request.databasePath;
    std::vector<unsigned char> data;
    bool dib = false;
    bool unicodeText = false;
    bool ansiText = false;
    CppSQLite3DB db;
    {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        if (!IsCurrent(request.generation))
            return nullptr;
        db.open(request.databasePath, true);
        m_runningGeneration = request.generation;
        m_busyTimeout = request.busyTimeout;
        db.setProgressHandler(1000, Progress, this);
        db.setBusyHandler(Busy, this);
        m_connection = &db;
    }
    // Always detach before the local connection is destroyed, including on
    // SQLite errors, allocation failures, and cancellation.
    struct ConnectionGuard
    {
        CAsyncClipPreview& owner;
        CppSQLite3DB& db;
        ~ConnectionGuard()
        {
            std::lock_guard<std::mutex> lock(owner.m_connectionMutex);
            owner.m_connection = nullptr;
            db.setProgressHandler(0, nullptr, nullptr);
            db.setBusyHandler(nullptr, nullptr);
            db.close();
        }
    } connectionGuard{*this, db};

    db.execDML(_T("BEGIN"));
    CString description;
    int parentId = -1;
    {
        auto metadata = db.execQueryEx(PreviewMetadataSql, request.clipId);
        if (metadata.eof())
            return result;
        result->found = true;
        description = metadata.getStringField(_T("mText"));
        result->added = metadata.getInt64Field(_T("lDate"));
        result->lastUsed = metadata.getInt64Field(_T("lastPasteDate"));
        result->dontAutoDelete = metadata.getIntField(_T("lDontAutoDelete")) > 0;
        result->quickPaste = metadata.getStringField(_T("QuickPasteText"));
        result->shortcut = metadata.getIntField(_T("lShortCut"));
        result->globalShortcut = metadata.getIntField(_T("globalShortCut")) != 0;
        result->sticky = metadata.getFloatField(request.inGroup ? _T("stickyClipGroupOrder") : _T("stickyClipOrder")) != -2147483647;
        parentId = metadata.getIntField(_T("lParentID"));
    }
    std::set<CString> formats;
    {
        auto query = db.execQueryEx(PreviewFormatsSql, request.clipId);
        while (!query.eof() && IsCurrent(request.generation))
        {
            formats.insert(query.getStringField(0));
            query.nextRow();
        }
    }
    result->hasText = formats.count(_T("CF_UNICODETEXT")) || formats.count(_T("CF_TEXT")) || !description.IsEmpty();
    result->hasRichText = formats.count(_T("Rich Text Format")) != 0;
    result->hasHtml = formats.count(_T("HTML Format")) != 0;
    result->hasImage = formats.count(_T("CF_DIB")) || formats.count(_T("PNG"));
    result->format = request.format;
    if (result->format == ClipPreviewFormat::Automatic)
        result->format = result->hasImage ? ClipPreviewFormat::Image :
            result->hasRichText ? ClipPreviewFormat::RichText :
            result->hasHtml ? ClipPreviewFormat::Html : ClipPreviewFormat::Text;

    std::set<int> visited;
    while (parentId > 0 && visited.size() < 100 && visited.insert(parentId).second && IsCurrent(request.generation))
    {
        auto folder = db.execQueryEx(PreviewFolderSql, parentId);
        if (folder.eof())
            break;
        result->folderPath = _T("\\") + CString(folder.getStringField(0)) + result->folderPath;
        parentId = folder.getIntField(1);
    }

    if (!IsCurrent(request.generation))
        return nullptr;
    switch (result->format)
    {
    case ClipPreviewFormat::Text:
        if (formats.count(_T("CF_UNICODETEXT")))
        {
            unicodeText = true;
            data = ReadFormat(db, request.clipId, _T("CF_UNICODETEXT"));
        }
        else if (formats.count(_T("CF_TEXT")))
        {
            ansiText = true;
            data = ReadFormat(db, request.clipId, _T("CF_TEXT"));
        }
        else
            result->text = description;
        break;
    case ClipPreviewFormat::RichText:
        data = ReadFormat(db, request.clipId, _T("Rich Text Format"));
        break;
    case ClipPreviewFormat::Html:
        data = ReadFormat(db, request.clipId, _T("HTML Format"));
        break;
    case ClipPreviewFormat::Image:
        dib = formats.count(_T("CF_DIB")) != 0;
        data = ReadFormat(db, request.clipId, dib ? _T("CF_DIB") : _T("PNG"));
        break;
    default:
        break;
    }
    db.execDML(_T("COMMIT"));
    // Release the snapshot before expensive decoding so writers remain free.
    if (!IsCurrent(request.generation))
        return nullptr;
    switch (result->format)
    {
    case ClipPreviewFormat::Text:
        if (unicodeText)
            result->text = DecodeUnicode(data);
        else if (ansiText)
            result->text = CString(DecodeBytes(data));
        result->textWordCount = WordCount(result->text);
        break;
    case ClipPreviewFormat::RichText:
        result->richText = DecodeBytes(data);
        break;
    case ClipPreviewFormat::Html:
        result->html = CTextConvert::Utf8ToUnicode(DecodeBytes(data));
        break;
    case ClipPreviewFormat::Image:
    {
        result->image = DecodeImage(data, dib);
        if (!result->image)
        {
            result->format = ClipPreviewFormat::Text;
            result->text = description;
            result->textWordCount = WordCount(result->text);
        }
        break;
    }
    default:
        break;
    }
    return IsCurrent(request.generation) ? std::move(result) : nullptr;
}
