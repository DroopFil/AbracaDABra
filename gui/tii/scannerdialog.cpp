/*
 * This file is part of the AbracaDABra project
 *
 * MIT License
 *
 * Copyright (c) 2019-2025 Petr Kopecký <xkejpi (at) gmail (dot) com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <QDebug>
#include <QHeaderView>

#include "channelselectiondialog.h"
#include "ensembleconfigdialog.h"
#if (QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)) && QT_CONFIG(permissions)
#include <QPermissions>
#endif
#include <QCoreApplication>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QQmlContext>
#include <QSpinBox>

#include "dabtables.h"
#include "scannerdialog.h"
#include "settings.h"
#include "signalstatelabel.h"

Q_LOGGING_CATEGORY(scanner, "Scanner", QtDebugMsg)

ScannerDialog::ScannerDialog(Settings *settings, QWidget *parent) : TxMapDialog(settings, false, parent)
{
    // UI
    setWindowTitle(tr("DAB Scanning Tool"));
    setMinimumSize(QSize(800, 500));
    resize(QSize(900, 800));

    // Set window flags to add maximize and minimize buttons
    setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);

    m_sortedFilteredModel->setColumnsFilter(false);

    // QML View
    m_qmlView = new QQuickView();
    QQmlContext *context = m_qmlView->rootContext();
    context->setContextProperty("tiiBackend", this);
    context->setContextProperty("tiiTable", m_sortedFilteredModel);
    context->setContextProperty("tiiTableSelectionModel", m_tableSelectionModel);
    m_qmlView->setSource(QUrl("qrc:/app/qmlcomponents/map.qml"));

    QWidget *qmlContainer = QWidget::createWindowContainer(m_qmlView, this);

    QSizePolicy sizePolicyContainer(QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding);
    sizePolicyContainer.setVerticalStretch(255);
    qmlContainer->setSizePolicy(sizePolicyContainer);

    // scanner widget
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    QHBoxLayout *controlsLayout = new QHBoxLayout();

    m_progressChannel = new QLabel(this);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(15);

    m_channelListButton = new QPushButton(this);
    m_startStopButton = new QPushButton(this);
    m_startStopButton->setDefault(true);
    m_menuLabel = new ClickableLabel(this);

    QLabel *labelMode = new QLabel(this);
    labelMode->setText(tr("Mode:"));

    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(tr("Fast"), Mode_Fast);
    m_modeCombo->addItem(tr("Normal"), Mode_Normal);
    m_modeCombo->addItem(tr("Precise"), Mode_Precise);
    int idx = m_modeCombo->findData(m_settings->scanner.mode, Qt::UserRole);
    if (idx < 0)
    {  // set Mode_Normal as default
        idx = 1;
    }
    m_modeCombo->setCurrentIndex(idx);

    QLabel *labelCycles = new QLabel(this);
    m_numCyclesSpinBox = new QSpinBox(this);
    m_numCyclesSpinBox->setSpecialValueText(tr("Inf"));
    m_numCyclesSpinBox->setValue(m_settings->scanner.numCycles);
    labelCycles->setText(tr("Number of cycles:"));

    m_scanningLabel = new QLabel(this);
    m_scanningLabel->setAlignment(Qt::AlignmentFlag::AlignCenter);
    m_signalStateLabel = new SignalStateLabel();
    m_signalStateLabel->setAlignment(Qt::AlignmentFlag::AlignCenter);
    int w = m_progressChannel->fontMetrics().boundingRect(" 13C").width();
    m_progressChannel->setMinimumWidth(w);
    m_progressChannel->setAlignment(Qt::AlignmentFlag::AlignCenter);
    m_snrLine = new QFrame(this);
    m_snrLine->setFrameShape(QFrame::Shape::VLine);
    m_snrLine->setFrameShadow(QFrame::Shadow::Sunken);

    m_snrLabel = new QLabel(this);
    m_snrLabel->setText(tr("SNR:"));
    m_snrLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont boldFont;
    boldFont.setBold(true);
    m_snrLabel->setFont(boldFont);
    m_snrValue = new QLabel(this);
    w = m_snrValue->fontMetrics().boundingRect("36.0 dB").width();
    m_snrValue->setFixedWidth(w);
    m_snrValue->setAlignment(Qt::AlignmentFlag::AlignCenter);
    m_snrValue->setToolTip(QString(tr("DAB signal SNR")));
    m_snrValue->setText("");
    m_snrLabel->setToolTip(m_snrValue->toolTip());

    auto snrLayout = new QHBoxLayout();
    snrLayout->addWidget(m_scanningLabel);
    snrLayout->addWidget(m_progressChannel);
    snrLayout->addWidget(m_snrLine);
    snrLayout->addWidget(m_signalStateLabel);
    snrLayout->addWidget(m_snrLabel);
    snrLayout->addWidget(m_snrValue);
    controlsLayout->addLayout(snrLayout);

    controlsLayout->addItem(new QSpacerItem(40, 2, QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Minimum));
    controlsLayout->addWidget(labelMode);
    controlsLayout->addWidget(m_modeCombo);
    controlsLayout->addWidget(labelCycles);
    controlsLayout->addWidget(m_numCyclesSpinBox);

    controlsLayout->addWidget(m_channelListButton);
    controlsLayout->addWidget(m_startStopButton);
    controlsLayout->addWidget(m_menuLabel);

    mainLayout->addLayout(controlsLayout);

    mainLayout->addWidget(m_progressBar);

    m_txTableView = new QTableView(this);
    mainLayout->addWidget(m_txTableView);

    QWidget *scannerWidget = new QWidget(this);
    scannerWidget->setLayout(mainLayout);

    m_splitter = new QSplitter(this);
    m_splitter->setOrientation(Qt::Vertical);
    m_splitter->addWidget(qmlContainer);
    m_splitter->addWidget(scannerWidget);
    m_splitter->setChildrenCollapsible(false);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_splitter);

    m_startStopButton->setFocus();

    if (!m_settings->scanner.splitterState.isEmpty())
    {
        m_splitter->restoreState(m_settings->scanner.splitterState);
    }

    m_txTableView->setModel(m_sortedFilteredModel);
    m_txTableView->setSelectionModel(m_tableSelectionModel);
    m_txTableView->verticalHeader()->hide();
    m_txTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_txTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    m_txTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_txTableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_txTableView->setSortingEnabled(true);
    m_txTableView->sortByColumn(TxTableModel::ColTime, Qt::AscendingOrder);
    m_txTableView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_txTableView, &QTableView::customContextMenuRequested, this, &ScannerDialog::showContextMenu);
    connect(m_txTableView, &QTableView::doubleClicked, this, &ScannerDialog::showEnsembleConfig);

    m_startStopButton->setText(tr("Start"));
    connect(m_startStopButton, &QPushButton::clicked, this, &ScannerDialog::startStopClicked);
    m_progressBar->setMinimum(0);
    m_progressBar->setValue(0);
    m_progressChannel->setText("");
    m_progressChannel->setVisible(false);
    m_scanningLabel->setText("");
    m_channelListButton->setText(tr("Select channels"));
    m_signalStateLabel->setVisible(false);
    m_snrLabel->setVisible(false);
    m_snrValue->setVisible(false);
    m_snrLine->setVisible(false);

    auto menu = new QMenu(this);

    auto clearOnStartAction = new QAction(tr("Clear scan results on start"), menu);
    clearOnStartAction->setCheckable(true);
    clearOnStartAction->setChecked(m_settings->scanner.clearOnStart);
    connect(clearOnStartAction, &QAction::triggered, this, [this](bool checked) { m_settings->scanner.clearOnStart = checked; });
    menu->addAction(clearOnStartAction);

    auto hideLocalTx = new QAction(tr("Hide local (known) transmitters"), menu);
    hideLocalTx->setCheckable(true);
    hideLocalTx->setChecked(m_settings->scanner.hideLocalTx);
    m_sortedFilteredModel->setLocalTxFilter(m_settings->scanner.hideLocalTx);
    connect(hideLocalTx, &QAction::triggered, this,
            [this](bool checked)
            {
                m_sortedFilteredModel->setLocalTxFilter(checked);
                m_settings->scanner.hideLocalTx = checked;
            });
    menu->addAction(hideLocalTx);

    auto autoSave = new QAction(tr("AutoSave CSV"), menu);
    autoSave->setCheckable(true);
    autoSave->setChecked(m_settings->scanner.autoSave);
    connect(autoSave, &QAction::triggered, this, [this](bool checked) { m_settings->scanner.autoSave = checked; });
    menu->addAction(autoSave);

    menu->addSeparator();

    m_exportAction = new QAction(tr("Save as CSV"), menu);
    m_exportAction->setEnabled(false);
    connect(m_exportAction, &QAction::triggered, this, &ScannerDialog::exportClicked);
    menu->addAction(m_exportAction);
    m_importAction = new QAction(tr("Load from CSV"), menu);
    m_importAction->setEnabled(true);
    connect(m_importAction, &QAction::triggered, this, &ScannerDialog::importClicked);
    menu->addAction(m_importAction);

    m_clearTableAction = new QAction(tr("Clear scan results"), menu);
    m_clearTableAction->setEnabled(false);
    connect(m_model, &QAbstractTableModel::rowsInserted, this, [this]() { m_clearTableAction->setEnabled(true); });
    connect(m_clearTableAction, &QAction::triggered, this,
            [this]()
            {
                QMessageBox *msgBox = new QMessageBox(QMessageBox::Warning, tr("Warning"), tr("Do you want to clear scan results?"), {}, this);
                msgBox->setWindowModality(Qt::WindowModal);
                msgBox->setInformativeText(tr("You will loose current scan results, this action is irreversible."));
                msgBox->setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
                msgBox->setDefaultButton(QMessageBox::Cancel);
                connect(msgBox, &QMessageBox::finished, this,
                        [this, msgBox](int result)
                        {
                            if (result == QMessageBox::Ok)
                            {
                                QTimer::singleShot(10, this, [this]() { reset(); });
                            }
                            msgBox->deleteLater();
                        });
                msgBox->open();
            });
    menu->addAction(m_clearTableAction);

    auto clearLocalTx = new QAction(tr("Clear local (known) transmitter database"), menu);
    clearLocalTx->setEnabled(true);
    connect(clearLocalTx, &QAction::triggered, this,
            [this]()
            {
                QMessageBox *msgBox =
                    new QMessageBox(QMessageBox::Warning, tr("Warning"), tr("Do you want to clear local (known) transmitter database?"), {}, this);
                msgBox->setWindowModality(Qt::WindowModal);
                msgBox->setInformativeText(tr("You will loose all records in the database, this action is irreversible."));
                msgBox->setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
                msgBox->setDefaultButton(QMessageBox::Cancel);
                connect(msgBox, &QMessageBox::finished, this,
                        [this, msgBox](int result)
                        {
                            if (result == QMessageBox::Ok)
                            {
                                QTimer::singleShot(10, this, [this]() { m_model->clearLocalTx(); });
                            }
                            msgBox->deleteLater();
                        });
                msgBox->open();
            });
    menu->addAction(clearLocalTx);

    m_menuLabel->setMenu(menu);

    for (auto it = DabTables::channelList.cbegin(); it != DabTables::channelList.cend(); ++it)
    {
        if (m_settings->scanner.channelSelection.contains(it.key()))
        {
            m_channelSelection.insert(it.key(), m_settings->scanner.channelSelection.value(it.key()));
        }
        else
        {
            m_channelSelection.insert(it.key(), true);
        }
    }
    connect(m_channelListButton, &QPushButton::clicked, this, &ScannerDialog::channelSelectionClicked);

    m_model->loadLocalTxList(m_settings->filePath + "/LocalTx.json");

    m_ensemble.ueid = RADIO_CONTROL_UEID_INVALID;

    onSettingsChanged();

    setZoomLevel(m_settings->scanner.mapZoom);
    if (m_settings->scanner.mapCenter.isValid())
    {
        setMapCenter(m_settings->scanner.mapCenter);
    }
    setCenterToCurrentPosition(m_settings->scanner.centerMapToCurrentPosition);

    if (!m_settings->scanner.geometry.isEmpty())
    {
        restoreGeometry(m_settings->scanner.geometry);
    }
    setMinimumWidth(900);
}

ScannerDialog::~ScannerDialog()
{
    delete m_qmlView;

    if (nullptr != m_timer)
    {
        m_timer->stop();
        delete m_timer;
    }
}

void ScannerDialog::startStopClicked()
{
    if (m_isScanning)
    {  // stop pressed
        m_startStopButton->setEnabled(false);
        m_isScanning = false;
        m_ensemble.reset();

        // the state machine has 4 possible states
        // 1. wait for tune (event)
        // 2. wait for sync (timer or event)
        // 4. wait for ensemble (timer or event)
        // 5. wait for tii (timer)
        if (m_timer->isActive())
        {  // state 2, 3, 4
            m_timer->stop();
            stopScan();
        }
        else
        {
            // timer not running -> state 1
            m_state = ScannerState::Interrupted;  // ==> it will be finished when tune is complete
        }
    }
    else
    {  // start pressed
        m_startStopButton->setText(tr("Stop"));
        m_numCyclesSpinBox->setEnabled(false);
        m_modeCombo->setEnabled(false);
        m_numSelectedChannels = 0;
        m_isPreciseMode = (m_modeCombo->currentData().toInt() == Mode::Mode_Precise);
        for (const auto ch : m_channelSelection)
        {
            m_numSelectedChannels += ch ? 1 : 0;
        }
        if (m_numCyclesSpinBox->value() > 0)
        {
            m_progressBar->setMaximum(m_numSelectedChannels * m_numCyclesSpinBox->value());
        }
        else
        {
            m_progressBar->setMaximum(m_numSelectedChannels);
        }
        startScan();
    }
}

void ScannerDialog::stopScan()
{
    if (m_isTiiActive)
    {
        emit setTii(false);
        m_isTiiActive = false;
    }

    if (m_settings->scanner.autoSave)
    {  // auto save log
        QString fileName = QString("%1/%2_scan.csv").arg(m_settings->tii.logFolder, m_scanStartTime.toString("yyyy-MM-dd_hhmmss"));
        saveToFile(fileName);
    }

    if (m_exitRequested)
    {
        close();
    }

    // restore UI
    m_progressChannel->setVisible(false);
    m_signalStateLabel->setVisible(false);
    m_snrLabel->setVisible(false);
    m_snrValue->setVisible(false);
    m_snrLine->setVisible(false);
    m_scanningLabel->setText(tr("Scanning finished"));
    m_scanningLabel->setFont(QFont());
    m_progressBar->setValue(0);
    m_progressChannel->setText("");
    m_startStopButton->setText(tr("Start"));
    // adding timeout to avoid timing issues due to double click on start button
    m_startStopButton->setEnabled(false);
    QTimer::singleShot(2500, this, [this]() { m_startStopButton->setEnabled(true); });
    m_importAction->setEnabled(true);
    m_numCyclesSpinBox->setEnabled(true);
    m_channelListButton->setEnabled(true);
    m_modeCombo->setEnabled(true);

    m_isScanning = false;
    m_state = ScannerState::Idle;

    emit scanFinished();
}

void ScannerDialog::importClicked()
{
    if (m_model->rowCount() > 0)
    {
        QMessageBox *msgBox = new QMessageBox(QMessageBox::Warning, tr("Warning"), tr("Data in the table will be replaced."), {}, this);
        msgBox->setWindowModality(Qt::WindowModal);
        msgBox->setInformativeText(tr("You will loose current data, this action is irreversible."));
        msgBox->setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
        connect(msgBox, &QMessageBox::finished, this,
                [this, msgBox](int result)
                {
                    if (result == QMessageBox::Ok)
                    {
                        QTimer::singleShot(10, this, [this]() { loadCSV(); });
                    }
                    msgBox->deleteLater();
                });
        msgBox->open();
    }
    else
    {
        loadCSV();
    }
}

void ScannerDialog::loadCSV()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Load CSV file"), QDir::toNativeSeparators(m_settings->scanner.exportPath),
                                                    tr("CSV Files") + " (*.csv)");
    if (!fileName.isEmpty())
    {
        reset();

        qCInfo(scanner) << "Loading file:" << fileName;

        QFile file(fileName);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            QTextStream in(&file);
            bool timeIsUTC = in.readLine().contains("(UTC)");
            int lineNum = 2;
            bool res = true;
            while (!in.atEnd())
            {
                QString line = in.readLine();
                // qDebug() << line;

                QStringList qsl = line.split(';');
                if (qsl.size() == TxTableModel::NumColsWithoutCoordinates)
                {
#if (QT_VERSION < QT_VERSION_CHECK(6, 7, 0))
                    QDateTime time = QDateTime::fromString(qsl.at(TxTableModel::ColTime), "yyyy-MM-dd hh:mm:ss").addYears(100);
                    if (time.isValid() == false)
                    {  // old format
                        time = QDateTime::fromString(qsl.at(TxTableModel::ColTime), "yy-MM-dd hh:mm:ss").addYears(100);
                    }

#else
                    QDateTime time = QDateTime::fromString(qsl.at(TxTableModel::ColTime), "yyyy-MM-dd hh:mm:ss", 2000);
                    if (time.isValid() == false)
                    {  // old format
                        time = QDateTime::fromString(qsl.at(TxTableModel::ColTime), "yy-MM-dd hh:mm:ss", 2000);
                    }
#endif
                    if (!time.isValid())
                    {
                        qCWarning(scanner) << "Invalid time value" << qsl.at(TxTableModel::ColTime) << "line #" << lineNum;
                        res = false;
                        break;
                    }

                    if (timeIsUTC)
                    {
                        time.setTimeSpec(Qt::TimeSpec::UTC);
                    }
                    else
                    {
                        time.setTimeSpec(Qt::TimeSpec::LocalTime);
                    }

                    bool isOk = false;
                    uint32_t freq = qsl.at(TxTableModel::ColFreq).toUInt(&isOk);
                    if (!isOk)
                    {
                        qCWarning(scanner) << "Invalid frequency value" << qsl.at(TxTableModel::ColFreq) << "line #" << lineNum;
                        res = false;
                        break;
                    }

                    uint32_t ueid = qsl.at(TxTableModel::ColEnsId).toUInt(&isOk, 16);
                    if (!isOk)
                    {
                        qCWarning(scanner) << "Invalid UEID value" << qsl.at(TxTableModel::ColEnsId) << "line #" << lineNum;
                        res = false;
                        break;
                    }
                    int numServices = qsl.at(TxTableModel::ColNumServices).toInt(&isOk);
                    if (!isOk)
                    {
                        qCWarning(scanner) << "Invalid number of services value" << qsl.at(TxTableModel::ColNumServices) << "line #" << lineNum;
                        res = false;
                        break;
                    }

                    float snr = qsl.at(TxTableModel::ColSnr).toFloat(&isOk);
                    if (!isOk)
                    {
                        qCWarning(scanner) << "Invalid SNR value" << qsl.at(TxTableModel::ColSnr) << "line #" << lineNum;
                        res = false;
                        break;
                    }
                    QList<dabsdrTii_t> tiiList;
                    if (!qsl.at(TxTableModel::ColMainId).isEmpty())
                    {
                        uint8_t main = qsl.at(TxTableModel::ColMainId).toUInt(&isOk);
                        if (!isOk)
                        {
                            qCWarning(scanner) << "Invalid TII code" << qsl.at(TxTableModel::ColMainId) << "line #" << lineNum;
                            res = false;
                            break;
                        }
                        uint8_t sub = qsl.at(TxTableModel::ColSubId).toUInt(&isOk);
                        if (!isOk)
                        {
                            qCWarning(scanner) << "Invalid TII code" << qsl.at(TxTableModel::ColSubId) << "line #" << lineNum;
                            res = false;
                            break;
                        }
                        float level = qsl.at(TxTableModel::ColLevel).toFloat(&isOk);
                        if (!isOk)
                        {
                            qCWarning(scanner) << "Invalid TX level value" << qsl.at(TxTableModel::ColLevel) << "line #" << lineNum;
                            res = false;
                            break;
                        }
                        dabsdrTii_t tiiItem({.main = main, .sub = sub, .level = level});
                        tiiList.append(tiiItem);
                    }

                    m_model->appendEnsData(time.toLocalTime(), tiiList, ServiceListId(freq, ueid), qsl.at(TxTableModel::ColEnsLabel), "", "",
                                           numServices, snr);
                }
                else
                {
                    qCWarning(scanner) << "Unexpected number of cols, line #" << lineNum;
                    res = false;
                    break;
                }
                lineNum += 1;
            }
            file.close();

            if (res)
            {
                m_txTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
                m_txTableView->horizontalHeader()->setSectionResizeMode(TxTableModel::ColLocation, QHeaderView::Stretch);
            }
            else
            {
                qCWarning(scanner) << "Failed to load file:" << fileName;
                reset();
            }
        }
    }
}

void ScannerDialog::exportClicked()
{
    QString f = QString("%1/%2.csv").arg(m_settings->scanner.exportPath, m_scanStartTime.toString("yyyy-MM-dd_hhmmss"));

    QString fileName = QFileDialog::getSaveFileName(this, tr("Export CSV file"), QDir::toNativeSeparators(f), tr("CSV Files") + " (*.csv)");

    if (!fileName.isEmpty())
    {
        m_settings->scanner.exportPath = QFileInfo(fileName).path();  // store path for next time
        saveToFile(fileName);
    }
}

void ScannerDialog::saveToFile(const QString &fileName)
{
    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly))
    {
        QTextStream out(&file);

        auto exportRole =
            m_settings->tii.timestampInUTC ? TxTableModel::TxTableModelRoles::ExportRoleUTC : TxTableModel::TxTableModelRoles::ExportRole;

        // Header
        for (int col = 0; col < TxTableModel::NumColsWithoutCoordinates - 1; ++col)
        {
            out << m_model->headerData(col, Qt::Horizontal, exportRole).toString() << ";";
        }
        out << m_model->headerData(TxTableModel::NumColsWithoutCoordinates - 1, Qt::Horizontal, exportRole).toString() << Qt::endl;

        // Body
        for (int row = 0; row < m_model->rowCount(); ++row)
        {
            if (m_settings->scanner.hideLocalTx && m_model->data(m_model->index(row, 0), TxTableModel::IsLocalRole).toBool())
            {  // do not export local TX if local filter is set
                continue;
            }

            for (int col = 0; col < TxTableModel::NumColsWithoutCoordinates - 1; ++col)
            {
                out << m_model->data(m_model->index(row, col), exportRole).toString() << ";";
            }
            out << m_model->data(m_model->index(row, TxTableModel::NumColsWithoutCoordinates - 1), exportRole).toString() << Qt::endl;
        }
        file.close();
        qCInfo(scanner) << "Log was saved to file:" << fileName;
    }
}

void ScannerDialog::channelSelectionClicked()
{
    auto dialog = new ChannelSelectionDialog(m_channelSelection, this);
    connect(dialog, &QDialog::accepted, this, [this, dialog]() { dialog->getChannelList(m_channelSelection); });
    connect(dialog, &ChannelSelectionDialog::finished, dialog, &QObject::deleteLater);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->open();
    // dialog->setModal(true);
    // dialog->show();
}

void ScannerDialog::startScan()
{
    m_isScanning = true;

    if (m_settings->scanner.clearOnStart)
    {
        reset();
    }
    m_scanStartTime = QDateTime::currentDateTime();
    m_scanningLabel->setText(tr("Channel:"));
    QFont boldFont;
    boldFont.setBold(true);
    m_scanningLabel->setFont(boldFont);
    m_progressChannel->setVisible(true);
    m_importAction->setEnabled(false);
    m_channelListButton->setEnabled(false);
    m_signalStateLabel->reset();
    m_signalStateLabel->setVisible(true);
    m_snrLabel->setVisible(true);
    m_snrValue->setVisible(true);
    m_snrLine->setVisible(true);
    m_scanCycleCntr = 0;
    m_frequency = 0;

    if (m_timer == nullptr)
    {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &ScannerDialog::scanStep);
    }

    m_state = ScannerState::Init;

    // using timer for mainwindow to cleanup and tune to 0 potentially (no timeout in case)
#ifdef Q_OS_WIN
    m_timer->start(6000);
#else
    m_timer->start(2000);
#endif
    qCInfo(scanner) << "Scanning starts";

    emit scanStarts();
}

void ScannerDialog::scanStep()
{
    if (ScannerState::Init == m_state)
    {  // first step
        m_channelIt = DabTables::channelList.constBegin();
    }
    else
    {  // next step
        ++m_channelIt;
    }

    // find active channel
    while ((m_channelSelection.value(m_channelIt.key()) == false) && (DabTables::channelList.constEnd() != m_channelIt))
    {
        ++m_channelIt;
    }

    if (DabTables::channelList.constEnd() == m_channelIt)
    {
        if (++m_scanCycleCntr == m_numCyclesSpinBox->value())
        {  // scan finished
            stopScan();
            return;
        }

        // restarting
        m_channelIt = DabTables::channelList.constBegin();
        if (m_numCyclesSpinBox->value() == 0)
        {  // endless scan
            m_progressBar->setValue(0);
        }

        // find first active channel
        while ((m_channelSelection.value(m_channelIt.key()) == false) && (DabTables::channelList.constEnd() != m_channelIt))
        {
            ++m_channelIt;
        }
    }

    m_progressBar->setValue(m_progressBar->value() + 1);
    // m_progressChannel->setText(QString("%1 [ %2 MHz ]").arg(m_channelIt.value()).arg(m_channelIt.key()/1000.0, 3, 'f', 3, QChar('0')));
    // m_progressChannel->setText(QString("%1 (%2 / %3)").arg(m_channelIt.value()).arg(m_progressBar->value()).arg(m_progressBar->maximum()));
    if (m_numCyclesSpinBox->value() == 1)
    {
        m_progressChannel->setText(m_channelIt.value());
    }
    else
    {
        m_progressChannel->setText(QString(tr("%1  (cycle %2)")).arg(m_channelIt.value()).arg(m_scanCycleCntr + 1));
    }

    if (m_frequency != m_channelIt.key())
    {
        m_frequency = m_channelIt.key();
        m_numServicesFound = 0;
        m_ensemble.reset();
        m_state = ScannerState::WaitForTune;
        qCInfo(scanner) << "Tune:" << m_frequency;
        emit tuneChannel(m_frequency);
    }
    else
    {  // this is a case when only 1 channel is selected for scanning
        m_state = ScannerState::WaitForEnsemble;
        onEnsembleInformation(m_ensemble);
    }
}

void ScannerDialog::onTuneDone(uint32_t freq)
{
    switch (m_state)
    {
        case ScannerState::Init:
            if (m_timer->isActive())
            {
                m_timer->stop();
            }
            scanStep();
            break;
        case ScannerState::Interrupted:
            // exit
            stopScan();
            break;
        case ScannerState::WaitForTune:
            // tuned to some frequency -> wait for sync
            m_state = ScannerState::WaitForSync;
            m_timer->start(m_settings->scanner.waitForSync * 1000);
            qCDebug(scanner) << "Waiting for sync @" << m_frequency;
            break;
        default:
            // do nothing
            m_startStopButton->setEnabled(true);
            break;
    }
}

void ScannerDialog::onSignalState(uint8_t sync, float snr)
{
    m_signalStateLabel->setSignalState(sync, snr);
    m_snrValue->setText(QString("%1 dB").arg(snr, 0, 'f', 1));
    if (DabSyncLevel::NullSync <= DabSyncLevel(sync))
    {
        if (ScannerState::WaitForSync == m_state)
        {  // if we are waiting for sync (move to next step)
            m_timer->stop();
            m_state = ScannerState::WaitForEnsemble;
            m_timer->start(m_settings->scanner.waitForEnsemble * 1000);
            qCInfo(scanner) << "Signal found, waiting for ensemble info @" << m_frequency;
        }
    }
    if (m_ensemble.isValid() && m_isScanning)
    {
        m_snr += snr;
        m_snrCntr += 1;
    }
}

void ScannerDialog::onEnsembleInformation(const RadioControlEnsemble &ens)
{
    if (ScannerState::WaitForEnsemble != m_state)
    {  // do nothing
        return;
    }
    m_timer->stop();
    if (ens.isValid())
    {  // this shoud be the normal case
        m_state = ScannerState::WaitForTII;

        // this will stop when TII data is received
        m_timer->start(5000 + m_modeCombo->currentData().toInt() * 5000);

        m_ensemble = ens;
        qCInfo(scanner, "Ensemble info: %s %6.6X @ %d kHz, waiting for TII", m_ensemble.label.toUtf8().data(), m_ensemble.ueid, m_ensemble.frequency);

        m_snr = 0.0;
        m_snrCntr = 0;
        m_tiiCntr = 0;
        if (m_isTiiActive == false)
        {
            emit setTii(true);
            m_isTiiActive = true;
        }
    }
    else
    {  // this can happen in single channel mode in no signal case
        // wait for ensemble
        qCDebug(scanner) << "Invalid ensemble info, still waiting @" << m_frequency;
        m_timer->start(m_settings->scanner.waitForEnsemble * 1000);
    }
}

void ScannerDialog::onServiceListEntry(const RadioControlEnsemble &, const RadioControlServiceComponent &)
{
    if (m_state > ScannerState::WaitForEnsemble)
    {
        m_numServicesFound += 1;
    }
}

void ScannerDialog::onTiiData(const RadioControlTIIData &data)
{
    if ((ScannerState::WaitForTII == m_state) && m_ensemble.isValid())
    {
        qCDebug(scanner) << "TII data @" << m_frequency;
        if (++m_tiiCntr == m_modeCombo->currentData().toInt())
        {
            if (nullptr != m_timer && m_timer->isActive())
            {
                m_timer->stop();
            }

            if (m_isPreciseMode)
            {  // request ensemble info
                m_tiiData = data;
                qCDebug(scanner) << "Requesting ensemble config @" << m_frequency;
                emit requestEnsembleConfiguration();
            }
            else
            {
                storeEnsembleData(data, QString(), QString());
            }
        }
    }
}

void ScannerDialog::storeEnsembleData(const RadioControlTIIData &tiiData, const QString &conf, const QString &csvConf)
{
    qCDebug(scanner) << "Storing results @" << m_frequency;

    m_model->appendEnsData(QDateTime::currentDateTime(), tiiData.idList, ServiceListId(m_ensemble), m_ensemble.label, conf, csvConf,
                           m_numServicesFound, m_snr / m_snrCntr);
    m_exportAction->setEnabled(true);

    m_txTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_txTableView->horizontalHeader()->setSectionResizeMode(TxTableModel::ColLocation, QHeaderView::Stretch);

    if (m_isTiiActive && m_numSelectedChannels > 1)
    {
        emit setTii(false);
        m_isTiiActive = false;
    }

    // handle selection
    int id = -1;
    QModelIndexList selectedList = m_tableSelectionModel->selectedRows();
    if (!selectedList.isEmpty())
    {
        QModelIndex currentIndex = selectedList.at(0);
    }

    // forcing update of UI
    onSelectionChanged(QItemSelection(), QItemSelection());

    qCInfo(scanner) << "Done:" << m_frequency;

    // next channel
    scanStep();
}

void ScannerDialog::showEnsembleConfig(const QModelIndex &index)
{
    if (m_isPreciseMode)
    {
        QModelIndex srcIndex = m_sortedFilteredModel->mapToSource(index);
        if (srcIndex.isValid())
        {
            auto dialog = new EnsembleConfigDialog(m_model->itemAt(srcIndex.row()), this);
            connect(dialog, &QDialog::finished, this,
                    [this, dialog]()
                    {
                        m_settings->scanner.exportPath = dialog->exportPath();
                        dialog->deleteLater();
                    });

            dialog->setExportPath(m_settings->scanner.exportPath);
            dialog->setWindowModality(Qt::WindowModal);
            dialog->open();
        }
    }
}

void ScannerDialog::showContextMenu(const QPoint &pos)
{
    QMenu *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto selectedRows = m_tableSelectionModel->selectedRows();
    int isLocal = 0;
    for (auto it = selectedRows.cbegin(); it != selectedRows.cend(); ++it)
    {
        QModelIndex srcIndex = m_sortedFilteredModel->mapToSource(*it);
        if (srcIndex.isValid())
        {  // local is +1, not local is -1
            isLocal += m_model->data(srcIndex, TxTableModel::IsLocalRole).toBool() ? 1 : -1;
        }
    }
    // isLocal > 0 means that more Tx in selection is marked as local
    // isLocal < 0 means that more Tx in selection is NOT marked as local
    // isLocal = 0 means the same number of loc and NOT local => offering marking as local

    QAction *markAsLocalAction = new QAction(isLocal > 0 ? tr("Unmark local (known) transmitter") : tr("Mark as local (known) transmitter"), this);
    menu->addAction(markAsLocalAction);

    QAction *showEnsInfoAction = new QAction(tr("Show ensemble information"), this);
    showEnsInfoAction->setEnabled(m_isPreciseMode && m_tableSelectionModel->selectedRows().count() == 1);
    menu->addAction(showEnsInfoAction);

    connect(menu, &QWidget::destroyed, this, [=]() { // delete actions
                markAsLocalAction->deleteLater();
                showEnsInfoAction->deleteLater();
            });

    QAction *selectedItem = menu->exec(m_txTableView->viewport()->mapToGlobal(pos));
    if (selectedItem == showEnsInfoAction)
    {
        QModelIndex index = m_txTableView->indexAt(pos);
        if (index.isValid())
        {
            showEnsembleConfig(index);
        }
    }
    else if (selectedItem == markAsLocalAction)
    {
        for (auto it = selectedRows.cbegin(); it != selectedRows.cend(); ++it)
        {
            QModelIndex srcIndex = m_sortedFilteredModel->mapToSource(*it);
            if (srcIndex.isValid())
            {
                m_model->setAsLocalTx(srcIndex, isLocal <= 0);
            }
        }
    }
}

void ScannerDialog::onEnsembleConfigurationAndCSV(const QString &config, const QString &csvString)
{
    qCDebug(scanner) << "Ensemble config received @" << m_frequency;
    storeEnsembleData(m_tiiData, config, csvString);
}

void ScannerDialog::onInputDeviceError(const InputDevice::ErrorCode)
{
    if (m_isScanning)
    {  // stop pressed
        m_startStopButton->setEnabled(false);
        m_isScanning = false;
        m_ensemble.reset();

        // the state machine has 4 possible states
        // 1. wait for tune (event)
        // 2. wait for sync (timer or event)
        // 4. wait for ensemble (timer or event)
        // 5. wait for tii (timer)
        if (m_timer->isActive())
        {  // state 2, 3, 4
            m_timer->stop();
        }
        stopScan();
        m_scanningLabel->setText(tr("Scanning failed"));
        m_scanningLabel->setFont(QFont());
    }
}

void ScannerDialog::setupDarkMode(bool darkModeEna)
{
    if (darkModeEna)
    {
        m_qmlView->setColor(Qt::black);
        m_menuLabel->setIcon(":/resources/menu_dark.png");
    }
    else
    {
        m_qmlView->setColor(Qt::white);
        m_menuLabel->setIcon(":/resources/menu.png");
    }
}

void ScannerDialog::onSelectedRowChanged()
{
    m_txInfo.clear();
    m_currentEnsemble.reset();
    if (selectedRow() < 0)
    {  // reset info
        emit txInfoChanged();
        emit ensembleInfoChanged();
        return;
    }

    TxTableModelItem item = m_model->data(m_model->index(selectedRow(), 0), TxTableModel::TxTableModelRoles::ItemRole).value<TxTableModelItem>();
    if (item.hasTxData())
    {
        m_txInfo.append(QString("<b>%1</b>").arg(item.transmitterData().location()));
        QGeoCoordinate coord = QGeoCoordinate(item.transmitterData().coordinates().latitude(), item.transmitterData().coordinates().longitude());
        m_txInfo.append(QString("GPS: <b>%1</b>").arg(coord.toString(QGeoCoordinate::DegreesWithHemisphere)));
        float alt = item.transmitterData().coordinates().altitude();
        if (alt)
        {
            m_txInfo.append(QString(tr("Altitude: <b>%1 m</b>")).arg(static_cast<int>(alt)));
        }
        int antHeight = item.transmitterData().antHeight();
        if (antHeight)
        {
            m_txInfo.append(QString(tr("Antenna height: <b>%1 m</b>")).arg(static_cast<int>(antHeight)));
        }
        m_txInfo.append(QString(tr("ERP: <b>%1 kW</b>")).arg(static_cast<double>(item.transmitterData().power()), 3, 'f', 1));
    }
    emit txInfoChanged();

    m_currentEnsemble.label = item.ensLabel();
    m_currentEnsemble.ueid = item.ensId().ueid();
    m_currentEnsemble.frequency = item.ensId().freq();

    emit ensembleInfoChanged();

    m_txTableView->scrollTo(m_sortedFilteredModel->mapFromSource(m_model->index(selectedRow(), 0)));
}

void ScannerDialog::reset()
{
    TxMapDialog::reset();
    m_clearTableAction->setEnabled(false);
    m_exportAction->setEnabled(false);
}

void ScannerDialog::showEvent(QShowEvent *event)
{
    if (!isVisible())
    {
        m_txTableView->setMinimumWidth(700);
        m_txTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    }

    TxMapDialog::showEvent(event);
}

void ScannerDialog::closeEvent(QCloseEvent *event)
{
    if (m_isScanning)
    {  // stopping scanning first
        m_exitRequested = true;
        QTimer::singleShot(50, this, [this]() { startStopClicked(); });
        event->ignore();
        return;
    }

    m_settings->scanner.splitterState = m_splitter->saveState();
    m_settings->scanner.geometry = saveGeometry();
    m_settings->scanner.numCycles = m_numCyclesSpinBox->value();
    m_settings->scanner.channelSelection = m_channelSelection;
    m_settings->scanner.mode = m_modeCombo->currentData().toInt();

    m_settings->scanner.mapZoom = zoomLevel();
    m_settings->scanner.mapCenter = mapCenter();
    m_settings->scanner.centerMapToCurrentPosition = centerToCurrentPosition();

    TxMapDialog::closeEvent(event);
}
