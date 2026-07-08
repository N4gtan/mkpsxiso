#include "platform.h"
#include "xmlwriter.h"
#include "cue.h"

#ifndef MKPSXISO_NO_LIBFLAC
#include "FLAC/stream_encoder.h"
#endif

typedef enum {
	EAF_WAV  = 1 << 0,
	EAF_FLAC = 1 << 1,
	EAF_PCM  = 1 << 2
} EncoderAudioFormats;

typedef struct {
	const char *codec;
	EncoderAudioFormats eaf;
	const char *notcompiledmessage;
} EncodingCodec;

static const EncodingCodec EncodingCodecs[] = {
	{"wave", EAF_WAV, ""},
	{"flac", EAF_FLAC, "ERROR: dumpsxiso was NOT built with libFLAC support!\n"},
	{"pcm",  EAF_PCM, ""}
};

const unsigned BUILTIN_CODECS = (EAF_WAV | EAF_PCM);
#define BUILTIN_CODEC_TEXT "wave, pcm"
#ifndef MKPSXISO_NO_LIBFLAC
	#define LIBFLAC_SUPPORTED EAF_FLAC
	#define LIBFLAC_CODEC_TEXT ", flac"
#else
    #define LIBFLAC_SUPPORTED 0
	#define LIBFLAC_CODEC_TEXT ""
#endif
const unsigned SUPPORTED_CODECS  = (BUILTIN_CODECS | LIBFLAC_SUPPORTED);
#define SUPPORTED_CODEC_TEXT BUILTIN_CODEC_TEXT LIBFLAC_CODEC_TEXT

namespace param {

    fs::path isoFile;
    fs::path outPath;
    fs::path xmlFile;
	bool dir = false;
	bool lba = false;
	bool raw = false;
	bool force = false;
	bool noxml = false;
	bool noWarns = false;
	bool QuietMode = false;
	bool pathTable = false;
    bool outputSortedByDir = false;
	EncoderAudioFormats encodingFormat = EAF_WAV;
}

namespace global
{
	cue::CueFile cueFile;
	bool ps2 = false;
	bool xa_edc = true;
	std::string licenseFile;
	std::optional<bool> cdvd_style = false;
}

fs::path GetEncodedDAPath(const fs::path& inputPath)
{
	fs::path outputPath(inputPath); 
	if(param::encodingFormat == EAF_WAV)
	{
		outputPath.replace_extension(".WAV");
	}
#ifndef MKPSXISO_NO_LIBFLAC
	else if(param::encodingFormat == EAF_FLAC)
	{
		outputPath.replace_extension(".FLAC");
	}
#endif
	else
	{
		outputPath.replace_extension(".PCM");
	}
	return outputPath;
}

template<size_t N>
void PrintId(const char* label, char (&id)[N])
{
	std::string_view view;

	const std::string_view id_view(id, N);
	const size_t last_char = id_view.find_last_not_of(' ');
	if (last_char != std::string_view::npos)
	{
		view = id_view.substr(0, last_char+1);
	}

	if (!view.empty())
	{
		printf("%s%.*s\n", label, static_cast<int>(view.length()), view.data());
	}
}

void PrintDate(const char* label, const cd::ISO_LONG_DATESTAMP& date)
{
	auto ZERO_DATE = GetUnspecifiedLongDate();
	if (memcmp(&date, &ZERO_DATE, sizeof(date)))
	{
		printf("%s%s\n", label, LongDateToString(date).c_str());
	}
}

void prepareRIFFHeader(cd::RIFF_HEADER* header, int dataSize) {
	memcpy(header->chunkID,"RIFF",4);
	header->chunkSize = 36 + dataSize;
	memcpy(header->format, "WAVE", 4);

	memcpy(header->subchunk1ID, "fmt ", 4);
	header->subchunk1Size = 16;
	header->audioFormat = 1;
	header->numChannels = 2;
	header->sampleRate = 44100;
	header->bitsPerSample = 16;
	header->byteRate = (header->sampleRate * header->numChannels * header->bitsPerSample) / 8;
	header->blockAlign = (header->numChannels * header->bitsPerSample) / 8;

	memcpy(header->subchunk2ID, "data", 4);
	header->subchunk2Size = dataSize;
}

// This will ensure that the EDC remains the same as in the original file. Games built with an old, buggy Sony's mastering tool version
// don't have EDC Form2 data (this can be checked at redump.org) and some games rely on this to do anti-piracy checks like DDR.
const bool CheckEDCXA()
{
	cd::SECTOR_M2F2 sector;
	while (cd::reader->ReadBytesXA(sector.subHead, XA_DATA_SIZE))
	{
		if (sector.subHead[2] & 0x20)
		{
			if (memcmp(sector.edc, "\0\0\0\0", sizeof(sector.edc)) == 0)
			{
				return false;
			}
			return true;
		}
	}
	return true;
}

// Some games from ~2003 apparently were built with a tool newer than Sony CD-ROM Generator 1.40.
// Likely an internal transition tool that shares the Sony CD/DVD-ROM Generator engine layout logic.
// These have different submodes in the descriptor sectors, a correct root year value and entries are not sorted by name.
const bool CheckISOver()
{
	cd::SECTOR_M2F2 sector;
	cd::reader->SeekToSector(15);
	cd::reader->ReadBytesXA(sector.subHead, XA_DATA_SIZE);
	if (sector.subHead[2] & 0x08)
	{
		global::ps2 = true;
	}
	cd::reader->ReadBytesXA(sector.subHead, XA_DATA_SIZE, true);
	if (sector.subHead[2] & 0x01)
	{
		return false;
	}
	return true;
}

std::unique_ptr<cd::ISO_LICENSE> ReadLicense(cd::IsoReader& reader) {
	auto license = std::make_unique<cd::ISO_LICENSE>();

	reader.SeekToSector(0);
	reader.ReadBytesXA(license->data, sizeof(license->data));

	return license;
}

void SaveLicense(const cd::ISO_LICENSE& license) {
	if (!param::QuietMode)
	{
		printf("\n  Creating license data...");
	}

	global::licenseFile = "license_data.dat";
	FILE* outFile = OpenFile(param::outPath / global::licenseFile, "wb");

	if (outFile == NULL)
	{
		printf("\nERROR: Cannot create license file.\n");
        exit(EXIT_FAILURE);
    }
	else if (!param::QuietMode)
	{
		printf(" Ok.\n");
	}

    fwrite(license.data, 1, sizeof(license.data), outFile);
    fclose(outFile);
}

void writePCMFile(FILE *outFile, cd::IsoReader& reader, const size_t cddaSize, const bool isInvalid)
{
	constexpr size_t bufferSize = 64 * 1024; // Use a 64KiB buffer for better I/O performance
	unsigned char copyBuff[bufferSize]{};
	size_t bytesLeft = cddaSize;
	while (bytesLeft > 0) {

    	size_t bytesToRead = bytesLeft;

    	if (bytesToRead > bufferSize)
    		bytesToRead = bufferSize;

		if (!isInvalid)
    		reader.ReadBytesDA(copyBuff, bytesToRead);

    	fwrite(copyBuff, 1, bytesToRead, outFile);

    	bytesLeft -= bytesToRead;
    }
}

void writeWaveFile(FILE *outFile, cd::IsoReader& reader, const size_t cddaSize, const bool isInvalid)
{
    cd::RIFF_HEADER riffHeader;
    prepareRIFFHeader(&riffHeader, cddaSize);
    fwrite((void*)&riffHeader, 1, sizeof(cd::RIFF_HEADER), outFile);

    writePCMFile(outFile, reader, cddaSize, isInvalid);
}

#ifndef MKPSXISO_NO_LIBFLAC
void writeFLACFile(FILE *outFile, cd::IsoReader& reader, const int cddaSize, const bool isInvalid)
{
	FLAC__bool ok = true;
	FLAC__StreamEncoder *encoder = 0;
	FLAC__StreamEncoderInitStatus init_status;
	if((encoder = FLAC__stream_encoder_new()) == NULL)
	{
		fprintf(stderr, "\nERROR: allocating encoder.\n");
		exit(EXIT_FAILURE);
	}
	unsigned sample_rate = 44100;
	unsigned channels = 2;
	unsigned bps = 16;
	unsigned total_samples = cddaSize / (channels * (bps/8));

	ok &= FLAC__stream_encoder_set_verify(encoder, true);
	ok &= FLAC__stream_encoder_set_compression_level(encoder, 5);
	ok &= FLAC__stream_encoder_set_channels(encoder, channels);
	ok &= FLAC__stream_encoder_set_bits_per_sample(encoder, bps);
	ok &= FLAC__stream_encoder_set_sample_rate(encoder, sample_rate);
	ok &= FLAC__stream_encoder_set_total_samples_estimate(encoder, total_samples);
	if(!ok)
	{
		fprintf(stderr, "\nERROR: setting encoder settings.\n");
		goto writeFLACFile_cleanup;
	}

	init_status = FLAC__stream_encoder_init_FILE(encoder, outFile, /*progress_callback=*/NULL, /*client_data=*/NULL);
	if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
	{
		fprintf(stderr, "\nERROR: initializing encoder: %s.\n", FLAC__StreamEncoderInitStatusString[init_status]);
		ok = false;
		goto writeFLACFile_cleanup;
	}

    {
    size_t left = (size_t)total_samples;
	size_t max_pcmframe_read = CD_SECTOR_SIZE / (channels * (bps/8));

    std::unique_ptr<int32_t[]> pcm(new int32_t[channels * max_pcmframe_read]);
	while (left && ok) {

		unsigned char copyBuff[CD_SECTOR_SIZE]{};

		size_t need = (left > max_pcmframe_read ? max_pcmframe_read : left);
		size_t needBytes = need * (channels * (bps/8));
		if(!isInvalid)
			reader.ReadBytesDA(copyBuff, needBytes);

    	/* convert the packed little-endian 16-bit PCM samples from WAVE into an interleaved FLAC__int32 buffer for libFLAC */
		for(size_t i = 0; i < need*channels; i++)
		{
			/* inefficient but simple and works on big- or little-endian machines */
			pcm[i] = (FLAC__int32)(((FLAC__int16)(FLAC__int8)copyBuff[2*i+1] << 8) | (FLAC__int16)copyBuff[2*i]);
		}
		/* feed samples to encoder */
		ok = FLAC__stream_encoder_process_interleaved(encoder, pcm.get(), need);
		left -= need;
    }
    }
	ok &= FLAC__stream_encoder_finish(encoder); // closes outFile
	if(!ok)
	{
		fprintf(stderr, "\nencoding: FAILED\n");
		fprintf(stderr, "   state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(encoder)]);
	}

writeFLACFile_cleanup:
	FLAC__stream_encoder_delete(encoder);
	if(!ok)
	{
		exit(EXIT_FAILURE);
	}
}
#endif

std::unique_ptr<cd::IsoDirEntries> ParseSubdirectory(cd::IsoReader& reader, ListView<cd::IsoDirEntries::Entry> view, int offs, int sectors,
	const fs::path& path)
{
    auto dirEntries = std::make_unique<cd::IsoDirEntries>(std::move(view));
	dirEntries->ReadDirEntries(&reader, offs, sectors);

    for (auto& e : dirEntries->dirEntryList.GetView())
	{
		auto& entry = e.get();

		entry.virtualPath = path;
        if (entry.entry.flags & 0x2)
		{
            entry.subdir = ParseSubdirectory(reader, dirEntries->dirEntryList.NewView(), entry.entry.entryOffs.lsb, GetSizeInSectors(entry.entry.entrySize.lsb),
				path / entry.identifier);
        }
    }

	return dirEntries;
}

std::unique_ptr<cd::IsoDirEntries> ParsePathTable(cd::IsoReader& reader, ListView<cd::IsoDirEntries::Entry> view, const std::vector<cd::IsoPathTable::Entry>& pathTableList,
	int index, const fs::path& path)
{
    auto dirEntries = std::make_unique<cd::IsoDirEntries>(std::move(view));

	// Calculate Directory Record sector size
	int dirRecordSectors = 0;
	if (*global::cdvd_style && pathTableList.size() != index + 1)
	{
		dirRecordSectors = pathTableList[index + 1].entry.dirOffs - pathTableList[index].entry.dirOffs;
	}
	else
	{
		reader.SeekToSector(pathTableList[index].entry.dirOffs);
		cd::SECTOR_M2F1 sector;
		do {
			dirRecordSectors++;
			reader.ReadBytesXA(sector.subHead, XA_DATA_SIZE);
		} while (!(sector.subHead[2] & 0x81)); // Directory records normally ends with submode 0x89
	}

	dirEntries->ReadDirEntries(&reader, pathTableList[index].entry.dirOffs, dirRecordSectors);

	// Only add the missing directories to the list
    for (int i = 1; i < pathTableList.size(); i++) {
        auto& e = pathTableList[i];
		if (e.entry.parentDirIndex - 1 == index &&
			!std::any_of(dirEntries->dirEntryList.GetView().begin(), dirEntries->dirEntryList.GetView().end(), [&e](const auto& entry)
				{
					return entry.get().identifier == e.name;
				}))
		{
            dirEntries->ReadRootDir(&reader, e.entry.dirOffs);
			dirEntries->dirEntryList.GetView().back().get().entry.flags |= 0x22; // We are setting the reserved 5th bit to simulate obfuscation
        }
    } 

    for (auto& e : dirEntries->dirEntryList.GetView()) {
    		auto& entry = e.get();
										
    		entry.virtualPath = path;

        if (entry.entry.flags & 0x2) {
						int index = -1;
						std::string s;
						for (int i = 1; i < pathTableList.size(); i++) {
								auto& ee = pathTableList[i];
								if (ee.entry.dirOffs == entry.entry.entryOffs.lsb) {
										index = i;
										s = ee.name;
										break;
								}
						}

						if (index < 0) continue;
						entry.identifier = s;
				
						entry.subdir = ParsePathTable(reader, dirEntries->dirEntryList.NewView(), pathTableList, index, path / s);
				}
    }
  
    return dirEntries;  
}

std::unique_ptr<cd::IsoDirEntries> ParseRoot(cd::IsoReader& reader, ListView<cd::IsoDirEntries::Entry> view, const std::vector<cd::IsoPathTable::Entry>& pathTableList)
{
    auto dirEntries = std::make_unique<cd::IsoDirEntries>(std::move(view));
	dirEntries->ReadRootDir(&reader, pathTableList[0].entry.dirOffs);

	if (dirEntries->dirEntryList.GetView().empty())
	{
		printf("\nERROR: Root directory is empty or invalid.\n");
		exit(EXIT_FAILURE);
	}
	auto& entry = dirEntries->dirEntryList.GetView().front().get();
	entry.subdir = !param::pathTable
		? ParseSubdirectory(reader, dirEntries->dirEntryList.NewView(), entry.entry.entryOffs.lsb, GetSizeInSectors(entry.entry.entrySize.lsb), entry.identifier)
		: ParsePathTable(reader, dirEntries->dirEntryList.NewView(), pathTableList, 0, entry.identifier);

	return dirEntries;
}

std::list<cd::IsoDirEntries::Entry*> ParseDAfiles(cd::IsoReader& reader, std::list<cd::IsoDirEntries::Entry>& entries)
{
	std::list<cd::IsoDirEntries::Entry*> DAfiles;
	unsigned tracknum = 2;

	// Get referenced DA files and assign them an ID number
	for(auto& entry : entries)
	{
		if(entry.type == EntryType::EntryDA)
		{
			entry.trackid = std::to_string(100 + tracknum).substr(1);
			tracknum++;
			DAfiles.push_back(&entry);
		}
	}

	if (tracknum <= global::cueFile.tracks.size())
	{
		std::vector<cd::IsoDirEntries::Entry> unrefDAbuff;
		// Create a buffer of unreferenced DA tracks
		for(const auto& track : global::cueFile.tracks)
		{
			// Skip non audio tracks
			if (track.type != "AUDIO")
				continue;
			// Skip referenced DA tracks
			if (tracknum > 2 && std::any_of(DAfiles.begin(), DAfiles.end(), [&track](const auto& entry)
									{
										return entry->entry.entryOffs.lsb == track.startSector;
									}))
			{
				continue;
			}

			// Add the unreferenced DA track to the buffer
			auto& entry = unrefDAbuff.emplace_back();
			entry.entry.entryOffs.lsb = track.startSector;
			entry.entry.entrySize.lsb = track.sizeInSectors * F1_DATA_SIZE;
			entry.identifier = GetEncodedDAPath("TRACK-" + track.number).string();
			entry.type = EntryType::EntryDA;

			// Additional safety check in case the .cue file had a wrong pause size
			// For ex, Mega Man X3 track 30 had 149 sectors pause, but at redump.org says it was a 150 standard one
			unsigned char sectorBuff[CD_SECTOR_SIZE];
			unsigned char emptyBuff[CD_SECTOR_SIZE] {};
			while (reader.SeekToSector(entry.entry.entryOffs.lsb - 1) || multiBinSeeker(entry.entry.entryOffs.lsb - 1, entry, reader, global::cueFile))
			{
				reader.ReadBytesDA(sectorBuff, CD_SECTOR_SIZE, true);
				if (memcmp(sectorBuff, emptyBuff, CD_SECTOR_SIZE) == 0)
					break;

				entry.entry.entryOffs.lsb--;
				entry.entry.entrySize.lsb += F1_DATA_SIZE;
			}
		}

		// Add unreferenced DA tracks to entries for further extraction
		for (auto& entry : unrefDAbuff)
		{
			entries.emplace_back(std::move(entry));
			DAfiles.push_back(&entries.back());
		}

		// Sort DA files by LBA
		DAfiles.sort([](const auto& left, const auto& right)
			{
				return left->entry.entryOffs.lsb < right->entry.entryOffs.lsb;
			});

		// Only recalculate the track id's if there were unreferenced tracks among the referenced ones
		// This is just for a prettier XML sort, because unsorted track id's have no impact at build time
		if (tracknum > 2)
		{
			tracknum = 2;
			for(const auto& entry : DAfiles)
			{
				if(!entry->trackid.empty())
				{
					entry->trackid = std::to_string(100 + tracknum).substr(1);
				}
				tracknum++;
			}
		}

		// Reopen the first file for safety
		reader.Open(global::cueFile.tracks[0].filePath);
	}

	return DAfiles;
}

void BruteForce(cd::IsoReader& reader, std::list<cd::IsoDirEntries::Entry>& entries, unsigned int currentLBA, const unsigned int totalLenLBA)
{
	cd::SECTOR_M2F2 sector;
	int fileCount = 0;

	auto processGaps = [&](const unsigned int endLBA)
	{
		cd::IsoDirEntries::Entry* gapEntry = &entries.front(); // Get root stats
		signed char rootGMTOff = gapEntry->entry.entryDate.GMToffs;
		unsigned short rootGID = gapEntry->extData.ownergroupid;
		unsigned short rootUID = gapEntry->extData.owneruserid;
		unsigned short rootPrm = gapEntry->extData.attributes & cdxa::XA_PERMISSIONS_MASK;
		bool processingFile = false;
		reader.SeekToSector(currentLBA);
		for (; currentLBA < endLBA; currentLBA++)
		{
			reader.ReadBytesXA(sector.subHead, XA_DATA_SIZE);

			// Process only non dummy sectors
			if (!processingFile && sector.subHead[2] != 0x20 && sector.subHead[2] != 0x00)
			{
				processingFile = true;
				gapEntry = &entries.emplace_front();
				gapEntry->entry.entryOffs.lsb 	  = currentLBA;
				gapEntry->entry.entrySize.lsb 	  = F1_DATA_SIZE;
				gapEntry->entry.entryDate.GMToffs = rootGMTOff;
				gapEntry->entry.flags 			  = 0x22; // We are setting the reserved 5th bit to simulate obfuscation
				gapEntry->extData.ownergroupid 	  = rootGID;
				gapEntry->extData.owneruserid 	  = rootUID;
				gapEntry->extData.attributes 	  = rootPrm;
				if ((sector.subHead[2] & 0x7E) == 0x08)
				{
					gapEntry->identifier = "UNKN" + std::to_string(10000 + fileCount++).substr(1) + ".DAT";
					gapEntry->type = EntryType::EntryFile;
				}
				else
				{
					gapEntry->identifier = "UNKN" + std::to_string(10000 + fileCount++).substr(1) + ".STR";
					gapEntry->type = EntryType::EntryXA;
				}
			}
			else if (processingFile)
			{
				gapEntry->entry.entrySize.lsb += F1_DATA_SIZE;
			}

			// Process file until EoF or EoR is set
			if (sector.subHead[2] & 0x81)
				processingFile = false;
		}
	};

	for (const auto& entry : entries)
	{
		if (currentLBA < entry.entry.entryOffs.lsb)
		{
			processGaps(entry.entry.entryOffs.lsb);
		}
		currentLBA += GetSizeInSectors(entry.entry.entrySize.lsb);
	}

	if (currentLBA < totalLenLBA)
	{
		processGaps(totalLenLBA);
	}

	entries.sort([](const auto& left, const auto& right)
		{
			return left.entry.entryOffs.lsb < right.entry.entryOffs.lsb;
		});
}

void ExtractFiles(cd::IsoReader& reader, const std::list<cd::IsoDirEntries::Entry>& files, const fs::path& rootPath)
{
	bool printedDA = false;
    for (const auto& entry : files)
	{
        if (entry.subdir == nullptr) // Do not extract directories, they're already prepared
		{
			const fs::path outputPath = rootPath / entry.virtualPath / entry.identifier;
			if (entry.type == EntryType::EntryXA)
			{
				// Extract XA or STR file.
				// For both XA and STR files, we need to extract the data 2336 bytes per sector.
				// When rebuilding the bin using mkpsxiso, we mark the file with mixed.
				// The source file will anyway be stored on our hard drive in raw form.
				if (!param::QuietMode)
				{
					printf("    Extracting XA \"%" PRFILESYSTEM_PATH "\"... ", outputPath.c_str());
				}
				fflush(stdout);

				FILE* outFile = OpenFile(outputPath, "wb");

				if (outFile == NULL || !reader.SeekToSector(entry.entry.entryOffs.lsb))
				{
					printf("\nERROR: Cannot create file \"%" PRFILESYSTEM_PATH "\"\n", outputPath.filename().c_str());
					exit(EXIT_FAILURE);
				}

				// this is the data to be read 2336 bytes per sector, both if the file is an STR or XA,
				// because the STR contains audio.
				size_t sectorsToRead = GetSizeInSectors(entry.entry.entrySize.lsb);

				// Copy loop
				{
				constexpr size_t bufferSize = 64 * 1024; // Use a 64KiB buffer for better I/O performance
				unsigned char copyBuff[bufferSize];
				auto ptrReadFunc = !param::raw ? &cd::IsoReader::ReadBytesXA : &cd::IsoReader::ReadBytesDA;
				size_t bytesLeft = (!param::raw ? XA_DATA_SIZE : CD_SECTOR_SIZE) * sectorsToRead;
				while(bytesLeft > 0) {

					size_t bytesToRead = bytesLeft;

					if (bytesToRead > bufferSize)
						bytesToRead = bufferSize;

					(reader.*ptrReadFunc)(copyBuff, bytesToRead, false);

					fwrite(copyBuff, 1, bytesToRead, outFile);

					bytesLeft -= bytesToRead;

				}
				}

				fclose(outFile);
			}
			else if (entry.type == EntryType::EntryDA)
			{
				// Extract CDDA file
				if (!printedDA && !param::QuietMode)
				{
					printf("\n  Creating CDDA files...\n");
					printedDA = true;
				}
				bool isInvalid = !global::cueFile.multiBIN
					? !reader.SeekToSector(entry.entry.entryOffs.lsb)
					: !multiBinSeeker(entry.entry.entryOffs.lsb, entry, reader, global::cueFile);
                auto daOutPath = GetEncodedDAPath(outputPath);
				auto outFile = OpenScopedFile(daOutPath, "wb");

				if (isInvalid && !param::noWarns)
				{
					printf( "\nWARNING: The CDDA file \"%" PRFILESYSTEM_PATH "\" is out of the iso file bounds.\n"
							"\t This usually means that the game has audio tracks, and they are on separate files.\n", daOutPath.filename().c_str() );
					if (global::cueFile.tracks.empty())
					{
						printf("\t Try using a .cue file, instead of an ISO image, to be able to access those files.\n");
					}
					printf( "\t DUMPSXISO will write the file as a dummy (silent) cdda file.\n"
							"\t This is generally fine, when the real CDDA file is also a dummy file.\n"
							"\t If it is not dummy, you WILL lose this audio data in the rebuilt iso... " );
					if (param::QuietMode)
					{
						printf("\n");
					}
				}
				else if (!param::QuietMode)
				{
					printf("    Extracting audio \"%" PRFILESYSTEM_PATH "\"... ", daOutPath.c_str());
				}
				fflush(stdout);

				if (!outFile) {
					printf("\nERROR: Cannot create file \"%" PRFILESYSTEM_PATH "\"\n", daOutPath.filename().c_str());
					exit(EXIT_FAILURE);
				}

				size_t sectorsToRead = GetSizeInSectors(entry.entry.entrySize.lsb);
				size_t cddaSize = CD_SECTOR_SIZE * sectorsToRead;

				if(param::encodingFormat == EAF_WAV)
				{
					writeWaveFile(outFile.get(), reader, cddaSize, isInvalid);
				}
#ifndef MKPSXISO_NO_LIBFLAC
				else if(param::encodingFormat == EAF_FLAC)
				{
					// libflac closes outFile
					writeFLACFile(outFile.release(), reader, cddaSize, isInvalid);
				}
#endif
				else
				{
					writePCMFile(outFile.get(), reader, cddaSize, isInvalid);
				}

				if (global::cueFile.multiBIN)
				{
					reader.Open(global::cueFile.tracks[0].filePath);
				}
			}
			else if (entry.type == EntryType::EntryFile)
			{
				// Extract regular file
				if (!param::QuietMode)
				{
					printf("    Extracting \"%" PRFILESYSTEM_PATH "\"... ", outputPath.c_str());
					fflush(stdout);
				}

				reader.SeekToSector(entry.entry.entryOffs.lsb);

				FILE* outFile = OpenFile(outputPath, "wb");

				if (outFile == NULL) {
					printf("\nERROR: Cannot create file \"%" PRFILESYSTEM_PATH "\"\n", outputPath.filename().c_str());
					exit(EXIT_FAILURE);
				}

				{
				constexpr size_t bufferSize = 64 * 1024; // Use a 64KiB buffer for better I/O performance
				unsigned char copyBuff[bufferSize];
				auto ptrReadFunc = !param::raw ? &cd::IsoReader::ReadBytes : &cd::IsoReader::ReadBytesDA;
				size_t bytesLeft = !param::raw ? entry.entry.entrySize.lsb : CD_SECTOR_SIZE * GetSizeInSectors(entry.entry.entrySize.lsb);
				while(bytesLeft > 0) {

					size_t bytesToRead = bytesLeft;

					if (bytesToRead > bufferSize)
						bytesToRead = bufferSize;

					(reader.*ptrReadFunc)(copyBuff, bytesToRead, false);
					fwrite(copyBuff, 1, bytesToRead, outFile);

					bytesLeft -= bytesToRead;

				}
				}

				fclose(outFile);
			}
			else
			{
				if (!param::noWarns)
				{
					printf("WARNING: File %s is of invalid type.\n", entry.identifier.c_str());
				}
				continue;
			}
			if (!param::QuietMode)
			{
				printf("Done.\n");
			}
        }
    }

	// Update timestamps AFTER all files have been extracted
	// else directories will have their timestamps discarded when files are being unpacked into them!
	for (const auto& entry : files)
	{
		if(entry.trackid.empty())
			continue; // Skip unreferenced entries

		fs::path toChange(rootPath / entry.virtualPath / entry.identifier);
		if(entry.type == EntryType::EntryDA)
		{
			toChange = GetEncodedDAPath(toChange);
		}
		UpdateTimestamps(toChange, entry.entry.entryDate);
	}
}

void ParseDIR()
{
	// Limits of LIBCD.H from PSYQ Run-time Library 3.3 and onwards; bypassing these will crash CdSearchFile().
	constexpr int CdlMAXFILE  = 64;	/* max number of files in a directory */
	constexpr int CdlMAXDIR	  = 128;/* max number of total directories */
	constexpr int CdlMAXLEVEL = 8;	/* max levels of directories */
	// These limits assume ISO Level 1 names (8.3), representing internal buffer capacities:
	// 64  = ((2048 - 96) / 60) * 2; 2 directory record sectors for files (96 bytes for "." & ".." overhead, 60 bytes max entry size).
	// 128 = (2048 / 16) * 1;		 1 path table sector for directories (16 bytes max average entry, including the root entry).
	// So, using names longer than 8.3 will decrease this limit even further.

	int postGap	 = 150; // SYSTEM DESCRIPTION CD-ROM XA Ch.II 2.3, postgap should be always >= 150 sectors.
	int dirCount = CdlMAXDIR - 1; // Reserve one for root
	auto ParseSubDIR = [&](auto &&self, ListView<cd::IsoDirEntries::Entry> view, const fs::path &path, int fileCount, int level) -> std::unique_ptr<cd::IsoDirEntries>
	{
		if (level > CdlMAXLEVEL)
		{
			printf("\nERROR: Exceeded maximum directory hierarchy depth levels (%d) at \"%" PRFILESYSTEM_PATH "\"\n", CdlMAXLEVEL, path.c_str());
			exit(EXIT_FAILURE);
		}

		auto dirEntries = std::make_unique<cd::IsoDirEntries>(std::move(view));

		std::error_code ec;
		auto iterator = fs::directory_iterator(path, ec);
		if (ec)
		{
			printf("\nERROR: Cannot read directory \"%" PRFILESYSTEM_PATH "\". %s\n", path.c_str(), ec.message().c_str());
			exit(EXIT_FAILURE);
		}

		for (const auto &fsEntry : iterator)
		{
			auto &entry = dirEntries->dirEntryList.EmplaceBack(cd::IsoDirEntries::Entry
			{
				.identifier  = fsEntry.path().filename().string(),
				.virtualPath = (path == param::outPath) ? fs::path() : path.lexically_proximate(param::outPath),
				.type		 = EntryType::EntryFile
			});

			if (level == 1 && entry.identifier.length() >= 7)
			{
				if (entry.identifier.length() == 10 && CompareICase(entry.identifier, "SYSTEM.CNF"))
				{
					dirEntries->dirEntryList.RotateBack();
				}
				else if (CompareICase(entry.identifier.substr(0, 7), "license"))
				{
					global::licenseFile = entry.identifier;
					dirEntries->dirEntryList.PopBack();
					continue;
				}
			}

			if (fsEntry.is_directory())
			{
				if (--dirCount == -1)
				{
					printf("\nWARNING: Exceeded maximum directories (%d) for LIBCD CdSearchFile() at \"%" PRFILESYSTEM_PATH "\"\n", CdlMAXDIR, path.c_str());
					exit(EXIT_FAILURE);
				}
				entry.type	 = EntryType::EntryDir;
				entry.subdir = self(self, dirEntries->dirEntryList.NewView(), fsEntry.path(), CdlMAXFILE, level + 1);
			}
			else
			{
				if (--fileCount == -1)
				{
					printf("\nWARNING: Exceeded maximum files per directory (%d) for LIBCD CdSearchFile() at \"%" PRFILESYSTEM_PATH "\"\n", CdlMAXFILE, path.c_str());
					exit(EXIT_FAILURE);
				}
				if (std::string ext = fsEntry.path().extension().string(); CompareICase(ext, ".xa") || CompareICase(ext, ".str"))
				{
					entry.type = EntryType::EntryXA;
				}
				else if (CompareICase(ext, ".wav") || CompareICase(ext, ".flac") || CompareICase(ext, ".pcm") || CompareICase(ext, ".mp3"))
				{
					entry.type = EntryType::EntryDA;
					entry.entry.entryOffs.lsb = ++postGap; // Do not change. Used only to avoid calculating deltas at XML write time
				}
			}
		}
		return dirEntries;
	};

	// Create descriptors
	cd::ISO_DESCRIPTOR descriptor{};
	memcpy(&descriptor.volumeID, 			  "MKPSXISO",	 sizeof("MKPSXISO"));
	memcpy(&descriptor.systemID, 			  "PLAYSTATION", sizeof("PLAYSTATION"));
	memcpy(&descriptor.applicationIdentifier, "PLAYSTATION", sizeof("PLAYSTATION"));

	if (!param::QuietMode)
		printf("\nParsing directory \"%" PRFILESYSTEM_PATH "\"... Done.\n", param::outPath.c_str());

	// Create root
	std::list<cd::IsoDirEntries::Entry> entries;
	auto rootDir = std::make_unique<cd::IsoDirEntries>(std::move(ListView(entries)));
	auto &entry  = rootDir->dirEntryList.EmplaceBack(cd::IsoDirEntries::Entry{.type	= EntryType::EntryDir});

	// Parse directory recursively
	entry.subdir = ParseSubDIR(ParseSubDIR, rootDir->dirEntryList.NewView(), param::outPath, CdlMAXFILE, 1);

	// Filter DA files
	auto DAfiles = ParseDAfiles(*cd::reader, entries);

	if (!param::QuietMode)
		printf("Creating XML document... ");

	// Write XML sorted by directories
	param::outputSortedByDir  = true;
	xml::WriteXML(descriptor, rootDir, DAfiles, postGap - DAfiles.size());
	if (!param::QuietMode)
	{
		printf("Done.\n");

		printf( "\n\n----------------------------------------------------\n"
				"Files in the root directory starting with \"license\"\n"
				"are assumed to be disc licenses.\n\n"
				"Files with extension .xa or .str\n"
				"are assumed to be interleaved files.\n\n"
				"Files with extension .wav, .flac, .pcm, or .mp3\n"
				"are assumed to be DA files.\n"
				"----------------------------------------------------\n\n\n"
				"IMPORTANT: XML entry order determines the final LBA.\n"
				"----------------------------------------------------\n"
				"Place the BOOT executable (from SYSTEM.CNF)\n"
				"immediately after the SYSTEM.CNF entry.\n" );
		if (!DAfiles.empty())
		{
			printf("\nPlace DA files at the end of directory_tree,\n"
				   "immediately after the last DUMMY entry.\n");
		}
		printf( "----------------------------------------------------\n" );
		printf( "Press Enter to continue..." );
		getchar();
	}
}

void ParseISO(cd::IsoReader& reader) {

    cd::ISO_DESCRIPTOR descriptor;
	auto license = ReadLicense(reader);
	global::xa_edc = CheckEDCXA();
	global::cdvd_style = CheckISOver();

    reader.SeekToSector(16);
    reader.ReadBytes(&descriptor, F1_DATA_SIZE);


	if (!param::QuietMode)
	{
		printf( "Scanning tracks...\n\n"
				"  Track #1 data:\n"
				"    Identifiers:\n" );
		PrintId("      System ID         : ", descriptor.systemID);
		PrintId("      Volume ID         : ", descriptor.volumeID);
		PrintId("      Volume Set ID     : ", descriptor.volumeSetIdentifier);
		PrintId("      Publisher ID      : ", descriptor.publisherIdentifier);
		PrintId("      Data Preparer ID  : ", descriptor.dataPreparerIdentifier);
		PrintId("      Application ID    : ", descriptor.applicationIdentifier);
		PrintId("      Copyright ID      : ", descriptor.copyrightFileIdentifier);

		PrintDate("      Creation Date     : ", descriptor.volumeCreateDate);
		PrintDate("      Modification Date : ", descriptor.volumeModifyDate);
		PrintDate("      Expiration Date   : ", descriptor.volumeExpiryDate);
	}

    cd::IsoPathTable pathTable;

	size_t numEntries = pathTable.ReadPathTable(&reader, descriptor.pathTable1Offs, descriptor.pathTableSize.lsb);

    if (numEntries == 0) {
		printf("\nNo files to find.\n");
        return;
    }

	if (pathTable.pathTableList[0].entry.dirOffs != descriptor.rootDirRecord.entryOffs.lsb)
	{
		printf("\nERROR: Root directory offset in path table does not match the one in volume descriptor.\n"
				 "       The ISO image may be corrupt or invalid.\n");
		exit(EXIT_FAILURE);
	}

	// Prepare output directories
	for(size_t i=0; i<numEntries; i++)
	{
		const fs::path dirPath = param::outPath / pathTable.GetFullDirPath(i);

		std::error_code ec;
		fs::create_directories(dirPath, ec);
		if (ec)
		{
			printf("\nERROR: Cannot create directory \"%" PRFILESYSTEM_PATH "\". %s\n", dirPath.parent_path().c_str(), ec.message().c_str());
			exit(EXIT_FAILURE);
		}
	}

	if (!param::QuietMode)
	{
		if (!param::noxml)
		{
			printf("\n    License file: \"%" PRFILESYSTEM_PATH "\"\n", (param::outPath / "license_data.dat").c_str());
		}
		printf("\n    Parsing directory tree...\n");
	}

	std::list<cd::IsoDirEntries::Entry> entries;
	std::unique_ptr<cd::IsoDirEntries> rootDir = ParseRoot(reader, ListView(entries), pathTable.pathTableList);

	// Sort files by LBA for "strict" output
	entries.sort([](const auto& left, const auto& right)
		{
			return left.entry.entryOffs.lsb < right.entry.entryOffs.lsb;
		});

	// Process DA tracks and add them to the entries list
	auto DAfiles = ParseDAfiles(reader, entries);

	const unsigned totalLenLBA = descriptor.volumeSize.lsb;
	if (param::force)
	{
		BruteForce(reader, entries, descriptor.rootDirRecord.entryOffs.lsb, totalLenLBA);
	}

	// SYSTEM DESCRIPTION CD-ROM XA Ch.II 2.3, postgap should be always >= 150 sectors for CD-DA discs and optionally for non CD-DA.
	unsigned endFS, postGap = 150;
	for (auto it = entries.rbegin(); it != entries.rend(); it++)
	{
		if (it->type != EntryType::EntryDA)
	 	{
	 		endFS = it->entry.entryOffs.lsb + GetSizeInSectors(it->entry.entrySize.lsb);
			if (!global::cueFile.tracks.empty() && global::cueFile.tracks[0].endSector <= totalLenLBA)
			{
				endFS += postGap = global::cueFile.tracks[0].endSector - endFS;
			}
			else if (!DAfiles.empty())
			{
				endFS += postGap = (DAfiles.front()->entry.entryOffs.lsb - endFS) / 2;
			}
			else if (totalLenLBA - endFS < postGap)
			{
				endFS += postGap = totalLenLBA - endFS;
				if (postGap && !param::noWarns)
				{
					printf("    WARNING: Size of DATA track postgap is of %u sectors instead of 150.\n", postGap);
				}
			}
			break;
		}
	}

	if (!param::QuietMode)
	{
		printf("      Files Total: %zu\n", entries.size() - numEntries);
		printf("      Directories: %zu\n", numEntries - 1);
		printf("      Total file system size: %u bytes (%u sectors)\n", endFS * CD_SECTOR_SIZE, endFS);

		int tracknum = 2;
		for(const auto& entry : DAfiles)
		{
			printf("\n  Track #%d audio:\n", tracknum);
			printf("    DA File \"%s\"\n", entry->identifier.c_str());
			tracknum++;
		}
		printf( "\nExtracting ISO...\n"
				"  Creating files...\n" );
	}

	ExtractFiles(reader, entries, param::outPath);

	if (!param::noxml)
	{
		SaveLicense(*license);
		if (!param::QuietMode)
		{
			printf("  Creating XML document...");
		}

		const unsigned currentLBA = xml::WriteXML(descriptor, rootDir, DAfiles, postGap);

		if (!param::QuietMode)
		{
			printf(" Ok.\n\n");
		}

		// Check if there is still an EoF gap
		if (!param::noWarns && (postGap > 150 || currentLBA < totalLenLBA))
		{
			printf( "WARNING: There is still a gap of %u sectors at the end of file system.\n"
					"\t This could mean that there are missing files or tracks.\n", postGap > 150 ? postGap : totalLenLBA - currentLBA);
			if (global::cueFile.tracks.empty())
			{
				printf("\t Try using a .cue file instead of an ISO image.\n");
			}
			else
			{
				printf("\t Try using the -pt or/and -f command, helps with obfuscated file systems.\n");
			}
		}
	}

	if (!param::QuietMode)
	{
		printf("ISO image dumped successfully.\n");
	}
}

int Main(int argc, char *argv[])
{
	static constexpr const char* HELP_TEXT =
		"Usage: dumpsxiso [options] <input>\n\n"
		"  <input>\t\tAny 2352-sector disc image/cue to extract, or a directory to generate an XML project.\n\n"
		"Options:\n"
		"  -h|--help\t\tShows this help text\n"
		"  -q|--quiet\t\tQuiet mode (suppress all but warnings and errors)\n"
		"  -w|--warns\t\tSuppress all warnings (can be used along with -q)\n"
		"  -x <path>\t\tOptional destination directory for extracted files (defaults to working dir)\n"
		"  -s <file>\t\tOptional XML name/destination for MKPSXISO script (defaults to working dir)\n"
		"  -pt|--path-table\tGo through every known directory in order; helps on soft obfuscated games (like DMW3)\n"
		"  -f|--force\t\tScans all unknown sectors for files; helps on heavy obfuscated games (like Xenogears)\n"
		"  -e|--encode <codec>\tCodec to encode CDDA/DA audio; supports " SUPPORTED_CODEC_TEXT " (defaults to wave)\n"
		"  -l|--lba\t\tWrites all source paths and LBA offsets in the XML to force them at build time\n"
		"  -n|--noxml\t\tDo not generate an XML file and license file\n"
		"  -r|--raw\t\tDumps all files in raw format (forces --noxml option)\n"
		"  -S|--sort-by-dir\tOutputs a \"pretty\" XML script where entries are grouped in directories\n"
		"\t\t\t(instead of strictly following their original order on the disc)\n";

	static constexpr const char* VERSION_TEXT =
		"DUMPSXISO " VERSION " - PlayStation ISO dumping tool\n"
		"Get the latest version at https://github.com/Lameguy64/mkpsxiso\n"
		"Original work: Meido-Tek Productions (John \"Lameguy\" Wilbert Villamor/Lameguy64)\n"
		"Maintained by: Silent (CookiePLMonster) and spicyjpeg\n"
		"Contributions: marco-calautti, G4Vi, Nagtan and all the ones from github\n\n";

	if (argc == 1)
	{
		printf(VERSION_TEXT);
		printf(HELP_TEXT);
		return EXIT_SUCCESS;
	}

	for (char** args = argv+1; *args != nullptr; args++)
	{
		// Is it a switch?
		if ((*args)[0] == '-')
		{
			if (ParseArgument(args, "h", "help"))
			{
				printf(VERSION_TEXT);
				printf(HELP_TEXT);
				return EXIT_SUCCESS;
			}
			if (ParseArgument(args, "f", "force"))
			{
				param::force = true;
				continue;
			}
			if (ParseArgument(args, "l", "lba"))
			{
				param::lba = true;
				continue;
			}
			if (ParseArgument(args, "n", "noxml"))
			{
				param::noxml = true;
				continue;
			}
			if (ParseArgument(args, "pt", "path-table"))
			{
				param::pathTable = true;
				continue;
			}
			if (ParseArgument(args, "q", "quiet"))
			{
				param::QuietMode = true;
				continue;
			}
			if (ParseArgument(args, "r", "raw"))
			{
				param::raw = true;
				param::noxml = true;
				param::encodingFormat = EAF_PCM;
				continue;
			}
			if (ParseArgument(args, "w", "warns"))
			{
				param::noWarns = true;
				continue;
			}
			if (ParseArgument(args, "S", "sort-by-dir"))
			{
				param::outputSortedByDir = true;
				continue;
			}
			if (auto outPath = ParsePathArgument(args, "x"); outPath.has_value())
			{
				param::outPath = outPath->lexically_normal();
				continue;
			}
			if (auto xmlPath = ParsePathArgument(args, "s"); xmlPath.has_value())
			{
				param::xmlFile = xmlPath->lexically_normal();
				continue;
			}
			if(auto encodingStr = ParseStringArgument(args, "e", "encode"); encodingStr.has_value())
			{
				unsigned i;
				for(i = 0; i < std::size(EncodingCodecs); i++)
				{
					if(CompareICase(*encodingStr, EncodingCodecs[i].codec))
					{
						break;
					}
				}
				if(i == std::size(EncodingCodecs))
				{
					printf("Unknown codec: %s\n", (*encodingStr).c_str());
					printf("Supported codecs: %s\n", SUPPORTED_CODEC_TEXT);
				    return EXIT_FAILURE;
				}
				if(EncodingCodecs[i].eaf & SUPPORTED_CODECS)
				{
					param::encodingFormat = EncodingCodecs[i].eaf;
					continue;
				}
				printf("%s", EncodingCodecs[i].notcompiledmessage);
				return EXIT_FAILURE;
			}

			// If we reach this point, an unknown parameter was passed
			printf("Unknown parameter: %s\n", *args);
			return EXIT_FAILURE;
		}

		if (param::isoFile.empty())
		{
			param::isoFile = fs::path(reinterpret_cast<const char8_t*>(*args)).lexically_normal().lexically_proximate(fs::current_path());
		}
		else
		{
			printf("Only one ISO file is supported.\n");
			return EXIT_FAILURE;
		}
	}

	if (param::isoFile.empty())
	{
		printf("No iso file specified.\n");
		return EXIT_FAILURE;
	}

	// PowerShell tab-completion adds a trailing slash to directories, breaking fs::path::stem. Idk who thought this was a good idea...
	if (!param::isoFile.has_filename() && param::isoFile.has_parent_path())
	{
		param::isoFile = param::isoFile.parent_path();
	}

	if (!param::QuietMode)
	{
		printf(VERSION_TEXT);
	}

	if (param::outPath.empty())
	{
		param::outPath = param::isoFile.stem();
	}

	if (param::xmlFile.empty() || fs::is_directory(param::xmlFile))
	{
		(param::xmlFile /= param::outPath.filename()) += ".xml";
	}

	cd::IsoReader& reader = *(cd::reader = std::make_unique<cd::IsoReader>());

	if (fs::is_directory(param::isoFile))
	{
		param::dir = true;
		param::outPath = param::isoFile;
		ParseDIR();
		return EXIT_SUCCESS;
	}

	if (CompareICase(param::isoFile.extension().string(), ".cue"))
	{
		global::cueFile = cue::parseCueFile(param::isoFile);
	}

	if (!reader.Open(param::isoFile)) {

		printf("ERROR: Cannot open file \"%" PRFILESYSTEM_PATH "\"\n", param::isoFile.c_str());
		return EXIT_FAILURE;

	}
	
	// Check if file has a valid ISO9660 header
	{
		cd::ISO_DESCRIPTOR descriptor;
		if (!reader.SeekToSector(16) || !reader.ReadBytes(&descriptor, F1_DATA_SIZE, true) || memcmp(&descriptor.header, "\1CD001\1", 7))
		{
			printf("ERROR: File does not contain a valid ISO9660 file system.\n");
			return EXIT_FAILURE;
		}
	}

	if (!param::QuietMode)
	{
		printf("Output directory : \"%" PRFILESYSTEM_PATH "\"\n\n", param::outPath.c_str());
	}

	tzset(); // Initializes the time-related environment variables
    ParseISO(reader);
	return EXIT_SUCCESS;
}
