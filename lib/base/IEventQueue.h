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

#ifndef IEVENTQUEUE_H
#define IEVENTQUEUE_H

#include "IInterface.h"
#include "CEvent.h"

#define EVENTQUEUE IEventQueue::getInstance()

class IEventJob;

// Opaque type for timer info.  This is defined by subclasses of
// IEventQueue.
class CEventQueueTimer;

//! Event queue interface
/*!
An event queue provides a queue of CEvents.  Clients can block waiting
on any event becoming available at the head of the queue and can place
new events at the end of the queue.  Clients can also add and remove
timers which generate events periodically.
*/
class IEventQueue : public IInterface {
public:
	class CTimerEvent {
	public:
		CEventQueueTimer*	m_timer;	//!< The timer
		UInt32				m_count;	//!< Number of repeats
	};

	//! @name manipulators
	//@{

	//! Remove event from queue
	/*!
	Returns the next event on the queue into \p event.  If no event is
	available then blocks for up to \p timeout seconds, or forever if
	\p timeout is negative.  Returns true iff an event was available.
	*/
	virtual bool		getEvent(CEvent& event, double timeout = -1.0) = 0;

	//! Dispatch an event
	/*!
	Looks up the dispatcher for the event's target and invokes it.
	Returns true iff a dispatcher exists for the target.
	*/
	virtual bool		dispatchEvent(const CEvent& event) = 0;

	//! Add event to queue
	/*!
	Adds \p event to the end of the queue.
	*/
	virtual void		addEvent(const CEvent& event) = 0;

	//! Create a recurring timer
	/*!
	Creates and returns a timer.  An event is returned after \p duration
	seconds and the timer is reset to countdown again.  When a timer event
	is returned the data points to a \c CTimerEvent.  The client must pass
	the returned timer to \c deleteTimer() (whether or not the timer has
	expired) to release the timer.  The returned timer event uses the
	given \p target.

	Events for a single timer don't accumulate in the queue, even if the
	client reading events can't keep up.  Instead, the \c m_count member
	of the \c CTimerEvent indicates how many events for the timer would
	have been put on the queue since the last event for the timer was
	removed (or since the timer was added).
	*/
	virtual CEventQueueTimer*
						newTimer(double duration, void* target = NULL) = 0;

	//! Create a one-shot timer
	/*!
	Creates and returns a one-shot timer.  An event is returned when
	the timer expires and the timer is removed from further handling.
	When a timer event is returned the data points to a \c CTimerEvent.
	The \m c_count member of the \c CTimerEvent is always 1.  The client
	must pass the returned timer to \c deleteTimer() (whether or not the
	timer has expired) to release the timer.  The returned timer event
	uses the given \p target.
	*/
	virtual CEventQueueTimer*
						newOneShotTimer(double duration,
							void* target = NULL) = 0;

	//! Destroy a timer
	/*!
	Destroys a previously created timer.  The timer is removed from the
	queue and will not generate event, even if the timer has expired.
	*/
	virtual void		deleteTimer(CEventQueueTimer*) = 0;

	//! Register an event handler
	/*!
	Registers an event handler for \p target.  The \p handler is
	adopted.  Any existing handler for the target is deleted.
	\c dispatchEvent() will invoke \p handler for any event for
	\p target that doesn't have a type specific handler.
	*/
	virtual void		adoptHandler(void* target, IEventJob* handler) = 0;

	//! Register an event handler for an event type
	/*!
	Registers an event handler for \p type and \p target.  The \p handler
	is adopted.  Any existing handler for the type,target pair is deleted.
	\c dispatchEvent() will invoke \p handler for any event for \p target
	of type \p type.
	*/
	virtual void		adoptHandler(CEvent::Type type,
							void* target, IEventJob* handler) = 0;

	//! Unregister an event handler
	/*!
	Unregisters an event handler for \p target and returns it.
	Returns NULL if there was no such handler.  The client becomes
	responsible for deleting the returned handler.
	*/
	virtual IEventJob*	orphanHandler(void* target) = 0;

	//! Unregister an event handler for an event type
	/*!
	Unregisters an event handler for the \p type, \p target pair and
	returns it.  Returns NULL if there was no such handler.  The
	client becomes responsible for deleting the returned handler.
	*/
	virtual IEventJob*	orphanHandler(CEvent::Type type, void* target) = 0;

	//! Unregister an event handler
	/*!
	Unregisters an event handler for \p target and deletes it.
	*/
	virtual void		removeHandler(void* target) = 0;

	//! Unregister an event handler for an event type
	/*!
	Unregisters an event handler for the \p type, \p target pair and
	deletes it.
	*/
	virtual void		removeHandler(CEvent::Type type, void* target) = 0;

	//@}
	//! @name accessors
	//@{

	//! Test if queue is empty
	/*!
	Returns true iff the queue has no events in it, including timer
	events.
	*/
	virtual bool		isEmpty() const = 0;

	//! Get an event handler
	/*!
	Finds and returns the event handler for the \p type, \p target pair.
	If there is no such handler, returns the handler for \p target.  If
	that doesn't exist, returns NULL.
	*/
	virtual IEventJob*	getHandler(CEvent::Type type, void* target) const = 0;

	//! Get the system event type target
	/*!
	Returns the target to use for dispatching \c CEvent::kSystem events.
	*/
	static void*		getSystemTarget();

	//! Get the singleton instance
	/*!
	Returns the singleton instance of the event queue
	*/
	static IEventQueue*	getInstance();

	//@}

protected:
	//! @name manipulators
	//@{

	//! Set the singleton instance
	/*!
	Sets the singleton instance of the event queue
	*/
	static void			setInstance(IEventQueue*);

	//@}

private:
	static IEventQueue*	s_instance;
};

#endif
