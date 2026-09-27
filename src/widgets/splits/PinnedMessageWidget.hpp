// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QTimer>

#include <memory>
#include <optional>

class QHBoxLayout;
class QLabel;
class QMenu;

namespace chatterino {

class TwitchChannel;
class DrawnButton;
class Scrollbar;
class SvgButton;
struct Link;
class PinnedMessageFade;
class PinnedMessageView;
struct HelixPinnedChatMessage;

/**
 * Banner shown between the split header and the chat view that
 * displays the channel's currently pinned message.
 *
 * Collapsed, it shows who pinned it and a one-line preview of the message.
 * Expanded, the message wraps in full and the sender's badges, name and send
 * time appear below it. The message is laid out like a chat message, so
 * emotes, badges, name colors and 7TV paints follow the theme and settings.
 */
class PinnedMessageWidget final : public BaseWidget
{
    Q_OBJECT

public:
    explicit PinnedMessageWidget(QWidget *parent = nullptr);

    // Pass nullptr to detach from any channel.
    void setChannel(TwitchChannel *channel);

    // Called by the header pin button to toggle manual visibility.
    void toggleUserPinned();

    /// Emitted whenever this widget becomes shown or hidden.
    pajlada::Signals::NoArgSignal visibilityChanged;

    /// Whether the pin changed while this banner wasn't on screen, and it
    /// hasn't been shown since. Cleared as soon as it's shown.
    bool hasUnseenUpdate() const;

    /// Emitted when #hasUnseenUpdate() changes.
    pajlada::Signals::NoArgSignal unseenUpdateChanged;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scaleChangedEvent(float newScale) override;
    void themeChangedEvent() override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void paintEvent(QPaintEvent *event) override;
    void refresh();
    /// Rebuilds the header, body and sender lines from the current pin.
    void rebuildMessages(const HelixPinnedChatMessage &pin);
    /// Builds the menu behind the kebab button. Moderator actions are only
    /// added when the current user can unpin.
    std::unique_ptr<QMenu> buildMenu();
    void setExpanded(bool expanded);
    void tickProgress();
    /// Sizes the message views to the current width and the body to either
    /// its first line (collapsed) or its full height up to a cap (expanded).
    void updateMessageLayout();
    /// Moves the body to the scrollbar's current position.
    void applyBodyScroll();
    /// Opens what a clicked name or link points to, like the chat does: a
    /// user card for names, the browser for URLs.
    void openLink(const Link &link);

    TwitchChannel *channel_ = nullptr;
    pajlada::Signals::SignalHolder signalHolder_;

    // Header row
    QHBoxLayout *headerRow_ = nullptr;
    QHBoxLayout *senderRow_ = nullptr;
    SvgButton *pinIcon_ = nullptr;
    PinnedMessageView *headerView_ = nullptr;
    QLabel *countdownLabel_ = nullptr;
    DrawnButton *menuButton_ = nullptr;
    DrawnButton *expandButton_ = nullptr;
    DrawnButton *collapseButton_ = nullptr;

    // Body: the message view inside a clipping viewport, scrolled by the same
    // scrollbar the chat uses once expanded past the height cap
    QWidget *bodyViewport_ = nullptr;
    PinnedMessageView *bodyView_ = nullptr;
    Scrollbar *bodyScrollbar_ = nullptr;
    PinnedMessageFade *bodyFade_ = nullptr;
    bool bodyScrollable_ = false;
    bool bodyLayoutQueued_ = false;
    PinnedMessageView *senderView_ = nullptr;

    QTimer *progressTimer_ = nullptr;
    QTimer *autoHideTimer_ = nullptr;
    /// Scaled cap for the expanded message body.
    int messageMaxHeight_ = 110;
    bool expanded_ = false;
    /// - `std::nullopt`: The user didn't toggle the widget yet.
    /// - `false`: The user hid the popup.
    /// - `true`: The user manually opened the popup.
    std::optional<bool> userToggled_ = std::nullopt;
    /// Invalid when no end time.
    QDateTime pinEndsAt_;

    /// The pin the views were last built for, and whether the original chat
    /// message was found. A stand-in built from the pin's text is swapped for
    /// the original once it shows up in the channel (e.g. history loading).
    QString builtForMessageID_;
    bool builtFromOriginal_ = false;

    void setUnseenUpdate(bool unseen);
    /// What identifies the pin as last refreshed: its message and when it
    /// unpins. A new pin or a re-timed one differs from it.
    QString lastPinKey_;
    bool unseenUpdate_ = false;
};

}  // namespace chatterino
