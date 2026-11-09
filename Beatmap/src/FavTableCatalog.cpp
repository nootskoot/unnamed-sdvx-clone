#include "FavTableCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <iconv.h>
#endif

namespace usc_fav {
namespace {
namespace fs = std::filesystem;

std::string LowerAscii(std::string str) {
    for (char& c : str) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z') c = char(uc + 32);
    }
    return str;
}

std::string Utf8Path(const fs::path& path) { return path.u8string(); }

// Validate actual UTF-8 rather than assuming that high-bit bytes are UTF-8.
bool IsUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        const int continuation = (c >= 0xC2 && c <= 0xDF) ? 1 :
                                 (c >= 0xE0 && c <= 0xEF) ? 2 :
                                 (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
        if (continuation < 0 || i + std::size_t(continuation) >= s.size()) return false;
        const unsigned char first = static_cast<unsigned char>(s[i+1]);
        if ((first & 0xC0) != 0x80 || (c == 0xE0 && first < 0xA0) ||
            (c == 0xED && first >= 0xA0) || (c == 0xF0 && first < 0x90) ||
            (c == 0xF4 && first >= 0x90)) return false;
        for (int j = 2; j <= continuation; ++j)
            if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) return false;
        i += std::size_t(continuation) + 1;
    }
    return true;
}

std::string DecodeCp932(const std::string& input) {
#if defined(_WIN32)
    if (input.empty()) return {};
    const int count = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("invalid CP932 data");
    std::wstring wide(std::size_t(count), L'\0');
    if (!MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), &wide[0], count))
        throw std::runtime_error("CP932 decode failed");
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), count,
        nullptr, 0, nullptr, nullptr);
    if (n <= 0) throw std::runtime_error("UTF-8 conversion failed");
    std::string output(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), count,
        &output[0], n, nullptr, nullptr);
    return output;
#else
    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == reinterpret_cast<iconv_t>(-1))
        cd = iconv_open("UTF-8", "SHIFT_JIS");
    if (cd == reinterpret_cast<iconv_t>(-1))
        throw std::runtime_error("no CP932/Shift-JIS decoder available");
    std::string output(input.size() * 4 + 4, '\0');
    char* inPtr = const_cast<char*>(input.data());
    std::size_t inBytes = input.size();
    char* outPtr = &output[0];
    std::size_t outBytes = output.size();
    const std::size_t result = iconv(cd, &inPtr, &inBytes, &outPtr, &outBytes);
    iconv_close(cd);
    if (result == std::size_t(-1) || inBytes != 0)
        throw std::runtime_error("invalid CP932/Shift-JIS data");
    output.resize(output.size() - outBytes);
    return output;
#endif
}

std::string DecodeText(const std::string& bytes) {
    std::string data = bytes;
    if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
    if (IsUtf8(data)) return data;
    return DecodeCp932(data);
}

// Normalize only separators and harmless '.' components. DO NOT make a fuzzy
// title/basename match: that would silently admit the wrong difficulty.
std::string NormalizeRelative(const std::string& raw) {
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.empty() || s.front() == '/' || s.find('\0') != std::string::npos)
        return {};
    std::vector<std::string> components;
    std::string part;
    auto flush = [&]() -> bool {
        if (part == "..") return false;
        if (!part.empty() && part != ".") components.push_back(part);
        part.clear();
        return true;
    };
    for (char c : s) {
        if (c == '/') {
            if (!flush()) return {};
        } else part += c;
    }
    if (!flush() || components.empty()) return {};
    if (components.front().find(':') != std::string::npos) return {}; // drive letter
    std::string normalized;
    for (const auto& p : components) {
        if (!normalized.empty()) normalized += '/';
        normalized += p;
    }
    return normalized;
}

std::string ReadTextFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open .fav file");
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() > 4 * 1024 * 1024) throw std::runtime_error(".fav file exceeds 4 MiB");
    return DecodeText(raw);
}

std::string TableName(const fs::path& file) {
    std::string name = file.stem().u8string();
    // Original sample archive prepends '(new341)' to 'os10.fav'.
    if (name.compare(0, 4, "(new") == 0) {
        std::size_t pos = 4;
        while (pos < name.size() && name[pos] >= '0' && name[pos] <= '9') ++pos;
        if (pos > 4 && pos < name.size() && name[pos] == ')') name.erase(0, pos + 1);
    }
    return name;
}

std::string RelativeKey(const fs::path& file, const fs::path& root) {
    // USC databases normally use absolute chart paths, but a caller can also
    // supply song-root-relative paths. Never infer matches by song title.
    if (file.is_relative()) {
        const auto normalized = NormalizeRelative(Utf8Path(file));
        if (!normalized.empty()) return normalized;
    }
    std::error_code ec;
    const auto absoluteFile = fs::absolute(file, ec);
    if (ec) return {};
    const auto absoluteRoot = fs::absolute(root, ec);
    if (ec) return {};
    const auto rel = absoluteFile.lexically_normal().lexically_relative(absoluteRoot.lexically_normal());
    if (rel.empty() || rel.is_absolute()) return {};
    return NormalizeRelative(Utf8Path(rel));
}

std::vector<fs::path> FindFavFiles(const std::vector<fs::path>& dirs) {
    std::vector<fs::path> files;
    std::unordered_set<std::string> seen;
    // Preserve directory priority: bin/fav-tables comes before the song roots.
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> fromDir;
        fs::recursive_directory_iterator it(dir,
            fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            std::error_code entryError;
            if (it->is_regular_file(entryError) &&
                LowerAscii(it->path().extension().u8string()) == ".fav") {
                const auto filename = it->path().lexically_normal();
                if (seen.insert(Utf8Path(filename)).second)
                    fromDir.push_back(filename);
            }
            it.increment(ec);
        }
        std::sort(fromDir.begin(), fromDir.end());
        files.insert(files.end(), fromDir.begin(), fromDir.end());
    }
    return files;
}
} // namespace

void Catalog::Refresh(const std::vector<fs::path>& favDirectories,
                      const std::vector<fs::path>& songRoots,
                      const std::vector<ChartRecord>& charts) {
    std::unordered_map<std::string, std::vector<std::size_t>> index;
    std::vector<std::string> diagnostics;
    std::vector<Table> tables;
    // The chart database contains absolute paths, but imported .fav lists are
    // relative to the original KSM song directory. A user may nest entire
    // packages under extra organizational folders; strict root-relative
    // matching alone therefore misses otherwise identical chart files.
    std::vector<std::string> fullChartPaths;
    fullChartPaths.reserve(charts.size());
    for (std::size_t i = 0; i < charts.size(); ++i) {
        for (const auto& root : songRoots) {
            const auto key = RelativeKey(charts[i].chartFile, root);
            if (!key.empty()) index[key].push_back(i);
        }
        std::string full = charts[i].chartFile.lexically_normal().generic_u8string();
        std::replace(full.begin(), full.end(), '\\', '/');
        fullChartPaths.push_back(std::move(full));
    }

    for (const auto& file : FindFavFiles(favDirectories)) {
        Table table;
        table.name = TableName(file);
        table.sourceFile = file;
        try {
            const std::string contents = ReadTextFile(file);
            std::size_t lineNumber = 0;
            std::size_t offset = 0;
            std::unordered_set<std::string> inputDedup;
            while (offset <= contents.size()) {
                const std::size_t end = contents.find('\n', offset);
                std::string line = contents.substr(offset, end == std::string::npos ?
                    std::string::npos : end - offset);
                ++lineNumber;
                offset = end == std::string::npos ? contents.size() + 1 : end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const std::string path = NormalizeRelative(line);
                if (path.empty()) {
                    diagnostics.push_back(Utf8Path(file) + ":" + std::to_string(lineNumber) +
                        ": rejected unsafe/empty chart path");
                    continue;
                }
                if (!inputDedup.insert(path).second) continue;
                Entry row;
                row.requestedPath = path;
                auto exact = index.find(path);
                std::vector<std::size_t> candidates;
                bool matchedViaSuffix = false;
                if (exact != index.end()) {
                    candidates = exact->second;  // Prefer a correctly rooted match.
                } else if (path.find('/') != std::string::npos) {
                    // Only match the *whole* path in the .fav file, including
                    // parent directories and exact difficulty filename. Never
                    // fall back to a bare filename/title match.
                    for (std::size_t i = 0; i < fullChartPaths.size(); ++i) {
                        const std::string& full = fullChartPaths[i];
                        if (full.size() < path.size()) continue;
                        const std::size_t start = full.size() - path.size();
                        if (full.compare(start, path.size(), path) == 0 &&
                            (start == 0 || full[start - 1] == '/'))
                            candidates.push_back(i);
                    }
                    matchedViaSuffix = !candidates.empty();
                }
                if (candidates.empty()) {
                    ++table.missing;
                    diagnostics.push_back(table.name + ": missing " + path);
                } else {
                    std::unordered_set<std::uint64_t> distinctIds;
                    for (auto i : candidates)
                        distinctIds.insert(charts[i].id);
                    if (distinctIds.size() != 1) {
                        row.ambiguous = true;
                        ++table.ambiguous;
                        diagnostics.push_back(table.name + ": ambiguous " + path);
                    } else {
                        const auto& chart = charts[candidates.front()];
                        row.matched = true;
                        row.chartId = chart.id;
                        row.resolvedFile = chart.chartFile;
                        table.chartIds.insert(chart.id);
                        if (matchedViaSuffix) ++table.suffixMatched;
                    }
                }
                table.entries.push_back(std::move(row));
            }
        } catch (const std::exception& e) {
            diagnostics.push_back(Utf8Path(file) + ": " + e.what());
            continue; // malformed files do not delete or alter ordinary collections
        }
        if (table.name.empty()) {
            diagnostics.push_back(Utf8Path(file) + ": invalid table name");
            continue;
        }
        auto isTaken = [&](const std::string& name) {
            return std::any_of(tables.begin(), tables.end(), [&](const Table& t) {
                return t.name == name;
            });
        };
        if (isTaken(table.name)) {
            // Two packs can both contain favorites.fav. Preserve the first
            // table's stable name, while giving the later one an identifiable
            // and distinct table URL instead of silently discarding it.
            const std::string base = table.name;
            std::string parent = file.parent_path().filename().u8string();
            if (parent.empty()) parent = "folder";
            table.name = base + " [" + parent + "]";
            if (isTaken(table.name)) {
                const auto stableSuffix = std::hash<std::string>{}(Utf8Path(file));
                table.name += " #" + std::to_string(stableSuffix);
            }
            diagnostics.push_back(Utf8Path(file) + ": duplicate stem " + base +
                ", imported as " + table.name);
        }
        tables.push_back(std::move(table));
    }
    // Commit all at once, avoiding partially updated tables on a hard failure.
    tables_.swap(tables);
    diagnostics_.swap(diagnostics);
}

const Table* Catalog::FindTable(const std::string& name) const noexcept {
    auto it = std::find_if(tables_.begin(), tables_.end(), [&](const Table& table) {
        return table.name == name;
    });
    return it == tables_.end() ? nullptr : &*it;
}

std::vector<SongView> Catalog::MakeSongView(
    const std::string& tableName, const std::vector<SongRecord>& songs) const {
    std::vector<SongView> result;
    const auto* table = FindTable(tableName);
    if (!table) return result;
    for (const auto& song : songs) {
        SongView view;
        view.songId = song.songId;
        for (const auto chartId : song.difficultyChartIds)
            if (table->ContainsChart(chartId)) view.visibleDifficultyChartIds.push_back(chartId);
        if (!view.visibleDifficultyChartIds.empty()) result.push_back(std::move(view));
    }
    return result;
}

bool Catalog::HasChart(const std::string& tableName, std::uint64_t chartId) const noexcept {
    const auto* table = FindTable(tableName);
    return table && table->ContainsChart(chartId);
}
} // namespace usc_fav
