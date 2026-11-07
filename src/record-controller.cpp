#include "record-controller.hpp"

#include "encoder-utils.hpp"
#include "localization.hpp"
#include "logger.hpp"

#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>

namespace {

constexpr uint32_t VIDEO_CHANNEL = 0;
constexpr uint64_t STOP_TIMEOUT_NS = 15ULL * 1000000000ULL;

std::atomic<unsigned> g_outputCounter{0};
std::atomic<unsigned> g_encoderCounter{0};

struct SourceSearch {
	const char *uuid;
	obs_source_t *source;
};

bool FindSourceByUuid(void *param, obs_source_t *source)
{
	auto *search = static_cast<SourceSearch *>(param);
	if (!source || obs_obj_is_private(source))
		return true;

	const char *uuid = obs_source_get_uuid(source);
	if (uuid && strcmp(uuid, search->uuid) == 0) {
		search->source = obs_source_get_ref(source);
		return false;
	}
	return true;
}

std::string FormatExtension(const std::string &format)
{
	if (format == "mp4")
		return "mp4";
	if (format == "mov")
		return "mov";
	if (format == "ts")
		return "ts";
	if (format == "flv")
		return "flv";
	return "mkv";
}

std::string NextOutputName()
{
	return "Record Source output " + std::to_string(g_outputCounter.fetch_add(1));
}

std::string NextEncoderName(const char *kind)
{
	return std::string("Record Source ") + kind + " " + std::to_string(g_encoderCounter.fetch_add(1));
}

uint64_t FirstMixerBit(uint32_t mixers)
{
	for (uint64_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
		if (mixers & (1u << i))
			return i;
	}
	return 0;
}

bool IsHighBitDepthFormat(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I412:
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
		return true;
	default:
		return false;
	}
}

bool IsHdrColorSpace(enum video_colorspace colorspace)
{
	return colorspace == VIDEO_CS_2100_PQ || colorspace == VIDEO_CS_2100_HLG;
}

// Runs `fn` on OBS' graphics thread and waits for it to finish when called from
// a different thread. Creating a private view, encoders and an output touches
// graphics resources, so keeping it on the graphics thread mirrors what OBS
// itself does for outputs.
void RunOnGraphics(const std::function<void()> &fn)
{
	if (obs_in_task_thread(OBS_TASK_GRAPHICS)) {
		fn();
		return;
	}

	struct TaskData {
		const std::function<void()> *fn;
	};

	TaskData data{&fn};
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *param) {
			auto *task = static_cast<TaskData *>(param);
			try {
				(*task->fn)();
			} catch (...) {
				// Exceptions must never cross the C callback boundary.
			}
		},
		&data, true);
}

} // namespace

RecordController::RecordController() = default;

RecordController::~RecordController()
{
	Shutdown();
}

bool RecordController::Start(const Config &config, std::string &error)
{
	if (output) {
		error = TrStd("Error.AlreadyRecording");
		return false;
	}

	bool ok = false;
	RunOnGraphics([&]() { ok = Setup(config, error); });

	if (!ok) {
		LogError(error);
		return false;
	}

	RunOnGraphics([&]() {
		if (output && !StartOutput(error)) {
			LogError(error);
			Cleanup();
			ok = false;
		}
	});

	if (ok)
		LogInfo("Recording started: source=" + config.sourceUuid +
			", file=" + outputPath);

	return ok;
}

bool RecordController::Setup(const Config &config, std::string &error)
{
	closing = false;
	stopRequested = false;
	stopRequestedAtNs = 0;
	audioDebugLogged = false;
	audioNonZeroLogged = false;
	lastError.clear();
	{
		std::lock_guard<std::mutex> lock(pathMutex);
		outputPath.clear();
	}
	startedAtNs = 0;
	endedAtNs = 0;
	lastBytes = 0;
	lastDroppedFrames = 0;

	SourceSearch search{config.sourceUuid.c_str(), nullptr};
	obs_enum_all_sources(FindSourceByUuid, &search);
	source = search.source;

	if (!source) {
		error = TrStd("Error.SourceNotFound");
		return false;
	}

	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_VIDEO) == 0) {
		error = TrStd("Error.SourceNoVideo");
		obs_source_release(source);
		source = nullptr;
		return false;
	}

	if (obs_source_removed(source)) {
		error = TrStd("Error.SourceRemoved");
		obs_source_release(source);
		source = nullptr;
		return false;
	}

	sourceName = obs_source_get_name(source);
	sourceUuid = config.sourceUuid;

	const bool splitByTime = config.splitMode == "time";
	const bool splitBySize = config.splitMode == "size";

	if (splitByTime && config.splitTimeMinutes < 1) {
		error = TrStd("Error.SplitTimeTooSmall");
		obs_source_release(source);
		source = nullptr;
		return false;
	}

	if (splitBySize && config.splitSizeMb < 20) {
		error = TrStd("Error.SplitSizeTooSmall");
		obs_source_release(source);
		source = nullptr;
		return false;
	}

	obs_source_t *selectedAudioSource = nullptr;
	if (!config.audioSourceUuid.empty() && config.audioSourceUuid != "__video__") {
		SourceSearch audioSearch{config.audioSourceUuid.c_str(), nullptr};
		obs_enum_all_sources(FindSourceByUuid, &audioSearch);
		selectedAudioSource = audioSearch.source;
		if (!selectedAudioSource) {
			obs_source_release(source);
			source = nullptr;
			error = TrStd("Error.AudioSourceNotFound");
			return false;
		}
		if (obs_source_removed(selectedAudioSource)) {
			obs_source_release(selectedAudioSource);
			obs_source_release(source);
			source = nullptr;
			error = TrStd("Error.AudioSourceRemoved");
			return false;
		}
	}

	if (config.audioSourceUuid == "__video__")
		selectedAudioSource = obs_source_get_ref(source);

	audioSource = selectedAudioSource ? selectedAudioSource : nullptr;
	if (audioSource && audioSource != source)
		obs_source_inc_active(audioSource);

	uint32_t width = obs_source_get_width(source);
	uint32_t height = obs_source_get_height(source);
	width += width & 1;
	height += height & 1;

	if (width == 0 || height == 0) {
		error = TrStd("Error.InvalidVideoSize");
		obs_source_release(source);
		source = nullptr;
		return false;
	}

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = TrStd("Error.VideoNotInitialized");
		obs_source_release(source);
		source = nullptr;
		return false;
	}

	ovi.base_width = width;
	ovi.base_height = height;
	ovi.output_width = width;
	ovi.output_height = height;

	// The recording view is independent from OBS' main output, so it can use
	// its own pixel format, color space and range. "Default"/NONE keeps the
	// values OBS is currently using.
	if (config.colorFormat != VIDEO_FORMAT_NONE)
		ovi.output_format = config.colorFormat;
	if (config.colorSpace != VIDEO_CS_DEFAULT)
		ovi.colorspace = config.colorSpace;
	if (config.colorRange != VIDEO_RANGE_DEFAULT)
		ovi.range = config.colorRange;

	const auto abortSetup = [this, &error](const char *message) {
		error = message;
		if (audioSource) {
			if (audioSource != source)
				obs_source_dec_active(audioSource);
			obs_source_release(audioSource);
			audioSource = nullptr;
		}
		if (source) {
			obs_source_release(source);
			source = nullptr;
		}
		return false;
	};

	if (IsHdrColorSpace(ovi.colorspace) && !IsHighBitDepthFormat(ovi.output_format))
		return abortSetup(TrStd("Error.Rec2100Needs10Bit").c_str());

	if (config.videoEncoderId == "obs_x264" &&
	    (IsHighBitDepthFormat(ovi.output_format) || IsHdrColorSpace(ovi.colorspace)))
		return abortSetup(TrStd("Error.X264NoHighDepth").c_str());

	view = obs_view_create();
	if (!view) {
		error = TrStd("Error.CreateView");
		obs_source_release(source);
		source = nullptr;
		return false;
	}

	videoOutput = obs_view_add2(view, &ovi);
	if (!videoOutput) {
		error = TrStd("Error.CreateVideoOutput");
		obs_view_destroy(view);
		view = nullptr;
		obs_source_release(source);
		source = nullptr;
		return false;
	}

	obs_view_set_source(view, VIDEO_CHANNEL, source);

	// Game/window capture only creates and activates its linked audio source
	// for the main view. The recording view is auxiliary, so mark the source
	// as active for the main view as well; this keeps the normal video path
	// unchanged while allowing its rerouted audio to be captured.
	obs_source_inc_active(source);
	sourceAudioActivated = true;

	obs_data_t *audioSettings =
		record_source::MergeEncoderSettings(config.audioEncoderId.c_str(), OBS_ENCODER_AUDIO,
						    config.audioSettings);

	if (!config.audioSourceUuid.empty() && config.audioSourceUuid != "__video__") {
		SourceSearch audioSearch{config.audioSourceUuid.c_str(), nullptr};
		obs_enum_all_sources(FindSourceByUuid, &audioSearch);
		selectedAudioSource = audioSearch.source;
		if (!selectedAudioSource) {
			obs_source_release(source);
			source = nullptr;
			error = TrStd("Error.AudioSourceNotFound");
			obs_data_release(audioSettings);
			return false;
		}
		if (obs_source_removed(selectedAudioSource)) {
			obs_source_release(selectedAudioSource);
			obs_source_release(source);
			source = nullptr;
			error = TrStd("Error.AudioSourceRemoved");
			obs_data_release(audioSettings);
			return false;
		}
	} else if (config.audioSourceUuid == "__video__") {
		selectedAudioSource = obs_source_get_ref(source);
	}

	audioSource = selectedAudioSource;
	if (audioSource) {
		int audioTrack = config.audioTrack;
		if (audioTrack < 1 || audioTrack > (int)MAX_AUDIO_MIXES)
			audioTrack = 2;

		originalAudioMixers = obs_source_get_audio_mixers(audioSource);
		restoreAudioMixers = true;
		obs_source_set_audio_mixers(audioSource, originalAudioMixers | (1u << (audioTrack - 1)));

		audioEncoder = obs_audio_encoder_create(config.audioEncoderId.c_str(),
							NextEncoderName("audio").c_str(), audioSettings,
							(size_t)(audioTrack - 1), nullptr);

		if (audioSource != source) {
			obs_source_inc_active(audioSource);
			selectedAudioActivated = true;
		}
	} else {
		const struct audio_output_info *globalAudio = audio_output_get_info(obs_get_audio());
		if (!globalAudio) {
			error = TrStd("Error.GetAudioConfig");
			obs_data_release(audioSettings);
			Cleanup();
			return false;
		}

		struct audio_output_info audioInfo = {};
		audioInfo.name = "Record Source audio";
		audioInfo.samples_per_sec = globalAudio->samples_per_sec;
		audioInfo.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audioInfo.speakers = globalAudio->speakers;
		audioInfo.input_callback = &RecordController::AudioCallback;
		audioInfo.input_param = this;

		if (audio_output_open(&audioOutput, &audioInfo) != 0 || !audioOutput) {
			error = TrStd("Error.CreateAudioTrack");
			obs_data_release(audioSettings);
			Cleanup();
			return false;
		}

		audioEncoder = obs_audio_encoder_create(config.audioEncoderId.c_str(),
							NextEncoderName("audio").c_str(), audioSettings,
							0, nullptr);
	}

	obs_data_release(audioSettings);

	if (!audioEncoder) {
		error = TrStd("Error.CreateAudioEncoder");
		Cleanup();
		return false;
	}

	if (audioOutput)
		obs_encoder_set_audio(audioEncoder, audioOutput);
	else if (audioSource)
		obs_encoder_set_audio(audioEncoder, obs_get_audio());

	obs_data_t *videoSettings =
		record_source::MergeEncoderSettings(config.videoEncoderId.c_str(), OBS_ENCODER_VIDEO,
						    config.videoSettings);
	videoEncoder = obs_video_encoder_create(config.videoEncoderId.c_str(),
						NextEncoderName("video").c_str(), videoSettings,
						nullptr);
	obs_data_release(videoSettings);

	if (!videoEncoder) {
		error = TrStd("Error.CreateVideoEncoder");
		Cleanup();
		return false;
	}

	obs_encoder_set_video(videoEncoder, videoOutput);

	if (os_mkdirs(config.directory.c_str()) == MKDIR_ERROR) {
		error = TrStd("Error.CreateDirectory") + config.directory;
		Cleanup();
		return false;
	}

	const std::string extension = FormatExtension(config.format);
	char *filename = os_generate_formatted_filename(
		extension.c_str(), true, "Record %CCYY-%MM-%DD %hh-%mm-%ss");

	std::string path = config.directory;
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path += '/';
	path += filename ? filename : "Record";
	bfree(filename);

	std::string candidate = path;
	for (int i = 1; os_file_exists(candidate.c_str()) && i < 1000; ++i) {
		candidate = path + " (" + std::to_string(i) + ")." + extension;
	}
	if (os_file_exists(candidate.c_str())) {
		error = TrStd("Error.NoFreeFileName");
		Cleanup();
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(pathMutex);
		outputPath = candidate;
	}

	obs_data_t *outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "path", outputPath.c_str());
	obs_data_set_string(outputSettings, "directory", config.directory.c_str());
	obs_data_set_string(outputSettings, "format", "Record %CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_string(outputSettings, "extension", extension.c_str());
	obs_data_set_bool(outputSettings, "split_file", splitByTime || splitBySize);
	obs_data_set_int(outputSettings, "max_size_mb", splitBySize ? config.splitSizeMb : 0);
	obs_data_set_int(outputSettings, "max_time_sec", splitByTime ? config.splitTimeMinutes * 60 : 0);
	obs_data_set_bool(outputSettings, "allow_overwrite", false);
	// Keep split parts named like the first file (with spaces), as OBS does.
	obs_data_set_bool(outputSettings, "allow_spaces", true);

	output = obs_output_create("ffmpeg_muxer", NextOutputName().c_str(), outputSettings, nullptr);
	obs_data_release(outputSettings);

	if (!output) {
		error = TrStd("Error.CreateFileOutput");
		Cleanup();
		return false;
	}

	// The muxer emits this signal when it starts a new part, so the UI can keep
	// pointing at the file that is currently being written.
	signal_handler_t *outputSignals = obs_output_get_signal_handler(output);
	signal_handler_connect(outputSignals, "file_changed", FileChanged, this);

	obs_output_set_video_encoder(output, videoEncoder);
	obs_output_set_audio_encoder(output, audioEncoder, 0);

	return true;
}

bool RecordController::StartOutput(std::string &error)
{
	if (!output) {
		error = TrStd("Error.OutputNotReady");
		return false;
	}

	if (!obs_output_start(output)) {
		const char *last = obs_output_get_last_error(output);
		error = last && *last ? std::string(last) : TrStd("Error.StartFailed");
		lastError = error;
		return false;
	}

	startedAtNs = os_gettime_ns();
	stopRequested = false;
	return true;
}

void RecordController::RequestStop()
{
	if (!output || stopRequested)
		return;

	stopRequested = true;
	stopRequestedAtNs = os_gettime_ns();
	closing = true;

	if (obs_output_active(output))
		obs_output_stop(output);
	else
		Cleanup();
}

void RecordController::Tick()
{
	if (!output)
		return;

	if (!stopRequested && source && obs_source_removed(source)) {
		lastError = TrStd("Error.SourceClosed");
		LogError(lastError);
		RequestStop();
		return;
	}

	if (stopRequested) {
		const bool inactive = !obs_output_active(output);
		const bool timedOut = stopRequestedAtNs &&
				      (os_gettime_ns() - stopRequestedAtNs) > STOP_TIMEOUT_NS;
		if (inactive) {
			Cleanup();
		} else if (timedOut) {
			if (!lastError.empty())
				lastError += " ";
			lastError += TrStd("Error.StopTimeout");
			LogError(lastError);
			obs_output_force_stop(output);
			Cleanup();
		}
	}
}

void RecordController::Shutdown()
{
	if (!output && !view && !source)
		return;

	closing = true;
	stopRequested = true;
	if (output) {
		if (obs_output_active(output))
			obs_output_force_stop(output);
	}
	Cleanup();
}

void RecordController::Cleanup()
{
	if (output && startedAtNs) {
		LogInfo("Recording stopped: file=" + OutputPath() +
			", bytes=" + std::to_string(obs_output_get_total_bytes(output)));
	}
	if (output) {
		signal_handler_t *outputSignals = obs_output_get_signal_handler(output);
		signal_handler_disconnect(outputSignals, "file_changed", FileChanged, this);
		lastBytes = obs_output_get_total_bytes(output);
		lastDroppedFrames = obs_output_get_frames_dropped(output);
		obs_output_release(output);
		output = nullptr;
	}

	if (audioEncoder) {
		obs_encoder_release(audioEncoder);
		audioEncoder = nullptr;
	}

	if (videoEncoder) {
		obs_encoder_release(videoEncoder);
		videoEncoder = nullptr;
	}

	if (audioOutput) {
		audio_output_close(audioOutput);
		audioOutput = nullptr;
	}

	if (sourceAudioActivated && source)
		obs_source_dec_active(source);
	if (selectedAudioActivated && audioSource && audioSource != source)
		obs_source_dec_active(audioSource);

	if (restoreAudioMixers && audioSource)
		obs_source_set_audio_mixers(audioSource, originalAudioMixers);

	if (view) {
		obs_view_set_source(view, VIDEO_CHANNEL, nullptr);
		obs_view_remove(view);
		obs_view_destroy(view);
		view = nullptr;
		videoOutput = nullptr;
	}

	if (audioSource) {
		obs_source_release(audioSource);
		audioSource = nullptr;
	}

	if (source) {
		obs_source_release(source);
		source = nullptr;
	}

	endedAtNs = os_gettime_ns();
}

uint64_t RecordController::DurationMs() const
{
	if (!startedAtNs)
		return 0;
	const uint64_t end = endedAtNs ? endedAtNs : os_gettime_ns();
	const uint64_t stop = stopRequestedAtNs && stopRequestedAtNs < end ? stopRequestedAtNs : end;
	return (stop - startedAtNs) / 1000000ULL;
}

uint64_t RecordController::Bytes() const
{
	if (output)
		return obs_output_get_total_bytes(output);
	return lastBytes;
}

int RecordController::DroppedFrames() const
{
	if (output)
		return obs_output_get_frames_dropped(output);
	return lastDroppedFrames;
}

std::string RecordController::LastError() const
{
	if (output) {
		const char *error = obs_output_get_last_error(output);
		if (error && *error)
			return error;
	}
	return lastError;
}

std::string RecordController::OutputPath() const
{
	std::lock_guard<std::mutex> lock(pathMutex);
	return outputPath;
}

void RecordController::FileChanged(void *param, calldata_t *calldata)
{
	auto *self = static_cast<RecordController *>(param);
	const char *path = calldata_string(calldata, "next_file");
	if (!path || !*path)
		return;

	std::lock_guard<std::mutex> lock(self->pathMutex);
	self->outputPath = path;
}

void RecordController::FillSilence(uint32_t mixers, struct audio_output_data *mixes) const
{
	const size_t channels = audioOutput ? audio_output_get_channels(audioOutput) : 2;
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
		if ((mixers & (1u << mix)) == 0)
			continue;
		for (size_t ch = 0; ch < channels; ++ch) {
			if (mixes[mix].data[ch])
				memset(mixes[mix].data[ch], 0, AUDIO_OUTPUT_FRAMES * sizeof(float));
		}
	}
}

namespace {

struct MixContext {
	struct obs_source_audio audio;
	uint64_t mixer;
};

void CalcMinTimestamp(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	auto *minTs = static_cast<uint64_t *>(param);
	if (!child || obs_source_audio_pending(child))
		return;

	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (ts && (!*minTs || ts < *minTs))
		*minTs = ts;
}

void MixChildAudio(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	if (!child || obs_source_audio_pending(child))
		return;

	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts)
		return;

	auto *ctx = static_cast<MixContext *>(param);
	if (ts < ctx->audio.timestamp)
		return;

	const size_t pos = (size_t)ns_to_audio_frames(ctx->audio.samples_per_sec, ts - ctx->audio.timestamp);
	if (pos >= AUDIO_OUTPUT_FRAMES)
		return;

	const size_t count = AUDIO_OUTPUT_FRAMES - pos;
	struct obs_source_audio_mix childAudio = {};
	obs_source_get_audio_mix(child, &childAudio);

	for (size_t ch = 0; ch < (size_t)ctx->audio.speakers; ++ch) {
		float *out = ((float *)ctx->audio.data[ch]) + pos;
		uint32_t childMixers = obs_source_get_audio_mixers(child);
		if (!childMixers)
			childMixers = 1;
		for (size_t srcMix = 0; srcMix < MAX_AUDIO_MIXES; ++srcMix) {
			if ((childMixers & (1u << srcMix)) == 0)
				continue;
			float *in = childAudio.output[srcMix].data[ch];
			if (!in)
				continue;
			for (size_t i = 0; i < count; ++i)
				out[i] += in[i];
		}
	}
}

} // namespace

bool RecordController::AudioCallback(void *param, uint64_t startTs, uint64_t endTs, uint64_t *outTs,
				     uint32_t mixers, struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(endTs);

	auto *self = static_cast<RecordController *>(param);
	self->FillSilence(mixers, mixes);
	*outTs = startTs;

	// When a real audio source is selected, the audio encoder reads OBS'
	// global audio output (the same model as OBS' own recordings). This
	// callback is only used for the explicit "Silence" option.
	return true;
}
