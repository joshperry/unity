#include "CStopwatch.h"

//
// CStopwatch
//

CStopwatch::CStopwatch(bool triggered) :
								m_mark(0.0),
								m_triggered(triggered),
								m_stopped(triggered)
{
	if (!triggered)
		m_mark = getClock();
}

CStopwatch::~CStopwatch()
{
	// do nothing
}

double					CStopwatch::reset()
{
	if (m_stopped) {
		const double dt = m_mark;
		m_mark = 0.0;
		return dt;
	}
	else {
		const double t	= getClock();
		const double dt = t - m_mark;
		m_mark = t;
		return dt;
	}
}

void					CStopwatch::stop()
{
	if (m_stopped)
		return;

	// save the elapsed time
	m_mark	  = getClock() - m_mark;
	m_stopped = true;
}

void					CStopwatch::start()
{
	m_triggered = false;
	if (!m_stopped)
		return;

	// set the mark such that it reports the time elapsed at stop()
	m_mark	  = getClock() - m_mark;
	m_stopped = false;
}

void					CStopwatch::setTrigger()
{
	stop();
	m_triggered = true;
}

double					CStopwatch::getTime()
{
	if (m_triggered) {
		const double dt = m_mark;
		start();
		return dt;
	}
	if (m_stopped)
		return m_mark;
	return getClock() - m_mark;
}

CStopwatch::operator double()
{
	return getTime();
}

bool					CStopwatch::isStopped() const
{
	return m_stopped;
}

double					CStopwatch::getTime() const
{
	if (m_stopped)
		return m_mark;
	return getClock() - m_mark;
}

CStopwatch::operator double() const
{
	return getTime();
}

#if defined(CONFIG_PLATFORM_UNIX)

#include <sys/time.h>

double					CStopwatch::getClock() const
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return (double)t.tv_sec + 1.0e-6 * (double)t.tv_usec;
}

#endif // CONFIG_PLATFORM_UNIX

#if defined(CONFIG_PLATFORM_WIN32)

// avoid getting a lot a crap from mmsystem.h that we don't need
#define MMNODRV         // Installable driver support
#define MMNOSOUND       // Sound support
#define MMNOWAVE        // Waveform support
#define MMNOMIDI        // MIDI support
#define MMNOAUX         // Auxiliary audio support
#define MMNOMIXER       // Mixer support
#define MMNOJOY         // Joystick support
#define MMNOMCI         // MCI support
#define MMNOMMIO        // Multimedia file I/O support
#define MMNOMMSYSTEM    // General MMSYSTEM functions

#include <windows.h>
#include <mmsystem.h>

typedef WINMMAPI DWORD (WINAPI *PTimeGetTime)(void);

static double			s_freq = 0.0;
static HINSTANCE		s_mmInstance = NULL;
static PTimeGetTime		s_tgt = NULL;

//
// initialize local variables
//

class CStopwatchInit {
  public:
	CStopwatchInit();
	~CStopwatchInit();
};
static CStopwatchInit	s_init;

CStopwatchInit::CStopwatchInit()
{
	LARGE_INTEGER freq;
	if (QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) {
		s_freq = 1.0 / static_cast<double>(freq.QuadPart);
	}
	else {
		// load winmm.dll and get timeGetTime
		s_mmInstance = LoadLibrary("winmm");
		if (s_mmInstance)
			s_tgt = (PTimeGetTime)GetProcAddress(s_mmInstance, "timeGetTime");
	}
}

CStopwatchInit::~CStopwatchInit()
{
	if (s_mmInstance)
		FreeLibrary(reinterpret_cast<HMODULE>(s_mmInstance));
}

double					CStopwatch::getClock() const
{
	// get time.  we try three ways, in order of descending precision
	if (s_freq != 0.0) {
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		return s_freq * static_cast<double>(c.QuadPart);
	}
	else if (s_tgt) {
		return 0.001 * static_cast<double>(s_tgt());
	}
	else {
		return 0.001 * static_cast<double>(GetTickCount());
	}
}

#endif // CONFIG_PLATFORM_WIN32