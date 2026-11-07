#include "logger.hpp"

#include <obs-module.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <ctime>

namespace {
std::mutex g_logMutex;
std::string g_pluginDirectory;

std::string PluginDirectory()
{
	if (!g_pluginDirectory.empty())
		return g_pluginDirectory;

	char *path = obs_module_file("manifest.json");
	if (!path)
		return {};

	std::string fullPath = path;
	bfree(path);

	// fullPath is "<plugin>/data/manifest.json"; strip the file name and the
	// "data" directory to get the plugin's own folder.
	const size_t lastSlash = fullPath.find_last_of("/\\");
	if (lastSlash == std::string::npos)
		return {};
	std::string dataDir = fullPath.substr(0, lastSlash);

	const size_t parentSlash = dataDir.find_last_of("/\\");
	if (parentSlash == std::string::npos)
		return {};
	g_pluginDirectory = dataDir.substr(0, parentSlash);
	return g_pluginDirectory;
}
} // namespace

void LogMessage(const std::string &level, const std::string &message)
{
	const std::string directory = PluginDirectory();
	if (directory.empty())
		return;

	const std::string path = directory + "/record-source.log";

	std::lock_guard<std::mutex> lock(g_logMutex);
	FILE *file = std::fopen(path.c_str(), "a");
	if (!file)
		return;

	std::time_t now = std::time(nullptr);
	std::tm *local = std::localtime(&now);
	char timestamp[64] = {};
	if (local)
		std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", local);

	std::fprintf(file, "[%s] [%s] %s\n", timestamp, level.c_str(), message.c_str());
	std::fflush(file);
	std::fclose(file);
}
