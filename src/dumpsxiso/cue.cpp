#include "cue.h"
#include "common.h"

static std::vector<std::string_view> tokenizeLine(char* buffer, int lineNumber)
{
	std::string_view line(buffer);
	std::vector<std::string_view> tokens;
	tokens.reserve(3);

	size_t pos = 0;
	constexpr std::string_view delimiters = " \t\r\n";
	while ((pos = line.find_first_not_of(delimiters, pos)) != std::string_view::npos) [[likely]]
	{
		size_t endPos;
		if (line[pos] == '"')
		{
			endPos = line.find('"', ++pos);
			if (endPos == std::string_view::npos) [[unlikely]]
			{
				printf("Error: Missing quotation on line %d\n", lineNumber);
				exit(EXIT_FAILURE);
			}
		}
		else
		{
			endPos = line.find_first_of(delimiters, pos);
			if (endPos == std::string_view::npos) [[unlikely]]
			{
				tokens.push_back(line.substr(pos));
				break;
			}
		}
		buffer[endPos] = '\0';
		tokens.push_back(line.substr(pos, endPos - pos));
		pos = endPos + 1;
	}

	return tokens;
}

const fs::path& cue::Load(const fs::path& inputFile)
{
	cueFile = {};
	if (!CompareICase(inputFile.extension().string(), ".cue"))
		return inputFile;

	unique_file fp = OpenScopedFile(inputFile, "rb");
	if (fp == nullptr) [[unlikely]]
		return inputFile;

	int cmdBitFlag = -1;
	int idx0Sector = 0; // INDEX 00 command
	int lineNumber = 1;
	int pregapSize = 0; // PREGAP command
	int virtualGap = 0; // Cumulative PREGAP sectors
	FileInfo* file = nullptr;
	TrackInfo* track = nullptr;
	const fs::path dirPath = inputFile.parent_path();

	auto finalizeTrack = [](TrackInfo& t, const uint32_t nextPregapLBA) -> void
	{
		t.length = nextPregapLBA - t.begLBA;
		t.endLBA = nextPregapLBA - 1;
	};

	auto parseCueTime = [lineNumber](const char* timecode) -> int
	{
		int sectors = TimecodeToSectors(timecode);
		if (sectors < 0) [[unlikely]]
		{
			printf("Error: Invalid CUE file timecode \"%s\" on line %d\n", timecode, lineNumber);
			exit(EXIT_FAILURE);
		}
		return sectors;
	};

	char buffer[1024];
	for (; std::fgets(buffer, sizeof(buffer), fp.get()) != nullptr; ++lineNumber) [[likely]]
	{
		auto tokens = tokenizeLine(buffer, lineNumber);

		if (tokens.size() < 3) [[unlikely]]
		{
			if (tokens.size() == 2)
				goto pregap;

			continue;
		}

		if (CompareICase(tokens[0], "INDEX"))
		{
			if (track == nullptr) [[unlikely]]
			{
				printf("Error: INDEX command appears before any TRACK declaration on line %d\n", lineNumber);
				exit(EXIT_FAILURE);
			}

			const int sector = file->begSector - virtualGap + parseCueTime(tokens[2].data());
			if (tokens[1] == "01" || tokens[1] == "1")
			{
				if (cmdBitFlag & 1) [[unlikely]]
				{
					printf("Error: Duplicated INDEX 01 command for TRACK %s on line %d\n", track->number, lineNumber);
					exit(EXIT_FAILURE);
				}
				cmdBitFlag |= 1;

				if (const size_t size = cueFile.tracks.size(); size > 1)
				{
					auto& prevTrack = cueFile.tracks[size - 2];

					// Only true for the 2nd+ track within the same FILE command.
					if (prevTrack.length == 0)
						finalizeTrack(prevTrack, (idx0Sector <= static_cast<int>(prevTrack.begLBA)) ? (sector - pregapSize) : idx0Sector);

					track->gapLBA = prevTrack.endLBA + 1;
					track->begLBA = sector;
				}
				else // Track 01
				{
					virtualGap	  = sector;
					track->gapLBA = -sector;
					cueFile.totalLBA -= sector;
				}
				track->file   = file;
				track->time   = SectorsToTimecode(track->begLBA);
				track->offset = virtualGap;

				idx0Sector = -1; // Reset
				pregapSize = 0; // Reset
			}
			else if (tokens[1] == "00" || tokens[1] == "0")
			{
				if (cmdBitFlag != 0) [[unlikely]]
				{
					printf("Error: Invalid INDEX 00 usage for TRACK %s on line %d\n", track->number, lineNumber);
					exit(EXIT_FAILURE);
				}
				cmdBitFlag |= 2;
				idx0Sector = sector;
			}
		}
		else if (CompareICase(tokens[0], "TRACK"))
		{
			if (file == nullptr) [[unlikely]]
			{
				printf("Error: TRACK command appears before any FILE declaration on line %d\n", lineNumber);
				exit(EXIT_FAILURE);
			}

			cmdBitFlag = 0;
			track = &cueFile.tracks.emplace_back();
			tokens[1].copy(track->number, sizeof(track->number) - 1);
			track->type = CompareICase(tokens[2], "AUDIO") ? "AUDIO" : tokens[2];
		}
		else if (CompareICase(tokens[0], "FILE"))
		{
			if (track != nullptr && track->file != nullptr)
			{
				finalizeTrack(*track, cueFile.totalLBA);
				cueFile.multiBIN = true;
			}

			fs::path filePath = (dirPath / reinterpret_cast<const char8_t*>(tokens[1].data())).lexically_normal();
			const int64_t fileSize = GetSize(filePath);
			if (fileSize < 0) [[unlikely]]
			{
				printf("Error: Failed to get the file size for \"%" PRFILESYSTEM_PATH "\"\n", filePath.c_str());
				exit(EXIT_FAILURE);
			}
			if (fileSize % CD_SECTOR_SIZE != 0) [[unlikely]]
			{
				printf("Error: File size for \"%" PRFILESYSTEM_PATH "\" is not a multiple of 2352\n", filePath.c_str());
				exit(EXIT_FAILURE);
			}
			const int totalSec  = static_cast<int>(fileSize / CD_SECTOR_SIZE);
			cueFile.totalLBA   += totalSec;

			const int begSector = file != nullptr ? file->endSector + 1 : 0;
			file = &cueFile.files.emplace_back(std::move(filePath));
			file->type			= tokens[2];
			file->begSector		= begSector;
			file->endSector		= begSector + totalSec - 1;
		}
		else [[unlikely]]
		{
		pregap:
			if (CompareICase(tokens[0], "PREGAP"))
			{
				if (track == nullptr) [[unlikely]]
				{
					printf("Error: PREGAP command appears before any TRACK declaration on line %d\n", lineNumber);
					exit(EXIT_FAILURE);
				}
				if (cmdBitFlag != 0) [[unlikely]]
				{
					printf("Error: Invalid PREGAP usage for TRACK %s on line %d\n", track->number, lineNumber);
					exit(EXIT_FAILURE);
				}
				cmdBitFlag |= 4;
				pregapSize  = parseCueTime(tokens[1].data());
				virtualGap -= pregapSize;
				cueFile.totalLBA += pregapSize;
			}
		}
		// Silently skip unsupported commands.
		// TODO: Support indexes > 01 if a real-world PSX case appears.
	}

	if (track == nullptr) [[unlikely]]
	{
		printf("Error: Invalid CUE file \"%" PRFILESYSTEM_PATH "\", no TRACK found\n", inputFile.c_str());
		exit(EXIT_FAILURE);
	}

	finalizeTrack(*track, cueFile.totalLBA);

	// The first track in the cue MUST be the main DATA one
	return cueFile.tracks.front().file->path;
}
