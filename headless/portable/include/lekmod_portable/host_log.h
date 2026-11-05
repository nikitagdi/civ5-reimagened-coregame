// Logging from the portable layer to the embedding host (gc_host.log once gc_create ran).
#ifndef LEKMOD_PORTABLE_HOST_LOG_H
#define LEKMOD_PORTABLE_HOST_LOG_H

namespace lekmod_portable {

enum LogLevel
{
	LOG_ERROR = 0,
	LOG_WARN = 1,
	LOG_INFO = 2,
	LOG_DEBUG = 3,
	LOG_TRACE = 4
};

typedef void (*LogSink)(void* user, int level, const char* msg);

// Without a sink, messages at LOG_WARN and above go to stderr.
void SetLogSink(LogSink sink, void* user);
void Log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// Logs "stub: <what>" once per distinct call site name.
void LogStubOnce(const char* what);

// A value-initialized T (zero, NULL, empty string): the default result of unimplemented engine calls.
template <class T>
inline T Zero()
{
	return T();
}

}  // namespace lekmod_portable

#endif
