#pragma once

#include <Windows.h>

// Clipboard sequence numbers also change when an owner renders delayed data.
// Retain its identity so that rendering does not erase a known source process.
struct ClipboardSourceContext
{
	DWORD sequence = 0;
	HWND owner = nullptr;
	HWND window = nullptr;
	DWORD processId = 0;

	static ClipboardSourceContext Capture(bool allowForegroundFallback = true)
	{
		ClipboardSourceContext source;
		source.sequence = ::GetClipboardSequenceNumber();
		source.owner = ::GetClipboardOwner();
		source.window = source.owner;
		if (source.window == nullptr && allowForegroundFallback)
		{
			source.window = ::GetForegroundWindow();
		}
		if (source.window != nullptr)
		{
			::GetWindowThreadProcessId(source.window, &source.processId);
		}
		if (source.owner == nullptr && source.processId == ::GetCurrentProcessId())
		{
			// A Ditto menu/window cannot recover an ownerless clip's original app.
			source.processId = 0;
		}
		return source;
	}

	bool Matches(DWORD currentSequence, HWND currentOwner, DWORD currentOwnerProcessId) const
	{
		if (sequence == 0 || currentSequence == 0 || processId == 0)
		{
			return false;
		}
		if (sequence == currentSequence)
		{
			return true;
		}
		// A foreground fallback is valid only for its original clipboard event.
		return owner != nullptr && owner == currentOwner && processId == currentOwnerProcessId;
	}

	bool IsCurrent() const
	{
		const DWORD currentSequence = ::GetClipboardSequenceNumber();
		if (sequence == currentSequence || owner == nullptr || processId == 0 || currentSequence == 0)
		{
			return Matches(currentSequence, nullptr, 0);
		}
		HWND currentOwner = ::GetClipboardOwner();
		DWORD currentOwnerProcessId = 0;
		if (currentOwner != nullptr)
		{
			::GetWindowThreadProcessId(currentOwner, &currentOwnerProcessId);
		}
		return Matches(currentSequence, currentOwner, currentOwnerProcessId);
	}
};
