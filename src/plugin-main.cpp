#include "localization.hpp"
#include "logger.hpp"
#include "record-source-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QByteArray>
#include <QPointer>
#include <QWidget>

OBS_DECLARE_MODULE()

namespace {

constexpr const char *DOCK_ID = "record-source-dock";

lookup_t *g_locale = nullptr;

QPointer<RecordSourceDock> g_dock;

bool IsSpanishLocale(const char *locale)
{
	if (!locale)
		return false;

	// OBS ships "es-ES" and the frontend only offers that Spanish variant,
	// but accept any "es" / "es-*" locale in case a custom one is configured.
	return (locale[0] == 'e' || locale[0] == 'E') &&
	       (locale[1] == 's' || locale[1] == 'S') &&
	       (locale[2] == '\0' || locale[2] == '-' || locale[2] == '_');
}

void FrontendEvent(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (g_dock)
		g_dock->HandleFrontendEvent(event);
}

} // namespace

// libobs looks these symbols up with os_dlsym when the module is loaded.
const char *obs_module_text(const char *key)
{
	const char *text = key;
	text_lookup_getstr(g_locale, key, &text);
	return text;
}

bool obs_module_get_string(const char *key, const char **text)
{
	return text_lookup_getstr(g_locale, key, text);
}

void obs_module_set_locale(const char *locale)
{
	if (g_locale) {
		text_lookup_destroy(g_locale);
		g_locale = nullptr;
	}

	// en-US is always loaded as the base text; es-ES overlays it when OBS is
	// running in Spanish. Any other language falls back to English.
	const char *target = IsSpanishLocale(locale) ? "es-ES" : "en-US";
	g_locale = obs_module_load_locale(obs_current_module(), "en-US", target);
}

void obs_module_free_locale(void)
{
	if (g_locale) {
		text_lookup_destroy(g_locale);
		g_locale = nullptr;
	}
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Record Source] loaded (version 0.2.1), OBS %s",
	     obs_get_version_string());
	LogInfo("Plugin loaded (version 0.2.1), OBS " + std::string(obs_get_version_string()));
	return true;
}

void obs_module_post_load(void)
{
	if (g_dock)
		return;

	const QByteArray dockTitle = Tr("Dock.Title").toUtf8();

	auto *dock = new RecordSourceDock(nullptr);
	if (!obs_frontend_add_dock_by_id(DOCK_ID, dockTitle.constData(), dock)) {
		blog(LOG_ERROR, "[Record Source] could not add the control dock");
		delete dock;
		return;
	}

	g_dock = dock;
	obs_frontend_add_event_callback(FrontendEvent, nullptr);

	blog(LOG_INFO, "[Record Source] control dock added");
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(FrontendEvent, nullptr);

	if (g_dock) {
		g_dock->Shutdown();
		obs_frontend_remove_dock(DOCK_ID);
		g_dock = nullptr;
	}

	blog(LOG_INFO, "[Record Source] unloaded");
	LogInfo("Plugin unloaded");
}
