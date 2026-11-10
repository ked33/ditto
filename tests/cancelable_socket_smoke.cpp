#include "../src/CancelableSocket.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

static void Require(bool value, const char* message)
{
    if (!value)
    {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

int main()
{
    WSADATA winsock = {};
    Require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "WSAStartup");
    std::atomic<bool> cancelled{false};
    unsigned int address = INADDR_NONE;
    Require(CancelableSocket::ResolveIPv4(L"localhost", &cancelled, address), "Resolve localhost");
    Require(address != INADDR_NONE, "IPv4 resolver result");
    Require(CancelableSocket::ResolveIPv4(L"127.0.0.1", &cancelled, address) &&
        address == htonl(INADDR_LOOPBACK), "Resolve numeric IPv4");
    cancelled.store(true);
    Require(!CancelableSocket::ResolveIPv4(L"unused.invalid", &cancelled, address), "Cancel before DNS");
    sockaddr_in endpoint = {};
    Require(!CancelableSocket::Connect(INVALID_SOCKET, endpoint, &cancelled), "Cancel before connect");
    cancelled.store(false);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(listener != INVALID_SOCKET, "Listener socket");
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Legacy application headers import std names. Keep std::bind visible to
    // exercise WinSock name resolution in the same environment as Server.cpp.
    using namespace std;
    Require(::bind(listener, reinterpret_cast<const sockaddr*>(&endpoint), static_cast<int>(sizeof(endpoint))) == 0, "Bind loopback");
    Require(listen(listener, 1) == 0, "Listen loopback");
    int endpointLength = static_cast<int>(sizeof(endpoint));
    Require(getsockname(listener, reinterpret_cast<sockaddr*>(&endpoint), &endpointLength) == 0, "Listener address");
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(client != INVALID_SOCKET, "Client socket");
    Require(CancelableSocket::Connect(client, endpoint, &cancelled), "Nonblocking connect");
    SOCKET receiver = accept(listener, nullptr, nullptr);
    Require(receiver != INVALID_SOCKET, "Accept");

    const char expected[] = "preview";
    const int payloadLength = static_cast<int>(sizeof(expected));
    Require(CancelableSocket::Send(client, expected, payloadLength, &cancelled) == payloadLength, "Send payload");
    char actual[sizeof(expected)] = {};
    DWORD readTimeout = 1000;
    Require(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&readTimeout), static_cast<int>(sizeof(readTimeout))) == 0, "Receive timeout");
    Require(recv(receiver, actual, payloadLength, MSG_WAITALL) == payloadLength, "Receive payload");
    Require(std::memcmp(expected, actual, sizeof(expected)) == 0, "Payload unchanged");

    // The receiver stops consuming data. Cancelling must release a sender
    // blocked by TCP backpressure, without forcibly terminating its thread.
    int sendBuffer = 4096;
    Require(setsockopt(client, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer), static_cast<int>(sizeof(sendBuffer))) == 0, "Send buffer");
    HANDLE done = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    Require(done != nullptr, "Completion event");
    std::atomic<int> sendError{0};
    std::thread sender([&] {
        std::vector<char> body(65536, 'x');
        while (CancelableSocket::Send(client, body.data(), static_cast<int>(body.size()), &cancelled) > 0) {}
        sendError.store(WSAGetLastError());
        SetEvent(done);
    });
    Sleep(100);
    cancelled.store(true);
    Require(WaitForSingleObject(done, 3000) == WAIT_OBJECT_0, "Cancellation failed to release sender");
    sender.join();
    Require(sendError.load() == WSA_OPERATION_ABORTED, "Expected cancelled send");
    CloseHandle(done);
    closesocket(receiver);
    closesocket(client);
    closesocket(listener);
    WSACleanup();
    std::puts("Cancelable socket DNS, connect, payload and blocked-send cancellation passed");
    return 0;
}
