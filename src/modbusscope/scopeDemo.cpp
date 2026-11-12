// Interactive Scope demo. The window is a thin shell around ScopeModel +
// ScopeWidget; read objects are added and removed through the model's paused
// editing API, not by rebuilding the model.
#include "modbusscope/scope.h"
#include "modbusscope/scopeWidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QString>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

QString transportText(modbusscope::LifeState state) {
    return QString::fromUtf8(modbusscope::to_string(state));
}

std::size_t channelColorSlot(modbusscope::ChannelId id) {
    return static_cast<std::size_t>(id == 0 ? 0 : id - 1);
}

QString functionText(modbusscope::FunctionCode function) {
    return function == modbusscope::FunctionCode::ReadInputRegisters
               ? QStringLiteral("0x04 Input")
               : QStringLiteral("0x03 Holding");
}

QString addressText(std::uint16_t address) {
    return QStringLiteral("0x") +
           QString::number(address, 16).rightJustified(4, QLatin1Char('0'))
               .toUpper();
}

QString channelText(const modbusscope::ScopeChannelConfig &channel) {
    return functionText(channel.function) + QStringLiteral(" · U") +
           QString::number(channel.unitId) + QStringLiteral(" · ") +
           addressText(channel.address);
}

QString readTaskText(std::size_t tasks) {
    return QString::number(tasks);
}

QString nextChannelName(const modbusscope::ScopeSnapshot &snapshot,
                        const modbusscope::ScopeChannelConfig &channel) {
    const QString stem =
        (channel.function ==
                 modbusscope::FunctionCode::ReadInputRegisters
             ? QStringLiteral("IR")
             : QStringLiteral("HR")) +
        QStringLiteral(" U") + QString::number(channel.unitId) +
        QStringLiteral(" @ ") + addressText(channel.address);
    QString candidate = stem;
    int suffix = 2;
    for (;;) {
        bool used = false;
        for (const auto &existing : snapshot.channels) {
            if (existing.config.name == candidate.toStdString()) {
                used = true;
                break;
            }
        }
        if (!used) {
            return candidate;
        }
        candidate = stem + QStringLiteral(" #") + QString::number(suffix++);
    }
}

class AddChannelDialog final : public QDialog {
public:
    explicit AddChannelDialog(QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Add read object"));
        setModal(true);

        m_function = new QComboBox(this);
        m_function->addItem(
            QStringLiteral("0x03 Read Holding Registers"),
            static_cast<int>(modbusscope::FunctionCode::ReadHoldingRegisters));
        m_function->addItem(
            QStringLiteral("0x04 Read Input Registers"),
            static_cast<int>(modbusscope::FunctionCode::ReadInputRegisters));

        m_address = new QSpinBox(this);
        m_address->setRange(0, 0xFFFF);
        m_address->setDisplayIntegerBase(16);
        m_address->setPrefix(QStringLiteral("0x"));
        m_address->setValue(0);

        m_unitId = new QSpinBox(this);
        m_unitId->setRange(1, 247);
        m_unitId->setValue(1);

        m_name = new QLineEdit(this);
        m_name->setPlaceholderText(
            QStringLiteral("optional; generated from function/address"));

        auto *form = new QFormLayout();
        form->addRow(QStringLiteral("Register type"), m_function);
        form->addRow(QStringLiteral("Address offset"), m_address);
        form->addRow(QStringLiteral("Unit id"), m_unitId);
        form->addRow(QStringLiteral("Name"), m_name);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                         &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);

        auto *layout = new QVBoxLayout(this);
        layout->addLayout(form);
        layout->addWidget(buttons);
    }

    modbusscope::ScopeChannelConfig channel() const {
        modbusscope::ScopeChannelConfig value;
        value.function = static_cast<modbusscope::FunctionCode>(
            m_function->currentData().toInt());
        value.address = static_cast<std::uint16_t>(m_address->value());
        value.unitId = static_cast<std::uint8_t>(m_unitId->value());
        value.name = m_name->text().trimmed().toStdString();
        return value;
    }

private:
    QComboBox *m_function = nullptr;
    QSpinBox *m_address = nullptr;
    QSpinBox *m_unitId = nullptr;
    QLineEdit *m_name = nullptr;
};

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("modbusScope"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));

    QString host = QStringLiteral("127.0.0.1");
    std::uint16_t port = 502;
    int pollIntervalMs = 250;
    bool startPaused = false;
    QString screenshotPath;

    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--host") && index + 1 < argc) {
            host = QString::fromLocal8Bit(argv[++index]);
        } else if (argument == QLatin1String("--port") && index + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++index]));
        } else if (argument == QLatin1String("--interval") &&
                   index + 1 < argc) {
            pollIntervalMs = std::atoi(argv[++index]);
        } else if (argument == QLatin1String("--screenshot") &&
                   index + 1 < argc) {
            screenshotPath = QString::fromLocal8Bit(argv[++index]);
        } else if (argument == QLatin1String("--paused")) {
            startPaused = true;
        }
    }

    modbusscope::ClientConfig config;
    config.endpoint.host = host.toStdString();
    config.endpoint.port = port;
    config.connectTimeoutMs = 3000;
    config.requestTimeoutMs = 1000;
    config.recoveryIntervalMs = 500;
    config.recoveryWindowMs = 5000;

    std::vector<modbusscope::ScopeChannelConfig> channels;
    for (std::uint16_t address = 0; address < 4; ++address) {
        modbusscope::ScopeChannelConfig channel;
        channel.name = "Reg " + std::to_string(address);
        channel.function =
            modbusscope::FunctionCode::ReadHoldingRegisters;
        channel.unitId = 1;
        channel.address = address;
        channels.push_back(std::move(channel));
    }

    auto created = modbusscope::ScopeModel::create(
        config, std::move(channels),
        static_cast<std::uint32_t>(pollIntervalMs), 600);
    if (created.is_fail()) {
        QMessageBox::critical(
            nullptr, QStringLiteral("modbusScope"),
            QString::fromStdString(created.get_error()->context()));
        return 1;
    }
    auto model =
        std::shared_ptr<modbusscope::ScopeModel>(std::move(*created.get()));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("modbusScope - Scope"));
    auto *central = new QWidget(&window);
    central->setObjectName(QStringLiteral("central"));
    central->setStyleSheet(QStringLiteral(
        "QFrame#channelCard { background: #111827; border: 1px solid #334155;"
        " border-radius: 14px; }"
        "QPushButton#channelChip { border: none; background: transparent;"
        " color: #e2e8f0; padding: 2px 4px; text-align: left; }"
        "QPushButton#channelChip:hover { color: #38bdf8; }"
        "QToolButton#channelRemove { border: none; color: #fda4af;"
        " font-weight: bold; }"
        "QToolButton#channelRemove:hover { background: #7f1d1d;"
        " border-radius: 10px; }"));

    auto *layout = new QVBoxLayout(central);
    auto *scope = new modbusscope::ScopeWidget(central);
    scope->setModel(model);
    layout->addWidget(scope, 1);

    auto *channelScroll = new QScrollArea(central);
    channelScroll->setWidgetResizable(true);
    channelScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    channelScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    channelScroll->setFixedHeight(58);
    channelScroll->setFrameShape(QFrame::NoFrame);
    auto *channelStrip = new QWidget(channelScroll);
    channelStrip->setObjectName(QStringLiteral("channelStrip"));
    auto *channelLayout = new QHBoxLayout(channelStrip);
    channelLayout->setContentsMargins(0, 4, 0, 4);
    channelLayout->setSpacing(8);
    channelLayout->setSizeConstraint(QLayout::SetMinimumSize);
    channelScroll->setWidget(channelStrip);
    layout->addWidget(channelScroll);

    auto *controls = new QHBoxLayout();
    auto *addButton =
        new QPushButton(QStringLiteral("Add read object"), central);
    auto *startButton = new QPushButton(QStringLiteral("Start"), central);
    auto *pauseButton = new QPushButton(QStringLiteral("Pause"), central);
    auto *clearButton = new QPushButton(QStringLiteral("Clear"), central);
    auto *quitButton = new QPushButton(QStringLiteral("Quit"), central);
    controls->addWidget(addButton);
    controls->addWidget(startButton);
    controls->addWidget(pauseButton);
    controls->addWidget(clearButton);
    controls->addStretch(1);
    controls->addWidget(quitButton);
    layout->addLayout(controls);

    auto *errorLabel = new QLabel(central);
    errorLabel->setStyleSheet(QStringLiteral("color: #fca5a5;"));
    errorLabel->setWordWrap(true);
    layout->addWidget(errorLabel);

    window.setCentralWidget(central);
    window.resize(1100, 740);

    QObject::connect(startButton, &QPushButton::clicked, &window,
                     [&model, &window] {
                         const auto started = model->start();
                         if (started.is_fail()) {
                             window.statusBar()->showMessage(
                                 QString::fromStdString(
                                     started.get_error()->context()),
                                 5000);
                         }
                     });
    QObject::connect(pauseButton, &QPushButton::clicked, &window,
                     [&model, &window] {
                         const bool paused =
                             model->run_state() ==
                             modbusscope::ScopeRunState::Paused;
                         QApplication::setOverrideCursor(Qt::WaitCursor);
                         const auto result =
                             paused ? model->resume() : model->pause();
                         QApplication::restoreOverrideCursor();
                         if (result.is_fail()) {
                             window.statusBar()->showMessage(
                                 QString::fromStdString(
                                     result.get_error()->context()),
                                 5000);
                         }
                     });
    QObject::connect(addButton, &QPushButton::clicked, &window,
                     [&model, &window] {
                         AddChannelDialog dialog(&window);
                         if (dialog.exec() != QDialog::Accepted) {
                             return;
                         }
                         auto channel = dialog.channel();
                         if (channel.name.empty()) {
                             channel.name =
                                 nextChannelName(model->snapshot(), channel)
                                     .toStdString();
                         }
                         const auto added =
                             model->addChannel(std::move(channel));
                         if (added.is_fail()) {
                             window.statusBar()->showMessage(
                                 QString::fromStdString(
                                     added.get_error()->context()),
                                 5000);
                         }
                     });
    QObject::connect(clearButton, &QPushButton::clicked, &window,
                     [&model] { (void)model->clear(); });
    QObject::connect(quitButton, &QPushButton::clicked, &window,
                     [&window] { window.close(); });

    QString channelSignature;
    auto rebuildChannels = [&](const modbusscope::ScopeSnapshot &snapshot,
                               const modbusscope::ScopePresentation &view) {
        QString signature = view.canEditChannels ? QStringLiteral("edit")
                                                 : QStringLiteral("view");
        for (const auto &channel : snapshot.channels) {
            signature += QStringLiteral("|%1:%2:%3:%4:%5")
                             .arg(channel.id)
                             .arg(static_cast<int>(channel.config.function))
                             .arg(channel.config.unitId)
                             .arg(channel.config.address)
                             .arg(QString::fromStdString(
                                 channel.config.name));
        }
        if (signature == channelSignature) {
            return;
        }
        channelSignature = std::move(signature);

        while (QLayoutItem *item = channelLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        if (snapshot.channels.empty()) {
            auto *empty = new QLabel(
                QStringLiteral("No read objects. Pause, then Add read object."),
                channelStrip);
            empty->setStyleSheet(QStringLiteral("color: #64748b;"));
            channelLayout->addWidget(empty);
            channelLayout->addStretch(1);
            channelStrip->adjustSize();
            return;
        }

        static const char *const palette[] = {
            "#22d3ee", "#f59e0b", "#a78bfa", "#fb7185",
            "#34d399", "#60a5fa", "#f472b6", "#facc15",
        };
        for (const auto &channel : snapshot.channels) {
            auto *card = new QFrame(channelStrip);
            card->setObjectName(QStringLiteral("channelCard"));
            auto *row = new QHBoxLayout(card);
            row->setContentsMargins(8, 2, 2, 2);
            row->setSpacing(2);

            auto *dot = new QLabel(card);
            dot->setFixedSize(8, 8);
            dot->setStyleSheet(
                QStringLiteral("background: %1; border-radius: 4px;")
                    .arg(QString::fromLatin1(
                        palette[channelColorSlot(channel.id) % 8])));
            auto *select = new QPushButton(
                channel.config.name.empty()
                    ? channelText(channel.config)
                    : QString::fromStdString(channel.config.name),
                card);
            select->setObjectName(QStringLiteral("channelChip"));
            select->setToolTip(channelText(channel.config));
            const QString label = select->text();
            QObject::connect(select, &QPushButton::clicked, &window,
                             [&window, label] {
                                 window.statusBar()->showMessage(
                                     QStringLiteral("Selected ") + label, 3000);
                             });

            auto *remove = new QToolButton(card);
            remove->setObjectName(QStringLiteral("channelRemove"));
            remove->setText(QString::fromUtf8("\xC3\x97"));
            remove->setFixedSize(22, 22);
            remove->setAutoRaise(true);
            remove->setToolTip(QStringLiteral("Delete ") + label);
            remove->setEnabled(view.canEditChannels);
            remove->setVisible(view.canEditChannels);
            const modbusscope::ChannelId id = channel.id;
            QObject::connect(remove, &QToolButton::clicked, &window,
                             [&model, &window, id] {
                                 const auto removed =
                                     model->removeChannel(id);
                                 if (removed.is_fail()) {
                                     window.statusBar()->showMessage(
                                         QString::fromStdString(
                                             removed.get_error()->context()),
                                         5000);
                                 }
                             });

            row->addWidget(dot);
            row->addWidget(select);
            row->addWidget(remove);
            channelLayout->addWidget(card);
        }
        channelLayout->addStretch(1);
        channelStrip->adjustSize();
    };

    auto *refresh = new QTimer(&window);
    refresh->setInterval(100);
    QObject::connect(
        refresh, &QTimer::timeout, &window,
        [&model, &window, addButton, startButton, pauseButton, clearButton,
         errorLabel, &rebuildChannels] {
            const auto snapshot = model->snapshot();
            const auto view = modbusscope::present(snapshot);
            rebuildChannels(snapshot, view);

            addButton->setEnabled(view.canEditChannels);
            startButton->setEnabled(view.canStart);
            pauseButton->setEnabled(view.canPause || view.canResume);
            pauseButton->setText(
                snapshot.run == modbusscope::ScopeRunState::Paused
                    ? QStringLiteral("Resume")
                    : QStringLiteral("Pause"));
            clearButton->setEnabled(view.canClear);
            errorLabel->setText(
                view.showError
                    ? QString::fromStdString(
                          snapshot.lastError.empty()
                              ? view.hint
                              : snapshot.lastError)
                    : QString());

            const std::size_t samples =
                snapshot.channels.empty()
                    ? 0
                    : snapshot.channels.front().samples.size();
            window.statusBar()->showMessage(
                transportText(snapshot.transport) + QStringLiteral(" / ") +
                QString::fromUtf8(modbusscope::to_string(snapshot.run)) +
                QStringLiteral(" / channels=") +
                QString::number(snapshot.channels.size()) +
                QStringLiteral(" / reads=") +
                readTaskText(snapshot.readTaskCount) +
                QStringLiteral(" / samples=") +
                QString::number(samples));
        });
    refresh->start();

    window.show();
    model->start();
    if (startPaused) {
        (void)model->pause();
    }
    if (!screenshotPath.isEmpty()) {
        QTimer::singleShot(2000, &window, [&app, &window, screenshotPath] {
            const bool saved = window.grab().save(screenshotPath);
            if (!saved) {
                window.statusBar()->showMessage(
                    QStringLiteral("could not save ") + screenshotPath, 5000);
            }
            app.quit();
        });
    }
    const int code = app.exec();
    (void)model->stop();
    return code;
}
