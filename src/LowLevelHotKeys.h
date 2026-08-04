#pragma once

// Fallback for system-reserved Win+* combos (e.g. Win+C) that RegisterHotKey cannot claim.
// Only used for Ditto activate hotkeys (DittoHotKey / 2 / 3) that include MOD_WIN and fail registration.
// Posts WM_HOTKEY with the same atom so existing OnHotKey handlers work unchanged.

struct LowLevelHotKeyBinding
{
	UINT modifiers; // MOD_* flags used by RegisterHotKey
	UINT vk;
	ATOM atom;
};

class CLowLevelHotKeys
{
public:
	CLowLevelHotKeys();
	~CLowLevelHotKeys();

	void SetHwnd(HWND hWnd) { m_hWnd = hWnd; }

	void Clear();
	bool Add(UINT modifiers, UINT vk, ATOM atom);

	// Install WH_KEYBOARD_LL when there is at least one binding.
	bool Start();
	void Stop();

	bool IsActive() const { return m_hHook != NULL; }
	int GetCount() const { return m_count; }

private:
	enum { MAX_BINDINGS = 8 };

	static LRESULT CALLBACK LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam);
	LRESULT ProcessHook(int nCode, WPARAM wParam, LPARAM lParam);

	void UpdateModifierState(WPARAM wParam, const KBDLLHOOKSTRUCT *pKbd);
	bool WinDown() const { return m_lwinDown || m_rwinDown; }
	bool CtrlDown() const { return m_lctrlDown || m_rctrlDown; }
	bool ShiftDown() const { return m_lshiftDown || m_rshiftDown; }
	bool AltDown() const { return m_laltDown || m_raltDown; }
	bool CurrentModifiersMatch(UINT requiredModifiers) const;
	bool TryFire(WPARAM wParam, const KBDLLHOOKSTRUCT *pKbd);
	void CancelWinKeyStartMenu();
	void ResetModifierState();

	HWND m_hWnd;
	HHOOK m_hHook;
	LowLevelHotKeyBinding m_bindings[MAX_BINDINGS];
	int m_count;

	// Track left/right independently. Do not rely on GetAsyncKeyState inside the
	// LL hook for the key currently being delivered — state may not be updated yet.
	bool m_lwinDown;
	bool m_rwinDown;
	bool m_lctrlDown;
	bool m_rctrlDown;
	bool m_lshiftDown;
	bool m_rshiftDown;
	bool m_laltDown;
	bool m_raltDown;

	// One fire per physical key-down (ignore typematic repeats).
	bool m_armed;
	UINT m_armedVk;
	ATOM m_armedAtom;
};

extern CLowLevelHotKeys g_LowLevelHotKeys;
