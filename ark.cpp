#include "ark.h"

#include "resource.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ark {
namespace {

// ---------------------------------------------------------
// PATHS
// ---------------------------------------------------------
std::string fileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool fileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------
// THE ARK HASH
// ---------------------------------------------------------
// The game looks an entry up by hash, and the hash is folded over the path
// in lower case: t = c + t * 31.
std::uint32_t pathHash(const char* path) {
    std::uint32_t value = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
        value = static_cast<std::uint32_t>(c) + value * 31u;
    }
    return value;
}

// ---------------------------------------------------------
// ARK STRUCTURES
// ---------------------------------------------------------
struct Header {
    std::uint32_t magic;     // 0x00010000
    std::uint32_t hdrLen;    // 16 + 20 * count
    std::uint32_t count;
    std::uint32_t unknown;   // always 16
    std::uint32_t strCount;  // always 0
    std::uint32_t strOffset; // == hdrLen
};

// The game's lzss, as the ark stores it: the first four bytes hold the length
// the block decompresses to.
bool lzssDecompress(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) {
    if (in.size() < 4) return false;

    out.clear();
    out.reserve(in.size() * 2);

    std::uint8_t window[4096] = {};
    std::size_t windowPos = 4078;
    std::size_t i = 4;
    std::uint32_t flags = 0;
    int flagCount = 0;

    while (i < in.size()) {
        if (flagCount == 0) {
            flags = in[i++];
            flagCount = 8;
        }
        if (flags & 1u) {
            if (i >= in.size()) break;
            const std::uint8_t byte = in[i++];
            out.push_back(byte);
            window[windowPos] = byte;
            windowPos = (windowPos + 1) & 0xFFF;
        } else {
            if (i + 2 > in.size()) break;
            const std::uint32_t b0 = in[i];
            const std::uint32_t b1 = in[i + 1];
            i += 2;
            const std::size_t offset = b0 | ((b1 & 0xF0u) << 4);
            const std::size_t length = (b1 & 0x0Fu) + 3;
            for (std::size_t k = 0; k < length; ++k) {
                const std::uint8_t byte = window[(offset + k) & 0xFFF];
                out.push_back(byte);
                window[windowPos] = byte;
                windowPos = (windowPos + 1) & 0xFFF;
            }
        }
        flags >>= 1;
        --flagCount;
    }
    return true;
}

// ---------------------------------------------------------
// EDITING ONE .PARAMS FILE
// ---------------------------------------------------------
struct SectionRule {
    const char* name;
    const char* x;
    const char* y;
};

// 'Enable Target Player Element' has to stay false: with it on, the game's own
// hud logic drives that element - showing the opponent's name tag and hiding
// it again - which fights the speed text the mod writes every frame. The mod
// calls Show itself, so it needs nothing from that flag.
//
// Bonnet keeps whatever the game shipped with, so it has no rule.
const SectionRule kRules[] = {
    { "Bumper",                 "-0.435", "0.815" },
    { "Chase",                  "-0.135", "0.285" },
    { "Splitscreen Horizontal", "-0.365", "0.970" },
    { "Splitscreen Vertical",   "-0.115", "0.740" },
};

const char* kXKey      = "Target Player Element X";
const char* kYKey      = "Target Player Element Y";
const char* kEnableKey = "Enable Target Player Element";
const char* kIndent    = "\t";

// "  '/Section/Leaf:'  ->  "Leaf"
bool sectionLeaf(const std::string& line, std::string& leaf) {
    if (line.empty() || line[0] != '/') return false;

    const std::size_t end = line.find_last_not_of(" \t\r\n");
    if (end == std::string::npos || line[end] != ':') return false;

    const std::string full = line.substr(1, end - 1);
    const std::size_t slash = full.find_last_of('/');
    leaf = (slash == std::string::npos) ? full : full.substr(slash + 1);
    return true;
}

// "'Some Key' = 1.0 (0.0, 1.0);"  ->  "Some Key"
bool lineKey(const std::string& line, std::string& key) {
    const std::size_t open = line.find('\'');
    if (open == std::string::npos) return false;

    const std::size_t close = line.find('\'', open + 1);
    if (close == std::string::npos) return false;

    if (line.find('=', close + 1) == std::string::npos) return false;

    key = line.substr(open + 1, close - open - 1);
    return true;
}

const SectionRule* findRule(const std::string& leaf) {
    for (const SectionRule& rule : kRules)
        if (leaf == rule.name) return &rule;
    return nullptr;
}

void setKey(std::vector<std::string>& body, const std::string& key,
            const std::string& desired, bool& changed) {
    for (std::string& line : body) {
        std::string k;
        if (!lineKey(line, k)) continue;
        if (k != key) continue;

        if (line != desired) {
            line = desired;
            changed = true;
        }
        return;
    }

    // Not there yet: put it where the game's own serialiser would have (these
    // files are written in alphabetical key order).
    std::size_t at = body.size();
    for (std::size_t i = 0; i < body.size(); ++i) {
        std::string k;
        if (!lineKey(body[i], k)) continue;
        if (k > key) { at = i; break; }
    }
    body.insert(body.begin() + at, desired);
    changed = true;
}

void editSection(std::vector<std::string>& body, const SectionRule& rule, bool& changed) {
    setKey(body, kEnableKey, std::string(kIndent) + "'" + kEnableKey + "' = false;", changed);
    setKey(body, kXKey, std::string(kIndent) + "'" + kXKey + "' = " + rule.x + " (-1.0, 1.0);", changed);
    setKey(body, kYKey, std::string(kIndent) + "'" + kYKey + "' = " + rule.y + " (0.0, 2.0);", changed);
}

void bytesToLines(const std::vector<std::uint8_t>& data, std::vector<std::string>& lines) {
    lines.clear();
    std::string line;
    for (const std::uint8_t c : data) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(static_cast<char>(c));
        }
    }
    if (!line.empty()) {
        if (line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
}

void linesToBytes(const std::vector<std::string>& lines, std::vector<std::uint8_t>& data) {
    data.clear();
    for (const std::string& line : lines) {
        data.insert(data.end(), line.begin(), line.end());
        data.push_back('\r');
        data.push_back('\n');
    }
}

enum class EditResult { Updated, Unchanged, Skipped, Failed };

EditResult editParamsText(const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst) {
    std::vector<std::string> lines;
    bytesToLines(src, lines);

    // A mode that has no target player element at all must not get keys
    // invented for it - the game would not know them.
    bool knowsElement = false;
    for (const std::string& line : lines) {
        std::string key;
        if (lineKey(line, key) && key == kXKey) { knowsElement = true; break; }
    }
    if (!knowsElement) return EditResult::Skipped;

    std::vector<std::string> out;
    bool changed = false;

    std::size_t i = 0;
    while (i < lines.size()) {
        std::string leaf;
        if (!sectionLeaf(lines[i], leaf)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        std::size_t end = i + 1;
        std::string next;
        while (end < lines.size() && !sectionLeaf(lines[end], next)) ++end;

        out.push_back(lines[i]);

        const SectionRule* rule = findRule(leaf);
        if (rule) {
            std::vector<std::string> body(lines.begin() + i + 1, lines.begin() + end);
            bool sectionChanged = false;
            editSection(body, *rule, sectionChanged);
            changed = changed || sectionChanged;
            out.insert(out.end(), body.begin(), body.end());
        } else {
            out.insert(out.end(), lines.begin() + i + 1, lines.begin() + end);
        }

        i = end;
    }

    if (!changed) return EditResult::Unchanged;
    linesToBytes(out, dst);
    return EditResult::Updated;
}

// ---------------------------------------------------------
// WHAT WE PUSH INTO THE ARCHIVE
// ---------------------------------------------------------
// One section per camera, and these are the files the game picks from per
// mode. Paths are exactly what the archive stores (no prefix).
const char* kParamsPaths[] = {
    "GameModeHUDs/Demolition.params",
    "GameModeHUDs/Elimination.params",
    "GameModeHUDs/HelicopterRevenge.params",
    "GameModeHUDs/HelicopterSurvival.params",
    "GameModeHUDs/KingOfTheAir.params",
    "GameModeHUDs/Nemesis.params",
    "GameModeHUDs/NemesisMulti.params",
    "GameModeHUDs/NemesisRace.params",
    "GameModeHUDs/NemesisSplitScreen.params",
    "GameModeHUDs/QuickRace.params",
    "GameModeHUDs/TimeTrial.params",
    "GameModeHUDs/TimeTrialPlus.params",
    "GameModeHUDs/Tutorial.params",
};

const char* kGfxPath = "UI/HUD/DialsInfo.gfx";

// ---------------------------------------------------------
// EMBEDDED PAYLOADS
// ---------------------------------------------------------
bool loadResource(int id, std::vector<std::uint8_t>& out) {
    const HRSRC res = FindResourceA(nullptr, MAKEINTRESOURCEA(id), LPCSTR(RT_RCDATA));
    if (!res) return false;

    const DWORD size = SizeofResource(nullptr, res);
    const HGLOBAL handle = LoadResource(nullptr, res);
    if (!handle || size == 0) return false;

    const void* locked = LockResource(handle);
    if (!locked) return false;

    const auto* bytes = static_cast<const std::uint8_t*>(locked);
    out.assign(bytes, bytes + size);
    return true;
}

const std::vector<std::uint8_t>& embeddedGfx() {
    static std::vector<std::uint8_t> data;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        loadResource(IDR_DIALSINFO_GFX, data);
    }
    return data;
}

// The archive's own path list, used only to explain a miss.
const std::vector<std::string>& pathList() {
    static std::vector<std::string> lines;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;

        std::vector<std::uint8_t> raw;
        if (loadResource(IDR_ARK_PATHS, raw)) {
            std::string line;
            for (const std::uint8_t c : raw) {
                if (c == '\n') {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) lines.push_back(line);
                    line.clear();
                } else {
                    line.push_back(static_cast<char>(c));
                }
            }
            if (!line.empty()) lines.push_back(line);
        }
    }
    return lines;
}

// "game mode huds/quickrace.params" is not in the archive - is the same file
// stored under another prefix? The list is what answers that.
std::string nearMiss(const std::string& wanted) {
    const std::string wantName = fileName(wanted);

    std::string hint;
    int shown = 0;
    for (const std::string& candidate : pathList()) {
        if (candidate.size() != wanted.size()) continue;
        if (_stricmp(candidate.c_str(), wanted.c_str()) == 0) continue;
        if (_stricmp(fileName(candidate).c_str(), wantName.c_str()) != 0) continue;

        if (!hint.empty()) hint += ", ";
        hint += candidate;
        if (++shown == 2) break;
    }
    return hint;
}

// ---------------------------------------------------------
// UNDO FILE
// ---------------------------------------------------------
struct UndoRow {
    std::uint32_t index;
    Entry original;
};

bool undoPath(const std::string& arkPath, std::string& out) {
    out = arkPath + ".speedo-undo";
    return true;
}

bool writeUndo(const std::string& arkPath, std::uint64_t originalSize,
               const std::vector<UndoRow>& rows, std::string& error) {
    std::string path;
    undoPath(arkPath, path);

    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) {
        error = "cannot write " + fileName(path);
        return false;
    }

    const char magic[4] = { 'U', 'S', 'D', '1' };
    const std::uint32_t count = static_cast<std::uint32_t>(rows.size());
    fwrite(magic, 1, 4, fp);
    fwrite(&originalSize, 1, 8, fp);
    fwrite(&count, 1, 4, fp);
    for (const UndoRow& row : rows) {
        fwrite(&row.index, 1, 4, fp);
        fwrite(&row.original, 1, 16, fp);
    }
    fclose(fp);
    return true;
}

}  // namespace

// ---------------------------------------------------------
// ARCHIVE
// ---------------------------------------------------------
Archive::~Archive() {
    close();
}

void Archive::close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    entries_.clear();
    path_.clear();
    fileSize_ = 0;
    rawCount_ = 0;
    compressedCount_ = 0;
}

bool Archive::seek(std::uint64_t offset, std::string& error) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) {
        error = "seek failed";
        return false;
    }
    return true;
}

bool Archive::open(const std::string& path, std::string& error) {
    close();

    handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        error = "cannot open for writing - is the game running?";
        return false;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_, &size)) {
        error = "cannot read the file size";
        return false;
    }
    fileSize_ = static_cast<std::uint64_t>(size.QuadPart);
    path_ = path;

    Header header = {};
    if (!seek(0, error)) return false;
    DWORD got = 0;
    if (!ReadFile(handle_, &header, sizeof(header), &got, nullptr) || got != sizeof(header)) {
        error = "cannot read the ark header";
        return false;
    }

    if (header.magic != 0x00010000u) {
        error = "not a pc ark (bad magic)";
        return false;
    }
    if (header.strCount != 0 || header.strOffset != header.hdrLen) {
        error = "unexpected ark header";
        return false;
    }
    if (header.count == 0 || header.count > 1000000u) {
        error = "bad entry count";
        return false;
    }

    const std::uint64_t tableEnd = 24ull + 20ull * header.count;
    if (tableEnd > fileSize_) {
        error = "entry table exceeds the file size";
        return false;
    }

    entries_.resize(header.count);
    if (!seek(24, error)) return false;
    if (!ReadFile(handle_, entries_.data(), static_cast<DWORD>(20ull * header.count), &got, nullptr) ||
        got != 20ull * header.count) {
        error = "cannot read the entry table";
        return false;
    }

    for (const Entry& entry : entries_) {
        if (entry.ctype == 0) {
            ++rawCount_;
        } else if (entry.ctype == 1) {
            ++compressedCount_;
        } else {
            error = "unknown compression type in the entry table";
            return false;
        }
        if (static_cast<std::uint64_t>(entry.offset) + entry.len > fileSize_) {
            error = "an entry points outside the file";
            return false;
        }
    }

    sortedByHash_ = std::is_sorted(entries_.begin(), entries_.end(),
                                   [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
    return true;
}

int Archive::findEntry(std::uint32_t hash) const {
    if (sortedByHash_) {
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), hash,
                                         [](const Entry& e, std::uint32_t v) { return e.hash < v; });
        if (it != entries_.end() && it->hash == hash)
            return static_cast<int>(it - entries_.begin());
        return -1;
    }

    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].hash == hash) return static_cast<int>(i);
    return -1;
}

bool Archive::readEntry(std::size_t index, std::vector<std::uint8_t>& out, std::string& error) {
    if (index >= entries_.size()) {
        error = "entry index out of range";
        return false;
    }
    const Entry& entry = entries_[index];

    std::vector<std::uint8_t> stored(entry.len);
    if (!seek(entry.offset, error)) return false;

    DWORD got = 0;
    if (!ReadFile(handle_, stored.data(), entry.len, &got, nullptr) || got != entry.len) {
        error = "cannot read the entry";
        return false;
    }

    if (entry.ctype == 0) {
        out = std::move(stored);
        return true;
    }
    if (!lzssDecompress(stored, out)) {
        error = "lzss decompress failed";
        return false;
    }
    return true;
}

bool Archive::writeAt(std::uint64_t offset, const void* data, std::uint32_t size, std::string& error) {
    if (!seek(offset, error)) return false;

    DWORD put = 0;
    if (!WriteFile(handle_, data, size, &put, nullptr) || put != size) {
        error = "write failed";
        return false;
    }
    return true;
}

bool Archive::truncate(std::uint64_t size, std::string& error) {
    if (!seek(size, error)) return false;
    if (!SetEndOfFile(handle_)) {
        error = "cannot truncate the archive";
        return false;
    }
    fileSize_ = size;
    return true;
}

bool Archive::flush() {
    return FlushFileBuffers(handle_) != 0;
}

// ---------------------------------------------------------
// LOCATING THE ARCHIVE
// ---------------------------------------------------------
// Search is strictly downward: the tool sits next to the game's exe (or inside
// its folder), so only the start folder, its data\ subfolder, and one level of
// subfolders are checked. Walking up to parent folders would pick up some
// other game install on the same drive - that is never what the user wants.
bool findResidentArk(const std::string& start, std::string& found) {
    std::vector<std::string> candidates;

    candidates.push_back(start + "\\Resident.ark");
    candidates.push_back(start + "\\data\\Resident.ark");

    // The tool may sit one folder above the game's own directory.
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((start + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
                candidates.push_back(start + "\\" + fd.cFileName + "\\data\\Resident.ark");
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    for (const std::string& candidate : candidates) {
        if (fileExists(candidate)) {
            found = candidate;
            return true;
        }
    }
    return false;
}

bool looksLikeArk(const std::string& path, std::string& error) {
    Archive archive;
    if (!archive.open(path, error)) return false;
    return true;
}

// ---------------------------------------------------------
// ANALYSIS
// ---------------------------------------------------------
void analyze(Archive& archive, std::vector<Item>& items) {
    for (Item& item : items) {
        item.indices.clear();
        item.data.clear();
        item.report = Report{};
        item.enabled = true;

        std::string error;

        if (item.target == Target::Params) {
            const int total = static_cast<int>(sizeof(kParamsPaths) / sizeof(kParamsPaths[0]));
            std::string missing;
            std::string unreadable;
            int found = 0;
            int pending = 0;

            for (const char* path : kParamsPaths) {
                const int index = archive.findEntry(pathHash(path));
                if (index < 0) {
                    if (missing.empty()) missing = path;
                    continue;
                }
                ++found;

                std::vector<std::uint8_t> src;
                if (!archive.readEntry(static_cast<std::size_t>(index), src, error)) {
                    if (unreadable.empty()) unreadable = path;
                    continue;
                }

                std::vector<std::uint8_t> dst;
                switch (editParamsText(src, dst)) {
                case EditResult::Updated:
                    item.indices.push_back(static_cast<std::size_t>(index));
                    item.data.push_back(std::move(dst));
                    ++pending;
                    break;
                case EditResult::Unchanged:
                case EditResult::Skipped:
                    break;
                case EditResult::Failed:
                    if (unreadable.empty()) unreadable = path;
                    break;
                }
            }

            item.report.total = total;
            item.report.found = found;
            item.report.pending = pending;

            char text[256];
            if (found == 0) {
                item.report.state = State::Mismatch;
                item.report.detail = "no GameModeHUDs entries - not the retail/steam archive?";
            } else if (!missing.empty()) {
                item.report.state = State::Mismatch;
                std::snprintf(text, sizeof(text), "missing %s", missing.c_str());
                item.report.detail = text;
                const std::string hint = nearMiss(missing);
                if (!hint.empty()) item.report.detail += " - stored as " + hint + "?";
            } else if (!unreadable.empty()) {
                item.report.state = State::Mismatch;
                std::snprintf(text, sizeof(text), "cannot read %s", unreadable.c_str());
                item.report.detail = text;
            } else {
                std::snprintf(text, sizeof(text), "%d/%d parameter files verified%s", found, total,
                              pending ? "" : " - all correct");
                item.report.detail = text;
                if (pending) {
                    std::snprintf(text, sizeof(text), ", %d will change", pending);
                    item.report.detail += text;
                    item.report.state = State::Patchable;
                } else {
                    item.report.state = State::AlreadyPatched;
                }
            }
            continue;
        }

        // DialsInfo.gfx
        const std::vector<std::uint8_t>& gfx = embeddedGfx();
        item.report.total = 1;

        if (gfx.empty()) {
            item.report.state = State::Mismatch;
            item.report.detail = "the embedded movie is missing";
            continue;
        }

        const int index = archive.findEntry(pathHash(kGfxPath));
        if (index < 0) {
            item.report.state = State::Mismatch;
            std::string text = "missing ";
            text += kGfxPath;
            const std::string hint = nearMiss(kGfxPath);
            if (!hint.empty()) text += " - stored as " + hint + "?";
            item.report.detail = text;
            continue;
        }

        item.report.found = 1;

        std::vector<std::uint8_t> current;
        if (!archive.readEntry(static_cast<std::size_t>(index), current, error)) {
            item.report.state = State::Mismatch;
            item.report.detail = "cannot read " + std::string(kGfxPath);
            continue;
        }

        if (current == gfx) {
            item.report.state = State::AlreadyPatched;
            item.report.detail = "already correct";
        } else {
            item.report.state = State::Patchable;
            item.report.pending = 1;
            item.report.detail = "will be replaced";
            item.indices.push_back(static_cast<std::size_t>(index));
            item.data.push_back(gfx);
        }
    }
}

// ---------------------------------------------------------
// APPLYING
// ---------------------------------------------------------
bool apply(Archive& archive, std::vector<Item>& items, bool fullBackup,
           std::string& message, std::string& error) {
    struct Staged {
        std::size_t index;
        const std::vector<std::uint8_t>* data;
    };

    std::vector<Staged> staged;
    int changedParams = 0;
    int changedFiles = 0;

    for (const Item& item : items) {
        if (!item.mandatory && !item.enabled) continue;

        for (std::size_t k = 0; k < item.indices.size(); ++k) {
            for (const Staged& already : staged) {
                if (already.index == item.indices[k]) {
                    error = "internal: the same entry was staged twice";
                    return false;
                }
            }
            staged.push_back({ item.indices[k], &item.data[k] });
            if (item.target == Target::Params) ++changedParams; else ++changedFiles;
        }
    }

    if (staged.empty()) {
        message = "Nothing to do - everything is already correct.";
        return true;
    }

    // The original rows, so the archive can be put back without a full copy.
    std::vector<UndoRow> originalRows;
    for (const Staged& entry : staged)
        originalRows.push_back({ static_cast<std::uint32_t>(entry.index), archive.entryAt(entry.index) });

    if (!writeUndo(archive.path(), archive.fileSize(), originalRows, error))
        return false;

    if (fullBackup) {
        const std::string backup = archive.path() + ".speedo-bak";
        if (GetFileAttributesA(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileA(archive.path().c_str(), backup.c_str(), TRUE)) {
                error = "cannot write " + fileName(backup);
                return false;
            }
        }
    }

    // The data first: if anything goes wrong here the old table still points at
    // the old bytes, so the archive stays valid. The table rows are the last
    // thing to change.
    std::uint64_t at = archive.fileSize();
    std::vector<std::uint64_t> offsets(staged.size());

    for (std::size_t i = 0; i < staged.size(); ++i) {
        const std::vector<std::uint8_t>& data = *staged[i].data;
        if (data.size() > 0xFFFFFFFFull) {
            error = "an entry is too large for this format";
            return false;
        }
        if (!archive.writeAt(at, data.data(), static_cast<std::uint32_t>(data.size()), error))
            return false;

        offsets[i] = at;
        at += data.size();
    }

    for (std::size_t i = 0; i < staged.size(); ++i) {
        Entry row = archive.entryAt(staged[i].index);
        row.ctype = 0;  // stored plainly: the game accepts either per entry
        row.offset = static_cast<std::uint32_t>(offsets[i]);
        row.len = static_cast<std::uint32_t>(staged[i].data->size());
        row.rawLen = row.len;

        // Only the first sixteen bytes change - the hash stays where it is.
        if (!archive.writeAt(24 + 20ull * staged[i].index, &row, 16, error))
            return false;
    }

    archive.flush();

    char text[256];
    std::snprintf(text, sizeof(text), "Success: %d parameter file%s%s updated in %s",
                  changedParams, changedParams == 1 ? "" : "s",
                  changedFiles ? " and DialsInfo.gfx" : "", fileName(archive.path()).c_str());
    message = text;
    return true;
}

bool restoreFromUndo(const std::string& arkPath, std::string& message, std::string& error) {
    std::string path;
    undoPath(arkPath, path);

    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) {
        error = "no undo file next to the archive";
        return false;
    }

    char magic[4] = {};
    std::uint64_t originalSize = 0;
    std::uint32_t count = 0;
    std::vector<UndoRow> rows;

    if (fread(magic, 1, 4, fp) != 4 || std::memcmp(magic, "USD1", 4) != 0 ||
        fread(&originalSize, 1, 8, fp) != 8 || fread(&count, 1, 4, fp) != 4) {
        fclose(fp);
        error = "the undo file is not readable";
        return false;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        UndoRow row = {};
        if (fread(&row.index, 1, 4, fp) != 4 || fread(&row.original, 1, 16, fp) != 16) {
            fclose(fp);
            error = "the undo file is truncated";
            return false;
        }
        rows.push_back(row);
    }
    fclose(fp);

    Archive archive;
    if (!archive.open(arkPath, error)) return false;

    for (const UndoRow& row : rows) {
        if (!archive.writeAt(24 + 20ull * row.index, &row.original, 16, error)) return false;
    }
    archive.flush();
    if (originalSize <= archive.fileSize()) archive.truncate(originalSize, error);

    message = "Restored " + fileName(arkPath) + " from its undo file.";
    return true;
}

}  // namespace ark
