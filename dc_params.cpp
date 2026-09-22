// ---------------------------------------------------------
// DC (UNPACKED) PARAMETER EDITING
//
// The retail / dc build keeps the hud parameters as loose
// .params files under Deferred\GameModeHUDs (or Data\GameModeHUDs
// on some layouts). This module rewrites the Target Player Element
// placement in every mode file, keeping a .bak of each file it
// touches. Running it twice is harmless.
// ---------------------------------------------------------
#include "dc_params.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "resource.h"

namespace dc {

namespace {

// ---------------------------------------------------------
// THE VALUES
// ---------------------------------------------------------
// 'Enable Target Player Element' has to stay false. With it on, the game's own
// HUD logic drives that element - showing the opponent name tag and hiding it
// again - which fights the speed text the mod writes every frame. The mod
// calls Show/SetTargetPlayerName itself, so it needs nothing from this flag.
struct SectionRule {
    const char* name;  // section leaf, e.g. "Splitscreen Horizontal"
    const char* x;
    const char* y;
};

const SectionRule kRules[] = {
    { "Bumper",                 "-0.435", "0.815" },
    { "Chase",                  "-0.135", "0.285" },
    { "Splitscreen Horizontal", "-0.365", "0.970" },
    { "Splitscreen Vertical",   "-0.115", "0.740" },
};

// Bonnet keeps whatever the game shipped with, so it has no rule.

const char* kXKey      = "Target Player Element X";
const char* kYKey      = "Target Player Element Y";
const char* kEnableKey = "Enable Target Player Element";

const char* kIndent = "\t";

// ---------------------------------------------------------
// SMALL HELPERS
// ---------------------------------------------------------
bool DirExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ParentDir(const std::string& p) {
    const size_t slash = p.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    if (slash == 2 && p.size() > 1 && p[1] == ':') return p.substr(0, 3);  // "D:\"
    if (slash == 0) return std::string();
    return p.substr(0, slash);
}

// "  '/Section/Leaf:'  ->  "Leaf"
bool SectionLeaf(const std::string& line, std::string& leaf) {
    if (line.empty() || line[0] != '/') return false;

    const size_t end = line.find_last_not_of(" \t\r\n");
    if (end == std::string::npos || line[end] != ':') return false;

    const std::string full = line.substr(1, end - 1);
    const size_t slash = full.find_last_of('/');
    leaf = (slash == std::string::npos) ? full : full.substr(slash + 1);
    return true;
}

// "'Some Key' = 1.0 (0.0, 1.0);"  ->  "Some Key"
bool LineKey(const std::string& line, std::string& key) {
    const size_t open = line.find('\'');
    if (open == std::string::npos) return false;

    const size_t close = line.find('\'', open + 1);
    if (close == std::string::npos) return false;

    if (line.find('=', close + 1) == std::string::npos) return false;

    key = line.substr(open + 1, close - open - 1);
    return true;
}

const SectionRule* FindRule(const std::string& leaf) {
    for (const SectionRule& rule : kRules)
        if (leaf == rule.name) return &rule;
    return nullptr;
}

// ---------------------------------------------------------
// EDITING ONE SECTION
// ---------------------------------------------------------
void SetKey(std::vector<std::string>& body, const std::string& key,
            const std::string& desired, bool& changed) {
    for (std::string& line : body) {
        std::string k;
        if (!LineKey(line, k)) continue;
        if (k != key) continue;

        if (line != desired) {
            line = desired;
            changed = true;
        }
        return;
    }

    // Not there yet: insert it where the game's own serialiser would have put
    // it (these files are written in alphabetical key order).
    size_t at = body.size();
    for (size_t i = 0; i < body.size(); ++i) {
        std::string k;
        if (!LineKey(body[i], k)) continue;
        if (k > key) { at = i; break; }
    }
    body.insert(body.begin() + at, desired);
    changed = true;
}

void EditSection(std::vector<std::string>& body, const SectionRule& rule, bool& changed) {
    SetKey(body, kEnableKey, std::string(kIndent) + "'" + kEnableKey + "' = false;", changed);
    SetKey(body, kXKey, std::string(kIndent) + "'" + kXKey + "' = " + rule.x + " (-1.0, 1.0);", changed);
    SetKey(body, kYKey, std::string(kIndent) + "'" + kYKey + "' = " + rule.y + " (0.0, 2.0);", changed);
}

// ---------------------------------------------------------
// FILE HANDLING
// ---------------------------------------------------------
std::vector<std::string> ReadLines(const std::string& path, bool& ok) {
    std::vector<std::string> lines;
    ok = false;

    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return lines;

    std::string text;
    char buffer[4096];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), fp)) > 0)
        text.append(buffer, read);
    fclose(fp);
    ok = true;

    std::string line;
    for (char c : text) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) {  // file without a trailing newline
        if (line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

bool WriteLines(const std::string& path, const std::vector<std::string>& lines) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return false;

    for (const std::string& line : lines) {
        fwrite(line.c_str(), 1, line.size(), fp);
        fwrite("\r\n", 1, 2, fp);
    }
    fclose(fp);
    return true;
}

// The original is kept once; a second run never overwrites a good backup.
bool BackupOnce(const std::string& path) {
    const std::string backup = path + ".bak";
    if (GetFileAttributesA(backup.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return CopyFileA(path.c_str(), backup.c_str(), TRUE) != 0;
}

// ---------------------------------------------------------
// ONE .params FILE
// ---------------------------------------------------------
enum class FileResult { Skipped, Unchanged, Updated, Failed };

FileResult ProcessFile(const std::string& path, bool& changedAny, std::string& error) {
    bool ok = false;
    const std::vector<std::string> lines = ReadLines(path, ok);
    if (!ok) {
        error = "cannot read";
        return FileResult::Failed;
    }

    // A mode that has no target player element at all (HelicopterMulti, for
    // instance) must not get keys invented for it - the game would not know
    // them.
    bool knowsElement = false;
    for (const std::string& line : lines) {
        std::string key;
        if (LineKey(line, key) && key == kXKey) { knowsElement = true; break; }
    }
    if (!knowsElement) return FileResult::Skipped;

    std::vector<std::string> out;
    bool changed = false;

    size_t i = 0;
    while (i < lines.size()) {
        std::string leaf;
        if (!SectionLeaf(lines[i], leaf)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        // Collect the whole section: the header plus every line up to the next
        // one.
        size_t end = i + 1;
        std::string next;
        while (end < lines.size() && !SectionLeaf(lines[end], next)) ++end;

        out.push_back(lines[i]);

        const SectionRule* rule = FindRule(leaf);
        if (rule) {
            std::vector<std::string> body(lines.begin() + i + 1, lines.begin() + end);

            bool sectionChanged = false;
            EditSection(body, *rule, sectionChanged);

            if (sectionChanged) {
                changed = true;
                changedAny = true;
            }
            out.insert(out.end(), body.begin(), body.end());
        } else {
            out.insert(out.end(), lines.begin() + i + 1, lines.begin() + end);
        }

        i = end;
    }

    if (!changed) return FileResult::Unchanged;

    if (!BackupOnce(path)) {
        error = "cannot write backup";
        return FileResult::Failed;
    }
    if (!WriteLines(path, out)) {
        error = "cannot write file";
        return FileResult::Failed;
    }
    return FileResult::Updated;
}

// ---------------------------------------------------------
// LOCATING THE GAME
// ---------------------------------------------------------
// The hud parameters live under a different root depending on the build: the
// retail exe reads Deferred\GameModeHUDs, the steam / xenon exe Data\GameModeHUDs.
void AddCandidates(std::vector<std::string>& out, const std::string& dir) {
    out.push_back(dir + "\\Deferred\\GameModeHUDs");
    out.push_back(dir + "\\Data\\GameModeHUDs");
}

bool FindHudDir(const std::string& start, std::string& found) {
    std::vector<std::string> candidates;

    std::string dir = start;
    for (int level = 0; level < 4 && !dir.empty(); ++level) {
        AddCandidates(candidates, dir);

        // The exe may sit one folder above the game's own directory.
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
                    AddCandidates(candidates, dir + "\\" + fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        const std::string parent = ParentDir(dir);
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }

    for (const std::string& candidate : candidates) {
        if (DirExists(candidate)) {
            found = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------
// PUBLIC API
// ---------------------------------------------------------
bool probe(const std::string& root, std::string& hudDir) {
    return FindHudDir(root, hudDir);
}

namespace {

// The embedded movie, from the resource compiler.
bool EmbeddedGfx(std::vector<std::uint8_t>& out) {
    const HRSRC res = FindResourceA(nullptr, MAKEINTRESOURCEA(IDR_DIALSINFO_GFX),
                                    LPCSTR(RT_RCDATA));
    if (!res) return false;
    const HGLOBAL handle = LoadResource(nullptr, res);
    if (!handle) return false;
    const void* data = LockResource(handle);
    const std::size_t size = static_cast<std::size_t>(SizeofResource(nullptr, res));
    if (!data || size == 0) return false;
    out.assign(static_cast<const std::uint8_t*>(data),
               static_cast<const std::uint8_t*>(data) + size);
    return true;
}

}  // namespace

bool installGfx(const std::string& root, std::string& error) {
    std::vector<std::uint8_t> gfx;
    if (!EmbeddedGfx(gfx)) {
        error = "embedded DialsInfo.gfx is missing";
        return false;
    }

    // Same search as FindHudDir, but for the ui\hud folder.
    std::vector<std::string> candidates;
    std::string dir = root;
    for (int level = 0; level < 4 && !dir.empty(); ++level) {
        candidates.push_back(dir + "\\Deferred\\UI\\HUD");
        candidates.push_back(dir + "\\Data\\UI\\HUD");

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                    candidates.push_back(dir + "\\" + fd.cFileName + "\\Deferred\\UI\\HUD");
                    candidates.push_back(dir + "\\" + fd.cFileName + "\\Data\\UI\\HUD");
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        const std::string parent = ParentDir(dir);
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }

    std::string target;
    for (const std::string& candidate : candidates) {
        const DWORD attrs = GetFileAttributesA(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            target = candidate;
            break;
        }
    }
    if (target.empty()) {
        error = "Could not find the ui hud folder (Deferred\\UI\\HUD or Data\\UI\\HUD).";
        return false;
    }

    const std::string path = target + "\\DialsInfo.gfx";

    // Skip when the game already carries exactly these bytes.
    HANDLE existing = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    if (existing != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        GetFileSizeEx(existing, &size);
        bool same = size.QuadPart == static_cast<LONGLONG>(gfx.size());
        if (same) {
            std::vector<char> onDisk(gfx.size());
            DWORD read = 0;
            if (ReadFile(existing, onDisk.data(), static_cast<DWORD>(gfx.size()), &read,
                         nullptr) && read == gfx.size()) {
                same = std::memcmp(onDisk.data(), gfx.data(), gfx.size()) == 0;
            } else {
                same = false;
            }
        }
        CloseHandle(existing);
        if (same) return true;  // already installed
    }

    const std::string backup = path + ".bak";
    if (GetFileAttributesA(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // a backup exists already; keep it
    } else if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileA(path.c_str(), backup.c_str(), TRUE)) {
            error = "cannot write the backup of DialsInfo.gfx";
            return false;
        }
    }

    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) {
        error = "cannot write DialsInfo.gfx";
        return false;
    }
    fwrite(gfx.data(), 1, gfx.size(), fp);
    fclose(fp);
    return true;
}

Result applyParams(const std::string& root) {
    Result result;

    if (!FindHudDir(root, result.hudDir)) {
        result.error = "Could not find the hud parameters "
                       "(Deferred\\GameModeHUDs or Data\\GameModeHUDs).";
        return result;
    }
    result.found = true;

    std::vector<std::string> files;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((result.hudDir + "\\*.params").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(result.hudDir + "\\" + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());

    for (const std::string& path : files) {
        std::string error;
        bool touched = false;
        switch (ProcessFile(path, touched, error)) {
        case FileResult::Updated:   ++result.updated;   break;
        case FileResult::Unchanged: ++result.unchanged; break;
        case FileResult::Skipped:   ++result.skipped;   break;
        case FileResult::Failed:
            ++result.failed;
            if (result.error.empty()) result.error = error + ": " + path;
            break;
        }
    }
    return result;
}

}  // namespace dc
