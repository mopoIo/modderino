// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ModDragSlider.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace {

constexpr int FLASH_DURATION_MS = 2600;

constexpr qreal HANDLE_WIDTH = 9;
constexpr qreal HANDLE_HEIGHT = 20;

QColor lerpColor(const QColor &from, const QColor &to, qreal t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor(
        from.red() + static_cast<int>((to.red() - from.red()) * t),
        from.green() + static_cast<int>((to.green() - from.green()) * t),
        from.blue() + static_cast<int>((to.blue() - from.blue()) * t));
}

QString formatTimeoutShort(int seconds)
{
    if (seconds < 60)
    {
        return QString("%1s").arg(seconds);
    }
    if (seconds < 3600)
    {
        return QString("%1m").arg(seconds / 60);
    }
    if (seconds < 86400)
    {
        return QString("%1h").arg(seconds / 3600);
    }
    return QString("%1d").arg(seconds / 86400);
}

/// Maps a 60px band to an integer count within [1, maxCount]
int bandCount(qreal dx, qreal bandStart, int maxCount)
{
    return static_cast<int>(
        std::lround(1 + (dx - bandStart) / 60.0 * (maxCount - 1)));
}

}  // namespace

namespace chatterino {

ModDragSlider::ModDragSlider(BaseWidget *parent)
    : BaseWidget(parent)
{
    this->hide();
    this->setCursor(Qt::OpenHandCursor);

    this->flashTimer_.setSingleShot(true);
    this->flashTimer_.setInterval(FLASH_DURATION_MS);
    QObject::connect(&this->flashTimer_, &QTimer::timeout, this, [this] {
        this->flashText_.clear();
        this->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        this->hide();
    });
}

void ModDragSlider::showFor(const ChannelPtr &channel,
                            const MessagePtr &message, const QRect &rowRect)
{
    if (this->dragging_ || !this->flashText_.isEmpty())
    {
        return;
    }

    this->channel_ = channel;
    this->messageId_ = message->id;
    this->login_ = message->loginName;
    this->displayName_ = message->displayName.isEmpty()
                             ? message->loginName
                             : message->displayName;
    this->canTimeout_ =
        dynamic_cast<TwitchChannel *>(channel.get()) != nullptr;
    this->rowRect_ = rowRect;

    this->applyIdleGeometry();
    this->show();
    this->raise();
}

void ModDragSlider::hideIfIdle()
{
    if (!this->dragging_ && this->flashText_.isEmpty())
    {
        this->hide();
    }
}

bool ModDragSlider::isDragging() const
{
    return this->dragging_;
}

QString ModDragSlider::draggedMessageId() const
{
    return this->dragging_ ? this->messageId_ : QString();
}

qreal ModDragSlider::dragOffset() const
{
    return this->dragging_ ? this->deltaX_ : 0;
}

void ModDragSlider::applyIdleGeometry()
{
    auto height =
        std::min<int>(this->rowRect_.height(),
                      static_cast<int>(HANDLE_HEIGHT * this->scale()) + 2);
    auto y = this->rowRect_.top() + (this->rowRect_.height() - height) / 2;
    this->setGeometry(
        this->rowRect_.left(),
        y,
        static_cast<int>(HANDLE_WIDTH * this->scale()) + 2,
        height);
}

ModDragSlider::Zone ModDragSlider::zoneForDelta(qreal dx) const
{
    if (dx < DELETE_AT)
    {
        return Zone::None;
    }
    if (dx < TIMEOUT_AT || !this->canTimeout_)
    {
        return Zone::Delete;
    }
    if (dx < BAN_AT)
    {
        return Zone::Timeout;
    }
    return Zone::Ban;
}

int ModDragSlider::timeoutSecondsForDelta(qreal dx) const
{
    if (dx < 140)
    {
        return bandCount(dx, 80, 59);  // 1s..59s
    }
    if (dx < 200)
    {
        return bandCount(dx, 140, 59) * 60;  // 1m..59m
    }
    if (dx < 260)
    {
        return bandCount(dx, 200, 23) * 3600;  // 1h..23h
    }
    return bandCount(dx, 260, 14) * 86400;  // 1d..14d
}

QString ModDragSlider::labelForDelta(qreal dx) const
{
    switch (this->zoneForDelta(dx))
    {
        case Zone::Delete:
            return "Delete";
        case Zone::Timeout:
            return formatTimeoutShort(this->timeoutSecondsForDelta(dx));
        case Zone::Ban:
            return "Ban";
        case Zone::None:
        default:
            return {};
    }
}

QColor ModDragSlider::barColorForDelta(qreal dx) const
{
    const QColor dimAmber("#665020");
    const QColor amber("#f5a623");
    const QColor orange1("#f59523");
    const QColor orange2("#e86c23");
    const QColor red("#d43d3d");
    const QColor darkRed("#a01010");
    const QColor banRed("#8b0000");

    if (dx < 40)
    {
        return lerpColor(dimAmber, amber, dx / 40.0);
    }
    if (dx < 80 || !this->canTimeout_)
    {
        return amber;
    }
    if (dx < 140)
    {
        return lerpColor(amber, orange1, (dx - 80) / 60.0);
    }
    if (dx < 200)
    {
        return lerpColor(orange1, orange2, (dx - 140) / 60.0);
    }
    if (dx < 260)
    {
        return lerpColor(orange2, red, (dx - 200) / 60.0);
    }
    if (dx < 320)
    {
        return lerpColor(red, darkRed, (dx - 260) / 60.0);
    }
    return banRed;
}

void ModDragSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }

    this->dragging_ = true;
    this->deltaX_ = 0;
    this->dragStartGlobal_ = event->globalPosition();
    this->setGeometry(this->rowRect_);
    this->setCursor(Qt::ClosedHandCursor);
    this->grabKeyboard();
    this->update();
}

void ModDragSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (!this->dragging_)
    {
        return;
    }

    this->deltaX_ =
        std::max(0.0, event->globalPosition().x() - this->dragStartGlobal_.x());
    this->update();
    // The dragged message is painted with this offset by the parent view
    this->parentWidget()->update();
}

void ModDragSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !this->dragging_)
    {
        return;
    }

    this->commit();
}

void ModDragSlider::keyPressEvent(QKeyEvent *event)
{
    if (this->dragging_ && event->key() == Qt::Key_Escape)
    {
        this->cancelDrag();
        return;
    }

    BaseWidget::keyPressEvent(event);
}

void ModDragSlider::cancelDrag()
{
    this->dragging_ = false;
    this->deltaX_ = 0;
    this->releaseKeyboard();
    this->setCursor(Qt::OpenHandCursor);
    this->applyIdleGeometry();
    this->update();
    this->parentWidget()->update();
}

void ModDragSlider::commit()
{
    auto dx = this->deltaX_ / this->scale();
    auto zone = this->zoneForDelta(dx);
    auto seconds = this->timeoutSecondsForDelta(dx);

    this->dragging_ = false;
    this->deltaX_ = 0;
    this->releaseKeyboard();
    this->setCursor(Qt::OpenHandCursor);
    this->parentWidget()->update();

    auto *twitchChannel = dynamic_cast<TwitchChannel *>(this->channel_.get());
    auto *kickChannel = dynamic_cast<KickChannel *>(this->channel_.get());

    auto sendCommand = [this](const QString &command) {
        auto message = getApp()->getCommands()->execCommand(
            command, this->channel_, false);
        if (!message.isEmpty())
        {
            this->channel_->sendMessage(message);
        }
    };

    switch (zone)
    {
        case Zone::Delete: {
            if (twitchChannel != nullptr)
            {
                twitchChannel->deleteMessagesAs(
                    this->messageId_,
                    getApp()->getAccounts()->twitch.getCurrent().get());
            }
            else if (kickChannel != nullptr)
            {
                kickChannel->deleteMessage(this->messageId_);
            }
            this->startFlash(this->displayName_ + " deleted");
        }
        break;

        case Zone::Timeout: {
            sendCommand(QString("/timeout %1 %2")
                            .arg(this->login_)
                            .arg(seconds));
            this->startFlash(this->displayName_ + " timed out " +
                             formatTimeoutShort(seconds));
        }
        break;

        case Zone::Ban: {
            sendCommand(QString("/ban %1").arg(this->login_));
            this->startFlash(this->displayName_ + " banned");
        }
        break;

        case Zone::None:
        default: {
            this->applyIdleGeometry();
            this->update();
        }
        break;
    }
}

void ModDragSlider::startFlash(const QString &text)
{
    this->flashText_ = text;
    // Keep covering the row to show the confirmation, but let mouse events
    // through to the chat below.
    this->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    this->setGeometry(this->rowRect_);
    this->flashTimer_.start();
    this->update();
}

void ModDragSlider::drawHandle(QPainter &painter, qreal x)
{
    auto scale = this->scale();
    qreal w = HANDLE_WIDTH * scale;
    qreal h = std::min<qreal>(this->height(), HANDLE_HEIGHT * scale);
    qreal y = (this->height() - h) / 2.0;

    QPainterPath path;
    qreal radius = 3 * scale;
    // rounded on the right side only; attached flat to the left edge
    path.moveTo(x, y);
    path.lineTo(x + w - radius, y);
    path.quadTo(x + w, y, x + w, y + radius);
    path.lineTo(x + w, y + h - radius);
    path.quadTo(x + w, y + h, x + w - radius, y + h);
    path.lineTo(x, y + h);
    path.closeSubpath();

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(path, QColor("#18181b"));
    painter.setPen(QPen(QColor(255, 255, 255, 153), 1));
    painter.drawPath(path);

    // three grip dots
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#a3a3a3"));
    qreal dotRadius = 1.0 * scale;
    qreal cx = x + w / 2.0;
    for (int i = -1; i <= 1; i++)
    {
        qreal cy = y + h / 2.0 + i * 3.5 * scale;
        painter.drawEllipse(QPointF(cx, cy), dotRadius, dotRadius);
    }
}

void ModDragSlider::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    auto scale = this->scale();

    if (!this->flashText_.isEmpty())
    {
        auto font = this->font();
        font.setBold(true);
        font.setPixelSize(static_cast<int>(10 * scale));
        painter.setFont(font);
        painter.setPen(QColor("#a3a3a3"));
        auto textRect = this->rect().adjusted(
            0, 0, -static_cast<int>(16 * scale), 0);
        painter.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter,
                         this->flashText_);
        return;
    }

    if (this->dragging_)
    {
        auto dx = this->deltaX_ / scale;
        if (this->deltaX_ > 0)
        {
            QRectF barRect(0, 0, this->deltaX_, this->height());
            painter.fillRect(barRect, this->barColorForDelta(dx));

            auto label = this->labelForDelta(dx);
            if (!label.isEmpty())
            {
                // fade + scale the label in over the 10px after the delete
                // threshold
                qreal opacity = std::clamp((dx - DELETE_AT) / 10.0, 0.0, 1.0);
                auto font = this->font();
                font.setBold(true);
                font.setPixelSize(static_cast<int>(12 * scale));
                painter.setFont(font);
                painter.setOpacity(opacity);
                painter.setPen(Qt::white);
                painter.drawText(barRect, Qt::AlignCenter, label);
                painter.setOpacity(1.0);
            }
        }

        this->drawHandle(painter, this->deltaX_);
        return;
    }

    this->drawHandle(painter, 0);
}

}  // namespace chatterino
