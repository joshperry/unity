#include "CXWindowsScreen.h"
#include "CThread.h"
#include "CLock.h"
#include "TMethodJob.h"
#include "CLog.h"
#include "CString.h"
#include <string.h>
#include <assert.h>
#include <X11/X.h>
#include <X11/Xatom.h>
#include <set>

//
// CXWindowsScreen
//

static const Atom kClipboardSelection = XA_PRIMARY;
static const UInt32 kMaxRequestSize = 4096;

CXWindowsScreen::CXWindowsScreen() :
								m_display(NULL),
								m_root(None),
								m_w(0), m_h(0),
								m_stop(false)
{
	// do nothing
}

CXWindowsScreen::~CXWindowsScreen()
{
	assert(m_display == NULL);
}

void					CXWindowsScreen::openDisplay()
{
	assert(m_display == NULL);

	// open the display
	log((CLOG_DEBUG "XOpenDisplay(%s)", "NULL"));
	m_display = XOpenDisplay(NULL);	// FIXME -- allow non-default
	if (m_display == NULL)
		throw int(5);	// FIXME -- make exception for this

	// get default screen
	m_screen = DefaultScreen(m_display);
	Screen* screen = ScreenOfDisplay(m_display, m_screen);

	// get screen size
	m_w = WidthOfScreen(screen);
	m_h = HeightOfScreen(screen);
	log((CLOG_INFO "display size: %dx%d", m_w, m_h));

	// get the root window
	m_root = RootWindow(m_display, m_screen);

	// get some atoms
	m_atomTargets      = XInternAtom(m_display, "TARGETS", False);
	m_atomMultiple     = XInternAtom(m_display, "MULTIPLE", False);
	m_atomTimestamp    = XInternAtom(m_display, "TIMESTAMP", False);
	m_atomAtom         = XInternAtom(m_display, "ATOM", False);
	m_atomAtomPair     = XInternAtom(m_display, "ATOM_PAIR", False);
	m_atomInteger      = XInternAtom(m_display, "INTEGER", False);
	m_atomData         = XInternAtom(m_display, "DESTINATION", False);
	m_atomINCR         = XInternAtom(m_display, "INCR", False);
	m_atomString       = XInternAtom(m_display, "STRING", False);
	m_atomText         = XInternAtom(m_display, "TEXT", False);
	m_atomCompoundText = XInternAtom(m_display, "COMPOUND_TEXT", False);

	// let subclass prep display
	onOpenDisplay();
}

void					CXWindowsScreen::closeDisplay()
{
	assert(m_display != NULL);

	// let subclass close down display
	onCloseDisplay();

	// clear out the clipboard request lists
	for (CRequestMap::iterator index = m_requests.begin();
								index != m_requests.end(); ++index) {
		CRequestList* list = index->second;
		for (CRequestList::iterator index2 = list->begin();
								index2 != list->end(); ++index2) {
			delete *index2;
		}
		delete list;
	}
	m_requests.clear();

	// close the display
	XCloseDisplay(m_display);
	m_display = NULL;
	log((CLOG_DEBUG "closed display"));
}

int						CXWindowsScreen::getScreen() const
{
	assert(m_display != NULL);
	return m_screen;
}

Window					CXWindowsScreen::getRoot() const
{
	assert(m_display != NULL);
	return m_root;
}

void					CXWindowsScreen::getScreenSize(
								SInt32* w, SInt32* h) const
{
	assert(m_display != NULL);
	assert(w != NULL && h != NULL);

	*w = m_w;
	*h = m_h;
}

Cursor					CXWindowsScreen::createBlankCursor() const
{
	// this seems just a bit more complicated than really necessary

	// get the closet cursor size to 1x1
	unsigned int w, h;
	XQueryBestCursor(m_display, m_root, 1, 1, &w, &h);

	// make bitmap data for cursor of closet size.  since the cursor
	// is blank we can use the same bitmap for shape and mask:  all
	// zeros.
	const int size = ((w + 7) >> 3) * h;
	char* data = new char[size];
	memset(data, 0, size);

	// make bitmap
	Pixmap bitmap = XCreateBitmapFromData(m_display, m_root, data, w, h);

	// need an arbitrary color for the cursor
	XColor color;
	color.pixel = 0;
	color.red   = color.green = color.blue = 0;
	color.flags = DoRed | DoGreen | DoBlue;

	// make cursor from bitmap
	Cursor cursor = XCreatePixmapCursor(m_display, bitmap, bitmap,
								&color, &color, 0, 0);

	// don't need bitmap or the data anymore
	delete[] data;
	XFreePixmap(m_display, bitmap);

	return cursor;
}

bool					CXWindowsScreen::getEvent(XEvent* xevent) const
{
	// wait for an event in a cancellable way and don't lock the
	// display while we're waiting.
	m_mutex.lock();
	while (!m_stop && XPending(m_display) == 0) {
		m_mutex.unlock();
		CThread::sleep(0.05);
		m_mutex.lock();
	}
	if (m_stop) {
		m_mutex.unlock();
		return false;
	}
	else {
		XNextEvent(m_display, xevent);
		m_mutex.unlock();
		return true;
	}
}

void					CXWindowsScreen::doStop()
{
	CLock lock(&m_mutex);
	m_stop = true;
}

bool					CXWindowsScreen::lostClipboard(
								Atom selection, Time timestamp)
{
	if (selection == kClipboardSelection) {
		// note the time
		CLock lock(&m_mutex);
		m_lostClipboard = timestamp;
		log((CLOG_INFO "lost clipboard ownership at %d", timestamp));
		return true;
	}
	return false;
}

bool					CXWindowsScreen::setDisplayClipboard(
								const IClipboard* clipboard,
								Window requestor, Time timestamp)
{
	CLock lock(&m_mutex);

	XSetSelectionOwner(m_display, kClipboardSelection, requestor, timestamp);
	if (XGetSelectionOwner(m_display, kClipboardSelection) == requestor) {
		// we got the selection
		log((CLOG_INFO "grabbed clipboard at %d", timestamp));
		m_gotClipboard = timestamp;

		if (clipboard != NULL) {
			// save clipboard to serve requests
			CClipboard::copy(&m_clipboard, clipboard);
		}
		else {
			// clear clipboard
			if (m_clipboard.open()) {
				m_clipboard.close();
			}
		}

		return true;
	}

	return false;
}

void					CXWindowsScreen::getDisplayClipboard(
								IClipboard* clipboard,
								Window requestor, Time timestamp) const
{
	assert(clipboard != NULL);
	assert(requestor != None);

	// clear the clipboard object
	if (!clipboard->open())
		return;

	// block others from using the display while we get the clipboard.
	// in particular, this prevents the event thread from stealing the
	// selection notify event we're expecting.
	CLock lock(&m_mutex);
	Atom selection = kClipboardSelection;

	// ask the selection for all the formats it has.  some owners return
	// the TARGETS atom and some the ATOM atom when TARGETS is requested.
	Atom format;
	CString targets;
	if (getDisplayClipboard(selection, m_atomTargets,
								requestor, timestamp, &format, &targets) &&
		(format == m_atomTargets || format == XA_ATOM)) {
		// get each target (that we can interpret).  some owners return
		// some targets multiple times in the list so don't try to get
		// those multiple times.
		const Atom* targetAtoms = reinterpret_cast<const Atom*>(targets.data());
		const SInt32 numTargets = targets.size() / sizeof(Atom);
		std::set<IClipboard::EFormat> clipboardFormats;
		std::set<Atom> targets;
		log((CLOG_INFO "getting selection with %d targets", numTargets));
		for (SInt32 i = 0; i < numTargets; ++i) {
			Atom format = targetAtoms[i];
			log((CLOG_DEBUG " source target %d", format));

			// skip already handled targets
			if (targets.count(format) > 0) {
				log((CLOG_DEBUG "  skipping handled target %d", format));
				continue;
			}

			// mark this target as done
			targets.insert(format);

			// determine the expected clipboard format
			IClipboard::EFormat expectedFormat = getFormat(format);

			// if we can use the format and we haven't already retrieved
			// it then get it
			if (expectedFormat == IClipboard::kNumFormats) {
				log((CLOG_DEBUG "  no format for target", format));
				continue;
			}
			if (clipboardFormats.count(expectedFormat) > 0) {
				log((CLOG_DEBUG "  skipping handled format %d", expectedFormat));
				continue;
			}

			CString data;
			if (!getDisplayClipboard(selection, format,
							requestor, timestamp, &format, &data)) {
				log((CLOG_DEBUG "  no data for target", format));
				continue;
			}

			// use the actual format, not the expected
			IClipboard::EFormat actualFormat = getFormat(format);
			if (actualFormat == IClipboard::kNumFormats) {
				log((CLOG_DEBUG "  no format for target", format));
				continue;
			}
			if (clipboardFormats.count(actualFormat) > 0) {
				log((CLOG_DEBUG "  skipping handled format %d", actualFormat));
				continue;
			}

			// add to clipboard and note we've done it
			clipboard->add(actualFormat, data);
			clipboardFormats.insert(actualFormat);
			log((CLOG_INFO "  added format %d for target %d", actualFormat, format));
		}
	}
	else {
		// non-ICCCM conforming selection owner.  try TEXT format.
		// FIXME
		log((CLOG_DEBUG "selection doesn't support TARGETS, format is %d", format));
	}

	// done with clipboard
	clipboard->close();
}

bool					CXWindowsScreen::getDisplayClipboard(
								Atom selection, Atom type,
								Window requestor, Time timestamp,
								Atom* outputType, CString* outputData) const
{
	assert(outputType != NULL);
	assert(outputData != NULL);

	// delete data property
	XDeleteProperty(m_display, requestor, m_atomData);

	// request data conversion
	XConvertSelection(m_display, selection, type,
								m_atomData, requestor, timestamp);

	// wait for the selection notify event.  can't just mask out other
	// events because X stupidly doesn't provide a mask for selection
	// events, so we use a predicate to find our event.
	XEvent xevent;
	while (XCheckIfEvent(m_display, &xevent,
								&CXWindowsScreen::findSelectionNotify,
								(XPointer)&requestor) != True) {
		// wait a bit
		CThread::sleep(0.05);
	}
	assert(xevent.type                 == SelectionNotify);
	assert(xevent.xselection.requestor == requestor);

	// make sure the transfer worked
	Atom property = xevent.xselection.property;
	if (property == None) {
		// cannot convert
		*outputType = type;
		log((CLOG_DEBUG "selection conversion failed for %d", type));
		return false;
	}

	// get the data and discard the property
	SInt32 datumSize;
	CString data;
	bool okay = getData(requestor, property, outputType, &datumSize, &data);
	XDeleteProperty(m_display, requestor, property);

	// fail if we couldn't get the data
	if (!okay) {
		log((CLOG_DEBUG "can't get data for selection format %d", type));
		return false;
	}

	// handle INCR type specially.  it means we'll be receiving the data
	// piecemeal so we just loop until we've collected all the data.
	if (*outputType == m_atomINCR) {
		log((CLOG_DEBUG "selection data for format %d is incremental", type));
		// the data is a lower bound on the amount of data to be
		// transferred.  use it as a hint to size our buffer.
		UInt32 size;
		switch (datumSize) {
		  case 8:
			size = *(reinterpret_cast<const UInt8*>(data.data()));
			break;

		  case 16:
			size = *(reinterpret_cast<const UInt16*>(data.data()));
			break;

		  case 32:
			size = *(reinterpret_cast<const UInt32*>(data.data()));
			break;

		  default:
			assert(0 && "invalid datum size");
		}

		// empty the buffer and reserve the lower bound
		data.erase();
		data.reserve(size);

		// look for property notify events with the following
		CPropertyNotifyInfo filter;
		filter.m_window   = requestor;
		filter.m_property = property;

		// now enter the INCR loop
		bool error = false;
		*outputType = (Atom)0;
		for (;;) {
			// wait for more data
			while (XCheckIfEvent(m_display, &xevent,
								&CXWindowsScreen::findPropertyNotify,
								(XPointer)&filter) != True) {
				// wait a bit
				CThread::sleep(0.05);
			}
			assert(xevent.type             == PropertyNotify);
			assert(xevent.xproperty.window == requestor);
			assert(xevent.xproperty.atom   == property);

			// get the additional data then delete the property to
			// ask the clipboard owner for the next chunk.
			Atom newType;
			CString newData;
			okay = getData(requestor, property, &newType, NULL, &newData);
			XDeleteProperty(m_display, requestor, property);

			// transfer has failed if we can't get the data
			if (!okay)
				error = true;

			// a zero length property means we got the last chunk
			if (newData.size() == 0)
				break;

			// if this is the first chunk then save the type.  otherwise
			// note that the new type is the same as the first chunk's
			// type.  if they're not the the clipboard owner is busted
			// but we have to continue the transfer because there's no
			// way to cancel it.
			if (*outputType == (Atom)0)
				*outputType = newType;
			else if (*outputType != newType)
				error = true;

			// append the data
			data += newData;
		}

		// if there was an error we could say the transferred failed
		// but we'll be liberal in what we accept.
		if (error) {
			log((CLOG_WARN "ICCCM violation by clipboard owner"));
//			return false;
		}
	}

	*outputData = data;
	return true;
}

bool					CXWindowsScreen::getData(
								Window window, Atom property,
								Atom* type, SInt32* datumSize,
								CString* data) const
{
	assert(type != NULL);
	assert(data != NULL);

	// clear out any existing data
	data->erase();

	// read the property
	long offset = 0;
	long length = 8192 / 4;
	for (;;) {
		// get more data
		int actualDatumSize;
		unsigned long numItems, bytesLeft;
		unsigned char* rawData;
		const int result = XGetWindowProperty(m_display, window, property,
								offset, length, False, AnyPropertyType,
								type, &actualDatumSize,
								&numItems, &bytesLeft,
								&rawData);
		if (result != Success) {
			// failed
			return false;
		}

		// save datum size
		if (datumSize != NULL)
			*datumSize = (SInt32)actualDatumSize;
		const SInt32 bytesPerDatum = (SInt32)actualDatumSize / 8;

		// advance read pointer.  since we can only read at offsets that
		// are multiples of 4 byte we take care to write multiples of 4
		// bytes to data, except when we've retrieved the last chunk.
		SInt32 quadCount = (numItems * bytesPerDatum) / 4;
		offset += quadCount;

		// append data
		if (bytesLeft == 0)
			data->append((char*)rawData, bytesPerDatum * numItems);
		else
			data->append((char*)rawData, 4 * quadCount);

		// done with returned data
		XFree(rawData);

		// done if no data is left
		if (bytesLeft == 0)
			return true;
	}
}

IClipboard::EFormat		CXWindowsScreen::getFormat(Atom src) const
{
	// FIXME -- handle more formats (especially mime-type-like formats
	// and various character encodings like unicode).
	if (src == XA_STRING ||
		src == m_atomText ||
		src == m_atomCompoundText)
		return IClipboard::kText;
	return IClipboard::kNumFormats;
}

Bool					CXWindowsScreen::findSelectionNotify(
								Display*, XEvent* xevent, XPointer arg)
{
	Window requestor = *reinterpret_cast<Window*>(arg);
	return (xevent->type                 == SelectionNotify &&
			xevent->xselection.requestor == requestor) ? True : False;
}

Bool					CXWindowsScreen::findPropertyNotify(
								Display*, XEvent* xevent, XPointer arg)
{
	CPropertyNotifyInfo* filter = reinterpret_cast<CPropertyNotifyInfo*>(arg);
	return (xevent->type             == PropertyNotify &&
			xevent->xproperty.window == filter->m_window &&
			xevent->xproperty.atom   == filter->m_property &&
			xevent->xproperty.state  == PropertyNewValue) ? True : False;
}

void					CXWindowsScreen::addClipboardRequest(
								Window /*owner*/, Window requestor,
								Atom selection, Atom target,
								Atom property, Time time)
{
	// we can only own kClipboardSelection
	if (selection != kClipboardSelection) {
		return;
	}

	// mutex the display
	CLock lock(&m_mutex);
	bool success = false;

	// a request for multiple targets is special
	if (target == m_atomMultiple) {
		// add a multiple request
		if (property != None) {
			success = sendClipboardMultiple(requestor, property, time);
		}
	}
	else {
		// handle remaining request formats
		success = sendClipboardData(requestor, target, property, time);
	}

	// send success or failure
	sendNotify(requestor, target, success ? property : None, time);
}

void					CXWindowsScreen::processClipboardRequest(
								Window requestor,
								Atom property, Time /*time*/)
{
	CLock lock(&m_mutex);

	// find the request list
	CRequestMap::iterator index = m_requests.find(requestor);
	if (index == m_requests.end()) {
		return;
	}
	CRequestList* list = index->second;
	assert(list != NULL);

	// find the property in the list
	CRequestList::iterator index2;
	for (index2 = list->begin(); index2 != list->end(); ++index2) {
		if ((*index2)->m_property == property) {
			break;
		}
	}
	if (index2 == list->end()) {
		log((CLOG_WARN "received property event on unexpected property"));
		return;
	}
	CClipboardRequest* request = *index2;
	assert(request != NULL);

	// compute amount of data to send
	assert(request->m_sent <= request->m_data.size());
	UInt32 count = request->m_data.size() - request->m_sent;
	if (count > kMaxRequestSize) {
		// limit maximum chunk size
		count = kMaxRequestSize;

		// make it a multiple of the size
		count &= ~((request->m_size >> 3) - 1);
	}

	// send more data
	// FIXME -- handle Alloc errors (by returning false)
	XChangeProperty(m_display, request->m_requestor, request->m_property,
								request->m_type, request->m_size,
								PropModeReplace,
								reinterpret_cast<const unsigned char*>(
									request->m_data.data() + request->m_sent),
								count / (request->m_size >> 3));

	// account for sent data
	request->m_sent += count;

	// if we sent zero bytes then we're done sending this data.  remove
	// it from the list and, if the list is empty, the list from the
	// map.  also stop watching the requestor for events.
	if (count == 0) {
		list->erase(index2);
		delete request;
		if (list->empty()) {
			m_requests.erase(index);
			delete list;
		}
		XSelectInput(m_display, requestor, NoEventMask);
	}
}

void					CXWindowsScreen::destroyClipboardRequest(
								Window requestor)
{
	CLock lock(&m_mutex);

	// find the request list
	CRequestMap::iterator index = m_requests.find(requestor);
	if (index == m_requests.end()) {
		return;
	}
	CRequestList* list = index->second;
	assert(list != NULL);

	// destroy every request in the list
	for (CRequestList::iterator index2 = list->begin();
								index2 != list->end(); ++index2) {
		delete *index2;
	}

	// remove and destroy the list
	m_requests.erase(index);
	delete list;

	// note -- we don't stop watching the window for events because
	// we're called in response to the window being destroyed.
}

bool					CXWindowsScreen::sendClipboardData(
								Window requestor, Atom target,
								Atom property, Time time)
{
	if (target == m_atomTargets) {
		return sendClipboardTargets(requestor, property, time);
	}
	else if (target == m_atomTimestamp) {
		return sendClipboardTimestamp(requestor, property, time);
	}
	else {
		// compute the type and size for the requested target and
		// convert the data from the clipboard.
		Atom type = None;
		int size = 0;
		CString data;
		if (target == m_atomText || target == m_atomString) {
			if (m_clipboard.has(IClipboard::kText)) {
				type = m_atomString;
				size = 8;
				data = m_clipboard.get(IClipboard::kText);
			}
		}

		// fail if we don't recognize or can't handle the target
		if (type == None || size == 0) {
			return false;
		}

		if (data.size() > kMaxRequestSize) {
			log((CLOG_DEBUG "handling clipboard request for %d as INCR", target));

			// get the appropriate list, creating it if necessary
			CRequestList* list = m_requests[requestor];
			if (list == NULL) {
				list = new CRequestList;
				m_requests[requestor] = list;
			}

			// create request object
			CClipboardRequest* request = new CClipboardRequest;
			request->m_data      = data;
			request->m_sent      = 0;
			request->m_requestor = requestor;
			request->m_property  = property;
			request->m_type      = type;
			request->m_size      = size;

			// add request to request list
			list->push_back(request);

			// start watching requestor for property changes and
			// destruction
			XSelectInput(m_display, requestor, StructureNotifyMask |
												PropertyChangeMask);

			// FIXME -- handle Alloc errors (by returning false)
			// set property to INCR
			const UInt32 zero = 0;
			XChangeProperty(m_display, requestor, property,
								m_atomINCR, 8 * sizeof(zero),
								PropModeReplace,
								reinterpret_cast<const unsigned char*>(&zero),
								1);
		}
		else {
			log((CLOG_DEBUG "handling clipboard request for %d", target));

			// FIXME -- handle Alloc errors (by returning false)
			XChangeProperty(m_display, requestor, property,
								type, size,
								PropModeReplace,
								reinterpret_cast<const unsigned char*>(data.data()),
								data.size() / (size >> 3));
		}
		return true;
	}
	return false;
}

bool					CXWindowsScreen::sendClipboardMultiple(
								Window requestor,
								Atom property, Time time)
{
	log((CLOG_DEBUG "handling clipboard request for MULTIPLE"));

	// get the list of requested formats
	Atom type;
	SInt32 size;
	CString data;
	getData(requestor, property, &type, &size, &data);
	if (type != m_atomAtomPair) {
		// unexpected data type
		return false;
	}

	// check each format, replacing ones we can't do with None.  set
	// the property for each to the requested data (for small requests)
	// or INCR (for large requests).
	bool success = false;
	bool updated = false;
	UInt32 numRequests = data.size() / (2 * sizeof(Atom));
	for (UInt32 index = 0; index < numRequests; ++index) {
		// get request info
		const Atom* request = reinterpret_cast<const Atom*>(data.data());
		const Atom target   = request[2 * index + 0];
		const Atom property = request[2 * index + 1];

		// handle target
		if (property != None) {
			if (!sendClipboardData(requestor, target, property, time)) {
				// couldn't handle target.  change property to None.
				const Atom none = None;
				data.replace((2 * index + 1) * sizeof(Atom), sizeof(Atom),
								reinterpret_cast<const char*>(&none),
								sizeof(none));
				updated = true;
			}
			else {
				success = true;
			}
		}
	}

	// update property if we changed it
	if (updated) {
		// FIXME -- handle Alloc errors (by returning false)
		XChangeProperty(m_display, requestor, property,
								m_atomAtomPair, 8 * sizeof(Atom),
								PropModeReplace,
								reinterpret_cast<const unsigned char*>(data.data()),
								data.length());
	}

	// send notify if any format was successful
	if (success) {
		sendNotify(requestor, m_atomMultiple, success ? property : None, time);
		return true;
	}

	return false;
}

bool					CXWindowsScreen::sendClipboardTargets(
								Window requestor,
								Atom property, Time /*time*/)
{
	log((CLOG_DEBUG "handling request for TARGETS"));

	// count the number of targets, plus TARGETS and MULTIPLE
	SInt32 numTargets = 2;
	if (m_clipboard.has(IClipboard::kText)) {
		numTargets += 2;
	}

	// construct response
	Atom* response = new Atom[numTargets];
	SInt32 count = 0;
	response[count++] = m_atomTargets;
	response[count++] = m_atomMultiple;
	if (m_clipboard.has(IClipboard::kText)) {
		response[count++] = m_atomText;
		response[count++] = m_atomString;
	}

	// send response (we assume we can transfer the entire list at once)
	// FIXME -- handle Alloc errors (by returning false)
	XChangeProperty(m_display, requestor, property,
								m_atomAtom, 8 * sizeof(Atom),
								PropModeReplace,
								reinterpret_cast<unsigned char*>(response),
								count);

	// done with our response
	delete[] response;

	return true;
}

bool					CXWindowsScreen::sendClipboardTimestamp(
								Window requestor,
								Atom property, Time /*time*/)
{
	log((CLOG_DEBUG "handling clipboard request for TIMESTAMP"));

	// FIXME -- handle Alloc errors (by returning false)
	XChangeProperty(m_display, requestor, property,
								m_atomInteger, 8 * sizeof(m_gotClipboard),
								PropModeReplace,
								reinterpret_cast<unsigned char*>(&m_gotClipboard),
								1);
	return true;
}

void					CXWindowsScreen::sendNotify(
								Window requestor, Atom target,
								Atom property, Time time)
{
	XEvent event;
	event.xselection.type      = SelectionNotify;
	event.xselection.display   = m_display;
	event.xselection.requestor = requestor;
	event.xselection.selection = kClipboardSelection;
	event.xselection.target    = target;
	event.xselection.property  = property;
	event.xselection.time      = time;
	XSendEvent(m_display, requestor, False, 0, &event);
}


//
// CXWindowsScreen::CDisplayLock
//

CXWindowsScreen::CDisplayLock::CDisplayLock(const CXWindowsScreen* screen) :
								m_mutex(&screen->m_mutex),
								m_display(screen->m_display)
{
	assert(m_display != NULL);

	m_mutex->lock();
}

CXWindowsScreen::CDisplayLock::~CDisplayLock()
{
	m_mutex->unlock();
}

CXWindowsScreen::CDisplayLock::operator Display*() const
{
	return m_display;
}