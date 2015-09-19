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

OptionsDialog::OptionsDialog(QWidget *parent, bool enableWallet) :
    QDialog(parent),
    ui(new Ui::OptionsDialog),
    model(0),
    mapper(0),
    fProxyIpValid(true)
{
    ui->setupUi(this);

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
    ui->proxyPort->setValidator(new QIntValidator(1, 65535, this));

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
    foreach(const QString &langStr, translations.entryList())
    {
        QLocale locale(langStr);

        /** check if the locale name consists of 2 parts (language_country) */
        if(langStr.contains("_"))
        {
#if QT_VERSION >= 0x040800
            /** display language strings as "native language - native country (locale name)", e.g. "Deutsch - Deutschland (de)" */
            ui->lang->addItem(locale.nativeLanguageName() + QString(" - ") + locale.nativeCountryName() + QString(" (") + langStr + QString(")"), QVariant(langStr));
#else
            /** display language strings as "language - country (locale name)", e.g. "German - Germany (de)" */
            ui->lang->addItem(QLocale::languageToString(locale.language()) + QString(" - ") + QLocale::countryToString(locale.country()) + QString(" (") + langStr + QString(")"), QVariant(langStr));
#endif
        }
        else
        {
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
    connect(this, SIGNAL(proxyIpChecks(QValidatedLineEdit *, int)), this, SLOT(doProxyIpChecks(QValidatedLineEdit *, int)));

    uint64_t max,ave;
    sendShaper.get(&max,&ave);
    bool enabled = (ave != LONG_MAX);
    ui->sendShapingEnable->setChecked(enabled);
    ui->sendBurstSlider->setRange(0,400);
    ui->sendAveSlider->setRange(0,400);
    ui->recvBurstSlider->setRange(0,400);
    ui->recvAveSlider->setRange(0,400);

    connect(ui->sendBurstSlider, SIGNAL(valueChanged(int)), this, SLOT(shapingSliderChanged()));
    connect(ui->sendAveSlider, SIGNAL(valueChanged(int)), this, SLOT(shapingSliderChanged()));
    connect(ui->recvBurstSlider, SIGNAL(valueChanged(int)), this, SLOT(shapingSliderChanged()));
    connect(ui->recvAveSlider, SIGNAL(valueChanged(int)), this, SLOT(shapingSliderChanged()));

    if (enabled)
      {
	ui->sendBurstEdit->setText(QString(boost::lexical_cast<std::string>(max/1024).c_str()));
	ui->sendAveEdit->setText(QString(boost::lexical_cast<std::string>(ave/1024).c_str()));
        ui->sendBurstSlider->setValue(max/1024);
        ui->sendAveSlider->setValue(ave/1024);
      }

    receiveShaper.get(&max,&ave);
    enabled = (ave != LONG_MAX);
    ui->recvShapingEnable->setChecked(enabled);
    if (enabled)
      {
	ui->recvBurstEdit->setText(QString(boost::lexical_cast<std::string>(max/1024).c_str()));
	ui->recvAveEdit->setText(QString(boost::lexical_cast<std::string>(ave/1024).c_str()));
        ui->recvBurstSlider->setValue(max/1024);
        ui->recvAveSlider->setValue(ave/1024);
      }
    

    
}

OptionsDialog::~OptionsDialog()
{
    delete ui;
}


void OptionsDialog::shapingSliderChanged(void)
{
    int val = ui->sendBurstSlider->value();
    ui->sendBurstEdit->setText(QString(boost::lexical_cast<std::string>(val).c_str()));

    val = ui->sendAveSlider->value();
    ui->sendAveEdit->setText(QString(boost::lexical_cast<std::string>(val).c_str()));

    val = ui->recvBurstSlider->value();
    ui->recvBurstEdit->setText(QString(boost::lexical_cast<std::string>(val).c_str()));

    val = ui->recvAveSlider->value();
    ui->recvAveEdit->setText(QString(boost::lexical_cast<std::string>(val).c_str()));  
}

void OptionsDialog::setModel(OptionsModel *model)
{
    this->model = model;

    if(model)
    {
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
    connect(ui->thirdPartyTxUrls, SIGNAL(textChanged(const QString &)), this, SLOT(showRestartWarning()));
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
    if(fProxyIpValid)
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
    if(model)
    {
        // confirmation dialog
        QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm options reset"),
            tr("Client restart required to activate changes.") + "<br><br>" + tr("Client will be shut down. Do you want to proceed?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

        if(btnRetVal == QMessageBox::Cancel)
            return;

        /* reset all options and close GUI */
        model->Reset();
        QApplication::quit();
    }
}

void OptionsDialog::on_okButton_clicked()
{
    mapper->submit();

    if (ui->sendShapingEnable->isChecked())
      {
	uint64_t burst, ave;
	sendShaper.get(&burst, &ave);  // Get the original values

        QString s = ui->sendBurstEdit->text();  // Get the changed value
        bool ok=false;
        int t = s.toInt(&ok); // try to convert to an int
        if (ok && t>=0) burst = t*1024;    // if conversion worked, then use the new value

        s = ui->sendAveEdit->text();  // Do the same with the average
        t = s.toInt(&ok); 
        if (ok && t>=0) ave = t*1024;

        sendShaper.set(burst,ave);  // Set the new (or old) burst and average
      }
    else sendShaper.disable();

    if (ui->recvShapingEnable->isChecked())
      {
	uint64_t burst, ave;
	receiveShaper.get(&burst, &ave);  
        QString s = ui->recvBurstEdit->text();
        bool ok=false;
        int t = s.toInt(&ok);  //boost::lexical_cast<uint64_t>(s.data());
        if (ok && t>=0) burst = t*1024;
        s = ui->recvAveEdit->text();
        t = s.toInt(&ok); //boost::lexical_cast<uint64_t>(s.data());
	if (ok && t>=0) ave = t*1024;
        receiveShaper.set(burst,ave);
      }
    else receiveShaper.disable();
  
    accept();
}

void OptionsDialog::on_cancelButton_clicked()
{
    reject();
}

void OptionsDialog::showRestartWarning(bool fPersistent)
{
    ui->statusLabel->setStyleSheet("QLabel { color: red; }");

    if(fPersistent)
    {
        ui->statusLabel->setText(tr("Client restart required to activate changes."));
    }
    else
    {
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

void OptionsDialog::doProxyIpChecks(QValidatedLineEdit *pUiProxyIp, int nProxyPort)
{
    Q_UNUSED(nProxyPort);

    const std::string strAddrProxy = pUiProxyIp->text().toStdString();
    CService addrProxy;

    /* Check for a valid IPv4 / IPv6 address */
    if (!(fProxyIpValid = LookupNumeric(strAddrProxy.c_str(), addrProxy)))
    {
        disableOkButton();
        pUiProxyIp->setValid(false);
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("The supplied proxy address is invalid."));
    }
    else
    {
        enableOkButton();
        ui->statusLabel->clear();
    }
}

bool OptionsDialog::eventFilter(QObject *object, QEvent *event)
{
    if(event->type() == QEvent::FocusOut)
    {
        if(object == ui->proxyIp)
        {
            emit proxyIpChecks(ui->proxyIp, ui->proxyPort->text().toInt());
        }
    }
    return QDialog::eventFilter(object, event);
}
