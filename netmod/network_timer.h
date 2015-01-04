#ifndef onyx_timer_h
#define onyx_timer_h

#include <stdint.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class network_timer
{
public:
	network_timer()
	{
		QueryPerformanceFrequency(&_frequency);
	}

	uint64_t get_milliseconds()
	{
		LARGE_INTEGER current_time;
		QueryPerformanceCounter(&current_time);

		uint64_t quotient = current_time.QuadPart / _frequency.QuadPart;
		uint64_t remainder = current_time.QuadPart % _frequency.QuadPart;

		return quotient * 1000 + (remainder * 1000) / _frequency.QuadPart;
	}
	uint64_t get_microseconds()
	{
		LARGE_INTEGER current_time;
		QueryPerformanceCounter(&current_time);

		uint64_t quotient = current_time.QuadPart / _frequency.QuadPart;
		uint64_t remainder = current_time.QuadPart % _frequency.QuadPart;

		return quotient * 1000000 + (remainder * 1000000) / _frequency.QuadPart;
	}
	uint64_t get_nanoseconds()
	{
		LARGE_INTEGER current_time;
		QueryPerformanceCounter(&current_time);

		uint64_t quotient = current_time.QuadPart / _frequency.QuadPart;
		uint64_t remainder = current_time.QuadPart % _frequency.QuadPart;

		return quotient * 1000000000 + (remainder * 1000000000) / _frequency.QuadPart;
	}

private:
	LARGE_INTEGER _frequency;
};

#endif