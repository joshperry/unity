/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2004 Chris Schoeneman
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

#ifndef COSXKEYSTATE_H
#define COSXKEYSTATE_H

#include "CKeyState.h"
#include "stdmap.h"
#include <Carbon/Carbon.h>

//! OS X key state
/*!
A key state for OS X.
*/
class COSXKeyState : public CKeyState {
public:
	// OS X uses a physical key if 0 for the 'A' key.  synergy reserves
	// KeyButton 0 so we offset all OS X physical key ids by this much
	// when used as a KeyButton and by minus this much to map a KeyButton
	// to a physical button.
	enum {
		KeyButtonOffset = 1
	};

	COSXKeyState();
	virtual ~COSXKeyState();

	//! Map physical key id to a KeyButton id
	/*!
	Maps an OS X key code to a KeyButton.  This simply remaps the ids
	so we don't use KeyButton 0.
	*/
	static KeyButton	mapKeyCodeToKeyButton(UInt32 keyCode);

	//! Map KeyButton id to a physical key id
	/*!
	Maps a KeyButton to an OS X key code.  This is the inverse of
	mapKeyCodeToKeyButton.
	*/
	static UInt32		mapKeyButtonToKeyCode(KeyButton keyButton);

	//! Map key event to a key
	/*!
	Converts a key event into a KeyID and the shadow modifier state
	to a modifier mask.
	*/
	KeyID				mapKeyFromEvent(EventRef event,
							KeyModifierMask* maskOut) const;

	//! Handle modifier key change
	/*!
	Determines which modifier keys have changed and updates the modifier
	state and sends key events as appropriate.
	*/
	void				handleModifierKeys(void* target,
							KeyModifierMask oldMask, KeyModifierMask newMask);

	// IKeyState overrides
	virtual void		setHalfDuplexMask(KeyModifierMask);
	virtual bool		fakeCtrlAltDel();
	virtual const char*	getKeyName(KeyButton) const;
	virtual void		sendKeyEvent(void* target,
							bool press, bool isAutoRepeat,
							KeyID key, KeyModifierMask mask,
							SInt32 count, KeyButton button);

protected:
	// IKeyState overrides
	virtual void		doUpdateKeys();
	virtual void		doFakeKeyEvent(KeyButton button,
							bool press, bool isAutoRepeat);
	virtual KeyButton	mapKey(Keystrokes& keys, KeyID id,
							KeyModifierMask desiredMask,
							bool isAutoRepeat) const;

private:
	bool				adjustModifiers(Keystrokes& keys,
							Keystrokes& undo,
							KeyModifierMask desiredMask) const;
	void				addKeyButton(KeyButtons& keys, KeyID id) const;
	void				handleModifierKey(void* target, KeyID id, bool down);

private:
	typedef std::map<KeyID, KeyButton> CKeyMap;

	CKeyMap				m_keyMap;

	static const KeyID	s_virtualKey[];
};

#endif
