#ifndef IPRIMARYSCREENRECEIVER_H
#define IPRIMARYSCREENRECEIVER_H

#include "IInterface.h"
#include "KeyTypes.h"
#include "MouseTypes.h"

//! Primary screen event receiver interface
/*!
The interface for receiving notification of events on the primary
screen.  The server implements this interface to handle user input.
Platform dependent primary screen implementation will need to take
an IPrimaryScreenReceiver* and notify it of events.
*/
class IPrimaryScreenReceiver : public IInterface {
public:
	//! Notify of screen saver change
	/*!
	Called when the screensaver is activated or deactivated.
	*/
	virtual void		onScreensaver(bool activated) = 0;

	// call to notify of events.  onMouseMovePrimary() returns
	// true iff the mouse enters a jump zone and jumps.
	//! Notify of key press
	virtual void		onKeyDown(KeyID, KeyModifierMask) = 0;
	//! Notify of key release
	virtual void		onKeyUp(KeyID, KeyModifierMask) = 0;
	//! Notify of key repeat
	virtual void		onKeyRepeat(KeyID, KeyModifierMask, SInt32 count) = 0;
	//! Notify of mouse button press
	virtual void		onMouseDown(ButtonID) = 0;
	//! Notify of mouse button release
	virtual void		onMouseUp(ButtonID) = 0;
	//! Notify of mouse motion
	/*!
	Called when the mouse has moved while on the primary screen.  \c x
	and \c y are the absolute screen position of the mouse.  Return
	true iff the mouse enters a jump zone and jumps.
	*/
	virtual bool		onMouseMovePrimary(SInt32 x, SInt32 y) = 0;
	//! Notify of mouse motion
	/*!
	Called when the mouse has moved while on the secondary screen.
	\c dx and \c dy are the relative motion from the last position.
	*/
	virtual void		onMouseMoveSecondary(SInt32 dx, SInt32 dy) = 0;
	//! Notify of mouse wheen motion
	virtual void		onMouseWheel(SInt32 delta) = 0;
};

#endif
