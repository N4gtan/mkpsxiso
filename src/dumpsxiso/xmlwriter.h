#pragma once

#include "cdreader.h"

namespace xml
{
	// Writes out entries, automatically filling LBA gaps with dummies.
	// Returns the last inferred LBA.
	unsigned WriteXML ( const cd::ISO_DESCRIPTOR& descriptor,
						const std::unique_ptr<cd::IsoDirEntries>& rootDir,
						const std::list<cd::IsoDirEntries::Entry*>& DAfiles,
						const unsigned postGap );
}
