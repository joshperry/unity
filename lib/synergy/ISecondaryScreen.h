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

#ifndef ISECONDARYSCREEN_H
#define ISECONDARYSCREEN_H

#include "IInterface.h"
#include "MouseTypes.h"

//! Secondary screen interface
/*!
This interface defines the methods common to all platform dependent
secondary screen implementations.
*/
class ISecondaryScreen : public IInterface {
public:
	//! @name accessors
	//@{

	//! Fake mouse press/release
	/*!
	Synthesize a press or release of mouse button \c id.
	*/
	virtual void		fakeMouseButton(ButtonID id, bool press) const = 0;

	//! Fake mouse move
	/*!
	Synthesize a mouse move to the absolute coordinates \c x,y.
	*/
	virtual void		fakeMouseMove(SInt32 x, SInt32 y) const = 0;

	//! Fake mouse wheel
	/*!
	Synthesize a mouse wheel event of amount \c delta.
	*/
	virtual void		fakeMouseWheel(SInt32 delta) const = 0;

	//@}
};

#endif
