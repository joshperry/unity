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

#include "CClientListener.h"
#include "CClientProxy.h"
#include "CConfig.h"
#include "CPrimaryClient.h"
#include "CServer.h"
#include "CScreen.h"
#include "ProtocolTypes.h"
#include "Version.h"
#include "XScreen.h"
#include "CSocketMultiplexer.h"
#include "CTCPSocketFactory.h"
#include "XSocket.h"
#include "CThread.h"
#include "CEventQueue.h"
#include "CFunctionEventJob.h"
#include "CLog.h"
#include "CString.h"
#include "CStringUtil.h"
#include "LogOutputters.h"
#include "CArch.h"
#include "XArch.h"
#include "stdfstream.h"
#include <cstring>

#define DAEMON_RUNNING(running_)
#if WINDOWS_LIKE
#include "CArchMiscWindows.h"
#include "CMSWindowsScreen.h"
#include "CMSWindowsUtil.h"
#include "CMSWindowsServerTaskBarReceiver.h"
#include "resource.h"
#undef DAEMON_RUNNING
#define DAEMON_RUNNING(running_) CArchMiscWindows::daemonRunning(running_)
#elif UNIX_LIKE
#include "CXWindowsScreen.h"
#include "CXWindowsServerTaskBarReceiver.h"
#endif

// platform dependent name of a daemon
#if WINDOWS_LIKE
#define DAEMON_NAME "Synergy Server"
#elif UNIX_LIKE
#define DAEMON_NAME "synergys"
#endif

// configuration file name
#if WINDOWS_LIKE
#define USR_CONFIG_NAME "synergy.sgc"
#define SYS_CONFIG_NAME "synergy.sgc"
#elif UNIX_LIKE
#define USR_CONFIG_NAME ".synergy.conf"
#define SYS_CONFIG_NAME "synergy.conf"
#endif

typedef int (*StartupFunc)(int, char**);
static void parse(int argc, const char* const* argv);
static bool loadConfig(const CString& pathname);
static void loadConfig();

//
// program arguments
//

#define ARG CArgs::s_instance

class CArgs {
public:
	CArgs() :
		m_pname(NULL),
		m_backend(false),
		m_restartable(true),
		m_daemon(true),
		m_configFile(),
		m_logFilter(NULL)
		{ s_instance = this; }
	~CArgs() { s_instance = NULL; }

public:
	static CArgs*		s_instance;
	const char* 		m_pname;
	bool				m_backend;
	bool				m_restartable;
	bool				m_daemon;
	CString		 		m_configFile;
	const char* 		m_logFilter;
	CString 			m_name;
	CNetworkAddress*	m_synergyAddress;
	CConfig*			m_config;
};

CArgs*					CArgs::s_instance = NULL;


//
// platform dependent factories
//

static
CScreen*
createScreen()
{
#if WINDOWS_LIKE
	return new CScreen(new CMSWindowsScreen(true, NULL, NULL));
#elif UNIX_LIKE
	return new CScreen(new CXWindowsScreen(true));
#endif
}

static
CServerTaskBarReceiver*
createTaskBarReceiver(const CBufferedLogOutputter* logBuffer)
{
#if WINDOWS_LIKE
	return new CMSWindowsServerTaskBarReceiver(
							CMSWindowsScreen::getInstance(), logBuffer);
#elif UNIX_LIKE
	return new CXWindowsServerTaskBarReceiver(logBuffer);
#endif
}


//
// platform independent main
//

static CServer*					s_server            = NULL;
static CScreen*					s_serverScreen      = NULL;
static CPrimaryClient*			s_primaryClient     = NULL;
static CClientListener*			s_listener          = NULL;
static CServerTaskBarReceiver*	s_taskBarReceiver   = NULL;
static CEvent::Type				s_reloadConfigEvent = CEvent::kUnknown;

CEvent::Type
getReloadConfigEvent()
{
	return CEvent::registerTypeOnce(s_reloadConfigEvent, "reloadConfig");
}

static
void
updateStatus()
{
	s_taskBarReceiver->updateStatus(s_server, "");
}

static
void
updateStatus(const CString& msg)
{
	s_taskBarReceiver->updateStatus(s_server, msg);
}

static
void
handleClientConnected(const CEvent&, void* vlistener)
{
	CClientListener* listener = reinterpret_cast<CClientListener*>(vlistener);
	CClientProxy* client = listener->getNextClient();
	if (client != NULL) {
		s_server->adoptClient(client);
		updateStatus();
	}
}

static
CClientListener*
openClientListener(const CNetworkAddress& address)
{
	CClientListener* listen =
		new CClientListener(address, new CTCPSocketFactory, NULL);
	EVENTQUEUE->adoptHandler(CClientListener::getConnectedEvent(), listen,
							new CFunctionEventJob(
								&handleClientConnected, listen));
	return listen;
}

static
void
closeClientListener(CClientListener* listen)
{
	if (listen != NULL) {
		EVENTQUEUE->removeHandler(CClientListener::getConnectedEvent(), listen);
		delete listen;
	}
}

static
void
handleScreenError(const CEvent&, void*)
{
	LOG((CLOG_CRIT "error on screen"));
	EVENTQUEUE->addEvent(CEvent(CEvent::kQuit));
}

static
CScreen*
openServerScreen()
{
	CScreen* screen = createScreen();
	EVENTQUEUE->adoptHandler(IScreen::getErrorEvent(),
							screen->getEventTarget(),
							new CFunctionEventJob(
								&handleScreenError));
	return screen;
}

static
void
closeServerScreen(CScreen* screen)
{
	if (screen != NULL) {
		EVENTQUEUE->removeHandler(IScreen::getErrorEvent(),
							screen->getEventTarget());
		delete screen;
	}
}

static
CPrimaryClient*
openPrimaryClient(const CString& name, CScreen* screen)
{
	LOG((CLOG_DEBUG1 "creating primary screen"));
	return new CPrimaryClient(name, screen);
}

static
void
closePrimaryClient(CPrimaryClient* primaryClient)
{
	delete primaryClient;
}

static
void
handleNoClients(const CEvent&, void*)
{
	updateStatus();
}

static
void
handleClientsDisconnected(const CEvent&, void*)
{
	EVENTQUEUE->addEvent(CEvent(CEvent::kQuit));
}

static
CServer*
openServer(const CConfig& config, CPrimaryClient* primaryClient)
{
	CServer* server = new CServer(config, primaryClient);
	EVENTQUEUE->adoptHandler(CServer::getDisconnectedEvent(), server,
						new CFunctionEventJob(handleNoClients));
	return server;
}

static
void
closeServer(CServer* server)
{
	if (server == NULL) {
		return;
	}

	// tell all clients to disconnect
	server->disconnect();

	// wait for clients to disconnect for up to timeout seconds
	double timeout = 3.0;
	CEventQueueTimer* timer = EVENTQUEUE->newOneShotTimer(timeout, NULL);
	EVENTQUEUE->adoptHandler(CEvent::kTimer, timer,
						new CFunctionEventJob(handleClientsDisconnected));
	EVENTQUEUE->adoptHandler(CServer::getDisconnectedEvent(), server,
						new CFunctionEventJob(handleClientsDisconnected));
	CEvent event;
	EVENTQUEUE->getEvent(event);
	while (event.getType() != CEvent::kQuit) {
		EVENTQUEUE->dispatchEvent(event);
		CEvent::deleteData(event);
		EVENTQUEUE->getEvent(event);
	}
	EVENTQUEUE->removeHandler(CEvent::kTimer, timer);
	EVENTQUEUE->deleteTimer(timer);
	EVENTQUEUE->removeHandler(CServer::getDisconnectedEvent(), server);

	// done with server
	delete server;
}

static bool startServer();

static
void
retryStartHandler(const CEvent&, void* vtimer)
{
	// discard old timer
	CEventQueueTimer* timer = reinterpret_cast<CEventQueueTimer*>(vtimer);
	EVENTQUEUE->deleteTimer(timer);
	EVENTQUEUE->removeHandler(CEvent::kTimer, NULL);

	// try starting the server again
	LOG((CLOG_DEBUG1 "retry starting server"));
	startServer();
}

static
bool
startServer()
{
	double retryTime;
	CScreen* serverScreen         = NULL;
	CPrimaryClient* primaryClient = NULL;
	CClientListener* listener     = NULL;
	try {
		CString name    = ARG->m_config->getCanonicalName(ARG->m_name);
		serverScreen    = openServerScreen();
		primaryClient   = openPrimaryClient(name, serverScreen);
		listener        = openClientListener(ARG->m_config->getSynergyAddress());
		s_server        = openServer(*ARG->m_config, primaryClient);
		s_serverScreen  = serverScreen;
		s_primaryClient = primaryClient;
		s_listener      = listener;
		updateStatus();
		LOG((CLOG_NOTE "started server"));
		return true;
	}
	catch (XScreenUnavailable& e) {
		LOG((CLOG_WARN "cannot open primary screen: %s", e.what()));
		closeClientListener(listener);
		closePrimaryClient(primaryClient);
		closeServerScreen(serverScreen);
		updateStatus(CString("cannot open primary screen: ") + e.what());
		retryTime = e.getRetryTime();
	}
	catch (XSocketAddressInUse& e) {
		LOG((CLOG_WARN "cannot listen for clients: %s", e.what()));
		closeClientListener(listener);
		closePrimaryClient(primaryClient);
		closeServerScreen(serverScreen);
		updateStatus(CString("cannot listen for clients: ") + e.what());
		retryTime = 10.0;
	}
	catch (XScreenOpenFailure& e) {
		LOG((CLOG_CRIT "cannot open primary screen: %s", e.what()));
		closeClientListener(listener);
		closePrimaryClient(primaryClient);
		closeServerScreen(serverScreen);
		return false;
	}
	catch (XBase& e) {
		LOG((CLOG_CRIT "failed to start server: %s", e.what()));
		closeClientListener(listener);
		closePrimaryClient(primaryClient);
		closeServerScreen(serverScreen);
		return false;
	}

	if (ARG->m_restartable) {
		// install a timer and handler to retry later
		LOG((CLOG_DEBUG "retry in %.0f seconds", retryTime));
		CEventQueueTimer* timer = EVENTQUEUE->newOneShotTimer(retryTime, NULL);
		EVENTQUEUE->adoptHandler(CEvent::kTimer, timer,
							new CFunctionEventJob(&retryStartHandler, timer));
		return true;
	}
	else {
		// don't try again
		return false;
	}
}

static
void
stopServer()
{
	closeClientListener(s_listener);
	closeServer(s_server);
	closePrimaryClient(s_primaryClient);
	closeServerScreen(s_serverScreen);
	s_server        = NULL;
	s_listener      = NULL;
	s_primaryClient = NULL;
	s_serverScreen  = NULL;
}

static
void
reloadSignalHandler(CArch::ESignal, void*)
{
	EVENTQUEUE->addEvent(CEvent(getReloadConfigEvent(),
							IEventQueue::getSystemTarget()));
}

static
void
reloadConfig(const CEvent&, void*)
{
	LOG((CLOG_DEBUG "reload configuration"));
	if (loadConfig(ARG->m_configFile)) {
		if (s_server != NULL) {
			s_server->setConfig(*ARG->m_config);
		}
		LOG((CLOG_NOTE "reloaded configuration"));
	}
}

static
int
mainLoop()
{
	// create socket multiplexer.  this must happen after daemonization
	// on unix because threads evaporate across a fork().
	CSocketMultiplexer multiplexer;

	// create the event queue
	CEventQueue eventQueue;

	// if configuration has no screens then add this system
	// as the default
	if (ARG->m_config->begin() == ARG->m_config->end()) {
		ARG->m_config->addScreen(ARG->m_name);
	}

	// set the contact address, if provided, in the config.
	// otherwise, if the config doesn't have an address, use
	// the default.
	if (ARG->m_synergyAddress->isValid()) {
		ARG->m_config->setSynergyAddress(*ARG->m_synergyAddress);
	}
	else if (!ARG->m_config->getSynergyAddress().isValid()) {
		ARG->m_config->setSynergyAddress(CNetworkAddress(kDefaultPort));
	}

	// canonicalize the primary screen name
	CString primaryName = ARG->m_config->getCanonicalName(ARG->m_name);
	if (primaryName.empty()) {
		LOG((CLOG_CRIT "unknown screen name `%s'", ARG->m_name.c_str()));
		return kExitFailed;
	}

	// start the server.  if this return false then we've failed and
	// we shouldn't retry.
	LOG((CLOG_DEBUG1 "starting server"));
	if (!startServer()) {
		return kExitFailed;
	}

	// handle hangup signal by reloading the server's configuration
	ARCH->setSignalHandler(CArch::kHANGUP, &reloadSignalHandler, NULL);
	EVENTQUEUE->adoptHandler(getReloadConfigEvent(),
							IEventQueue::getSystemTarget(),
							new CFunctionEventJob(&reloadConfig));

	// run event loop.  if startServer() failed we're supposed to retry
	// later.  the timer installed by startServer() will take care of
	// that.
	CEvent event;
	DAEMON_RUNNING(true);
	EVENTQUEUE->getEvent(event);
	while (event.getType() != CEvent::kQuit) {
		EVENTQUEUE->dispatchEvent(event);
		CEvent::deleteData(event);
		EVENTQUEUE->getEvent(event);
	}
	DAEMON_RUNNING(false);

	// close down
	LOG((CLOG_DEBUG1 "stopping server"));
	EVENTQUEUE->removeHandler(getReloadConfigEvent(),
							IEventQueue::getSystemTarget());
	stopServer();
	updateStatus();
	LOG((CLOG_NOTE "stopped server"));

	return kExitSuccess;
}

static
int
daemonMainLoop(int, const char**)
{
	CSystemLogger sysLogger(DAEMON_NAME);
	return mainLoop();
}

static
int
standardStartup(int argc, char** argv)
{
	// parse command line
	parse(argc, argv);

	// load configuration
	loadConfig();

	// daemonize if requested
	if (ARG->m_daemon) {
		return ARCH->daemonize(DAEMON_NAME, &daemonMainLoop);
	}
	else {
		return mainLoop();
	}
}

static
int
run(int argc, char** argv, ILogOutputter* outputter, StartupFunc startup)
{
	// general initialization
	ARG->m_synergyAddress = new CNetworkAddress;
	ARG->m_config         = new CConfig;
	ARG->m_pname          = ARCH->getBasename(argv[0]);

	// install caller's output filter
	if (outputter != NULL) {
		CLOG->insert(outputter);
	}

	// save log messages
	CBufferedLogOutputter logBuffer(1000);
	CLOG->insert(&logBuffer, true);

	// make the task bar receiver.  the user can control this app
	// through the task bar.
	s_taskBarReceiver = createTaskBarReceiver(&logBuffer);

	// run
	int result = startup(argc, argv);

	// done with task bar receiver
	delete s_taskBarReceiver;

	// done with log buffer
	CLOG->remove(&logBuffer);

	delete ARG->m_config;
	delete ARG->m_synergyAddress;
	return result;
}


//
// command line parsing
//

#define BYE "\nTry `%s --help' for more information."

static void				(*bye)(int) = &exit;

static
void
version()
{
	LOG((CLOG_PRINT
"%s %s, protocol version %d.%d\n"
"%s",
								ARG->m_pname,
								kVersion,
								kProtocolMajorVersion,
								kProtocolMinorVersion,
								kCopyright));
}

static
void
help()
{
#if WINDOWS_LIKE

#  define PLATFORM_ARGS												\
" {--daemon|--no-daemon}"
#  define PLATFORM_DESC
#  define PLATFORM_EXTRA											\
"At least one command line argument is required.  If you don't otherwise\n"	\
"need an argument use `--daemon'.\n"										\
"\n"

#else

#  define PLATFORM_ARGS												\
" [--daemon|--no-daemon]"
#  define PLATFORM_DESC
#  define PLATFORM_EXTRA

#endif

	LOG((CLOG_PRINT
"Usage: %s"
" [--address <address>]"
" [--config <pathname>]"
" [--debug <level>]"
" [--name <screen-name>]"
" [--restart|--no-restart]\n"
PLATFORM_ARGS
"\n"
"Start the synergy mouse/keyboard sharing server.\n"
"\n"
"  -a, --address <address>  listen for clients on the given address.\n"
"  -c, --config <pathname>  use the named configuration file instead.\n"
"  -d, --debug <level>      filter out log messages with priorty below level.\n"
"                           level may be: FATAL, ERROR, WARNING, NOTE, INFO,\n"
"                           DEBUG, DEBUG1, DEBUG2.\n"
"  -f, --no-daemon          run the server in the foreground.\n"
"*     --daemon             run the server as a daemon.\n"
"  -n, --name <screen-name> use screen-name instead the hostname to identify\n"
"                           this screen in the configuration.\n"
"  -1, --no-restart         do not try to restart the server if it fails for\n"
"                           some reason.\n"
"*     --restart            restart the server automatically if it fails.\n"
PLATFORM_DESC
"  -h, --help               display this help and exit.\n"
"      --version            display version information and exit.\n"
"\n"
"* marks defaults.\n"
"\n"
PLATFORM_EXTRA
"The argument for --address is of the form: [<hostname>][:<port>].  The\n"
"hostname must be the address or hostname of an interface on the system.\n"
"The default is to listen on all interfaces.  The port overrides the\n"
"default port, %d.\n"
"\n"
"If no configuration file pathname is provided then the first of the\n"
"following to load successfully sets the configuration:\n"
"  %s\n"
"  %s\n"
"If no configuration file can be loaded then the configuration uses its\n"
"defaults with just the server screen.\n"
"\n"
"Where log messages go depends on the platform and whether or not the\n"
"server is running as a daemon.",
								ARG->m_pname,
								kDefaultPort,
								ARCH->concatPath(
									ARCH->getUserDirectory(),
									USR_CONFIG_NAME).c_str(),
								ARCH->concatPath(
									ARCH->getSystemDirectory(),
									SYS_CONFIG_NAME).c_str()));
}

static
bool
isArg(int argi, int argc, const char* const* argv,
				const char* name1, const char* name2,
				int minRequiredParameters = 0)
{
	if ((name1 != NULL && strcmp(argv[argi], name1) == 0) ||
		(name2 != NULL && strcmp(argv[argi], name2) == 0)) {
		// match.  check args left.
		if (argi + minRequiredParameters >= argc) {
			LOG((CLOG_PRINT "%s: missing arguments for `%s'" BYE,
								ARG->m_pname, argv[argi], ARG->m_pname));
			bye(kExitArgs);
		}
		return true;
	}

	// no match
	return false;
}

static
void
parse(int argc, const char* const* argv)
{
	assert(ARG->m_pname != NULL);
	assert(argv       != NULL);
	assert(argc       >= 1);

	// set defaults
	ARG->m_name = ARCH->getHostName();

	// parse options
	int i;
	for (i = 1; i < argc; ++i) {
		if (isArg(i, argc, argv, "-d", "--debug", 1)) {
			// change logging level
			ARG->m_logFilter = argv[++i];
		}

		else if (isArg(i, argc, argv, "-a", "--address", 1)) {
			// save listen address
			try {
				*ARG->m_synergyAddress = CNetworkAddress(argv[i + 1],
														kDefaultPort);
			}
			catch (XSocketAddress& e) {
				LOG((CLOG_PRINT "%s: %s" BYE,
								ARG->m_pname, e.what(), ARG->m_pname));
				bye(kExitArgs);
			}
			++i;
		}

		else if (isArg(i, argc, argv, "-n", "--name", 1)) {
			// save screen name
			ARG->m_name = argv[++i];
		}

		else if (isArg(i, argc, argv, "-c", "--config", 1)) {
			// save configuration file path
			ARG->m_configFile = argv[++i];
		}

		else if (isArg(i, argc, argv, "-f", "--no-daemon")) {
			// not a daemon
			ARG->m_daemon = false;
		}

		else if (isArg(i, argc, argv, NULL, "--daemon")) {
			// daemonize
			ARG->m_daemon = true;
		}

		else if (isArg(i, argc, argv, "-1", "--no-restart")) {
			// don't try to restart
			ARG->m_restartable = false;
		}

		else if (isArg(i, argc, argv, NULL, "--restart")) {
			// try to restart
			ARG->m_restartable = true;
		}

		else if (isArg(i, argc, argv, "-z", NULL)) {
			ARG->m_backend = true;
		}

		else if (isArg(i, argc, argv, "-h", "--help")) {
			help();
			bye(kExitSuccess);
		}

		else if (isArg(i, argc, argv, NULL, "--version")) {
			version();
			bye(kExitSuccess);
		}

		else if (isArg(i, argc, argv, "--", NULL)) {
			// remaining arguments are not options
			++i;
			break;
		}

		else if (argv[i][0] == '-') {
			LOG((CLOG_PRINT "%s: unrecognized option `%s'" BYE,
								ARG->m_pname, argv[i], ARG->m_pname));
			bye(kExitArgs);
		}

		else {
			// this and remaining arguments are not options
			break;
		}
	}

	// no non-option arguments are allowed
	if (i != argc) {
		LOG((CLOG_PRINT "%s: unrecognized option `%s'" BYE,
								ARG->m_pname, argv[i], ARG->m_pname));
		bye(kExitArgs);
	}

	// increase default filter level for daemon.  the user must
	// explicitly request another level for a daemon.
	if (ARG->m_daemon && ARG->m_logFilter == NULL) {
#if WINDOWS_LIKE
		if (CArchMiscWindows::isWindows95Family()) {
			// windows 95 has no place for logging so avoid showing
			// the log console window.
			ARG->m_logFilter = "FATAL";
		}
		else
#endif
		{
			ARG->m_logFilter = "NOTE";
		}
	}

	// set log filter
	if (!CLOG->setFilter(ARG->m_logFilter)) {
		LOG((CLOG_PRINT "%s: unrecognized log level `%s'" BYE,
								ARG->m_pname, ARG->m_logFilter, ARG->m_pname));
		bye(kExitArgs);
	}
}

static
bool
loadConfig(const CString& pathname)
{
	try {
		// load configuration
		LOG((CLOG_DEBUG "opening configuration \"%s\"", pathname.c_str()));
		std::ifstream configStream(pathname.c_str());
		if (!configStream) {
			throw XConfigRead("cannot open file");
		}
		configStream >> *ARG->m_config;
		LOG((CLOG_DEBUG "configuration read successfully"));
		return true;
	}
	catch (XConfigRead& e) {
		LOG((CLOG_DEBUG "cannot read configuration \"%s\": %s",
								pathname.c_str(), e.what()));
	}
	return false;
}

static
void
loadConfig()
{
	bool loaded = false;

	// load the config file, if specified
	if (!ARG->m_configFile.empty()) {
		loaded = loadConfig(ARG->m_configFile);
	}

	// load the default configuration if no explicit file given
	else {
		// get the user's home directory
		CString path = ARCH->getUserDirectory();
		if (!path.empty()) {
			// complete path
			path = ARCH->concatPath(path, USR_CONFIG_NAME);

			// now try loading the user's configuration
			if (loadConfig(path)) {
				loaded            = true;
				ARG->m_configFile = path;
			}
		}
		if (!loaded) {
			// try the system-wide config file
			path = ARCH->getSystemDirectory();
			if (!path.empty()) {
				path = ARCH->concatPath(path, SYS_CONFIG_NAME);
				if (loadConfig(path)) {
					loaded            = true;
					ARG->m_configFile = path;
				}
			}
		}
	}

	if (!loaded) {
		LOG((CLOG_PRINT "%s: no configuration available", ARG->m_pname));
		bye(kExitConfig);
	}
}


//
// platform dependent entry points
//

#if WINDOWS_LIKE

static bool				s_hasImportantLogMessages = false;

//
// CMessageBoxOutputter
//
// This class writes severe log messages to a message box
//

class CMessageBoxOutputter : public ILogOutputter {
public:
	CMessageBoxOutputter() { }
	virtual ~CMessageBoxOutputter() { }

	// ILogOutputter overrides
	virtual void		open(const char*) { }
	virtual void		close() { }
	virtual bool		write(ELevel level, const char* message);
	virtual const char*	getNewline() const { return ""; }
};

bool
CMessageBoxOutputter::write(ELevel level, const char* message)
{
	// note any important messages the user may need to know about
	if (level <= CLog::kWARNING) {
		s_hasImportantLogMessages = true;
	}

	// FATAL and PRINT messages get a dialog box if not running as
	// backend.  if we're running as a backend the user will have
	// a chance to see the messages when we exit.
	if (!ARG->m_backend && level <= CLog::kFATAL) {
		MessageBox(NULL, message, ARG->m_pname, MB_OK | MB_ICONWARNING);
		return false;
	}
	else {
		return true;
	}
}

static
void
byeThrow(int x)
{
	CArchMiscWindows::daemonFailed(x);
}

static
int
daemonNTMainLoop(int argc, const char** argv)
{
	parse(argc, argv);
	ARG->m_backend = false;
	loadConfig();
	return CArchMiscWindows::runDaemon(mainLoop);
}

static
int
daemonNTStartup(int, char**)
{
	CSystemLogger sysLogger(DAEMON_NAME);
	bye = &byeThrow;
	return ARCH->daemonize(DAEMON_NAME, &daemonNTMainLoop);
}

static
void
showError(HINSTANCE instance, const char* title, UINT id, const char* arg)
{
	CString fmt = CMSWindowsUtil::getString(instance, id);
	CString msg = CStringUtil::format(fmt.c_str(), arg);
	MessageBox(NULL, msg.c_str(), title, MB_OK | MB_ICONWARNING);
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
	try {
		CArch arch(instance);
		CMSWindowsScreen::init(instance);
		CLOG;
		CThread::getCurrentThread().setPriority(-14);
		CArgs args;

		// windows NT family starts services using no command line options.
		// since i'm not sure how to tell the difference between that and
		// a user providing no options we'll assume that if there are no
		// arguments and we're on NT then we're being invoked as a service.
		// users on NT can use `--daemon' or `--no-daemon' to force us out
		// of the service code path.
		StartupFunc startup = &standardStartup;
		if (__argc <= 1 && !CArchMiscWindows::isWindows95Family()) {
			startup = &daemonNTStartup;
		}

		// send PRINT and FATAL output to a message box
		int result = run(__argc, __argv, new CMessageBoxOutputter, startup);

		// let user examine any messages if we're running as a backend
		// by putting up a dialog box before exiting.
		if (args.m_backend && s_hasImportantLogMessages) {
			showError(instance, args.m_pname, IDS_FAILED, "");
		}

		delete CLOG;
		return result;
	}
	catch (XBase& e) {
		showError(instance, __argv[0], IDS_UNCAUGHT_EXCEPTION, e.what());
		throw;
	}
	catch (XArch& e) {
		showError(instance, __argv[0], IDS_INIT_FAILED, e.what().c_str());
		return kExitFailed;
	}
	catch (...) {
		showError(instance, __argv[0], IDS_UNCAUGHT_EXCEPTION, "<unknown>");
		throw;
	}
}

#elif UNIX_LIKE

int
main(int argc, char** argv)
{
	CArgs args;
	try {
		int result;
		CArch arch;
		CLOG;
		CArgs args;
		result = run(argc, argv, NULL, &standardStartup);
		delete CLOG;
		return result;
	}
	catch (XBase& e) {
		LOG((CLOG_CRIT "Uncaught exception: %s\n", e.what()));
		throw;
	}
	catch (XArch& e) {
		LOG((CLOG_CRIT "Initialization failed: %s" BYE, e.what().c_str()));
		return kExitFailed;
	}
	catch (...) {
		LOG((CLOG_CRIT "Uncaught exception: <unknown exception>\n"));
		throw;
	}
}

#else

#error no main() for platform

#endif
