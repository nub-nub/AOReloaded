#pragma once

namespace aor {

void LogInit();
void LogShutdown();
void Log(const char* fmt, ...);

}  // namespace aor
