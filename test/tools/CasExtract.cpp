// CasExtract: a developer tool that opens an install's CASC storage the way the client does
// and writes the named files out, for reading the interface code the client runs.
//
//   CasExtract <install _classic_ dir> <out dir> <file name>...
//   CasExtract <install _classic_ dir> <out dir> @<list file>
//   ... [--keys <key list>]...
//
// An entry #<fileDataId> loads by id and is written to <out>/<fileDataId>; in a list file,
// #<fileDataId> <name> writes it to <out>/<name> instead.
//
// Encrypted blocks are decrypted with the keys the client knows: TactKey and TactKeyLookup are
// loaded and the client's key store (TactKeysSetup) answers CASC's key callback. The install
// ships only some keys; the rest reach the client as TactKey hotfixes from the server. Pass
// them with --keys (a TACTKeys-format list) and they are stored as such a hotfix would be. A
// file that still needs an unknown key fails, with the CAS error.

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
#include "db/DB2Tables.hpp"
#include "db/DBClient.hpp"
#include "db/TactKeys.hpp"
#include "db/WowClientDB2.hpp"

namespace {

// A key list in the TACTKeys format: "<key name, 16 hex digits> <key, 32 hex digits>", anything
// after that ignored, lines that do not parse skipped. Returns the keys read, or -1 when the
// file will not open.
int32_t ProvideKeys(const char* path) {
    std::ifstream list(path);
    if (!list) {
        return -1;
    }
    int32_t count = 0;
    std::string line;
    while (std::getline(list, line)) {
        char nameText[17] = {};
        char keyText[33] = {};
        if (std::sscanf(line.c_str(), "%16[0-9A-Fa-f] %32[0-9A-Fa-f]", nameText, keyText) != 2
            || std::strlen(nameText) != 16 || std::strlen(keyText) != 32) {
            continue;
        }
        uint64_t name = std::strtoull(nameText, nullptr, 16);
        uint8_t key[16];
        for (int i = 0; i < 16; i++) {
            char byte[3] = { keyText[i * 2], keyText[i * 2 + 1], '\0' };
            key[i] = static_cast<uint8_t>(std::strtoul(byte, nullptr, 16));
        }
        TactKeysProvide(name, key);
        count++;
    }
    return count;
}

}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: CasExtract <install dir> <out dir> <name>... | @<list>\n");
        return 2;
    }
    std::vector<std::string> keyFiles;
    for (int i = 3; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--keys") == 0) {
            keyFiles.push_back(std::filesystem::absolute(argv[i + 1]).string());
        }
    }
    std::filesystem::current_path(argv[1]);
    std::vector<std::string> names;
    for (int i = 3; i < argc; i++) {
        if (std::strcmp(argv[i], "--keys") == 0) {
            i++;
        } else if (argv[i][0] == '@') {
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

    // The keys, as the client has them at startup.
    TactKeysSetup(nullptr);
    if (!WowClientDB2LoadWithFlags(&g_tactKeyDB, 4) || !WowClientDB2LoadWithFlags(&g_tactKeyLookupDB, 4)) {
        std::fprintf(stderr, "TactKey / TactKeyLookup failed to load; encrypted blocks will fail\n");
    }
    for (const std::string& keyFile : keyFiles) {
        int32_t count = ProvideKeys(keyFile.c_str());
        if (count < 0) {
            std::fprintf(stderr, "cannot open key file %s\n", keyFile.c_str());
            return 2;
        }
        std::printf("%d keys from %s\n", count, keyFile.c_str());
    }

    int failed = 0;
    for (const std::string& entry : names) {
        uint8_t* data = nullptr;
        uint32_t size = 0;
        std::string name = entry;
        bool loaded = false;
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
        cas::CASFileFree(data);
        std::printf("%s (%u bytes)\n", name.c_str(), size);
    }
    return failed != 0 ? 1 : 0;
}
