#ifndef CXWINDOWSPRIMARYSCREEN_H
#define CXWINDOWSPRIMARYSCREEN_H

#include "KeyTypes.h"
#include "MouseTypes.h"
#include "CXWindowsScreen.h"
#include "IPrimaryScreen.h"

class CXWindowsPrimaryScreen : public CXWindowsScreen, public IPrimaryScreen {
public:
	CXWindowsPrimaryScreen();
	virtual ~CXWindowsPrimaryScreen();

	// IPrimaryScreen overrides
	virtual void		run();
	virtual void		stop();
	virtual void		open(CServer*);
	virtual void		close();
	virtual void		enter(SInt32 xAbsolute, SInt32 yAbsolute);
	virtual void		leave();
	virtual void		warpCursor(SInt32 xAbsolute, SInt32 yAbsolute);
	virtual void		setClipboard(ClipboardID, const IClipboard*);
	virtual void		grabClipboard(ClipboardID);
	virtual void		getSize(SInt32* width, SInt32* height) const;
	virtual SInt32		getJumpZoneSize() const;
	virtual void		getClipboard(ClipboardID, IClipboard*) const;
	virtual KeyModifierMask	getToggleMask() const;

protected:
	// CXWindowsScreen overrides
	virtual void		onOpenDisplay();
	virtual void		onCloseDisplay();
	virtual long		getEventMask(Window) const;

private:
	void				selectEvents(Display*, Window) const;
	void				warpCursorNoLock(Display*,
								SInt32 xAbsolute, SInt32 yAbsolute);

	KeyModifierMask		mapModifier(unsigned int state) const;
	KeyID				mapKey(XKeyEvent*) const;
	ButtonID			mapButton(unsigned int button) const;

	void				updateModifierMap(Display* display);

	class CKeyEventInfo {
	public:
		int				m_event;
		Window			m_window;
		Time			m_time;
		KeyCode			m_keycode;
	};
	static Bool			findKeyEvent(Display*, XEvent* xevent, XPointer arg);

private:
	CServer*			m_server;
	bool				m_active;
	Window				m_window;

	// note if caps lock key toggles on up/down (false) or on
	// transition (true)
	bool				m_capsLockHalfDuplex;

	// masks that indicate which modifier bits are for toggle keys
	unsigned int		m_numLockMask;
	unsigned int		m_capsLockMask;
	unsigned int		m_scrollLockMask;
};

#endif
