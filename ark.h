// ---------------------------------------------------------
// ARK ACCESS
//
// The steam build keeps its ui and hud data inside data\Resident.ark. Editing
// an entry does not rebuild the archive: the new bytes are written at the END
// of the file and that entry's table row is pointed at them, so nothing else
// in the archive moves and a failure halfway through still leaves a valid
// file. The bytes the entry used to occupy simply become unused.
// ---------------------------------------------------------
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ark {

// ---------------------------------------------------------
// TARGETS
// ---------------------------------------------------------
// Two things can be pushed into the archive. The parameter files are what the
// speedometer reads its placement from, so they are not optional; the movie
// that carries the readout's own text field is.
enum class Target { Params, Gfx, Count };

enum class State {
    Patchable,       // every entry found, and at least one still needs the new bytes
    AlreadyPatched,  // every entry found, and all of them already carry the new bytes
    Mismatch,        // not the archive we know, or a required entry is missing
};

struct Report {
    State state = State::Mismatch;
    int found = 0;    // target entries located in the archive
    int total = 0;    // target entries the archive is expected to contain
    int pending = 0;  // entries that would actually be rewritten
    std::string detail;
};

struct Item {
    Target target = Target::Params;
    const char* label = "";
    bool mandatory = false;  // the ui keeps these ticked and locked
    bool enabled = true;     // user choice; ignored for mandatory items

    std::vector<std::size_t> indices;              // archive rows involved
    std::vector<std::vector<std::uint8_t>> data;   // replacement bytes
    Report report;
};

// One row of the archive's entry table.
struct Entry {
    std::uint32_t ctype = 0;    // 0 = stored, 1 = lzss
    std::uint32_t offset = 0;
    std::uint32_t len = 0;      // bytes in the file
    std::uint32_t rawLen = 0;   // bytes once decompressed
    std::uint32_t hash = 0;
};
static_assert(sizeof(Entry) == 20, "ark entry must be 20 bytes");

class Archive {
public:
    Archive() = default;
    ~Archive();

    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();

    const std::string& path() const { return path_; }
    std::uint64_t fileSize() const { return fileSize_; }
    std::size_t entryCount() const { return entries_.size(); }
    unsigned rawCount() const { return rawCount_; }
    unsigned compressedCount() const { return compressedCount_; }

    int findEntry(std::uint32_t hash) const;
    const Entry& entryAt(std::size_t index) const { return entries_[index]; }
    bool readEntry(std::size_t index, std::vector<std::uint8_t>& out, std::string& error);
    bool writeAt(std::uint64_t offset, const void* data, std::uint32_t size, std::string& error);
    bool truncate(std::uint64_t size, std::string& error);
    bool flush();

private:
    bool seek(std::uint64_t offset, std::string& error);

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::string path_;
    std::uint64_t fileSize_ = 0;
    std::vector<Entry> entries_;
    unsigned rawCount_ = 0;
    unsigned compressedCount_ = 0;
    bool sortedByHash_ = true;
};

// ---------------------------------------------------------
// PUBLIC API
// ---------------------------------------------------------
// Looks for data\Resident.ark inside `start`, its data\ subfolder, and one
// level of subfolders. It never walks up to parent folders: the tool must find
// the game it was installed next to, not some other copy on the same drive.
bool findResidentArk(const std::string& start, std::string& found);

// Fills `items` (one per Target, labels already set) from what the archive
// currently holds. Nothing is written.
void analyze(Archive& archive, std::vector<Item>& items);

// Writes every enabled item. `fullBackup` copies the whole archive to
// <path>.speedo-bak first; the small <path>.speedo-undo file is always
// written, so a full copy is never required to be able to go back.
bool apply(Archive& archive, std::vector<Item>& items, bool fullBackup,
           std::string& message, std::string& error);

// Restores an archive from its .speedo-undo file.
bool restoreFromUndo(const std::string& arkPath, std::string& message, std::string& error);

// True when the file has an ark table we could work with (used by the browser
// so an obviously wrong pick is rejected before anything else happens).
bool looksLikeArk(const std::string& path, std::string& error);

}  // namespace ark
