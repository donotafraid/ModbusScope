#include "modbusscope/scopeWidget.h"

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSizePolicy>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace modbusscope {
namespace {

constexpr int kRefreshMinMs = 16;
constexpr int kRefreshMaxMs = 2000;
constexpr int kDefaultRefreshMs = 100;

QColor channelColor(std::size_t index) {
    static const char *const palette[] = {
        "#22d3ee", "#f59e0b", "#a78bfa", "#fb7185",
        "#34d399", "#60a5fa", "#f472b6", "#facc15",
    };
    return QColor(QString::fromLatin1(palette[index % 8]));
}

QString formatValue(double value) {
    if (!std::isfinite(value)) {
        return QStringLiteral("n/a");
    }
    const double magnitude = std::fabs(value);
    if (magnitude >= 100000.0 || (magnitude > 0.0 && magnitude < 0.001)) {
        return QString::number(value, 'e', 3);
    }
    return QString::number(value, 'f', magnitude >= 100.0 ? 1 : 3);
}

QString formatTime(double seconds) {
    if (std::fabs(seconds) >= 60.0) {
        return QString::number(seconds / 60.0, 'f', 1) + "m";
    }
    if (std::fabs(seconds) >= 1.0) {
        return QString::number(seconds, 'f', 1) + "s";
    }
    return QString::number(seconds * 1000.0, 'f', 0) + "ms";
}

} // namespace

class ScopeWidget::Impl {
public:
    explicit Impl(ScopeWidget *owner) : q(owner) {
        timer.setInterval(kDefaultRefreshMs);
        QObject::connect(&timer, &QTimer::timeout, q,
                         [this] { pull(); });
        timer.start();
    }

    void setModel(std::shared_ptr<ScopeModel> next) {
        model = std::move(next);
        pull();
    }

    void clearModel() {
        model.reset();
        pull();
    }

    void pull() {
        if (model) {
            snapshot = model->snapshot();
        } else {
            snapshot = ScopeSnapshot{};
        }
        presentation = present(snapshot);
        q->update();
    }

    ScopeWidget *q = nullptr;
    QTimer timer;
    std::shared_ptr<ScopeModel> model;
    ScopeSnapshot snapshot;
    ScopePresentation presentation;
    bool hover = false;
    QPointF hoverPos;
};

ScopeWidget::ScopeWidget(QWidget *parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>(this)) {
    setAutoFillBackground(false);
    setMouseTracking(true);
    setMinimumSize(480, 300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

ScopeWidget::~ScopeWidget() = default;

void ScopeWidget::setModel(std::shared_ptr<ScopeModel> model) {
    m_impl->setModel(std::move(model));
}

void ScopeWidget::clearModel() { m_impl->clearModel(); }

ScopeSnapshot ScopeWidget::snapshot() const { return m_impl->snapshot; }

ScopePresentation ScopeWidget::presentation() const {
    return m_impl->presentation;
}

QString ScopeWidget::statusText() const {
    return QString::fromUtf8(m_impl->presentation.label);
}

void ScopeWidget::setRefreshIntervalMs(int intervalMs) {
    const int clamped =
        std::clamp(intervalMs, kRefreshMinMs, kRefreshMaxMs);
    m_impl->timer.setInterval(clamped);
}

int ScopeWidget::refreshIntervalMs() const {
    return m_impl->timer.interval();
}

QSize ScopeWidget::minimumSizeHint() const { return QSize(640, 360); }

QSize ScopeWidget::sizeHint() const { return QSize(960, 540); }

void ScopeWidget::mouseMoveEvent(QMouseEvent *event) {
    m_impl->hover = true;
    m_impl->hoverPos = event->localPos();
    QWidget::mouseMoveEvent(event);
}

void ScopeWidget::leaveEvent(QEvent *event) {
    m_impl->hover = false;
    update();
    QWidget::leaveEvent(event);
}

void ScopeWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF widgetRect = rect();
    QLinearGradient background(widgetRect.topLeft(),
                               widgetRect.bottomRight());
    background.setColorAt(0.0, QColor("#0b1120"));
    background.setColorAt(1.0, QColor("#111827"));
    painter.fillRect(widgetRect, background);

    const ScopeSnapshot &snap = m_impl->snapshot;
    const ScopePresentation &view = m_impl->presentation;

    const QRectF plot(72.0, 58.0, width() - 92.0, height() - 112.0);
    if (plot.width() <= 8.0 || plot.height() <= 8.0) {
        return;
    }

    // --- data range -------------------------------------------------------
    bool hasSamples = false;
    double valueMin = std::numeric_limits<double>::max();
    double valueMax = std::numeric_limits<double>::lowest();
    std::int64_t timeMin = std::numeric_limits<std::int64_t>::max();
    std::int64_t timeMax = std::numeric_limits<std::int64_t>::lowest();
    for (const auto &channel : snap.channels) {
        for (const auto &sample : channel.samples) {
            hasSamples = true;
            valueMin = std::min(valueMin, sample.value);
            valueMax = std::max(valueMax, sample.value);
            timeMin = std::min(timeMin, sample.timestampMs);
            timeMax = std::max(timeMax, sample.timestampMs);
        }
    }

    // --- grid and axes ----------------------------------------------------
    painter.setPen(QPen(QColor("#1f2937"), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(plot.adjusted(-0.5, -0.5, 0.5, 0.5), 8.0, 8.0);

    if (hasSamples) {
        if (valueMax - valueMin < 1e-9) {
            const double pad = std::max(1.0, std::fabs(valueMax) * 0.1);
            valueMin -= pad;
            valueMax += pad;
        }
        if (timeMax <= timeMin) {
            timeMax = timeMin + 1;
        }
        const double yRange = valueMax - valueMin;
        const double tRange = static_cast<double>(timeMax - timeMin);

        const QFont axisFont = font();
        painter.setFont(axisFont);
        const QFontMetrics metrics(axisFont);

        for (int line = 0; line <= 5; ++line) {
            const double y = plot.top() + plot.height() * line / 5.0;
            painter.setPen(QPen(QColor("#1f2937"), 1.0, Qt::DotLine));
            painter.drawLine(QPointF(plot.left(), y),
                             QPointF(plot.right(), y));
            const double value =
                valueMax - yRange * line / 5.0;
            painter.setPen(QColor("#64748b"));
            const QString text = formatValue(value);
            painter.drawText(
                QRectF(0.0, y - 10.0, plot.left() - 10.0, 20.0),
                Qt::AlignRight | Qt::AlignVCenter, text);
        }

        for (int line = 0; line <= 6; ++line) {
            const double x = plot.left() + plot.width() * line / 6.0;
            painter.setPen(QPen(QColor("#1f2937"), 1.0, Qt::DotLine));
            painter.drawLine(QPointF(x, plot.top()),
                             QPointF(x, plot.bottom()));
            const double seconds =
                tRange * line / 6.0 / 1000.0;
            painter.setPen(QColor("#64748b"));
            const QString text = "-" + formatTime(seconds);
            painter.drawText(
                QRectF(x - 40.0, plot.bottom() + 6.0, 80.0, 20.0),
                Qt::AlignHCenter | Qt::AlignTop, text);
        }

        // --- traces -------------------------------------------------------
        painter.setClipRect(plot.adjusted(1.0, 1.0, -1.0, -1.0));
        for (std::size_t index = 0; index < snap.channels.size(); ++index) {
            const auto &channel = snap.channels[index];
            if (channel.samples.empty()) {
                continue;
            }
            QPolygonF points;
            points.reserve(static_cast<int>(channel.samples.size()));
            for (const auto &sample : channel.samples) {
                const double x =
                    plot.left() +
                    (static_cast<double>(sample.timestampMs - timeMin) /
                     tRange) *
                        plot.width();
                const double y =
                    plot.bottom() -
                    ((sample.value - valueMin) / yRange) * plot.height();
                points.push_back(QPointF(x, y));
            }
            painter.setPen(QPen(channelColor(index), 1.8));
            painter.drawPolyline(points);
        }
        painter.setClipping(false);

        // --- hover crosshair ---------------------------------------------
        if (m_impl->hover && plot.contains(m_impl->hoverPos)) {
            const double ratio =
                (m_impl->hoverPos.x() - plot.left()) / plot.width();
            const std::int64_t hoverTime =
                timeMin + static_cast<std::int64_t>(ratio * tRange);
            painter.setPen(QPen(QColor("#94a3b8"), 1.0, Qt::DashLine));
            painter.drawLine(QPointF(m_impl->hoverPos.x(), plot.top()),
                             QPointF(m_impl->hoverPos.x(), plot.bottom()));
            painter.setPen(QColor("#cbd5e1"));
            painter.drawText(
                QRectF(m_impl->hoverPos.x() - 42.0, plot.top() - 22.0, 84.0,
                       20.0),
                Qt::AlignHCenter | Qt::AlignBottom,
                formatTime(static_cast<double>(hoverTime - timeMin) / 1000.0));

            QString tooltip;
            for (std::size_t index = 0; index < snap.channels.size(); ++index) {
                const auto &channel = snap.channels[index];
                if (channel.samples.empty()) {
                    continue;
                }
                const auto best = std::min_element(
                    channel.samples.begin(), channel.samples.end(),
                    [hoverTime](const ScopeSample &a, const ScopeSample &b) {
                        return std::llabs(a.timestampMs - hoverTime) <
                               std::llabs(b.timestampMs - hoverTime);
                    });
                if (tooltip.isEmpty()) {
                    tooltip = channel.config.name.c_str();
                } else {
                    tooltip += "\n";
                    tooltip += channel.config.name.c_str();
                }
                tooltip += " = " + formatValue(best->value);
            }
            if (!tooltip.isEmpty()) {
                const QFontMetrics metrics(font());
                const QRectF box =
                    metrics.boundingRect(QRect(0, 0, 240, 200),
                                         Qt::AlignLeft | Qt::AlignTop,
                                         tooltip)
                        .adjusted(-8.0, -6.0, 8.0, 6.0);
                QPointF origin(m_impl->hoverPos.x() + 14.0,
                               m_impl->hoverPos.y() + 14.0);
                if (origin.x() + box.width() > width() - 8.0) {
                    origin.setX(m_impl->hoverPos.x() - box.width() - 14.0);
                }
                if (origin.y() + box.height() > height() - 8.0) {
                    origin.setY(m_impl->hoverPos.y() - box.height() - 14.0);
                }
                const QRectF target(origin, box.size());
                painter.setPen(QPen(QColor("#334155"), 1.0));
                painter.setBrush(QColor(15, 23, 42, 235));
                painter.drawRoundedRect(target, 6.0, 6.0);
                painter.setPen(QColor("#e2e8f0"));
                painter.drawText(target, Qt::AlignLeft | Qt::AlignVCenter,
                                 tooltip);
            }
        }
    } else {
        painter.setPen(QColor("#64748b"));
        painter.drawText(plot, Qt::AlignCenter,
                         QStringLiteral("No samples yet.\n"
                                        "Start the model to acquire data."));
    }

    // --- legend -----------------------------------------------------------
    double legendX = 12.0;
    double legendY = 12.0;
    const QFontMetrics metrics(font());
    for (std::size_t index = 0; index < snap.channels.size(); ++index) {
        const auto &channel = snap.channels[index];
        const QString label =
            QString::fromStdString(channel.config.name) + "  " +
            (channel.hasValue ? formatValue(channel.latest)
                              : QStringLiteral("--"));
        const int width = metrics.horizontalAdvance(label) + 26;
        if (legendX + width > this->width() - 180.0) {
            legendX = 12.0;
            legendY += 22.0;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(channelColor(index));
        painter.drawEllipse(QPointF(legendX + 5.0, legendY + 8.0), 5.0,
                            5.0);
        painter.setPen(QColor("#cbd5e1"));
        painter.drawText(QRectF(legendX + 16.0, legendY, width, 18.0),
                         Qt::AlignLeft | Qt::AlignVCenter, label);
        legendX += width + 12.0;
    }

    // --- status pill ------------------------------------------------------
    const QString status = QString::fromUtf8(view.label);
    const QColor statusColor(QString::fromLatin1(view.color));
    const int statusWidth = metrics.horizontalAdvance(status) + 28;
    const QRectF pill(this->width() - statusWidth - 14.0, 12.0,
                      statusWidth, 26.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(statusColor.red(), statusColor.green(),
                            statusColor.blue(), 40));
    painter.drawRoundedRect(pill, 13.0, 13.0);
    painter.setPen(statusColor);
    painter.drawText(pill, Qt::AlignCenter, status);

    // --- error and hint ---------------------------------------------------
    if (view.showError) {
        const QString error =
            snap.lastError.empty()
                ? QString::fromUtf8(view.hint)
                : QString::fromStdString(snap.lastError);
        painter.setPen(QColor("#fca5a5"));
        painter.drawText(
            QRectF(plot.left(), height() - 40.0, plot.width(), 32.0),
            Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(error, Qt::ElideRight,
                               static_cast<int>(plot.width())));
    } else {
        painter.setPen(QColor("#64748b"));
        painter.drawText(
            QRectF(plot.left(), height() - 40.0, plot.width(), 32.0),
            Qt::AlignLeft | Qt::AlignVCenter,
            QString::fromUtf8(view.hint));
    }
}

} // namespace modbusscope
