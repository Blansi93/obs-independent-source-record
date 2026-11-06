#include "localization.hpp"

#include <obs-module.h>

QString Tr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

std::string TrStd(const char *key)
{
	return std::string(obs_module_text(key));
}
