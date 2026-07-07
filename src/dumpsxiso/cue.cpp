#include "cue.h"
#include "platform.h"

bool cue::multiBinSeeker(const unsigned int sector, const cd::IsoDirEntries::Entry &entry, cd::IsoReader &reader, const CueFile &cueFile)
{
	int trackIndex = (entry.trackid.empty() ? std::stoi(entry.identifier.substr(6, 2)) : std::stoi(entry.trackid)) - 1;
	if (trackIndex < 1 || trackIndex >= static_cast<int>(cueFile.tracks.size()))
	{
		printf("Error: Invalid cue TRACK index \"%02d\" for AUDIO file.\n", trackIndex + 1);
		exit(EXIT_FAILURE);
	}
	reader.Open(cueFile.tracks[trackIndex].filePath);
	return reader.SeekToSector(sector - cueFile.tracks[trackIndex - 1].endSector);
}

cue::CueFile cue::parseCueFile(fs::path& inputFile)
{
	unique_file file = OpenScopedFile(inputFile, "r");
	if (file == nullptr)
	{
		printf("ERROR: Cannot open file \"%s\"\n", inputFile.string().c_str());
		exit(EXIT_FAILURE);
	}

	CueFile cueFile;
	std::string fileType;
	fs::path filePath = inputFile;
	int fileSectors;
	int lineNumber = 1;
	int pregapSectors = 150;
	int pregapStartSector = 0;
	int previousStartSector = 0;

	auto finalizeTrack = [&cueFile](TrackInfo &lastTrack) -> void
	{
		lastTrack.sizeInSectors = cueFile.totalSectors - lastTrack.startSector;
		lastTrack.endSector = lastTrack.startSector + lastTrack.sizeInSectors;
	};
	
	auto parseCueTime = [lineNumber](std::string_view line, const size_t offset) -> int
	{
		std::string timecode(line.substr(offset, line.find_first_of("\r\n") - offset));
		int sectors = TimecodeToSectors(timecode);
		if (sectors < 0)
		{
			printf("Error: Invalid cue file timecode \"%s\" on line %d\n", timecode.c_str(), lineNumber);
			exit(EXIT_FAILURE);
		}
		return sectors;
	};

	char buffer[1024];
	while (std::fgets(buffer, sizeof(buffer), file.get()) != nullptr)
	{
		size_t commandPos;
		std::string_view line(buffer);

		if (commandPos = line.find("FILE"); commandPos != std::string::npos)
		{
			if (!cueFile.tracks.empty())
			{
				finalizeTrack(cueFile.tracks.back());
				cueFile.multiBIN = true;
			}

			size_t firstQuote = line.find('"');
			size_t lastQuote = line.rfind('"');
			std::string fileName(line.substr(firstQuote + 1, lastQuote - firstQuote - 1));
			filePath.replace_filename(reinterpret_cast<const char8_t*>(fileName.c_str()));
			if (int64_t fileSize = GetSize(filePath); fileSize < 0)
			{
				printf("Error: Failed to get the file size for \"%s\"\n", fileName.c_str());
				exit(EXIT_FAILURE);
			}
			else
			{
				if (fileSize % CD_SECTOR_SIZE != 0)
				{
					printf("Error: File size for \"%s\" is not a multiple of 2352\n", fileName.c_str());
					exit(EXIT_FAILURE);
				}
				fileSectors = static_cast<int>(fileSize / CD_SECTOR_SIZE);
				cueFile.totalSectors += fileSectors;
			}

			if (line.find("BINARY") != std::string::npos)
			{
				fileType = "BINARY";
			}
			else
			{
				fileType = "UNKNOWN";
			}

			// We set inputFile to the first track entry in the cue because that's the main DATA file
			if (cueFile.tracks.empty())
			{
				inputFile = filePath;
			}
		}
		else if (commandPos = line.find("TRACK"); commandPos != std::string::npos)
		{
			TrackInfo track{};
			track.number = line.substr(commandPos + sizeof("TRACK"), 2);
			track.filePath = filePath;
			track.fileType = fileType;

			if (line.find("AUDIO") != std::string::npos)
			{
				track.type = "AUDIO";
			}
			else if (line.find("MODE2/2352") != std::string::npos)
			{
				track.type = "MODE2/2352";
			}
			else
			{
				track.type = "UNKNOWN";
			}

			cueFile.tracks.push_back(track);
		}
		else if (commandPos = line.find("INDEX 01"); commandPos != std::string::npos)
		{
			int startSector = parseCueTime(line, commandPos + sizeof("INDEX 01"));

			std::string startTime;
			if (cueFile.multiBIN)
			{
				pregapSectors = startSector;
				startSector = cueFile.totalSectors - fileSectors + pregapSectors;
				pregapStartSector = startSector - pregapSectors;
				startTime = SectorsToTimecode(startSector);
				pregapSectors = 150; // Reset
			}
			else if (pregapStartSector <= 0)
			{
				pregapStartSector = startSector - pregapSectors;
				pregapSectors = 150; // Reset
			}

			if (cueFile.tracks.size() > 1)
			{
				cueFile.tracks[cueFile.tracks.size() - 2].sizeInSectors = pregapStartSector - previousStartSector;
				cueFile.tracks[cueFile.tracks.size() - 2].endSector = pregapStartSector;
				pregapStartSector = 0; // Reset
			}

			cueFile.tracks.back().startTime = startTime;
			cueFile.tracks.back().startSector = startSector;
			previousStartSector = startSector;
		}
		else if (!cueFile.multiBIN)
		{
			if (commandPos = line.find("INDEX 00"); commandPos != std::string::npos)
			{
				pregapStartSector = parseCueTime(line, commandPos + sizeof("INDEX 00"));
			}
			else if (commandPos = line.find("PREGAP"); commandPos != std::string::npos)
			{
				pregapSectors = parseCueTime(line, commandPos + sizeof("PREGAP"));
			}
		}
		// Silently skip unsupported commands.
		// TODO: Support indexes > 01 if a real-world PSX case appears.

		lineNumber++;
	}

	if (!cueFile.tracks.empty())
	{
		finalizeTrack(cueFile.tracks.back());
	}

	return cueFile;
}
