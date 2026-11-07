#include "record-source-dock.hpp"

#include "encoder-properties-dialog.hpp"
#include "encoder-utils.hpp"
#include "localization.hpp"
#include "logger.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace {

constexpr const char *CONFIG_FILE = "settings.json";
constexpr const char *VIDEO_SETTINGS_GROUP = "video_encoder_settings";
constexpr const char *AUDIO_SETTINGS_GROUP = "audio_encoder_settings";

QString ToQString(const std::string &text)
{
	return QString::fromUtf8(text.c_str());
}

QString ToQString(const char *text)
{
	return text ? QString::fromUtf8(text) : QString();
}

std::string ConfigFilePath()
{
	char *path = obs_module_config_path(CONFIG_FILE);
	std::string result = path ? path : std::string();
	bfree(path);
	return result;
}

void EnsureParentDirectory(const std::string &file)
{
	const size_t slash = file.find_last_of("/\\");
	if (slash == std::string::npos)
		return;

	const std::string directory = file.substr(0, slash);
	if (!directory.empty())
		os_mkdirs(directory.c_str());
}

QString DefaultDirectory()
{
	QString directory;
	char *recordPath = obs_frontend_get_current_record_output_path();
	if (recordPath && *recordPath)
		directory = ToQString(recordPath);
	bfree(recordPath);

	if (directory.isEmpty() || !QDir(directory).exists())
		directory = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (directory.isEmpty())
		directory = QDir::homePath();

	return QDir::toNativeSeparators(directory);
}

QString FormatDuration(uint64_t milliseconds)
{
	const uint64_t totalSeconds = milliseconds / 1000;
	const uint64_t hours = totalSeconds / 3600;
	const uint64_t minutes = (totalSeconds / 60) % 60;
	const uint64_t seconds = totalSeconds % 60;

	return QStringLiteral("%1:%2:%3")
		.arg(hours, 2, 10, QChar('0'))
		.arg(minutes, 2, 10, QChar('0'))
		.arg(seconds, 2, 10, QChar('0'));
}

QString FormatBytes(uint64_t bytes)
{
	const double value = (double)bytes;
	if (value >= 1024.0 * 1024.0 * 1024.0)
		return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
	if (value >= 1024.0 * 1024.0)
		return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 2);
	if (value >= 1024.0)
		return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
	return QStringLiteral("%1 B").arg(bytes);
}

obs_data_t *LoadEncoderSettings(obs_data_t *root, const char *group, const char *encoderId)
{
	obs_data_t *settings = nullptr;

	if (encoderId && *encoderId && root) {
		obs_data_t *all = obs_data_get_obj(root, group);
		if (all) {
			settings = obs_data_get_obj(all, encoderId);
			obs_data_release(all);
		}
	}

	if (!settings)
		settings = obs_data_create();

	return settings;
}

void StoreEncoderSettings(obs_data_t *root, const char *group, const char *encoderId, obs_data_t *settings)
{
	if (!root || !encoderId || !*encoderId || !settings)
		return;

	obs_data_t *all = obs_data_get_obj(root, group);
	if (!all)
		all = obs_data_create();

	obs_data_set_obj(all, encoderId, settings);
	obs_data_set_obj(root, group, all);
	obs_data_release(all);
}

struct SourceEntry {
	QString name;
	QString uuid;
};

bool EnumSource(void *param, obs_source_t *source)
{
	if (!source || obs_obj_is_private(source))
		return true;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0)
		return true;

	const char *uuid = obs_source_get_uuid(source);
	const char *name = obs_source_get_name(source);
	if (!uuid || !*uuid)
		return true;

	auto *sources = static_cast<std::vector<SourceEntry> *>(param);
	sources->push_back({ToQString(name), ToQString(uuid)});
	return true;
}

bool EnumAudioSource(void *param, obs_source_t *source)
{
	if (!source || obs_obj_is_private(source))
		return true;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return true;

	const char *uuid = obs_source_get_uuid(source);
	const char *name = obs_source_get_name(source);
	if (!uuid || !*uuid)
		return true;

	auto *sources = static_cast<std::vector<SourceEntry> *>(param);
	sources->push_back({ToQString(name), ToQString(uuid)});
	return true;
}

QString EncoderKey(enum obs_encoder_type type)
{
	return type == OBS_ENCODER_VIDEO ? QStringLiteral("video_encoder") : QStringLiteral("audio_encoder");
}

QString PreferredEncoder(enum obs_encoder_type type)
{
	// Prefer commonly available OBS encoders; otherwise the first available one
	// is selected once the list has been populated.
	if (type == OBS_ENCODER_VIDEO) {
		const char *candidates[] = {"obs_x264", "jim_nvenc", "obs_qsv11_v2", "obs_qsv11",
					    "ffmpeg_svt_av1", "jim_av1_nvenc"};
		for (const char *id : candidates) {
			if (record_source::EncoderExists(id, type))
				return ToQString(id);
		}
	} else {
		const char *candidates[] = {"ffmpeg_aac", "ffmpeg_opus"};
		for (const char *id : candidates) {
			if (record_source::EncoderExists(id, type))
				return ToQString(id);
		}
	}

	const auto encoders = record_source::ListEncoders(type);
	return encoders.empty() ? QString() : ToQString(encoders.front().id);
}

enum video_format ColorFormatFromKey(const QString &key)
{
	if (key == QStringLiteral("nv12"))
		return VIDEO_FORMAT_NV12;
	if (key == QStringLiteral("i420"))
		return VIDEO_FORMAT_I420;
	if (key == QStringLiteral("i444"))
		return VIDEO_FORMAT_I444;
	if (key == QStringLiteral("p010"))
		return VIDEO_FORMAT_P010;
	return VIDEO_FORMAT_NONE;
}

enum video_colorspace ColorSpaceFromKey(const QString &key)
{
	if (key == QStringLiteral("srgb"))
		return VIDEO_CS_SRGB;
	if (key == QStringLiteral("709"))
		return VIDEO_CS_709;
	if (key == QStringLiteral("601"))
		return VIDEO_CS_601;
	if (key == QStringLiteral("2100pq"))
		return VIDEO_CS_2100_PQ;
	if (key == QStringLiteral("2100hlg"))
		return VIDEO_CS_2100_HLG;
	return VIDEO_CS_DEFAULT;
}

enum video_range_type ColorRangeFromKey(const QString &key)
{
	if (key == QStringLiteral("partial"))
		return VIDEO_RANGE_PARTIAL;
	if (key == QStringLiteral("full"))
		return VIDEO_RANGE_FULL;
	return VIDEO_RANGE_DEFAULT;
}

} // namespace

RecordSourceDock::RecordSourceDock(QWidget *parent) : QWidget(parent)
{
	settings = obs_data_create();

	const std::string configPath = ConfigFilePath();
	if (!configPath.empty()) {
		obs_data_t *loaded = obs_data_create_from_json_file(configPath.c_str());
		if (loaded) {
			obs_data_apply(settings, loaded);
			obs_data_release(loaded);
		}
	}

	BuildUi();
	LoadSettings();
	uiBuilt = true;
	UpdateUi();

	timer = new QTimer(this);
	timer->setInterval(250);
	connect(timer, &QTimer::timeout, this, [this]() {
		controller.Tick();
		UpdateUi();
	});
	timer->start();
}

RecordSourceDock::~RecordSourceDock()
{
	Shutdown();

	if (videoSettings) {
		obs_data_release(videoSettings);
		videoSettings = nullptr;
	}
	if (audioSettings) {
		obs_data_release(audioSettings);
		audioSettings = nullptr;
	}
	if (settings) {
		obs_data_release(settings);
		settings = nullptr;
	}
}

void RecordSourceDock::BuildUi()
{
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(8, 8, 8, 8);
	mainLayout->setSpacing(8);

	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

	sourceCombo = new QComboBox(this);
	sourceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	sourceCombo->setMinimumContentsLength(18);
	refreshSourcesButton = new QPushButton(Tr("Dock.Refresh"), this);
	refreshSourcesButton->setToolTip(Tr("Dock.Refresh.Tooltip"));
	refreshSourcesButton->setAutoDefault(false);

	auto *sourceRow = new QHBoxLayout();
	sourceRow->setContentsMargins(0, 0, 0, 0);
	sourceRow->setSpacing(6);
	sourceRow->addWidget(sourceCombo, 1);
	sourceRow->addWidget(refreshSourcesButton);
	form->addRow(Tr("Dock.Source"), sourceRow);

	directoryEdit = new QLineEdit(this);
	directoryEdit->setPlaceholderText(Tr("Dock.Directory.Placeholder"));
	browseButton = new QPushButton(Tr("Dock.Browse"), this);
	browseButton->setAutoDefault(false);

	auto *directoryRow = new QHBoxLayout();
	directoryRow->setContentsMargins(0, 0, 0, 0);
	directoryRow->setSpacing(6);
	directoryRow->addWidget(directoryEdit, 1);
	directoryRow->addWidget(browseButton);
	form->addRow(Tr("Dock.Directory"), directoryRow);

	formatCombo = new QComboBox(this);
	formatCombo->addItem(QStringLiteral("Matroska (.mkv)"), QStringLiteral("mkv"));
	formatCombo->addItem(QStringLiteral("MP4 (.mp4)"), QStringLiteral("mp4"));
	formatCombo->addItem(QStringLiteral("QuickTime (.mov)"), QStringLiteral("mov"));
	formatCombo->addItem(QStringLiteral("MPEG-TS (.ts)"), QStringLiteral("ts"));
	formatCombo->addItem(QStringLiteral("FLV (.flv)"), QStringLiteral("flv"));
	form->addRow(Tr("Dock.Format"), formatCombo);

	splitModeCombo = new QComboBox(this);
	splitModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	splitModeCombo->setMinimumContentsLength(12);
	splitModeCombo->addItem(Tr("Dock.Split.None"), QStringLiteral(""));
	splitModeCombo->addItem(Tr("Dock.Split.Time"), QStringLiteral("time"));
	splitModeCombo->addItem(Tr("Dock.Split.Size"), QStringLiteral("size"));
	splitModeCombo->setToolTip(Tr("Dock.Split.Tooltip"));

	splitValueSpin = new QSpinBox(this);
	splitValueSpin->setKeyboardTracking(false);
	splitValueSpin->setAccelerated(true);

	auto *splitRow = new QHBoxLayout();
	splitRow->setContentsMargins(0, 0, 0, 0);
	splitRow->setSpacing(6);
	splitRow->addWidget(splitModeCombo, 1);
	splitRow->addWidget(splitValueSpin);
	form->addRow(Tr("Dock.Split"), splitRow);

	colorFormatCombo = new QComboBox(this);
	colorFormatCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	colorFormatCombo->setMinimumContentsLength(18);
	colorFormatCombo->addItem(Tr("Dock.Auto"), QStringLiteral("auto"));
	colorFormatCombo->addItem(QStringLiteral("NV12 (8 bit, 4:2:0)"), QStringLiteral("nv12"));
	colorFormatCombo->addItem(QStringLiteral("I420 (8 bit, 4:2:0)"), QStringLiteral("i420"));
	colorFormatCombo->addItem(QStringLiteral("I444 (8 bit, 4:4:4)"), QStringLiteral("i444"));
	colorFormatCombo->addItem(QStringLiteral("P010 (10 bit, 4:2:0)"), QStringLiteral("p010"));
	colorFormatCombo->setToolTip(Tr("Dock.ColorFormat.Tooltip"));
	form->addRow(Tr("Dock.ColorFormat"), colorFormatCombo);

	colorSpaceCombo = new QComboBox(this);
	colorSpaceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	colorSpaceCombo->setMinimumContentsLength(18);
	colorSpaceCombo->addItem(Tr("Dock.Auto"), QStringLiteral("auto"));
	colorSpaceCombo->addItem(QStringLiteral("Rec. 709 (SDR)"), QStringLiteral("709"));
	colorSpaceCombo->addItem(QStringLiteral("Rec. 601 (SDR)"), QStringLiteral("601"));
	colorSpaceCombo->addItem(QStringLiteral("sRGB (SDR)"), QStringLiteral("srgb"));
	colorSpaceCombo->addItem(QStringLiteral("Rec. 2100 PQ (HDR, Rec. 2020)"),
				 QStringLiteral("2100pq"));
	colorSpaceCombo->addItem(QStringLiteral("Rec. 2100 HLG (HDR, Rec. 2020)"),
				 QStringLiteral("2100hlg"));
	colorSpaceCombo->setToolTip(Tr("Dock.ColorSpace.Tooltip"));
	form->addRow(Tr("Dock.ColorSpace"), colorSpaceCombo);

	colorRangeCombo = new QComboBox(this);
	colorRangeCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	colorRangeCombo->setMinimumContentsLength(18);
	colorRangeCombo->addItem(Tr("Dock.Auto"), QStringLiteral("auto"));
	colorRangeCombo->addItem(Tr("Dock.Range.Partial"), QStringLiteral("partial"));
	colorRangeCombo->addItem(Tr("Dock.Range.Full"), QStringLiteral("full"));
	colorRangeCombo->setToolTip(Tr("Dock.ColorRange.Tooltip"));
	form->addRow(Tr("Dock.ColorRange"), colorRangeCombo);

	videoEncoderCombo = new QComboBox(this);
	videoEncoderCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	videoEncoderCombo->setMinimumContentsLength(18);
	videoConfigButton = new QPushButton(Tr("Dock.Configure"), this);
	videoConfigButton->setToolTip(Tr("Dock.Video.Tooltip"));
	videoConfigButton->setAutoDefault(false);

	auto *videoRow = new QHBoxLayout();
	videoRow->setContentsMargins(0, 0, 0, 0);
	videoRow->setSpacing(6);
	videoRow->addWidget(videoEncoderCombo, 1);
	videoRow->addWidget(videoConfigButton);
	form->addRow(Tr("Dock.Video"), videoRow);

	audioEncoderCombo = new QComboBox(this);
	audioEncoderCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	audioEncoderCombo->setMinimumContentsLength(18);
	audioConfigButton = new QPushButton(Tr("Dock.Configure"), this);
	audioConfigButton->setToolTip(Tr("Dock.Audio.Tooltip"));
	audioConfigButton->setAutoDefault(false);

	auto *audioRow = new QHBoxLayout();
	audioRow->setContentsMargins(0, 0, 0, 0);
	audioRow->setSpacing(6);
	audioRow->addWidget(audioEncoderCombo, 1);
	audioRow->addWidget(audioConfigButton);
	form->addRow(Tr("Dock.Audio"), audioRow);

	audioSourceCombo = new QComboBox(this);
	audioSourceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	audioSourceCombo->setMinimumContentsLength(18);

	const QString audioSourceHelpText = Tr("Dock.AudioSource.Tooltip");

	auto *audioSourceLabel = new QLabel(Tr("Dock.AudioSource"), this);
	audioSourceLabel->setToolTip(audioSourceHelpText);

	auto *audioSourceHelp = new QLabel(QStringLiteral("(?)"), this);
	audioSourceHelp->setToolTip(audioSourceHelpText);
	audioSourceHelp->setCursor(Qt::WhatsThisCursor);

	auto *audioSourceLabelBox = new QWidget(this);
	auto *audioSourceLabelRow = new QHBoxLayout(audioSourceLabelBox);
	audioSourceLabelRow->setContentsMargins(0, 0, 0, 0);
	audioSourceLabelRow->setSpacing(4);
	audioSourceLabelRow->addStretch(1);
	audioSourceLabelRow->addWidget(audioSourceLabel);
	audioSourceLabelRow->addWidget(audioSourceHelp);

	form->addRow(audioSourceLabelBox, audioSourceCombo);

	audioTrackCombo = new QComboBox(this);
	for (int track = 1; track <= 6; ++track) {
		audioTrackCombo->addItem(Tr("Dock.AudioTrack.Number").arg(track), track);
	}
	form->addRow(Tr("Dock.AudioTrack"), audioTrackCombo);

	mainLayout->addLayout(form);

	auto *buttonRow = new QHBoxLayout();
	buttonRow->setContentsMargins(0, 0, 0, 0);
	buttonRow->setSpacing(6);

	startButton = new QPushButton(Tr("Dock.Start"), this);
	startButton->setDefault(true);
	stopButton = new QPushButton(Tr("Dock.Stop"), this);
	openFolderButton = new QPushButton(Tr("Dock.OpenFolder"), this);
	openFolderButton->setAutoDefault(false);

	buttonRow->addWidget(startButton, 1);
	buttonRow->addWidget(stopButton);
	buttonRow->addWidget(openFolderButton);
	mainLayout->addLayout(buttonRow);

	statusLabel = new QLabel(Tr("Dock.Status.Ready"), this);
	statusLabel->setWordWrap(true);
	statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	mainLayout->addWidget(statusLabel);

	durationLabel = new QLabel(this);
	sizeLabel = new QLabel(this);
	fileLabel = new QLabel(this);
	fileLabel->setWordWrap(true);
	fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

	mainLayout->addWidget(durationLabel);
	mainLayout->addWidget(sizeLabel);
	mainLayout->addWidget(fileLabel);
	mainLayout->addStretch(1);

	connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
		PopulateAudioSources();
		UpdateUi();
	});
	connect(splitModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
		UpdateSplitControls();
		UpdateUi();
	});
	connect(splitValueSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
		const QString mode = splitModeCombo->currentData().toString();
		if (mode == QStringLiteral("time"))
			splitTimeMinutes = value;
		else if (mode == QStringLiteral("size"))
			splitSizeMb = value;
	});
	connect(refreshSourcesButton, &QPushButton::clicked, this, [this]() {
		PopulateSources();
		PopulateAudioSources();
		UpdateUi();
	});
	connect(browseButton, &QPushButton::clicked, this, &RecordSourceDock::BrowseDirectory);
	connect(startButton, &QPushButton::clicked, this, &RecordSourceDock::StartRecording);
	connect(stopButton, &QPushButton::clicked, this, &RecordSourceDock::StopRecording);
	connect(openFolderButton, &QPushButton::clicked, this, &RecordSourceDock::OpenOutputFolder);
	connect(videoConfigButton, &QPushButton::clicked, this, &RecordSourceDock::ConfigureVideoEncoder);
	connect(audioConfigButton, &QPushButton::clicked, this, &RecordSourceDock::ConfigureAudioEncoder);

	connect(videoEncoderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
		const QString previous = videoEncoderCombo->property("currentId").toString();
		if (!previous.isEmpty() && videoSettings)
			StoreEncoderSettings(settings, VIDEO_SETTINGS_GROUP, previous.toUtf8().constData(),
					     videoSettings);

		if (videoSettings) {
			obs_data_release(videoSettings);
			videoSettings = nullptr;
		}

		const QString current = videoEncoderCombo->currentData().toString();
		videoEncoderCombo->setProperty("currentId", current);
		videoSettings = LoadEncoderSettings(settings, VIDEO_SETTINGS_GROUP,
						    current.toUtf8().constData());
		obs_data_set_string(settings, "video_encoder", current.toUtf8().constData());
		UpdateUi();
	});

	connect(audioEncoderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
		const QString previous = audioEncoderCombo->property("currentId").toString();
		if (!previous.isEmpty() && audioSettings)
			StoreEncoderSettings(settings, AUDIO_SETTINGS_GROUP, previous.toUtf8().constData(),
					     audioSettings);

		if (audioSettings) {
			obs_data_release(audioSettings);
			audioSettings = nullptr;
		}

		const QString current = audioEncoderCombo->currentData().toString();
		audioEncoderCombo->setProperty("currentId", current);
		audioSettings = LoadEncoderSettings(settings, AUDIO_SETTINGS_GROUP,
						    current.toUtf8().constData());
		obs_data_set_string(settings, "audio_encoder", current.toUtf8().constData());
		UpdateUi();
	});

	PopulateEncoders();
	PopulateSources();
	PopulateAudioSources();
	UpdateUi();
}

void RecordSourceDock::LoadSettings()
{
	QString directory = ToQString(obs_data_get_string(settings, "directory"));
	if (directory.isEmpty())
		directory = DefaultDirectory();
	directoryEdit->setText(QDir::toNativeSeparators(directory));

	const QString format = ToQString(obs_data_get_string(settings, "format"));
	{
		const QSignalBlocker blocker(formatCombo);
		int index = formatCombo->findData(format);
		if (index < 0)
			index = formatCombo->findData(QStringLiteral("mkv"));
		formatCombo->setCurrentIndex(index < 0 ? 0 : index);
	}

	const auto loadColorCombo = [this](QComboBox *combo, const char *key) {
		const QSignalBlocker blocker(combo);
		const QString saved = ToQString(obs_data_get_string(settings, key));
		int index = combo->findData(saved);
		if (index < 0)
			index = 0; // Automatic
		combo->setCurrentIndex(index);
	};
	loadColorCombo(colorFormatCombo, "color_format");
	loadColorCombo(colorSpaceCombo, "color_space");
	loadColorCombo(colorRangeCombo, "color_range");

	{
		const QSignalBlocker blocker(splitModeCombo);
		const QString savedMode = ToQString(obs_data_get_string(settings, "split_mode"));
		int index = splitModeCombo->findData(savedMode);
		if (index < 0)
			index = 0; // No dividir
		splitModeCombo->setCurrentIndex(index);
	}
	splitTimeMinutes = obs_data_get_int(settings, "split_time_min");
	if (splitTimeMinutes < 1)
		splitTimeMinutes = 15;
	splitSizeMb = obs_data_get_int(settings, "split_size_mb");
	if (splitSizeMb < 20)
		splitSizeMb = 2048;
	UpdateSplitControls();

	const QString videoEncoder = ToQString(obs_data_get_string(settings, "video_encoder"));
	{
		const QSignalBlocker blocker(videoEncoderCombo);
		int index = videoEncoderCombo->findData(videoEncoder);
		if (index < 0)
			index = videoEncoderCombo->findData(PreferredEncoder(OBS_ENCODER_VIDEO));
		videoEncoderCombo->setCurrentIndex(index < 0 ? 0 : index);
		videoEncoderCombo->setProperty("currentId", videoEncoderCombo->currentData().toString());
	}

	const QString audioEncoder = ToQString(obs_data_get_string(settings, "audio_encoder"));
	{
		const QSignalBlocker blocker(audioEncoderCombo);
		int index = audioEncoderCombo->findData(audioEncoder);
		if (index < 0)
			index = audioEncoderCombo->findData(PreferredEncoder(OBS_ENCODER_AUDIO));
		audioEncoderCombo->setCurrentIndex(index < 0 ? 0 : index);
		audioEncoderCombo->setProperty("currentId", audioEncoderCombo->currentData().toString());
	}

	videoSettings = LoadEncoderSettings(settings, VIDEO_SETTINGS_GROUP,
					    videoEncoderCombo->currentData().toString().toUtf8().constData());
	audioSettings = LoadEncoderSettings(settings, AUDIO_SETTINGS_GROUP,
					    audioEncoderCombo->currentData().toString().toUtf8().constData());

	QString audioSource = ToQString(obs_data_get_string(settings, "audio_source_uuid"));
	if (audioSource == QStringLiteral("__video__"))
		audioSource = sourceCombo->currentData().toString();
	{
		const QSignalBlocker blocker(audioSourceCombo);
		int index = audioSourceCombo->findData(audioSource);
		if (index < 0 && !sourceCombo->currentData().toString().isEmpty())
			index = audioSourceCombo->findData(sourceCombo->currentData().toString());
		if (index < 0)
			index = 0;
		audioSourceCombo->setCurrentIndex(index);
	}

	{
		const QSignalBlocker blocker(audioTrackCombo);
		int track = obs_data_get_int(settings, "audio_track");
		if (track < 1 || track > 6)
			track = 2;
		int index = audioTrackCombo->findData(track);
		if (index < 0)
			index = audioTrackCombo->findData(2);
		if (index < 0)
			index = 0;
		audioTrackCombo->setCurrentIndex(index);
	}

	savedSourceUuid = obs_data_get_string(settings, "source_uuid");
	SelectSavedSource();
	UpdateUi();
}

void RecordSourceDock::PopulateSources()
{
	const QString previous = sourceCombo->currentData().toString();

	std::vector<SourceEntry> sources;
	obs_enum_sources(EnumSource, &sources);

	std::sort(sources.begin(), sources.end(), [](const SourceEntry &a, const SourceEntry &b) {
		return QString::localeAwareCompare(a.name, b.name) < 0;
	});

	const QSignalBlocker blocker(sourceCombo);
	sourceCombo->clear();

	for (const SourceEntry &entry : sources) {
		sourceCombo->addItem(entry.name, entry.uuid);
		sourceCombo->setItemData(sourceCombo->count() - 1, entry.uuid,
					Qt::ToolTipRole);
	}

	int index = sourceCombo->findData(previous);
	if (index < 0)
		index = 0;

	if (sourceCombo->count() > 0)
		sourceCombo->setCurrentIndex(index);
}

void RecordSourceDock::PopulateEncoders()
{
	const auto fill = [this](QComboBox *combo, enum obs_encoder_type type) {
		const QString current = combo->currentData().toString();
		const QString saved = ToQString(obs_data_get_string(settings, EncoderKey(type).toUtf8().constData()));

		const QSignalBlocker blocker(combo);
		combo->clear();

		for (const auto &encoder : record_source::ListEncoders(type)) {
			combo->addItem(ToQString(encoder.displayName), ToQString(encoder.id));
		}

		QString preferred = current;
		if (preferred.isEmpty())
			preferred = saved;
		if (preferred.isEmpty())
			preferred = PreferredEncoder(type);

		int index = combo->findData(preferred);
		if (index < 0 && combo->count() > 0)
			index = 0;

		if (index >= 0)
			combo->setCurrentIndex(index);

		combo->setProperty("currentId", combo->currentData().toString());
	};

	fill(videoEncoderCombo, OBS_ENCODER_VIDEO);
	fill(audioEncoderCombo, OBS_ENCODER_AUDIO);
}

void RecordSourceDock::PopulateAudioSources()
{
	const QString previous = audioSourceCombo->currentData().toString();
	const QString videoUuid = sourceCombo->currentData().toString();

	std::vector<SourceEntry> audioSources;
	obs_enum_sources(EnumAudioSource, &audioSources);

	std::vector<SourceEntry> filtered = audioSources;

	std::sort(filtered.begin(), filtered.end(), [](const SourceEntry &a, const SourceEntry &b) {
		return QString::localeAwareCompare(a.name, b.name) < 0;
	});

	const QSignalBlocker blocker(audioSourceCombo);
	audioSourceCombo->clear();
	audioSourceCombo->addItem(Tr("Dock.AudioSource.Silence"), QStringLiteral(""));

	for (const SourceEntry &entry : filtered) {
		audioSourceCombo->addItem(entry.name, entry.uuid);
	}

	int index = audioSourceCombo->findData(previous);
	if (index < 0 && !videoUuid.isEmpty())
		index = audioSourceCombo->findData(videoUuid);
	if (index < 0)
		index = 0;

	if (audioSourceCombo->count() > 0)
		audioSourceCombo->setCurrentIndex(index);
}

void RecordSourceDock::SelectSavedSource()
{
	if (sourceCombo->count() == 0) {
		UpdateUi();
		return;
	}

	int index = -1;
	if (!savedSourceUuid.empty())
		index = sourceCombo->findData(ToQString(savedSourceUuid));

	if (index < 0)
		index = 0;

	const QSignalBlocker blocker(sourceCombo);
	sourceCombo->setCurrentIndex(index);
	savedSourceUuid = sourceCombo->currentData().toString().toStdString();
}

void RecordSourceDock::UpdateSplitControls()
{
	const QString mode = splitModeCombo->currentData().toString();
	const bool byTime = mode == QStringLiteral("time");
	const bool bySize = mode == QStringLiteral("size");

	const QSignalBlocker blocker(splitValueSpin);

	if (byTime) {
		splitValueSpin->setRange(1, 525600);
		splitValueSpin->setSuffix(QStringLiteral(" min"));
		splitValueSpin->setValue(std::clamp(splitTimeMinutes, 1, 525600));
		splitTimeMinutes = splitValueSpin->value();
		splitValueSpin->setToolTip(Tr("Dock.Split.Time.Tooltip"));
	} else if (bySize) {
		splitValueSpin->setRange(20, 1048576);
		splitValueSpin->setSuffix(QStringLiteral(" MB"));
		splitValueSpin->setValue(std::clamp(splitSizeMb, 20, 1048576));
		splitSizeMb = splitValueSpin->value();
		splitValueSpin->setToolTip(Tr("Dock.Split.Size.Tooltip"));
	} else {
		splitValueSpin->setRange(0, 0);
		splitValueSpin->setValue(0);
		splitValueSpin->setSuffix(QString());
		splitValueSpin->setToolTip(Tr("Dock.Split.NoSplit.Tooltip"));
	}
}

void RecordSourceDock::UpdateUi()
{
	if (!uiBuilt)
		return;

	const bool active = controller.active();
	const bool stopping = controller.stopping();

	sourceCombo->setEnabled(!active);
	refreshSourcesButton->setEnabled(!active);
	directoryEdit->setEnabled(!active);
	browseButton->setEnabled(!active);
	formatCombo->setEnabled(!active);
	splitModeCombo->setEnabled(!active);
	const QString splitMode = splitModeCombo->currentData().toString();
	splitValueSpin->setEnabled(!active && !splitMode.isEmpty());
	colorFormatCombo->setEnabled(!active);
	colorSpaceCombo->setEnabled(!active);
	colorRangeCombo->setEnabled(!active);
	videoEncoderCombo->setEnabled(!active);
	audioEncoderCombo->setEnabled(!active);
	videoConfigButton->setEnabled(!active && videoEncoderCombo->count() > 0);
	audioConfigButton->setEnabled(!active && audioEncoderCombo->count() > 0);
	startButton->setEnabled(!active && sourceCombo->count() > 0);
	stopButton->setEnabled(active && !stopping);
	openFolderButton->setEnabled(!active || !controller.OutputPath().empty());

	if (active) {
		durationLabel->setText(Tr("Dock.Duration").arg(FormatDuration(controller.DurationMs())));
		sizeLabel->setText(Tr("Dock.Size").arg(FormatBytes(controller.Bytes())));
		fileLabel->setText(Tr("Dock.File").arg(ToQString(controller.OutputPath())));

		if (stopping) {
			statusLabel->setText(Tr("Dock.Status.Stopping"));
		} else {
			statusLabel->setText(Tr("Dock.Status.Recording").arg(ToQString(controller.SourceName())));
		}
		return;
	}

	if (wasRecording) {
		wasRecording = false;
		lastFile = controller.OutputPath();
		durationLabel->setText(
			Tr("Dock.Duration").arg(FormatDuration(controller.DurationMs())));
		sizeLabel->setText(Tr("Dock.Size").arg(FormatBytes(controller.Bytes())));
		if (!lastFile.empty())
			fileLabel->setText(Tr("Dock.File").arg(ToQString(lastFile)));

		const std::string error = controller.LastError();
		if (!error.empty()) {
			statusLabel->setText(Tr("Dock.Status.StoppedWithError").arg(ToQString(error)));
		} else {
			statusLabel->setText(Tr("Dock.Status.Finished"));
		}
	}

	if (sourceCombo->count() == 0) {
		statusLabel->setText(Tr("Dock.Status.NoSources"));
	} else if (statusLabel->text() == Tr("Dock.Status.NoSources")) {
		statusLabel->setText(Tr("Dock.Status.Ready"));
	}
}

void RecordSourceDock::StartRecording()
{
	if (controller.active())
		return;

	if (sourceCombo->count() == 0) {
		statusLabel->setText(Tr("Dock.Status.SelectSource"));
		return;
	}

	std::string directory = directoryEdit->text().trimmed().toStdString();
	if (directory.empty()) {
		statusLabel->setText(Tr("Dock.Status.SelectDirectory"));
		return;
	}

	const QString videoEncoder = videoEncoderCombo->currentData().toString();
	const QString audioEncoder = audioEncoderCombo->currentData().toString();
	if (videoEncoder.isEmpty() || audioEncoder.isEmpty()) {
		statusLabel->setText(Tr("Dock.Status.SelectEncoders"));
		return;
	}

	SaveSettings();

	RecordController::Config config;
	config.sourceUuid = CurrentSourceUuid();
	config.directory = directory;
	config.format = ChosenFormat();
	config.colorFormat = ColorFormatFromKey(colorFormatCombo->currentData().toString());
	config.colorSpace = ColorSpaceFromKey(colorSpaceCombo->currentData().toString());
	config.colorRange = ColorRangeFromKey(colorRangeCombo->currentData().toString());
	config.videoEncoderId = videoEncoder.toStdString();
	config.audioEncoderId = audioEncoder.toStdString();
	config.audioSourceUuid = audioSourceCombo->currentData().toString().toStdString();
	config.audioTrack = audioTrackCombo->currentData().toInt();
	config.splitMode = splitModeCombo->currentData().toString().toStdString();
	config.splitTimeMinutes = splitTimeMinutes;
	config.splitSizeMb = splitSizeMb;
	config.videoSettings = videoSettings;
	config.audioSettings = audioSettings;

	std::string error;
	if (!controller.Start(config, error)) {
		statusLabel->setText(Tr("Dock.Status.StartFailed").arg(ToQString(error)));
		LogError("Could not start recording: " + error);
		UpdateUi();
		return;
	}

	wasRecording = true;
	lastFile.clear();
	durationLabel->clear();
	sizeLabel->clear();
	fileLabel->clear();
	statusLabel->setText(Tr("Dock.Status.Starting"));
	UpdateUi();
}

void RecordSourceDock::StopRecording()
{
	if (!controller.active())
		return;

	SaveSettings();
	controller.RequestStop();
	statusLabel->setText(Tr("Dock.Status.Stopping"));
	UpdateUi();
}

void RecordSourceDock::BrowseDirectory()
{
	const QString directory = QFileDialog::getExistingDirectory(
		this, Tr("Dock.Dialog.SelectRecordingFolder"), directoryEdit->text());

	if (directory.isEmpty())
		return;

	directoryEdit->setText(QDir::toNativeSeparators(directory));
	SaveSettings();
	UpdateUi();
}

void RecordSourceDock::ConfigureVideoEncoder()
{
	const QString encoderId = videoEncoderCombo->currentData().toString();
	if (encoderId.isEmpty())
		return;

	EncoderPropertiesDialog dialog(encoderId.toStdString(), OBS_ENCODER_VIDEO, videoSettings, this);
	if (dialog.exec() != QDialog::Accepted)
		return;

	obs_data_t *newSettings = dialog.TakeSettings();
	if (videoSettings)
		obs_data_release(videoSettings);
	videoSettings = newSettings;

	StoreEncoderSettings(settings, VIDEO_SETTINGS_GROUP, encoderId.toUtf8().constData(),
			     videoSettings);
	SaveSettings();
}

void RecordSourceDock::ConfigureAudioEncoder()
{
	const QString encoderId = audioEncoderCombo->currentData().toString();
	if (encoderId.isEmpty())
		return;

	EncoderPropertiesDialog dialog(encoderId.toStdString(), OBS_ENCODER_AUDIO, audioSettings, this);
	if (dialog.exec() != QDialog::Accepted)
		return;

	obs_data_t *newSettings = dialog.TakeSettings();
	if (audioSettings)
		obs_data_release(audioSettings);
	audioSettings = newSettings;

	StoreEncoderSettings(settings, AUDIO_SETTINGS_GROUP, encoderId.toUtf8().constData(),
			     audioSettings);
	SaveSettings();
}

void RecordSourceDock::OpenOutputFolder()
{
	QString target;
	if (!lastFile.empty())
		target = ToQString(lastFile);
	else if (!controller.OutputPath().empty())
		target = ToQString(controller.OutputPath());
	else
		target = directoryEdit->text();

	if (target.isEmpty()) {
		statusLabel->setText(Tr("Dock.Status.NoFolderYet"));
		return;
	}

	const QFileInfo info(target);
	QString directory = info.isDir() ? info.absoluteFilePath() : info.absolutePath();

	if (directory.isEmpty() || !QDir(directory).exists()) {
		statusLabel->setText(Tr("Dock.Status.FolderMissing"));
		return;
	}

	QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
}

void RecordSourceDock::SaveSettings()
{
	if (!uiBuilt || !settings)
		return;

	const std::string sourceUuid = CurrentSourceUuid();
	const std::string directory = directoryEdit->text().toUtf8().toStdString();
	const std::string format = ChosenFormat();
	const std::string videoEncoder = videoEncoderCombo->currentData().toString().toStdString();
	const std::string audioEncoder = audioEncoderCombo->currentData().toString().toStdString();

	obs_data_set_string(settings, "source_uuid", sourceUuid.c_str());
	obs_data_set_string(settings, "directory", directory.c_str());
	obs_data_set_string(settings, "format", format.c_str());
	obs_data_set_string(settings, "color_format",
			    colorFormatCombo->currentData().toString().toUtf8().constData());
	obs_data_set_string(settings, "color_space",
			    colorSpaceCombo->currentData().toString().toUtf8().constData());
	obs_data_set_string(settings, "color_range",
			    colorRangeCombo->currentData().toString().toUtf8().constData());
	obs_data_set_string(settings, "split_mode",
			    splitModeCombo->currentData().toString().toUtf8().constData());
	obs_data_set_int(settings, "split_time_min", splitTimeMinutes);
	obs_data_set_int(settings, "split_size_mb", splitSizeMb);
	obs_data_set_string(settings, "video_encoder", videoEncoder.c_str());
	obs_data_set_string(settings, "audio_encoder", audioEncoder.c_str());
	obs_data_set_string(settings, "audio_source_uuid",
			    audioSourceCombo->currentData().toString().toUtf8().constData());
	obs_data_set_int(settings, "audio_track", audioTrackCombo->currentData().toInt());

	if (videoSettings)
		StoreEncoderSettings(settings, VIDEO_SETTINGS_GROUP, videoEncoder.c_str(), videoSettings);
	if (audioSettings)
		StoreEncoderSettings(settings, AUDIO_SETTINGS_GROUP, audioEncoder.c_str(), audioSettings);

	const std::string path = ConfigFilePath();
	if (path.empty())
		return;

	EnsureParentDirectory(path);
	obs_data_save_json_safe(settings, path.c_str(), "tmp", "bak");
}

void RecordSourceDock::Shutdown()
{
	if (!uiBuilt)
		return;

	SaveSettings();
	uiBuilt = false;
	controller.Shutdown();

	if (timer)
		timer->stop();
}

void RecordSourceDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	if (!uiBuilt)
		return;

	// Sources can be added or removed while OBS is running, so refresh both
	// lists whenever the dock becomes visible again.
	PopulateSources();
	PopulateAudioSources();
	UpdateUi();
}

void RecordSourceDock::HandleFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		PopulateSources();
		SelectSavedSource();
		PopulateAudioSources();
		UpdateUi();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		PopulateSources();
		SelectSavedSource();
		PopulateAudioSources();
		UpdateUi();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		if (controller.active())
			StopRecording();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		Shutdown();
		break;
	default:
		break;
	}
}

std::string RecordSourceDock::ChosenFormat() const
{
	const QString format = formatCombo->currentData().toString();
	return format.isEmpty() ? std::string("mkv") : format.toStdString();
}

std::string RecordSourceDock::CurrentSourceUuid() const
{
	const QString uuid = sourceCombo->currentData().toString();
	return uuid.toStdString();
}
