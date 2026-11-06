#include "encoder-utils.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>

namespace record_source {

namespace {

std::string UniqueEncoderName(enum obs_encoder_type type)
{
	static std::atomic<unsigned> counter{0};
	const char *kind = type == OBS_ENCODER_VIDEO ? "video" : "audio";
	return std::string("Record Source preview ") + kind + " " + std::to_string(counter.fetch_add(1));
}

} // namespace

std::vector<EncoderChoice> ListEncoders(enum obs_encoder_type type)
{
	std::vector<EncoderChoice> encoders;
	std::set<std::string> seenIds;
	std::set<std::string> seenNames;

	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		if (!id || obs_get_encoder_type(id) != type)
			continue;
		// Internal encoders (e.g. NVENC "Fallback") are hidden in OBS and used
		// as an automatic reroute; exposing them only adds noise and slower paths.
		if (obs_get_encoder_caps(id) & OBS_ENCODER_CAP_INTERNAL)
			continue;
		if (!seenIds.insert(id).second)
			continue;

		const char *display = obs_encoder_get_display_name(id);
		std::string displayName = display && *display ? display : id;
		std::string normalizedName = displayName;
		std::transform(normalizedName.begin(), normalizedName.end(), normalizedName.begin(),
			       [](unsigned char c) { return (char)std::tolower(c); });

		if (!seenNames.insert(normalizedName).second)
			continue;

		EncoderChoice choice;
		choice.id = id;
		choice.displayName = displayName;
		encoders.push_back(std::move(choice));
	}

	std::sort(encoders.begin(), encoders.end(),
		  [](const EncoderChoice &a, const EncoderChoice &b) { return a.displayName < b.displayName; });

	return encoders;
}

bool EncoderExists(const char *id, enum obs_encoder_type type)
{
	if (!id || !*id)
		return false;

	const char *value = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &value); ++i) {
		if (value && strcmp(value, id) == 0)
			return obs_get_encoder_type(id) == type;
	}
	return false;
}

obs_data_t *CreateEncoderDefaults(const char *id, enum obs_encoder_type type)
{
	if (!id || !*id)
		return obs_data_create();

	obs_data_t *empty = obs_data_create();
	obs_encoder_t *encoder = nullptr;

	if (type == OBS_ENCODER_VIDEO) {
		encoder = obs_video_encoder_create(id, UniqueEncoderName(type).c_str(), empty, nullptr);
	} else {
		encoder = obs_audio_encoder_create(id, UniqueEncoderName(type).c_str(), empty, 0, nullptr);
	}

	obs_data_t *defaults = nullptr;
	if (encoder) {
		defaults = obs_encoder_get_defaults(encoder);
		obs_encoder_release(encoder);
	}

	if (!defaults)
		defaults = obs_data_create();

	obs_data_release(empty);
	return defaults;
}

obs_data_t *MergeEncoderSettings(const char *id, enum obs_encoder_type type, obs_data_t *saved)
{
	obs_data_t *merged = CreateEncoderDefaults(id, type);
	if (saved)
		obs_data_apply(merged, saved);
	return merged;
}

std::string GetEncoderDisplayName(const char *id)
{
	if (!id)
		return {};
	const char *display = obs_encoder_get_display_name(id);
	return display && *display ? display : id;
}

} // namespace record_source
