// Runs every case in printf_cases.txt through SStrPrintf and prints one result per line, for
// tools/printf_oracle.py to compare against the reference's own SStrPrintf run under emulation.
//
//   PrintfTest.exe <printf_cases.txt>

#include "storm/String.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct Argument {
    char type;
    std::string text;
};

// Calls SStrPrintf with up to 4 arguments of mixed types: each slot is passed as the type
// the case declares, so the variadic call sees exactly what a C caller would pass.
uint64_t AsInteger(const Argument& a) {
    switch (a.type) {
    case 'i':
        return static_cast<uint64_t>(static_cast<int64_t>(std::strtol(a.text.c_str(), nullptr, 10)));
    case 'I':
        return static_cast<uint64_t>(std::strtoll(a.text.c_str(), nullptr, 10));
    case 'u':
    case 'c':
        return std::strtoull(a.text.c_str(), nullptr, 10);
    case 'n':
        return 0;
    case 's':
        return reinterpret_cast<uint64_t>(a.text.c_str());
    default:
        return 0;
    }
}

double AsDouble(const Argument& a) {
    if (a.text == "inf") return std::numeric_limits<double>::infinity();
    if (a.text == "-inf") return -std::numeric_limits<double>::infinity();
    if (a.text == "nan") return std::strtod("nan", nullptr);
    return std::strtod(a.text.c_str(), nullptr);
}

// MSVC passes variadic doubles in integer slots by value, so passing a uint64 bit pattern or a
// double through the same position is not interchangeable; build the call per type signature.
template <class T>
T Get(const Argument& a);
template <>
uint64_t Get<uint64_t>(const Argument& a) { return AsInteger(a); }
template <>
double Get<double>(const Argument& a) { return AsDouble(a); }

void Run(char* out, uint64_t size, const char* format, const std::vector<Argument>& args) {
    auto isDouble = [&](size_t i) { return i < args.size() && args[i].type == 'd'; };
    switch (args.size()) {
    case 0:
        SStrPrintf(out, size, format);
        return;
    case 1:
        if (isDouble(0)) SStrPrintf(out, size, format, Get<double>(args[0]));
        else SStrPrintf(out, size, format, Get<uint64_t>(args[0]));
        return;
    case 2:
        if (isDouble(0) && !isDouble(1)) SStrPrintf(out, size, format, Get<double>(args[0]), Get<uint64_t>(args[1]));
        else if (!isDouble(0) && isDouble(1)) SStrPrintf(out, size, format, Get<uint64_t>(args[0]), Get<double>(args[1]));
        else if (isDouble(0)) SStrPrintf(out, size, format, Get<double>(args[0]), Get<double>(args[1]));
        else SStrPrintf(out, size, format, Get<uint64_t>(args[0]), Get<uint64_t>(args[1]));
        return;
    case 3:
        if (isDouble(0)) SStrPrintf(out, size, format, Get<double>(args[0]), Get<uint64_t>(args[1]), Get<uint64_t>(args[2]));
        else SStrPrintf(out, size, format, Get<uint64_t>(args[0]), Get<uint64_t>(args[1]), Get<uint64_t>(args[2]));
        return;
    default:
        std::snprintf(out, size, "<unsupported argument count>");
    }
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: PrintfTest <printf_cases.txt>\n");
        return 1;
    }
    std::ifstream in(argv[1]);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> fields;
        size_t start = 0;
        for (;;) {
            size_t tab = line.find('\t', start);
            fields.push_back(line.substr(start, tab - start));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        std::vector<Argument> args;
        for (size_t i = 1; i < fields.size(); i++) {
            args.push_back({ fields[i][0], fields[i].substr(2) });
        }
        char out[512];
        Run(out, sizeof(out), fields[0].c_str(), args);
        std::printf("%s\n", out);
    }
    return 0;
}
