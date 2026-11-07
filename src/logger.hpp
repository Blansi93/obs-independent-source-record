#pragma once

#include <string>

// Appends timestamped messages to record-source.log, located next to the
// plugin DLL. Safe to call from any thread.
void LogMessage(const std::string &level, const std::string &message);
inline void LogInfo(const std::string &message) { LogMessage("INFO", message); }
inline void LogWarning(const std::string &message) { LogMessage("WARN", message); }
inline void LogError(const std::string &message) { LogMessage("ERROR", message); }
