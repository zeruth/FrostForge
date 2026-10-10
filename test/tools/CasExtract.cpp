// CasExtract: a developer tool that opens an install's CASC storage the way the client does
// and writes the named files out, for reading the interface code the client runs.
//
//   CasExtract <install _classic_ dir> <out dir> <file name>...
//   CasExtract <install _classic_ dir> <out dir> @<list file>

#include <cstdio>
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
    for (const std::string& name : names) {
        uint8_t* data = nullptr;
        uint32_t size = 0;
        if (!cas::CASFileLoadByName(name.c_str(), &data, &size, 0, 0)) {
            std::fprintf(stderr, "missing: %s\n", name.c_str());
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
