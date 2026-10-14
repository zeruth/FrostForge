// The client's WDB caches (WoW/Source/DB/DBClient/DBCache.h): what every cache instance shares.
#include "client/DBCache.hpp"
#include "client/Locale.hpp"
#include "os/Os.hpp"

// The build the cache files are written for; a file from another build is discarded.
// ref: FUN_14033faf0
uint32_t DBCacheGetBuild() {
    return 54261;
}

// The cache directory for the text locale, "Cache/WDB/<locale>", created if it is missing.
// The reference inlines SStrCopy and SStrPack here.
// ref: FUN_14033fb00
void DBCacheGetDirectory(char* path, uint64_t size) {
    const char* source = "Cache/WDB/";
    char* dest = path;
    uint64_t left = size;
    if (size != 0) {
        char c;
        do {
            if (--left == 0) {
                *dest = '\0';
                break;
            }
            c = *source++;
            *dest++ = c;
        } while (c != '\0');
    }
    const char* locale = LocaleGetName(LocaleGetText());
    dest = path;
    left = size;
    while (left != 0 && (--left, *dest != '\0')) {
        dest++;
    }
    uint64_t room = size - static_cast<uint64_t>(dest - path);
    if (room != 0) {
        for (; *locale != '\0'; locale++) {
            if (room != 1) {
                *dest++ = *locale;
                room--;
            }
        }
        *dest = '\0';
    }
    OsCreateDirectory(path, true, false);
}
