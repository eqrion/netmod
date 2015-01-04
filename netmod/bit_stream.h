#ifndef bit_stream_h
#define bit_stream_h

#include <stdint.h>

class bit_stream
{
public:
	bit_stream() : _begin(nullptr), _size(0), _seek(nullptr), _bit_offset(0) { }
	bit_stream(char* buffer, size_t length) : _begin(buffer), _size(length), _seek(buffer), _bit_offset(0) { }

	void attach(char* buffer, size_t length)
	{
		deattach();
		_begin = _seek = buffer;
		_size = length;
	}
	void deattach()
	{
		_begin = nullptr;
		_seek = nullptr;
		_size = 0;
		_bit_offset = 0;
	}

	char* begin() { return _begin; }
	char* seek() { return _seek; }
	char* end() { return _begin + _size; }

	size_t tell() const { return _seek - _begin + ((_bit_offset + 7) / 8); }
	size_t size() const { return _size; }

	void skip(size_t amount)
	{
		_seek += amount;
	}

	bool eof() const { return _seek >= _begin + _size; }
	
	template<uint8_t bit_length>
	void write_int(int32_t value)
	{
		static_assert(bit_length != 0, "don't write a zero length integer");
		static_assert(bit_length <= 32, "bit length out of range of a 32 bit integer");

		// 1010101111111 : original value (7 sig bits)
		// 1111111111111 : clean mask
		// 0000001111111 : clean mask rshifted 7 bits
		// 0000001111111 : original value cleaned
		// 0011111110000 : cleaned and moved. ready to be |'ed onto destination

		if (bit_length + _bit_offset > 32)
		{
			// move to the next char, not enough room here

			//_bit_offset is always [0,8)
			//_bit_offset += 8 - _bit_offset;
			//_seek += _bit_offset / 8;

			_bit_offset = 0;
			_seek += 1;
		}

		*((int32_t*)_seek) |= (value & (0xFFFFFFFF >> (32 - bit_length))) << _bit_offset;

		_bit_offset += bit_length;
		_seek += _bit_offset / 8;
		_bit_offset = _bit_offset % 8;
	}
	template<uint8_t bit_length>
	void write_uint(uint32_t value)
	{
		static_assert(bit_length != 0, "don't write a zero length integer");
		static_assert(bit_length <= 32, "bit length out of range of a 32 bit integer");

		if (bit_length + _bit_offset > 32)
		{
			_bit_offset = 0;
			_seek += 1;
		}

		*((uint32_t*)_seek) |= (value & (0xFFFFFFFF >> (32 - bit_length))) << _bit_offset;

		_bit_offset += bit_length;
		_seek += _bit_offset / 8;
		_bit_offset = _bit_offset % 8;
	}
		
	template<uint8_t bit_length>
	int32_t read_int()
	{
		static_assert(bit_length != 0, "cannot read a zero length integer");
		static_assert(bit_length <= 32, "bit length out of range of a 32 bit integer");
			
		if (bit_length + _bit_offset > 32)
		{
			_bit_offset = 0;
			_seek += 1;
		}

		int32_t value = *((int32_t*)_seek);
		value = (value >> _bit_offset) & (0xFFFFFFFF >> (32 - bit_length));

		_bit_offset += bit_length;
		_seek += _bit_offset / 8;
		_bit_offset = _bit_offset % 8;
			
		return value;
	}		
	template<uint8_t bit_length>
	uint32_t read_uint()
	{
		static_assert(bit_length != 0, "cannot read a zero length integer");
		static_assert(bit_length <= 32, "bit length out of range of a 32 bit integer");
			
		if (bit_length + _bit_offset > 32)
		{
			_bit_offset = 0;
			_seek += 1;
		}

		uint32_t value = *((uint32_t*)_seek);
		value = (value >> _bit_offset) & (0xFFFFFFFF >> (32 - bit_length));

		_bit_offset += bit_length;
		_seek += _bit_offset / 8;
		_bit_offset = _bit_offset % 8;
			
		return value;
	}

	void write_bool(bool value)
	{
		if (_bit_offset > 31)
		{
			_bit_offset = 0;
			_seek += 1;
		}

		*((uint32_t*)_seek) |= ((uint32_t)value & (0xFFFFFFFF >> (32 - 1))) << _bit_offset;

		_bit_offset += 1;
		_seek += _bit_offset / 8;
		_bit_offset = _bit_offset % 8;
	}
	bool read_bool()
	{	
		if (_bit_offset > 31)
		{
			_bit_offset = 0;
			_seek += 1;
		}

		uint32_t value = *((uint32_t*)_seek);
		value = (value >> _bit_offset) & (0xFFFFFFFF >> (32 - 1));

		_bit_offset += 1;
		_seek += _bit_offset / 8;
		_bit_offset = _bit_offset % 8;
			
		return value != 0;
	}

	template<typename T>
	T fast_read()
	{
		_bit_offset = 0;
		T result = *(T*)_seek;
		_seek += sizeof(T);
		return result;
	}

	template<typename T>
	void fast_write(T val)
	{
		_bit_offset = 0;
		*((T*)_seek) = val;
		_seek += sizeof(T);
	}

private:
	char* _begin;
	char* _seek;
	size_t _size;

	// always between [0,8)
	uint8_t _bit_offset;
};

#endif