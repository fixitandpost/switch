
#include "switcher-settings.hpp"

#include <QLabel>

#include <obs-module.h>
#include <QCheckBox>
#include <QCompleter>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>

#include "switcher-dock.hpp"

#ifndef QT_UTF8
#define QT_UTF8(str) QString::fromUtf8(str)
#endif
#ifndef QT_TO_UTF8
#define QT_TO_UTF8(str) str.toUtf8().constData()
#endif

SwitcherSettingsDialog::SwitcherSettingsDialog(QMainWindow *parent)
	: QDialog(parent),
		  mainLayout(new QGridLayout),
		  sourceCombo(new QComboBox()),
		  titleEdit(new QLineEdit()),
		  visibleCheckBox(new QCheckBox()),
	  previewCheckBox(new QCheckBox()),
	  volMeterCheckBox(new QCheckBox()),
	  volControlsCheckBox(new QCheckBox()),
	  mediaControlsCheckBox(new QCheckBox()),
	  switchSceneCheckBox(new QCheckBox()),
	  showActiveCheckBox(new QCheckBox()),
	  sceneItemsCheckBox(new QCheckBox()),
	  propertiesCheckBox(new QCheckBox()),
	  filtersCheckBox(new QCheckBox()),
	  textInputCheckBox(new QCheckBox())
{
	int idx = 0;

	mainLayout->setContentsMargins(0, 0, 0, 0);
	auto label = new QLabel(QT_UTF8(obs_module_text("Source")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new QLabel(QT_UTF8(obs_module_text("Title")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("Visible")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("Preview")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("VolumeMeter")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("AudioControls")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("MediaControls")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("SwitchScene")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("ShowActive")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("Properties")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("Filters")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("TextInput")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);
	label = new VerticalLabel(QT_UTF8(obs_module_text("SceneItems")));
	label->setStyleSheet("font-weight: bold;");
	mainLayout->addWidget(label, 0, idx++, Qt::AlignCenter);

	selectBoxColumn = idx;

	auto checkbox = new QCheckBox;
	mainLayout->addWidget(checkbox, 0, idx++, Qt::AlignCenter);
	mainLayout->setColumnStretch(0, 1);
	mainLayout->setColumnStretch(1, 1);

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(checkbox, &QCheckBox::checkStateChanged, [this]() { SelectAllChanged(); });
#else
	connect(checkbox, &QCheckBox::stateChanged, [this]() { SelectAllChanged(); });
#endif

	idx = 0;

	sourceCombo->setEditable(true);
	auto *completer = sourceCombo->completer();
	completer->setCaseSensitivity(Qt::CaseInsensitive);
	completer->setFilterMode(Qt::MatchContains);
	completer->setCompletionMode(QCompleter::PopupCompletion);
	sourceCombo->addItem("", QByteArray(""));
	sourceCombo->addItem(QT_UTF8(obs_module_text("CurrentSelectedSource")), QByteArray("CurrentSelectedSource"));

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t index = 0; index < scenes.sources.num; index++)
		AddSource(sourceCombo, scenes.sources.array[index]);
	obs_frontend_source_list_free(&scenes);
	obs_enum_sources(AddSource, sourceCombo);
	mainLayout->addWidget(sourceCombo, 1, idx++);

	connect(sourceCombo, SIGNAL(editTextChanged(const QString &)), SLOT(RefreshTable()));

	mainLayout->addWidget(titleEdit, 1, idx++);

	visibleCheckBox->setChecked(true);
	mainLayout->addWidget(visibleCheckBox, 1, idx++);

	previewCheckBox->setChecked(true);
	mainLayout->addWidget(previewCheckBox, 1, idx++);

	mainLayout->addWidget(volMeterCheckBox, 1, idx++);

	mainLayout->addWidget(volControlsCheckBox, 1, idx++);

	mainLayout->addWidget(mediaControlsCheckBox, 1, idx++);

	mainLayout->addWidget(switchSceneCheckBox, 1, idx++);

	mainLayout->addWidget(showActiveCheckBox, 1, idx++);

	mainLayout->addWidget(propertiesCheckBox, 1, idx++);

	mainLayout->addWidget(filtersCheckBox, 1, idx++);

	mainLayout->addWidget(textInputCheckBox, 1, idx++);

	mainLayout->addWidget(sceneItemsCheckBox, 1, idx++);

	auto addButton = new QPushButton(QT_UTF8(obs_module_text("Add")));
	connect(addButton, &QPushButton::clicked, [this]() { AddClicked(); });
	mainLayout->addWidget(addButton, 1, idx++, Qt::AlignCenter);

	RefreshTable();

	auto controlArea = new QWidget;
	controlArea->setLayout(mainLayout);
	controlArea->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

	auto vlayout = new QVBoxLayout;
	vlayout->addWidget(controlArea);
	//vlayout->setAlignment(controlArea, Qt::AlignTop);
	auto widget = new QWidget;
	widget->setLayout(vlayout);
	widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

	auto scrollArea = new QScrollArea;
	scrollArea->setWidget(widget);
	scrollArea->setWidgetResizable(true);

	auto closeButton = new QPushButton(QT_UTF8(obs_module_text("Close")));
	auto deleteButton = new QPushButton(QT_UTF8(obs_module_text("Delete")));
	auto ltCheckBox = new QCheckBox(QT_UTF8("⌜"));
	ltCheckBox->setChecked(parent->corner(Qt::TopLeftCorner) == Qt::LeftDockWidgetArea);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(ltCheckBox, &QCheckBox::checkStateChanged, [ltCheckBox]() {
#else
	connect(ltCheckBox, &QCheckBox::stateChanged, [ltCheckBox]() {
#endif
		auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		main_window->setCorner(Qt::TopLeftCorner, ltCheckBox->isChecked() ? Qt::LeftDockWidgetArea : Qt::TopDockWidgetArea);
	});
	auto rtCheckBox = new QCheckBox(QT_UTF8("⌝"));
	rtCheckBox->setChecked(parent->corner(Qt::TopRightCorner) == Qt::RightDockWidgetArea);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(rtCheckBox, &QCheckBox::checkStateChanged, [rtCheckBox]() {
#else
	connect(rtCheckBox, &QCheckBox::stateChanged, [rtCheckBox]() {
#endif
		auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		main_window->setCorner(Qt::TopRightCorner,
				       rtCheckBox->isChecked() ? Qt::RightDockWidgetArea : Qt::TopDockWidgetArea);
	});
	auto rbCheckBox = new QCheckBox(QT_UTF8("⌟"));
	rbCheckBox->setChecked(parent->corner(Qt::BottomRightCorner) == Qt::RightDockWidgetArea);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(rbCheckBox, &QCheckBox::checkStateChanged, [rbCheckBox]() {
#else
	connect(rbCheckBox, &QCheckBox::stateChanged, [rbCheckBox]() {
#endif
		auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		main_window->setCorner(Qt::BottomRightCorner,
				       rbCheckBox->isChecked() ? Qt::RightDockWidgetArea : Qt::BottomDockWidgetArea);
	});
	auto lbCheckBox = new QCheckBox(QT_UTF8("⌞"));
	lbCheckBox->setChecked(parent->corner(Qt::BottomLeftCorner) == Qt::LeftDockWidgetArea);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(lbCheckBox, &QCheckBox::checkStateChanged, [lbCheckBox]() {
#else
	connect(lbCheckBox, &QCheckBox::stateChanged, [lbCheckBox]() {
#endif
		auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		main_window->setCorner(Qt::BottomLeftCorner,
				       lbCheckBox->isChecked() ? Qt::LeftDockWidgetArea : Qt::BottomDockWidgetArea);
	});
	auto bottomLayout = new QHBoxLayout;
	bottomLayout->addWidget(deleteButton, 0, Qt::AlignLeft);
	bottomLayout->addWidget(ltCheckBox, 0, Qt::AlignCenter);
	bottomLayout->addWidget(rtCheckBox, 0, Qt::AlignCenter);
	bottomLayout->addWidget(rbCheckBox, 0, Qt::AlignCenter);
	bottomLayout->addWidget(lbCheckBox, 0, Qt::AlignCenter);
	bottomLayout->addWidget(closeButton, 0, Qt::AlignRight);

	connect(deleteButton, &QPushButton::clicked, [this]() { DeleteClicked(); });
	connect(closeButton, &QPushButton::clicked, [this]() { close(); });

	vlayout = new QVBoxLayout;
	vlayout->setContentsMargins(11, 11, 11, 11);
	vlayout->addWidget(scrollArea);
	vlayout->addLayout(bottomLayout);
	setLayout(vlayout);

	setWindowTitle(QT_UTF8(obs_module_text("SwitcherDocks")));
	setSizeGripEnabled(true);

	setMinimumSize(200, 200);
}

SwitcherSettingsDialog::~SwitcherSettingsDialog() {}

void update_selected_source();

void SwitcherSettingsDialog::AddClicked()
{
	const auto sn = sourceCombo->currentText();
	if (sn.isEmpty())
		return;
	const auto t2 = sn.toUtf8();
	const auto t3 = t2.constData();
	if (!t3 || !strlen(t3))
		return;
	auto title = titleEdit->text();
	if (title.isEmpty())
		title = sn;
	obs_source_t *source = nullptr;
	if (sourceCombo->currentIndex() != 1) {
		const auto sourceUuid = sourceCombo->currentData().toByteArray();
		if (!sourceUuid.isEmpty())
			source = obs_get_source_by_uuid(sourceUuid.constData());
		if (!source)
			source = obs_get_source_by_name(t3);
		if (!source)
			return;
	}

	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	SwitcherDockRegistrationOptions options;
	options.selected = source == nullptr;
	options.dockId = SwitcherDock::CreateDockId();
	options.preview = previewCheckBox->isChecked();
	options.volMeter = volMeterCheckBox->isChecked();
	options.volControls = volControlsCheckBox->isChecked();
	options.mediaControls = mediaControlsCheckBox->isChecked();
	options.switchScene = switchSceneCheckBox->isChecked();
	options.showActive = showActiveCheckBox->isChecked();
	options.properties = propertiesCheckBox->isChecked();
	options.filters = filtersCheckBox->isChecked();
	options.textInput = textInputCheckBox->isChecked();
	options.sceneItems = sceneItemsCheckBox->isChecked();
	options.visible = visibleCheckBox->isChecked();
		options.applyPlacement = true;
		options.dockArea = Qt::LeftDockWidgetArea;
		options.floating = false;

	auto *tmp = CreateRegisteredSwitcherDock(title, source, main_window, options);
	if (!tmp) {
		if (source)
			obs_source_release(source);
		return;
	}

	if (source)
		obs_source_release(source);
	else
		update_selected_source();

	RefreshTable();
}

void SwitcherSettingsDialog::RefreshTable()
{
	for (auto row = mainLayout->rowCount() - 1; row >= 2; row--) {
		for (auto col = mainLayout->columnCount() - 1; col >= 0; col--) {
			auto *item = mainLayout->itemAtPosition(row, col);
			if (item) {
				mainLayout->removeItem(item);
				delete item->widget();
				delete item;
			}
		}
	}
	auto row = 2;
	SwitcherDock *dock = nullptr;
	const auto sourceName = sourceCombo->currentText();
	const auto title = titleEdit->text();
	for (const auto &it : switcher_docks) {
		if (!sourceName.isEmpty()) {
			if (it->GetSelected()) {
				if (!QT_UTF8(obs_module_text("CurrentSelectedSource")).contains(sourceName, Qt::CaseInsensitive))
					continue;
			} else if (!QString::fromUtf8(obs_source_get_name(it->GetSource())).contains(sourceName, Qt::CaseInsensitive)) {
				continue;
			}
		}
		QString t = it->windowTitle();
		if (!title.isEmpty() && !t.contains(title, Qt::CaseInsensitive))
			continue;
		auto *parent = it->OwningMainWindow();
		if (!parent)
			parent = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		auto col = 0;
		auto *label = new QLabel(it->GetSelected() ? QT_UTF8(obs_module_text("CurrentSelectedSource"))
							    : QString::fromUtf8(obs_source_get_name(it->GetSource())));
		mainLayout->addWidget(label, row, col++);

		label = new QLabel(t);
		mainLayout->addWidget(label, row, col++);

		dock = it;

		auto *checkBox = new QCheckBox;
		auto *dockWidget = dock->ParentDockWidget();
		checkBox->setChecked(dockWidget && !dockWidget->isHidden());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
			connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
			connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
				auto *dockWidget = dock->ParentDockWidget();
				if (!dockWidget)
					return;
				if (checkBox->isChecked()) {
					dockWidget->show();
					if (auto *ownerWindow = dock->OwningMainWindow())
						ownerWindow->show();
				} else {
					dockWidget->hide();
				}
			});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->PreviewEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnablePreview();
				if (!dock->PreviewEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisablePreview();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->VolMeterEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableVolMeter();
				if (!dock->VolMeterEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableVolMeter();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->VolControlsEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableVolControls();
				if (!dock->VolControlsEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableVolControls();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->MediaControlsEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableMediaControls();
				if (!dock->MediaControlsEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableMediaControls();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->SwitchSceneEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableSwitchScene();
				if (!dock->SwitchSceneEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableSwitchScene();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->ShowActiveEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableShowActive();
				if (!dock->ShowActiveEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableShowActive();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->PropertiesEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableProperties();
				if (!dock->PropertiesEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableProperties();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->FiltersEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableFilters();
				if (!dock->FiltersEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableFilters();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->TextInputEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableTextInput();
				if (!dock->TextInputEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableTextInput();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		checkBox->setChecked(dock->SceneItemsEnabled());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(checkBox, &QCheckBox::checkStateChanged, [checkBox, dock]() {
#else
		connect(checkBox, &QCheckBox::stateChanged, [checkBox, dock]() {
#endif
			if (checkBox->isChecked()) {
				dock->EnableSceneItems();
				if (!dock->SceneItemsEnabled())
					checkBox->setChecked(false);
			} else {
				dock->DisableSceneItems();
			}
		});
		mainLayout->addWidget(checkBox, row, col++);

		checkBox = new QCheckBox;
		mainLayout->addWidget(checkBox, row, col++, Qt::AlignCenter);
		row++;
	}
}

void SwitcherSettingsDialog::mouseDoubleClickEvent(QMouseEvent *event)
{
	QWidget *widget = childAt(event->pos());
	if (!widget)
		return;
	int index = mainLayout->indexOf(widget);
	if (index < 0)
		return;

	int row, column, row_span, col_span;
	mainLayout->getItemPosition(index, &row, &column, &row_span, &col_span);
	if (row < 2)
		return;
	QLayoutItem *item = mainLayout->itemAtPosition(row, 0);
	if (!item)
		return;
	auto label = dynamic_cast<QLabel *>(item->widget());
	if (!label)
		return;
	const QString sourceName = label->text();
	if (sourceName.isEmpty())
		return;

	item = mainLayout->itemAtPosition(row, 1);
	if (!item)
		return;
	label = dynamic_cast<QLabel *>(item->widget());
	if (!label)
		return;
	const QString title = label->text();
	if (title.isEmpty())
		return;

	sourceCombo->setCurrentText(sourceName);
	titleEdit->setText(title);
}

void SwitcherSettingsDialog::DeleteClicked()
{
	for (auto row = 2; row < mainLayout->rowCount(); row++) {
		auto *item = mainLayout->itemAtPosition(row, selectBoxColumn);
		if (!item)
			continue;
		auto *checkBox = dynamic_cast<QCheckBox *>(item->widget());
		if (!checkBox || !checkBox->isChecked())
			continue;

		item = mainLayout->itemAtPosition(row, 0);
		auto *label = dynamic_cast<QLabel *>(item->widget());
		if (!label)
			continue;
		std::string sourceName = label->text().toUtf8().constData();
		item = mainLayout->itemAtPosition(row, 1);
		label = dynamic_cast<QLabel *>(item->widget());
		if (!label)
			continue;
		auto title = label->text();
		for (auto it = switcher_docks.begin(); it != switcher_docks.end();) {
			if ((*it)->windowTitle() != title) {
				++it;
				continue;
			}
			if (!(*it)->GetSelected() && sourceName != obs_source_get_name((*it)->GetSource())) {
				++it;
				continue;
			}
			obs_frontend_remove_dock((*it)->objectName().toUtf8().constData());
			it = switcher_docks.erase(it);
		}
	}
	RefreshTable();
}

void SwitcherSettingsDialog::SelectAllChanged()
{
	auto *item = mainLayout->itemAtPosition(0, selectBoxColumn);
	auto *checkBox = dynamic_cast<QCheckBox *>(item->widget());
	bool checked = checkBox && checkBox->isChecked();
	for (auto row = 2; row < mainLayout->rowCount(); row++) {
		item = mainLayout->itemAtPosition(row, selectBoxColumn);
		if (!item)
			continue;
		auto *checkBox = dynamic_cast<QCheckBox *>(item->widget());
		if (!checkBox)
			continue;
		checkBox->setChecked(checked);
	}
}

bool SwitcherSettingsDialog::AddSource(void *data, obs_source_t *source)
{
	const char *sn = obs_source_get_name(source);
	const char *sourceUuid = obs_source_get_uuid(source);
	auto sourceCombo = static_cast<QComboBox *>(data);
	const QByteArray uuid = sourceUuid ? QByteArray(sourceUuid) : QByteArray();
	if (!uuid.isEmpty() && sourceCombo->findData(uuid) >= 0)
		return true;
	sourceCombo->addItem(QT_UTF8(sn), uuid);
	return true;
}

VerticalLabel::VerticalLabel(QWidget *parent) : QLabel(parent) {}

VerticalLabel::VerticalLabel(const QString &text, QWidget *parent) : QLabel(text, parent) {}

void VerticalLabel::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	//painter.setPen(Qt::black);
	//painter.setBrush(Qt::Dense1Pattern);

	painter.rotate(90);

	painter.drawText(0, 0, text());
}

QSize VerticalLabel::minimumSizeHint() const
{
	QSize s = QLabel::minimumSizeHint();
	return QSize(s.height(), s.width());
}

QSize VerticalLabel::sizeHint() const
{
	QSize s = QLabel::sizeHint();
	return QSize(s.height(), s.width());
}
