#include "CProtocolUtil.h"
#include "IInputStream.h"
#include "IOutputStream.h"
#include "CLog.h"
#include <assert.h>
#include <ctype.h>
#include <string.h>

//
// CProtocolUtil
//

void					CProtocolUtil::writef(IOutputStream* stream,
								const char* fmt, ...) throw(XIO)
{
	assert(stream != NULL);
	assert(fmt != NULL);
	log((CLOG_DEBUG "writef(%s)", fmt));

	va_list args;

	// determine total size to write
	va_start(args, fmt);
	UInt32 count = getLength(fmt, args);
	va_end(args);

	// done if nothing to write
	if (count == 0) {
		return;
	}

	// fill buffer
	UInt8* buffer = new UInt8[count];
	va_start(args, fmt);
	writef(buffer, fmt, args);
	va_end(args);

	// write buffer
	UInt8* scan = buffer;
	while (count > 0) {
		const UInt32 n = stream->write(scan, count);
		log((CLOG_DEBUG "wrote %d of %d bytes", n, count));
		count -= n;
		scan  += n;
	}

	delete[] buffer;
}

void					CProtocolUtil::readf(IInputStream* stream,
								const char* fmt, ...) throw(XIO)
{
	assert(stream != NULL);
	assert(fmt != NULL);

	va_list args;
	va_start(args, fmt);

	// begin scanning
	while (*fmt) {
		if (*fmt == '%') {
			// format specifier.  determine argument size.
			++fmt;
			UInt32 len = eatLength(&fmt);
			switch (*fmt) {
			  case 'i': {
				// check for valid length
				assert(len == 1 || len == 2 || len == 4);

				// read the data
				UInt8 buffer[4];
				read(stream, buffer, len);

				// convert it
				UInt32* v = va_arg(args, UInt32*);
				switch (len) {
				  case 1:
					// 1 byte integer
					*v = static_cast<UInt32>(buffer[0]);
					break;

				  case 2:
					// 2 byte integer
					*v = (static_cast<UInt32>(buffer[0]) << 8) |
						  static_cast<UInt32>(buffer[1]);
					break;

				  case 4:
					// 4 byte integer
					*v = (static_cast<UInt32>(buffer[0]) << 24) |
						 (static_cast<UInt32>(buffer[1]) << 16) |
						 (static_cast<UInt32>(buffer[2]) <<  8) |
						  static_cast<UInt32>(buffer[3]);
					break;
				}
				break;
			  }

			  case 's': {
				assert(len == 0);

				// read the string length
				UInt8 buffer[128];
				read(stream, buffer, 4);
				UInt32 len = (static_cast<UInt32>(buffer[0]) << 24) |
							 (static_cast<UInt32>(buffer[1]) << 16) |
							 (static_cast<UInt32>(buffer[2]) <<  8) |
							  static_cast<UInt32>(buffer[3]);

				// use a fixed size buffer if its big enough
				const bool useFixed = (len <= sizeof(buffer));

				// allocate a buffer to read the data
				UInt8* sBuffer;
				if (useFixed) {
					sBuffer = buffer;
				}
				else {
					sBuffer = new UInt8[len];
				}

				// read the data
				try {
					read(stream, sBuffer, len);
				}
				catch (...) {
					if (!useFixed) {
						delete[] sBuffer;
					}
					throw;
				}

				// save the data
				CString* dst = va_arg(args, CString*);
				dst->assign((const char*)sBuffer, len);

				// release the buffer
				if (!useFixed) {
					delete[] sBuffer;
				}
				break;
			  }

			  case '%':
				assert(len == 0);
				break;

			  default:
				assert(0 && "invalid format specifier");
			}

			// next format character
			++fmt;
		}
		else {
			// read next character
			char buffer[1];
			read(stream, buffer, 1);

			// verify match
			if (buffer[0] != *fmt) {
				throw XIOReadMismatch();
			}

			// next format character
			++fmt;
		}
	}

	va_end(args);
}

UInt32					CProtocolUtil::getLength(
								const char* fmt, va_list args) throw()
{
	UInt32 n = 0;
	while (*fmt) {
		if (*fmt == '%') {
			// format specifier.  determine argument size.
			++fmt;
			UInt32 len = eatLength(&fmt);
			switch (*fmt) {
			  case 'i':
				assert(len == 1 || len == 2 || len == 4);
				(void)va_arg(args, UInt32);
				break;

			  case 's':
				assert(len == 0);
				len = va_arg(args, UInt32) + 4;
				(void)va_arg(args, UInt8*);
				break;

			  case '%':
				assert(len == 0);
				len = 1;
				break;

			  default:
				assert(0 && "invalid format specifier");
			}

			// accumulate size
			n += len;
			++fmt;
		}
		else {
			// regular character
			++n;
			++fmt;
		}
	}
	return n;
}

void			 		CProtocolUtil::writef(void* buffer,
								const char* fmt, va_list args) throw(XIO)
{
	UInt8* dst = reinterpret_cast<UInt8*>(buffer);

	while (*fmt) {
		if (*fmt == '%') {
			// format specifier.  determine argument size.
			++fmt;
			UInt32 len = eatLength(&fmt);
			switch (*fmt) {
			  case 'i': {
				const UInt32 v = va_arg(args, UInt32);
				switch (len) {
				  case 1:
					// 1 byte integer
					*dst++ = static_cast<UInt8>(v & 0xff);
					break;

				  case 2:
					// 2 byte integer
					*dst++ = static_cast<UInt8>((v >> 8) & 0xff);
					*dst++ = static_cast<UInt8>( v       & 0xff);
					break;

				  case 4:
					// 4 byte integer
					*dst++ = static_cast<UInt8>((v >> 24) & 0xff);
					*dst++ = static_cast<UInt8>((v >> 16) & 0xff);
					*dst++ = static_cast<UInt8>((v >>  8) & 0xff);
					*dst++ = static_cast<UInt8>( v        & 0xff);
					break;

				  default:
					assert(0 && "invalid integer format length");
					return;
				}
				break;
			  }

			  case 's': {
				assert(len == 0);
				const UInt32 len = va_arg(args, UInt32);
				const UInt8* src = va_arg(args, UInt8*);
				*dst++ = static_cast<UInt8>((len >> 24) & 0xff);
				*dst++ = static_cast<UInt8>((len >> 16) & 0xff);
				*dst++ = static_cast<UInt8>((len >>  8) & 0xff);
				*dst++ = static_cast<UInt8>( len        & 0xff);
				memcpy(dst, src, len);
				dst += len;
				break;
			  }

			  case '%':
				assert(len == 0);
				*dst++ = '%';
				break;

			  default:
				assert(0 && "invalid format specifier");
			}

			// next format character
			++fmt;
		}
		else {
			// copy regular character
			*dst++ = *fmt++;
		}
	}
}

UInt32					CProtocolUtil::eatLength(const char** pfmt) throw()
{
	const char* fmt = *pfmt;
	UInt32 n = 0;
	for (;;) {
		UInt32 d;
		switch (*fmt) {
		  case '0': d = 0; break;
		  case '1': d = 1; break;
		  case '2': d = 2; break;
		  case '3': d = 3; break;
		  case '4': d = 4; break;
		  case '5': d = 5; break;
		  case '6': d = 6; break;
		  case '7': d = 7; break;
		  case '8': d = 8; break;
		  case '9': d = 9; break;
		  default: *pfmt = fmt; return n;
		}
		n = 10 * n + d;
		++fmt;
	}
}

void					CProtocolUtil::read(IInputStream* stream,
								void* vbuffer, UInt32 count) throw(XIO)
{
	assert(stream != NULL);
	assert(vbuffer != NULL);

	UInt8* buffer = reinterpret_cast<UInt8*>(vbuffer);
	while (count > 0) {
		// read more
		UInt32 n = stream->read(buffer, count);

		// bail if stream has hungup
		if (n == 0) {
			throw XIOEndOfStream();
		}

		// prepare for next read
		buffer += n;
		count  -= n;
	}
}


//
// XIOReadMismatch
//

CString					XIOReadMismatch::getWhat() const throw()
{
	return "CProtocolUtil::readf() mismatch";
}
