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

#include "CServer.h"
#include "CClientProxy.h"
#include "CClientProxyUnknown.h"
#include "CPrimaryClient.h"
#include "IPlatformScreen.h"
#include "OptionTypes.h"
#include "ProtocolTypes.h"
#include "XScreen.h"
#include "XSynergy.h"
#include "IDataSocket.h"
#include "IListenSocket.h"
#include "XSocket.h"
#include "IEventQueue.h"
#include "CLog.h"
#include "TMethodEventJob.h"
#include "CArch.h"

//
// CServer
//

CEvent::Type			CServer::s_errorEvent        = CEvent::kUnknown;
CEvent::Type			CServer::s_disconnectedEvent = CEvent::kUnknown;

CServer::CServer(const CConfig& config, CPrimaryClient* primaryClient) :
	m_primaryClient(primaryClient),
	m_active(primaryClient),
	m_seqNum(0),
	m_config(config),
	m_activeSaver(NULL),
	m_switchDir(kNoDirection),
	m_switchScreen(NULL),
	m_switchWaitDelay(0.0),
	m_switchWaitTimer(NULL),
	m_switchTwoTapDelay(0.0),
	m_switchTwoTapEngaged(false),
	m_switchTwoTapArmed(false),
	m_switchTwoTapZone(3)
{
	// must have a primary client and it must have a canonical name
	assert(m_primaryClient != NULL);
	assert(m_config.isScreen(primaryClient->getName()));

	CString primaryName = getName(primaryClient);

	// clear clipboards
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		CClipboardInfo& clipboard   = m_clipboards[id];
		clipboard.m_clipboardOwner  = primaryName;
		clipboard.m_clipboardSeqNum = m_seqNum;
		if (clipboard.m_clipboard.open(0)) {
			clipboard.m_clipboard.empty();
			clipboard.m_clipboard.close();
		}
		clipboard.m_clipboardData   = clipboard.m_clipboard.marshall();
	}

	// install event handlers
	EVENTQUEUE->adoptHandler(CEvent::kTimer, this,
							new TMethodEventJob<CServer>(this,
								&CServer::handleSwitchWaitTimeout));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getKeyDownEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleKeyDownEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getKeyUpEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleKeyUpEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getKeyRepeatEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleKeyRepeatEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getButtonDownEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleButtonDownEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getButtonUpEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleButtonUpEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getMotionOnPrimaryEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleMotionPrimaryEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getMotionOnSecondaryEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleMotionSecondaryEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getWheelEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleWheelEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getScreensaverActivatedEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleScreensaverActivatedEvent));
	EVENTQUEUE->adoptHandler(IPlatformScreen::getScreensaverDeactivatedEvent(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleScreensaverDeactivatedEvent));

	// add connection
	addClient(m_primaryClient);

	// tell primary client about its options
	sendOptions(m_primaryClient);

	m_primaryClient->enable();
}

CServer::~CServer()
{
	// remove event handlers and timers
	EVENTQUEUE->removeHandler(IPlatformScreen::getKeyDownEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getKeyUpEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getKeyRepeatEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getButtonDownEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getButtonUpEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getMotionOnPrimaryEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getMotionOnSecondaryEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getWheelEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getScreensaverActivatedEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(IPlatformScreen::getScreensaverDeactivatedEvent(),
							m_primaryClient->getEventTarget());
	EVENTQUEUE->removeHandler(CEvent::kTimer, this);
	stopSwitch();

	// force immediate disconnection of secondary clients
	disconnect();
	for (COldClients::iterator index = m_oldClients.begin();
							index != m_oldClients.begin(); ++index) {
		IClient* client = index->first;
		EVENTQUEUE->deleteTimer(index->second);
		EVENTQUEUE->removeHandler(CEvent::kTimer, client);
		EVENTQUEUE->removeHandler(CClientProxy::getDisconnectedEvent(), client);
		delete client;
	}

	// disconnect primary client
	removeClient(m_primaryClient);
}

bool
CServer::setConfig(const CConfig& config)
{
	// refuse configuration if it doesn't include the primary screen
	if (!config.isScreen(m_primaryClient->getName())) {
		return false;
	}

	// close clients that are connected but being dropped from the
	// configuration.
	closeClients(config);

	// cut over
	m_config = config;
	processOptions();

	// tell primary screen about reconfiguration
	m_primaryClient->reconfigure(getActivePrimarySides());

	// tell all (connected) clients about current options
	for (CClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		IClient* client = index->second;
		sendOptions(client);
	}

	return true;
}

void
CServer::adoptClient(IClient* client)
{
	assert(client != NULL);

	// watch for client disconnection
	EVENTQUEUE->adoptHandler(CClientProxy::getDisconnectedEvent(), client,
							new TMethodEventJob<CServer>(this,
								&CServer::handleClientDisconnected, client));

	// name must be in our configuration
	if (!m_config.isScreen(client->getName())) {
		LOG((CLOG_WARN "a client with name \"%s\" is not in the map", client->getName().c_str()));
		closeClient(client, kMsgEUnknown);
		return;
	}

	// add client to client list
	if (!addClient(client)) {
		// can only have one screen with a given name at any given time
		LOG((CLOG_WARN "a client with name \"%s\" is already connected", getName(client).c_str()));
		closeClient(client, kMsgEBusy);
		return;
	}
	LOG((CLOG_NOTE "client \"%s\" has connected", getName(client).c_str()));

	// send configuration options to client
	sendOptions(client);

	// activate screen saver on new client if active on the primary screen
	if (m_activeSaver != NULL) {
		client->screensaver(true);
	}
}

void
CServer::disconnect()
{
	// close all secondary clients
	if (m_clients.size() > 1 || !m_oldClients.empty()) {
		CConfig emptyConfig;
		closeClients(emptyConfig);
	}
	else {
		EVENTQUEUE->addEvent(CEvent(getDisconnectedEvent(), this));
	}
}

UInt32
CServer::getNumClients() const
{
	return m_clients.size();
}

void
CServer::getClients(std::vector<CString>& list) const
{
	list.clear();
	for (CClientList::const_iterator index = m_clients.begin();
							index != m_clients.end(); ++index) {
		list.push_back(index->first);
	}
}

CEvent::Type
CServer::getErrorEvent()
{
	return CEvent::registerTypeOnce(s_errorEvent,
							"CServer::error");
}

CEvent::Type
CServer::getDisconnectedEvent()
{
	return CEvent::registerTypeOnce(s_disconnectedEvent,
							"CServer::disconnected");
}

bool
CServer::onCommandKey(KeyID id, KeyModifierMask /*mask*/, bool /*down*/)
{
	if (id == kKeyScrollLock) {
		m_primaryClient->reconfigure(getActivePrimarySides());
	}
	return false;
}

CString
CServer::getName(const IClient* client) const
{
	CString name = m_config.getCanonicalName(client->getName());
	if (name.empty()) {
		name = client->getName();
	}
	return name;
}

UInt32
CServer::getActivePrimarySides() const
{
	UInt32 sides = 0;
	if (!isLockedToScreen()) {
		if (getNeighbor(m_primaryClient, kLeft) != NULL) {
			sides |= kLeftMask;
		}
		if (getNeighbor(m_primaryClient, kRight) != NULL) {
			sides |= kRightMask;
		}
		if (getNeighbor(m_primaryClient, kTop) != NULL) {
			sides |= kTopMask;
		}
		if (getNeighbor(m_primaryClient, kBottom) != NULL) {
			sides |= kBottomMask;
		}
	}
	return sides;
}

bool
CServer::isLockedToScreen() const
{
	// locked if scroll-lock is toggled on
	if ((m_primaryClient->getToggleMask() & KeyModifierScrollLock) != 0) {
		LOG((CLOG_DEBUG "locked by ScrollLock"));
		return true;
	}

	// locked if primary says we're locked
	if (m_primaryClient->isLockedToScreen()) {
		return true;
	}

	// not locked
	return false;
}

SInt32
CServer::getJumpZoneSize(IClient* client) const
{
	if (client == m_primaryClient) {
		return m_primaryClient->getJumpZoneSize();
	}
	else {
		return 0;
	}
}

void
CServer::switchScreen(IClient* dst, SInt32 x, SInt32 y, bool forScreensaver)
{
	assert(dst != NULL);
#ifndef NDEBUG
	{
		SInt32 dx, dy, dw, dh;
		dst->getShape(dx, dy, dw, dh);
		assert(x >= dx && y >= dy && x < dx + dw && y < dy + dh);
	}
#endif
	assert(m_active != NULL);

	LOG((CLOG_INFO "switch from \"%s\" to \"%s\" at %d,%d", getName(m_active).c_str(), getName(dst).c_str(), x, y));

	// stop waiting to switch
	stopSwitch();

	// record new position
	m_x = x;
	m_y = y;

	// wrapping means leaving the active screen and entering it again.
	// since that's a waste of time we skip that and just warp the
	// mouse.
	if (m_active != dst) {
		// leave active screen
		if (!m_active->leave()) {
			// cannot leave screen
			LOG((CLOG_WARN "can't leave screen"));
			return;
		}

		// update the primary client's clipboards if we're leaving the
		// primary screen.
		if (m_active == m_primaryClient) {
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				CClipboardInfo& clipboard = m_clipboards[id];
				if (clipboard.m_clipboardOwner == getName(m_primaryClient)) {
					onClipboardChanged(m_primaryClient,
						id, clipboard.m_clipboardSeqNum);
				}
			}
		}

		// cut over
		m_active = dst;

		// increment enter sequence number
		++m_seqNum;

		// enter new screen
		m_active->enter(x, y, m_seqNum,
								m_primaryClient->getToggleMask(),
								forScreensaver);

		// send the clipboard data to new active screen
		for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
			m_active->setClipboard(id, &m_clipboards[id].m_clipboard);
		}
	}
	else {
		m_active->mouseMove(x, y);
	}
}

IClient*
CServer::getNeighbor(IClient* src, EDirection dir) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	// get source screen name
	CString srcName = getName(src);
	assert(!srcName.empty());
	LOG((CLOG_DEBUG2 "find neighbor on %s of \"%s\"", CConfig::dirName(dir), srcName.c_str()));

	// get first neighbor.  if it's the source then the source jumps
	// to itself and we return the source.
	CString dstName(m_config.getNeighbor(srcName, dir));
	if (dstName == srcName) {
		LOG((CLOG_DEBUG2 "\"%s\" is on %s of \"%s\"", dstName.c_str(), CConfig::dirName(dir), srcName.c_str()));
		return src;
	}

	// keep checking
	for (;;) {
		// if nothing in that direction then return NULL. if the
		// destination is the source then we can make no more
		// progress in this direction.  since we haven't found a
		// connected neighbor we return NULL.
		if (dstName.empty() || dstName == srcName) {
			LOG((CLOG_DEBUG2 "no neighbor on %s of \"%s\"", CConfig::dirName(dir), srcName.c_str()));
			return NULL;
		}

		// look up neighbor cell.  if the screen is connected and
		// ready then we can stop.
		CClientList::const_iterator index = m_clients.find(dstName);
		if (index != m_clients.end()) {
			LOG((CLOG_DEBUG2 "\"%s\" is on %s of \"%s\"", dstName.c_str(), CConfig::dirName(dir), srcName.c_str()));
			return index->second;
		}

		// skip over unconnected screen
		LOG((CLOG_DEBUG2 "ignored \"%s\" on %s of \"%s\"", dstName.c_str(), CConfig::dirName(dir), srcName.c_str()));
		srcName = dstName;

		// look up name of neighbor of skipped screen
		dstName = m_config.getNeighbor(srcName, dir);
	}
}

IClient*
CServer::getNeighbor(IClient* src,
				EDirection srcSide, SInt32& x, SInt32& y) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	// get the first neighbor
	IClient* dst = getNeighbor(src, srcSide);
	if (dst == NULL) {
		return NULL;
	}

	// get the source screen's size (needed for kRight and kBottom)
	SInt32 sx, sy, sw, sh;
	SInt32 dx, dy, dw, dh;
	IClient* lastGoodScreen = src;
	lastGoodScreen->getShape(sx, sy, sw, sh);
	lastGoodScreen->getShape(dx, dy, dw, dh);

	// find destination screen, adjusting x or y (but not both).  the
	// searches are done in a sort of canonical screen space where
	// the upper-left corner is 0,0 for each screen.  we adjust from
	// actual to canonical position on entry to and from canonical to
	// actual on exit from the search.
	switch (srcSide) {
	case kLeft:
		x -= dx;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			x += dw;
			if (x >= 0) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		break;

	case kRight:
		x -= dx;
		while (dst != NULL) {
			x -= dw;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (x < dw) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		break;

	case kTop:
		y -= dy;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			y += dh;
			if (y >= 0) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		break;

	case kBottom:
		y -= dy;
		while (dst != NULL) {
			y -= dh;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (y < sh) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
		return NULL;
	}

	// save destination screen
	assert(lastGoodScreen != NULL);
	dst = lastGoodScreen;

	// if entering primary screen then be sure to move in far enough
	// to avoid the jump zone.  if entering a side that doesn't have
	// a neighbor (i.e. an asymmetrical side) then we don't need to
	// move inwards because that side can't provoke a jump.
	if (dst == m_primaryClient) {
		const CString dstName(getName(dst));
		switch (srcSide) {
		case kLeft:
			if (!m_config.getNeighbor(dstName, kRight).empty() &&
				x > dx + dw - 1 - getJumpZoneSize(dst))
				x = dx + dw - 1 - getJumpZoneSize(dst);
			break;

		case kRight:
			if (!m_config.getNeighbor(dstName, kLeft).empty() &&
				x < dx + getJumpZoneSize(dst))
				x = dx + getJumpZoneSize(dst);
			break;

		case kTop:
			if (!m_config.getNeighbor(dstName, kBottom).empty() &&
				y > dy + dh - 1 - getJumpZoneSize(dst))
				y = dy + dh - 1 - getJumpZoneSize(dst);
			break;

		case kBottom:
			if (!m_config.getNeighbor(dstName, kTop).empty() &&
				y < dy + getJumpZoneSize(dst))
				y = dy + getJumpZoneSize(dst);
			break;

		case kNoDirection:
			assert(0 && "bad direction");
			return NULL;
		}
	}

	// adjust the coordinate orthogonal to srcSide to account for
	// resolution differences.  for example, if y is 200 pixels from
	// the top on a screen 1000 pixels high (20% from the top) when
	// we cross the left edge onto a screen 600 pixels high then y
	// should be set 120 pixels from the top (again 20% from the
	// top).
	switch (srcSide) {
	case kLeft:
	case kRight:
		y -= sy;
		if (y < 0) {
			y = 0;
		}
		else if (y >= sh) {
			y = dh - 1;
		}
		else {
			y = static_cast<SInt32>(0.5 + y *
								static_cast<double>(dh - 1) / (sh - 1));
		}
		y += dy;
		break;

	case kTop:
	case kBottom:
		x -= sx;
		if (x < 0) {
			x = 0;
		}
		else if (x >= sw) {
			x = dw - 1;
		}
		else {
			x = static_cast<SInt32>(0.5 + x *
								static_cast<double>(dw - 1) / (sw - 1));
		}
		x += dx;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
		return NULL;
	}

	return dst;
}

bool
CServer::isSwitchOkay(IClient* newScreen, EDirection dir, SInt32 x, SInt32 y)
{
	LOG((CLOG_DEBUG1 "try to leave \"%s\" on %s", getName(m_active).c_str(), CConfig::dirName(dir)));

	// is there a neighbor?
	if (newScreen == NULL) {
		// there's no neighbor.  we don't want to switch and we don't
		// want to try to switch later.
		LOG((CLOG_DEBUG1 "no neighbor %s", CConfig::dirName(dir)));
		stopSwitch();
		return false;
	}

	// should we switch or not?
	bool preventSwitch = false;
	bool allowSwitch   = false;

	// note if the switch direction has changed.  save the new
	// direction and screen if so.
	bool isNewDirection  = (dir != m_switchDir);
	if (isNewDirection || m_switchScreen == NULL) {
		m_switchDir    = dir;
		m_switchScreen = newScreen;
	}

	// is this a double tap and do we care?
	if (!allowSwitch && m_switchTwoTapDelay > 0.0) {
		if (isNewDirection ||
			!isSwitchTwoTapStarted() || !shouldSwitchTwoTap()) {
			// tapping a different or new edge or second tap not
			// fast enough.  prepare for second tap.
			preventSwitch = true;
			startSwitchTwoTap();
		}
		else {
			// got second tap
			allowSwitch = true;
		}
	}

	// if waiting before a switch then prepare to switch later
	if (!allowSwitch && m_switchWaitDelay > 0.0) {
		if (isNewDirection || !isSwitchWaitStarted()) {
			startSwitchWait(x, y);
		}
		preventSwitch = true;
	}

	// ignore if mouse is locked to screen and don't try to switch later
	if (!preventSwitch && isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		preventSwitch = true;
		stopSwitch();
	}

	return !preventSwitch;
}

void
CServer::noSwitch(SInt32 x, SInt32 y)
{
	armSwitchTwoTap(x, y);
	stopSwitchWait();
}

void
CServer::stopSwitch()
{
	if (m_switchScreen != NULL) {
		m_switchScreen = NULL;
		m_switchDir    = kNoDirection;
		stopSwitchTwoTap();
		stopSwitchWait();
	}
}

void
CServer::startSwitchTwoTap()
{
	m_switchTwoTapEngaged = true;
	m_switchTwoTapArmed   = false;
	m_switchTwoTapTimer.reset();
	LOG((CLOG_DEBUG1 "waiting for second tap"));
}

void
CServer::armSwitchTwoTap(SInt32 x, SInt32 y)
{
	if (m_switchTwoTapEngaged) {
		if (m_switchTwoTapTimer.getTime() > m_switchTwoTapDelay) {
			// second tap took too long.  disengage.
			stopSwitchTwoTap();
		}
		else if (!m_switchTwoTapArmed) {
			// still time for a double tap.  see if we left the tap
			// zone and, if so, arm the two tap.
			SInt32 ax, ay, aw, ah;
			m_active->getShape(ax, ay, aw, ah);
			SInt32 tapZone = m_primaryClient->getJumpZoneSize();
			if (tapZone < m_switchTwoTapZone) {
				tapZone = m_switchTwoTapZone;
			}
			if (x >= ax + tapZone && x < ax + aw - tapZone &&
				y >= ay + tapZone && y < ay + ah - tapZone) {
				m_switchTwoTapArmed = true;
			}
		}
	}
}

void
CServer::stopSwitchTwoTap()
{
	m_switchTwoTapEngaged = false;
	m_switchTwoTapArmed   = false;
}

bool
CServer::isSwitchTwoTapStarted() const
{
	return m_switchTwoTapEngaged;
}

bool
CServer::shouldSwitchTwoTap() const
{
	// this is the second tap if two-tap is armed and this tap
	// came fast enough
	return (m_switchTwoTapArmed &&
			m_switchTwoTapTimer.getTime() <= m_switchTwoTapDelay);
}

void
CServer::startSwitchWait(SInt32 x, SInt32 y)
{
	stopSwitchWait();
	m_switchWaitX     = x;
	m_switchWaitY     = y;
	m_switchWaitTimer = EVENTQUEUE->newOneShotTimer(m_switchWaitDelay, this);
	LOG((CLOG_DEBUG1 "waiting to switch"));
}

void
CServer::stopSwitchWait()
{
	if (m_switchWaitTimer != NULL) {
		EVENTQUEUE->deleteTimer(m_switchWaitTimer);
		m_switchWaitTimer = NULL;
	}
}

bool
CServer::isSwitchWaitStarted() const
{
	return (m_switchWaitTimer != NULL);
}

void
CServer::sendOptions(IClient* client) const
{
	COptionsList optionsList;

	// look up options for client
	const CConfig::CScreenOptions* options =
						m_config.getOptions(getName(client));
	if (options != NULL) {
		// convert options to a more convenient form for sending
		optionsList.reserve(2 * options->size());
		for (CConfig::CScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			optionsList.push_back(index->first);
			optionsList.push_back(static_cast<UInt32>(index->second));
		}
	}

	// look up global options
	options = m_config.getOptions("");
	if (options != NULL) {
		// convert options to a more convenient form for sending
		optionsList.reserve(optionsList.size() + 2 * options->size());
		for (CConfig::CScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			optionsList.push_back(index->first);
			optionsList.push_back(static_cast<UInt32>(index->second));
		}
	}

	// send the options
	client->setOptions(optionsList);
}

void
CServer::processOptions()
{
	const CConfig::CScreenOptions* options = m_config.getOptions("");
	if (options == NULL) {
		return;
	}

	for (CConfig::CScreenOptions::const_iterator index = options->begin();
								index != options->end(); ++index) {
		const OptionID id       = index->first;
		const OptionValue value = index->second;
		if (id == kOptionScreenSwitchDelay) {
			m_switchWaitDelay = 1.0e-3 * static_cast<double>(value);
			if (m_switchWaitDelay < 0.0) {
				m_switchWaitDelay = 0.0;
			}
			stopSwitchWait();
		}
		else if (id == kOptionScreenSwitchTwoTap) {
			m_switchTwoTapDelay = 1.0e-3 * static_cast<double>(value);
			if (m_switchTwoTapDelay < 0.0) {
				m_switchTwoTapDelay = 0.0;
			}
			stopSwitchTwoTap();
		}
	}
}

void
CServer::handleShapeChanged(const CEvent&, void* vclient)
{
	// ignore events from unknown clients
	IClient* client = reinterpret_cast<IClient*>(vclient);
	if (m_clientSet.count(client) == 0) {
		return;
	}

	// update the mouse coordinates
	if (client == m_active) {
		client->getCursorPos(m_x, m_y);
	}
	LOG((CLOG_INFO "screen \"%s\" shape changed", getName(client).c_str()));

	// handle resolution change to primary screen
	if (client == m_primaryClient) {
		if (client == m_active) {
			onMouseMovePrimary(m_x, m_y);
		}
		else {
			onMouseMoveSecondary(0, 0);
		}
	}
}

void
CServer::handleClipboardGrabbed(const CEvent& event, void* vclient)
{
	// ignore events from unknown clients
	IClient* grabber = reinterpret_cast<IClient*>(vclient);
	if (m_clientSet.count(grabber) == 0) {
		return;
	}
	const IScreen::CClipboardInfo* info =
		reinterpret_cast<const IScreen::CClipboardInfo*>(event.getData());

	// ignore grab if sequence number is old.  always allow primary
	// screen to grab.
	CClipboardInfo& clipboard = m_clipboards[info->m_id];
	if (grabber != m_primaryClient &&
		info->m_sequenceNumber < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" grab of clipboard %d", getName(grabber).c_str(), info->m_id));
		return;
	}

	// mark screen as owning clipboard
	LOG((CLOG_INFO "screen \"%s\" grabbed clipboard %d from \"%s\"", getName(grabber).c_str(), info->m_id, clipboard.m_clipboardOwner.c_str()));
	clipboard.m_clipboardOwner  = getName(grabber);
	clipboard.m_clipboardSeqNum = info->m_sequenceNumber;

	// clear the clipboard data (since it's not known at this point)
	if (clipboard.m_clipboard.open(0)) {
		clipboard.m_clipboard.empty();
		clipboard.m_clipboard.close();
	}
	clipboard.m_clipboardData = clipboard.m_clipboard.marshall();

	// tell all other screens to take ownership of clipboard.  tell the
	// grabber that it's clipboard isn't dirty.
	for (CClientList::iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		IClient* client = index->second;
		if (client == grabber) {
			client->setClipboardDirty(info->m_id, false);
		}
		else {
			client->grabClipboard(info->m_id);
		}
	}
}

void
CServer::handleClipboardChanged(const CEvent& event, void* vclient)
{
	// ignore events from unknown clients
	IClient* sender = reinterpret_cast<IClient*>(vclient);
	if (m_clientSet.count(sender) == 0) {
		return;
	}
	const IScreen::CClipboardInfo* info =
		reinterpret_cast<const IScreen::CClipboardInfo*>(event.getData());
	onClipboardChanged(sender, info->m_id, info->m_sequenceNumber);
}

void
CServer::handleKeyDownEvent(const CEvent& event, void*)
{
	IPlatformScreen::CKeyInfo* info =
		reinterpret_cast<IPlatformScreen::CKeyInfo*>(event.getData());
	onKeyDown(info->m_key, info->m_mask, info->m_button);
}

void
CServer::handleKeyUpEvent(const CEvent& event, void*)
{
	IPlatformScreen::CKeyInfo* info =
		 reinterpret_cast<IPlatformScreen::CKeyInfo*>(event.getData());
	onKeyUp(info->m_key, info->m_mask, info->m_button);
}

void
CServer::handleKeyRepeatEvent(const CEvent& event, void*)
{
	IPlatformScreen::CKeyInfo* info =
		reinterpret_cast<IPlatformScreen::CKeyInfo*>(event.getData());
	onKeyRepeat(info->m_key, info->m_mask, info->m_count, info->m_button);
}

void
CServer::handleButtonDownEvent(const CEvent& event, void*)
{
	IPlatformScreen::CButtonInfo* info =
		reinterpret_cast<IPlatformScreen::CButtonInfo*>(event.getData());
	onMouseDown(info->m_button);
}

void
CServer::handleButtonUpEvent(const CEvent& event, void*)
{
	IPlatformScreen::CButtonInfo* info =
		reinterpret_cast<IPlatformScreen::CButtonInfo*>(event.getData());
	onMouseUp(info->m_button);
}

void
CServer::handleMotionPrimaryEvent(const CEvent& event, void*)
{
	IPlatformScreen::CMotionInfo* info =
		reinterpret_cast<IPlatformScreen::CMotionInfo*>(event.getData());
	onMouseMovePrimary(info->m_x, info->m_y);
}

void
CServer::handleMotionSecondaryEvent(const CEvent& event, void*)
{
	IPlatformScreen::CMotionInfo* info =
		reinterpret_cast<IPlatformScreen::CMotionInfo*>(event.getData());
	onMouseMoveSecondary(info->m_x, info->m_y);
}

void
CServer::handleWheelEvent(const CEvent& event, void*)
{
	IPlatformScreen::CWheelInfo* info =
		reinterpret_cast<IPlatformScreen::CWheelInfo*>(event.getData());
	onMouseWheel(info->m_wheel);
}

void
CServer::handleScreensaverActivatedEvent(const CEvent&, void*)
{
	onScreensaver(true);
}

void
CServer::handleScreensaverDeactivatedEvent(const CEvent&, void*)
{
	onScreensaver(false);
}

void
CServer::handleSwitchWaitTimeout(const CEvent&, void*)
{
	// ignore if mouse is locked to screen
	if (isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		stopSwitch();
		return;
	}

	// switch screen
	switchScreen(m_switchScreen, m_switchWaitX, m_switchWaitY, false);
}

void
CServer::handleClientDisconnected(const CEvent&, void* vclient)
{
	// client has disconnected.  it might be an old client or an
	// active client.  we don't care so just handle it both ways.
	IClient* client = reinterpret_cast<IClient*>(vclient);
	removeActiveClient(client);
	removeOldClient(client);
	delete client;
}

void
CServer::handleClientCloseTimeout(const CEvent&, void* vclient)
{
	// client took too long to disconnect.  just dump it.
	IClient* client = reinterpret_cast<IClient*>(vclient);
	LOG((CLOG_NOTE "forced disconnection of client \"%s\"", getName(client).c_str()));
	removeOldClient(client);
	delete client;
}

void
CServer::onClipboardChanged(IClient* sender, ClipboardID id, UInt32 seqNum)
{
	CClipboardInfo& clipboard = m_clipboards[id];

	// ignore update if sequence number is old
	if (seqNum < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" update of clipboard %d (missequenced)", clipboard.m_clipboardOwner.c_str(), id));
		return;
	}

	// should be the expected client
	assert(sender == m_clients.find(clipboard.m_clipboardOwner)->second);

	// get data
	sender->getClipboard(id, &clipboard.m_clipboard);

	// ignore if data hasn't changed
	CString data = clipboard.m_clipboard.marshall();
	if (data == clipboard.m_clipboardData) {
		LOG((CLOG_DEBUG "ignored screen \"%s\" update of clipboard %d (unchanged)", clipboard.m_clipboardOwner.c_str(), id));
		return;
	}

	// got new data
	LOG((CLOG_INFO "screen \"%s\" updated clipboard %d", clipboard.m_clipboardOwner.c_str(), id));
	clipboard.m_clipboardData = data;

	// tell all clients except the sender that the clipboard is dirty
	for (CClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		IClient* client = index->second;
		client->setClipboardDirty(id, client != sender);
	}

	// send the new clipboard to the active screen
	m_active->setClipboard(id, &clipboard.m_clipboard);
}

void
CServer::onScreensaver(bool activated)
{
	LOG((CLOG_DEBUG "onScreenSaver %s", activated ? "activated" : "deactivated"));

	if (activated) {
		// save current screen and position
		m_activeSaver = m_active;
		m_xSaver      = m_x;
		m_ySaver      = m_y;

		// jump to primary screen
		if (m_active != m_primaryClient) {
			switchScreen(m_primaryClient, 0, 0, true);
		}
	}
	else {
		// jump back to previous screen and position.  we must check
		// that the position is still valid since the screen may have
		// changed resolutions while the screen saver was running.
		if (m_activeSaver != NULL && m_activeSaver != m_primaryClient) {
			// check position
			IClient* screen = m_activeSaver;
			SInt32 x, y, w, h;
			screen->getShape(x, y, w, h);
			SInt32 zoneSize = getJumpZoneSize(screen);
			if (m_xSaver < x + zoneSize) {
				m_xSaver = x + zoneSize;
			}
			else if (m_xSaver >= x + w - zoneSize) {
				m_xSaver = x + w - zoneSize - 1;
			}
			if (m_ySaver < y + zoneSize) {
				m_ySaver = y + zoneSize;
			}
			else if (m_ySaver >= y + h - zoneSize) {
				m_ySaver = y + h - zoneSize - 1;
			}

			// jump
			switchScreen(screen, m_xSaver, m_ySaver, false);
		}

		// reset state
		m_activeSaver = NULL;
	}

	// send message to all clients
	for (CClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		IClient* client = index->second;
		client->screensaver(activated);
	}
}

void
CServer::onKeyDown(KeyID id, KeyModifierMask mask, KeyButton button)
{
	LOG((CLOG_DEBUG1 "onKeyDown id=%d mask=0x%04x button=0x%04x", id, mask, button));
LOG((CLOG_INFO "onKeyDown: id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);

	// handle command keys
	if (onCommandKey(id, mask, true)) {
		return;
	}

	// relay
	m_active->keyDown(id, mask, button);
}

void
CServer::onKeyUp(KeyID id, KeyModifierMask mask, KeyButton button)
{
	LOG((CLOG_DEBUG1 "onKeyUp id=%d mask=0x%04x button=0x%04x", id, mask, button));
LOG((CLOG_INFO "onKeyUp id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);

	// handle command keys
	if (onCommandKey(id, mask, false)) {
		return;
	}

	// relay
	m_active->keyUp(id, mask, button);
}

void
CServer::onKeyRepeat(KeyID id, KeyModifierMask mask,
				SInt32 count, KeyButton button)
{
	LOG((CLOG_DEBUG1 "onKeyRepeat id=%d mask=0x%04x count=%d button=0x%04x", id, mask, count, button));
LOG((CLOG_INFO "onKeyRepeat id=%d mask=0x%04x count=%d button=0x%04x", id, mask, count, button));
	assert(m_active != NULL);

	// handle command keys
	if (onCommandKey(id, mask, false)) {
		onCommandKey(id, mask, true);
		return;
	}

	// relay
	m_active->keyRepeat(id, mask, count, button);
}

void
CServer::onMouseDown(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseDown id=%d", id));
	assert(m_active != NULL);

	// relay
	m_active->mouseDown(id);
}

void
CServer::onMouseUp(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseUp id=%d", id));
	assert(m_active != NULL);

	// relay
	m_active->mouseUp(id);
}

bool
CServer::onMouseMovePrimary(SInt32 x, SInt32 y)
{
	LOG((CLOG_DEBUG2 "onMouseMovePrimary %d,%d", x, y));

	// mouse move on primary (server's) screen
	assert(m_active == m_primaryClient);

	// save position
	m_x = x;
	m_y = y;

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);
	SInt32 zoneSize = getJumpZoneSize(m_active);

	// see if we should change screens
	EDirection dir;
	if (x < ax + zoneSize) {
		x  -= zoneSize;
		dir = kLeft;
	}
	else if (x >= ax + aw - zoneSize) {
		x  += zoneSize;
		dir = kRight;
	}
	else if (y < ay + zoneSize) {
		y  -= zoneSize;
		dir = kTop;
	}
	else if (y >= ay + ah - zoneSize) {
		y  += zoneSize;
		dir = kBottom;
	}
	else {
		// still on local screen
		noSwitch(x, y);
		return false;
	}

	// get jump destination
	IClient* newScreen = getNeighbor(m_active, dir, x, y);

	// should we switch or not?
	if (isSwitchOkay(newScreen, dir, x, y)) {
		// switch screen
		switchScreen(newScreen, x, y, false);
		return true;
	}
	else {
		return false;
	}
}

void
CServer::onMouseMoveSecondary(SInt32 dx, SInt32 dy)
{
	LOG((CLOG_DEBUG2 "onMouseMoveSecondary %+d,%+d", dx, dy));

	// mouse move on secondary (client's) screen
	assert(m_active != NULL);
	if (m_active == m_primaryClient) {
		// we're actually on the primary screen.  this can happen
		// when the primary screen begins processing a mouse move
		// for a secondary screen, then the active (secondary)
		// screen disconnects causing us to jump to the primary
		// screen, and finally the primary screen finishes
		// processing the mouse move, still thinking it's for
		// a secondary screen.  we just ignore the motion.
		return;
	}

	// save old position
	const SInt32 xOld = m_x;
	const SInt32 yOld = m_y;

	// accumulate motion
	m_x += dx;
	m_y += dy;

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);

	// find direction of neighbor and get the neighbor
	bool jump = true;
	IClient* newScreen;
	do {
		EDirection dir;
		if (m_x < ax) {
			dir = kLeft;
		}
		else if (m_x > ax + aw - 1) {
			dir = kRight;
		}
		else if (m_y < ay) {
			dir = kTop;
		}
		else if (m_y > ay + ah - 1) {
			dir = kBottom;
		}
		else {
			// we haven't left the screen
			newScreen = m_active;
			jump      = false;

			// if waiting and mouse is not on the border we're waiting
			// on then stop waiting.  also if it's not on the border
			// then arm the double tap.
			if (m_switchScreen != NULL) {
				bool clearWait;
				SInt32 zoneSize = m_primaryClient->getJumpZoneSize();
				switch (m_switchDir) {
				case kLeft:
					clearWait = (m_x >= ax + zoneSize);
					break;

				case kRight:
					clearWait = (m_x <= ax + aw - 1 - zoneSize);
					break;

				case kTop:
					clearWait = (m_y >= ay + zoneSize);
					break;

				case kBottom:
					clearWait = (m_y <= ay + ah - 1 + zoneSize);
					break;

				default:
					clearWait = false;
					break;
				}
				if (clearWait) {
					// still on local screen
					noSwitch(m_x, m_y);
				}
			}

			// skip rest of block
			break;
		}

		// try to switch screen.  get the neighbor.
		newScreen = getNeighbor(m_active, dir, m_x, m_y);

		// see if we should switch
		if (!isSwitchOkay(newScreen, dir, m_x, m_y)) {
			newScreen = m_active;
			jump      = false;
		}
	} while (false);

	if (jump) {
		// switch screens
		switchScreen(newScreen, m_x, m_y, false);
	}
	else {
		// same screen.  clamp mouse to edge.
		m_x = xOld + dx;
		m_y = yOld + dy;
		if (m_x < ax) {
			m_x = ax;
			LOG((CLOG_DEBUG2 "clamp to left of \"%s\"", getName(m_active).c_str()));
		}
		else if (m_x > ax + aw - 1) {
			m_x = ax + aw - 1;
			LOG((CLOG_DEBUG2 "clamp to right of \"%s\"", getName(m_active).c_str()));
		}
		if (m_y < ay) {
			m_y = ay;
			LOG((CLOG_DEBUG2 "clamp to top of \"%s\"", getName(m_active).c_str()));
		}
		else if (m_y > ay + ah - 1) {
			m_y = ay + ah - 1;
			LOG((CLOG_DEBUG2 "clamp to bottom of \"%s\"", getName(m_active).c_str()));
		}

		// warp cursor if it moved.
		if (m_x != xOld || m_y != yOld) {
			LOG((CLOG_DEBUG2 "move on %s to %d,%d", getName(m_active).c_str(), m_x, m_y));
			m_active->mouseMove(m_x, m_y);
		}
	}
}

void
CServer::onMouseWheel(SInt32 delta)
{
	LOG((CLOG_DEBUG1 "onMouseWheel %+d", delta));
	assert(m_active != NULL);

	// relay
	m_active->mouseWheel(delta);
}

bool
CServer::addClient(IClient* client)
{
	CString name = getName(client);
	if (m_clients.count(name) != 0) {
		return false;
	}

	// add event handlers
	EVENTQUEUE->adoptHandler(IScreen::getShapeChangedEvent(),
							client->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleShapeChanged, client));
	EVENTQUEUE->adoptHandler(IScreen::getClipboardGrabbedEvent(),
							client->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleClipboardGrabbed, client));
	EVENTQUEUE->adoptHandler(CClientProxy::getClipboardChangedEvent(),
							client->getEventTarget(),
							new TMethodEventJob<CServer>(this,
								&CServer::handleClipboardChanged, client));

	// add to list
	m_clientSet.insert(client);
	m_clients.insert(std::make_pair(name, client));

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());

	return true;
}

bool
CServer::removeClient(IClient* client)
{
	// return false if not in list
	CClientList::iterator i = m_clients.find(getName(client));
	if (i == m_clients.end()) {
		return false;
	}

	// remove event handlers
	EVENTQUEUE->removeHandler(IScreen::getShapeChangedEvent(),
							client->getEventTarget());
	EVENTQUEUE->removeHandler(IScreen::getClipboardGrabbedEvent(),
							client->getEventTarget());
	EVENTQUEUE->removeHandler(CClientProxy::getClipboardChangedEvent(),
							client->getEventTarget());

	// remove from list
	m_clients.erase(i);
	m_clientSet.erase(client);

	return true;
}

void
CServer::closeClient(IClient* client, const char* msg)
{
	assert(client != m_primaryClient);
	assert(msg != NULL);

	// send message to client.  this message should cause the client
	// to disconnect.  we add this client to the closed client list
	// and install a timer to remove the client if it doesn't respond
	// quickly enough.  we also remove the client from the active
	// client list since we're not going to listen to it anymore.
	// note that this method also works on clients that are not in
	// the m_clients list.  adoptClient() may call us with such a
	// client.
	LOG((CLOG_NOTE "disconnecting client \"%s\"", getName(client).c_str()));

	// send message
	// FIXME -- avoid type cast (kinda hard, though)
	((CClientProxy*)client)->close(msg);

	// install timer.  wait timeout seconds for client to close.
	double timeout = 5.0;
	CEventQueueTimer* timer = EVENTQUEUE->newOneShotTimer(timeout, NULL);
	EVENTQUEUE->adoptHandler(CEvent::kTimer, timer,
							new TMethodEventJob<CServer>(this,
								&CServer::handleClientCloseTimeout, client));

	// move client to closing list
	removeClient(client);
	m_oldClients.insert(std::make_pair(client, timer));

	// if this client is the active screen then we have to
	// jump off of it
	forceLeaveClient(client);
}

void
CServer::closeClients(const CConfig& config)
{
	// collect the clients that are connected but are being dropped
	// from the configuration (or who's canonical name is changing).
	typedef std::set<IClient*> CRemovedClients;
	CRemovedClients removed;
	for (CClientList::iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		if (!config.isCanonicalName(index->first)) {
			removed.insert(index->second);
		}
	}

	// don't close the primary client
	removed.erase(m_primaryClient);

	// now close them.  we collect the list then close in two steps
	// because closeClient() modifies the collection we iterate over.
	for (CRemovedClients::iterator index = removed.begin();
								index != removed.end(); ++index) {
		closeClient(*index, kMsgCClose);
	}
}

void
CServer::removeActiveClient(IClient* client)
{
	if (removeClient(client)) {
		forceLeaveClient(client);
		EVENTQUEUE->removeHandler(CClientProxy::getDisconnectedEvent(), client);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			EVENTQUEUE->addEvent(CEvent(getDisconnectedEvent(), this));
		}
	}
}

void
CServer::removeOldClient(IClient* client)
{
	COldClients::iterator i = m_oldClients.find(client);
	if (i != m_oldClients.end()) {
		EVENTQUEUE->removeHandler(CClientProxy::getDisconnectedEvent(), client);
		EVENTQUEUE->removeHandler(CEvent::kTimer, i->second);
		EVENTQUEUE->deleteTimer(i->second);
		m_oldClients.erase(i);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			EVENTQUEUE->addEvent(CEvent(getDisconnectedEvent(), this));
		}
	}
}

void
CServer::forceLeaveClient(IClient* client)
{
	IClient* active = (m_activeSaver != NULL) ? m_activeSaver : m_active;
	if (active == client) {
		// record new position (center of primary screen)
		m_primaryClient->getCursorCenter(m_x, m_y);

		// stop waiting to switch to this client
		if (active == m_switchScreen) {
			stopSwitch();
		}

		// don't notify active screen since it has probably already
		// disconnected.
		LOG((CLOG_INFO "jump from \"%s\" to \"%s\" at %d,%d", getName(active).c_str(), getName(m_primaryClient).c_str(), m_x, m_y));

		// cut over
		m_active = m_primaryClient;

		// enter new screen (unless we already have because of the
		// screen saver)
		if (m_activeSaver == NULL) {
			m_primaryClient->enter(m_x, m_y, m_seqNum,
								m_primaryClient->getToggleMask(), false);
		}
	}

	// if this screen had the cursor when the screen saver activated
	// then we can't switch back to it when the screen saver
	// deactivates.
	if (m_activeSaver == client) {
		m_activeSaver = NULL;
	}

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());
}


//
// CServer::CClipboardInfo
//

CServer::CClipboardInfo::CClipboardInfo() :
	m_clipboard(),
	m_clipboardData(),
	m_clipboardOwner(),
	m_clipboardSeqNum(0)
{
	// do nothing
}
