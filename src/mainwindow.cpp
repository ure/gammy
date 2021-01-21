﻿/**
 * Copyright (C) Francesco Fusco. All rights reserved.
 * License: https://github.com/Fushko/gammy#license
 */

#ifdef _WIN32
	#include <Windows.h>
#endif

#include <QScreen>
#include <QMenu>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusConnection>
#include "cfg.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "tempscheduler.h"

MainWindow::MainWindow(): ui(new Ui::MainWindow), tray_icon(new QSystemTrayIcon(this))
{
	ui->setupUi(this);
}

void MainWindow::init()
{
	if (!windows && !listenWakeupSignal()) {
		LOGE << "Gammy is unable to reset the proper brightness / temperature when resuming from suspend.";
	}

	QIcon icon = QIcon(":res/icons/128x128ball.ico");

	setWindowProperties(icon);
	setLabels();
	setSliders();

	toggleBrtSliders(cfg["brt_auto"]);
	ui->autoBrtCheck->setChecked(cfg["brt_auto"]);
	ui->autoTempCheck->setChecked(cfg["temp_auto"]);

	createTrayIcon(icon);
}

void MainWindow::setLabels()
{
	ui->brtLabel->setText(QStringLiteral("%1 %").arg(int(remap(cfg["brt_step"].get<int>(), 0, brt_steps_max, 0, 100))));
	ui->minBrLabel->setText(QStringLiteral("%1 %").arg(int(ceil(remap(cfg["brt_min"].get<int>(), 0, brt_steps_max, 0, 100)))));
	ui->maxBrLabel->setText(QStringLiteral("%1 %").arg(int(ceil(remap(cfg["brt_max"].get<int>(), 0, brt_steps_max, 0, 100)))));
	ui->speedLabel->setText(QStringLiteral("%1 s").arg(QString::number(cfg["brt_speed"].get<int>() / 1000., 'g', 2)));
	ui->thresholdLabel->setText(QStringLiteral("%1").arg(cfg["brt_threshold"].get<int>()));
	ui->pollingLabel->setText(QStringLiteral("%1").arg(cfg["brt_polling_rate"].get<int>()));

	double temp_kelvin = remap(temp_steps_max - cfg["temp_step"].get<int>(), 0, temp_steps_max, temp_k_max, temp_k_min);
	temp_kelvin = floor(temp_kelvin / 100) * 100;
	ui->tempLabel->setText(QStringLiteral("%1 K").arg(temp_kelvin));
}

void MainWindow::setWindowProperties(QIcon &icon)
{
	setWindowTitle("Gammy");
	setWindowIcon(icon);
	setMinimumHeight(wnd_height);
	setMaximumHeight(wnd_height);
	setVisible(cfg["show_on_startup"]);

	QApplication::setAttribute(Qt::AA_DisableWindowContextHelpButton);

	// Extending brightness range doesn't work yet on Windows
	if (windows)
		ui->extendBr->hide();

	ui->speedWidget->hide();
	ui->threshWidget->hide();
	ui->pollingWidget->hide();
	ui->line->setPalette(QPalette(qRgb(25, 27, 37)));

	// Move window to bottom right
	QRect scr = QGuiApplication::primaryScreen()->availableGeometry();
	move(scr.width() - this->width() - wnd_offset_x, scr.height() - this->height() - wnd_offset_y);
}

void MainWindow::setSliders()
{
	ui->brtSlider->setValue(cfg["brt_step"]);
	ui->extendBr->setChecked(cfg["brt_extend"]);

	if (cfg["brt_extend"].get<bool>()) {
		toggleBrtSlidersRange(true);
	}

	ui->offsetSlider->setValue(cfg["brt_offset"]);
	ui->tempSlider->setRange(0, temp_steps_max);
	ui->tempSlider->setValue(cfg["temp_step"]);
	ui->speedSlider->setValue(cfg["brt_speed"]);
	ui->thresholdSlider->setValue(cfg["brt_threshold"]);
	ui->pollingSlider->setValue(cfg["brt_polling_rate"]);
}

void MainWindow::createTrayIcon(QIcon &icon)
{
	QMenu *menu = createMenu();
	tray_icon->setContextMenu(menu);
	tray_icon->setToolTip(QString("Gammy"));
	tray_icon->setIcon(icon);
	connect(tray_icon, &QSystemTrayIcon::activated, this, &MainWindow::iconActivated);
	tray_icon->show();

	systray_available = QSystemTrayIcon::isSystemTrayAvailable();

	if (!systray_available) {
		this->show();
		LOGE << "Systray unavailable. Closing the window will quit the app.";
	}
}

bool MainWindow::listenWakeupSignal()
{
	QDBusConnection dbus = QDBusConnection::systemBus();

	if (!dbus.isConnected()) {
		LOGE << "Cannot connect to the system D-Bus.";
		return false;
	}

	const QString service   = "org.freedesktop.login1";
	const QString path      = "/org/freedesktop/login1";
	const QString interface = "org.freedesktop.login1.Manager";
	const QString name      = "PrepareForSleep";

	QDBusInterface iface(service, path, interface, dbus, this);

	if (!iface.isValid()) {
		LOGE << "Wakeup interface not found.";
		return false;
	}

	bool connected = dbus.connect(service, path, interface, name, this, SLOT(wakeupSlot(bool)));

	if (!connected) {
		LOGE << "Cannot connect to wakeup signal.";
		return false;
	}

	return true;
}

void MainWindow::wakeupSlot(bool status)
{
	// The signal emits TRUE when going to sleep. We only care about wakeup (FALSE)
	if (status)
		return;

	mediator->notify(this, SYSTEM_WAKE_UP);
}

void MainWindow::quit(bool prev_gamma)
{
	this->prev_gamma = prev_gamma;
	tray_icon->hide();
	mediator->notify(this, prev_gamma ? APP_QUIT : APP_QUIT_PURE_GAMMA);
	config::write();
	QApplication::quit();
}

/**
 * This event gets fired when the window is closed or we quit.
 * We ignore it so that we only hide the window, if the systray is available.
 */
void MainWindow::closeEvent(QCloseEvent *e)
{
	this->hide();
	config::write();

	if (!systray_available)
		this->quit(prev_gamma);

	e->ignore();
}

void MainWindow::showOnTop()
{
	if (!isHidden())
		return;

	// Move the window to bottom right again. For some reason it moves up.
	QRect scr = QGuiApplication::primaryScreen()->availableGeometry();
	move(scr.width() - this->width() - wnd_offset_x, scr.height() - this->height() - wnd_offset_y);

	show();
	updateBrtLabel(cfg["brt_step"]);
}

QMenu* MainWindow::createMenu()
{
	QMenu *menu = new QMenu(nullptr);

#ifdef _WIN32
	QAction *run_startup = new QAction("&Run at startup", this);
	run_startup->setCheckable(true);

	connect(run_startup, &QAction::triggered, [=]{ toggleRegkey(run_startup->isChecked()); });
	menu->addAction(run_startup);

	LRESULT s = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Gammy", RRF_RT_REG_SZ, nullptr, nullptr, nullptr);
	run_startup->setChecked(s == ERROR_SUCCESS);
#else
	QAction *show_wnd = new QAction("&Show Gammy", this);

	connect(show_wnd, &QAction::triggered, this, [=] { showOnTop(); });
	menu->addAction(show_wnd);
#endif

	menu->addSeparator();

	QAction *quit_prev = new QAction("&Quit", this);
	connect(quit_prev, &QAction::triggered, this, [=] { quit(true); });
	menu->addAction(quit_prev);

	if (!windows) {
		QAction *quit_pure = new QAction("&Quit (set pure gamma)", this);
		connect(quit_pure, &QAction::triggered, this, [=] { quit(false); });
		menu->addAction(quit_pure);
	}

	menu->addSeparator();

	QAction *about = new QAction("&Gammy " + QString(g_app_version), this);
	about->setEnabled(false);
	menu->addAction(about);

	return menu;
}

/**
 * These are called by the mediator when brt/temp is getting adjusted.
 * Triggers a valueChanged, but not a sliderMoved. This allows us
 * to differentiate between app and user action.
 */
void MainWindow::setBrtSlider(int val)
{
	ui->brtSlider->setValue(val);
}

void MainWindow::setTempSlider(int val)
{
	ui->tempSlider->setValue(val);
}

/**
 * Triggered when the user changes the slider manually via:
 * left click, middle click, single step, page step.
 * We need to update the gamma manually in this case.
 */
void MainWindow::on_brtSlider_actionTriggered([[maybe_unused]] int action)
{
	if (ui->autoBrtCheck->isChecked())
		ui->autoBrtCheck->setChecked(false);

	int val = ui->brtSlider->sliderPosition();
	cfg["brt_step"] = val;
	mediator->notify(this, GAMMA_SLIDER_MOVED);
	updateBrtLabel(val);
}

void MainWindow::on_tempSlider_actionTriggered([[maybe_unused]] int action)
{
	if (ui->autoTempCheck->isChecked())
		ui->autoTempCheck->setChecked(false);

	int val = ui->tempSlider->sliderPosition();
	cfg["temp_step"] = val;
	mediator->notify(this, GAMMA_SLIDER_MOVED);
	updateTempLabel(val);
}

/**
 * Triggered when the sliders are moved by the gamma controller.
 * Just update the labels, since the gamma has been adjusted already.
 */
void MainWindow::on_brtSlider_valueChanged(int val)
{
	updateBrtLabel(val);
}

void MainWindow::on_tempSlider_valueChanged(int val)
{
	updateTempLabel(val);
}

void MainWindow::on_brRange_lowerValueChanged(int val)
{
	cfg["brt_min"] = val;
	val = int(ceil(remap(val, 0, brt_steps_max, 0, 100)));
	ui->minBrLabel->setText(QStringLiteral("%1 %").arg(val));
}

void MainWindow::on_brRange_upperValueChanged(int val)
{
	cfg["brt_max"] = val;
	val = int(ceil(remap(val, 0, brt_steps_max, 0, 100)));
	ui->maxBrLabel->setText(QStringLiteral("%1 %").arg(val));
}

void MainWindow::toggleBrtSlidersRange(bool extend)
{
	int brt_limit = brt_steps_max;

	if (extend)
		brt_limit *= 2;

	const int min = cfg["brt_min"].get<int>();
	const int max = cfg["brt_max"].get<int>();
	ui->brRange->setMaximum(brt_limit);

	// We set the upper/lower values again because they reset after setMaximum
	ui->brRange->setUpperValue(max);
	ui->brRange->setLowerValue(min);

	ui->brtSlider->setRange(100, brt_limit);

	if (!cfg["brt_auto"].get<bool>()) {
		ui->brtSlider->setValue(cfg["brt_step"]);
		emit on_brtSlider_actionTriggered(QAbstractSlider::SliderMove);
	}
}

void MainWindow::updateBrtLabel(int val)
{
	val = int(ceil(remap(val, 0, brt_steps_max, 0, 100)));
	ui->brtLabel->setText(QStringLiteral("%1 %").arg(val));
}

void MainWindow::updateTempLabel(int val)
{
	double temp_kelvin = remap(temp_steps_max - val, 0, temp_steps_max, temp_k_max, temp_k_min);
	temp_kelvin = floor(temp_kelvin / 10) * 10;
	ui->tempLabel->setText(QStringLiteral("%1 K").arg(temp_kelvin));
}

void MainWindow::on_offsetSlider_valueChanged(int val)
{
	cfg["brt_offset"] = val;
	ui->offsetLabel->setText(QStringLiteral("%1 %").arg(int(remap(val, 0, brt_steps_max, 0, 100))));
}

void MainWindow::on_speedSlider_valueChanged(int val)
{
	cfg["brt_speed"] = val;
	ui->speedLabel->setText(QStringLiteral("%1 s").arg(QString::number(val / 1000., 'g', 2)));
}

void MainWindow::on_thresholdSlider_valueChanged(int val)
{
	cfg["brt_threshold"] = val;
}

void MainWindow::on_pollingSlider_valueChanged(int val)
{
	cfg["brt_polling_rate"] = val;
}

void MainWindow::toggleBrtSliders(bool checked)
{
	ui->rangeWidget->setVisible(checked);
	ui->offsetWidget->setVisible(checked);

	// Enable advanced settings button when auto brt is enabled
	ui->advBrSettingsBtn->setEnabled(checked);
	ui->advBrSettingsBtn->setChecked(false);

	auto btn_color = "color:rgba(0,0,0,0)";
	auto height    = 170;

	if (checked) {
		btn_color = "color:white";
		height = wnd_height;
	}

	ui->advBrSettingsBtn->setStyleSheet(btn_color);

	this->setMinimumHeight(height);
	this->setMaximumHeight(height);
}

void MainWindow::on_advBrSettingsBtn_toggled(bool checked)
{
	ui->speedWidget->setVisible(checked);
	ui->threshWidget->setVisible(checked);
	ui->pollingWidget->setVisible(checked);

	const int h = checked ? wnd_height + 185 : wnd_height;

	this->setMinimumHeight(h);
	this->setMaximumHeight(h);
}

void MainWindow::on_autoBrtCheck_toggled(bool checked)
{
	cfg["brt_auto"] = checked;
	mediator->notify(this, AUTO_BRT_TOGGLED);

	toggleBrtSliders(checked);
}

void MainWindow::on_autoTempCheck_toggled(bool checked)
{
	cfg["temp_auto"] = checked;
	this->mediator->notify(this, AUTO_TEMP_TOGGLED);
}

void MainWindow::on_extendBr_clicked(bool checked)
{
	cfg["brt_extend"] = checked;
	toggleBrtSlidersRange(cfg["brt_extend"]);
}

void MainWindow::on_pushButton_clicked()
{
	TempScheduler ts(this->mediator);
	ts.exec();
}

void MainWindow::setPollingRange(int min, int max)
{
	const int poll = cfg["brt_polling_rate"];

	LOGD << "Setting polling rate slider range to: " << min << ", " << max;

	ui->pollingSlider->setRange(min, max);

	if (poll < min)
		cfg["brt_polling_rate"] = min;
	else if (poll > max)
		cfg["brt_polling_rate"] = max;

	ui->pollingLabel->setText(QString::number(poll));
	ui->pollingSlider->setValue(poll);
}

void MainWindow::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
	if (reason == QSystemTrayIcon::Trigger) {
		updateBrtLabel(cfg["brt_step"]);
		show();
	}
}

MainWindow::~MainWindow()
{
	delete ui;
}
