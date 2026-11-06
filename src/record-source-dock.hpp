#pragma once

#include <obs.h>

#include <QWidget>

#include "record-controller.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTimer;

class RecordSourceDock : public QWidget {
	Q_OBJECT

public:
	explicit RecordSourceDock(QWidget *parent = nullptr);
	~RecordSourceDock() override;

	void SaveSettings();
	void Shutdown();
	void HandleFrontendEvent(enum obs_frontend_event event);

private:
	void showEvent(QShowEvent *event) override;
	void BuildUi();
	void LoadSettings();
	void PopulateSources();
	void PopulateEncoders();
	void PopulateAudioSources();
	void SelectSavedSource();
	void UpdateSplitControls();
	void UpdateUi();
	void StartRecording();
	void StopRecording();
	void BrowseDirectory();
	void ConfigureVideoEncoder();
	void ConfigureAudioEncoder();
	void OpenOutputFolder();

	std::string ChosenFormat() const;
	std::string CurrentSourceUuid() const;

	RecordController controller;

	QComboBox *sourceCombo = nullptr;
	QPushButton *refreshSourcesButton = nullptr;
	QLineEdit *directoryEdit = nullptr;
	QPushButton *browseButton = nullptr;
	QComboBox *formatCombo = nullptr;
	QComboBox *splitModeCombo = nullptr;
	QSpinBox *splitValueSpin = nullptr;
	QComboBox *colorFormatCombo = nullptr;
	QComboBox *colorSpaceCombo = nullptr;
	QComboBox *colorRangeCombo = nullptr;
	QComboBox *videoEncoderCombo = nullptr;
	QPushButton *videoConfigButton = nullptr;
	QComboBox *audioEncoderCombo = nullptr;
	QPushButton *audioConfigButton = nullptr;
	QComboBox *audioSourceCombo = nullptr;
	QComboBox *audioTrackCombo = nullptr;
	QPushButton *startButton = nullptr;
	QPushButton *stopButton = nullptr;
	QPushButton *openFolderButton = nullptr;
	QLabel *statusLabel = nullptr;
	QLabel *durationLabel = nullptr;
	QLabel *sizeLabel = nullptr;
	QLabel *fileLabel = nullptr;
	QTimer *timer = nullptr;

	obs_data_t *settings = nullptr;
	obs_data_t *videoSettings = nullptr;
	obs_data_t *audioSettings = nullptr;

	std::string savedSourceUuid;
	std::string lastFile;
	int splitTimeMinutes = 15;
	int splitSizeMb = 2048;
	bool wasRecording = false;
	bool uiBuilt = false;
};
