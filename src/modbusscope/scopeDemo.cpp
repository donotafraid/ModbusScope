// Minimal interactive Scope demo. It is deliberately a thin shell around
// ScopeModel + ScopeWidget so the state mapping lives in the library, not in
// main().
#include "modbusscope/scope.h"
#include "modbusscope/scopeWidget.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QStatusBar>
#include <QString>
#include <QTimer>
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

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("modbusScope"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QString host = QStringLiteral("127.0.0.1");
    std::uint16_t port = 502;
    int pollIntervalMs = 250;
    QString screenshotPath;

    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--host") && index + 1 < argc) {
            host = QString::fromLocal8Bit(argv[++index]);
        } else if (argument == QLatin1String("--port") && index + 1 < argc) {
            port = static_cast<std::uint16_t>(
                std::atoi(argv[++index]));
        } else if (argument == QLatin1String("--interval") &&
                   index + 1 < argc) {
            pollIntervalMs = std::atoi(argv[++index]);
        } else if (argument == QLatin1String("--screenshot") &&
                   index + 1 < argc) {
            screenshotPath = QString::fromLocal8Bit(argv[++index]);
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
    auto *layout = new QVBoxLayout(central);

    auto *scope = new modbusscope::ScopeWidget(central);
    scope->setModel(model);
    layout->addWidget(scope, 1);

    auto *controls = new QHBoxLayout();
    auto *startButton = new QPushButton(QStringLiteral("Start"), central);
    auto *pauseButton = new QPushButton(QStringLiteral("Pause"), central);
    auto *clearButton = new QPushButton(QStringLiteral("Clear"), central);
    auto *quitButton = new QPushButton(QStringLiteral("Quit"), central);
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
    window.resize(1100, 700);

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
                         const auto result =
                             paused ? model->resume() : model->pause();
                         if (result.is_fail()) {
                             window.statusBar()->showMessage(
                                 QString::fromStdString(
                                     result.get_error()->context()),
                                 5000);
                         }
                     });
    QObject::connect(clearButton, &QPushButton::clicked, &window,
                     [&model] { (void)model->clear(); });
    QObject::connect(quitButton, &QPushButton::clicked, &window,
                     [&window] { window.close(); });

    auto *refresh = new QTimer(&window);
    refresh->setInterval(100);
    QObject::connect(refresh, &QTimer::timeout, &window,
                     [&model, startButton, pauseButton, clearButton,
                      errorLabel, &window, scope] {
                         const auto snapshot = model->snapshot();
                         const auto view = modbusscope::present(snapshot);
                         startButton->setEnabled(view.canStart);
                         pauseButton->setEnabled(view.canPause ||
                                                 view.canResume);
                         pauseButton->setText(
                             snapshot.run ==
                                     modbusscope::ScopeRunState::Paused
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
                         window.statusBar()->showMessage(
                             transportText(snapshot.transport) +
                             QStringLiteral(" / ") +
                             QString::fromUtf8(
                                 modbusscope::to_string(snapshot.run)) +
                             QStringLiteral(" / samples=") +
                             QString::number(
                                 snapshot.channels.empty()
                                     ? 0
                                     : snapshot.channels.front()
                                           .samples.size()));
                     });
    refresh->start();

    window.show();
    model->start();
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
