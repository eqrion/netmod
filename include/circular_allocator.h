#ifndef onyx_circular_allocator_h
#define onyx_circular_allocator_h

#include <stdint.h>

class circular_allocator
{
public:

	circular_allocator() :
		_buffer_begin(nullptr), _buffer_end(nullptr), _alloc_begin(nullptr), _alloc_end(nullptr), _allocated(0)
	{

	}
	~circular_allocator()
	{
		destroy();
	}

	void create(size_t size)
	{
		destroy();

		_buffer_begin = new char[size];
		_buffer_end = _buffer_begin + size;

		_alloc_begin = _buffer_begin;
		_alloc_end = _buffer_begin;
	}
	void destroy()
	{
		if (_buffer_begin != nullptr)
		{
			delete[] _buffer_begin;
		}

		_buffer_begin = nullptr;
		_buffer_end = nullptr;

		_alloc_begin = nullptr;
		_alloc_end = nullptr;
		_allocated = 0;
	}

	char* push_back(size_t size)
	{
		if (_alloc_end == _alloc_begin && _allocated > 0)
		{
			return nullptr;
		}

		size += sizeof(size_t);
		char* new_alloc_end = _alloc_end + size;

		// has the current alloc_end wrapped around?

		if (_alloc_end >= _alloc_begin)
		{
			// we have not wrapped around, so we only need to worry about surpassing the end of the buffer

			if (new_alloc_end <= _buffer_end)
			{
				// this is a simple allocation

				*((size_t*)_alloc_end) = size;
				
				char* result = _alloc_end + sizeof(size_t);
				_alloc_end = new_alloc_end;
				_allocated += size;
				return result;
			}
			else
			{
				// there is not enough room going forward, we will have to try and wrap around

				// if there is room for a (size_t):
				// write a special value here to indicate that we
				// wrapped around and that there is no allocation here
				//
				// if there is not room, it is understood that we wrapped around
				// and there is no allocation here

				if (_buffer_end - _alloc_end >= sizeof(size_t))
				{
					*((size_t*)_alloc_end) = ~((size_t)0);
				}

				// try and start from the beginning

				new_alloc_end = _buffer_begin + size;

				// this is almost a copy of the wrapped around push_back code
				// take care to set the wrapped_around_flag for when we move alloc_end to alloc_begin

				if (new_alloc_end < _alloc_begin)
				{
					*((size_t*)_buffer_begin) = size;

					char* result = _buffer_begin + sizeof(size_t);
					_alloc_end = new_alloc_end;
					_allocated += size;
					return result;
				}
				else if (new_alloc_end == _alloc_begin)
				{
					*((size_t*)_buffer_begin) = size;

					char* result = _alloc_end + sizeof(size_t);
					_alloc_end = new_alloc_end;
					_allocated += size;
					return result;
				}

				// there is not enough room even with a wrap around
				return nullptr;
			}
		}
		else
		{
			if (new_alloc_end <= _alloc_begin)
			{
				*((size_t*)_alloc_end) = size;

				char* result = _alloc_end + sizeof(size_t);
				_alloc_end = new_alloc_end;
				_allocated += size;
				return result;
			}

			// there is not enough room
			return nullptr;
		}
	}
	void pop_front()
	{
		// there is always a valid allocation of at least sizeof(size_t) if alloc_begin != alloc_end
		// if they are equal there is no work to do

		if ((_alloc_begin == _alloc_end) && !(_allocated > 0))
		{
			return;
		}

		// alloc_begin can only wrap around if alloc_end has wrapped around
		// if there isn't enough room for a marker then assume it has wrapped around
		// otherwhise check the marker to see if it is the special value
		
		if (
			(_alloc_end < _alloc_begin || (_allocated > 0)) &&
			(_buffer_end - _alloc_begin < sizeof(size_t) || *((size_t*)_alloc_begin) == ~((size_t)0))
			)
		{
			// we need to wrap alloc_begin to the beginning of the buffer

			_alloc_begin = _buffer_begin;
		}

		// move the beginning of the allocation forward by however much the marker tells us to
		// the marker has itself included in its value so we don't have to worry about that

		size_t allocation_size = *((size_t*)_alloc_begin);

		_alloc_begin += allocation_size;
		_allocated -= allocation_size;

		// if we have popped all allocations then reset the markers to the beginning
		// this isn't really necessary but I think it is clean

		if (_alloc_begin == _alloc_end)
		{
			_alloc_begin = _alloc_end = _buffer_begin;
		}
	}

	void reset()
	{
		_alloc_begin = _alloc_end = _buffer_begin;
		_allocated = 0;
	}

	circular_allocator(const circular_allocator& rhs) :
		_buffer_begin(nullptr), _buffer_end(nullptr), _alloc_begin(nullptr), _alloc_end(nullptr), _allocated(0)
	{
		size_t size = rhs._buffer_end - rhs._buffer_begin;

		if (size != 0)
		{
			_buffer_begin = new char[size];
			_buffer_end = _buffer_begin + size;
			_alloc_begin = _buffer_begin + (rhs._buffer_begin - rhs._alloc_begin);
			_alloc_end = _buffer_begin + (rhs._buffer_begin - rhs._alloc_end);
			_allocated = rhs._allocated;
		}
	}
	const circular_allocator& operator=(const circular_allocator& rhs)
	{
		destroy();

		size_t size = rhs._buffer_end - rhs._buffer_begin;

		if (size != 0)
		{
			_buffer_begin = new char[size];
			_buffer_end = _buffer_begin + size;
			_alloc_begin = _buffer_begin + (rhs._buffer_begin - rhs._alloc_begin);
			_alloc_end = _buffer_begin + (rhs._buffer_begin - rhs._alloc_end);
			_allocated = rhs._allocated;
		}

		return *this;
	}

	circular_allocator(circular_allocator&& rhs) :
		_buffer_begin(nullptr), _buffer_end(nullptr), _alloc_begin(nullptr), _alloc_end(nullptr), _allocated(0)
	{
		std::swap(_buffer_begin, rhs._buffer_begin);
		std::swap(_buffer_end, rhs._buffer_end);
		std::swap(_alloc_begin, rhs._alloc_begin);
		std::swap(_alloc_end, rhs._alloc_end);
		std::swap(_allocated, rhs._allocated);
	}
	circular_allocator& operator=(circular_allocator&& rhs)
	{
		std::swap(_buffer_begin, rhs._buffer_begin);
		std::swap(_buffer_end, rhs._buffer_end);
		std::swap(_alloc_begin, rhs._alloc_begin);
		std::swap(_alloc_end, rhs._alloc_end);
		std::swap(_allocated, rhs._allocated);

		return *this;
	}

private:
	char* _buffer_begin;
	char* _buffer_end;

	char* _alloc_begin;
	char* _alloc_end;
	size_t _allocated;
};


#endif