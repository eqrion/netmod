#ifndef onyx_uuid_h
#define onyx_uuid_h

#include <stdint.h>
#include <string>

namespace details
{
	inline char to_char(size_t i) {
		if (i <= 9) {
			return static_cast<char>('0' + i);
		}
		else {
			return static_cast<char>('a' + (i - 10));
		}
	}
	inline wchar_t to_wchar(size_t i) {
		if (i <= 9) {
			return static_cast<wchar_t>(L'0' + i);
		}
		else {
			return static_cast<wchar_t>(L'a' + (i - 10));
		}
	}
}

struct uuid
{
public:
	static const size_t data_size = 16;

	uint8_t*		begin() { return data; }
	uint8_t*		end() { return data + data_size; }

	const uint8_t*	cbegin() const { return data; }
	const uint8_t*	cend() const { return data + data_size; }

	bool is_nil() const
	{
		for (size_t i = 0; i < data_size; ++i)
		{
			if (data[i] != 0U)
				return false;
		}
		return true;
	}

	std::string to_string() const
	{
		std::string result;
		result.reserve(36);

		size_t i = 0;
		for (const uint8_t* it_data = cbegin(); it_data != cend(); ++it_data, ++i)
		{
			const size_t hi = ((*it_data) >> 4) & 0x0F;
			result.push_back(details::to_char(hi));

			const size_t lo = (*it_data) & 0x0F;
			result.push_back(details::to_char(lo));

			if (i == 3 || i == 5 || i == 7 || i == 9)
			{
				result.push_back('-');
			}
		}
		return result;
	}
	std::wstring to_wstring() const
	{
		std::wstring result;
		result.reserve(36);

		size_t i = 0;
		for (const uint8_t* it_data = cbegin(); it_data != cend(); ++it_data, ++i)
		{
			const size_t hi = ((*it_data) >> 4) & 0x0F;
			result.push_back(details::to_wchar(hi));

			const size_t lo = (*it_data) & 0x0F;
			result.push_back(details::to_wchar(lo));

			if (i == 3 || i == 5 || i == 7 || i == 9)
			{
				result.push_back('-');
			}
		}
		return result;
	}

public:
	uint8_t data[data_size];
};

static_assert(sizeof(uuid) == 16, "");

static bool operator==(const uuid& a, const uuid& b)
{
	return std::equal(a.cbegin(), a.cend(), b.cbegin());
}

struct string_uuid_generator
{
	// Dispatch Functions
	uuid operator()(const char* s) const
	{
		return operator()(s, s + std::strlen(s));
	}
	uuid operator()(const wchar_t* s) const
	{
		return operator()(s, s + std::wcslen(s));
	}

	// Parser Function
	template <typename CharIterator>
	uuid operator()(CharIterator begin, CharIterator end) const
	{
		typedef typename std::iterator_traits<CharIterator>::value_type char_t;

		// check open brace
		char_t c = get_next_char(begin, end);
		bool has_open_brace = is_open_brace(c);
		char_t open_brace_char = c;
		if (has_open_brace) {
			c = get_next_char(begin, end);
		}

		bool has_dashes = false;

		uuid u;
		int i = 0;
		for (uint8_t* it_byte = u.begin(); it_byte != u.end(); ++it_byte, ++i)
		{
			if (it_byte != u.begin()) {
				c = get_next_char(begin, end);
			}

			if (i == 4) {
				has_dashes = is_dash(c);
				if (has_dashes) {
					c = get_next_char(begin, end);
				}
			}

			if (has_dashes) {
				if (i == 6 || i == 8 || i == 10) {
					if (is_dash(c)) {
						c = get_next_char(begin, end);
					}
					else {
						throw_invalid();
					}
				}
			}

			*it_byte = get_value(c);

			c = get_next_char(begin, end);
			*it_byte <<= 4;
			*it_byte |= get_value(c);
		}

		// check close brace
		if (has_open_brace) {
			c = get_next_char(begin, end);
			check_close_brace(c, open_brace_char);
		}

		return u;
	}

private:
	template <typename CharIterator>
	typename std::iterator_traits<CharIterator>::value_type
		get_next_char(CharIterator& begin, CharIterator end) const
	{
		if (begin == end)
		{
			throw_invalid();
		}
		return *begin++;
	}

	unsigned char get_value(char c) const {
		static char const*const digits_begin = "0123456789abcdefABCDEF";
		static char const*const digits_end = digits_begin + 22;

		static unsigned char const values[] =
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 10, 11, 12, 13, 14, 15
		, static_cast<unsigned char>(-1) };

		char const* d = std::find(digits_begin, digits_end, c);
		return values[d - digits_begin];
	}

	unsigned char get_value(wchar_t c) const {
		static wchar_t const*const digits_begin = L"0123456789abcdefABCDEF";
		static wchar_t const*const digits_end = digits_begin + 22;

		static unsigned char const values[] =
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 10, 11, 12, 13, 14, 15
		, static_cast<unsigned char>(-1) };

		wchar_t const* d = std::find(digits_begin, digits_end, c);
		return values[d - digits_begin];
	}

	bool is_dash(char c) const {
		return c == '-';
	}

	bool is_dash(wchar_t c) const {
		return c == L'-';
	}

	// return closing brace
	bool is_open_brace(char c) const {
		return (c == '{');
	}

	bool is_open_brace(wchar_t c) const {
		return (c == L'{');
	}

	void check_close_brace(char c, char open_brace) const {
		if (open_brace == '{' && c == '}') {
			//great
		}
		else {
			throw_invalid();
		}
	}

	void check_close_brace(wchar_t c, wchar_t open_brace) const {
		if (open_brace == L'{' && c == L'}') {
			// great
		}
		else {
			throw_invalid();
		}
	}

	void throw_invalid() const;
};

template<class random_generator_t>
struct random_uuid_generator
{
public:
	uuid operator()(random_generator_t& random)
	{
		uuid u;

		int i = 0;
		unsigned long random_value = random();

		for (uint8_t* it = u.begin(); it != u.end(); ++it, ++i)
		{
			if (i == sizeof(unsigned long))
			{
				random_value = random();
				i = 0;
			}

			*it = static_cast<uint8_t>((random_value >> (i * 8)) & 0xFF);
		}

		return u;
	}
};

#endif