// Copyright (c) 2011-2015 The Bitcoin Core and Bitcoin XT developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#include "optionsdialog.h"
#include "ui_optionsdialog.h"

#include "bitcoinunits.h"
#include "guiutil.h"
#include "optionsmodel.h"

#include "main.h" // for MAX_SCRIPTCHECK_THREADS
#include "netbase.h"
#include "net.h"  // for access to the network traffic shapers
#include "txdb.h" // for -dbcache defaults

#ifdef ENABLE_WALLET
#include "wallet/wallet.h" // for CWallet::minTxFee
#endif

#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>

#include <QDataWidgetMapper>
#include <QDir>
#include <QIntValidator>
#include <QLocale>
#include <QMessageBox>
#include <QTimer>

inline int64_t bwEdit2Slider(int64_t x) { return sqrt(x * 100); }
inline int64_t bwSlider2Edit(int64_t x) { return x * x / 100; }

OptionsDialog::OptionsDialog(QWidget* parent, bool enableWallet) : QDialog(parent),
                                                                   ui(new Ui::OptionsDialog),
                                                                   model(0),
                                                                   mapper(0),
                                                                   fProxyIpValid(true),
                                                                   portValidator(1, 65536, this),
                                                                   burstValidator(0, 100000000, this),
                                                                   sendAveValidator(0, 100000000, this),
                                                                   recvAveValidator(0, 100000000, this)
{
    ui->setupUi(this);
    sendAveValidator.initialize(ui->sendBurstEdit, ui->errorText);
    recvAveValidator.initialize(ui->recvBurstEdit, ui->errorText);
    /* Main elements init */
    ui->databaseCache->setMinimum(nMinDbCache);
    ui->databaseCache->setMaximum(nMaxDbCache);
    ui->threadsScriptVerif->setMinimum(-(int)boost::thread::hardware_concurrency());
    ui->threadsScriptVerif->setMaximum(MAX_SCRIPTCHECK_THREADS);

/* Network elements init */
#ifndef USE_UPNP
    ui->mapPortUpnp->setEnabled(false);
#endif

    ui->proxyIp->setEnabled(false);
    ui->proxyPort->setEnabled(false);
    ui->proxyPort->setValidator(&portValidator); //new QIntValidator(1, 65535, this));

    connect(ui->connectSocks, SIGNAL(toggled(bool)), ui->proxyIp, SLOT(setEnabled(bool)));
    connect(ui->connectSocks, SIGNAL(toggled(bool)), ui->proxyPort, SLOT(setEnabled(bool)));

    ui->proxyIp->installEventFilter(this);

/* Window elements init */
#ifdef Q_OS_MAC
    /* remove Window tab on Mac */
    ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->tabWindow));
#endif

    /* remove Wallet tab in case of -disablewallet */
    if (!enableWallet) {
        ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->tabWallet));
    }

    /* Display elements init */
    QDir translations(":translations");
    ui->lang->addItem(QString("(") + tr("default") + QString(")"), QVariant(""));
    foreach (const QString& langStr, translations.entryList()) {
        QLocale locale(langStr);

        /** check if the locale name consists of 2 parts (language_country) */
        if (langStr.contains("_")) {
#if QT_VERSION >= 0x040800
            /** display language strings as "native language - native country (locale name)", e.g. "Deutsch - Deutschland (de)" */
            ui->lang->addItem(locale.nativeLanguageName() + QString(" - ") + locale.nativeCountryName() + QString(" (") + langStr + QString(")"), QVariant(langStr));
#else
            /** display language strings as "language - country (locale name)", e.g. "German - Germany (de)" */
            ui->lang->addItem(QLocale::languageToString(locale.language()) + QString(" - ") + QLocale::countryToString(locale.country()) + QString(" (") + langStr + QString(")"), QVariant(langStr));
#endif
        } else {
#if QT_VERSION >= 0x040800
            /** display language strings as "native language (locale name)", e.g. "Deutsch (de)" */
            ui->lang->addItem(locale.nativeLanguageName() + QString(" (") + langStr + QString(")"), QVariant(langStr));
#else
            /** display language strings as "language (locale name)", e.g. "German (de)" */
            ui->lang->addItem(QLocale::languageToString(locale.language()) + QString(" (") + langStr + QString(")"), QVariant(langStr));
#endif
        }
    }
#if QT_VERSION >= 0x040700
    ui->thirdPartyTxUrls->setPlaceholderText("https://example.com/tx/%s");
#endif

    ui->unit->setModel(new BitcoinUnits(this));

    /* Widget-to-option mapper */
    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
    mapper->setOrientation(Qt::Vertical);

    /* setup/change UI elements when proxy IP is invalid/valid */
    connect(this, SIGNAL(proxyIpChecks(QValidatedLineEdit*, int)), this, SLOT(doProxyIpChecks(QValidatedLineEdit*, int)));

    int64_t max, ave;
    sendShaper.get(&max, &ave);
    int64_t longMax = LONG_LONG_MAX;
    bool enabled = (ave != longMax);
    ui->sendShapingEnable->setChecked(enabled);
    ui->sendBurstSlider->setRange(0, 1000); // The slider is just for convenience so setting their ranges to what is commonly chosen
    ui->sendAveSlider->setRange(0, 1000);
    ui->recvBurstSlider->setRange(0, 1000);
    ui->recvAveSlider->setRange(0, 1000);

    ui->sendBurstEdit->setValidator(&burstValidator);
    ui->recvBurstEdit->setValidator(&burstValidator);
    ui->sendAveEdit->setValidator(&sendAveValidator);
    ui->recvAveEdit->setValidator(&recvAveValidator);

    connect(ui->sendShapingEnable, SIGNAL(clicked(bool)), this, SLOT(shapingEnableChanged(bool)));
    connect(ui->recvShapingEnable, SIGNAL(clicked(bool)), this, SLOT(shapingEnableChanged(bool)));

    connect(ui->sendBurstSlider, SIGNAL(valueChanged(int)), this, SLOT(shapingSliderChanged()));
    connect(ui->sendAveSlider, SIGNAL(valueChanged(int)), this, SLOT(shapingSliderChanged()));
    connect(ui->recvBurstSlider, SIGNAL(valueChanged(int)), this, SLOT(shapingSliderChanged()));
    connect(ui->recvAveSlider, SIGNAL(valueChanged(int)), this, SLOT(shapingSliderChanged()));

    connect(ui->recvAveEdit, SIGNAL(editingFinished()), this, SLOT(shapingAveEditFinished()));
    connect(ui->sendAveEdit, SIGNAL(editingFinished()), this, SLOT(shapingAveEditFinished()));
    connect(ui->recvBurstEdit, SIGNAL(editingFinished()), this, SLOT(shapingMaxEditFinished()));
    connect(ui->sendBurstEdit, SIGNAL(editingFinished()), this, SLOT(shapingMaxEditFinished()));

    if (enabled) {
        ui->sendBurstEdit->setText(QString(boost::lexical_cast<std::string>(max / 1024).c_str()));
        ui->sendAveEdit->setText(QString(boost::lexical_cast<std::string>(ave / 1024).c_str()));
        ui->sendBurstSlider->setValue(bwEdit2Slider(max / 1024));
        ui->sendAveSlider->setValue(bwEdit2Slider(ave / 1024));
    } else {
        ui->sendBurstEdit->setText("");
        ui->sendAveEdit->setText("");
    }

    receiveShaper.get(&max, &ave);
    enabled = (ave != LONG_LONG_MAX);
    ui->recvShapingEnable->setChecked(enabled);
    if (enabled) {
        ui->recvBurstEdit->setText(QString(boost::lexical_cast<std::string>(max / 1024).c_str()));
        ui->recvAveEdit->setText(QString(boost::lexical_cast<std::string>(ave / 1024).c_str()));
        ui->recvBurstSlider->setValue(bwEdit2Slider(max / 1024));
        ui->recvAveSlider->setValue(bwEdit2Slider(ave / 1024));
    } else {
        ui->recvBurstEdit->setText("");
        ui->recvAveEdit->setText("");
    }
    shapingEnableChanged(false);
}

OptionsDialog::~OptionsDialog()
{
    delete ui;
}


void OptionsDialog::shapingAveEditFinished(void)
{
    bool ok, ok2 = false;

    if (ui->sendShapingEnable->isChecked()) {
        // If the user adjusted the average to be higher than the max, then auto-bump the max up to = the average
        int maxVal = ui->sendBurstEdit->text().toInt(&ok);
        int aveVal = ui->sendAveEdit->text().toInt(&ok2);

        if (ok && ok2) {
            ui->sendAveSlider->setValue(bwEdit2Slider(aveVal));
            if (maxVal < aveVal) {
                ui->sendBurstEdit->setText(ui->sendAveEdit->text());
                ui->sendBurstSlider->setValue(bwEdit2Slider(aveVal));
            }
        }
    }

    if (ui->recvShapingEnable->isChecked()) {
        int maxVal = ui->recvBurstEdit->text().toInt(&ok);
        int aveVal = ui->recvAveEdit->text().toInt(&ok2);
        if (ok && ok2) {
            ui->recvAveSlider->setValue(bwEdit2Slider(aveVal));
            if (maxVal < aveVal) {
                ui->recvBurstEdit->setText(ui->recvAveEdit->text());
                ui->recvBurstSlider->setValue(bwEdit2Slider(aveVal));
            }
        }
    }
}

void OptionsDialog::shapingMaxEditFinished(void)
{
    bool ok, ok2 = false;

    if (ui->sendShapingEnable->isChecked()) {
        // If the user adjusted the max to be lower than the average, then move the average down
        int maxVal = ui->sendBurstEdit->text().toInt(&ok);
        int aveVal = ui->sendAveEdit->text().toInt(&ok2);
        if (ok && ok2) {
            ui->sendBurstSlider->setValue(bwEdit2Slider(maxVal)); // Move the slider based on the edit box change
            if (maxVal < aveVal)                                  // If the max was changed to be lower than the average, bump the average down to the maximum, because having an ave > the max makes no sense.
            {
                ui->sendAveEdit->setText(ui->sendBurstEdit->text()); // I use the string text here just so I don't have to convert back from int to string
                ui->sendAveSlider->setValue(bwEdit2Slider(maxVal));
            }
        }
    }


    if (ui->recvShapingEnable->isChecked()) {
        int maxVal = ui->recvBurstEdit->text().toInt(&ok);
        int aveVal = ui->recvAveEdit->text().toInt(&ok2);
        if (ok && ok2) {
            ui->recvBurstSlider->setValue(bwEdit2Slider(maxVal)); // Move the slider based on the edit box change
            if (maxVal < aveVal) {
                ui->recvAveEdit->setText(ui->recvBurstEdit->text()); // I use the string text here just so I don't have to convert back from int to string
                ui->recvAveSlider->setValue(bwEdit2Slider(maxVal));
            }
        }
    }
}

void OptionsDialog::shapingEnableChanged(bool val)
{
    bool enabled = ui->sendShapingEnable->isChecked();

    ui->sendBurstSlider->setEnabled(enabled);
    ui->sendAveSlider->setEnabled(enabled);
    ui->sendBurstEdit->setEnabled(enabled);
    ui->sendAveEdit->setEnabled(enabled);

    enabled = ui->recvShapingEnable->isChecked();
    ui->recvBurstSlider->setEnabled(enabled);
    ui->recvAveSlider->setEnabled(enabled);
    ui->recvBurstEdit->setEnabled(enabled);
    ui->recvAveEdit->setEnabled(enabled);
}

void OptionsDialog::shapingSliderChanged(void)
{
    // When the sliders change, I want to update the edit box.  Rather then have the pain of making a separate function for every slider, I just set them all whenever one changes.
    int64_t sval;
    int64_t val;
    int64_t cur;

    if (ui->sendShapingEnable->isChecked()) {
        sval = ui->sendBurstSlider->value();
        val = bwSlider2Edit(sval); // Transform the slider linear position into a bandwidth in Kb
        cur = ui->sendBurstEdit->text().toLongLong();

        // The slider is imprecise compared to the edit box.  So we only want to change the edit box if the slider's change is larger than its imprecision.
        if (bwEdit2Slider(cur) != sval) {
            ui->sendBurstEdit->setText(QString::number(val));
            int64_t other = ui->sendAveEdit->text().toLongLong();
            if (other > val) // Set average to burst if its greater
            {
                ui->sendAveEdit->setText(QString::number(val));
                ui->sendAveSlider->setValue(bwEdit2Slider(val));
            }
        }

        sval = ui->sendAveSlider->value();
        val = bwSlider2Edit(sval); // Transform the slider linear position into a bandwidth
        cur = ui->sendAveEdit->text().toLongLong();
        if (bwEdit2Slider(cur) != sval) {
            ui->sendAveEdit->setText(QString(boost::lexical_cast<std::string>(val).c_str()));
            int64_t burst = ui->sendBurstEdit->text().toLongLong();
            if (burst < val) // Set burst to average if it is less
            {
                ui->sendBurstEdit->setText(QString::number(val));
                ui->sendBurstSlider->setValue(bwEdit2Slider(val));
            }
        }
    }

    if (ui->recvShapingEnable->isChecked()) {
        sval = ui->recvBurstSlider->value();
        val = bwSlider2Edit(sval); // Transform the slider linear position into a bandwidth
        cur = ui->recvBurstEdit->text().toLongLong();
        if (bwEdit2Slider(cur) != sval) {
            ui->recvBurstEdit->setText(QString(boost::lexical_cast<std::string>(val).c_str()));
            int64_t other = ui->recvAveEdit->text().toLongLong();
            if (other > val) // Set average to burst if its greater
            {
                ui->recvAveEdit->setText(QString::number(val));
                ui->recvAveSlider->setValue(bwEdit2Slider(val));
            }
        }

        sval = ui->recvAveSlider->value();
        val = bwSlider2Edit(sval); // Transform the slider linear position into a bandwidth
        cur = ui->recvAveEdit->text().toLongLong();
        if (bwEdit2Slider(cur) != sval) {
            ui->recvAveEdit->setText(QString(boost::lexical_cast<std::string>(val).c_str()));
            int64_t burst = ui->recvBurstEdit->text().toLongLong();
            if (burst < val) // Set burst to average if it is less
            {
                ui->recvBurstEdit->setText(QString::number(val));
                ui->recvBurstSlider->setValue(bwEdit2Slider(val));
            }
        }
    }
}

void OptionsDialog::setModel(OptionsModel* model)
{
    this->model = model;

    if (model) {
        /* check if client restart is needed and show persistent message */
        if (model->isRestartRequired())
            showRestartWarning(true);

        QString strLabel = model->getOverriddenByCommandLine();
        if (strLabel.isEmpty())
            strLabel = tr("none");
        ui->overriddenByCommandLineLabel->setText(strLabel);

        mapper->setModel(model);
        setMapper();
        mapper->toFirst();
    }

    /* warn when one of the following settings changes by user action (placed here so init via mapper doesn't trigger them) */

    /* Main */
    connect(ui->databaseCache, SIGNAL(valueChanged(int)), this, SLOT(showRestartWarning()));
    connect(ui->threadsScriptVerif, SIGNAL(valueChanged(int)), this, SLOT(showRestartWarning()));
    /* Wallet */
    connect(ui->spendZeroConfChange, SIGNAL(clicked(bool)), this, SLOT(showRestartWarning()));
    /* Network */
    connect(ui->allowIncoming, SIGNAL(clicked(bool)), this, SLOT(showRestartWarning()));
    connect(ui->connectSocks, SIGNAL(clicked(bool)), this, SLOT(showRestartWarning()));
    /* Display */
    connect(ui->lang, SIGNAL(valueChanged()), this, SLOT(showRestartWarning()));
    connect(ui->thirdPartyTxUrls, SIGNAL(textChanged(const QString&)), this, SLOT(showRestartWarning()));
}

void OptionsDialog::setMapper()
{
    /* Main */
    mapper->addMapping(ui->bitcoinAtStartup, OptionsModel::StartAtStartup);
    mapper->addMapping(ui->threadsScriptVerif, OptionsModel::ThreadsScriptVerif);
    mapper->addMapping(ui->databaseCache, OptionsModel::DatabaseCache);

    /* Wallet */
    mapper->addMapping(ui->spendZeroConfChange, OptionsModel::SpendZeroConfChange);
    mapper->addMapping(ui->coinControlFeatures, OptionsModel::CoinControlFeatures);

    /* Network */
    mapper->addMapping(ui->mapPortUpnp, OptionsModel::MapPortUPnP);
    mapper->addMapping(ui->allowIncoming, OptionsModel::Listen);

    mapper->addMapping(ui->connectSocks, OptionsModel::ProxyUse);
    mapper->addMapping(ui->proxyIp, OptionsModel::ProxyIP);
    mapper->addMapping(ui->proxyPort, OptionsModel::ProxyPort);

    mapper->addMapping(ui->sendShapingEnable, OptionsModel::UseSendShaping);
    mapper->addMapping(ui->sendBurstEdit, OptionsModel::SendBurst);
    mapper->addMapping(ui->sendAveEdit, OptionsModel::SendAve);
    mapper->addMapping(ui->recvShapingEnable, OptionsModel::UseReceiveShaping);
    mapper->addMapping(ui->recvBurstEdit, OptionsModel::ReceiveBurst);
    mapper->addMapping(ui->recvAveEdit, OptionsModel::ReceiveAve);

/* Window */
#ifndef Q_OS_MAC
    mapper->addMapping(ui->minimizeToTray, OptionsModel::MinimizeToTray);
    mapper->addMapping(ui->minimizeOnClose, OptionsModel::MinimizeOnClose);
#endif

    /* Display */
    mapper->addMapping(ui->lang, OptionsModel::Language);
    mapper->addMapping(ui->unit, OptionsModel::DisplayUnit);
    mapper->addMapping(ui->thirdPartyTxUrls, OptionsModel::ThirdPartyTxUrls);
}

void OptionsDialog::enableOkButton()
{
    /* prevent enabling of the OK button when data modified, if there is an invalid proxy address present */
    if (fProxyIpValid)
        setOkButtonState(true);
}

void OptionsDialog::disableOkButton()
{
    setOkButtonState(false);
}

void OptionsDialog::setOkButtonState(bool fState)
{
    ui->okButton->setEnabled(fState);
}

void OptionsDialog::on_resetButton_clicked()
{
    if (model) {
        // confirmation dialog
        QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm options reset"), tr("Client restart required to activate changes.") + "<br><br>" + tr("Client will be shut down. Do you want to proceed?"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

        if (btnRetVal == QMessageBox::Cancel)
            return;

        /* reset all options and close GUI */
        model->Reset();
        QApplication::quit();
    }
}

void OptionsDialog::on_okButton_clicked()
{
    mapper->submit();
    accept();
}

void OptionsDialog::on_cancelButton_clicked()
{
    reject();
}

void OptionsDialog::showRestartWarning(bool fPersistent)
{
    ui->statusLabel->setStyleSheet("QLabel { color: red; }");

    if (fPersistent) {
        ui->statusLabel->setText(tr("Client restart required to activate changes."));
    } else {
        ui->statusLabel->setText(tr("This change would require a client restart."));
        // clear non-persistent status label after 10 seconds
        // Todo: should perhaps be a class attribute, if we extend the use of statusLabel
        QTimer::singleShot(10000, this, SLOT(clearStatusLabel()));
    }
}

void OptionsDialog::clearStatusLabel()
{
    ui->statusLabel->clear();
}

void OptionsDialog::doProxyIpChecks(QValidatedLineEdit* pUiProxyIp, int nProxyPort)
{
    Q_UNUSED(nProxyPort);

    const std::string strAddrProxy = pUiProxyIp->text().toStdString();
    CService addrProxy;

    /* Check for a valid IPv4 / IPv6 address */
    if (!(fProxyIpValid = LookupNumeric(strAddrProxy.c_str(), addrProxy))) {
        disableOkButton();
        pUiProxyIp->setValid(false);
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("The supplied proxy address is invalid."));
    } else {
        enableOkButton();
        ui->statusLabel->clear();
    }
}

bool OptionsDialog::eventFilter(QObject* object, QEvent* event)
{
    if (event->type() == QEvent::FocusOut) {
        if (object == ui->proxyIp) {
            emit proxyIpChecks(ui->proxyIp, ui->proxyPort->text().toInt());
        }
    }
    return QDialog::eventFilter(object, event);
}

QValidator::State LessThanValidator::validate(QString& input, int& pos) const
{
    QValidator::State ret = QIntValidator::validate(input, pos);
    bool clearError = true;
    if (ret == QValidator::Acceptable) {
        if (other) {
            bool ok, ok2 = false;
            int otherVal = other->text().toInt(&ok); // try to convert to an int
            int myVal = input.toInt(&ok2);
            if (ok && ok2) {
                if (myVal > otherVal) {
                    clearError = false;
                    if (errorDisplay)
                        errorDisplay->setText("<span style=\"color:#aa0000;\">Average must be less than or equal Maximum</span>");
                }
            }
        }
    }
    if (clearError && errorDisplay)
        errorDisplay->setText("");
    return ret;
}
