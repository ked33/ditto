#include "stdafx.h"
#include "LowLevelHotKeys.h"
#include "Misc.h"

CLowLevelHotKeys g_LowLevelHotKeys;

CLowLevelHotKeys::CLowLevelHotKeys()
	: m_hWnd(NULL)
	, m_hHook(NULL)
	, m_count(0)
	, m_lwinDown(false)
	, m_rwinDown(false)
	, m_lctrlDown(false)
	, m_rctrlDown(false)
	, m_lshiftDown(false)
	, m_rshiftDown(false)
	, m_laltDown(false)
	, m_raltDown(false)
	, m_armed(false)
	, m_armedVk(0)
	, m_armedAtom(0)
{
	ZeroMemory(m_bindings, sizeof(m_bindings));
}

CLowLevelHotKeys::~CLowLevelHotKeys()
{
	Stop();
}

void CLowLevelHotKeys::ResetModifierState()
{
	m_lwinDown = false;
	m_rwinDown = false;
	m_lctrlDown = false;
	m_rctrlDown = false;
	m_lshiftDown = false;
	m_rshiftDown = false;
	m_laltDown = false;
	m_raltDown = false;
	m_armed = false;
	m_armedVk = 0;
	m_armedAtom = 0;
}

void CLowLevelHotKeys::Clear()
{
	m_count = 0;
	ZeroMemory(m_bindings, sizeof(m_bindings));
	m_armed = false;
	m_armedVk = 0;
	m_armedAtom = 0;
}

bool CLowLevelHotKeys::Add(UINT modifiers, UINT vk, ATOM atom)
{
	if(vk == 0 || atom == 0)
	{
		return false;
	}

	if(m_count >= MAX_BINDINGS)
	{
		Log(_T("CLowLevelHotKeys::Add - binding table full"));
		return false;
	}

	// Replace existing entry for the same atom (re-register after options apply).
	for(int i = 0; i < m_count; i++)
	{
		if(m_bindings[i].atom == atom)
		{
			m_bindings[i].modifiers = modifiers;
			m_bindings[i].vk = vk;
			return true;
		}
	}

	m_bindings[m_count].modifiers = modifiers;
	m_bindings[m_count].vk = vk;
	m_bindings[m_count].atom = atom;
	m_count++;
	return true;
}

bool CLowLevelHotKeys::Start()
{
	if(m_hHook != NULL)
	{
		return true;
	}

	if(m_count <= 0 || m_hWnd == NULL || !::IsWindow(m_hWnd))
	{
		return false;
	}

	m_hHook = ::SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelProc, ::GetModuleHandle(NULL), 0);
	if(m_hHook == NULL)
	{
		Log(StrF(_T("CLowLevelHotKeys::Start - SetWindowsHookEx failed, error %d"), ::GetLastError()));
		return false;
	}

	// Seed from async state once at install time (not used per-event in the hook).
	m_lwinDown = (::GetAsyncKeyState(VK_LWIN) & 0x8000) != 0;
	m_rwinDown = (::GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
	m_lctrlDown = (::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
	m_rctrlDown = (::GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
	m_lshiftDown = (::GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
	m_rshiftDown = (::GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
	m_laltDown = (::GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
	m_raltDown = (::GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
	m_armed = false;
	m_armedVk = 0;
	m_armedAtom = 0;

	Log(StrF(_T("CLowLevelHotKeys::Start - WH_KEYBOARD_LL active, %d binding(s)"), m_count));
	return true;
}

void CLowLevelHotKeys::Stop()
{
	if(m_hHook != NULL)
	{
		::UnhookWindowsHookEx(m_hHook);
		m_hHook = NULL;
		Log(_T("CLowLevelHotKeys::Stop - WH_KEYBOARD_LL removed"));
	}

	ResetModifierState();
}

LRESULT CALLBACK CLowLevelHotKeys::LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	return g_LowLevelHotKeys.ProcessHook(nCode, wParam, lParam);
}

LRESULT CLowLevelHotKeys::ProcessHook(int nCode, WPARAM wParam, LPARAM lParam)
{
	if(nCode == HC_ACTION && lParam != 0)
	{
		const KBDLLHOOKSTRUCT *pKbd = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);

		UpdateModifierState(wParam, pKbd);

		// Ignore our own synthetic keys (Start-menu cancel) for matching.
		const bool injected = (pKbd->flags & LLKHF_INJECTED) != 0;
		if(!injected && TryFire(wParam, pKbd))
		{
			return 1; // eat target key so system/other apps do not also handle it
		}
	}

	return ::CallNextHookEx(m_hHook, nCode, wParam, lParam);
}

void CLowLevelHotKeys::UpdateModifierState(WPARAM wParam, const KBDLLHOOKSTRUCT *pKbd)
{
	const bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
	const bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
	if(!keyUp && !keyDown)
	{
		return;
	}

	const bool down = keyDown;
	switch(pKbd->vkCode)
	{
	case VK_LWIN:
		m_lwinDown = down;
		if(!WinDown())
		{
			m_armed = false;
			m_armedVk = 0;
			m_armedAtom = 0;
		}
		break;
	case VK_RWIN:
		m_rwinDown = down;
		if(!WinDown())
		{
			m_armed = false;
			m_armedVk = 0;
			m_armedAtom = 0;
		}
		break;
	case VK_LCONTROL:
		m_lctrlDown = down;
		break;
	case VK_RCONTROL:
		m_rctrlDown = down;
		break;
	case VK_CONTROL:
		// Rare generic code — mirror onto both.
		m_lctrlDown = down;
		m_rctrlDown = down;
		break;
	case VK_LSHIFT:
		m_lshiftDown = down;
		break;
	case VK_RSHIFT:
		m_rshiftDown = down;
		break;
	case VK_SHIFT:
		m_lshiftDown = down;
		m_rshiftDown = down;
		break;
	case VK_LMENU:
		m_laltDown = down;
		break;
	case VK_RMENU:
		m_raltDown = down;
		break;
	case VK_MENU:
		m_laltDown = down;
		m_raltDown = down;
		break;
	default:
		break;
	}

	if(keyUp && m_armed && pKbd->vkCode == m_armedVk)
	{
		m_armed = false;
		m_armedVk = 0;
		m_armedAtom = 0;
	}
}

bool CLowLevelHotKeys::CurrentModifiersMatch(UINT requiredModifiers) const
{
	const bool needWin = (requiredModifiers & MOD_WIN) != 0;
	const bool needCtrl = (requiredModifiers & MOD_CONTROL) != 0;
	const bool needShift = (requiredModifiers & MOD_SHIFT) != 0;
	const bool needAlt = (requiredModifiers & MOD_ALT) != 0;

	if(needWin != WinDown())
		return false;
	if(needCtrl != CtrlDown())
		return false;
	if(needShift != ShiftDown())
		return false;
	if(needAlt != AltDown())
		return false;

	return true;
}

bool CLowLevelHotKeys::TryFire(WPARAM wParam, const KBDLLHOOKSTRUCT *pKbd)
{
	if(wParam != WM_KEYDOWN && wParam != WM_SYSKEYDOWN)
	{
		return false;
	}

	if(m_hWnd == NULL || !::IsWindow(m_hWnd) || m_count <= 0)
	{
		return false;
	}

	const UINT vk = pKbd->vkCode;

	// Modifier keys themselves are never the "action" key for our bindings.
	switch(vk)
	{
	case VK_LWIN:
	case VK_RWIN:
	case VK_LCONTROL:
	case VK_RCONTROL:
	case VK_CONTROL:
	case VK_LSHIFT:
	case VK_RSHIFT:
	case VK_SHIFT:
	case VK_LMENU:
	case VK_RMENU:
	case VK_MENU:
		return false;
	default:
		break;
	}

	for(int i = 0; i < m_count; i++)
	{
		const LowLevelHotKeyBinding &b = m_bindings[i];
		if(b.vk != vk)
		{
			continue;
		}

		if(!CurrentModifiersMatch(b.modifiers))
		{
			continue;
		}

		// Typematic repeat: only fire once until key-up; still eat repeats.
		if(m_armed && m_armedVk == vk && m_armedAtom == b.atom)
		{
			return true;
		}

		m_armed = true;
		m_armedVk = vk;
		m_armedAtom = b.atom;

		// Same message shape as RegisterHotKey → WM_HOTKEY.
		const LPARAM lp = MAKELPARAM(b.modifiers, b.vk);
		if(!::PostMessage(m_hWnd, WM_HOTKEY, (WPARAM)b.atom, lp))
		{
			Log(StrF(_T("CLowLevelHotKeys::TryFire - PostMessage failed, error %d"), ::GetLastError()));
		}
		else
		{
			Log(StrF(_T("CLowLevelHotKeys::TryFire - posted WM_HOTKEY atom=%u vk=0x%X mod=0x%X"),
				(unsigned)b.atom, b.vk, b.modifiers));
		}

		// Prevent Start menu / shell from treating bare Win as a press.
		if(b.modifiers & MOD_WIN)
		{
			CancelWinKeyStartMenu();
		}

		return true;
	}

	return false;
}

void CLowLevelHotKeys::CancelWinKeyStartMenu()
{
	// Common trick: a synthetic Ctrl tap cancels the Win-key Start menu arming.
	INPUT inputs[2];
	ZeroMemory(inputs, sizeof(inputs));

	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wVk = VK_CONTROL;

	inputs[1].type = INPUT_KEYBOARD;
	inputs[1].ki.wVk = VK_CONTROL;
	inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

	::SendInput(2, inputs, sizeof(INPUT));
}
