#include "CLog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(CONFIG_PLATFORM_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define vsnprintf _vsnprintf
#endif

// names of priorities
static const char*		g_priority[] = {
								"FATAL",
								"ERROR",
								"WARNING",
								"NOTE",
								"INFO",
								"DEBUG",
								"DEBUG1",
								"DEBUG2"
							};

// number of priorities
static const int		g_numPriority = (int)(sizeof(g_priority) /
											sizeof(g_priority[0]));

// the default priority
#if defined(NDEBUG)
static const int		g_defaultMaxPriority = 4;
#else
static const int		g_defaultMaxPriority = 5;
#endif

// length of longest string in g_priority
static const int		g_maxPriorityLength = 7;

// length of suffix string (": ")
static const int		g_prioritySuffixLength = 2;

// amount of padded required to fill in the priority prefix
static const int		g_priorityPad = g_maxPriorityLength +
										g_prioritySuffixLength;

// minimum length of a newline sequence
static const int		g_newlineLength = 2;

//
// CLog
//

CLog::Outputter			CLog::s_outputter = NULL;
CLog::Lock				CLog::s_lock = &CLog::dummyLock;
int						CLog::s_maxPriority = -1;

void
CLog::print(const char* fmt, ...)
{
	// check if fmt begins with a priority argument
	int priority = 4;
	if (fmt[0] == '%' && fmt[1] == 'z') {
		priority = fmt[2] - '\060';
		fmt += 3;
	}

	// done if below priority threshold
	if (priority > getMaxPriority()) {
		return;
	}

	// compute prefix padding length
	int pad = g_priorityPad;

	// print to buffer
	char stack[1024];
	va_list args;
	va_start(args, fmt);
	char* buffer = vsprint(pad, stack,
							sizeof(stack) / sizeof(stack[0]), fmt, args);
	va_end(args);

	// output buffer
	output(priority, buffer);

	// clean up
	if (buffer != stack)
		delete[] buffer;
}

void
CLog::printt(const char* file, int line, const char* fmt, ...)
{
	// check if fmt begins with a priority argument
	int priority = 4;
	if (fmt[0] == '%' && fmt[1] == 'z') {
		priority = fmt[2] - '\060';
		fmt += 3;
	}

	// done if below priority threshold
	if (priority > getMaxPriority()) {
		return;
	}

	// compute prefix padding length
	char stack[1024];
	sprintf(stack, "%d", line);
	int pad = strlen(file) + 1 /* comma */ +
				strlen(stack) + 1 /* colon */ + 1 /* space */ +
				g_priorityPad;

	// print to buffer, leaving space for a newline at the end
	va_list args;
	va_start(args, fmt);
	char* buffer = vsprint(pad, stack,
								sizeof(stack) / sizeof(stack[0]), fmt, args);
	va_end(args);

	// print the prefix to the buffer.  leave space for priority label.
	sprintf(buffer + g_priorityPad, "%s,%d:", file, line);
	buffer[pad - 1] = ' ';

	// discard file and line if priority < 0
	char* message = buffer;
	if (priority < 0) {
		message += pad - g_priorityPad;
	}

	// output buffer
	output(priority, message);

	// clean up
	if (buffer != stack)
		delete[] buffer;
}

void
CLog::setOutputter(Outputter outputter)
{
	CHoldLock lock(s_lock);
	s_outputter = outputter;
}

CLog::Outputter
CLog::getOutputter()
{
	CHoldLock lock(s_lock);
	return s_outputter;
}

void
CLog::setLock(Lock newLock)
{
	CHoldLock lock(s_lock);
	s_lock = (newLock == NULL) ? dummyLock : newLock;
}

CLog::Lock
CLog::getLock()
{
	CHoldLock lock(s_lock);
	return (s_lock == dummyLock) ? NULL : s_lock;
}

bool
CLog::setFilter(const char* maxPriority)
{
	if (maxPriority != NULL) {
		for (int i = 0; i < g_numPriority; ++i) {
			if (strcmp(maxPriority, g_priority[i]) == 0) {
				setFilter(i);
				return true;
			}
		}
		return false;
	}
	return true;
}

void
CLog::setFilter(int maxPriority)
{
	CHoldLock lock(s_lock);
	s_maxPriority = maxPriority;
}

int
CLog::getFilter()
{
	CHoldLock lock(s_lock);
	return getMaxPriority();
}

void
CLog::dummyLock(bool)
{
	// do nothing
}

int
CLog::getMaxPriority()
{
	CHoldLock lock(s_lock);

	if (s_maxPriority == -1) {
		s_maxPriority = g_defaultMaxPriority;
		setFilter(getenv("SYN_LOG_PRI"));
	}

	return s_maxPriority;
}

void
CLog::output(int priority, char* msg)
{
	assert(priority >= -1 && priority < g_numPriority);
	assert(msg != NULL);

	// insert priority label
	int n = -g_prioritySuffixLength;
	if (priority >= 0) {
		n = strlen(g_priority[priority]);
		sprintf(msg + g_maxPriorityLength - n,
							"%s:", g_priority[priority]);
		msg[g_maxPriorityLength + 1] = ' ';
	}

	// put a newline at the end
#if defined(CONFIG_PLATFORM_WIN32)
	strcat(msg + g_priorityPad, "\r\n");
#else
	strcat(msg + g_priorityPad, "\n");
#endif

	// print it
	CHoldLock lock(s_lock);
	if (s_outputter == NULL ||
		!s_outputter(priority, msg + g_maxPriorityLength - n)) {
#if defined(CONFIG_PLATFORM_WIN32)
		openConsole();
#endif
		fprintf(stderr, "%s", msg + g_maxPriorityLength - n);
	}
}

char*
CLog::vsprint(int pad, char* buffer, int len, const char* fmt, va_list args)
{
	assert(len > 0);

	// try writing to input buffer
	int n;
	if (len >= pad) {
		n = vsnprintf(buffer + pad, len - pad, fmt, args);
		if (n != -1 && n <= len - pad + g_newlineLength)
			return buffer;
	}

	// start allocating buffers until we write the whole string
	buffer = NULL;
	do {
		delete[] buffer;
		len *= 2;
		buffer = new char[len + pad];
		n = vsnprintf(buffer + pad, len - pad, fmt, args);
	} while (n == -1 || n > len - pad + g_newlineLength);

	return buffer;
}

#if defined(CONFIG_PLATFORM_WIN32)

static DWORD			s_thread = 0;

static
BOOL WINAPI
CLogSignalHandler(DWORD)
{
	// terminate cleanly and skip remaining handlers
	PostThreadMessage(s_thread, WM_QUIT, 0, 0);
	return TRUE;
}

void
CLog::openConsole()
{
	static bool s_hasConsole = false;

	// ignore if already created
	if (s_hasConsole)
		return;

	// remember the current thread.  when we get a ctrl+break or the
	// console is closed we'll post WM_QUIT to this thread to shutdown
	// cleanly.
	// note -- win95/98/me are broken and will not receive a signal
	// when the console is closed nor during logoff or shutdown,
	// see microsoft articles Q130717 and Q134284.  we could work
	// around this in a painful way using hooks and hidden windows
	// (as apache does) but it's not worth it.  the app will still
	// quit, just not cleanly.  users in-the-know can use ctrl+c.
	s_thread = GetCurrentThreadId();

	// open a console
	if (!AllocConsole())
		return;

	// get the handle for error output
	HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);

	// prep console.  windows 95 and its ilk have braindead
	// consoles that can't even resize independently of the
	// buffer size.  use a 25 line buffer for those systems.
	OSVERSIONINFO osInfo;
	COORD size = { 80, 1000 };
	osInfo.dwOSVersionInfoSize = sizeof(osInfo);
	if (GetVersionEx(&osInfo) &&
		osInfo.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
		size.Y = 25;
	SetConsoleScreenBufferSize(herr, size);
	SetConsoleTextAttribute(herr,
							FOREGROUND_RED |
							FOREGROUND_GREEN |
							FOREGROUND_BLUE);
	SetConsoleCtrlHandler(CLogSignalHandler, TRUE);

	// reopen stderr to point at console
	freopen("con", "w", stderr);
	s_hasConsole = true;
}

#endif
