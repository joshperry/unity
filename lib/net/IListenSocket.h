/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2002 Chris Schoeneman
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

#ifndef ILISTENSOCKET_H
#define ILISTENSOCKET_H

#include "ISocket.h"

class IDataSocket;

//! Listen socket interface
/*!
This interface defines the methods common to all network sockets that
listen for incoming connections.
*/
class IListenSocket : public ISocket {
public:
	//! @name manipulators
	//@{

	//! Accept connection
	/*!
	Accept a connection, returning a socket representing the full-duplex
	data stream.  Returns NULL if no socket is waiting to be accepted.
	This is only valid after a call to \c bind().
	*/
	virtual IDataSocket*	accept() = 0;

	//@}
	//! @name accessors
	//@{

	//! Get connecting event type
	/*!
	Returns the socket connecting event type.  A socket sends this
	event when a remote connection is waiting to be accepted.
	*/
	static CEvent::Type	getConnectingEvent();

	//@}

	// ISocket overrides
	virtual void		bind(const CNetworkAddress&) = 0;
	virtual void		close() = 0;
	virtual void*		getEventTarget() const = 0;

private:
	static CEvent::Type	s_connectingEvent;
};

#endif
