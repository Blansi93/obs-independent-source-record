#pragma once

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Records one OBS source to an independent file.
//
// Video is rendered through a private obs_view and encoded by a private video
// encoder. Audio is tapped from the source through a private audio_t (the same
// technique used by the source-record plugin), encoded by a private audio
// encoder and muxed by a private ffmpeg_muxer output. None of these objects are
// connected to OBS' streaming, recording or replay-buffer outputs.
class RecordController {
public:
	struct Config {
		std::string sourceUuid;
		std::string audioSourceUuid;
		int audioTrack = 2;
		std::string directory;
		std::string format; // mkv, mp4, mov, ts, flv
		// VIDEO_FORMAT_NONE / VIDEO_CS_DEFAULT / VIDEO_RANGE_DEFAULT mean
		// "keep the value OBS is currently using".
		enum video_format colorFormat = VIDEO_FORMAT_NONE;
		enum video_colorspace colorSpace = VIDEO_CS_DEFAULT;
		enum video_range_type colorRange = VIDEO_RANGE_DEFAULT;
		std::string videoEncoderId;
		std::string audioEncoderId;
		obs_data_t *videoSettings = nullptr; // borrowed
		obs_data_t *audioSettings = nullptr; // borrowed
		// File splitting: empty disables it, "time" starts a new file every
		// splitTimeMinutes and "size" every splitSizeMb.
		std::string splitMode;
		int splitTimeMinutes = 0;
		int splitSizeMb = 0;
	};

	RecordController();
	~RecordController();

	RecordController(const RecordController &) = delete;
	RecordController &operator=(const RecordController &) = delete;

	bool Start(const Config &config, std::string &error);

	// Requests a graceful stop. Call Tick() regularly until active() is false.
	void RequestStop();

	// Drives asynchronous cleanup and detects a removed source.
	void Tick();

	// Forcefully stops and releases everything (used on OBS exit).
	void Shutdown();

	bool active() const { return output != nullptr; }
	bool stopping() const { return stopRequested.load(std::memory_order_relaxed); }

	uint64_t DurationMs() const;
	uint64_t Bytes() const;
	int DroppedFrames() const;

	std::string LastError() const;
	std::string OutputPath() const;

	const std::string &SourceName() const { return sourceName; }

private:
	static bool AudioCallback(void *param, uint64_t startTs, uint64_t endTs, uint64_t *outTs,
				  uint32_t mixers, struct audio_output_data *mixes);
	static void FileChanged(void *param, calldata_t *calldata);

	bool Setup(const Config &config, std::string &error);
	bool StartOutput(std::string &error);
	void Cleanup();

	void FillSilence(uint32_t mixers, struct audio_output_data *mixes) const;

	obs_source_t *source = nullptr;
	obs_source_t *audioSource = nullptr;
	obs_view_t *view = nullptr;

	bool sourceAudioActivated = false;
	bool selectedAudioActivated = false;
	bool restoreAudioMixers = false;
	uint32_t originalAudioMixers = 0;
	video_t *videoOutput = nullptr;

	audio_t *audioOutput = nullptr;
	obs_encoder_t *videoEncoder = nullptr;
	obs_encoder_t *audioEncoder = nullptr;
	obs_output_t *output = nullptr;

	std::atomic<bool> closing{false};
	std::atomic<bool> stopRequested{false};
	bool audioDebugLogged{false};
	bool audioNonZeroLogged{false};

	uint64_t startedAtNs = 0;
	uint64_t stopRequestedAtNs = 0;
	uint64_t endedAtNs = 0;

	uint64_t lastBytes = 0;
	int lastDroppedFrames = 0;

	std::string sourceName;
	std::string sourceUuid;
	std::string outputPath;
	std::string lastError;
	mutable std::mutex pathMutex;

	int tapMixerIndex = 0;
};
