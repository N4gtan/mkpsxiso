#include "cue.h"
#include "platform.h"

bool multiBinSeeker(const unsigned int sector, const cd::IsoDirEntries::Entry &entry, cd::IsoReader &reader, const CueFile &cueFile)
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

CueFile parseCueFile(fs::path& inputFile)
{
	CueFile cueFile;
	std::string fileType;
	unique_file file = OpenScopedFile(inputFile, "r");
	fs::path filePath = inputFile;
	int fileSectors;
	int lineNumber = 1;
	int pregapSectors = 150;
	int pregapStartSector = 0;
	int previousStartSector = 0;

	char buffer[1024];
	while (std::fgets(buffer, sizeof(buffer), file.get()) != nullptr)
	{
		std::string_view line(buffer);
		if (line.find("FILE") != std::string::npos)
		{

			if (!cueFile.tracks.empty())
			{
				TrackInfo &lastTrack = cueFile.tracks.back();
				lastTrack.sizeInSectors = cueFile.totalSectors - lastTrack.startSector;
				lastTrack.endSector = lastTrack.startSector + lastTrack.sizeInSectors;
				cueFile.multiBIN = true;
			}

			size_t firstQuote = line.find('"');
			size_t lastQuote = line.rfind('"');
			std::string fileName(line.substr(firstQuote + 1, lastQuote - firstQuote - 1));
			filePath.replace_filename(fileName);
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
		else if (line.find("TRACK") != std::string::npos)
		{
			TrackInfo track{};
			size_t trackNumStart = line.find("TRACK") + 6;
			track.number = line.substr(trackNumStart, 2);
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
		else if (line.find("INDEX 01") != std::string::npos)
		{
			size_t timeStart = line.find("INDEX 01") + 9;
			std::string startTime(line.substr(timeStart, line.find_first_of("\r\n") - timeStart));
			int startSector = TimecodeToSectors(startTime);
			if (startSector < 0)
			{
				printf("Error: Invalid cue file timecode \"%s\" on line %d\n", startTime.c_str(), lineNumber);
				exit(EXIT_FAILURE);
			}

			if (cueFile.multiBIN)
			{
				pregapSectors = startSector;
				startSector = cueFile.totalSectors - fileSectors + pregapSectors;
				pregapStartSector = startSector - pregapSectors;
				startTime = SectorsToTimecode(startSector);
				pregapSectors = 150;
			}
			else if (pregapStartSector <= 0)
			{
				pregapStartSector = startSector - pregapSectors;
				pregapSectors = 150;
			}

			if (cueFile.tracks.size() > 1)
			{
				cueFile.tracks[cueFile.tracks.size() - 2].sizeInSectors = pregapStartSector - previousStartSector;
				cueFile.tracks[cueFile.tracks.size() - 2].endSector = pregapStartSector;
				pregapStartSector = 0;
			}

			cueFile.tracks.back().startTime = startTime;
			cueFile.tracks.back().startSector = startSector;
			previousStartSector = startSector;
		}
		else if (!cueFile.multiBIN)
		{
			if (line.find("INDEX 00") != std::string::npos)
			{
				size_t timeStart = line.find("INDEX 00") + 9;
				std::string startTime(line.substr(timeStart, line.find_first_of("\r\n") - timeStart));
				pregapStartSector = TimecodeToSectors(startTime);
				if (pregapStartSector < 0)
				{
					printf("Error: Invalid cue file timecode \"%s\" on line %d\n", startTime.c_str(), lineNumber);
					exit(EXIT_FAILURE);
				}
			}
			else if (line.find("PREGAP") != std::string::npos)
			{
				size_t timeStart = line.find("PREGAP") + 7;
				std::string pregapTime(line.substr(timeStart, line.find_first_of("\r\n") - timeStart));
				pregapSectors = TimecodeToSectors(pregapTime);
				if (pregapSectors < 0)
				{
					printf("Error: Invalid cue file timecode \"%s\" on line %d\n", pregapTime.c_str(), lineNumber);
					exit(EXIT_FAILURE);
				}
			}
		}
		// Silently skip unsupported commands.
		// TODO: Support indexes > 01 if a real-world PSX case appears.

		lineNumber++;
	}

	if (!cueFile.tracks.empty())
	{
		TrackInfo &lastTrack = cueFile.tracks.back();
		lastTrack.sizeInSectors = cueFile.totalSectors - lastTrack.startSector;
		lastTrack.endSector = lastTrack.startSector + lastTrack.sizeInSectors;
	}

	return cueFile;
}
