#ifndef UTIL_UNIMPLEMENTED_HPP
#define UTIL_UNIMPLEMENTED_HPP

// Marks the body of a reference function that is identified (its `// ref:` tag is in place)
// but not ported yet. The first time each one is reached it is logged with its name, so a run
// shows exactly which unported function the client got to. It never aborts: the stub's
// return value, chosen at the call site, decides what the caller does next.
void FrostUnimplemented(const char* function);

#define FROST_UNIMPLEMENTED() FrostUnimplemented(__FUNCTION__)

#endif
