/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2002 Chris Schoeneman
 * 
 * This package is free software you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file COPYING that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "common.h"
#include "CArch.h"

#undef ARCH_CONSOLE
#undef ARCH_DAEMON
#undef ARCH_FILE
#undef ARCH_LOG
#undef ARCH_MULTITHREAD
#undef ARCH_NETWORK
#undef ARCH_SLEEP
#undef ARCH_STRING
#undef ARCH_TIME

// include appropriate architecture implementation
#if WINDOWS_LIKE
#	include "CArchConsoleWindows.h"
#	include "CArchDaemonWindows.h"
#	include "CArchFileWindows.h"
#	include "CArchLogWindows.h"
#	include "CArchMultithreadWindows.h"
#	include "CArchNetworkWinsock.h"
#	include "CArchSleepWindows.h"
#	include "CArchStringWindows.h"
#	include "CArchTimeWindows.h"
#elif UNIX_LIKE
#	include "CArchConsoleUnix.h"
#	include "CArchDaemonUnix.h"
#	include "CArchFileUnix.h"
#	include "CArchLogUnix.h"
#	if HAVE_PTHREAD
#		include "CArchMultithreadPosix.h"
#	endif
#	include "CArchNetworkBSD.h"
#	include "CArchSleepUnix.h"
#	include "CArchStringUnix.h"
#	include "CArchTimeUnix.h"
#endif

#if !defined(ARCH_CONSOLE)
#	error unsupported platform for console
#endif

#if !defined(ARCH_DAEMON)
#	error unsupported platform for daemon
#endif

#if !defined(ARCH_FILE)
#	error unsupported platform for file
#endif

#if !defined(ARCH_LOG)
#	error unsupported platform for logging
#endif

#if !defined(ARCH_MULTITHREAD)
#	error unsupported platform for multithreading
#endif

#if !defined(ARCH_NETWORK)
#	error unsupported platform for network
#endif

#if !defined(ARCH_SLEEP)
#	error unsupported platform for sleep
#endif

#if !defined(ARCH_STRING)
#	error unsupported platform for string
#endif

#if !defined(ARCH_TIME)
#	error unsupported platform for time
#endif

//
// CArch
//

CArch*					CArch::s_instance = NULL;

CArch::CArch(ARCH_ARGS)
{
	// only once instance of CArch
	assert(s_instance == NULL);
	s_instance = this;

	// create architecture implementation objects
	m_mt      = new ARCH_MULTITHREAD;
	m_file    = new ARCH_FILE;
	m_log     = new ARCH_LOG;
	m_net     = new ARCH_NETWORK;
	m_sleep   = new ARCH_SLEEP;
	m_string  = new ARCH_STRING;
	m_time    = new ARCH_TIME;
	m_console = new ARCH_CONSOLE;
	m_daemon  = new ARCH_DAEMON;
}

CArch::~CArch()
{
	// clean up
	delete m_daemon;
	delete m_console;
	delete m_time;
	delete m_string;
	delete m_sleep;
	delete m_net;
	delete m_log;
	delete m_file;
	delete m_mt;

	// no instance
	s_instance = NULL;
}

CArch*
CArch::getInstance()
{
	return s_instance;
}

void
CArch::openConsole(const char* title)
{
	m_console->openConsole(title);
}

void
CArch::closeConsole()
{
	m_console->closeConsole();
}

void
CArch::writeConsole(const char* str)
{
	m_console->writeConsole(str);
}

const char*
CArch::getNewlineForConsole()
{
	return m_console->getNewlineForConsole();
}

void
CArch::installDaemon(const char* name,
				const char* description,
				const char* pathname,
				const char* commandLine,
				bool allUsers)
{
	m_daemon->installDaemon(name, description, pathname, commandLine, allUsers);
}

void
CArch::uninstallDaemon(const char* name, bool allUsers)
{
	m_daemon->uninstallDaemon(name, allUsers);
}

int
CArch::daemonize(const char* name, DaemonFunc func)
{
	return m_daemon->daemonize(name, func);
}

bool
CArch::canInstallDaemon(const char* name, bool allUsers)
{
	return m_daemon->canInstallDaemon(name, allUsers);
}

bool
CArch::isDaemonInstalled(const char* name, bool allUsers)
{
	return m_daemon->isDaemonInstalled(name, allUsers);
}

const char*
CArch::getBasename(const char* pathname)
{
	return m_file->getBasename(pathname);
}

std::string
CArch::getUserDirectory()
{
	return m_file->getUserDirectory();
}

std::string
CArch::getSystemDirectory()
{
	return m_file->getSystemDirectory();
}

std::string
CArch::concatPath(const std::string& prefix, const std::string& suffix)
{
	return m_file->concatPath(prefix, suffix);
}

void
CArch::openLog(const char* name)
{
	m_log->openLog(name);
}

void
CArch::closeLog()
{
	m_log->closeLog();
}

void
CArch::writeLog(ELevel level, const char* msg)
{
	m_log->writeLog(level, msg);
}

CArchCond
CArch::newCondVar()
{
	return m_mt->newCondVar();
}

void
CArch::closeCondVar(CArchCond cond)
{
	m_mt->closeCondVar(cond);
}

void
CArch::signalCondVar(CArchCond cond)
{
	m_mt->signalCondVar(cond);
}

void
CArch::broadcastCondVar(CArchCond cond)
{
	m_mt->broadcastCondVar(cond);
}

bool
CArch::waitCondVar(CArchCond cond, CArchMutex mutex, double timeout)
{
	return m_mt->waitCondVar(cond, mutex, timeout);
}

CArchMutex
CArch::newMutex()
{
	return m_mt->newMutex();
}

void
CArch::closeMutex(CArchMutex mutex)
{
	m_mt->closeMutex(mutex);
}

void
CArch::lockMutex(CArchMutex mutex)
{
	m_mt->lockMutex(mutex);
}

void
CArch::unlockMutex(CArchMutex mutex)
{
	m_mt->unlockMutex(mutex);
}

CArchThread
CArch::newThread(ThreadFunc func, void* data)
{
	return m_mt->newThread(func, data);
}

CArchThread
CArch::newCurrentThread()
{
	return m_mt->newCurrentThread();
}

CArchThread
CArch::copyThread(CArchThread thread)
{
	return m_mt->copyThread(thread);
}

void
CArch::closeThread(CArchThread thread)
{
	m_mt->closeThread(thread);
}

void
CArch::cancelThread(CArchThread thread)
{
	m_mt->cancelThread(thread);
}

void
CArch::setPriorityOfThread(CArchThread thread, int n)
{
	m_mt->setPriorityOfThread(thread, n);
}

void
CArch::testCancelThread()
{
	m_mt->testCancelThread();
}

bool
CArch::wait(CArchThread thread, double timeout)
{
	return m_mt->wait(thread, timeout);
}

bool
CArch::waitForEvent(double timeout)
{
	return m_mt->waitForEvent(timeout);
}

bool
CArch::isSameThread(CArchThread thread1, CArchThread thread2)
{
	return m_mt->isSameThread(thread1, thread2);
}

bool
CArch::isExitedThread(CArchThread thread)
{
	return m_mt->isExitedThread(thread);
}

void*
CArch::getResultOfThread(CArchThread thread)
{
	return m_mt->getResultOfThread(thread);
}

IArchMultithread::ThreadID
CArch::getIDOfThread(CArchThread thread)
{
	return m_mt->getIDOfThread(thread);
}

CArchSocket
CArch::newSocket(EAddressFamily family, ESocketType type)
{
	return m_net->newSocket(family, type);
}

CArchSocket
CArch::copySocket(CArchSocket s)
{
	return m_net->copySocket(s);
}

void
CArch::closeSocket(CArchSocket s)
{
	m_net->closeSocket(s);
}

void
CArch::closeSocketForRead(CArchSocket s)
{
	m_net->closeSocketForRead(s);
}

void
CArch::closeSocketForWrite(CArchSocket s)
{
	m_net->closeSocketForWrite(s);
}

void
CArch::bindSocket(CArchSocket s, CArchNetAddress addr)
{
	m_net->bindSocket(s, addr);
}

void
CArch::listenOnSocket(CArchSocket s)
{
	m_net->listenOnSocket(s);
}

CArchSocket
CArch::acceptSocket(CArchSocket s, CArchNetAddress* addr)
{
	return m_net->acceptSocket(s, addr);
}

void
CArch::connectSocket(CArchSocket s, CArchNetAddress name)
{
	m_net->connectSocket(s, name);
}

int
CArch::pollSocket(CPollEntry pe[], int num, double timeout)
{
	return m_net->pollSocket(pe, num, timeout);
}

size_t
CArch::readSocket(CArchSocket s, void* buf, size_t len)
{
	return m_net->readSocket(s, buf, len);
}

size_t
CArch::writeSocket(CArchSocket s, const void* buf, size_t len)
{
	return m_net->writeSocket(s, buf, len);
}

void
CArch::throwErrorOnSocket(CArchSocket s)
{
	m_net->throwErrorOnSocket(s);
}

bool
CArch::setBlockingOnSocket(CArchSocket s, bool blocking)
{
	return m_net->setBlockingOnSocket(s, blocking);
}

bool
CArch::setNoDelayOnSocket(CArchSocket s, bool noDelay)
{
	return m_net->setNoDelayOnSocket(s, noDelay);
}

std::string
CArch::getHostName()
{
	return m_net->getHostName();
}

CArchNetAddress
CArch::newAnyAddr(EAddressFamily family)
{
	return m_net->newAnyAddr(family);
}

CArchNetAddress
CArch::copyAddr(CArchNetAddress addr)
{
	return m_net->copyAddr(addr);
}

CArchNetAddress
CArch::nameToAddr(const std::string& name)
{
	return m_net->nameToAddr(name);
}

void
CArch::closeAddr(CArchNetAddress addr)
{
	m_net->closeAddr(addr);
}

std::string
CArch::addrToName(CArchNetAddress addr)
{
	return m_net->addrToName(addr);
}

std::string
CArch::addrToString(CArchNetAddress addr)
{
	return m_net->addrToString(addr);
}

IArchNetwork::EAddressFamily
CArch::getAddrFamily(CArchNetAddress addr)
{
	return m_net->getAddrFamily(addr);
}

void
CArch::setAddrPort(CArchNetAddress addr, int port)
{
	m_net->setAddrPort(addr, port);
}

int
CArch::getAddrPort(CArchNetAddress addr)
{
	return m_net->getAddrPort(addr);
}

bool
CArch::isAnyAddr(CArchNetAddress addr)
{
	return m_net->isAnyAddr(addr);
}

void
CArch::sleep(double timeout)
{
	m_sleep->sleep(timeout);
}

int
CArch::vsnprintf(char* str, int size, const char* fmt, va_list ap)
{
	return m_string->vsnprintf(str, size, fmt, ap);
}

CArchMBState
CArch::newMBState()
{
	return m_string->newMBState();
}

void
CArch::closeMBState(CArchMBState state)
{
	m_string->closeMBState(state);
}

void
CArch::initMBState(CArchMBState state)
{
	m_string->initMBState(state);
}

bool
CArch::isInitMBState(CArchMBState state)
{
	return m_string->isInitMBState(state);
}

int
CArch::convMBToWC(wchar_t* dst, const char* src, int n, CArchMBState state)
{
	return m_string->convMBToWC(dst, src, n, state);
}

int
CArch::convWCToMB(char* dst, wchar_t src, CArchMBState state)
{
	return m_string->convWCToMB(dst, src, state);
}

double
CArch::time()
{
	return m_time->time();
}
