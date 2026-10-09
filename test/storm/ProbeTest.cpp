// Exposes pure ported functions on the command line, one call per line of output, so
// tools/probe_oracle.py can compare them with the reference's own run under emulation.
//
//   ProbeTest hash <hex bytes>           SStrHashHT
//   ProbeTest normalize <path>           OsPathNormalize
//   ProbeTest utf8 <hex UTF-16 units>    UnicodeConvertUtf16ToUtf8 (result bytes as hex, return)
//   ProbeTest tokenize <string>          SStrTokenize (tokens, quoted flags)
//   ProbeTest float <hex bytes>          SStrToFloat (the float's bits)
//   ProbeTest fnv <hex bytes>            t2util::FnvHashStringI from the FNV-1a basis

#include "base/Status.hpp"
#include "cas/CAS.hpp"
#include "cas/CASLocalManifest.hpp"
#include "cas/CASManifest.hpp"
#include "blz/String.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "client/Locale.hpp"
#include "blz/Log.hpp"
#include "tact/Instance.hpp"
#include "tact/InstallationInfo.hpp"
#include "tact/ImmediateRead.hpp"
#include "tact/TaskScheduler.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include "util/Fnv.hpp"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        return 1;
    }
    std::string what = argv[1];
    const char* input = argv[2];

    // Hex bytes: argv arrives in the ANSI codepage, which cannot carry arbitrary UTF-8.
    std::string bytes;
    for (const char* p = input; p[0] && p[1]; p += 2) {
        char hex[3] = { p[0], p[1], 0 };
        bytes.push_back(static_cast<char>(std::strtoul(hex, nullptr, 16)));
    }

    if (what == "hash") {
        std::printf("%08x\n", SStrHashHT(bytes.c_str()));
    } else if (what == "float") {
        float value = SStrToFloat(bytes.c_str());
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        std::printf("%08x\n", bits);
    } else if (what == "locale") {
        // hex code, then the fallback as the third argument
        uint32_t code = static_cast<uint32_t>(std::strtoul(input, nullptr, 16));
        uint32_t fallback = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 0;
        std::printf("%u %u\n", LocaleFromCodeOrLanguage(code, fallback), LocaleToCode(code % 12));
    } else if (what == "region") {
        std::printf("%u\n", RegionFromName(bytes.c_str()));
    } else if (what == "ipv4") {
        uint32_t address = 0;
        bool ok = OsParseIPv4(bytes.c_str(), &address);
        std::printf("%d %08x\n", ok ? 1 : 0, ok ? address : 0);
    } else if (what == "ipv6") {
        uint8_t address[16] = {};
        bool ok = OsParseIPv6(bytes.c_str(), address);
        std::printf("%d ", ok ? 1 : 0);
        for (int32_t i = 0; i < 16; i++) {
            std::printf("%02x", ok ? address[i] : 0);
        }
        std::printf("\n");
    } else if (what == "tasks") {
        // N tasks over the TACT scheduler with 4 workers; each bumps a counter
        static volatile long s_done = 0;
        long count = std::strtol(input, nullptr, 10);
        {
            tact::TaskScheduler scheduler;
            if (!scheduler.Start(4, ~uint64_t(0), 4)) {
                std::printf("start failed\n");
                return 1;
            }
            for (long i = 0; i < count; i++) {
                scheduler.Post([](void*) { InterlockedIncrement(&s_done); }, nullptr, static_cast<int32_t>(i & 1));
            }
            while (s_done < count) {
                Sleep(1);
            }
            std::printf("ran %ld of %ld, pool allocated %d free slots %d\n", s_done, count,
                scheduler.m_pool.m_allocated, scheduler.m_pool.m_freeSlots);
        }
        std::printf("scheduler destroyed\n");
    } else if (what == "tactinit") {
        // tactinit <install dir> <storage dir>: a tact instance with the parameters CAS builds
        // (0x1402d1420), logging to stdout (unbuffered, so a crash at a stub keeps the log).
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        blz::g_logSinks[0] = [](const char* category, int32_t level, const char* file, int32_t line,
                                 const char* format, va_list args) {
            (void)file;
            std::printf("[%s %d:%d] ", category, level, line);
            std::vprintf(format, args);
            std::printf("\n");
        };
        tact::INSTALLATIONINFO* info = tact::InstallationInfoRead(input);
        tact::BUILDINFOENTRY* entry = info->m_active;
        if (!entry) {
            std::printf("no active build info\n");
            return 1;
        }
        tact::INSTANCEPARAMS params = {};
        params.m_08 = ~uint64_t(0);
        params.m_storagePath = argc > 3 ? argv[3] : "Data";
        std::memcpy(params.m_buildKey, entry->m_buildKey, 16);
        std::memcpy(params.m_cdnKey, entry->m_cdnKey, 16);
        params.m_cdnPath = entry->m_cdnPath;
        params.m_cdnHosts = entry->m_cdnServers;
        params.m_cdnHostCount = entry->m_cdnServerCount;
        params.m_armadilloKey = entry->m_armadillo;
        params.m_60 = 200;
        params.m_64 = 1;
        params.m_65 = 1;
        params.m_68 = 0x10;
        params.m_70 = 0x10;
        params.m_74 = 0x20;
        params.m_78 = 0x20000;
        params.m_affinity = ~uint64_t(0);
        params.m_priority = 4;
        params.m_threads = 1;
        params.m_a0 = ~uint64_t(0);
        params.m_a8 = 4;
        params.m_b0 = 0.75;
        params.m_c8 = 1;
        params.m_loadEncodingTable = 1;
        params.m_1d8 = 1;
        uint8_t error = 0xFF;
        tact::Instance* instance = tact::Instance::Create(&params, &error);
        std::printf("Create -> %p error %u (%s)\n", static_cast<void*>(instance), error, tact::ErrorString(error));
        if (instance) {
            tact::BUILDCONFIG& config = instance->m_buildConfig;
            std::printf("patch size %u, vfs %u, key layouts %u, build lines %u\n", config.m_patchSize,
                config.m_vfs.m_count, config.m_keyLayouts.m_count, config.m_buildLines.m_count);
            // What CAS does next (0x1402d2cd0): stat ".root" and read it by name.
            const char* name = argc > 4 ? argv[4] : ".root";
            tact::FILESTAT stat = {};
            uint8_t statError = tact::InstanceStatByName(instance, name, &stat, 1);
            std::printf("stat %s -> %u state %u csize %u esize %u espec %s\n", name, statError, stat.m_state,
                stat.m_key.m_csize, stat.m_key.m_esize, stat.m_key.m_eSpec ? stat.m_key.m_eSpec : "(none)");
            if (statError == 0 && stat.m_state == 1 && stat.m_key.m_csize != 0) {
                std::vector<uint8_t> data(stat.m_key.m_csize + 1);
                uint32_t read = 0;
                uint8_t readError =
                    tact::ImmediateReadByName(instance, name, data.data(), 0, stat.m_key.m_csize, &read, 0);
                std::printf("read -> %u (%s), %u bytes:", readError, tact::ErrorString(readError), read);
                for (uint32_t i = 0; i < 32 && i < read; i++) {
                    std::printf(" %02x", data[i]);
                }
                std::printf("\n");
                if (readError == 0 && std::strcmp(name, ".root") == 0) {
                    // The manifest load CAS runs next (0x1402d2c00): region US, enUS text and speech.
                    CStatus status;
                    cas::MANIFESTLOCALE locale = { 1, 1, 1, 1, {} };
                    auto manifest = static_cast<cas::CASMANIFEST*>(SMemAlloc(sizeof(cas::CASMANIFEST), 10, __FILE__, 0, 0, 0));
                    manifest->Construct();
                    const uint8_t* root = data.data();
                    bool loaded = manifest->Load(&root, read, &locale, &status);
                    std::printf("manifest -> %d, status %llu messages (max %d)\n", loaded ? 1 : 0,
                        static_cast<unsigned long long>(status.m_list.m_count), status.m_maxSeverity);
                    for (auto msg = status.m_list.m_next; msg != reinterpret_cast<CStatus::STATUSENTRY*>(&status.m_list);
                         msg = msg->m_next) {
                        std::printf("  [%d] %s\n", msg->m_severity, msg->m_text.m_data);
                    }
                    if (loaded) {
                        cas::BUILDMANIFEST* current = manifest->m_current;
                        std::printf("by id: capacity %llu count %llu; by name: capacity %llu count %llu\n",
                            static_cast<unsigned long long>(current->m_byId.m_capacity),
                            static_cast<unsigned long long>(current->m_byId.m_count),
                            static_cast<unsigned long long>(current->m_byName.m_capacity),
                            static_cast<unsigned long long>(current->m_byName.m_count));
                        cas::BuildManifestEntry* first = cas::g_manifestCursor.m_entry;
                        if (first != cas::g_manifestCursor.m_end) {
                            std::printf("first: fdid %u locale %08x content %08x name %016llx ckey ", first->m_fileDataId,
                                first->m_localeFlags, first->m_contentFlags,
                                static_cast<unsigned long long>(first->m_nameHash));
                            for (uint8_t b : first->m_ckey) {
                                std::printf("%02x", b);
                            }
                            std::printf("\n");
                        }
                    }
                }
            }
        }
        tact::InstallationInfoFree(info);
    } else if (what == "casinit") {
        // casinit <data dir> [local root]: CASInitFromBuildInfo as stage 3 calls it, but offline
        // (no CDN servers), region US with enUS text and speech.
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        blz::log_register([](const char* category, int32_t level, const char* file, int32_t line, const char* format,
                              va_list args) {
            (void)file;
            std::printf("[%s %d:%d] ", category, level, line);
            std::vprintf(format, args);
            std::printf("\n");
        });
        CStatus status;
        blz::string product;
        cas::MANIFESTLOCALE locale = { 1, 1, 1, 1, {} };
        cas::CASSetLogEnabled(true);
        const char* localRoot = argc > 3 ? argv[3] : "";
        bool ok = cas::CASInitFromBuildInfo(0x76c3, "3.4.3.54261", &locale, input, localRoot, &status, &product, true);
        std::printf("CASInitFromBuildInfo -> %d, product %s, status %llu messages\n", ok ? 1 : 0, product.m_data,
            static_cast<unsigned long long>(status.m_list.m_count));
        for (auto msg = status.m_list.m_next; msg != reinterpret_cast<CStatus::STATUSENTRY*>(&status.m_list);
             msg = msg->m_next) {
            std::printf("  [%d] %s\n", msg->m_severity, msg->m_text.m_data);
        }
        if (ok) {
            cas::BUILDMANIFEST* current = cas::g_casManifest->m_current;
            std::printf("manifest: %llu by id, %llu by name\n", static_cast<unsigned long long>(current->m_byId.m_count),
                static_cast<unsigned long long>(current->m_byName.m_count));
            cas::LOCALMANIFEST* local = cas::g_localManifest;
            std::printf("local root %s: %llu by name, %llu by id, next local id %d\n", local->m_root,
                static_cast<unsigned long long>(local->m_byName.m_count),
                static_cast<unsigned long long>(local->m_byId.m_count), local->m_nextLocalId);
            for (uint64_t i = 0; local->m_byName.m_buckets && i < local->m_byName.m_capacity; i++) {
                for (cas::LOCALNAMENODE* node = local->m_byName.m_buckets[i]; node; node = node->m_next) {
                    std::printf("  %016llx %d %s\n", static_cast<unsigned long long>(node->m_key),
                        node->m_value.m_fileDataId, node->m_value.m_path);
                }
            }
            for (int32_t i = 4; i < argc; i++) {
                int32_t fdid = cas::CASGetFileDataIdByName(argv[i]);
                std::printf("%s -> fdid %d\n", argv[i], fdid);
            }
        }
        cas::CASShutdown();
        std::printf("shutdown done\n");
    } else if (what == "buildinfo") {
        tact::INSTALLATIONINFO* info = tact::InstallationInfoRead(input);
        std::printf("build %p flavor %p active %p\n", static_cast<void*>(info->m_buildInfo),
            static_cast<void*>(info->m_flavorInfo), static_cast<void*>(info->m_active));
        if (info->m_flavorInfo && info->m_flavorInfo->m_entryCount) {
            std::printf("flavor %s\n", static_cast<tact::FLAVORINFOENTRY*>(info->m_flavorInfo->m_entries)[0].m_flavor);
        }
        if (tact::BUILDINFOENTRY* e = info->m_active) {
            std::printf("product %s branch %s active %u version %s\ncdnPath %s tags %s\nbuildKey ", e->m_product,
                e->m_branch, e->m_active, e->m_version, e->m_cdnPath, e->m_tags);
            for (uint8_t b : e->m_buildKey) {
                std::printf("%02x", b);
            }
            std::printf("\nhosts %u:", e->m_cdnHostCount);
            for (uint32_t i = 0; i < e->m_cdnHostCount; i++) {
                std::printf(" %s", e->m_cdnHosts[i]);
            }
            std::printf("\n");
        }
        tact::InstallationInfoFree(info);
    } else if (what == "cmp") {
        // two hex strings separated by '-'
        std::string second;
        const char* dash = std::strchr(input, '-');
        for (const char* p = dash ? dash + 1 : ""; p[0] && p[1]; p += 2) {
            char hex[3] = { p[0], p[1], 0 };
            second.push_back(static_cast<char>(std::strtoul(hex, nullptr, 16)));
        }
        std::string first = bytes.substr(0, dash ? static_cast<size_t>(dash - input) / 2 : bytes.size());
        std::printf("%d\n", SStrCmpUTF8I(first.c_str(), second.c_str(), 0x7FFFFFFF));
    } else if (what == "fnv") {
        std::printf("%016llx\n", static_cast<unsigned long long>(t2util::FnvHashStringI(bytes.c_str(), t2util::FNV64_BASIS)));
    } else if (what == "normalize") {
        char out[0x104];
        OsPathNormalize(out, sizeof(out), input);
        std::printf("%s\n", out);
    } else if (what == "utf8") {
        std::vector<uint16_t> units;
        for (const char* p = input; p[0] && p[1] && p[2] && p[3]; p += 4) {
            char hex[5] = { p[0], p[1], p[2], p[3], 0 };
            units.push_back(static_cast<uint16_t>(std::strtoul(hex, nullptr, 16)));
        }
        units.push_back(0);
        char out[64];
        std::memset(out, 0xCC, sizeof(out));
        int32_t written = -1;
        int32_t used = -1;
        int32_t result = UnicodeConvertUtf16ToUtf8(out, 16, units.data(), 0x7FFFFFFF, &written, &used);
        for (int32_t i = 0; i < 16; i++) {
            std::printf("%02x", static_cast<uint8_t>(out[i]));
        }
        std::printf(" %d %d %d\n", result, written, used);
    } else if (what == "tokenize") {
        const char* p = input;
        for (int32_t i = 0; i < 8; i++) {
            char token[32];
            int32_t quoted = -1;
            SStrTokenize(&p, token, sizeof(token), " ,;\"\t\n\r\x1A", &quoted);
            std::printf("[%s]%d ", token, quoted);
            if (*p == '\0') {
                break;
            }
        }
        std::printf("\n");
    }
    return 0;
}
