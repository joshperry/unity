#include "stdpre.h"
#if !defined(CONFIG_PLATFORM_LINUX)
#include <istream>
#else
// some versions of libstdc++ don't have <istream>
// FIXME -- only include iostream for versions that don't have istream
#include <iostream>
#endif
#include "stdpost.h"

#if defined(CONFIG_PLATFORM_WIN32) && defined(_MSC_VER)
// istream has no overloads for __int* types
inline
std::istream& operator>>(std::istream& s, SInt8& i)
{ return s >> (signed char&)i; }
inline
std::istream& operator>>(std::istream& s, SInt16& i)
{ return s >> (short&)i; }
inline
std::istream& operator>>(std::istream& s, SInt32& i)
{ return s >> (int&)i; }
inline
std::istream& operator>>(std::istream& s, UInt8& i)
{ return s >> (unsigned char&)i; }
inline
std::istream& operator>>(std::istream& s, UInt16& i)
{ return s >> (unsigned short&)i; }
inline
std::istream& operator>>(std::istream& s, UInt32& i)
{ return s >> (unsigned int&)i; }
#endif
