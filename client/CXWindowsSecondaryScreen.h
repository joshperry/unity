#ifndef CXWINDOWSSECONDARYSCREEN_H
#define CXWINDOWSSECONDARYSCREEN_H

#include "CXWindowsScreen.h"
#include "ISecondaryScreen.h"
#include <vector>

class CXWindowsSecondaryScreen : public CXWindowsScreen, public ISecondaryScreen {
  public:
	CXWindowsSecondaryScreen();
	virtual ~CXWindowsSecondaryScreen();

	// ISecondaryScreen overrides
	virtual void		run();
	virtual void		stop();
	virtual void		open(CClient*);
	virtual void		close();
	virtual void		enter(SInt32 xAbsolute, SInt32 yAbsolute);
	virtual void		leave();
	virtual void		keyDown(KeyID, KeyModifierMask);
	virtual void		keyRepeat(KeyID, KeyModifierMask, SInt32 count);
	virtual void		keyUp(KeyID, KeyModifierMask);
	virtual void		mouseDown(ButtonID);
	virtual void		mouseUp(ButtonID);
	virtual void		mouseMove(SInt32 xAbsolute, SInt32 yAbsolute);
	virtual void		mouseWheel(SInt32 delta);
	virtual void		setClipboard(ClipboardID, const IClipboard*);
	virtual void		grabClipboard(ClipboardID);
	virtual void		getSize(SInt32* width, SInt32* height) const;
	virtual SInt32		getJumpZoneSize() const;
	virtual void		getClipboard(ClipboardID, IClipboard*) const;

  protected:
	// CXWindowsScreen overrides
	virtual void		onOpenDisplay();
	virtual void		onCloseDisplay();

  private:
	struct KeyCodeMask {
	public:
		KeyCode			keycode;
		unsigned int	keyMask;
		unsigned int	keyMaskMask;
	};
	typedef std::pair<KeyCode, Bool> Keystroke;
	typedef std::vector<Keystroke> Keystrokes;
	typedef std::vector<KeyCode> KeyCodes;
	typedef std::map<KeyID, KeyCodeMask> KeyCodeMap;
	typedef std::map<KeyCode, unsigned int> ModifierMap;

	void				leaveNoLock(Display*);
	unsigned int		mapButton(ButtonID button) const;

	unsigned int		mapKey(Keystrokes&, KeyCode&, KeyID,
								KeyModifierMask, Bool press) const;
	bool				findKeyCode(KeyCode&, unsigned int&,
								KeyID id, unsigned int) const;
	unsigned int		maskToX(KeyModifierMask) const;

	void				updateKeys(Display* display);
	void				updateKeycodeMap(Display* display);
	void				updateModifiers(Display* display);
	void				updateModifierMap(Display* display);
	static bool			isToggleKeysym(KeySym);

  private:
	CClient*			m_client;
	Window				m_window;

	// set entries indicate keys that are pressed.  indexed by keycode.
	bool				m_keys[256];

	// current active modifiers (X key masks)
	unsigned int		m_mask;

	// maps key IDs to X keycodes and the X modifier key mask needed
	// to generate the right keysym
	KeyCodeMap			m_keycodeMap;

	// the modifiers that have keys bound to them
	unsigned int		m_modifierMask;

	// set bits indicate modifiers that toggle (e.g. caps-lock)
	unsigned int		m_toggleModifierMask;

	// masks that indicate which modifier bits are num-lock and caps-lock
	unsigned int		m_numLockMask;
	unsigned int		m_capsLockMask;

	// map X modifier key indices to the key codes bound to them
	unsigned int		m_keysPerModifier;
	KeyCodes			m_modifierToKeycode;

	// maps keycodes to modifier indices
	ModifierMap			m_keycodeToModifier;
};

#endif
