#include "encoder-properties-dialog.hpp"

#include "encoder-utils.hpp"
#include "localization.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstring>

namespace {

QString ToQString(const char *text)
{
	return text ? QString::fromUtf8(text) : QString();
}

QString PropertyLabel(obs_property_t *property)
{
	const char *description = obs_property_description(property);
	const char *name = obs_property_name(property);
	if (description && *description)
		return QString::fromUtf8(description);
	if (name && *name)
		return QString::fromUtf8(name);
	return QStringLiteral("—");
}

void ApplyTooltip(QWidget *widget, obs_property_t *property)
{
	const char *description = obs_property_long_description(property);
	if (description && *description)
		widget->setToolTip(QString::fromUtf8(description));
}

QHBoxLayout *RowLayout(QWidget *row)
{
	return qobject_cast<QHBoxLayout *>(row->layout());
}

obs_data_array_t *StringListToArray(const QString &text)
{
	obs_data_array_t *array = obs_data_array_create();
	const QStringList parts = text.split(',', Qt::SkipEmptyParts);
	for (const QString &part : parts) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", part.trimmed().toUtf8().constData());
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	return array;
}

QString ArrayToStringList(obs_data_t *settings, const char *name)
{
	obs_data_array_t *array = obs_data_get_array(settings, name);
	if (!array)
		return {};

	QStringList parts;
	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *value = obs_data_get_string(item, "value");
		if (value && *value)
			parts << QString::fromUtf8(value);
		obs_data_release(item);
	}
	obs_data_array_release(array);
	return parts.join(", ");
}

} // namespace

EncoderPropertiesDialog::EncoderPropertiesDialog(const std::string &id, enum obs_encoder_type type,
						 obs_data_t *currentSettings, QWidget *parent)
	: QDialog(parent), encoderId(id), encoderType(type)
{
	setWindowTitle(Tr("Encoder.Title")
			       .arg(ToQString(record_source::GetEncoderDisplayName(id.c_str()).c_str())));
	setModal(true);
	resize(620, 640);

	defaults = record_source::CreateEncoderDefaults(id.c_str(), type);
	settings = obs_data_create();
	obs_data_apply(settings, defaults);
	if (currentSettings)
		obs_data_apply(settings, currentSettings);

	properties = obs_get_encoder_properties(id.c_str());
	if (properties)
		obs_properties_apply_settings(properties, settings);

	auto *mainLayout = new QVBoxLayout(this);

	scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	mainLayout->addWidget(scrollArea, 1);

	auto *buttons = new QDialogButtonBox(this);
	QPushButton *resetButton =
		buttons->addButton(Tr("Encoder.Reset"), QDialogButtonBox::ResetRole);
	buttons->addButton(QDialogButtonBox::Ok);
	buttons->addButton(QDialogButtonBox::Cancel);
	mainLayout->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(resetButton, &QPushButton::clicked, this, &EncoderPropertiesDialog::ResetDefaults);

	BuildForm();
}

EncoderPropertiesDialog::~EncoderPropertiesDialog()
{
	if (properties)
		obs_properties_destroy(properties);
	if (settings)
		obs_data_release(settings);
	if (defaults)
		obs_data_release(defaults);
}

obs_data_t *EncoderPropertiesDialog::TakeSettings()
{
	obs_data_t *result = obs_data_create();
	obs_data_apply(result, settings);
	return result;
}

void EncoderPropertiesDialog::BuildForm()
{
	rebuilding = true;

	formHost = new QWidget(scrollArea);
	formLayout = new QVBoxLayout(formHost);
	formLayout->setContentsMargins(6, 6, 6, 6);
	formLayout->setSpacing(6);

	rows.clear();

	if (!properties) {
		auto *label = new QLabel(Tr("Encoder.NoSettings"), formHost);
		label->setWordWrap(true);
		formLayout->addWidget(label);
	} else {
		AddProperties(properties, formLayout, 0);
	}

	formLayout->addStretch(1);
	scrollArea->setWidget(formHost);
	rebuilding = false;

	UpdateRows();
}

void EncoderPropertiesDialog::RebuildForm()
{
	if (rebuilding)
		return;
	if (properties)
		obs_properties_apply_settings(properties, settings);
	BuildForm();
}

void EncoderPropertiesDialog::AddProperties(obs_properties_t *props, QVBoxLayout *layout, int depth)
{
	obs_property_t *property = obs_properties_first(props);
	while (property) {
		AddProperty(property, layout, depth);
		if (!obs_property_next(&property))
			break;
	}
}

bool EncoderPropertiesDialog::AddProperty(obs_property_t *property, QVBoxLayout *layout, int depth)
{
	const enum obs_property_type type = obs_property_get_type(property);
	if (type == OBS_PROPERTY_INVALID)
		return false;

	if (type == OBS_PROPERTY_GROUP) {
		const char *description = obs_property_description(property);
		if (description && *description) {
			auto *header = new QLabel(QString::fromUtf8(description), formHost);
			QFont font = header->font();
			font.setBold(true);
			header->setFont(font);
			header->setContentsMargins(depth * 12, 6, 0, 0);
			layout->addWidget(header);
			rows.push_back({property, header, nullptr, true});
		}

		obs_properties_t *content = obs_property_group_content(property);
		if (content)
			AddProperties(content, layout, depth + 1);
		return true;
	}

	auto *row = new QWidget(formHost);
	auto *rowLayout = new QHBoxLayout(row);
	rowLayout->setContentsMargins(depth * 12, 0, 0, 0);
	rowLayout->setSpacing(10);

	auto *label = new QLabel(PropertyLabel(property), row);
	label->setWordWrap(true);
	label->setMinimumWidth(150);
	label->setMaximumWidth(230);
	ApplyTooltip(label, property);
	rowLayout->addWidget(label);

	layout->addWidget(row);
	rows.push_back({property, row, label, false});

	switch (type) {
	case OBS_PROPERTY_BOOL:
		AddBool(property, row);
		break;
	case OBS_PROPERTY_INT:
		AddInt(property, row);
		break;
	case OBS_PROPERTY_FLOAT:
		AddFloat(property, row);
		break;
	case OBS_PROPERTY_TEXT:
		AddText(property, row);
		break;
	case OBS_PROPERTY_PATH:
		AddPath(property, row);
		break;
	case OBS_PROPERTY_LIST:
		AddList(property, row);
		break;
	case OBS_PROPERTY_COLOR:
	case OBS_PROPERTY_COLOR_ALPHA:
		AddColor(property, row);
		break;
	case OBS_PROPERTY_BUTTON:
		AddButton(property, row);
		break;
	case OBS_PROPERTY_EDITABLE_LIST:
		AddEditableList(property, row);
		break;
	default: {
		auto *unsupported =
			new QLabel(Tr("Encoder.Unsupported").arg((int)type), row);
		unsupported->setEnabled(false);
		RowLayout(row)->addWidget(unsupported, 1);
		break;
	}
	}

	return true;
}

void EncoderPropertiesDialog::AddBool(obs_property_t *property, QWidget *row)
{
	const char *name = obs_property_name(property);
	auto *check = new QCheckBox(row);
	check->setChecked(obs_data_get_bool(settings, name));
	ApplyTooltip(check, property);
	RowLayout(row)->addWidget(check, 1);

	connect(check, &QCheckBox::toggled, this, [this, property, name](bool checked) {
		if (rebuilding)
			return;
		obs_data_set_bool(settings, name, checked);
		OnChanged(property, true);
	});
}

void EncoderPropertiesDialog::AddInt(obs_property_t *property, QWidget *row)
{
	const char *name = obs_property_name(property);
	auto *spin = new QSpinBox(row);
	spin->setRange(obs_property_int_min(property), obs_property_int_max(property));
	spin->setSingleStep(std::max(1, obs_property_int_step(property)));
	spin->setKeyboardTracking(false);
	const char *suffix = obs_property_int_suffix(property);
	if (suffix && *suffix)
		spin->setSuffix(QString::fromUtf8(suffix));
	spin->setValue((int)obs_data_get_int(settings, name));
	ApplyTooltip(spin, property);
	RowLayout(row)->addWidget(spin, 1);

	connect(spin, &QSpinBox::valueChanged, this,
		[this, property, name](int value) {
			if (rebuilding)
				return;
			obs_data_set_int(settings, name, value);
			OnChanged(property, false);
		});
}

void EncoderPropertiesDialog::AddFloat(obs_property_t *property, QWidget *row)
{
	const char *name = obs_property_name(property);
	auto *spin = new QDoubleSpinBox(row);
	spin->setRange(obs_property_float_min(property), obs_property_float_max(property));
	const double step = obs_property_float_step(property);
	spin->setSingleStep(step > 0.0 ? step : 0.1);
	spin->setDecimals(step > 0.0 && step < 0.01 ? 4 : (step < 0.1 ? 3 : 2));
	spin->setKeyboardTracking(false);
	const char *suffix = obs_property_float_suffix(property);
	if (suffix && *suffix)
		spin->setSuffix(QString::fromUtf8(suffix));
	spin->setValue(obs_data_get_double(settings, name));
	ApplyTooltip(spin, property);
	RowLayout(row)->addWidget(spin, 1);

	connect(spin, &QDoubleSpinBox::valueChanged, this,
		[this, property, name](double value) {
			if (rebuilding)
				return;
			obs_data_set_double(settings, name, value);
			OnChanged(property, false);
		});
}

void EncoderPropertiesDialog::AddText(obs_property_t *property, QWidget *row)
{
	const char *name = obs_property_name(property);
	const enum obs_text_type textType = obs_property_text_type(property);

	if (textType == OBS_TEXT_INFO) {
		auto *info = new QLabel(QString::fromUtf8(obs_data_get_string(settings, name)), row);
		info->setWordWrap(obs_property_text_info_word_wrap(property));
		info->setTextInteractionFlags(Qt::TextSelectableByMouse);

		const enum obs_text_info_type infoType = obs_property_text_info_type(property);
		if (infoType == OBS_TEXT_INFO_WARNING)
			info->setStyleSheet(QStringLiteral("color: #c08000;"));
		else if (infoType == OBS_TEXT_INFO_ERROR)
			info->setStyleSheet(QStringLiteral("color: #c00000;"));
		else
			info->setStyleSheet(QStringLiteral("color: palette(mid);"));

		RowLayout(row)->addWidget(info, 1);
		return;
	}

	if (textType == OBS_TEXT_MULTILINE) {
		auto *edit = new QPlainTextEdit(row);
		edit->setPlainText(QString::fromUtf8(obs_data_get_string(settings, name)));
		edit->setMinimumHeight(90);
		ApplyTooltip(edit, property);
		RowLayout(row)->addWidget(edit, 1);

		connect(edit, &QPlainTextEdit::textChanged, this, [this, property, name, edit]() {
			if (rebuilding)
				return;
			obs_data_set_string(settings, name, edit->toPlainText().toUtf8().constData());
			OnChanged(property, false);
		});
		return;
	}

	auto *edit = new QLineEdit(row);
	edit->setText(QString::fromUtf8(obs_data_get_string(settings, name)));
	if (textType == OBS_TEXT_PASSWORD)
		edit->setEchoMode(QLineEdit::Password);
	ApplyTooltip(edit, property);
	RowLayout(row)->addWidget(edit, 1);

	connect(edit, &QLineEdit::textChanged, this, [this, property, name, edit]() {
		if (rebuilding)
			return;
		obs_data_set_string(settings, name, edit->text().toUtf8().constData());
		OnChanged(property, false);
	});
}

void EncoderPropertiesDialog::AddPath(obs_property_t *property, QWidget *row)
{
	const char *name = obs_property_name(property);
	auto *container = new QWidget(row);
	auto *layout = new QHBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	auto *edit = new QLineEdit(container);
	edit->setText(QString::fromUtf8(obs_data_get_string(settings, name)));
	ApplyTooltip(edit, property);
	layout->addWidget(edit, 1);

	auto *browse = new QPushButton(QStringLiteral("..."), container);
	browse->setFixedWidth(34);
	layout->addWidget(browse);
	RowLayout(row)->addWidget(container, 1);

	connect(edit, &QLineEdit::textChanged, this, [this, property, name, edit]() {
		if (rebuilding)
			return;
		obs_data_set_string(settings, name, edit->text().toUtf8().constData());
		OnChanged(property, false);
	});

	connect(browse, &QPushButton::clicked, this, [this, property, name, edit]() {
		const enum obs_path_type pathType = obs_property_path_type(property);
		const QString filter = ToQString(obs_property_path_filter(property));
		const QString start = ToQString(obs_property_path_default_path(property));
		QString selected;
		if (pathType == OBS_PATH_DIRECTORY) {
			selected = QFileDialog::getExistingDirectory(this, Tr("Encoder.SelectFolder"),
								     edit->text().isEmpty() ? start : edit->text());
		} else if (pathType == OBS_PATH_FILE_SAVE) {
			selected = QFileDialog::getSaveFileName(this, Tr("Encoder.SelectFile"),
							       edit->text().isEmpty() ? start : edit->text(),
							       filter);
		} else {
			selected = QFileDialog::getOpenFileName(this, Tr("Encoder.SelectFile"),
							       edit->text().isEmpty() ? start : edit->text(),
							       filter);
		}
		if (!selected.isEmpty())
			edit->setText(selected);
	});
}

void EncoderPropertiesDialog::AddList(obs_property_t *property, QWidget *row)
{
	const char *name = obs_property_name(property);
	const enum obs_combo_format format = obs_property_list_format(property);
	const size_t count = obs_property_list_item_count(property);

	auto *combo = new QComboBox(row);
	combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

	int selectedIndex = 0;
	const char *currentString = obs_data_get_string(settings, name);
	const long long currentInt = obs_data_get_int(settings, name);
	const double currentFloat = obs_data_get_double(settings, name);
	const bool currentBool = obs_data_get_bool(settings, name);

	for (size_t i = 0; i < count; ++i) {
		const char *itemName = obs_property_list_item_name(property, i);
		combo->addItem(itemName ? QString::fromUtf8(itemName) : QString::number((int)i));

		bool selected = false;
		switch (format) {
		case OBS_COMBO_FORMAT_STRING:
			selected = currentString &&
				   strcmp(currentString, obs_property_list_item_string(property, i)) == 0;
			break;
		case OBS_COMBO_FORMAT_INT:
			selected = currentInt == obs_property_list_item_int(property, i);
			break;
		case OBS_COMBO_FORMAT_FLOAT:
			selected = std::abs(currentFloat - obs_property_list_item_float(property, i)) < 1e-9;
			break;
		case OBS_COMBO_FORMAT_BOOL:
			selected = currentBool == obs_property_list_item_bool(property, i);
			break;
		default:
			break;
		}
		if (selected)
			selectedIndex = (int)i;

		if (obs_property_list_item_disabled(property, i))
			combo->setItemData((int)i, QVariant(), Qt::UserRole - 1);
	}

	combo->setCurrentIndex(selectedIndex);

	const enum obs_combo_type comboType = obs_property_list_type(property);
	if (comboType == OBS_COMBO_TYPE_EDITABLE && format == OBS_COMBO_FORMAT_STRING) {
		combo->setEditable(true);
		combo->setCurrentText(ToQString(currentString));
	}

	ApplyTooltip(combo, property);
	RowLayout(row)->addWidget(combo, 1);

	connect(combo, &QComboBox::currentIndexChanged, this,
		[this, property, name, format, combo](int index) {
			if (rebuilding || index < 0)
				return;

			switch (format) {
			case OBS_COMBO_FORMAT_STRING: {
				const char *value = obs_property_list_item_string(property, (size_t)index);
				if (combo->isEditable())
					value = combo->currentText().toUtf8().constData();
				obs_data_set_string(settings, name, value ? value : "");
				break;
			}
			case OBS_COMBO_FORMAT_INT:
				obs_data_set_int(settings, name,
						 obs_property_list_item_int(property, (size_t)index));
				break;
			case OBS_COMBO_FORMAT_FLOAT:
				obs_data_set_double(settings, name,
						    obs_property_list_item_float(property, (size_t)index));
				break;
			case OBS_COMBO_FORMAT_BOOL:
				obs_data_set_bool(settings, name,
						  obs_property_list_item_bool(property, (size_t)index));
				break;
			default:
				break;
			}
			OnChanged(property, true);
		});
}

void EncoderPropertiesDialog::AddColor(obs_property_t *property, QWidget *row)
{
	const char *name = obs_property_name(property);
	auto *button = new QPushButton(row);
	button->setMinimumWidth(90);

	auto updateButton = [button](uint32_t value) {
		QColor color = QColor::fromRgb(value & 0xFFFFFF);
		button->setText(color.name().toUpper());
		button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
					      .arg(color.name())
					      .arg(color.lightness() < 128 ? "white" : "black"));
	};

	updateButton((uint32_t)obs_data_get_int(settings, name));
	RowLayout(row)->addWidget(button, 1);

	connect(button, &QPushButton::clicked, this, [this, property, name, updateButton, button]() {
		const QColor initial = QColor::fromRgb((uint32_t)obs_data_get_int(settings, name) & 0xFFFFFF);
		const QColor color = QColorDialog::getColor(initial, this, Tr("Encoder.SelectColor"));
		if (!color.isValid())
			return;
		obs_data_set_int(settings, name, (long long)color.rgb());
		updateButton((uint32_t)color.rgb());
		OnChanged(property, false);
	});
}

void EncoderPropertiesDialog::AddButton(obs_property_t *property, QWidget *row)
{
	auto *button = new QPushButton(PropertyLabel(property), row);
	ApplyTooltip(button, property);
	RowLayout(row)->addWidget(button, 1);

	connect(button, &QPushButton::clicked, this, [this, property]() {
		obs_property_button_clicked(property, nullptr);
		QTimer::singleShot(0, this, &EncoderPropertiesDialog::RebuildForm);
	});
}

void EncoderPropertiesDialog::AddEditableList(obs_property_t *property, QWidget *row)
{
	const char *name = obs_property_name(property);
	auto *edit = new QLineEdit(row);
	edit->setPlaceholderText(Tr("Encoder.CommaSeparated"));
	edit->setText(ArrayToStringList(settings, name));
	ApplyTooltip(edit, property);
	RowLayout(row)->addWidget(edit, 1);

	connect(edit, &QLineEdit::editingFinished, this, [this, property, name, edit]() {
		if (rebuilding)
			return;
		obs_data_array_t *array = StringListToArray(edit->text());
		obs_data_set_array(settings, name, array);
		obs_data_array_release(array);
		OnChanged(property, false);
	});
}

void EncoderPropertiesDialog::UpdateRows()
{
	for (const Row &row : rows) {
		if (!row.property || !row.widget)
			continue;
		const bool visible = obs_property_visible(row.property);
		row.widget->setVisible(visible);
		row.widget->setEnabled(obs_property_enabled(row.property));
		if (row.label)
			row.label->setVisible(visible);
	}
}

void EncoderPropertiesDialog::ResetDefaults()
{
	obs_data_clear(settings);
	obs_data_apply(settings, defaults);
	if (properties)
		obs_properties_apply_settings(properties, settings);
	BuildForm();
}

void EncoderPropertiesDialog::OnChanged(obs_property_t *property, bool rebuild)
{
	if (rebuilding)
		return;

	obs_property_modified(property, settings);
	if (properties)
		obs_properties_apply_settings(properties, settings);

	if (rebuild) {
		QTimer::singleShot(0, this, &EncoderPropertiesDialog::RebuildForm);
	} else {
		UpdateRows();
	}
}
