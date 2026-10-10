// DB2Check: a developer tool that opens an install's storage the way the client does, loads
// the tables of DBClient's load groups through the client's own DB2 loader and reports what
// failed, with the record counts of the sparse tables.
//
//   DB2Check <install _classic_ dir> [--keys <key list>] [group...]
//
// With no groups, all six load. Keys come from TactKey / TactKeyLookup as at startup, and
// --keys adds a TACTKeys-format list as hotfixed keys would (see CasExtract).

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
        uint8_t key[16];
        for (int i = 0; i < 16; i++) {
            char byte[3] = { keyText[i * 2], keyText[i * 2 + 1], '\0' };
            key[i] = static_cast<uint8_t>(std::strtoul(byte, nullptr, 16));
        }
        TactKeysProvide(std::strtoull(nameText, nullptr, 16), key);
        count++;
    }
    return count;
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: DB2Check <install dir> [--keys <list>] [group...]\n");
        return 2;
    }
    std::string keyFile;
    std::vector<int> groups;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            keyFile = std::filesystem::absolute(argv[++i]).string();
        } else {
            groups.push_back(std::atoi(argv[i]));
        }
    }
    if (groups.empty()) {
        for (int g = 0; g < DB2_LOAD_GROUP_COUNT; g++) {
            groups.push_back(g);
        }
    }
    std::filesystem::current_path(argv[1]);

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
    TactKeysSetup(nullptr);
    WowClientDB2LoadWithFlags(&g_tactKeyDB, 4);
    WowClientDB2LoadWithFlags(&g_tactKeyLookupDB, 4);
    if (!keyFile.empty() && ProvideKeys(keyFile.c_str()) < 0) {
        std::fprintf(stderr, "cannot open key file %s\n", keyFile.c_str());
        return 2;
    }

    int failed = 0;
    int loaded = 0;
    for (int g : groups) {
        if (g < 0 || g >= DB2_LOAD_GROUP_COUNT) {
            continue;
        }
        for (WowClientDB2_Base** table = g_db2LoadGroups[g].m_tables; *table != nullptr; table++) {
            if ((*table)->m_loaded) {
                loaded++;
                continue;
            }
            if (WowClientDB2LoadWithFlags(*table, 4) && (*table)->m_loaded) {
                loaded++;
            } else {
                std::printf("group %d: %s failed to load\n", g, (*table)->m_meta->m_name);
                failed++;
            }
        }
    }
    WowClientDB2_Base* sparse[] = { &g_spellDB, &g_itemSparseDB, &g_conversationLineDB, &g_sceneScriptTextDB };
    for (WowClientDB2_Base* table : sparse) {
        std::printf("%s: loaded %d, %u records, ids %d..%d\n", table->m_meta->m_name, table->m_loaded,
            table->m_recordCount, table->m_minId, table->m_maxId);
    }
    std::printf("%d loaded, %d failed\n", loaded, failed);
    return failed != 0 ? 1 : 0;
}
