/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2003 Chris Schoeneman
 * 
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file COPYING that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef CXWINDOWSKEYMAPPER_H
#define CXWINDOWSKEYMAPPER_H

#include "IKeyState.h"
#include "stdmap.h"
#if defined(X_DISPLAY_MISSING)
#	error X11 is required to build synergy
#else
#	include <X11/Xlib.h>
#endif

//! X Windows key mapper
/*!
This class maps KeyIDs to keystrokes.
*/
class CXWindowsKeyMapper {
public:
	CXWindowsKeyMapper();
	~CXWindowsKeyMapper();

	//! @name manipulators
	//@{

	//! Update key mapper
	/*!
	Updates the key mapper's internal tables according to the display's
	current keyboard mapping and updates \c keyState.
	*/
	void				update(Display*, IKeyState* keyState);

	//@}
	//! @name accessors
	//@{

	//! Map key press/repeat to keystrokes
	/*!
	Converts a press/repeat of key \c id with the modifiers as given
	in \c desiredMask into the keystrokes necessary to synthesize
	that key event.  Returns the platform specific code of the key
	being pressed, or 0 if the key cannot be mapped or \c isAutoRepeat
	is true and the key does not auto-repeat.
	*/
	KeyButton			mapKey(IKeyState::Keystrokes&,
							const IKeyState& keyState, KeyID id,
							KeyModifierMask desiredMask,
							bool isAutoRepeat) const;

	//! Convert X modifier mask to synergy mask
	/*!
	Returns the synergy modifier mask corresponding to the given X
	modifier mask.
	*/
	KeyModifierMask		mapModifier(unsigned int state) const;

	//@}

private:
	class KeyMapping {
	public:
		KeyMapping();

	public:
		// KeyCode to generate keysym and whether keycode[i] is
		// sensitive to shift and mode switch.
		KeyCode			m_keycode[4];
		bool			m_shiftSensitive[4];
		bool			m_modeSwitchSensitive[4];

		// the modifier mask of keysym or 0 if not a modifier
		KeyModifierMask	m_modifierMask;

		// whether keysym is sensitive to caps and num lock
		bool			m_numLockSensitive;
		bool			m_capsLockSensitive;
	};
	typedef std::map<KeySym, KeyMapping> KeySymMap;
	typedef KeySymMap::const_iterator KeySymIndex;

	// save the current keyboard mapping and note the currently
	// pressed keys in \c keyState.
	void				updateKeysymMap(Display* display, IKeyState* keyState);

	// note interesting modifier KeySyms
	void				updateModifiers();

	// map a modifier index and its KeySym to a modifier mask.  also
	// save the modifier mask in one of m_*Mask.
	KeyModifierMask		mapToModifierMask(unsigned int, KeySym);

	// convert a KeyID to a KeySym
	KeySym				keyIDToKeySym(KeyID id, KeyModifierMask mask) const;

	// map a KeySym into the keystrokes to produce it
	KeyButton			mapToKeystrokes(IKeyState::Keystrokes& keys,
							const IKeyState& keyState,
							KeySymIndex keyIndex,
							bool isAutoRepeat) const;

	// choose the best set of modifiers to generate the KeySym
	unsigned int		findBestKeyIndex(KeySymIndex keyIndex,
							KeyModifierMask currentMask) const;

	// returns true if the sense of shift is inverted for KeySym
	bool				isShiftInverted(KeySymIndex keyIndex,
							KeyModifierMask currentMask) const;

	// returns the keystrokes to adjust the modifiers into the desired
	// state the keystrokes to get back to the current state.
	bool				adjustModifiers(IKeyState::Keystrokes& keys,
							IKeyState::Keystrokes& undo,
							const IKeyState& keyState,
							KeyModifierMask desiredMask) const;

	// returns true if keysym is sensitive to the NumLock state
	bool				isNumLockSensitive(KeySym keysym) const;

	// returns true if keysym is sensitive to the CapsLock state
	bool				isCapsLockSensitive(KeySym keysym) const;

private:
	// keysym to keycode mapping
	KeySymMap			m_keysymMap;

	// the keyboard control state the last time this screen was entered
	XKeyboardState		m_keyControl;

	// modifier keysyms
	KeySym				m_modeSwitchKeysym;

	// modifier masks
	unsigned int		m_altMask;
	unsigned int		m_metaMask;
	unsigned int		m_superMask;
	unsigned int		m_modeSwitchMask;
	unsigned int		m_numLockMask;
	unsigned int		m_scrollLockMask;
};

#endif
