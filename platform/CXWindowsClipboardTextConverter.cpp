#include "CXWindowsClipboardTextConverter.h"
#include "CUnicode.h"

//
// CXWindowsClipboardTextConverter
//

CXWindowsClipboardTextConverter::CXWindowsClipboardTextConverter(
				Display* display, const char* name) :
	m_atom(XInternAtom(display, name, False))
{
	// do nothing
}

CXWindowsClipboardTextConverter::~CXWindowsClipboardTextConverter()
{
	// do nothing
}

IClipboard::EFormat
CXWindowsClipboardTextConverter::getFormat() const
{
	return IClipboard::kText;
}

Atom
CXWindowsClipboardTextConverter::getAtom() const
{
	return m_atom;
}

int
CXWindowsClipboardTextConverter::getDataSize() const
{
	return 8;
}

CString
CXWindowsClipboardTextConverter::fromIClipboard(const CString& data) const
{
	return CUnicode::UTF8ToText(data);
}

CString
CXWindowsClipboardTextConverter::toIClipboard(const CString& data) const
{
	// convert to UTF-8
	bool errors;
	CString utf8 = CUnicode::textToUTF8(data, &errors);

	// if there were decoding errors then, to support old applications
	// that don't understand UTF-8 but can report the exact binary
	// UTF-8 representation, see if the data appears to be UTF-8.  if
	// so then use it as is.
	if (errors && CUnicode::isUTF8(data)) {
		return data;
	}

	return utf8;
}
