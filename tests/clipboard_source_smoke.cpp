#include <Windows.h>
#include <cstdio>

// Exercise the production context with deterministic Win32 metadata. No real
// clipboard contents, windows, or user processes are touched by these tests.
static DWORD testSequence = 10;
static HWND testOwner = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
static HWND testForeground = reinterpret_cast<HWND>(static_cast<UINT_PTR>(2));
static DWORD testOwnerProcess = 100;
static DWORD testForegroundProcess = 200;
static unsigned ownerQueries = 0;
static unsigned processQueries = 0;

static DWORD TestGetClipboardSequenceNumber() { return testSequence; }
static HWND TestGetClipboardOwner() { ++ownerQueries; return testOwner; }
static HWND TestGetForegroundWindow() { return testForeground; }
static DWORD TestGetCurrentProcessId() { return 999; }
static DWORD TestGetWindowThreadProcessId(HWND window, LPDWORD process)
{
	++processQueries;
	*process = window == testOwner ? testOwnerProcess : testForegroundProcess;
	return 1;
}

#define GetClipboardSequenceNumber TestGetClipboardSequenceNumber
#define GetClipboardOwner TestGetClipboardOwner
#define GetForegroundWindow TestGetForegroundWindow
#define GetCurrentProcessId TestGetCurrentProcessId
#define GetWindowThreadProcessId TestGetWindowThreadProcessId
#include "../src/ClipboardSource.h"
#undef GetClipboardSequenceNumber
#undef GetClipboardOwner
#undef GetForegroundWindow
#undef GetCurrentProcessId
#undef GetWindowThreadProcessId

static int failures = 0;
static int checks = 0;
static void Check(bool condition, const char* description)
{
	++checks;
	if (!condition)
	{
		std::printf("FAIL: %s\n", description);
		++failures;
	}
}

int main()
{
	auto source = ClipboardSourceContext::Capture();
	Check(source.processId == 100 && source.window == testOwner, "owner takes precedence over foreground");
	ownerQueries = processQueries = 0;
	Check(source.IsCurrent(), "unchanged owner copy remains valid");
	Check(ownerQueries == 0 && processQueries == 0, "unchanged sequence uses the fast path");

	++testSequence;
	Check(source.IsCurrent(), "delayed rendering with the same owner keeps the source");
	testOwnerProcess = 101;
	Check(!source.IsCurrent(), "reused owner window with a different process is rejected");
	testOwnerProcess = 100;
	testOwner = reinterpret_cast<HWND>(static_cast<UINT_PTR>(3));
	Check(!source.IsCurrent(), "replacement owner window is rejected");
	testOwner = nullptr;
	Check(!source.IsCurrent(), "changed clipboard with a missing owner is rejected");

	// Editors such as Sublime Text can leave the clipboard owner empty. Capture
	// the foreground identity at the event, never after returning to Ditto.
	source = ClipboardSourceContext::Capture();
	Check(source.owner == nullptr && source.processId == 200, "ownerless event captures foreground process");
	Check(source.IsCurrent(), "ownerless event with an unchanged sequence is valid");
	testForegroundProcess = 201;
	Check(source.IsCurrent() && source.processId == 200, "later focus changes do not replace the captured source");
	++testSequence;
	Check(!source.IsCurrent(), "foreground fallback cannot identify a later clipboard event");

	Check(!ClipboardSourceContext::Capture(false).IsCurrent(), "manual saves do not guess ownerless sources");
	testForegroundProcess = 999;
	Check(!ClipboardSourceContext::Capture().IsCurrent(), "Ditto itself is not an ownerless fallback");
	testForeground = nullptr;
	Check(!ClipboardSourceContext::Capture().IsCurrent(), "missing owner and foreground stay unknown");
	testForeground = reinterpret_cast<HWND>(static_cast<UINT_PTR>(2));
	testForegroundProcess = 200;
	testSequence = 0;
	Check(!ClipboardSourceContext::Capture().IsCurrent(), "unavailable sequence stays unknown");

	std::printf("Clipboard source context: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
