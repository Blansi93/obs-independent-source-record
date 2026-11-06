#pragma once

#include <QString>

#include <string>

// Looks up a localized string through OBS' module locale system.
// Falls back to the key itself when the locale has not been loaded yet.
QString Tr(const char *key);
std::string TrStd(const char *key);
