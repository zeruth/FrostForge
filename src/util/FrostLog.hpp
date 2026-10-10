#ifndef UTIL_FROSTLOG_HPP
#define UTIL_FROSTLOG_HPP

// FROST: not a reference facility. One log, Logs\FrostLite.log (truncated each run), that
// collects every problem the port runs into -- Lua errors with their stacks, unported
// functions reached with their callers, script status warnings -- so a run can be read in one
// place instead of grepping several logs and stdout. Each line is flushed as it is written, so
// the file is complete even when the client is killed or crashes.
void FrostLog(const char* category, const char* format, ...);

// Appends the native call stack above the caller, symbolized from the PDB, one indented line
// per frame (`skip` frames above the caller are left out, `count` are written).
void FrostLogStack(int skip, int count);

#endif
