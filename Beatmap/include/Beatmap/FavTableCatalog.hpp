#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace usc_fav {

// Feed the existing USC chart database into this structure: one record per
// DIFFICULTY / CHART, NOT one record per song.
struct ChartRecord {
    std::uint64_t id = 0;
    std::filesystem::path chartFile; // absolute or root-relative .ksh/.kson
};

struct Entry {
    std::string requestedPath; // normalized, UTF-8 relative to song root
    std::uint64_t chartId = 0;
    std::filesystem::path resolvedFile;
    bool matched = false;
    bool ambiguous = false;
};

struct Table {
    std::string name;           // e.g. os10, os10_unique
    std::filesystem::path sourceFile;
    std::vector<Entry> entries; // retains input order
    std::unordered_set<std::uint64_t> chartIds;
    std::size_t missing = 0;
    std::size_t ambiguous = 0;
    std::size_t suffixMatched = 0; // chart was under an extra directory

    bool ContainsChart(std::uint64_t chartId) const {
        return chartIds.find(chartId) != chartIds.end();
    }
};

// Song/difficulty projection for the existing song wheel. Only listed
// difficulty IDs are allowed: a song with one listed chart will have one entry.
struct SongRecord {
    std::uint64_t songId = 0;
    std::vector<std::uint64_t> difficultyChartIds;
};
struct SongView {
    std::uint64_t songId = 0;
    std::vector<std::uint64_t> visibleDifficultyChartIds;
};

class Catalog {
public:
    // Refresh tables after USC's chart DB scan or on a user-requested rescan.
    // No database writes, no song copies, no calls into render/gameplay code.
    // songRoots are the folders from USC's song search-path settings.
    void Refresh(const std::vector<std::filesystem::path>& favDirectories,
                 const std::vector<std::filesystem::path>& songRoots,
                 const std::vector<ChartRecord>& charts);

    const std::vector<Table>& Tables() const noexcept { return tables_; }
    const Table* FindTable(const std::string& name) const noexcept;
    const std::vector<std::string>& Diagnostics() const noexcept { return diagnostics_; }
    bool HasChart(const std::string& tableName, std::uint64_t chartId) const noexcept;
    std::vector<SongView> MakeSongView(const std::string& tableName,
                                       const std::vector<SongRecord>& songs) const;

private:
    std::vector<Table> tables_;
    std::vector<std::string> diagnostics_;
};

} // namespace usc_fav
