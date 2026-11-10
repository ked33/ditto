#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>

// Used only by background automatic sends. Manual transfers retain their
// existing socket mode and behavior when no cancellation flag is supplied.
namespace CancelableSocket
{
    inline bool IsCancelled(const std::atomic<bool>* cancelled)
    {
        return cancelled != nullptr && cancelled->load();
    }

    inline bool ResolveIPv4(const wchar_t* name, const std::atomic<bool>* cancelled, unsigned int& address)
    {
        if (IsCancelled(cancelled))
            return false;
        OVERLAPPED operation = {};
        operation.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr)
            return false;
        ADDRINFOEXW hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        PADDRINFOEXW addresses = nullptr;
        HANDLE query = nullptr;
        timeval timeout = {10, 0};
        int status = GetAddrInfoExW(name, nullptr, NS_ALL, nullptr, &hints,
            &addresses, &timeout, &operation, nullptr, &query);
        if (status == WSA_IO_PENDING)
        {
            while (WaitForSingleObject(operation.hEvent, 50) == WAIT_TIMEOUT)
            {
                if (IsCancelled(cancelled))
                {
                    GetAddrInfoExCancel(&query);
                    // The OVERLAPPED and result storage must survive until the
                    // resolver acknowledges cancellation (or its 10 s timeout).
                    WaitForSingleObject(operation.hEvent, INFINITE);
                    break;
                }
            }
            status = GetAddrInfoExOverlappedResult(&operation);
        }
        bool found = false;
        if (status == 0 && !IsCancelled(cancelled))
        {
            for (auto item = addresses; item != nullptr; item = item->ai_next)
            {
                if (item->ai_family == AF_INET && item->ai_addrlen >= sizeof(sockaddr_in))
                {
                    address = reinterpret_cast<sockaddr_in*>(item->ai_addr)->sin_addr.s_addr;
                    found = true;
                    break;
                }
            }
        }
        if (addresses != nullptr)
            FreeAddrInfoExW(addresses);
        CloseHandle(operation.hEvent);
        return found;
    }

    inline bool WaitWritable(SOCKET socket, const std::atomic<bool>* cancelled, DWORD timeoutMs)
    {
        const ULONGLONG start = GetTickCount64();
        while (!IsCancelled(cancelled) && GetTickCount64() - start < timeoutMs)
        {
            fd_set writable, errors;
            FD_ZERO(&writable);
            FD_ZERO(&errors);
            FD_SET(socket, &writable);
            FD_SET(socket, &errors);
            timeval timeout = {0, 50000};
            const int ready = select(0, nullptr, &writable, &errors, &timeout);
            if (ready == SOCKET_ERROR)
                return false;
            if (ready > 0 && FD_ISSET(socket, &errors))
            {
                int error = 0;
                int size = static_cast<int>(sizeof(error));
                if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0)
                    WSASetLastError(error != 0 ? error : WSAECONNABORTED);
                return false;
            }
            if (ready > 0 && FD_ISSET(socket, &writable))
            {
                if (!IsCancelled(cancelled))
                    return true;
                break;
            }
        }
        WSASetLastError(IsCancelled(cancelled) ? WSA_OPERATION_ABORTED : WSAETIMEDOUT);
        return false;
    }

    inline bool Connect(SOCKET socket, const sockaddr_in& address, const std::atomic<bool>* cancelled)
    {
        if (IsCancelled(cancelled))
            return false;
        u_long nonblocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0)
            return false;
        if (connect(socket, reinterpret_cast<const sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0)
        {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
                return false;
            if (!WaitWritable(socket, cancelled, 10000))
                return false;
            int connectionError = 0;
            int size = static_cast<int>(sizeof(connectionError));
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connectionError), &size) != 0 || connectionError != 0)
                return false;
        }
        return !IsCancelled(cancelled);
    }

    inline int Send(SOCKET socket, const char* data, int length, const std::atomic<bool>* cancelled)
    {
        const ULONGLONG start = GetTickCount64();
        while (!IsCancelled(cancelled))
        {
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= 30000)
                break;
            if (!WaitWritable(socket, cancelled, 30000 - static_cast<DWORD>(elapsed)))
                return SOCKET_ERROR;
            const int sent = send(socket, data, length, 0);
            if (sent != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK)
                return sent;
        }
        WSASetLastError(IsCancelled(cancelled) ? WSA_OPERATION_ABORTED : WSAETIMEDOUT);
        return SOCKET_ERROR;
    }
}
