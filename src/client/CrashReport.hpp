#ifndef CLIENT_CRASH_REPORT_HPP
#define CLIENT_CRASH_REPORT_HPP

#include <cstdint>

void CrashReportAddFile(const char* path);

void CrashReportRecordCommandLine(char** argv, int32_t a2, int32_t a3, int32_t a4);

void CrashReportConfigure();

#endif
