#include "CClient.h"
#include "CInputPacketStream.h"
#include "COutputPacketStream.h"
#include "CProtocolUtil.h"
#include "CClipboard.h"
#include "ISecondaryScreen.h"
#include "ProtocolTypes.h"
#include "CLock.h"
#include "CThread.h"
#include "CTimerThread.h"
#include "XSynergy.h"
#include "TMethodJob.h"
#include "CLog.h"
#include <assert.h>
#include <memory>

// hack to work around operator=() bug in STL in g++ prior to v3
#if defined(__GNUC__) && (__GNUC__ < 3)
#define assign(_dst, _src, _type)	_dst.reset(_src)
#else
#define assign(_dst, _src, _type)	_dst = std::auto_ptr<_type >(_src)
#endif


//
// CClient
//

CClient::CClient(const CString& clientName) :
								m_name(clientName),
								m_input(NULL),
								m_output(NULL),
								m_screen(NULL)
{
	// do nothing
}

CClient::~CClient()
{
	// do nothing
}

void					CClient::run(const CNetworkAddress& serverAddress)
{
	CThread* thread;
	try {
		log((CLOG_NOTE "starting client"));

		// connect to secondary screen
		openSecondaryScreen();

		// start server interactions
		m_serverAddress = &serverAddress;
		thread = new CThread(new TMethodJob<CClient>(this, &CClient::runSession));

		// handle events
		log((CLOG_DEBUG "starting event handling"));
		m_screen->run();

		// clean up
		log((CLOG_DEBUG "stopping client"));
		thread->cancel();
		thread->wait();
		delete thread;
		closeSecondaryScreen();
	}
	catch (XBase& e) {
		log((CLOG_ERR "client error: %s", e.what()));

		// clean up
		thread->cancel();
		thread->wait();
		delete thread;
		closeSecondaryScreen();
	}
	catch (...) {
		log((CLOG_DEBUG "unknown client error"));

		// clean up
		thread->cancel();
		thread->wait();
		delete thread;
		closeSecondaryScreen();
		throw;
	}
}

void					CClient::onClipboardChanged()
{
	log((CLOG_DEBUG "sending clipboard changed"));
	CLock lock(&m_mutex);
	if (m_output != NULL) {
		// m_output can be NULL if the screen calls this method
		// before we've gotten around to connecting to the server.
		CProtocolUtil::writef(m_output, kMsgCClipboard);
	}
}

#include "CTCPSocket.h" // FIXME
void					CClient::runSession(void*)
{
	log((CLOG_DEBUG "starting client \"%s\"", m_name.c_str()));

	std::auto_ptr<ISocket> socket;
	std::auto_ptr<IInputStream> input;
	std::auto_ptr<IOutputStream> output;
	try {
		// allow connect this much time to succeed
		CTimerThread timer(30.0);		// FIXME -- timeout in member

		// create socket and attempt to connect to server
		log((CLOG_DEBUG "connecting to server"));
		assign(socket, new CTCPSocket(), ISocket);	// FIXME -- use factory
		socket->connect(*m_serverAddress);
		log((CLOG_INFO "connected to server"));

		// get the input and output streams
		IInputStream*  srcInput  = socket->getInputStream();
		IOutputStream* srcOutput = socket->getOutputStream();

		// attach the encryption layer
		bool own = false;
/* FIXME -- implement ISecurityFactory
		if (m_securityFactory != NULL) {
			input.reset(m_securityFactory->createInputFilter(srcInput, own));
			output.reset(m_securityFactory->createOutputFilter(srcOutput, own));
			srcInput  = input.get();
			srcOutput = output.get();
			own       = true;
		}
*/

		// attach the packetizing filters
		assign(input, new CInputPacketStream(srcInput, own), IInputStream);
		assign(output, new COutputPacketStream(srcOutput, own), IOutputStream);

		// wait for hello from server
		log((CLOG_DEBUG "wait for hello"));
		SInt32 major, minor;
		CProtocolUtil::readf(input.get(), "Synergy%2i%2i", &major, &minor);

		// check versions
		log((CLOG_DEBUG "got hello version %d.%d", major, minor));
		if (major < kMajorVersion ||
			(major == kMajorVersion && minor < kMinorVersion)) {
			throw XIncompatibleClient(major, minor);
		}

		// say hello back
		log((CLOG_DEBUG "say hello version %d.%d", kMajorVersion, kMinorVersion));
		CProtocolUtil::writef(output.get(), "Synergy%2i%2i%s",
								kMajorVersion, kMinorVersion, &m_name);

		// record streams in a more useful place
		CLock lock(&m_mutex);
		m_input  = input.get();
		m_output = output.get();
	}
	catch (XIncompatibleClient& e) {
		log((CLOG_ERR "server has incompatible version %d.%d", e.getMajor(), e.getMinor()));
		m_screen->stop();
		return;
	}
	catch (XThread&) {
		log((CLOG_ERR "connection timed out"));
		m_screen->stop();
		throw;
	}
	catch (XBase& e) {
		log((CLOG_ERR "connection failed: %s", e.what()));
		m_screen->stop();
		return;
	}

	try {
		// handle messages from server
		for (;;) {
			// wait for reply
			log((CLOG_DEBUG "waiting for message"));
			UInt8 code[4];
			UInt32 n = input->read(code, 4);

			// verify we got an entire code
			if (n == 0) {
				log((CLOG_NOTE "server disconnected"));
				// server hungup
				break;
			}
			if (n != 4) {
				// client sent an incomplete message
				log((CLOG_ERR "incomplete message from server"));
				break;
			}

			// parse message
			log((CLOG_DEBUG "msg from server: %c%c%c%c", code[0], code[1], code[2], code[3]));
			if (memcmp(code, kMsgDMouseMove, 4) == 0) {
				onMouseMove();
			}
			else if (memcmp(code, kMsgDMouseWheel, 4) == 0) {
				onMouseWheel();
			}
			else if (memcmp(code, kMsgDKeyDown, 4) == 0) {
				onKeyDown();
			}
			else if (memcmp(code, kMsgDKeyUp, 4) == 0) {
				onKeyUp();
			}
			else if (memcmp(code, kMsgDMouseDown, 4) == 0) {
				onMouseDown();
			}
			else if (memcmp(code, kMsgDMouseUp, 4) == 0) {
				onMouseUp();
			}
			else if (memcmp(code, kMsgDKeyRepeat, 4) == 0) {
				onKeyRepeat();
			}
			else if (memcmp(code, kMsgCEnter, 4) == 0) {
				onEnter();
			}
			else if (memcmp(code, kMsgCLeave, 4) == 0) {
				onLeave();
			}
			else if (memcmp(code, kMsgCClipboard, 4) == 0) {
				onGrabClipboard();
			}
			else if (memcmp(code, kMsgCScreenSaver, 4) == 0) {
				onScreenSaver();
			}
			else if (memcmp(code, kMsgQInfo, 4) == 0) {
				onQueryInfo();
			}
			else if (memcmp(code, kMsgQClipboard, 4) == 0) {
				onQueryClipboard();
			}
			else if (memcmp(code, kMsgDClipboard, 4) == 0) {
				onSetClipboard();
			}
			else if (memcmp(code, kMsgCClose, 4) == 0) {
				// server wants us to hangup
				break;
			}
			else {
				// unknown message
				log((CLOG_ERR "unknown message from server"));
				break;
			}
		}
	}
	catch (XBase& e) {
		log((CLOG_ERR "error: %s", e.what()));
		m_screen->stop();
		return;
	}

	// done with socket
	log((CLOG_DEBUG "disconnecting from server"));
	socket->close();

	// exit event loop
	m_screen->stop();
}

// FIXME -- use factory to create screen
#if defined(CONFIG_PLATFORM_WIN32)
#include "CMSWindowsSecondaryScreen.h"
#elif defined(CONFIG_PLATFORM_UNIX)
#include "CXWindowsSecondaryScreen.h"
#endif
void					CClient::openSecondaryScreen()
{
	assert(m_screen == NULL);

	// open screen
	log((CLOG_DEBUG "creating secondary screen"));
#if defined(CONFIG_PLATFORM_WIN32)
	m_screen = new CMSWindowsSecondaryScreen;
#elif defined(CONFIG_PLATFORM_UNIX)
	m_screen = new CXWindowsSecondaryScreen;
#endif
	log((CLOG_DEBUG "opening secondary screen"));
	m_screen->open(this);
}

void					CClient::closeSecondaryScreen()
{
	assert(m_screen != NULL);

	// close the secondary screen
	try {
		log((CLOG_DEBUG "closing secondary screen"));
		m_screen->close();
	}
	catch (...) {
		// ignore
	}

	// clean up
	log((CLOG_DEBUG "destroying secondary screen"));
	delete m_screen;
	m_screen = NULL;
}

void					CClient::onEnter()
{
	SInt32 x, y;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgCEnter + 4, &x, &y);
	}
	m_screen->enter(x, y);
}

void					CClient::onLeave()
{
	m_screen->leave();
}

void					CClient::onGrabClipboard()
{
	m_screen->grabClipboard();
}

void					CClient::onScreenSaver()
{
	SInt32 on;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgCScreenSaver + 4, &on);
	}
	// FIXME
}

void					CClient::onQueryInfo()
{
	SInt32 w, h;
	m_screen->getSize(&w, &h);
	SInt32 zoneSize = m_screen->getJumpZoneSize();

	log((CLOG_DEBUG "sending info size=%d,%d zone=%d", w, h, zoneSize));
	CLock lock(&m_mutex);
	CProtocolUtil::writef(m_output, kMsgDInfo, w, h, zoneSize);
}

void					CClient::onQueryClipboard()
{
	// parse message
	UInt32 seqNum;
	CClipboard clipboard;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgQClipboard + 4, &seqNum);
	}
	log((CLOG_DEBUG "received query clipboard seqnum=%d", seqNum));

	// get screen's clipboard data
	m_screen->getClipboard(&clipboard);

	// marshall the data
	CString data = clipboard.marshall();

	// send it
	log((CLOG_DEBUG "sending clipboard seqnum=%d, size=%d", seqNum, data.size()));
	{
		CLock lock(&m_mutex);
		CProtocolUtil::writef(m_output, kMsgDClipboard, seqNum, &data);
	}
}

void					CClient::onSetClipboard()
{
	CString data;
	{
		// parse message
		UInt32 seqNum;
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgDClipboard + 4, &seqNum, &data);
	}
	log((CLOG_DEBUG "received clipboard size=%d", data.size()));

	// unmarshall
	CClipboard clipboard;
	clipboard.unmarshall(data);

	// set screen's clipboard
	m_screen->setClipboard(&clipboard);
}

void					CClient::onKeyDown()
{
	SInt32 id, mask;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgDKeyDown + 4, &id, &mask);
	}
	m_screen->keyDown(static_cast<KeyID>(id),
								static_cast<KeyModifierMask>(mask));
}

void					CClient::onKeyRepeat()
{
	SInt32 id, mask, count;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgDKeyRepeat + 4, &id, &mask, &count);
	}
	m_screen->keyRepeat(static_cast<KeyID>(id),
								static_cast<KeyModifierMask>(mask),
								count);
}

void					CClient::onKeyUp()
{
	SInt32 id, mask;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgDKeyUp + 4, &id, &mask);
	}
	m_screen->keyUp(static_cast<KeyID>(id),
								static_cast<KeyModifierMask>(mask));
}

void					CClient::onMouseDown()
{
	SInt32 id;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgDMouseDown + 4, &id);
	}
	m_screen->mouseDown(static_cast<ButtonID>(id));
}

void					CClient::onMouseUp()
{
	SInt32 id;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgDMouseUp + 4, &id);
	}
	m_screen->mouseUp(static_cast<ButtonID>(id));
}

void					CClient::onMouseMove()
{
	SInt32 x, y;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgDMouseMove + 4, &x, &y);
	}
	m_screen->mouseMove(x, y);
}

void					CClient::onMouseWheel()
{
	SInt32 delta;
	{
		CLock lock(&m_mutex);
		CProtocolUtil::readf(m_input, kMsgDMouseWheel + 4, &delta);
	}
	m_screen->mouseWheel(delta);
}