#pragma once

#include "platform.h"
#include <deque>
#include <vector>

namespace cue
{
struct FileInfo
{
	fs::path	path;
	std::string type;
	uint32_t	begSector;
	uint32_t	endSector;
};

struct TrackInfo
{
	FileInfo*	file;
	std::string type;
	std::string time;
	char		number[4];
	int32_t 	offset;
	int32_t 	gapLBA;
	uint32_t	begLBA;
	uint32_t	length;
	uint32_t	endLBA;
};

struct CueInfo
{
	bool multiBIN = false;
	uint32_t totalLBA = 0;
	std::deque<FileInfo> files;
	std::vector<TrackInfo> tracks;
};
inline CueInfo cueFile;

const fs::path& Load(const fs::path& inputFile);
}
