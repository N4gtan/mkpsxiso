#include "iso.h"		// ISO file system generator module
#include "xml.h"

#define MA_NO_THREADING
#define MA_NO_DEVICE_IO
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio_helpers.h"

namespace global
{
	time_t	BuildTime;
	bool	xa_edc		= true;
	bool	ps2			= false;
	bool	noWarns		= false;
	bool	QuietMode	= false;
	bool	Overwrite	= false;
	bool	NoIsoGen 	= false;
	bool	noXA		= false;
	int		trackNum	= 1;

	std::optional<bool> cdvd_style;
	std::optional<std::string> volid_override;
	std::optional<fs::path> cuefile;
	fs::path XMLscript;
	fs::path LBAfile;
	fs::path LBAheaderFile;
	fs::path ImageName;
	fs::path LicenseFile;
	fs::path RebuildXMLScript;

	tinyxml2::XMLDocument xmlIdFile;
};


bool ParseDirectory(iso::DirTreeClass* dirTree, const tinyxml2::XMLElement* parentElement, const fs::path& xmlPath, const EntryAttributes& defaultAttributes, const fs::path& currentPath);
iso::DirTreeClass* ParseISOfileSystem(const tinyxml2::XMLElement* trackElement, const fs::path& xmlPath, iso::EntryList& entries, iso::IDENTIFIERS& isoIdentifiers, int& totalLen);

bool PackFileAsCDDA(void* buffer, const fs::path& audioFile);

iso::DIRENTRY* UpdateDAFilesWithLBA(iso::EntryList& entries, const char *trackid, const unsigned lba)
{
	for(auto it = entries.rbegin(); it != entries.rend(); ++it)
	{
		auto& entry = *it;
		if(entry.trackid != trackid) continue;
		if(entry.lba == 0)
		{
			entry.lba = lba;
		}
		if ( !global::QuietMode )
		{
			printf("    DA File \"%s\"\n", entry.id.c_str());
		}
		return &entry;
	}

	printf( "ERROR: Did not find entry with trackid %s\n",  trackid);
	return nullptr;
}

int Main(int argc, char* argv[])
{
	static constexpr const char* HELP_TEXT =
		"Usage: mkpsxiso [options] <input>\n\n"
		"  <input>\t\tAny XML project file defining the disc layout to build.\n\n"
		"Options:\n"
		"  -h|--help\t\tShows this help text\n"
		"  -q|--quiet\t\tQuiet mode (suppress all but warnings and errors)\n"
		"  -w|--warns\t\tSuppress all warnings (can be used along with -q)\n"
		"  -l|--label\t\tSpecify volume ID (overrides volume element)\n"
		"  -o|--output <file>\tSpecify output file (overrides image_name attribute)\n"
		"  -c|--cuefile <file>\tSpecify cue sheet file (overrides cue_sheet attribute)\n"
		"  -L|--license <file>\tSpecify license file (overrides file attribute)\n"
		"  -y\t\t\tAlways overwrite ISO image files\n"
		"  -lba <file>\t\tGenerate a log of file LBA locations in disc image\n"
		"  -lbahead <file>\tGenerate a C header of file LBA locations in disc image\n"
		"  -rebuildxml\t\tRebuild the XML using our newest schema\n"
		"  -noisogen\t\tDo not generate ISO, but calculate file LBA locations (for use with -lba or -lbahead)\n";

	static constexpr const char* VERSION_TEXT =
		"MKPSXISO " VERSION " - PlayStation ISO Image Maker\n"
		"Get the latest version at https://github.com/Lameguy64/mkpsxiso\n"
		"Original work: Meido-Tek Productions (John \"Lameguy\" Wilbert Villamor/Lameguy64)\n"
		"Maintained by: Silent (CookiePLMonster) and spicyjpeg\n"
		"Contributions: marco-calautti, G4Vi, Nagtan and all the ones from github\n\n";

	if ( argc == 1 )
	{
		printf(VERSION_TEXT);
		printf(HELP_TEXT);
		return EXIT_SUCCESS;
	}

	bool OutputOverride = false;
	// Parse arguments
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
			if (ParseArgument(args, "noisogen"))
			{
				global::NoIsoGen = true;
				continue;
			}
			if (ParseArgument(args, "q", "quiet"))
			{
				global::QuietMode = true;
				continue;
			}
			if (ParseArgument(args, "w", "warns"))
			{
				global::noWarns = true;
				continue;
			}
			if (ParseArgument(args, "y"))
			{
				global::Overwrite = true;
				continue;
			}
			if (auto lbaHead = ParsePathArgument(args, "lbahead"); lbaHead.has_value())
			{
				if (CompareICase(lbaHead->extension().string(), ".xml"))
				{
					args--;
					global::LBAheaderFile = lbaHead->stem() += "_LBA.h";
					continue;
				}
				global::LBAheaderFile = lbaHead->lexically_normal();
				continue;
			}
			if (auto lbaFile = ParsePathArgument(args, "lba"); lbaFile.has_value())
			{
				if (CompareICase(lbaFile->extension().string(), ".xml"))
				{
					args--;
					global::LBAfile = lbaFile->stem() += "_LBA.txt";
					continue;
				}
				global::LBAfile	= lbaFile->lexically_normal();
				continue;
			}
			if (auto output = ParsePathArgument(args, "o", "output"); output.has_value())
			{
				global::ImageName = output->lexically_normal();
				OutputOverride = true;
				continue;
			}
			if (auto output = ParsePathArgument(args, "c", "cuefile"); output.has_value())
			{
				global::cuefile = output->lexically_normal();
				OutputOverride = true;
				continue;
			}
			if (auto output = ParsePathArgument(args, "L", "license"); output.has_value())
			{
				global::LicenseFile = output->lexically_normal();
				continue;
			}
			if (auto newxmlfile = ParsePathArgument(args, "rebuildxml"); newxmlfile.has_value())
			{
				global::RebuildXMLScript = newxmlfile->lexically_normal();
				continue;
			}
			if (auto label = ParseStringArgument(args, "l", "label"); label.has_value())
			{
				global::volid_override = label;
				continue;
			}

			// If we reach this point, an unknown parameter was passed
			printf("Unknown parameter: %s\n", *args);
			return EXIT_FAILURE;
		}

		if ( global::XMLscript.empty() )
		{
			global::XMLscript = ARGV_PATH(*args).lexically_normal().lexically_proximate(fs::current_path());
		}
		else
		{
			printf("Only one XML script is supported. (use an XML with multiple <iso_project> elements instead)\n");
			return EXIT_FAILURE;
		}

	}

	if ( global::XMLscript.empty() )
	{
		printf( "No XML script specified.\n" );
		return EXIT_FAILURE;
	}

	if ( !global::QuietMode )
	{
		printf(VERSION_TEXT);
	}

	if ( global::LBAfile == "-lba" )
	{
		global::LBAfile = global::XMLscript.stem() += "_LBA.txt";
	}

	if ( global::LBAheaderFile == "-lbahead" )
	{
		global::LBAheaderFile = global::XMLscript.stem() += "_LBA.h";
	}

	tzset(); // Initializes the time-related environment variables
	// Get current time to be used as date stamps for all directories
	time( &global::BuildTime );

	// Load XML file
	tinyxml2::XMLDocument xmlFile;
	{
		tinyxml2::XMLError error;
		if (FILE* file = OpenFile(global::XMLscript, "rb"); file != nullptr)
		{
			error = xmlFile.LoadFile(file);
			fclose(file);
		}
		else
		{
			error = tinyxml2::XML_ERROR_FILE_NOT_FOUND;
		}

		if ( error != tinyxml2::XML_SUCCESS )
		{
			printf("ERROR: ");
			if ( error == tinyxml2::XML_ERROR_FILE_NOT_FOUND )
			{
				printf("File not found.\n");
			}
			else if ( error == tinyxml2::XML_ERROR_FILE_COULD_NOT_BE_OPENED )
			{
				printf("File cannot be opened.\n");
			}
			else if ( error == tinyxml2::XML_ERROR_FILE_READ_ERROR )
			{
				printf("Error reading file.\n");
			}
			else
			{
				printf("%s on line %d\n", xmlFile.ErrorName(), xmlFile.ErrorLineNum());
			}
			return EXIT_FAILURE;
		}
    }

	// Check if there is an <iso_project> element
	const tinyxml2::XMLElement* projectElement = xmlFile.FirstChildElement(xml::elem::ISO_PROJECT);
	if ( projectElement == nullptr )
	{
		printf( "ERROR: Cannot find <iso_project> element in XML document.\n" );
		return EXIT_FAILURE;
	}

	// Fix old XML trees to our current spec
	// convert DA file source syntax to DA file trackid syntax
	for(tinyxml2::XMLElement *modifyProject = xmlFile.FirstChildElement(xml::elem::ISO_PROJECT);
		modifyProject != nullptr;
		modifyProject = modifyProject->NextSiblingElement(xml::elem::ISO_PROJECT))
	{
		unsigned audioIndex = 0;
		tinyxml2::XMLElement *modifyTrack = modifyProject->FirstChildElement(xml::elem::TRACK);
		if((modifyTrack == nullptr) || (!modifyTrack->Attribute(xml::attrib::TRACK_TYPE, "data")))
		{
			continue;
		}
		if (const char *new_type = modifyTrack->Attribute("new_type"); new_type != nullptr)
		{
			if (modifyTrack->Attribute(xml::attrib::CDVD_STYLE) == nullptr)
			{
				modifyTrack->SetAttribute(xml::attrib::CDVD_STYLE, new_type);
			}
			modifyTrack->DeleteAttribute("new_type"); // new_type is deprecated
		}
		tinyxml2::XMLElement *dt =  modifyTrack->FirstChildElement(xml::elem::DIRECTORY_TREE);
		if(dt == nullptr)
		{
			continue;
		}
		std::vector<std::tuple<tinyxml2::XMLElement *, std::string_view, std::string_view>> audioTracks;
		std::queue<tinyxml2::XMLElement *> toscan({dt});
		while(!toscan.empty())
		{
			tinyxml2::XMLElement *scanElm = toscan.front();
			toscan.pop();
			if (const char *srcdir = scanElm->Attribute("srcdir"); srcdir != nullptr)
			{
				if (scanElm->Attribute(xml::attrib::ENTRY_SOURCE) == nullptr)
				{
					scanElm->SetAttribute(xml::attrib::ENTRY_SOURCE, srcdir);
				}
				scanElm->DeleteAttribute("srcdir"); // srcdir is deprecated
			}
			if(CompareICase(scanElm->Name(), xml::elem::FILE))
			{
				if(scanElm->Attribute(xml::attrib::ENTRY_TYPE, "da"))
				{
					const char *source = scanElm->Attribute(xml::attrib::ENTRY_SOURCE);
					if(source != nullptr && *source != 0)
					{
						auto updateAudioIndexFromTID = [&](const char *tid) -> const char*
						{
							if (int id = atoi(tid); audioIndex <= id)
							{
								audioIndex = id + 1;
							}
							return tid;
						};

						// Index existing tracks only once
						if (audioIndex == 0)
						{
							for (tinyxml2::XMLElement *t = modifyTrack->NextSiblingElement(xml::elem::TRACK); t != nullptr; t = t->NextSiblingElement(xml::elem::TRACK))
							{
								const char* tid = t->Attribute(xml::attrib::TRACK_ID);
								const char* src = t->Attribute(xml::attrib::TRACK_SOURCE);
								audioTracks.emplace_back(t, tid ? updateAudioIndexFromTID(tid) : "", src ? src : "");
							}
						}

						// Reuse or create ID for this DA file
						const std::string trackid = [&]() -> std::string
						{
							if (const char* tid = scanElm->Attribute(xml::attrib::TRACK_ID); tid != nullptr && *tid != 0)
							{
								return updateAudioIndexFromTID(tid);
							}
							return Format("%02u", audioIndex++);
						}();

						auto matchByID = audioTracks.end();
						std::string_view sourceSV = source;
						for (auto it = audioTracks.begin(); it != audioTracks.end(); ++it)
						{
							auto& [t, tid, src] = *it;
							// reuse track with same source if possible
							if (src == sourceSV)
							{
								modifyTrack = t;
								modifyTrack->SetAttribute(xml::attrib::TRACK_ID, trackid.c_str());
								audioTracks.erase(it);
								goto update_file;
							}
							if (tid == trackid)
							{
								matchByID = it;
							}
						}

						// reuse track with same ID if source not matched
						if (matchByID != audioTracks.end())
						{
							modifyTrack = std::get<0>(*matchByID);
							modifyTrack->SetAttribute(xml::attrib::TRACK_SOURCE, source);
							audioTracks.erase(matchByID);
							goto update_file;
						}{

                        // add a new track
						tinyxml2::XMLElement *newtrack = xmlFile.NewElement(xml::elem::TRACK);
						newtrack->SetAttribute(xml::attrib::TRACK_TYPE, "audio");
						newtrack->SetAttribute(xml::attrib::TRACK_ID, trackid.c_str());
						newtrack->SetAttribute(xml::attrib::TRACK_SOURCE, source);
						// a 2 second pregap is assumed, don't write it
						/*tinyxml2::XMLElement *pregap = newtrack->InsertNewChildElement(xml::elem::TRACK_PREGAP);
						pregap->SetAttribute(xml::attrib::PREGAP_DURATION, "00:02:00");*/
						modifyProject->InsertAfterChild(modifyTrack, newtrack);
						modifyTrack = newtrack;

						}update_file: // update the file to point to the track
						scanElm->DeleteAttribute(xml::attrib::ENTRY_SOURCE);
						scanElm->SetAttribute(xml::attrib::TRACK_ID, trackid.c_str());
					}
				}
				continue;
			}

			// add children to scan
			scanElm = scanElm->FirstChildElement();
			while(scanElm != nullptr)
			{
				toscan.push(scanElm);
				scanElm = scanElm->NextSiblingElement();
			}
		}
	}

	if(!global::RebuildXMLScript.empty())
	{
		if ( !global::QuietMode )
		{
			printf( "      Writing new XML ... " );
		}
		if (FILE* file = OpenFile(global::RebuildXMLScript, "w"); file != nullptr)
	    {
	    	xmlFile.SaveFile(file);
	    	fclose(file);
	    }
		else
		{
			printf( "ERROR: Cannot open %" PRFILESYSTEM_PATH " for writing\n", 
				global::RebuildXMLScript.c_str());
			return EXIT_FAILURE;
		}
		if ( !global::QuietMode )
		{
			printf("Ok.\n");
		}
	    return EXIT_SUCCESS;
	}

    int imagesCount = 0;
	// Build loop for XML scripts with multiple <iso_project> elements
	while ( projectElement != nullptr )
	{
		imagesCount++;
		if ( imagesCount > 1 && OutputOverride )
		{
			printf( "ERROR: -o or -c switches cannot be used in a multi-disc ISO "
				"project.\n" );
			return EXIT_FAILURE;
		}

		// Check if image_name attribute is specified
		if ( global::ImageName.empty() )
		{
			if ( const char* image_name = projectElement->Attribute(xml::attrib::IMAGE_NAME); image_name != nullptr && *image_name != 0 )
			{
				global::ImageName = reinterpret_cast<const char8_t*>(image_name);
			}
			else
			{
				// Use file name of XML project as the image file name
				global::ImageName = global::XMLscript.stem();
				global::ImageName += ".bin";
			}
		}

		if ( !global::cuefile )
		{
			if ( const char* cue_sheet = projectElement->Attribute(xml::attrib::CUE_SHEET); cue_sheet != nullptr )
			{
				global::cuefile = reinterpret_cast<const char8_t*>(cue_sheet);
			}
		}

		// Check if cue_sheet attribute is specified
		if ( global::cuefile && global::cuefile->empty() )
		{
			printf( "ERROR: %s attribute is blank.\n", xml::attrib::CUE_SHEET );
			return EXIT_FAILURE;
		}

		if ( !global::QuietMode )
		{
			printf( "Building ISO Image: \"%" PRFILESYSTEM_PATH "\"", global::ImageName.c_str() );

			if ( global::cuefile )
			{
				printf( " + \"%" PRFILESYSTEM_PATH "\"", global::cuefile->c_str() );
			}

			printf( "\n" );
		}

		global::noXA = projectElement->BoolAttribute( xml::attrib::NO_XA );

		if ( !global::Overwrite && !global::NoIsoGen && !global::noWarns)
		{
			if ( GetSize( global::ImageName ) >= 0 )
			{
				printf( "WARNING: ISO image already exists, overwrite? <y/n> " );
				char key;

				do {

					key = getchar();

					if ( std::tolower( key ) == 'n' )
					{
						return EXIT_FAILURE;
					}
				} while( tolower( key ) != 'y' );
			}
		}
		if ( !global::QuietMode )
		{
			printf( "\n" );
		}

		// Check if there is a track element specified
		if ( projectElement->FirstChildElement(xml::elem::TRACK) == nullptr )
		{
			printf( "ERROR: At least one <track> element must be specified.\n" );
			return EXIT_FAILURE;
		}

		global::trackNum = 1;
		iso::EntryList entries;
		iso::IDENTIFIERS isoIdentifiers {};
		iso::DirTreeClass* dirTree = nullptr;
		int totalLenLBA = 0;

		bool hasDataTrack = false;
		std::vector<iso::DIRENTRY*> audioTracks;
		std::string cueSheet = Format<4096>( "FILE \"%s\" BINARY\n", global::ImageName.filename().u8string().c_str() );

		// Parse tracks
		if ( !global::QuietMode )
		{
			printf("Scanning tracks...\n\n");
		}
		for ( const tinyxml2::XMLElement* trackElement = projectElement->FirstChildElement(xml::elem::TRACK);
			trackElement != nullptr; trackElement = trackElement->NextSiblingElement(xml::elem::TRACK) )
		{
			const char* track_type = trackElement->Attribute(xml::attrib::TRACK_TYPE);

			if ( track_type == nullptr )
			{
				if ( !global::QuietMode )
				{
					printf( "  " );
				}

				printf( "ERROR: %s attribute not specified in <track> "
					"element on line %d.\n", xml::attrib::TRACK_TYPE, trackElement->GetLineNum() );

				return EXIT_FAILURE;
			}

			if ( !global::QuietMode )
			{
				printf( "  Track #%d %s:\n", global::trackNum,
					track_type );
			}

			// Generate ISO file system for data track
			if ( CompareICase( "data", track_type ) )
			{
				global::xa_edc = trackElement->BoolAttribute(xml::attrib::XA_EDC, true);

				// This check is necessary so as to leave an empty value for compatibility with <=v2.04 dumped files timestamps
				if ( trackElement->Attribute(xml::attrib::CDVD_STYLE) != nullptr )
				{
					global::cdvd_style = trackElement->BoolAttribute(xml::attrib::CDVD_STYLE);
				}
				if ( (global::ps2 = trackElement->BoolAttribute(xml::attrib::PS2)) )
				{
					global::cdvd_style = true; // Force true if it's an PS2 disc
				}

				if ( global::trackNum != 1 )
				{
					if ( !global::QuietMode )
					{
						printf( "  " );
					}

					printf( "ERROR: Only the first track can be set as a "
						"data track on line: %d\n", trackElement->GetLineNum() );

					return EXIT_FAILURE;
				}
				hasDataTrack = true;

				dirTree = ParseISOfileSystem( trackElement, global::XMLscript.parent_path(), entries, isoIdentifiers, totalLenLBA );
				if ( dirTree == nullptr )
				{
					return EXIT_FAILURE;
				}

				if ( totalLenLBA < (2 * 75) && !global::noWarns )
				{
					printf( "WARNING: System duration < 2 seconds. It's recommended to set <dummy sectors=\"150\"/>\n"
							"         at the end of <%s>; otherwise, the burner might reject the image!\n\n", xml::elem::DIRECTORY_TREE );
				}
				else if ( totalLenLBA > (71 * 60 * 75) && !global::noWarns )
				{
					printf( "WARNING: System duration > 71 minutes\n\n" );
				}

				FormatTo( cueSheet,	"  TRACK %02d MODE2/2352\n"
									"    INDEX 01 00:00:00\n", global::trackNum);

			// Add audio track
			}
			else if ( CompareICase( "audio", track_type ) )
			{

				// Only allow audio tracks if the cue_sheet attribute is specified
				if ( !global::cuefile && !global::NoIsoGen )
				{
					if ( !global::QuietMode )
					{
						printf( "    " );
					}

					printf( "ERROR: %s attribute or -c parameter must be specified "
						"when using audio tracks.\n", xml::attrib::CUE_SHEET );

					return EXIT_FAILURE;
				}

				// Write track information to the CUE sheet
				const char* trackRelativeSource = trackElement->Attribute(xml::attrib::TRACK_SOURCE);
				if ( trackRelativeSource == nullptr || *trackRelativeSource == 0 )
				{
					if ( !global::QuietMode )
					{
						printf("    ");
					}

					printf( "ERROR: %s attribute not specified "
						"for track on line %d.\n", xml::attrib::TRACK_SOURCE, trackElement->GetLineNum() );

					return EXIT_FAILURE;
				}

				fs::path trackSource = (global::XMLscript.parent_path() / reinterpret_cast<const char8_t*>(trackRelativeSource)).lexically_normal();
				FormatTo( cueSheet, "  TRACK %02d AUDIO\n", global::trackNum );

				// pregap
				int pregapSectors = global::trackNum > 1 ? 150 : 0; // SYSTEM DESCRIPTION CD-ROM XA Ch.II 2.3, pause should be always >= 150 sectors.
				const tinyxml2::XMLElement *pregapElement = trackElement->FirstChildElement(xml::elem::TRACK_PREGAP);
				if(pregapElement != nullptr)
				{
					const char *duration = pregapElement->Attribute(xml::attrib::PREGAP_DURATION);
					if(duration != nullptr)
					{
						pregapSectors = TimecodeToSectors(duration);
						if(pregapSectors < 0)
						{
							printf( "ERROR: %s duration has invalid MM:SS:FF "
								"for track on line %d.\n", xml::elem::TRACK_PREGAP, pregapElement->GetLineNum() );
							return EXIT_FAILURE;
						}

						if (pregapSectors > (1 * 5 * 75) && !global::noWarns)
						{
							if ( !global::QuietMode )
							{
								printf( "    " );
							}
							printf( "WARNING: Pregap is unusually large (%s > 5 seconds) on line %d.\n",
								xml::attrib::PREGAP_DURATION, pregapElement->GetLineNum() );
						}
					}
				}

				if(pregapSectors > 0)
				{
					FormatTo( cueSheet, "    INDEX 00 %s\n", SectorsToTimecode(totalLenLBA).c_str() );
					totalLenLBA += pregapSectors;
				}

				const char *trackid = trackElement->Attribute(xml::attrib::TRACK_ID);
				if (trackid == nullptr)
				{
					trackid = "";
					if (dirTree == nullptr)
					{
						dirTree = (entries.emplace_back().subdir = std::make_unique<iso::DirTreeClass>(entries)).get();
					}
					if (!dirTree->AddFileEntry(std::string(U8ToSv(trackSource.filename().u8string())), EntryType::EntryDA, trackSource, EntryAttributes{.HFLAG = 2}, trackid, nullptr))
					{
						return EXIT_FAILURE;
					}
				}

				auto *entry = UpdateDAFilesWithLBA(entries, trackid, totalLenLBA);
				if (entry == nullptr)
				{
					return EXIT_FAILURE;
				}
				audioTracks.push_back(entry);
				FormatTo( cueSheet, "    INDEX 01 %s\n", SectorsToTimecode(entry->lba).c_str() );

				const int entryLenLBA = GetSizeInSectors(entry->length, CD_SECTOR_SIZE);
				if (entryLenLBA < (2 * 75) && !global::noWarns)
				{
					if ( !global::QuietMode )
					{
						printf( "    " );
					}
					printf( "WARNING: Audio duration < 2 seconds. The burner might reject the image!\n" );
				}

				totalLenLBA = std::max<int>(totalLenLBA, entry->lba + entryLenLBA); // std::max prevents rewind on forced LBAs

				if ( !global::QuietMode )
				{
					printf( "\n" );
				}

			// If an unknown track type is specified
			}
			else
			{
				if ( !global::QuietMode )
				{
					printf( "    " );
				}

				printf( "ERROR: Unknown track type on line %d.\n",
					trackElement->GetLineNum() );

				return EXIT_FAILURE;
			}

			global::trackNum++;
		}

		if ( totalLenLBA > (80 * 60 * 75) && !global::noWarns )
		{
			printf( "WARNING: Total duration > 80 minutes (%d sectors)\n", totalLenLBA );
			if ( !global::QuietMode )
			{
				printf( "\n" );
			}
		}

		if ( !global::LBAfile.empty() )
		{
			FILE* fp = OpenFile( global::LBAfile, "wb" );
			if (fp != nullptr)
			{
				dirTree->SortDirectoryEntries(true);

				fprintf( fp, "File LBA log generated by MKPSXISO v" VERSION "\n\n" );
				fprintf( fp, "Image bin file: \"%s\"\n", reinterpret_cast<const char*>(global::ImageName.generic_u8string().c_str()) );

				if ( global::cuefile )
				{
					fprintf( fp, "Image cue file: \"%s\"\n", reinterpret_cast<const char*>(global::cuefile->generic_u8string().c_str()) );
				}

				fprintf( fp, "\nFile System:\n\n" );
				fprintf( fp, "     Type |     Name     | Length |  LBA  "
					"| Timecode |   Bytes   |    Source File\n\n" );

				dirTree->OutputLBAlisting( fp, 0 );

				dirTree->SortDirectoryEntries();

				fclose( fp );

				if ( !global::QuietMode )
				{
					printf( "Wrote file LBA log \"%" PRFILESYSTEM_PATH "\"\n\n",
						global::LBAfile.c_str() );
				}
			}
			else
			{
				if ( !global::QuietMode )
				{
					printf( "Failed to write LBA log \"%" PRFILESYSTEM_PATH "\"!\n\n",
						global::LBAfile.c_str() );
				}
			}
		}

		if ( !global::LBAheaderFile.empty() )
		{
			FILE* fp = OpenFile( global::LBAheaderFile, "wb" );
			if (fp != nullptr)
			{
				dirTree->SortDirectoryEntries(true);

				dirTree->OutputHeaderListing( fp, 0, "<ROOT>" );

				dirTree->SortDirectoryEntries();

				fprintf( fp, "\n#endif\n" );

				fclose( fp );

				if ( !global::QuietMode )
				{
					printf( "Wrote file LBA listing header \"%" PRFILESYSTEM_PATH "\"\n\n",
						global::LBAheaderFile.c_str() );
				}
			}
			else
			{
				if ( !global::QuietMode )
				{
					printf( "Failed to write LBA listing header \"%" PRFILESYSTEM_PATH "\"!\n\n",
						global::LBAheaderFile.c_str() );
				}
			}
		}

		if ( !global::NoIsoGen )
		{
			// Create ISO image for writing
			cd::IsoWriter writer;

			if ( !writer.Create(global::ImageName, totalLenLBA ) ) {

				if ( !global::QuietMode )
				{
					printf( "  " );
				}

				printf( "ERROR: Cannot open or create output image file.\n" );
				return EXIT_FAILURE;

			}

			if ( global::cuefile )
			{
				unique_file cuefp = OpenScopedFile( *global::cuefile, "wb" );
				if ( cuefp == nullptr )
				{
					printf( "ERROR: Cannot open or create output cue sheet.\n" );
					return EXIT_FAILURE;
				}

				fwrite( cueSheet.c_str(), sizeof(char), cueSheet.length(), cuefp.get() );
			}

			// Write the file system
			if ( !global::QuietMode )
			{
				printf( "Writing ISO...\n"
						"  Writing files...\n" );
			}

			// Copy the files into the disc image
			dirTree->WriteFiles( &writer );

			if ( !global::QuietMode && !audioTracks.empty() )
			{
				printf("\n  Writing CDDA tracks...\n");
			}

			// Write out the audio tracks
			for (const auto* track : audioTracks)
			{
				const uint32_t sizeInSectors = GetSizeInSectors(track->length, CD_SECTOR_SIZE);
				auto sectorView = writer.GetRawSectorView(track->lba, sizeInSectors);

				// Pack the audio file
				if ( !global::QuietMode )
				{
					printf( "    Packing audio \"%" PRFILESYSTEM_PATH "\"... ", track->srcfile.c_str() );
					fflush(stdout);
				}

				if ( !PackFileAsCDDA( sectorView->GetRawBuffer(), track->srcfile ) )
				{
					return EXIT_FAILURE;
				}
				if ( !global::QuietMode )
				{
					printf( "Done.\n" );
				}
			}

			if ( !global::QuietMode )
			{
				printf( "\n" );
			}
						
			if ( !hasDataTrack )
			{
				goto close;
			}

			// Write license data
			if ( !global::LicenseFile.empty() )
			{
				FILE* fp = OpenFile( global::LicenseFile, "rb" );
				if (fp != nullptr)
				{
					auto license = std::make_unique<cd::ISO_LICENSE>();
					if (fread( license->data, sizeof(license->data), 1, fp ) == 1)
					{
						if ( !global::QuietMode )
						{
							printf( "  Writing license data..." );
						}

						iso::WriteLicenseData( &writer, license->data, global::ps2 );

						if ( !global::QuietMode )
						{
							printf( "Ok.\n" );
						}
					}
					fclose( fp );
				}
			}
			else
			{
				// Write blank sectors if no license data is to be injected
				auto appBlankSectors = 
					writer.GetSectorViewM2F1(0, 16, cd::IsoWriter::EdcEccForm::Form2);
				appBlankSectors->WriteBlankSectors(16);
			}

			// Write file system
			if ( !global::QuietMode )
			{
				printf( "  Writing directories... " );
			}

			// Write directory entries
			dirTree->WriteDirectoryRecords( &writer );

			// Write file system descriptors to finish the image
	        dirTree->WriteDescriptor( &writer, isoIdentifiers, totalLenLBA );

			if ( !global::QuietMode )
			{
				printf( "Ok.\n\n" );
			}

		close:
			writer.Close();

			if ( !global::QuietMode )
			{
				printf( "ISO image generated successfully.\n" );
				printf( "Total image size: %d bytes (%d sectors)\n",
					(CD_SECTOR_SIZE*totalLenLBA), totalLenLBA );
			}
		}
		else
		{
			printf( "Skipped generating ISO image.\n" );
		}

		// Check for next <iso_project> element
		projectElement = projectElement->NextSiblingElement(xml::elem::ISO_PROJECT);

	}

    return 0;
}

EntryAttributes ReadEntryAttributes(EntryAttributes current, const tinyxml2::XMLElement* dirElement)
{
	if (dirElement != nullptr)
	{
		auto getAttributeIfExists = [dirElement](auto& value, const char* name)
		{
			using type = std::decay_t<decltype(value)>;
			if constexpr (std::is_unsigned_v<type>)
			{
				value = static_cast<type>(dirElement->UnsignedAttribute(name, value));
			}
			else
			{
				value = static_cast<type>(dirElement->IntAttribute(name, value));
			}
		};

		getAttributeIfExists(current.GMTOffs, xml::attrib::GMT_OFFSET);
		getAttributeIfExists(current.HFLAG, xml::attrib::HIDDEN_FLAG);
		getAttributeIfExists(current.XAAttrib, xml::attrib::XA_ATTRIBUTES);
		getAttributeIfExists(current.XAPerm, xml::attrib::XA_PERMISSIONS);
		getAttributeIfExists(current.GID, xml::attrib::XA_GID);
		getAttributeIfExists(current.UID, xml::attrib::XA_UID);
		getAttributeIfExists(current.ORDER, xml::attrib::ORDER);
		getAttributeIfExists(current.FLBA, xml::attrib::OFFSET);
	}

	return current;
};

iso::DirTreeClass* ParseISOfileSystem(const tinyxml2::XMLElement* trackElement, const fs::path& xmlPath, iso::EntryList& entries, iso::IDENTIFIERS& isoIdentifiers, int& totalLen)
{
	const tinyxml2::XMLElement* identifierElement =
		trackElement->FirstChildElement(xml::elem::IDENTIFIERS);

	// Set file system identifiers
	if ( identifierElement != nullptr )
	{
		// Use individual elements defined by each attribute
		isoIdentifiers.SystemID		= identifierElement->Attribute(xml::attrib::SYSTEM_ID);
		isoIdentifiers.VolumeID		= identifierElement->Attribute(xml::attrib::VOLUME_ID);
		isoIdentifiers.VolumeSet	= identifierElement->Attribute(xml::attrib::VOLUME_SET);
		isoIdentifiers.Publisher	= identifierElement->Attribute(xml::attrib::PUBLISHER);
		isoIdentifiers.Application	= identifierElement->Attribute(xml::attrib::APPLICATION);
		isoIdentifiers.DataPreparer	= identifierElement->Attribute(xml::attrib::DATA_PREPARER);
		isoIdentifiers.Copyright	= identifierElement->Attribute(xml::attrib::COPYRIGHT);
		isoIdentifiers.CreationDate	= identifierElement->Attribute(xml::attrib::CREATION_DATE);
		isoIdentifiers.ModificationDate = identifierElement->Attribute(xml::attrib::MODIFICATION_DATE);
		isoIdentifiers.AbstractFile = identifierElement->Attribute(xml::attrib::ABSTRACT_FILE);
		isoIdentifiers.BibliographicFile = identifierElement->Attribute(xml::attrib::BIBLIOGRAPHIC_FILE);

		// Is an ID file specified?
		if( const char* identifierFile = identifierElement->Attribute(xml::attrib::ID_FILE) )
		{
			// Load the file as an XML document
			{
				tinyxml2::XMLError error;
				if (FILE* file = OpenFile(reinterpret_cast<const char8_t*>(identifierFile), "rb"); file != nullptr)
				{
					error = global::xmlIdFile.LoadFile(file);
					fclose(file);
				}
				else
				{
					error = tinyxml2::XML_ERROR_FILE_NOT_FOUND;
				}

				if ( error != tinyxml2::XML_SUCCESS )
				{
					printf("ERROR: ");
					if ( error == tinyxml2::XML_ERROR_FILE_NOT_FOUND )
					{
						printf("File not found.\n");
					}
					else if ( error == tinyxml2::XML_ERROR_FILE_COULD_NOT_BE_OPENED )
					{
						printf("File cannot be opened.\n");
					}
					else if ( error == tinyxml2::XML_ERROR_FILE_READ_ERROR )
					{
						printf("Error reading file.\n");
					}
					else
					{
						printf("%s on line %d\n", global::xmlIdFile.ErrorName(), global::xmlIdFile.ErrorLineNum());
					}
					return nullptr;
				}
			}
			
			// Get the identifier element, if there is one
			if( (identifierElement = global::xmlIdFile.FirstChildElement(xml::elem::IDENTIFIERS)) )
			{
				const char *str;
				// Use strings defined in file, otherwise leave ones already defined alone
				if( (str = identifierElement->Attribute(xml::attrib::SYSTEM_ID)) )
					isoIdentifiers.SystemID			= str;
				if( (str = identifierElement->Attribute(xml::attrib::VOLUME_ID)) )
					isoIdentifiers.VolumeID			= str;
				if( (str = identifierElement->Attribute(xml::attrib::VOLUME_SET)) )
					isoIdentifiers.VolumeSet		= str;
				if( (str = identifierElement->Attribute(xml::attrib::PUBLISHER)) )
					isoIdentifiers.Publisher		= str;
				if( (str = identifierElement->Attribute(xml::attrib::APPLICATION)) )
					isoIdentifiers.Application		= str;
				if( (str = identifierElement->Attribute(xml::attrib::DATA_PREPARER)) )
					isoIdentifiers.DataPreparer		= str;
				if( (str = identifierElement->Attribute(xml::attrib::COPYRIGHT)) )
					isoIdentifiers.Copyright		= str;
				if( (str = identifierElement->Attribute(xml::attrib::CREATION_DATE)) )
					isoIdentifiers.CreationDate		= str;
				if( (str = identifierElement->Attribute(xml::attrib::MODIFICATION_DATE)) )
					isoIdentifiers.ModificationDate = str;
				if( (str = identifierElement->Attribute(xml::attrib::ABSTRACT_FILE)) )
					isoIdentifiers.AbstractFile = str;
				if( (str = identifierElement->Attribute(xml::attrib::BIBLIOGRAPHIC_FILE)) )
					isoIdentifiers.BibliographicFile = str;
			}
		}
	}

	{ // Set default identifiers if not present
		bool hasSystemID = true;
		if ( isoIdentifiers.SystemID == nullptr )
		{
			hasSystemID = false;
			isoIdentifiers.SystemID = "PLAYSTATION";
		}

		bool hasApplication = true;
		if ( isoIdentifiers.Application == nullptr )
		{
			hasApplication = false;
			isoIdentifiers.Application = "PLAYSTATION";
		}

		if( global::volid_override )
		{
			isoIdentifiers.VolumeID = global::volid_override->c_str();
		}

		// Print out identifiers if present
		if ( !global::QuietMode )
		{
			printf("    Identifiers:\n");
			printf( "      System ID         : %s%s\n",
				isoIdentifiers.SystemID,
				hasSystemID ? "" : " (default)" );

			if ( isoIdentifiers.VolumeID != nullptr )
			{
				printf( "      Volume ID         : %s\n",
					isoIdentifiers.VolumeID );
			}
			if ( isoIdentifiers.VolumeSet != nullptr )
			{
				printf( "      Volume Set ID     : %s\n",
					isoIdentifiers.VolumeSet );
			}
			if ( isoIdentifiers.Publisher != nullptr )
			{
				printf( "      Publisher ID      : %s\n",
					isoIdentifiers.Publisher );
			}
			if ( isoIdentifiers.DataPreparer != nullptr )
			{
				printf( "      Data Preparer ID  : %s\n",
					isoIdentifiers.DataPreparer );
			}

			printf( "      Application ID    : %s%s\n",
				isoIdentifiers.Application,
				hasApplication ? "" : " (default)" );

			if ( isoIdentifiers.Copyright != nullptr )
			{
				printf( "      Copyright ID      : %s\n",
					isoIdentifiers.Copyright );
			}
			if ( isoIdentifiers.AbstractFile != nullptr )
			{
				printf( "      Abstract File ID  : %s\n",
					isoIdentifiers.AbstractFile );
			}
			if ( isoIdentifiers.BibliographicFile != nullptr )
			{
				printf( "      Bibliographic ID  : %s\n",
					isoIdentifiers.BibliographicFile );
			}
			if ( isoIdentifiers.CreationDate != nullptr )
			{
				printf( "      Creation Date     : %s\n",
					isoIdentifiers.CreationDate );
			}
			if ( isoIdentifiers.ModificationDate != nullptr )
			{
				printf( "      Modification Date : %s\n",
					isoIdentifiers.ModificationDate );
			}
			printf( "\n" );
		}
	}

	// Check for license file
	bool gotLicFromXML = false;
	const tinyxml2::XMLElement* licenseElement = trackElement->FirstChildElement(xml::elem::LICENSE);
	if ( global::LicenseFile.empty() && licenseElement != nullptr )
	{
		const char* license_file_attrib = licenseElement->Attribute(xml::attrib::LICENSE_FILE);

		if ( license_file_attrib == nullptr || *license_file_attrib == 0 )
		{
			printf( "ERROR: File attribute of <license> element is missing "
				"or blank on line %d.\n", licenseElement->GetLineNum() );
			return nullptr;
		}

		global::LicenseFile = (xmlPath / reinterpret_cast<const char8_t*>(license_file_attrib)).lexically_normal();
		gotLicFromXML = true;
	}

	// If still empty, blank sectors will be written.
	if ( !global::LicenseFile.empty() )
	{
		if ( !global::QuietMode )
		{
			printf( "    License file: \"%" PRFILESYSTEM_PATH "\"\n\n", global::LicenseFile.c_str() );
		}

		int64_t licenseSize = GetSize( global::LicenseFile );

		if ( licenseSize < 0 )
		{
			printf( "ERROR: Specified license file " );

			if ( gotLicFromXML )
			{
				printf( "(on line %d) ", licenseElement->GetLineNum() );
			}

			printf( "not found.\n" );

			return nullptr;
		}

		if ( licenseSize != sizeof(cd::ISO_LICENSE) && !global::noWarns )
		{
			if ( !global::QuietMode )
			{
				printf("    ");
			}
			printf( "WARNING: Specified license file may not be of "
				"correct format.\n" );
		}
	}

	// Establish the volume timestamp to either the current local time or isoIdentifiers.CreationDate (if specified)
	cd::ISO_DATESTAMP volumeDate;
	// Try to use time from XML. If it's malformed, fall back to local time.
	if ( !ParseDateFromString(volumeDate, isoIdentifiers.CreationDate) )
	{
		// Use local time
		const tm imageTime = *localtime( &global::BuildTime );

		volumeDate.year = imageTime.tm_year;
		volumeDate.month = imageTime.tm_mon + 1;
		volumeDate.day = imageTime.tm_mday;
		volumeDate.hour = imageTime.tm_hour;
		volumeDate.minute = imageTime.tm_min;
		volumeDate.second = imageTime.tm_sec;
		volumeDate.GMToffs = 36; // Use Japan GMT

		// Convert ISO_DATESTAMP to ISO_LONG_DATESTAMP char*
		static const std::string creationDate = DateToString(volumeDate, true);
		isoIdentifiers.CreationDate = creationDate.c_str();
	}

	// Establish default entry attributes from XML (if any)
	const EntryAttributes defaultAttributes = ReadEntryAttributes(EntryAttributes{}, trackElement->FirstChildElement(xml::elem::DEFAULT_ATTRIBUTES));

	// Parse directory entries in the directory_tree element
	if ( !global::QuietMode )
	{
		printf( "    Parsing directory tree...\n" );
	}

	const tinyxml2::XMLElement* directoryTree = trackElement->FirstChildElement(xml::elem::DIRECTORY_TREE);
	if ( directoryTree == nullptr )
	{
		if ( !global::QuietMode )
		{
			printf( "      " );
		}
		printf( "ERROR: No %s element specified for data track "
			"on line %d.\n", xml::elem::DIRECTORY_TREE, trackElement->GetLineNum() );
		return nullptr;
	}

	const char* dirTreePath = directoryTree->Attribute(xml::attrib::ENTRY_SOURCE);
	fs::path currentPath = dirTreePath != nullptr && *dirTreePath != 0
		? (xmlPath / reinterpret_cast<const char8_t*>(dirTreePath)).lexically_normal()
		: xmlPath;

	if (const char* rootDateStr = directoryTree->Attribute(xml::attrib::ENTRY_DATE))
	{
		ParseDateFromString(volumeDate, rootDateStr, volumeDate.GMToffs);
	}
	iso::DIRENTRY& root = iso::DirTreeClass::CreateRootDirectory(entries, volumeDate, ReadEntryAttributes(defaultAttributes, directoryTree));
	iso::DirTreeClass* dirTree = root.subdir.get();

	if ( !ParseDirectory(dirTree, directoryTree, xmlPath, defaultAttributes, currentPath) )
	{
		return nullptr;
	}

	int pathTableLen = dirTree->CalculatePathTableLen(root);

	// 16 license sectors + 2 header sectors
	const int rootLBA = 18+(GetSizeInSectors(pathTableLen)*4);

	// Sort directory entries, calculate tree LBAs and retrieve size of image
	dirTree->SortDirectoryEntries();
	totalLen = dirTree->CalculateTreeLBA(rootLBA);

	if ( !global::QuietMode )
	{
		printf( "      Files Total: %d\n", dirTree->GetFileCountTotal() );
		printf( "      Directories: %d\n", dirTree->GetDirCountTotal() );
		printf( "      Total file system size: %d bytes (%d sectors)\n\n",
			CD_SECTOR_SIZE*totalLen, totalLen);
	}

	return dirTree;
}

static bool ParseFileEntry(iso::DirTreeClass* dirTree, const tinyxml2::XMLElement* dirElement, const fs::path& xmlPath, const EntryAttributes& defaultAttributes, const fs::path& currentPath)
{
	const char* nameElement = dirElement->Attribute(xml::attrib::ENTRY_NAME);
	const char* sourceElement = dirElement->Attribute(xml::attrib::ENTRY_SOURCE);

	if ( (nameElement == nullptr || *nameElement == 0) && (sourceElement == nullptr || *sourceElement == 0) )
	{
		if ( !global::QuietMode )
		{
			printf("      ");
		}

		printf( "ERROR: Missing name and source attributes on "
			"line %d.\n", dirElement->GetLineNum() );

		return false;
	}

	fs::path srcFile;
	std::string name;
	if ( sourceElement != nullptr && *sourceElement != 0 )
	{
		srcFile = (xmlPath / reinterpret_cast<const char8_t*>(sourceElement)).lexically_normal();

		if ( nameElement != nullptr && *nameElement != 0 )
		{
			name = nameElement;
		}
		else
		{
			name = U8ToSv(srcFile.filename().u8string());
		}
	}
	else
	{
		name = nameElement;
		srcFile = currentPath / name;
	}

	{ // ECMA-119 7.5.1 - File Identifier shall contain one dot and uppercase alphanumeric characters or underscores.
		int dots = 0;
    	for (char &ch : name)
		{
			if (ch == '.')
			{
				if (++dots > 1)
				{
					printf("ERROR: File name '%s' on line %d shall contain only one dot.\n", name.c_str(), dirElement->GetLineNum());
					return false;
				}
			}
			else if (isalnum((unsigned char)ch))
			{
				ch = std::toupper((unsigned char)ch);
			}
			else if (ch != '_')
			{
				printf("ERROR: File name '%s' on line %d contains an invalid character '%c'.\n", name.c_str(), dirElement->GetLineNum(), ch);
				return false;
			}
		}
		/* Legend of Mana does not follow this rule.
		if (dots == 0)
			name += '.';*/
	}

	// ECMA-119 7.5.1 and 10.1 - File Identifier shall be 1-30 characters long plus one dot.
	if ( name.size() > 12 )
	{
		if ( name.size() > 31 )
		{
			printf( "ERROR: File name '%s' on line %d is more than 31 "
				"characters long.\n", name.c_str(), dirElement->GetLineNum() );
			return false;
		}
		if ( !global::noWarns )
		{
			if ( !global::QuietMode )
			{
				printf("      ");
			}
			printf( "WARNING: File name '%s' on line %d is more than 12 "
				"characters long.\n", name.c_str(), dirElement->GetLineNum() );
		}

		// ECMA-119 6.8.2.1 - The path length of any file shall not exceed 255.
		size_t pathLength = name.length() + 2; // 2 = ";1"
		int depth = dirTree->GetPathDepth(&pathLength);
		if (pathLength + depth > 255)
		{
			printf("ERROR: File path length exceeds 255 characters on line %d.\n", dirElement->GetLineNum());
			return false;
		}
	}

	EntryType entry = EntryType::EntryFile;
	const char *trackid = nullptr;

	const char* typeElement = dirElement->Attribute(xml::attrib::ENTRY_TYPE);
	if ( typeElement != nullptr )
	{
		if ( CompareICase( "data", typeElement ) )
		{
			entry = EntryType::EntryFile;
		} else if ( CompareICase( "mixed", typeElement ) ||
                    CompareICase( "xa", typeElement ) || //alias xa and str to mixed
                    CompareICase( "str", typeElement ) )
		{
			entry = EntryType::EntryXA;
		}
		else if ( CompareICase( "da", typeElement ) )
		{
			entry = EntryType::EntryDA;
			if ( !global::cuefile )
			{
				if ( !global::QuietMode )
				{
					printf( "      " );
				}
				printf( "ERROR: DA audio file(s) specified but no CUE sheet specified.\n" );
				return false;
			}
			trackid = dirElement->Attribute(xml::attrib::TRACK_ID);
			if ( trackid == nullptr || *trackid == 0 )
			{
				printf( "ERROR: DA audio file '%s' on line %d does not have an associated CDDA trackid.\n", name.c_str(), dirElement->GetLineNum() );
				return false;
			}
			// locate the node containing the tracks
			const tinyxml2::XMLElement *isoElement;
			for( const tinyxml2::XMLElement *parent = (tinyxml2::XMLElement *)dirElement->Parent(); ; parent = (tinyxml2::XMLElement *)parent->Parent())
			{
				if(parent == nullptr)
				{
					printf( "ERROR: locating <%s> elem, necessary for locating corresponding track to da file\n", xml::elem::ISO_PROJECT );
					return false;
				}
				if(CompareICase(parent->Name(), xml::elem::ISO_PROJECT))
				{
					isoElement = parent;
					break;
				}
			}
			// locate the track with trackid
			const tinyxml2::XMLElement *trackElement;
			for(trackElement = isoElement->FirstChildElement(xml::elem::TRACK); ; trackElement = trackElement->NextSiblingElement(xml::elem::TRACK))
			{
				if(trackElement == nullptr)
				{
					printf( "ERROR: locating <%s %s=\"audio\" %s=\"%s\"> for da file\n", xml::elem::TRACK, xml::attrib::TRACK_TYPE, xml::attrib::TRACK_ID, trackid);
					return false;
				}
				if(trackElement->Attribute(xml::attrib::TRACK_TYPE, "audio") && trackElement->Attribute(xml::attrib::TRACK_ID, trackid))
				{
					break;
				}
			}
			// set the src file to the trackid source
			sourceElement = trackElement->Attribute(xml::attrib::TRACK_SOURCE);
			if(sourceElement == nullptr || *sourceElement == 0)
			{
				// Safe cast, the root object is mutable. Casting here to modify the attribute without refactoring the entire call chain.
				const_cast<tinyxml2::XMLElement*>(trackElement)->SetAttribute(xml::attrib::ENTRY_SOURCE, reinterpret_cast<const char*>(srcFile.generic_u8string().c_str()));
			}
			else
			{
				srcFile = (xmlPath / reinterpret_cast<const char8_t*>(sourceElement)).lexically_normal();
			}
		}
		else
		{
			if ( !global::QuietMode )
			{
				printf( "      " );
			}

			printf( "ERROR: Unknown type %s on line %d\n",
				dirElement->Attribute(xml::attrib::ENTRY_TYPE),
				dirElement->GetLineNum() );

			return false;
		}
	}

	return dirTree->AddFileEntry(std::move(name), entry, std::move(srcFile), ReadEntryAttributes(defaultAttributes, dirElement), trackid, dirElement->Attribute(xml::attrib::ENTRY_DATE));
}

static bool ParseDirEntry(iso::DirTreeClass* dirTree, const tinyxml2::XMLElement* dirElement, const fs::path& xmlPath, const EntryAttributes& defaultAttributes, const fs::path& currentPath)
{
	fs::path srcDir;
	std::string name;
	if (const char *sourceElement = dirElement->Attribute(xml::attrib::ENTRY_SOURCE); sourceElement != nullptr && *sourceElement != 0)
	{
		srcDir = (xmlPath / reinterpret_cast<const char8_t*>(sourceElement)).lexically_normal();
	}

	if (const char *nameElement = dirElement->Attribute(xml::attrib::ENTRY_NAME); nameElement != nullptr && *nameElement != 0)
	{
		name = nameElement;
		if (srcDir.empty())
		{
			srcDir = currentPath / name;
		}
	}
	else if (!srcDir.empty())
	{
		name = U8ToSv(srcDir.filename().u8string());
	}
	else
	{
		printf("ERROR: Directory name missing on line %d.\n", dirElement->GetLineNum());
		return false;
	}

	// ECMA-119 7.6.3 and 10.1 - Directory Identifier shall be 1-31 characters long.
	if ( name.length() > 8 )
	{
		if ( name.length() > 31 )
		{
			printf( "ERROR: Directory name '%s' on line %d is more than 31 "
				"characters long.\n", name.c_str(), dirElement->GetLineNum() );
			return false;
		}
		if ( !global::noWarns )
		{
			if ( !global::QuietMode )
			{
				printf("      ");
			}
			printf( "WARNING: Directory name '%s' on line %d is more than 8 "
				"characters long.\n", name.c_str(), dirElement->GetLineNum() );
		}
	}

	{ // ECMA-119 6.8.2.1 - The number of levels in the hierarchy shall not exceed eight.
		int level = dirTree->GetPathDepth() + 2; // +1 for root, +1 for this dir
		if (level > 8)
		{
			printf("ERROR: Directory hierarchy depth exceeds 8 levels on line %d.\n", dirElement->GetLineNum());
			return false;
		}
	}

	// ECMA-119 7.6.1 - Directory Identifier shall contain uppercase alphanumeric characters or underscores.
    for (char &ch : name)
	{
		if (isalnum((unsigned char)ch))
		{
			ch = std::toupper((unsigned char)ch);
		}
		else if (ch != '_')
		{
			printf("ERROR: Directory name '%s' on line %d contains an invalid character '%c'.\n",
				name.c_str(), dirElement->GetLineNum(), ch);
			return false;
		}
	}

	bool alreadyExists = false;
	iso::DirTreeClass* subdir = dirTree->AddSubDirEntry(
		std::move(name), srcDir, ReadEntryAttributes(defaultAttributes, dirElement), alreadyExists, dirElement->Attribute(xml::attrib::ENTRY_DATE) );

	if ( subdir == nullptr )
	{
		return false;
	}

	return ParseDirectory(subdir, dirElement, xmlPath, defaultAttributes, srcDir);
}

bool ParseDirectory(iso::DirTreeClass* dirTree, const tinyxml2::XMLElement* parentElement, const fs::path& xmlPath, const EntryAttributes& defaultAttributes, const fs::path& currentPath)
{
	for ( const tinyxml2::XMLElement* dirElement = parentElement->FirstChildElement(); dirElement != nullptr; dirElement = dirElement->NextSiblingElement() )
	{
		
		if ( CompareICase( "file", dirElement->Name() ))
		{
			if (!ParseFileEntry(dirTree, dirElement, xmlPath, defaultAttributes, currentPath))
			{
				return false;
			}
		}
		else if ( CompareICase( "dummy", dirElement->Name() ))
		{
			dirTree->AddDummyEntry( dirElement->UnsignedAttribute(xml::attrib::NUM_DUMMY_SECTORS),
									dirElement->UnsignedAttribute(xml::attrib::ENTRY_TYPE),
									dirElement->UnsignedAttribute(xml::attrib::OFFSET),
									dirElement->BoolAttribute(xml::attrib::ECC_ADDRESS) );
        }
		else if ( CompareICase( "dir", dirElement->Name() ))
		{
			if (!ParseDirEntry(dirTree, dirElement, xmlPath, defaultAttributes, currentPath))
			{
				return false;
			}
        }
	}

	return true;
}

bool PackFileAsCDDA(void* buffer, const fs::path& audioFile)
{
	// open the decoder
	ma_decoder decoder;
	VirtualWav vw;
	bool isLossy;
	bool isPCM;
	if(ma_redbook_decoder_init_path_by_ext(audioFile, &decoder, &vw, isLossy, isPCM) != MA_SUCCESS)
	{
		ma_decoder_uninit(&decoder);
		return false;
	}
	else if (isPCM && !global::QuietMode && !global::noWarns)
	{
		printf("\n      WARNING: Guessing it's signed 16 bit stereo @ 44100 kHz pcm audio... ");
	}

	// note if there's some data converting going on
	ma_format internalFormat;
	ma_uint32 internalChannels;
	ma_uint32 internalSampleRate;
	if(ma_data_source_get_data_format(decoder.pBackend, &internalFormat, &internalChannels, &internalSampleRate, NULL, 0) != MA_SUCCESS)
	{
		printf("\n    ERROR: unable to get internal metadata for \"%" PRFILESYSTEM_PATH "\"\n", audioFile.c_str());
		ma_decoder_uninit(&decoder);
		return false;
	}
	if(((internalFormat != ma_format_s16) || (internalChannels != 2) || (internalSampleRate != 44100) || isLossy) && !global::QuietMode && !global::noWarns)
	{
		printf("\n      WARNING: This is not Redbook audio, converting... ");
	}

	// get expected pcm frame count (if your file isn't redbook this can vary from the input file's amount)
	// unfortunately it needs to decode the whole file to determine this for mp3
	ma_uint64 expectedPCMFrames;
	if(ma_decoder_get_length_in_pcm_frames(&decoder, &expectedPCMFrames) != MA_SUCCESS)
	{
		printf("\n    ERROR: corrupt file? unable to get_length_in_pcm_frames\n");
		ma_decoder_uninit(&decoder);
		return false;
	}

	ma_uint64 framesRead;
	ma_decoder_read_pcm_frames(&decoder, buffer, expectedPCMFrames, &framesRead);
	ma_decoder_uninit(&decoder);

	if(framesRead != expectedPCMFrames)
	{
		printf("\n    ERROR: corrupt file? (framesRead != expectedPCMFrames)\n");
		return false;
	}
	return true;
}
