// CasExtract: a developer tool that opens an install's CASC storage the way the client does
// and writes the named files out, for reading the interface code the client runs.
//
//   CasExtract <install _classic_ dir> <out dir> <file name>...
//   CasExtract <install _classic_ dir> <out dir> @<list file>
//
// An entry #<fileDataId> loads by id and is written to <out>/<fileDataId>; in a list file,
// #<fileDataId> <name> writes it to <out>/<name> instead.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "base/Status.hpp"
#include "blz/String.hpp"
#include "cas/CAS.hpp"
#include "cas/CASManifest.hpp"
#include "client/Locale.hpp"

namespace {

// The client has no TactKey manager yet (FUN_141359ff0 is not ported), so no key getter is
// installed and a file with any encrypted block fails as a whole. The client's DB2 loader reads
// by section and skips the encrypted ones; here the file is read in spans instead, and a span
// that will not decrypt is written as zeros (narrowed to 0x40 bytes) so the rest survives.
bool LoadByIdZeroFilling(int32_t fileDataId, uint8_t** data, uint32_t* size, uint32_t* zeroed) {
    cas::CASFILE* file = nullptr;
    if (!cas::CASFileOpenById(fileDataId, &file, 0)) {
        return false;
    }
    uint32_t total = static_cast<uint32_t>(cas::CASFileGetSize(file));
    auto buffer = static_cast<uint8_t*>(std::calloc(total != 0 ? total : 1, 1));
    constexpr uint32_t SPAN = 0x1000;
    constexpr uint32_t FINE = 0x40;
    uint32_t bad = 0;
    for (uint32_t offset = 0; offset < total; offset += SPAN) {
        uint32_t length = total - offset < SPAN ? total - offset : SPAN;
        uint32_t read = 0;
        if (cas::CASFileReadAtOffset(file, buffer + offset, offset, length, &read, 0, -1) && read == length) {
            continue;
        }
        for (uint32_t fine = offset; fine < offset + length; fine += FINE) {
            uint32_t piece = offset + length - fine < FINE ? offset + length - fine : FINE;
            if (!cas::CASFileReadAtOffset(file, buffer + fine, fine, piece, &read, 0, -1) || read != piece) {
                std::memset(buffer + fine, 0, piece);
                bad += piece;
            }
        }
    }
    cas::CASFileClose(&file);
    if (bad == total && total != 0) {
        std::free(buffer);
        return false;
    }
    *data = buffer;
    *size = total;
    *zeroed = bad;
    return true;
}

}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: CasExtract <install dir> <out dir> <name>... | @<list>\n");
        return 2;
    }
    std::filesystem::current_path(argv[1]);
    std::vector<std::string> names;
    for (int i = 3; i < argc; i++) {
        if (argv[i][0] == '@') {
            std::ifstream list(argv[i] + 1);
            std::string line;
            while (std::getline(list, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    names.push_back(line);
                }
            }
        } else {
            names.emplace_back(argv[i]);
        }
    }

    uint32_t locale = LocaleFromString("enUS");
    cas::MANIFESTLOCALE manifestLocale = {};
    manifestLocale.m_region = RegionFromName("US");
    manifestLocale.m_text = LocaleToCasIndex(locale);
    manifestLocale.m_speech = LocaleToCasIndex(locale);
    std::string root = std::filesystem::current_path().string();
    CStatus status;
    blz::string product;
    if (!cas::CASInitFromBuildInfo(0x76c3, "3.4.3.54261", &manifestLocale, nullptr, root.c_str(), &status, &product, false)) {
        std::fprintf(stderr, "CAS initialization failed\n");
        return 1;
    }
    int failed = 0;
    for (const std::string& entry : names) {
        uint8_t* data = nullptr;
        uint32_t size = 0;
        std::string name = entry;
        bool loaded;
        bool zeroFilled = false;
        if (entry[0] == '#') {
            // #<fileDataId> [name]: by id, written to <out>/<name> or <out>/<fileDataId>
            size_t space = entry.find_first_of(" \t");
            std::string id = entry.substr(1, space == std::string::npos ? std::string::npos : space - 1);
            name = id;
            if (space != std::string::npos) {
                size_t start = entry.find_first_not_of(" \t", space);
                if (start != std::string::npos) {
                    name = entry.substr(start);
                }
            }
            char* end = nullptr;
            long fileDataId = std::strtol(id.c_str(), &end, 10);
            loaded = !id.empty() && *end == '\0'
                && cas::CASFileLoadById(static_cast<int32_t>(fileDataId), &data, &size, 0, 0);
            if (!loaded && !id.empty() && *end == '\0') {
                uint32_t zeroed = 0;
                loaded = LoadByIdZeroFilling(static_cast<int32_t>(fileDataId), &data, &size, &zeroed);
                zeroFilled = loaded;
                if (loaded && zeroed != 0) {
                    std::fprintf(stderr, "partial: %s (%u bytes would not decrypt, written as zeros)\n",
                        entry.c_str(), zeroed);
                }
            }
        } else {
            loaded = cas::CASFileLoadByName(name.c_str(), &data, &size, 0, 0);
        }
        if (!loaded) {
            char error[0x200];
            std::fprintf(stderr, "missing: %s (%s)\n", entry.c_str(), cas::CASFormatLastError(error, sizeof(error)));
            failed++;
            continue;
        }
        std::string path = name;
        for (char& c : path) {
            if (c == '\\') {
                c = '/';
            }
        }
        std::filesystem::path out = std::filesystem::path(argv[2]) / path;
        std::filesystem::create_directories(out.parent_path());
        std::ofstream file(out, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data), size);
        if (zeroFilled) {
            std::free(data);
        } else {
            cas::CASFileFree(data);
        }
        std::printf("%s (%u bytes)\n", name.c_str(), size);
    }
    return failed != 0 ? 1 : 0;
}
