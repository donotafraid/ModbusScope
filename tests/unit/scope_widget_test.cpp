#include "FakeScopeSource.h"

#include "modbusscope/scope.h"
#include "modbusscope/scopeWidget.h"

#include <QApplication>
#include <QByteArray>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QtGlobal>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool waitUntil(const std::function<bool()> &predicate,
               std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

modbusscope::ScopeChannelConfig channel(std::string name) {
    modbusscope::ScopeChannelConfig value;
    value.name = std::move(name);
    value.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    value.unitId = 1;
    value.address = 0;
    return value;
}

std::shared_ptr<modbusscope::ScopeModel> makeModel() {
    auto source = std::make_unique<modbusscope::test::FakeScopeSource>();
    source->setValues({42.0});
    auto created = modbusscope::ScopeModel::createWithSource(
        std::move(source), {channel("pressure")}, 5, 20);
    EXPECT_TRUE(created.is_success());
    if (created.is_fail()) {
        return {};
    }
    return std::shared_ptr<modbusscope::ScopeModel>(
        std::move(*created.get()));
}

} // namespace

TEST(ScopeWidget, RendersLiveSnapshotOffscreen) {
    auto model = makeModel();
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->start().is_success());
    ASSERT_TRUE(waitUntil([&] {
        return model->snapshot().channels[0].samples.size() >= 2;
    }));

    modbusscope::ScopeWidget widget;
    widget.resize(800, 480);
    widget.setModel(model);
    widget.show();
    QApplication::processEvents();

    EXPECT_EQ(widget.statusText(), QStringLiteral("Live"));
    const auto snapshot = widget.snapshot();
    ASSERT_EQ(snapshot.channels.size(), 1u);
    EXPECT_EQ(snapshot.transport, modbusscope::LifeState::READY);

    QImage image(widget.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    painter.end();
    EXPECT_FALSE(image.isNull());
    EXPECT_EQ(image.size(), widget.size());

    ASSERT_TRUE(model->pause().is_success());
    std::this_thread::sleep_for(20ms);
    widget.setModel(model);
    EXPECT_EQ(widget.statusText(), QStringLiteral("Paused"));

    auto added = model->addChannel(channel("temperature"));
    ASSERT_TRUE(added.is_success());
    widget.setModel(model);
    QApplication::processEvents();
    const auto edited = widget.snapshot();
    ASSERT_EQ(edited.channels.size(), 2u);
    EXPECT_EQ(edited.channels[1].id, *added.get());
    EXPECT_EQ(edited.readTaskCount, 1u);

    QImage editedImage(widget.size(), QImage::Format_ARGB32);
    editedImage.fill(Qt::transparent);
    QPainter editedPainter(&editedImage);
    widget.render(&editedPainter);
    editedPainter.end();
    EXPECT_FALSE(editedImage.isNull());

    ASSERT_TRUE(model->removeChannel(*added.get()).is_success());
    widget.setModel(model);
    QApplication::processEvents();
    EXPECT_EQ(widget.snapshot().channels.size(), 1u);

    widget.clearModel();
    EXPECT_EQ(widget.statusText(), QStringLiteral("Stopped"));
    EXPECT_TRUE(model->stop().is_success());
}

int main(int argc, char **argv) {
    // gtest_discover_tests runs this binary with --gtest_list_tests before
    // CTest starts the real tests. Listing must not construct QApplication:
    // the offscreen platform plugin allocates process-lifetime state that LSan
    // reports during discovery.
    bool listOnly = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--gtest_list_tests") == 0) {
            listOnly = true;
            break;
        }
    }
    if (listOnly) {
        ::testing::InitGoogleTest(&argc, argv);
        return RUN_ALL_TESTS();
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    }
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
