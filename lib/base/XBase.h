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

#ifndef XBASE_H
#define XBASE_H

#include "CString.h"
#include "stdpre.h"
#include <exception>
#include "stdpost.h"

//! Exception base class
/*!
This is the base class of most exception types.
*/
class XBase : public std::exception {
public:
	//! Use getWhat() as the result of what()
	XBase();
	//! Use \c msg as the result of what()
	XBase(const CString& msg);
	virtual ~XBase();

	// std::exception overrides
	virtual const char*	what() const;

protected:
	//! Get a human readable string describing the exception
	virtual CString		getWhat() const throw() = 0;

	//! Format a string
	/*!
	Looks up a message format using \c id, using \c defaultFormat if
	no format can be found, then replaces positional parameters in
	the format string and returns the result.
	*/
	virtual CString		format(const char* id,
							const char* defaultFormat, ...) const throw();

private:
	mutable CString		m_what;
};

//! Mix-in for handling \c errno
/*!
This mix-in class for exception classes provides storage and query of
\c errno.
*/
class MXErrno {
public:
	//! Save \c errno as the error code
	MXErrno();
	//! Save \c err as the error code
	MXErrno(int err);

	//! @name accessors
	//@{

	//! Get the error code
	int					getErrno() const;

	//! Get the human readable string for the error code
	const char*			getErrstr() const;

	//@}

private:
	int					m_errno;
};

#endif
