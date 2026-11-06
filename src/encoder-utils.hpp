#pragma once

#include <obs.h>

#include <string>
#include <vector>

namespace record_source {

struct EncoderChoice {
	std::string id;
	std::string displayName;
};

std::vector<EncoderChoice> ListEncoders(enum obs_encoder_type type);

bool EncoderExists(const char *id, enum obs_encoder_type type);

// Returns a new reference to the encoder's default settings.
obs_data_t *CreateEncoderDefaults(const char *id, enum obs_encoder_type type);

// Returns a new settings object: encoder defaults overridden by `saved`.
obs_data_t *MergeEncoderSettings(const char *id, enum obs_encoder_type type, obs_data_t *saved);

std::string GetEncoderDisplayName(const char *id);

} // namespace record_source
