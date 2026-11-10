#pragma once
#include <QWidget>

#include <memory>

#include "modbusscope/scope.h"

namespace modbusscope {

// Qt Widgets view for a ScopeModel. All model reads happen through
// ScopeModel::snapshot(); the widget never touches ScopeModel::Impl.
//
// ScopeWidget is a QWidget: create, use and destroy it on the GUI thread.
// The model it points to may be driven from another thread because
// ScopeModel::snapshot() is internally synchronized.
//
// The widget is intentionally a thin projection:
//   ScopeModel::Impl state  ->  ScopeSnapshot  ->  ScopePresentation -> paint
//   Client::Impl LifeState  ->  ScopeSnapshot::transport -> ScopePresentation
class ScopeWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ScopeWidget(QWidget *parent = nullptr);
    ~ScopeWidget() override;

    ScopeWidget(const ScopeWidget &) = delete;
    ScopeWidget &operator=(const ScopeWidget &) = delete;
    ScopeWidget(ScopeWidget &&) = delete;
    ScopeWidget &operator=(ScopeWidget &&) = delete;

    // The widget shares ownership so a model can outlive a temporary window.
    void setModel(std::shared_ptr<ScopeModel> model);
    void clearModel();

    ScopeSnapshot snapshot() const;
    ScopePresentation presentation() const;
    QString statusText() const;

    // 16..2000 ms; the default is 100 ms.
    void setRefreshIntervalMs(int intervalMs);
    int refreshIntervalMs() const;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace modbusscope
