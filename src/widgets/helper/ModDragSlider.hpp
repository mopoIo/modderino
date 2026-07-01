// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "messages/Message.hpp"
#include "widgets/BaseWidget.hpp"

#include <QTimer>

namespace chatterino {

/// @brief Drag-to-moderate handle shown on hovered messages.
///
/// When the current user has mod rights in the hovered message's channel, a
/// small grip appears on the left edge of the message row. Dragging it to the
/// right grows a colored bar through escalating action zones:
///
///   0-39px: nothing | 40-79px: delete | 80-319px: timeout (distance maps to
///   1s..14d) | >=320px: permanent ban
///
/// The action is committed on release; pressing Escape during the drag
/// cancels it. After committing, a short textual "flash" confirmation is
/// shown on the right side of the row.
class ModDragSlider : public BaseWidget
{
    Q_OBJECT

public:
    ModDragSlider(BaseWidget *parent);

    /// Shows the grip for a message occupying @a rowRect (parent coordinates).
    void showFor(const ChannelPtr &channel, const MessagePtr &message,
                 const QRect &rowRect);

    /// Hides the grip unless a drag or a flash message is in progress.
    void hideIfIdle();

    bool isDragging() const;

    /// The id of the message currently being dragged; empty if none
    QString draggedMessageId() const;

    /// The current horizontal drag offset in pixels (0 when not dragging)
    qreal dragOffset() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class Zone : std::uint8_t { None, Delete, Timeout, Ban };

    /// Distance thresholds, in unscaled logical pixels
    static constexpr qreal DELETE_AT = 40;
    static constexpr qreal TIMEOUT_AT = 80;
    static constexpr qreal BAN_AT = 320;

    Zone zoneForDelta(qreal dx) const;
    int timeoutSecondsForDelta(qreal dx) const;
    QString labelForDelta(qreal dx) const;
    QColor barColorForDelta(qreal dx) const;

    void applyIdleGeometry();
    void cancelDrag();
    void commit();
    void startFlash(const QString &text);

    void drawHandle(QPainter &painter, qreal x);

    ChannelPtr channel_;
    QString messageId_;
    QString login_;
    QString displayName_;
    /// Kick channels can only delete messages; the timeout/ban zones are
    /// capped to delete there.
    bool canTimeout_ = false;

    QRect rowRect_;
    bool dragging_ = false;
    QPointF dragStartGlobal_;
    qreal deltaX_ = 0;

    QString flashText_;
    QTimer flashTimer_;
};

}  // namespace chatterino
