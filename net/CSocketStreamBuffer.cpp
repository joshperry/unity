#include "CSocketStreamBuffer.h"
#include <assert.h>

//
// CSocketStreamBuffer
//

const UInt32			CSocketStreamBuffer::kChunkSize = 4096;

CSocketStreamBuffer::CSocketStreamBuffer() : m_size(0)
{
	// do nothing
}

CSocketStreamBuffer::~CSocketStreamBuffer()
{
	// do nothing
}

const void*				CSocketStreamBuffer::peek(UInt32 n)
{
	assert(n <= m_size);

	// reserve space in first chunk
	ChunkList::iterator head = m_chunks.begin();
	head->reserve(n);

	// consolidate chunks into the first chunk until it has n bytes
	ChunkList::iterator scan = head;
	++scan;
	while (head->size() < n && scan != m_chunks.end()) {
		head->insert(head->end(), scan->begin(), scan->end());
		scan = m_chunks.erase(scan);
	}

	return reinterpret_cast<const void*>(head->begin());
}

void					CSocketStreamBuffer::pop(UInt32 n)
{
	m_size -= n;

	// discard chunks until more than n bytes would've been discarded
	ChunkList::iterator scan = m_chunks.begin();
	while (scan->size() <= n && scan != m_chunks.end()) {
		n -= scan->size();
		scan = m_chunks.erase(scan);
	}

	// if there's anything left over then remove it from the head chunk.
	// if there's no head chunk then we're already empty.
	if (scan == m_chunks.end()) {
		m_size = 0;
	}
	else if (n > 0) {
		scan->erase(scan->begin(), scan->begin() + n);
	}
}

void					CSocketStreamBuffer::write(
								const void* vdata, UInt32 n)
{
	assert(vdata != NULL);

	if (n == 0) {
		return;
	}
	m_size += n;

	// cast data to bytes
	const UInt8* data = reinterpret_cast<const UInt8*>(vdata);

	// point to last chunk if it has space, otherwise append an empty chunk
	ChunkList::iterator scan = m_chunks.end();
	if (scan != m_chunks.begin()) {
		--scan;
		if (scan->size() >= kChunkSize)
			++scan;
	}
	if (scan == m_chunks.end()) {
		scan = m_chunks.insert(scan);
	}

	// append data in chunks
	while (n > 0) {
		// choose number of bytes for next chunk
		UInt32 count = kChunkSize - scan->size();
		if (count > n)
			count = n;

		// transfer data
		scan->insert(scan->end(), data, data + count);
		n    -= count;
		data += count;

		// append another empty chunk if we're not done yet
		if (n > 0) {
			scan = m_chunks.insert(scan);
		}
	}
}

UInt32					CSocketStreamBuffer::getSize() const
{
	return m_size;
}