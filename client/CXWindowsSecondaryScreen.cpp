#include "CXWindowsSecondaryScreen.h"
#include "CClient.h"
#include "CThread.h"
#include "CLog.h"
#include <assert.h>
#include <X11/X.h>
#include <X11/Xutil.h>
#define XK_MISCELLANY
#include <X11/keysymdef.h>
#include <X11/extensions/XTest.h>

//
// CXWindowsSecondaryScreen
//

CXWindowsSecondaryScreen::CXWindowsSecondaryScreen() :
								m_client(NULL),
								m_window(None)
{
	// do nothing
}

CXWindowsSecondaryScreen::~CXWindowsSecondaryScreen()
{
	assert(m_window == None);
}

void					CXWindowsSecondaryScreen::run()
{
	assert(m_window != None);

	for (;;) {
		// wait for and get the next event
		XEvent xevent;
		if (!getEvent(&xevent)) {
			break;
		}

		// handle event
		switch (xevent.type) {
		  case MappingNotify: {
			// keyboard mapping changed
			CDisplayLock display(this);
			XRefreshKeyboardMapping(&xevent.xmapping);
			updateKeys(display);
			updateKeycodeMap(display);
			updateModifierMap(display);
			updateModifiers(display);
			break;
		  }

		  case LeaveNotify: {
			// mouse moved out of hider window somehow.  hide the window.
			assert(m_window != None);
			CDisplayLock display(this);
			XUnmapWindow(display, m_window);
			break;
		  }

		  case SelectionClear:
			// we just lost the selection.  that means someone else
			// grabbed the selection so this screen is now the
			// selection owner.  report that to the server.
			if (lostClipboard(xevent.xselectionclear.selection,
								xevent.xselectionclear.time)) {
				m_client->onClipboardChanged();
			}
			break;

		  case SelectionNotify:
			// notification of selection transferred.  we shouldn't
			// get this here because we handle them in the selection
			// retrieval methods.  we'll just delete the property
			// with the data (satisfying the usual ICCCM protocol).
			if (xevent.xselection.property != None) {
				CDisplayLock display(this);
				XDeleteProperty(display, m_window, xevent.xselection.property);
			}
			break;

		  case SelectionRequest:
			// somebody is asking for clipboard data
			if (xevent.xselectionrequest.owner == m_window) {
				addClipboardRequest(m_window,
								xevent.xselectionrequest.requestor,
								xevent.xselectionrequest.selection,
								xevent.xselectionrequest.target,
								xevent.xselectionrequest.property,
								xevent.xselectionrequest.time);
			}
			break;

		  case PropertyNotify:
			// clipboard transfers involve property changes so forward
			// the event to the superclass.  we only care about the
			// deletion of properties.
			if (xevent.xproperty.state == PropertyDelete) {
				processClipboardRequest(xevent.xproperty.window,
								xevent.xproperty.atom,
								xevent.xproperty.time);
			}
			break;

		  case DestroyNotify:
			// looks like one of the windows that requested a clipboard
			// transfer has gone bye-bye.
			destroyClipboardRequest(xevent.xdestroywindow.window);
			break;
		}
	}
}

void					CXWindowsSecondaryScreen::stop()
{
	doStop();
}

void					CXWindowsSecondaryScreen::open(CClient* client)
{
	assert(m_client == NULL);
	assert(client   != NULL);

	// set the client
	m_client = client;

	// open the display
	openDisplay();

	// verify the availability of the XTest extension
	CDisplayLock display(this);
	int majorOpcode, firstEvent, firstError;
	if (!XQueryExtension(display, XTestExtensionName,
								&majorOpcode, &firstEvent, &firstError))
		throw int(6);	// FIXME -- make exception for this

	// update key state
	updateKeys(display);
	updateKeycodeMap(display);
	updateModifierMap(display);
	updateModifiers(display);
}

void					CXWindowsSecondaryScreen::close()
{
	assert(m_client != NULL);

	// close the display
	closeDisplay();

	// done with client
	m_client = NULL;
}

void					CXWindowsSecondaryScreen::enter(SInt32 x, SInt32 y)
{
	assert(m_window != None);

	CDisplayLock display(this);

	// warp to requested location
	XTestFakeMotionEvent(display, getScreen(), x, y, CurrentTime);
	XSync(display, False);

	// show cursor
	XUnmapWindow(display, m_window);

	// update our keyboard state to reflect the local state
	updateKeys(display);
	updateModifiers(display);
}

void					CXWindowsSecondaryScreen::leave()
{
	CDisplayLock display(this);
	leaveNoLock(display);
}

void					CXWindowsSecondaryScreen::keyDown(
								KeyID key, KeyModifierMask mask)
{
	Keystrokes keys;
	KeyCode keycode;

	CDisplayLock display(this);

	// get the sequence of keys to simulate key press and the final
	// modifier state.
	m_mask = mapKey(keys, keycode, key, mask, True);
	if (keys.empty())
		return;

	// generate key events
	for (Keystrokes::const_iterator k = keys.begin(); k != keys.end(); ++k)
		XTestFakeKeyEvent(display, k->first, k->second, CurrentTime);

	// note that key is now down
	m_keys[keycode] = true;

	// update
	XSync(display, False);
}

void					CXWindowsSecondaryScreen::keyRepeat(
								KeyID, KeyModifierMask, SInt32)
{
	CDisplayLock display(this);
	// FIXME
}

void					CXWindowsSecondaryScreen::keyUp(
								KeyID key, KeyModifierMask mask)
{
	Keystrokes keys;
	KeyCode keycode;

	CDisplayLock display(this);

	// get the sequence of keys to simulate key release and the final
	// modifier state.
	m_mask = mapKey(keys, keycode, key, mask, False);
	if (keys.empty())
		return;

	// generate key events
	for (Keystrokes::const_iterator k = keys.begin(); k != keys.end(); ++k)
		XTestFakeKeyEvent(display, k->first, k->second, CurrentTime);

	// note that key is now up
	m_keys[keycode] = false;

	// update
	XSync(display, False);
}

void					CXWindowsSecondaryScreen::mouseDown(ButtonID button)
{
	CDisplayLock display(this);
	XTestFakeButtonEvent(display, mapButton(button), True, CurrentTime);
	XSync(display, False);
}

void					CXWindowsSecondaryScreen::mouseUp(ButtonID button)
{
	CDisplayLock display(this);
	XTestFakeButtonEvent(display, mapButton(button), False, CurrentTime);
	XSync(display, False);
}

void					CXWindowsSecondaryScreen::mouseMove(SInt32 x, SInt32 y)
{
	CDisplayLock display(this);
	XTestFakeMotionEvent(display, getScreen(), x, y, CurrentTime);
	XSync(display, False);
}

void					CXWindowsSecondaryScreen::mouseWheel(SInt32)
{
	CDisplayLock display(this);
	// FIXME
}

void					CXWindowsSecondaryScreen::setClipboard(
								const IClipboard* clipboard)
{
	// FIXME -- don't use CurrentTime
	setDisplayClipboard(clipboard, m_window, CurrentTime);
}

void					CXWindowsSecondaryScreen::grabClipboard()
{
	// FIXME -- don't use CurrentTime
	setDisplayClipboard(NULL, m_window, CurrentTime);
}

void					CXWindowsSecondaryScreen::getSize(
								SInt32* width, SInt32* height) const
{
	getScreenSize(width, height);
}

SInt32					CXWindowsSecondaryScreen::getJumpZoneSize() const
{
	return 0;
}

void					CXWindowsSecondaryScreen::getClipboard(
								IClipboard* clipboard) const
{
	// FIXME -- don't use CurrentTime
	getDisplayClipboard(clipboard, m_window, CurrentTime);
}

void					CXWindowsSecondaryScreen::onOpenDisplay()
{
	assert(m_window == None);

	CDisplayLock display(this);

	// create the cursor hiding window.  this window is used to hide the
	// cursor when it's not on the screen.  the window is hidden as soon
	// as the cursor enters the screen or the display's real cursor is
	// moved.
	XSetWindowAttributes attr;
	attr.event_mask            = LeaveWindowMask;
	attr.do_not_propagate_mask = 0;
	attr.override_redirect     = True;
	attr.cursor                = createBlankCursor();
	m_window = XCreateWindow(display, getRoot(), 0, 0, 1, 1, 0, 0,
								InputOnly, CopyFromParent,
								CWDontPropagate | CWEventMask |
								CWOverrideRedirect | CWCursor,
								&attr);

	// become impervious to server grabs
	XTestGrabControl(display, True);

	// hide the cursor
	leaveNoLock(display);
}

void					CXWindowsSecondaryScreen::onCloseDisplay()
{
	assert(m_window != None);

	// no longer impervious to server grabs
	CDisplayLock display(this);
	XTestGrabControl(display, False);

	// destroy window
	XDestroyWindow(display, m_window);
	m_window = None;
}

void					CXWindowsSecondaryScreen::leaveNoLock(Display* display)
{
	assert(display  != NULL);
	assert(m_window != None);

	// move hider window under the mouse (rather than moving the mouse
	// somewhere else on the screen)
	int x, y, dummy;
	unsigned int dummyMask;
	Window dummyWindow;
	XQueryPointer(display, getRoot(), &dummyWindow, &dummyWindow,
								&x, &y, &dummy, &dummy, &dummyMask);
	XMoveWindow(display, m_window, x, y);

	// raise and show the hider window
	XMapRaised(display, m_window);

	// hide cursor by moving it into the hider window
	XWarpPointer(display, None, m_window, 0, 0, 0, 0, 0, 0);
}

unsigned int			CXWindowsSecondaryScreen::mapButton(
								ButtonID id) const
{
	// FIXME -- should use button mapping?
	return static_cast<unsigned int>(id);
}

KeyModifierMask			CXWindowsSecondaryScreen::mapKey(
								Keystrokes& keys,
								KeyCode& keycode,
								KeyID id, KeyModifierMask mask,
								Bool press) const
{
	// note -- must have display locked on entry

	// the system translates key events into characters depending
	// on the modifier key state at the time of the event.  to
	// generate the right keysym we need to set the modifier key
	// states appropriately.
	//
	// the mask passed by the caller is the desired mask.  however,
	// there may not be a keycode mapping to generate the desired
	// keysym with that mask.  we override the bits in the mask
	// that cannot be accomodated.

	// lookup the a keycode for this key id.  also return the
	// key modifier mask required.
	unsigned int outMask;
	if (!findKeyCode(keycode, outMask, id, maskToX(mask))) {
		// we cannot generate the desired keysym because no key
		// maps to that keysym.  just return the current mask.
		return m_mask;
	}

	// if we cannot match the modifier mask then don't return any
	// keys and just return the current mask.
	if ((outMask & m_modifierMask) != outMask) {
		return m_mask;
	}

	// note if the key is a modifier
	ModifierMap::const_iterator index = m_keycodeToModifier.find(keycode);
	const bool isModifier = (index != m_keycodeToModifier.end());

	// add the key events required to get to the modifier state
	// necessary to generate an event yielding id.  also save the
	// key events required to restore the state.  if the key is
	// a modifier key then skip this because modifiers should not
	// modify modifiers.
	Keystrokes undo;
	if (outMask != m_mask && !isModifier) {
		for (unsigned int i = 0; i < 8; ++i) {
			unsigned int bit = (1 << i);
			if ((outMask & bit) != (m_mask & bit)) {
				// get list of keycodes for the modifier.  there must
				// be at least one.
				const KeyCode* modifierKeys =
								&m_modifierToKeycode[i * m_keysPerModifier];
				assert(modifierKeys[0] != 0);

				if ((outMask & bit) != 0) {
					// modifier is not active but should be.  if the
					// modifier is a toggle then toggle it on with a
					// press/release, otherwise activate it with a
					// press.  use the first keycode for the modifier.
					const KeyCode modifierKey = modifierKeys[0];
					keys.push_back(std::make_pair(modifierKey, True));
					if ((bit & m_toggleModifierMask) != 0) {
						keys.push_back(std::make_pair(modifierKey, False));
						undo.push_back(std::make_pair(modifierKey, False));
						undo.push_back(std::make_pair(modifierKey, True));
					}
					else {
						undo.push_back(std::make_pair(modifierKey, False));
					}
				}

				else {
					// modifier is active but should not be.  if the
					// modifier is a toggle then toggle it off with a
					// press/release, otherwise deactivate it with a
					// release.  we must check each keycode for the
					// modifier if not a toggle.
					if (bit & m_toggleModifierMask) {
						const KeyCode modifierKey = modifierKeys[0];
						keys.push_back(std::make_pair(modifierKey, True));
						keys.push_back(std::make_pair(modifierKey, False));
						undo.push_back(std::make_pair(modifierKey, False));
						undo.push_back(std::make_pair(modifierKey, True));
					}
					else {
						for (unsigned int j = 0; j < m_keysPerModifier; ++j) {
							const KeyCode key = modifierKeys[j];
							if (m_keys[key]) {
								keys.push_back(std::make_pair(key, False));
								undo.push_back(std::make_pair(key, True));
							}
						}
					}
				}
			}
		}
	}

	// add the key event
	keys.push_back(std::make_pair(keycode, press));

	// add key events to restore the modifier state.  apply events in
	// the reverse order that they're stored in undo.
	while (!undo.empty()) {
		keys.push_back(undo.back());
		undo.pop_back();
	}

	// if the key is a modifier key then compute the modifier map after
	// this key is pressed.
	mask = m_mask;
	if (isModifier) {
		// get modifier
		const unsigned int modifierBit = (1 << index->second);

		// toggle keys modify the state on press if toggling on and on
		// release if toggling off.  other keys set the bit on press
		// and clear the bit on release.
		if ((modifierBit & m_toggleModifierMask) != 0) {
			if (((mask & modifierBit) == 0) == press)
				mask ^= modifierBit;
		}
		else if (press) {
			mask |= modifierBit;
		}
		else {
			// can't reset bit until all keys that set it are released.
			// scan those keys to see if any (except keycode) are pressed.
			bool down = false;
			const KeyCode* modifierKeys = &m_modifierToKeycode[
											index->second * m_keysPerModifier];
			for (unsigned int j = 0; !down && j < m_keysPerModifier; ++j) {
				if (m_keys[modifierKeys[j]])
					down = true;
			}
			if (!down)
				mask &= ~modifierBit;
		}
	}

	return mask;
}

bool					CXWindowsSecondaryScreen::findKeyCode(
								KeyCode& keycode,
								unsigned int& maskOut,
								KeyID id,
								unsigned int maskIn) const
{
	// find a keycode to generate id.  XKeysymToKeycode() almost does
	// what we need but won't tell us which index to use with the
	// keycode.  return false if there's no keycode to generate id.
	KeyCodeMap::const_iterator index = m_keycodeMap.find(id);
	if (index == m_keycodeMap.end())
		return false;

	// save the keycode
	keycode = index->second.keycode;

	// compute output mask.  that's the set of modifiers that need to
	// be enabled when the keycode event is encountered in order to
	// generate the id keysym and match maskIn.  it's possible that
	// maskIn wants, say, a shift key to be down but that would make
	// it impossible to generate the keysym.  in that case we must
	// override maskIn.
	//
	// this is complicated by caps/shift-lock and num-lock.  for
	// example, if id is a keypad keysym and maskIn indicates that
	// shift is not active but keyMask indicates that shift is
	// required then we can either activate shift and then send
	// the keycode or we can activate num-lock and then send the
	// keycode.  the latter is better because applications may
	// treat, say, shift+Home differently than Home.
	maskOut = (maskIn & ~index->second.keyMaskMask);
	if (IsKeypadKey(id) || IsPrivateKeypadKey(id)) {
		// compare shift state of maskIn and keyMask
		const bool agree = ((maskIn & ShiftMask) ==
							(index->second.keyMask & ShiftMask));

		// get num-lock state
		const bool numLockActive = ((m_mask & m_numLockMask) != 0);

		// if num-lock is active and the shift states agree or if
		// num-lock is not active and the shift states do not agree
		// then we should toggle num-lock.
		if (numLockActive == agree) {
			maskOut |= (maskIn & ShiftMask) |
						(index->second.keyMask & ~ShiftMask);
			if (numLockActive)
				maskOut &= ~m_numLockMask;
			else
				maskOut |= m_numLockMask;
		}
		else {
			maskOut |= index->second.keyMask;
		}
	}
	else {
		// compare shift state of maskIn and keyMask
		const bool agree = ((maskIn & ShiftMask) ==
							(index->second.keyMask & ShiftMask));

		// get caps-lock state
		const bool capsLockActive = ((m_mask & m_capsLockMask) != 0);

		// if caps-lock is active and the shift states agree or if
		// caps-lock is not active and the shift states do not agree
		// then we should toggle caps-lock.
		if (capsLockActive == agree) {
			maskOut |= (maskIn & ShiftMask) |
						(index->second.keyMask & ~ShiftMask);
			if (capsLockActive)
				maskOut &= ~m_capsLockMask;
			else
				maskOut |= m_capsLockMask;
		}
		else {
			maskOut |= index->second.keyMask;
		}
	}

	return true;
}

unsigned int			CXWindowsSecondaryScreen::maskToX(
								KeyModifierMask inMask) const
{
	// FIXME -- should be configurable.  not using Mod3Mask.
	unsigned int outMask = 0;
	if (inMask & KeyModifierShift)
		outMask |= ShiftMask;
	if (inMask & KeyModifierControl)
		outMask |= ControlMask;
	if (inMask & KeyModifierAlt)
		outMask |= Mod1Mask;
	if (inMask & KeyModifierMeta)
		outMask |= Mod4Mask;
	if (inMask & KeyModifierCapsLock)
		outMask |= LockMask;
	if (inMask & KeyModifierNumLock)
		outMask |= Mod2Mask;
	if (inMask & KeyModifierScrollLock)
		outMask |= Mod5Mask;
	return outMask;
}

void					CXWindowsSecondaryScreen::updateKeys(Display* display)
{
	// ask server which keys are pressed
	char keys[32];
	XQueryKeymap(display, keys);

	// transfer to our state
	for (unsigned int i = 0, j = 0; i < 32; j += 8, ++i) {
		m_keys[j + 0] = ((keys[i] & 0x01) != 0);
		m_keys[j + 1] = ((keys[i] & 0x02) != 0);
		m_keys[j + 2] = ((keys[i] & 0x04) != 0);
		m_keys[j + 3] = ((keys[i] & 0x08) != 0);
		m_keys[j + 4] = ((keys[i] & 0x10) != 0);
		m_keys[j + 5] = ((keys[i] & 0x20) != 0);
		m_keys[j + 6] = ((keys[i] & 0x40) != 0);
		m_keys[j + 7] = ((keys[i] & 0x80) != 0);
	}
}

void					CXWindowsSecondaryScreen::updateModifiers(
								Display*)
{
	// update active modifier mask
	m_mask = 0;
	for (unsigned int i = 0; i < 8; ++i) {
		const unsigned int bit = (1 << i);
		if ((bit & m_toggleModifierMask) == 0) {
			for (unsigned int j = 0; j < m_keysPerModifier; ++j) {
				if (m_keys[m_modifierToKeycode[i * m_keysPerModifier + j]])
					m_mask |= bit;
			}
		}
		else {
			// FIXME -- not sure how to check current lock states
		}
	}
}

void					CXWindowsSecondaryScreen::updateKeycodeMap(
								Display* display)
{
	// get the number of keycodes
	int minKeycode, maxKeycode;
	XDisplayKeycodes(display, &minKeycode, &maxKeycode);
	const int numKeycodes = maxKeycode - minKeycode + 1;

	// get the keyboard mapping for all keys
	int keysymsPerKeycode;
	KeySym* keysyms = XGetKeyboardMapping(display,
								minKeycode, numKeycodes,
								&keysymsPerKeycode);

	// restrict keysyms per keycode to 2 because, frankly, i have no
	// idea how/what modifiers are used to access keysyms beyond the
	// first 2.
	int numKeysyms = 2;	// keysymsPerKeycode

	// initialize
	KeyCodeMask entry;
	m_keycodeMap.clear();

	// insert keys
	for (int i = 0; i < numKeycodes; ++i) {
		// how many keysyms for this keycode?
		int n;
		for (n = 0; n < numKeysyms; ++n) {
			if (keysyms[i * keysymsPerKeycode + n] == NoSymbol) {
				break;
			}
		}

		// move to next keycode if there are no keysyms
		if (n == 0) {
			continue;
		}

		// set the mask of modifiers that this keycode uses
		entry.keyMaskMask = (n == 1) ? 0 : (ShiftMask | LockMask);

		// add entries for this keycode
		entry.keycode = static_cast<KeyCode>(minKeycode + i);
		for (int j = 0; j < numKeysyms; ++j) {
			entry.keyMask = (j == 0) ? 0 : ShiftMask;
			m_keycodeMap.insert(std::make_pair(keysyms[i *
									keysymsPerKeycode + j], entry));
		}
	}

	// clean up
	XFree(keysyms);
}

void					CXWindowsSecondaryScreen::updateModifierMap(
								Display* display)
{
	// get modifier map from server
	XModifierKeymap* keymap = XGetModifierMapping(display);

	// initialize
	m_modifierMask       = 0;
	m_toggleModifierMask = 0;
	m_numLockMask        = 0;
	m_capsLockMask       = 0;
	m_keysPerModifier    = keymap->max_keypermod;
	m_modifierToKeycode.clear();
	m_modifierToKeycode.resize(8 * m_keysPerModifier);

	// set keycodes and masks
	for (unsigned int i = 0; i < 8; ++i) {
		const unsigned int bit = (1 << i);
		for (unsigned int j = 0; j < m_keysPerModifier; ++j) {
			KeyCode keycode = keymap->modifiermap[i * m_keysPerModifier + j];

			// save in modifier to keycode
			m_modifierToKeycode[i * m_keysPerModifier + j] = keycode;

			// save in keycode to modifier
			m_keycodeToModifier.insert(std::make_pair(keycode, i));

			// modifier is enabled if keycode isn't 0
			if (keycode != 0)
				m_modifierMask |= bit;

			// modifier is a toggle if the keysym is a toggle modifier
			const KeySym keysym = XKeycodeToKeysym(display, keycode, 0);
			if (isToggleKeysym(keysym)) {
				m_toggleModifierMask |= bit;

				// note num/caps-lock
				if (keysym == XK_Num_Lock)
					m_numLockMask |= bit;
				if (keysym == XK_Caps_Lock)
					m_capsLockMask |= bit;
			}
		}
	}

	XFreeModifiermap(keymap);
}

bool					CXWindowsSecondaryScreen::isToggleKeysym(KeySym key)
{
	switch (key) {
	case XK_Caps_Lock:
	case XK_Shift_Lock:
	case XK_Num_Lock:
	case XK_Scroll_Lock:
		return true;

	default:
		return false;
	}
}