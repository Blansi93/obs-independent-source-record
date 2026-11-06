#pragma once

#include <obs.h>

#include <QDialog>
#include <string>
#include <vector>

class QFormLayout;
class QLabel;
class QScrollArea;
class QVBoxLayout;
class QWidget;

// A small property editor that renders the exact obs_properties_t exposed by an
// OBS encoder (the same controls OBS shows in Settings -> Output). It supports
// the property types used by OBS' video and audio encoders (bool, int, float,
// text, path, list, color and button) and keeps the visibility rules of the
// encoder up to date as values change.
class EncoderPropertiesDialog : public QDialog {
	Q_OBJECT

public:
	EncoderPropertiesDialog(const std::string &encoderId, enum obs_encoder_type type,
				obs_data_t *currentSettings, QWidget *parent = nullptr);
	~EncoderPropertiesDialog() override;

	// Returns a new reference to the edited settings.
	obs_data_t *TakeSettings();

private:
	void BuildForm();
	void RebuildForm();
	void UpdateRows();
	void ResetDefaults();
	void OnChanged(obs_property_t *property, bool rebuild);

	void AddProperties(obs_properties_t *properties, QVBoxLayout *layout, int depth);
	bool AddProperty(obs_property_t *property, QVBoxLayout *layout, int depth);
	void AddBool(obs_property_t *property, QWidget *row);
	void AddInt(obs_property_t *property, QWidget *row);
	void AddFloat(obs_property_t *property, QWidget *row);
	void AddText(obs_property_t *property, QWidget *row);
	void AddPath(obs_property_t *property, QWidget *row);
	void AddList(obs_property_t *property, QWidget *row);
	void AddColor(obs_property_t *property, QWidget *row);
	void AddButton(obs_property_t *property, QWidget *row);
	void AddEditableList(obs_property_t *property, QWidget *row);

	struct Row {
		obs_property_t *property = nullptr;
		QWidget *widget = nullptr;
		QLabel *label = nullptr;
		bool header = false;
	};

	std::string encoderId;
	enum obs_encoder_type encoderType;

	obs_properties_t *properties = nullptr;
	obs_data_t *settings = nullptr;
	obs_data_t *defaults = nullptr;

	QScrollArea *scrollArea = nullptr;
	QWidget *formHost = nullptr;
	QVBoxLayout *formLayout = nullptr;
	std::vector<Row> rows;

	bool rebuilding = false;
};
