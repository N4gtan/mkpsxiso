#pragma once

#include "cd.h"
#include "ghc/fs_std.hpp"
#include <optional>
#include <cstring>

enum class EntryType
{
	EntryDir,
	EntryFile,
	EntryXA,
	EntryXA_DO,
	EntryDA,
	EntryDummy
};

class EntryAttributes
{
private:
	static constexpr signed char DEFAULT_GMTOFFS = 36;
	static constexpr unsigned char DEFAULT_HIDDEN_FLAG = 0;
	static constexpr unsigned char DEFAULT_XAATRIB = 0xFF;
	static constexpr unsigned short DEFAULT_XAPERM = 0x555; // rx
	static constexpr unsigned short	DEFAULT_OWNER_ID = 0;
	static constexpr signed short	DEFAULT_ORDER = -1;
	static constexpr unsigned short	DEFAULT_FORCE_LBA = 0;

public:
	signed char GMTOffs = DEFAULT_GMTOFFS;
	unsigned char HFLAG = DEFAULT_HIDDEN_FLAG;
	unsigned char XAAttrib = DEFAULT_XAATRIB;
	unsigned short XAPerm = DEFAULT_XAPERM;
	unsigned short GID = DEFAULT_OWNER_ID;
	unsigned short UID = DEFAULT_OWNER_ID;
	signed short ORDER = DEFAULT_ORDER;
	unsigned int FLBA = DEFAULT_FORCE_LBA;
};

// Shared by mkpsxiso and dumpsxiso
namespace global
{
	extern std::optional<bool> cdvd_style;
}

// Helper functions for datestamp manipulation
bool ParseDateFromString(cd::ISO_DATESTAMP& result, const char* str, char defaultGMT = 36);
bool ParseLongDateFromString(cd::ISO_LONG_DATESTAMP& result, const char* str, char defaultGMT = 36);
cd::ISO_LONG_DATESTAMP GetUnspecifiedLongDate();
std::string DateToString(const cd::ISO_DATESTAMP& src, bool ext);
std::string LongDateToString(const cd::ISO_LONG_DATESTAMP& src);

// Helper functions for sector conversion
uint32_t GetSizeInSectors(uint64_t size, uint32_t sectorSize = F1_DATA_SIZE);
int32_t TimecodeToSectors(const std::string_view timecode);
std::string SectorsToTimecode(const unsigned sectors);

// Endianness swap
unsigned short SwapBytes16(unsigned short val);
unsigned int SwapBytes32(unsigned int val);

// Scoped helpers for a few resources
struct file_deleter
{
	void operator()(FILE* file) const noexcept
	{
		std::fclose(file);
	}
};
using unique_file = std::unique_ptr<FILE, file_deleter>;
unique_file OpenScopedFile(const fs::path& path, const char* mode);

// Helper functions for string manipulation
bool CompareICase(std::string_view strLeft, std::string_view strRight);
std::string_view U8ToSv(std::u8string_view str);

template <size_t BuffSize = 256, typename... Args>
void FormatTo(std::string& dst, const char* fmt, Args&&... args)
{
	char buf[BuffSize];
	const int len = snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
	if (len > 0) [[likely]]
	{
		dst.append(buf, std::min<size_t>(len, sizeof(buf) - 1));
	}
}

template <size_t Reserve = 0, typename... Args>
std::string Format(const char* fmt, Args&&... args)
{
	std::string result;
	if constexpr (Reserve > 0)
	{
		result.reserve(Reserve);
	}
	FormatTo(result, fmt, std::forward<Args>(args)...);
	return result;
}

// Argument parsing
bool ParseArgument(char** argv, std::string_view command, std::string_view longCommand = std::string_view{});
std::optional<fs::path> ParsePathArgument(char**& argv, std::string_view command, std::string_view longCommand = std::string_view{});
std::optional<std::string> ParseStringArgument(char**& argv, std::string_view command, std::string_view longCommand = std::string_view{});
