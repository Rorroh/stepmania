#include "RageString.hpp"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <algorithm>
#include "RageUnicode.hpp"

void make_upper( char *p, size_t len );
void make_lower( char *p, size_t len );

std::string Rage::head(std::string const &source, int32_t const length)
{
	if (std::abs(length) >= source.size())
	{
		return source;
	}
	if (length < 0)
	{
		return source.substr(0, source.size() + length);
	}
	return source.substr(0, length);
}

std::string Rage::tail(std::string const &source, int32_t const length)
{
	if (std::abs(length) >= source.size())
	{
		return source;
	}
	if (length < 0)
	{
		return source.substr(-length);
	}
	return source.substr(source.size() - length);
}

bool Rage::starts_with(std::string const &source, std::string const &target)
{
	return Rage::head(source, target.size()) == target;
}

bool Rage::ends_with(std::string const &source, std::string const &target)
{
	return Rage::tail(source, target.size()) == target;
}

std::string Rage::hexify(wchar_t const src, unsigned int dstlen)
{
	static char const alphabet[] = "0123456789abcdef";

	std::stringstream builder;
	
	unsigned int i = 0;
	wchar_t const *ptr = &src;
	
	while (*ptr && ( 2 * i ) + 1 < dstlen)
	{
		builder << alphabet[*ptr / 16];
		builder << alphabet[*ptr % 16];
		++ptr;
		++i;
	}
	
	std::string setup{builder.str()};
	while (setup.size() < dstlen)
	{
		setup = '0' + setup;
	}
	return setup;
}

void Rage::replace(std::string &target, char from, char to)
{
	std::replace(target.begin(), target.end(), from, to);
}

void Rage::replace(std::string &target, std::string const &from, std::string const &to)
{
	std::string newString;
	newString.reserve(target.length());  // avoids a few memory allocations

	std::string::size_type lastPos = 0;
	std::string::size_type findPos;

	while (std::string::npos != (findPos = target.find(from, lastPos)))
	{
		newString.append(target, lastPos, findPos - lastPos);
		newString += to;
		lastPos = findPos + from.length();
	}

	// Care for the rest after last occurrence
	newString += target.substr(lastPos);

	target.swap(newString);
}

std::string Rage::join(std::string const &delimiter, std::vector<std::string> const &source)
{
	if (source.empty())
	{
		return "";
	}
	return Rage::join(delimiter, source.begin(), source.end());
}

std::string Rage::join(std::string const &delimiter, std::vector<std::string>::const_iterator start, std::vector<std::string>::const_iterator finish )
{
	if (start == finish)
	{
		return "";
	}
	std::stringstream builder;
	
	auto append = [&builder, &delimiter](std::string const &target) {
		builder << target;
		builder << delimiter;
	};
	auto inclusive = finish - 1;
	std::for_each(start, inclusive, append);
	
	builder << *inclusive;
	return builder.str();
}

std::string Rage::trim_left(std::string const &source)
{
	return Rage::trim_left(source, "\r\n\t ");
}

std::string Rage::trim_left(std::string const &source, std::string const &delimiters)
{
	auto n = 0;
	auto end = source.size();
	auto const *d_str = delimiters.c_str();
	while (n < end && std::strchr(d_str, source[n]))
	{
		++n;
	}
	return source.substr(n);
}

std::string Rage::trim_right(std::string const &source)
{
	return Rage::trim_right(source, "\r\n\t ");
}

std::string Rage::trim_right(std::string const &source, std::string const &delimiters)
{
	int n = source.size();
	auto const *d_str = delimiters.c_str();
	while( n > 0 && std::strchr(d_str, source[n - 1]) )
	{
		n--;
	}
	
	return source.substr(0, n);
}

std::string Rage::trim(std::string const &source)
{
	return Rage::trim(source, "\r\n\t ");
}

std::string Rage::trim(std::string const &source, std::string const &delimiters)
{
	std::string::size_type start = 0;
	std::string::size_type lastPos = source.size();
	auto const *d_str = delimiters.c_str();
	while ( start < lastPos && std::strchr(d_str, source[start]))
	{
		++start;
	}
	while (start < lastPos && std::strchr(d_str, source[lastPos - 1]))
	{
		--lastPos;
	}
	return source.substr(start, lastPos - start);
}

std::string Rage::base_name(std::string const &dir)
{
	size_t iEnd = dir.find_last_not_of( "/\\" );
	if( iEnd == dir.npos )
	{
		return "";
	}
	size_t iStart = dir.find_last_of( "/\\", iEnd );
	if( iStart == dir.npos )
	{
		iStart = 0;
	}
	else
	{
		++iStart;
	}
	return dir.substr( iStart, iEnd-iStart+1 );
}

std::string Rage::dir_name(std::string const &dir)
{
	// Special case: "/" -> "/".
	if( dir.size() == 1 && dir[0] == '/' )
	{
		return "/";
	}
	int pos = dir.size()-1;
	// Skip trailing slashes.
	while( pos >= 0 && dir[pos] == '/' )
	{
		--pos;
	}
	// Skip the last component.
	while( pos >= 0 && dir[pos] != '/' )
	{
		--pos;
	}
	if( pos < 0 )
	{
		return "./";
	}
	return dir.substr(0, pos+1);
}

/* Branch optimizations: */
#if defined(__GNUC__) || defined(__clang__)
#define likely(x) (__builtin_expect(!!(x), 1))
#define unlikely(x) (__builtin_expect(!!(x), 0))
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

int unicode_do_casing( char *p, size_t iLen, const unsigned char pMapping[256] )
{
	// Note: this has problems with certain accented characters. -aj
	wchar_t wc = L'\0';
	unsigned iStart = 0;
	if( !Rage::utf8_to_wchar(p, iLen, iStart, wc) )
		return 1;
	
	wchar_t iUpper = wc;
	if( wc < 256 )
		iUpper = pMapping[wc];
	if( iUpper != wc )
	{
		std::string sOut;
		Rage::wchar_to_utf8( iUpper, sOut );
		if( sOut.size() == iStart )
		{
			std::memcpy( p, sOut.data(), sOut.size() );
		}
		else
		{
			// TODO: Find another
			// WARN( fmt::sprintf("UnicodeDoUpper: invalid character at \"%s\"", RString(p,iLen).c_str()) );
		}
	}
	
	return iStart;
}

void make_upper( char *p, size_t len )
{
	char *start = p;
	char *end = p + len;
	while( p < end )
	{
		// Fast path:
		if( likely( !(*p & 0x80) ) )
		{
			if( unlikely(*p >= 'a' && *p <= 'z') )
				*p += 'A' - 'a';
			++p;
			continue;
		}
		
		int iRemaining = len - (p - start);
		p += unicode_do_casing( p, iRemaining, Rage::upperCase );
	}
}

void make_lower( char *p, size_t len )
{
	char *start = p;
	char *end = p + len;
	while( p < end )
	{
		// Fast path:
		if( likely( !(*p & 0x80) ) )
		{
			if( unlikely(*p >= 'A' && *p <= 'Z') )
				*p -= 'A' - 'a';
			++p;
			continue;
		}
		
		int iRemaining = len - (p - start);
		p += unicode_do_casing( p, iRemaining, Rage::lowerCase );
	}
}

std::string Rage::make_upper(std::string const &source)
{
	std::vector<char> buffer{source.begin(), source.end()};
	
	// Ensure a null terminating character is in place just in case.
	buffer.push_back(0);
	
	::make_upper(&buffer[0], source.size());
	
	return std::string{buffer.begin(), buffer.end() - 1};
}

std::string Rage::make_lower(std::string const &source)
{
	std::vector<char> buffer{source.begin(), source.end()};
	
	// Ensure a null terminating character is in place just in case.
	buffer.push_back(0);
	
	::make_lower(&buffer[0], source.size());
	
	return std::string{buffer.begin(), buffer.end() - 1};
}
