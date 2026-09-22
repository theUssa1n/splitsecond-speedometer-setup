// ---------------------------------------------------------
// DC (UNPACKED) PARAMETER EDITING
//
// The retail / dc build keeps the hud parameters as loose
// .params files under Deferred\GameModeHUDs (or Data\GameModeHUDs
// on some layouts). This module rewrites the Target Player Element
// placement in every mode file, keeping a .bak of each file it
// touches. Running it twice is harmless.
// ---------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace dc {

struct Result {
    int updated = 0;
    int unchanged = 0;
    int skipped = 0;
    int failed = 0;
    bool found = false;       // a GameModeHUDs folder was located at all
    std::string hudDir;       // where the parameters were found
    std::string error;
};

// Locates the game's GameModeHUDs folder without touching any file.
// Returns true and fills `hudDir` when found.
bool probe(const std::string& root, std::string& hudDir);

// Applies the speedometer's camera placement to every .params file under
// the game's GameModeHUDs folder. `root` is the folder to search from
// (the folder the tool was run from). Returns what happened.
Result applyParams(const std::string& root);

// Installs the embedded DialsInfo.gfx into the game's ui folder
// (Deferred\UI\HUD or Data\UI\HUD). The original is kept once as .bak.
// Returns false with `error` set when the ui folder could not be located.
bool installGfx(const std::string& root, std::string& error);

}  // namespace dc
