#include "CXWindowsClipboard.h"
#include "CXWindowsClipboardTextConverter.h"
#include "CXWindowsClipboardUCS2Converter.h"
#include "CXWindowsClipboardUTF8Converter.h"
#include "CXWindowsUtil.h"
#include "CThread.h"
#include "CLog.h"
#include "CStopwatch.h"
#include "stdvector.h"
#include <cstdio>
#include <X11/Xatom.h>

//
// CXWindowsClipboard
//

CXWindowsClipboard::CXWindowsClipboard(Display* display,
				Window window, ClipboardID id) :
	m_display(display),
	m_window(window),
	m_id(id),
	m_open(false),
	m_time(0),
	m_owner(false),
	m_timeOwned(0),
	m_timeLost(0)
{
	// get some atoms
	m_atomTargets         = XInternAtom(m_display, "TARGETS", False);
	m_atomMultiple        = XInternAtom(m_display, "MULTIPLE", False);
	m_atomTimestamp       = XInternAtom(m_display, "TIMESTAMP", False);
	m_atomAtomPair        = XInternAtom(m_display, "ATOM_PAIR", False);
	m_atomData            = XInternAtom(m_display, "CLIP_TEMPORARY", False);
	m_atomINCR            = XInternAtom(m_display, "INCR", False);
	m_atomMotifClipLock   = XInternAtom(m_display, "_MOTIF_CLIP_LOCK", False);
	m_atomMotifClipHeader = XInternAtom(m_display, "_MOTIF_CLIP_HEADER", False);
	m_atomMotifClipAccess = XInternAtom(m_display,
								"_MOTIF_CLIP_LOCK_ACCESS_VALID", False);
	m_atomGDKSelection    = XInternAtom(m_display, "GDK_SELECTION", False);

	// set selection atom based on clipboard id
	switch (id) {
	case kClipboardClipboard:
		m_selection = XInternAtom(m_display, "CLIPBOARD", False);
		break;

	case kClipboardSelection:
	default:
		m_selection = XA_PRIMARY;
		break;
	}

	// add converters, most desired first
	m_converters.push_back(new CXWindowsClipboardUTF8Converter(m_display,
								"text/plain;charset=UTF-8"));
	m_converters.push_back(new CXWindowsClipboardUTF8Converter(m_display,
								"UTF8_STRING"));
	m_converters.push_back(new CXWindowsClipboardUCS2Converter(m_display,
								"text/plain;charset=ISO-10646-UCS-2"));
	m_converters.push_back(new CXWindowsClipboardUCS2Converter(m_display,
								"text/unicode"));
	m_converters.push_back(new CXWindowsClipboardTextConverter(m_display,
								"text/plain"));
	m_converters.push_back(new CXWindowsClipboardTextConverter(m_display,
								"STRING"));

	// we have no data
	clearCache();
}

CXWindowsClipboard::~CXWindowsClipboard()
{
	clearReplies();
	clearConverters();
}

void
CXWindowsClipboard::lost(Time time)
{
	log((CLOG_DEBUG "lost clipboard %d ownership at %d", m_id, time));
	if (m_owner) {
		m_owner    = false;
		m_timeLost = time;
		clearCache();
	}
}

void
CXWindowsClipboard::addRequest(Window owner, Window requestor,
				Atom target, ::Time time, Atom property)
{
	// must be for our window and we must have owned the selection
	// at the given time.
	bool success = false;
	if (owner == m_window) {
		log((CLOG_DEBUG1 "request for clipboard %d, target %d by 0x%08x (property=%d)", m_selection, target, requestor, property));
		if (wasOwnedAtTime(time)) {
			if (target == m_atomMultiple) {
				// add a multiple request.  property may not be None
				// according to ICCCM.
				if (property != None) {
					success = insertMultipleReply(requestor, time, property);
				}
			}
			else {
				addSimpleRequest(requestor, target, time, property);

				// addSimpleRequest() will have already handled failure
				success = true;
			}
		}
		else {
			log((CLOG_DEBUG1 "failed, not owned at time %d", time));
		}
	}

	if (!success) {
		// send failure
		log((CLOG_DEBUG1 "failed"));
		insertReply(new CReply(requestor, target, time));
	}

	// send notifications that are pending
	pushReplies();
}

bool
CXWindowsClipboard::addSimpleRequest(Window requestor,
				Atom target, ::Time time, Atom property)
{
	// obsolete requestors may supply a None property.  in
	// that case we use the target as the property to store
	// the conversion.
	if (property == None) {
		property = target;
	}

	// handle targets
	CString data;
	Atom type  = None;
	int format = 0;
	if (target == m_atomTargets) {
		type = getTargetsData(data, &format);
	}
	else if (target == m_atomTimestamp) {
		type = getTimestampData(data, &format);
	}
	else {
		IXWindowsClipboardConverter* converter = getConverter(target);
		if (converter != NULL) {
			IClipboard::EFormat clipboardFormat = converter->getFormat();
			if (m_added[clipboardFormat]) {
				try {
					data   = converter->fromIClipboard(m_data[clipboardFormat]);
					format = converter->getDataSize();
					type   = converter->getAtom();
				}
				catch (...) {
					// ignore -- cannot convert
				}
			}
		}
	}

	if (type != None) {
		// success
		log((CLOG_DEBUG1 "success"));
		insertReply(new CReply(requestor, target, time,
								property, data, type, format));
		return true;
	}
	else {
		// failure
		log((CLOG_DEBUG1 "failed"));
		insertReply(new CReply(requestor, target, time));
		return false;
	}
}

bool
CXWindowsClipboard::processRequest(Window requestor,
				::Time /*time*/, Atom property)
{
	CReplyMap::iterator index = m_replies.find(requestor);
	if (index == m_replies.end()) {
		// unknown requestor window
		return false;
	}
	log((CLOG_DEBUG1 "received property %d delete from 0x08%x", property, requestor));

	// find the property in the known requests.  it should be the
	// first property but we'll check 'em all if we have to.
	CReplyList& replies = index->second;
	for (CReplyList::iterator index2 = replies.begin();
								index2 != replies.end(); ++index2) {
		CReply* reply = *index2;
		if (reply->m_replied && reply->m_property == property) {
			// if reply is complete then remove it and start the
			// next one.
			pushReplies(index, replies, index2);
			return true;
		}
	}

	return false;
}

bool
CXWindowsClipboard::destroyRequest(Window requestor)
{
	CReplyMap::iterator index = m_replies.find(requestor);
	if (index == m_replies.end()) {
		// unknown requestor window
		return false;
	}

	// destroy all replies for this window
	clearReplies(index->second);
	m_replies.erase(index);

	// note -- we don't stop watching the window for events because
	// we're called in response to the window being destroyed.

	return true;
}

Window
CXWindowsClipboard::getWindow() const
{
	return m_window;
}

Atom
CXWindowsClipboard::getSelection() const
{
	return m_selection;
}

bool
CXWindowsClipboard::empty()
{
	assert(m_open);

	log((CLOG_DEBUG "empty clipboard %d", m_id));

	// assert ownership of clipboard
	XSetSelectionOwner(m_display, m_selection, m_window, m_time);
	if (XGetSelectionOwner(m_display, m_selection) != m_window) {
		log((CLOG_DEBUG "failed to grab clipboard %d", m_id));
		return false;
	}

	// clear all data.  since we own the data now, the cache is up
	// to date.
	clearCache();
	m_cached = true;

	// FIXME -- actually delete motif clipboard items?
	// FIXME -- do anything to motif clipboard properties?

	// save time
	m_timeOwned = m_time;
	m_timeLost  = 0;

	// we're the owner now
	m_owner = true;
	log((CLOG_DEBUG "grabbed clipboard %d", m_id));

	return true;
}

void
CXWindowsClipboard::add(EFormat format, const CString& data)
{
	assert(m_open);
	assert(m_owner);

	log((CLOG_DEBUG "add %d bytes to clipboard %d format: %d", data.size(), m_id, format));

	m_data[format]  = data;
	m_added[format] = true;

	// FIXME -- set motif clipboard item?
}

bool
CXWindowsClipboard::open(Time time) const
{
	assert(!m_open);

	log((CLOG_DEBUG "open clipboard %d", m_id));

	// assume not motif
	m_motif = false;

	// lock clipboard
	if (m_id == kClipboardClipboard) {
		if (!motifLockClipboard()) {
			return false;
		}

		// check if motif owns the selection.  unlock motif clipboard
		// if it does not.
		m_motif = motifOwnsClipboard();
		log((CLOG_DEBUG1 "motif does %sown clipboard", m_motif ? "" : "not "));
		if (!m_motif) {
			motifUnlockClipboard();
		}
	}

	// now open
	m_open = true;
	m_time = time;

	// be sure to flush the cache later if it's dirty
	m_checkCache = true;

	return true;
}

void
CXWindowsClipboard::close() const
{
	assert(m_open);

	log((CLOG_DEBUG "close clipboard %d", m_id));

	// unlock clipboard
	if (m_motif) {
		motifUnlockClipboard();
	}

	m_motif = false;
	m_open  = false;
}

IClipboard::Time
CXWindowsClipboard::getTime() const
{
	checkCache();
	return m_timeOwned;
}

bool
CXWindowsClipboard::has(EFormat format) const
{
	assert(m_open);

	fillCache();
	return m_added[format];
}

CString
CXWindowsClipboard::get(EFormat format) const
{
	assert(m_open);

	fillCache();
	return m_data[format];
}

void
CXWindowsClipboard::clearConverters()
{
	for (ConverterList::iterator index = m_converters.begin();
								index != m_converters.end(); ++index) {
		delete *index;
	}
	m_converters.clear();
}

IXWindowsClipboardConverter*
CXWindowsClipboard::getConverter(Atom target, bool onlyIfNotAdded) const
{
	IXWindowsClipboardConverter* converter = NULL;
	for (ConverterList::const_iterator index = m_converters.begin();
								index != m_converters.end(); ++index) {
		converter = *index;
		if (converter->getAtom() == target) {
			break;
		}
	}
	if (converter == NULL) {
		log((CLOG_DEBUG1 "  no converter for target %d", target));
		return NULL;
	}

	// optionally skip already handled targets
	if (onlyIfNotAdded) {
		if (m_added[converter->getFormat()]) {
			log((CLOG_DEBUG1 "  skipping handled format %d", converter->getFormat()));
			return NULL;
		}
	}

	return converter;
}

void
CXWindowsClipboard::checkCache() const
{
	if (!m_checkCache) {
		return;
	}
	m_checkCache = false;

	// get the time the clipboard ownership was taken by the current
	// owner.
	if (m_motif) {
		m_timeOwned = motifGetTime();
	}
	else {
		m_timeOwned = icccmGetTime();
	}

	// if we can't get the time then use the time passed to us
	if (m_timeOwned == 0) {
		m_timeOwned = m_time;
	}

	// if the cache is dirty then flush it
	if (m_timeOwned != m_cacheTime) {
		clearCache();
	}
}

void
CXWindowsClipboard::clearCache() const
{
	const_cast<CXWindowsClipboard*>(this)->doClearCache();
}

void
CXWindowsClipboard::doClearCache()
{
	m_checkCache = false;
	m_cached     = false;
	for (SInt32 index = 0; index < kNumFormats; ++index) {
		m_data[index]  = "";
		m_added[index] = false;
	}
}

void
CXWindowsClipboard::fillCache() const
{
	// get the selection data if not already cached
	checkCache();
	if (!m_cached) {
		const_cast<CXWindowsClipboard*>(this)->doFillCache();
	}
}

void
CXWindowsClipboard::doFillCache()
{
	if (m_motif) {
		motifFillCache();
	}
	else {
		icccmFillCache();
	}
	m_checkCache = false;
	m_cached     = true;
	m_cacheTime  = m_timeOwned;
}

void
CXWindowsClipboard::icccmFillCache()
{
	log((CLOG_DEBUG "ICCCM fill clipboard %d", m_id));

	// see if we can get the list of available formats from the selection.
	// if not then use a default list of formats.
	const Atom atomTargets = m_atomTargets;
	Atom target;
	CString data;
	if (!icccmGetSelection(atomTargets, &target, &data)) {
		log((CLOG_DEBUG1 "selection doesn't support TARGETS"));
		data = "";

		target = XA_STRING;
		data.append(reinterpret_cast<char*>(&target), sizeof(target));
	}

	// try each converter in order (because they're in order of
	// preference).
	const Atom* targets = reinterpret_cast<const Atom*>(data.data());
	const UInt32 numTargets = data.size() / sizeof(Atom);
	for (ConverterList::const_iterator index = m_converters.begin();
								index != m_converters.end(); ++index) {
		IXWindowsClipboardConverter* converter = *index;

		// skip already handled targets
		if (m_added[converter->getFormat()]) {
			continue;
		}

		// see if atom is in target list
		Atom target = None;
		for (UInt32 i = 0; i < numTargets; ++i) {
			if (converter->getAtom() == targets[i]) {
				target = targets[i];
				break;
			}
		}
		if (target == None) {
			continue;
		}

		// get the data
		Atom actualTarget;
		CString targetData;
		if (!icccmGetSelection(target, &actualTarget, &targetData)) {
			log((CLOG_DEBUG1 "  no data for target", target));
			continue;
		}
		if (actualTarget != target) {
			log((CLOG_DEBUG1 "  expeted (%d) and actual (%d) targets differ", target, actualTarget));
			continue;
		}

		// add to clipboard and note we've done it
		IClipboard::EFormat format = converter->getFormat();
		m_data[format]  = converter->toIClipboard(targetData);
		m_added[format] = true;
		log((CLOG_DEBUG "  added format %d for target %d", converter->getFormat(), target));
	}
}

bool
CXWindowsClipboard::icccmGetSelection(Atom target,
				Atom* actualTarget, CString* data) const
{
	assert(actualTarget != NULL);
	assert(data         != NULL);

	// request data conversion
	CICCCMGetClipboard getter(m_window, m_time, m_atomData);
	if (!getter.readClipboard(m_display, m_selection,
								target, actualTarget, data)) {
		log((CLOG_DEBUG1 "can't get data for selection target %d", target));
		logc(getter.m_error, (CLOG_WARN "ICCCM violation by clipboard owner"));
		return false;
	}
	else if (*actualTarget == None) {
		log((CLOG_DEBUG1 "selection conversion failed for target %d", target));
		return false;
	}
	return true;
}

IClipboard::Time
CXWindowsClipboard::icccmGetTime() const
{
	Atom actualTarget;
	CString data;
	if (icccmGetSelection(m_atomTimestamp, &actualTarget, &data) &&
		actualTarget == m_atomTimestamp) {
		Time time = *reinterpret_cast<const Time*>(data.data());
		log((CLOG_DEBUG1 "got ICCCM time %d", time));
		return time;
	}
	else {
		// no timestamp
		log((CLOG_DEBUG1 "can't get ICCCM time"));
		return 0;
	}
}

bool
CXWindowsClipboard::motifLockClipboard() const
{
	// fail if anybody owns the lock (even us, so this is non-recursive)
    Window lockOwner = XGetSelectionOwner(m_display, m_atomMotifClipLock);
	if (lockOwner != None) {
		log((CLOG_DEBUG1 "motif lock owner 0x%08x", lockOwner));
		return false;
	}

	// try to grab the lock
	// FIXME -- is this right?  there's a race condition here --
	// A grabs successfully, B grabs successfully, A thinks it
	// still has the grab until it gets a SelectionClear.
	Time time = CXWindowsUtil::getCurrentTime(m_display, m_window);
	XSetSelectionOwner(m_display, m_atomMotifClipLock, m_window, time);
    lockOwner = XGetSelectionOwner(m_display, m_atomMotifClipLock);
	if (lockOwner != m_window) {
		log((CLOG_DEBUG1 "motif lock owner 0x%08x", lockOwner));
		return false;
	}

	log((CLOG_DEBUG1 "locked motif clipboard"));
	return true;
}

void
CXWindowsClipboard::motifUnlockClipboard() const
{
	log((CLOG_DEBUG1 "unlocked motif clipboard"));

	// fail if we don't own the lock
	Window lockOwner = XGetSelectionOwner(m_display, m_atomMotifClipLock);
	if (lockOwner != m_window) {
		return;
	}

	// release lock
	Time time = CXWindowsUtil::getCurrentTime(m_display, m_window);
	XSetSelectionOwner(m_display, m_atomMotifClipLock, None, time);
}

bool
CXWindowsClipboard::motifOwnsClipboard() const
{
	// get the current selection owner
	// FIXME -- this can't be right.  even if the window is destroyed
	// Motif will still have a valid clipboard.  how can we tell if
	// some other client owns CLIPBOARD?
	Window owner = XGetSelectionOwner(m_display, m_selection);
	if (owner == None) {
		return false;
	}

	// get the Motif clipboard header property from the root window
	Atom target;
	SInt32 format;
	CString data;
	Window root     = RootWindow(m_display, DefaultScreen(m_display));
	if (!CXWindowsUtil::getWindowProperty(m_display, root,
								m_atomMotifClipHeader,
								&data, &target, &format, False)) {
		return false;
	}
	if (target != m_atomMotifClipHeader) {
		return false;
	}

	// check the owner window against the current clipboard owner
	const CMotifClipHeader* header =
						reinterpret_cast<const CMotifClipHeader*>(data.data());
	if (data.size() >= sizeof(CMotifClipHeader) &&
		header->m_id == kMotifClipHeader) {
		if (header->m_selectionOwner == owner) {
			return true;
		}
	}

	return false;
}

void
CXWindowsClipboard::motifFillCache()
{
	log((CLOG_DEBUG "Motif fill clipboard %d", m_id));

	// get the Motif clipboard header property from the root window
	Atom target;
	SInt32 format;
	CString data;
	Window root = RootWindow(m_display, DefaultScreen(m_display));
	if (!CXWindowsUtil::getWindowProperty(m_display, root,
								m_atomMotifClipHeader,
								&data, &target, &format, False)) {
		return;
	}
	if (target != m_atomMotifClipHeader) {
		return;
	}

	// check that the header is okay
	const CMotifClipHeader* header =
						reinterpret_cast<const CMotifClipHeader*>(data.data());
	if (data.size() < sizeof(CMotifClipHeader) ||
		header->m_id != kMotifClipHeader ||
		header->m_numItems < 1) {
		return;
	}

	// get the Motif item property from the root window
	char name[18 + 20];
	sprintf(name, "_MOTIF_CLIP_ITEM_%d", header->m_item);
    Atom atomItem = XInternAtom(m_display, name, False);
	data = "";
	if (!CXWindowsUtil::getWindowProperty(m_display, root,
								atomItem, &data,
								&target, &format, False)) {
		return;
	}
	if (target != atomItem) {
		return;
	}

	// check that the item is okay
	const CMotifClipItem* item =
					reinterpret_cast<const CMotifClipItem*>(data.data());
	if (data.size() < sizeof(CMotifClipItem) ||
		item->m_id != kMotifClipItem ||
		item->m_numFormats < 1) {
		return;
	}

	// get the available formats
	std::vector<CMotifClipFormat> motifFormats;
	for (SInt32 i = 0; i < item->m_numFormats; ++i) {
		// get Motif format property from the root window
		sprintf(name, "_MOTIF_CLIP_ITEM_%d", item->m_formats[i]);
    	Atom atomFormat = XInternAtom(m_display, name, False);
		CString data;
		if (!CXWindowsUtil::getWindowProperty(m_display, root,
									atomFormat, &data,
									&target, &format, False)) {
			continue;
		}
		if (target != atomFormat) {
			continue;
		}

		// check that the format is okay
		const CMotifClipFormat* motifFormat =
						reinterpret_cast<const CMotifClipFormat*>(data.data());
		if (data.size() < sizeof(CMotifClipFormat) ||
			motifFormat->m_id != kMotifClipFormat ||
			motifFormat->m_length < 0 ||
			motifFormat->m_type == None) {
			continue;
		}

		// save it
		motifFormats.push_back(*motifFormat);
	}
	const UInt32 numMotifFormats = motifFormats.size();



	// try each converter in order (because they're in order of
	// preference).
	const Atom* targets = reinterpret_cast<const Atom*>(data.data());
	const UInt32 numTargets = data.size() / sizeof(Atom);
	for (ConverterList::const_iterator index = m_converters.begin();
								index != m_converters.end(); ++index) {
		IXWindowsClipboardConverter* converter = *index;

		// skip already handled targets
		if (m_added[converter->getFormat()]) {
			continue;
		}

		// see if atom is in target list
		UInt32 i;
		Atom target = None;
		for (i = 0; i < numMotifFormats; ++i) {
			if (converter->getAtom() == motifFormats[i].m_type) {
				target = motifFormats[i].m_type;
				break;
			}
		}
		if (target == None) {
			continue;
		}

		// get the data (finally)
		SInt32 length = motifFormats[i].m_length;
		sprintf(name, "_MOTIF_CLIP_ITEM_%d", motifFormats[i].m_data);
    	Atom atomData = XInternAtom(m_display, name, False), atomTarget;
		CString targetData;
		if (!CXWindowsUtil::getWindowProperty(m_display, root,
									atomData, &targetData,
									&atomTarget, &format, False)) {
			log((CLOG_DEBUG1 "  no data for target", target));
			continue;
		}
		if (atomTarget != atomData) {
			continue;
		}

		// truncate data to length specified in the format
		targetData.erase(length);

		// add to clipboard and note we've done it
		IClipboard::EFormat format = converter->getFormat();
		m_data[format]  = converter->toIClipboard(targetData);
		m_added[format] = true;
		log((CLOG_DEBUG "  added format %d for target %d", converter->getFormat(), target));
	}
}

IClipboard::Time
CXWindowsClipboard::motifGetTime() const
{
	// FIXME -- does Motif report this?
	return 0;
}

bool
CXWindowsClipboard::insertMultipleReply(Window requestor,
				::Time time, Atom property)
{
	// get the requested targets
	Atom target;
	SInt32 format;
	CString data;
	if (!CXWindowsUtil::getWindowProperty(m_display, requestor,
								property, &data, &target, &format, False)) {
		// can't get the requested targets
		return false;
	}

	// fail if the requested targets isn't of the correct form
	if (format != 32 ||
		target != m_atomAtomPair) {
		return false;
	}

	// data is a list of atom pairs:  target, property
	const Atom* targets = reinterpret_cast<const Atom*>(data.data());
	const UInt32 numTargets = data.size() / sizeof(Atom);

	// add replies for each target
	bool changed = false;
	for (UInt32 i = 0; i < numTargets; i += 2) {
		const Atom target   = targets[i + 0];
		const Atom property = targets[i + 1];
		if (!addSimpleRequest(requestor, target, time, property)) {
			// note that we can't perform the requested conversion
			static const Atom none = None;
			data.replace(i * sizeof(Atom), sizeof(Atom),
								reinterpret_cast<const char*>(&none),
								sizeof(Atom));
			changed = true;
		}
	}

	// update the targets property if we changed it
	if (changed) {
		CXWindowsUtil::setWindowProperty(m_display, requestor,
								property, data.data(), data.size(),
								target, format);
	}

	// add reply for MULTIPLE request
	insertReply(new CReply(requestor, m_atomMultiple,
								time, property, CString(), None, 32));

	return true;
}

void
CXWindowsClipboard::insertReply(CReply* reply)
{
	assert(reply != NULL);

	// note -- we must respond to requests in order if requestor,target,time
	// are the same, otherwise we can use whatever order we like with one
	// exception:  each reply in a MULTIPLE reply must be handled in order
	// as well.  those replies will almost certainly not share targets so
	// we can't simply use requestor,target,time as map index.
	//
	// instead we'll use just the requestor.  that's more restrictive than
	// necessary but we're guaranteed to do things in the right order.
	// note that we could also include the time in the map index and still
	// ensure the right order.  but since that'll just make it harder to
	// find the right reply when handling property notify events we stick
	// to just the requestor.

	const bool newWindow = (m_replies.count(reply->m_requestor) == 0);
	m_replies[reply->m_requestor].push_back(reply);

	// adjust requestor's event mask if we haven't done so already.  we
	// want events in case the window is destroyed or any of its
	// properties change.
	if (newWindow) {
		// note errors while we adjust event masks
		bool error = false;
		CXWindowsUtil::CErrorLock lock(m_display, &error);

		// get and save the current event mask
		XWindowAttributes attr;
		XGetWindowAttributes(m_display, reply->m_requestor, &attr);
		m_eventMasks[reply->m_requestor] = attr.your_event_mask;

		// add the events we want
		XSelectInput(m_display, reply->m_requestor, attr.your_event_mask |
								StructureNotifyMask | PropertyChangeMask);

		// if we failed then the window has already been destroyed
		if (error) {
			m_replies.erase(reply->m_requestor);
			delete reply;
		}
	}
}

void
CXWindowsClipboard::pushReplies()
{
	// send the first reply for each window if that reply hasn't
	// been sent yet.
	for (CReplyMap::iterator index = m_replies.begin();
								index != m_replies.end(); ++index) {
		assert(!index->second.empty());
		if (!index->second.front()->m_replied) {
			pushReplies(index, index->second, index->second.begin());
		}
	}
}

void
CXWindowsClipboard::pushReplies(CReplyMap::iterator mapIndex,
				CReplyList& replies, CReplyList::iterator index)
{
	CReply* reply = *index;
	while (sendReply(reply)) {
		// reply is complete.  discard it and send the next reply,
		// if any.
		index = replies.erase(index);
		delete reply;
		if (index == replies.end()) {
			break;
		}
		reply = *index;
	}

	// if there are no more replies in the list then remove the list
	// and stop watching the requestor for events.
	if (replies.empty()) {
		CXWindowsUtil::CErrorLock lock(m_display);
		Window requestor = mapIndex->first;
		XSelectInput(m_display, requestor, m_eventMasks[requestor]);
		m_replies.erase(mapIndex);
		m_eventMasks.erase(requestor);
	}
}

bool
CXWindowsClipboard::sendReply(CReply* reply)
{
	assert(reply != NULL);

	// bail out immediately if reply is done
	if (reply->m_done) {
		log((CLOG_DEBUG1 "clipboard: finished reply to 0x%08x,%d,%d", reply->m_requestor, reply->m_target, reply->m_property));
		return true;
	}

	// start in failed state if property is None
	bool failed = (reply->m_property == None);
	if (!failed) {
		log((CLOG_DEBUG1 "clipboard: setting property on 0x%08x,%d,%d", reply->m_requestor, reply->m_target, reply->m_property));

		// send using INCR if already sending incrementally or if reply
		// is too large, otherwise just send it.
		const UInt32 maxRequestSize = 4 * XMaxRequestSize(m_display);
		const bool useINCR = (reply->m_data.size() > maxRequestSize);

		// send INCR reply if incremental and we haven't replied yet
		if (useINCR && !reply->m_replied) {
			UInt32 size = reply->m_data.size();
			if (!CXWindowsUtil::setWindowProperty(m_display,
								reply->m_requestor, reply->m_property,
								&size, 4, m_atomINCR, 32)) {
				failed = true;
			}
		}

		// send more INCR reply or entire non-incremental reply
		else {
			// how much more data should we send?
			UInt32 size = reply->m_data.size() - reply->m_ptr;
			if (size > maxRequestSize)
				size = maxRequestSize;

			// send it
			if (!CXWindowsUtil::setWindowProperty(m_display,
								reply->m_requestor, reply->m_property,
								reply->m_data.data() + reply->m_ptr,
								size,
								reply->m_type, reply->m_format)) {
				failed = true;
			}
			else {
				reply->m_ptr += size;

				// we've finished the reply if we just sent the zero
				// size incremental chunk or if we're not incremental.
				reply->m_done = (size == 0 || !useINCR);
			}
		}
	}

	// if we've failed then delete the property and say we're done.
	// if we haven't replied yet then we can send a failure notify,
	// otherwise we've failed in the middle of an incremental
	// transfer;  i don't know how to cancel that so i'll just send
	// the final zero-length property.
	// FIXME -- how do you gracefully cancel an incremental transfer?
	if (failed) {
		log((CLOG_DEBUG1 "clipboard: sending failure to 0x%08x,%d,%d", reply->m_requestor, reply->m_target, reply->m_property));
		reply->m_done = true;
		if (reply->m_property != None) {
			CXWindowsUtil::CErrorLock lock(m_display);
			XDeleteProperty(m_display, reply->m_requestor, reply->m_property);
		}

		if (!reply->m_replied) {
			sendNotify(reply->m_requestor, m_selection,
								reply->m_target, None,
								reply->m_time);

			// don't wait for any reply (because we're not expecting one)
			return true;
		}
		else {
			static const char dummy = 0;
			CXWindowsUtil::setWindowProperty(m_display,
								reply->m_requestor, reply->m_property,
								&dummy,
								0,
								reply->m_type, reply->m_format);

			// wait for delete notify
			return false;
		}
	}

	// send notification if we haven't yet
	if (!reply->m_replied) {
		log((CLOG_DEBUG1 "clipboard: sending notify to 0x%08x,%d,%d", reply->m_requestor, reply->m_target, reply->m_property));
		reply->m_replied = true;

		// HACK -- work around apparent bug in lesstif, which doesn't
		// wait around for the SelectionNotify then gets confused when
		// it sees it the next time it requests the selection.  if it
		// looks like a lesstif requestor window then don't send the
		// SelectionNotify.  it looks like a lesstif requestor if:
		//   it has a _MOTIF_CLIP_LOCK_ACCESS_VALID property
		//   it does not have a GDK_SELECTION property
		CString dummy;
		if (m_id != kClipboardClipboard ||
			!CXWindowsUtil::getWindowProperty(m_display,
								reply->m_requestor,
								m_atomMotifClipAccess,
								&dummy, NULL, NULL, False) ||
			CXWindowsUtil::getWindowProperty(m_display,
								reply->m_requestor,
								m_atomGDKSelection,
								&dummy, NULL, NULL, False)) {
			sendNotify(reply->m_requestor, m_selection,
								reply->m_target, reply->m_property,
								reply->m_time);
		}
	}

	// wait for delete notify
	return false;
}

void
CXWindowsClipboard::clearReplies()
{
	for (CReplyMap::iterator index = m_replies.begin();
								index != m_replies.end(); ++index) {
		clearReplies(index->second);
	}
	m_replies.clear();
	m_eventMasks.clear();
}

void
CXWindowsClipboard::clearReplies(CReplyList& replies)
{
	for (CReplyList::iterator index = replies.begin();
								index != replies.end(); ++index) {
		delete *index;
	}
	replies.clear();
}

void
CXWindowsClipboard::sendNotify(Window requestor,
				Atom selection, Atom target, Atom property, Time time)
{
	XEvent event;
	event.xselection.type      = SelectionNotify;
	event.xselection.display   = m_display;
	event.xselection.requestor = requestor;
	event.xselection.selection = selection;
	event.xselection.target    = target;
	event.xselection.property  = property;
	event.xselection.time      = time;
	CXWindowsUtil::CErrorLock lock(m_display);
	XSendEvent(m_display, requestor, False, 0, &event);
}

bool
CXWindowsClipboard::wasOwnedAtTime(::Time time) const
{
	// not owned if we've never owned the selection
	checkCache();
	if (m_timeOwned == 0) {
		return false;
	}

	// if time is CurrentTime then return true if we still own the
	// selection and false if we do not.  else if we still own the
	// selection then get the current time, otherwise use
	// m_timeLost as the end time.
	Time lost = m_timeLost;
	if (m_timeLost == 0) {
		if (time == CurrentTime) {
			return true;
		}
		else {
			lost = CXWindowsUtil::getCurrentTime(m_display, m_window);
		}
	}
	else {
		if (time == CurrentTime) {
			return false;
		}
	}

	// compare time to range
	Time duration = lost - m_timeOwned;
	Time when     = time - m_timeOwned;
	return (/*when >= 0 &&*/ when < duration);
}

Atom
CXWindowsClipboard::getTargetsData(CString& data, int* format) const
{
	assert(format != NULL);

	// add standard targets
	Atom atom;
	atom = m_atomTargets;
	data.append(reinterpret_cast<char*>(&atom), sizeof(Atom));
	atom = m_atomMultiple;
	data.append(reinterpret_cast<char*>(&atom), sizeof(Atom));
	atom = m_atomTimestamp;
	data.append(reinterpret_cast<char*>(&atom), sizeof(Atom));

	// add targets we can convert to
	for (ConverterList::const_iterator index = m_converters.begin();
								index != m_converters.end(); ++index) {
		IXWindowsClipboardConverter* converter = *index;

		// skip formats we don't have
		if (m_added[converter->getFormat()]) {
			atom = converter->getAtom();
			data.append(reinterpret_cast<char*>(&atom), sizeof(Atom));
		}
	}

	*format = 32;
	return m_atomTargets;
}

Atom
CXWindowsClipboard::getTimestampData(CString& data, int* format) const
{
	assert(format != NULL);

	assert(sizeof(m_timeOwned) == 4);
	checkCache();
	data.append(reinterpret_cast<const char*>(&m_timeOwned), 4);
	*format = 32;
	return m_atomTimestamp;
}


//
// CXWindowsClipboard::CICCCMGetClipboard
//

CXWindowsClipboard::CICCCMGetClipboard::CICCCMGetClipboard(
				Window requestor, Time time, Atom property) :
	m_requestor(requestor),
	m_time(time),
	m_property(property),
	m_incr(false),
	m_failed(false),
	m_done(false),
	m_reading(false),
	m_data(NULL),
	m_actualTarget(NULL),
	m_error(false)
{
	// do nothing
}

CXWindowsClipboard::CICCCMGetClipboard::~CICCCMGetClipboard()
{
	// do nothing
}

bool
CXWindowsClipboard::CICCCMGetClipboard::readClipboard(Display* display,
				Atom selection, Atom target, Atom* actualTarget, CString* data)
{
	assert(actualTarget != NULL);
	assert(data         != NULL);

	log((CLOG_DEBUG1 "request selection=%d, target=%d, window=%x", selection, target, m_requestor));

	// save output pointers
	m_actualTarget = actualTarget;
	m_data         = data;

	// assume failure
	*m_actualTarget = None;
	*m_data         = "";

	// delete target property
	XDeleteProperty(display, m_requestor, m_property);

	// select window for property changes
	XWindowAttributes attr;
	XGetWindowAttributes(display, m_requestor, &attr);
	XSelectInput(display, m_requestor,
								attr.your_event_mask | PropertyChangeMask);

	// request data conversion
	XConvertSelection(display, selection, target,
								m_property, m_requestor, m_time);

	// synchronize with server before we start following timeout countdown
	XSync(display, False);

	// Xlib inexplicably omits the ability to wait for an event with
	// a timeout.  (it's inexplicable because there's no portable way
	// to do it.)  we'll poll until we have what we're looking for or
	// a timeout expires.  we use a timeout so we don't get locked up
	// by badly behaved selection owners.
	XEvent xevent;
	std::vector<XEvent> events;
	CStopwatch timeout(true);
	static const double s_timeout = 0.25;	// FIXME -- is this too short?
	while (!m_done && !m_failed) {
		// fail if timeout has expired
		if (timeout.getTime() >= s_timeout) {
			m_failed = true;
			break;
		}

		// process events if any otherwise sleep
		if (XPending(display) > 0) {
			while (!m_done && !m_failed && XPending(display) > 0) {
				XNextEvent(display, &xevent);
				if (!processEvent(display, &xevent)) {
					// not processed so save it
					events.push_back(xevent);
				}
			}
		}
		else {
			CThread::sleep(0.01);
		}
	}

	// put unprocessed events back
	for (UInt32 i = events.size(); i > 0; --i) {
		XPutBackEvent(display, &events[i - 1]);
	}

	// restore mask
	XSelectInput(display, m_requestor, attr.your_event_mask);

	// return success or failure
	log((CLOG_DEBUG1 "request %s", m_failed ? "failed" : "succeeded"));
	return !m_failed;
}

bool
CXWindowsClipboard::CICCCMGetClipboard::processEvent(
				Display* display, XEvent* xevent)
{
	// process event
	switch (xevent->type) {
	case DestroyNotify:
		if (xevent->xdestroywindow.window == m_requestor) {
			m_failed = true;
			return true;
		}

		// not interested
		return false;

	case SelectionNotify:
		if (xevent->xselection.requestor == m_requestor) {
			// done if we can't convert
			if (xevent->xselection.property == None) {
				m_done = true;
				return true;
			}

			// proceed if conversion successful
			else if (xevent->xselection.property == m_property) {
				m_reading = true;
				break;
			}
		}

		// otherwise not interested
		return false;

	case PropertyNotify:
		// proceed if conversion successful and we're receiving more data
		if (xevent->xproperty.window == m_requestor &&
			xevent->xproperty.atom   == m_property &&
			xevent->xproperty.state  == PropertyNewValue) {
			if (!m_reading) {
				// we haven't gotten the SelectionNotify yet
				return true;
			}
			break;
		}

		// otherwise not interested
		return false;

	default:
		// not interested
		return false;
	}

	// get the data from the property
	Atom target;
	const CString::size_type oldSize = m_data->size();
	if (!CXWindowsUtil::getWindowProperty(display, m_requestor,
								m_property, m_data, &target, NULL, True)) {
		// unable to read property
		m_failed = true;
		return true;
	}

	// note if incremental.  if we're already incremental then the
	// selection owner is busted.  if the INCR property has no size
	// then the selection owner is busted.
	if (target == XInternAtom(display, "INCR", False)) {
log((CLOG_INFO "  INCR"));	// FIXME
		if (m_incr) {
log((CLOG_INFO "  INCR repeat"));	// FIXME
			m_failed = true;
			m_error  = true;
		}
		else if (m_data->size() == oldSize) {
log((CLOG_INFO "  INCR zero size"));	// FIXME
			m_failed = true;
			m_error  = true;
		}
		else {
log((CLOG_INFO "  INCR start"));	// FIXME
			m_incr   = true;

			// discard INCR data
			*m_data = "";
		}
	}

	// handle incremental chunks
	else if (m_incr) {
		// if first incremental chunk then save target
		if (oldSize == 0) {
			log((CLOG_DEBUG1 "  INCR first chunk, target %d", target));
			*m_actualTarget = target;
		}

		// secondary chunks must have the same target
		else {
log((CLOG_INFO "  INCR secondary chunk"));	// FIXME
			if (target != *m_actualTarget) {
				log((CLOG_WARN "  INCR target mismatch"));
				m_failed = true;
				m_error  = true;
			}
		}

		// note if this is the final chunk
		if (m_data->size() == oldSize) {
			log((CLOG_DEBUG1 "  INCR final chunk: %d bytes total", m_data->size()));
			m_done = true;
		}
	}

	// not incremental;  save the target.
	else {
		log((CLOG_DEBUG1 "  target %d", target));
		*m_actualTarget = target;
		m_done          = true;
	}

	// this event has been processed
	logc(!m_incr, (CLOG_DEBUG1 "  got data, %d bytes", m_data->size()));
	return true;
}


//
// CXWindowsClipboard::CReply
//

CXWindowsClipboard::CReply::CReply(Window requestor, Atom target, ::Time time) :
	m_requestor(requestor),
	m_target(target),
	m_time(time),
	m_property(None),
	m_replied(false),
	m_done(false),
	m_data(),
	m_type(None),
	m_format(32),
	m_ptr(0)
{
	// do nothing
}

CXWindowsClipboard::CReply::CReply(Window requestor, Atom target, ::Time time,
				Atom property, const CString& data, Atom type, int format) :
	m_requestor(requestor),
	m_target(target),
	m_time(time),
	m_property(property),
	m_replied(false),
	m_done(false),
	m_data(data),
	m_type(type),
	m_format(format),
	m_ptr(0)
{
	// do nothing
}
