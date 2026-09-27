// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PinnedMessageWidget.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageParseArgs.hpp"
#include "messages/Selection.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/IncognitoBrowser.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/TooltipWidget.hpp"

#include <IrcMessage>
#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <optional>

using namespace std::chrono_literals;
using namespace Qt::Literals;

namespace chatterino {

namespace {

const Selection EMPTY_SELECTION;

/// Same cap as the chat: past this, a stack of zero-width emotes is truncated
constexpr size_t TOOLTIP_EMOTE_ENTRIES_LIMIT = 7;

float getTooltipScale(EmoteTooltipScale emoteTooltipScale)
{
    switch (emoteTooltipScale)
    {
        case EmoteTooltipScale::Small:
            return 0.5F;
        case EmoteTooltipScale::Medium:
            return 1.0F;
        case EmoteTooltipScale::Large:
            return 1.5F;
        case EmoteTooltipScale::Huge:
            return 2.0F;

        default:
            return 1.0F;
    }
}

/// How much of the second line a collapsed preview shows, faded out, so a
/// message that continues looks like it does.
constexpr int COLLAPSED_PEEK = 9;

/// The "Pinned by" and sender lines are drawn smaller than chat, text, emotes
/// and badges alike, so they don't compete with the message itself.
constexpr float HEADER_SCALE = 0.8F;

/// The message without the parts that belong to the line around it rather
/// than to what was said: timestamp, badges, name, reply context and so on.
MessageElementFlags bodyFlags(MessageElementFlags flags)
{
    flags.unset(
        MessageElementFlag::Timestamp, MessageElementFlag::HeaderTimestamp,
        MessageElementFlag::Username, MessageElementFlag::KickUsername,
        MessageElementFlag::Badges, MessageElementFlag::ChannelName,
        MessageElementFlag::ModeratorTools, MessageElementFlag::ReplyButton,
        MessageElementFlag::RepliedMessage,
        MessageElementFlag::AnnouncementHeader,
        MessageElementFlag::SubscriptionHeader,
        MessageElementFlag::WatchStreakHeader,
        MessageElementFlag::PlatformBadgeAlways,
        MessageElementFlag::PlatformBadgeIfUnselected);
    return flags;
}

QString escapeTagValue(QString value)
{
    return value.replace(u'\\', u"\\\\"_s)
        .replace(u';', u"\\:"_s)
        .replace(u' ', u"\\s"_s)
        .replace(u'\r', u"\\r"_s)
        .replace(u'\n', u"\\n"_s);
}

/// Builds the pinned message as the chat would have shown it, for pins whose
/// original message isn't in the channel (pinned before we joined, or scrolled
/// out of the buffer). The pin carries no badges or Twitch emote positions, so
/// those are missing, but name color, 7TV/BTTV/FFZ emotes and third party
/// badges still resolve like they do for any message.
MessagePtr buildStandInMessage(TwitchChannel &channel,
                               const HelixPinnedChatMessage &pin)
{
    QString tags = u"display-name=%1;id=%2;user-id=%3;room-id=%4"_s.arg(
        escapeTagValue(pin.sender.displayName), escapeTagValue(pin.messageID),
        escapeTagValue(pin.sender.id), escapeTagValue(channel.roomId()));

    auto color = channel.getUserColor(pin.sender.login);
    if (color.isValid())
    {
        tags += u";color="_s + color.name();
    }

    const auto &login = pin.sender.login;
    auto raw = u"@%1 :%2!%2@%2.tmi.twitch.tv PRIVMSG #%3 :%4"_s.arg(
        tags, login, channel.getName(), pin.messageText);

    std::unique_ptr<Communi::IrcMessage> ircMessage(
        Communi::IrcMessage::fromData(raw.toUtf8(), nullptr));

    MessageParseArgs args;
    args.disablePingSounds = true;
    args.allowIgnore = false;

    auto [message, alert] = MessageBuilder::makeIrcMessage(
        &channel, ircMessage.get(), args, pin.messageText, 0);
    (void)alert;
    return message;
}

}  // namespace

/// Lays out and paints one message like a chat view would, restricted to the
/// given element flags, at a fixed width. Optionally only its first line is
/// shown, for a one-line preview.
class PinnedMessageView : public BaseWidget
{
public:
    explicit PinnedMessageView(QWidget *parent)
        : BaseWidget(parent)
    {
        auto *windows = getApp()->getWindows();
        // emote images finishing their load, or settings that change the
        // layout, relayout the chat views; follow along
        this->signalHolder_.managedConnect(windows->layoutRequested,
                                           [this](Channel * /*channel*/) {
                                               this->relayout();
                                           });
        this->signalHolder_.managedConnect(windows->wordFlagsChanged, [this] {
            this->relayout();
        });
        this->signalHolder_.managedConnect(windows->gifRepaintRequested,
                                           [this] {
                                               if (this->hasAnimatedElements_)
                                               {
                                                   this->update();
                                               }
                                           });
        this->messagePreferences_.connectSettings(getSettings(),
                                                  this->signalHolder_);
        // the pin should read at full strength, never faded as history
        this->messagePreferences_.fadeMessageHistory = false;
        this->themeChangedEvent();

        // hovering emotes, badges and names works like in the chat; the
        // hover card itself is only created once something is hovered
        this->setMouseTracking(true);
    }

    /// Called when a link (a username, a URL) is clicked.
    void setOnLinkClicked(std::function<void(const Link &)> callback)
    {
        this->onLinkClicked_ = std::move(callback);
    }

    void setMessage(const MessagePtr &message, MessageElementFlags flags)
    {
        this->flags_ = flags;
        if (!message)
        {
            this->layout_.reset();
            this->setFixedSize(this->width_, 0);
            return;
        }
        this->layout_ = std::make_unique<MessageLayout>(message);
        // no highlight tint and no "collapse long messages" cut-off: the
        // banner shows the message on its own terms
        this->layout_->flags.set(MessageLayoutFlag::IgnoreHighlights,
                                 MessageLayoutFlag::Expanded);
        this->relayout(true);
    }

    void setWidth(int width)
    {
        if (this->width_ != width)
        {
            this->width_ = width;
            this->relayout();
        }
    }

    /// Shows only the first line, plus a sliver of the second when there is
    /// one (@a peek unscaled pixels), hinting that the message goes on.
    void setFirstLineOnly(bool firstLineOnly, int peek = 0)
    {
        if (this->firstLineOnly_ != firstLineOnly || this->peek_ != peek)
        {
            this->firstLineOnly_ = firstLineOnly;
            this->peek_ = peek;
            this->relayout();
        }
    }

    /// Lays the message out at this fraction of the UI scale, text, emotes
    /// and badges alike, for a smaller line that keeps chat's proportions.
    void setScaleFactor(float factor)
    {
        this->scaleFactor_ = factor;
        this->relayout(true);
    }

    /// Removes (unscaled) pixels of the padding every chat message has around
    /// it, so lines can sit closer together than messages in a chat view.
    void setTrim(int left, int top, int bottom)
    {
        this->trimLeft_ = left;
        this->trimTop_ = top;
        this->trimBottom_ = bottom;
        this->relayout();
    }

    /// Whether some of the message is cut off by #setFirstLineOnly
    bool isClipped() const
    {
        return this->layout_ &&
               this->height() + this->trimTopPx() + this->trimBottomPx() <
                   this->layout_->getHeight();
    }

    /// Called after the view's height changed on its own, e.g. once emote
    /// images load and make the message taller.
    void setOnHeightChanged(std::function<void()> callback)
    {
        this->onHeightChanged_ = std::move(callback);
    }

    void setBackground(QColor background)
    {
        this->messageColors_.regularBg = background;
        this->messageColors_.alternateBg = background;
        if (this->layout_)
        {
            this->layout_->invalidateBuffer();
        }
        this->update();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        if (!this->layout_)
        {
            return;
        }

        QPainter painter(this);
        painter.translate(-this->trimLeftPx(), -this->trimTopPx());
        auto ctx = MessagePaintContext{
            .painter = painter,
            .selection = EMPTY_SELECTION,
            .colorProvider = ColorProvider::instance(),
            .messageColors = this->messageColors_,
            .preferences = this->messagePreferences_,

            .canvasWidth = this->width_ + this->trimLeftPx(),
            .isWindowFocused = this->window() == QApplication::activeWindow(),
            .isMentions = false,

            .y = 0,
            .messageIndex = 0,
            .isLastReadMessage = false,
        };
        this->hasAnimatedElements_ =
            this->layout_->paint(ctx).hasAnimatedElements;
    }

    void themeChangedEvent() override
    {
        auto background = this->messageColors_.regularBg;
        this->messageColors_.applyTheme(getTheme(), false, 255);
        if (background.isValid())
        {
            this->messageColors_.regularBg = background;
            this->messageColors_.alternateBg = background;
        }
        if (this->layout_)
        {
            this->layout_->invalidateBuffer();
        }
    }

    void scaleChangedEvent(float /*newScale*/) override
    {
        this->relayout(true);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const auto *hovered = this->elementAt(event->position());
        if (hovered == nullptr)
        {
            this->setCursor(Qt::ArrowCursor);
            this->hideTooltip();
            return;
        }

        const bool isLink = hovered->getLink().isValid();
        this->setCursor(isLink ? Qt::PointingHandCursor : Qt::ArrowCursor);
        this->showTooltip(hovered->getCreator(), isLink, event);
    }

    void leaveEvent(QEvent * /*event*/) override
    {
        this->hideTooltip();
        this->setCursor(Qt::ArrowCursor);
    }

    void hideEvent(QHideEvent * /*event*/) override
    {
        this->hideTooltip();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // taken here so the split below doesn't open its own menu
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton &&
            event->button() != Qt::MiddleButton)
        {
            return;
        }
        const auto *clicked = this->elementAt(event->position());
        if (clicked && clicked->getLink().isValid() && this->onLinkClicked_)
        {
            this->hideTooltip();
            this->onLinkClicked_(clicked->getLink());
        }
    }

private:
    void hideTooltip()
    {
        if (this->tooltip_)
        {
            this->tooltip_->hide();
        }
    }

    const MessageLayoutElement *elementAt(QPointF position) const
    {
        if (!this->layout_)
        {
            return nullptr;
        }
        // the trimmed padding is painted off-widget, so shift back into it
        return this->layout_->getElementAt(
            position + QPointF(this->trimLeftPx(), this->trimTopPx()));
    }

    /// The chat's hover cards: emotes and badges with their preview image,
    /// stacked zero-width emotes, and plain tooltips for everything else.
    void showTooltip(const MessageElement &element, bool isLink,
                     QMouseEvent *event)
    {
        const auto *emoteElement = dynamic_cast<const EmoteElement *>(&element);
        const auto *layeredEmoteElement =
            dynamic_cast<const LayeredEmoteElement *>(&element);
        const auto *badgeElement = dynamic_cast<const BadgeElement *>(&element);
        const bool isNotEmote =
            emoteElement == nullptr && layeredEmoteElement == nullptr;

        if (element.getTooltip().isEmpty() ||
            (isLink && isNotEmote && !getSettings()->linkInfoTooltip))
        {
            this->hideTooltip();
            return;
        }

        if (!this->tooltip_)
        {
            this->tooltip_ = new TooltipWidget(this);
        }

        const auto previewMode = getSettings()->emotesTooltipPreview.getEnum();
        const bool showThumbnail =
            previewMode == ThumbnailPreviewMode::AlwaysShow ||
            (previewMode == ThumbnailPreviewMode::ShowOnShift &&
             event->modifiers() == Qt::ShiftModifier);
        const float scale =
            getTooltipScale(getSettings()->emoteTooltipScale.getEnum());

        if (emoteElement)
        {
            this->tooltip_->setOne(TooltipEntry::scaled(
                showThumbnail ? emoteElement->getEmote()->images.getImage(3.0)
                              : nullptr,
                element.getTooltip(), scale));
        }
        else if (layeredEmoteElement)
        {
            const auto &layers = layeredEmoteElement->getEmotes();
            if (layers.empty())
            {
                this->hideTooltip();
                return;
            }
            const auto &layerTooltips = layeredEmoteElement->getEmoteTooltips();

            bool truncating = layers.size() > TOOLTIP_EMOTE_ENTRIES_LIMIT;
            size_t shown =
                truncating ? TOOLTIP_EMOTE_ENTRIES_LIMIT - 1 : layers.size();

            std::vector<TooltipEntry> entries;
            entries.reserve(shown + 1);
            for (size_t i = 0; i < shown; ++i)
            {
                const auto &emote = layers[i].ptr;
                // the base emote gets a large image and its full description,
                // the zero-width ones a small image and their name
                entries.push_back(TooltipEntry::scaled(
                    showThumbnail ? emote->images.getImage(i == 0 ? 3.0 : 1.0)
                                  : nullptr,
                    i == 0 ? layerTooltips[i] : emote->name.string, scale));
            }
            if (truncating)
            {
                entries.push_back({nullptr, "..."});
            }
            this->tooltip_->set(entries, layers.size() > 2
                                             ? TooltipStyle::Grid
                                             : TooltipStyle::Vertical);
        }
        else if (badgeElement)
        {
            this->tooltip_->setOne(TooltipEntry::scaled(
                showThumbnail ? badgeElement->getEmote()->images.getImage(3.0)
                              : nullptr,
                element.getTooltip(), scale));
        }
        else
        {
            this->tooltip_->setOne(TooltipEntry{
                .image = nullptr,
                .text = element.getTooltip(),
            });
        }

        this->tooltip_->moveTo(
            event->globalPosition().toPoint() + QPoint(16, 16),
            widgets::BoundsChecking::CursorPosition);
        this->tooltip_->setWordWrap(isLink);
        this->tooltip_->show();
    }

    float layoutScale() const
    {
        return this->scale() * this->scaleFactor_;
    }

    int trimLeftPx() const
    {
        return static_cast<int>(this->trimLeft_ * this->layoutScale());
    }

    int trimTopPx() const
    {
        return static_cast<int>(this->trimTop_ * this->layoutScale());
    }

    int trimBottomPx() const
    {
        return static_cast<int>(this->trimBottom_ * this->layoutScale());
    }

    void relayout(bool invalidate = false)
    {
        if (!this->layout_ || this->width_ <= 0)
        {
            return;
        }

        const float scale = this->layoutScale();
        this->layout_->layout(
            {
                .messageColors = this->messageColors_,
                .flags = this->flags_,
                // the trimmed left padding is painted off-widget
                .width = this->width_ + this->trimLeftPx(),
                .scale = scale,
                .imageScale =
                    scale * static_cast<float>(this->devicePixelRatio()),
                .selectedChannel = nullptr,
                .message = *this->layout_->getMessagePtr(),
            },
            invalidate);

        int height = this->layout_->getHeight();
        if (this->firstLineOnly_)
        {
            // below the first line: the container's bottom margin, or the
            // peek into the next line if that's taller. A single line
            // message is shorter than either and stays whole.
            int belowFirstLine =
                static_cast<int>(std::max(4, this->peek_) * scale);
            height = std::min(
                height, this->layout_->getFirstLineBottom() + belowFirstLine);
        }
        height = std::max(0, height - this->trimTopPx() - this->trimBottomPx());
        const bool heightChanged = height != this->height();
        this->setFixedSize(this->width_, height);
        this->update();
        if (heightChanged && this->onHeightChanged_)
        {
            this->onHeightChanged_();
        }
    }

    std::unique_ptr<MessageLayout> layout_;
    MessageElementFlags flags_;
    MessageColors messageColors_;
    MessagePreferences messagePreferences_;
    pajlada::Signals::SignalHolder signalHolder_;
    int width_ = 0;
    float scaleFactor_ = 1.0F;
    int trimLeft_ = 0;
    int trimTop_ = 0;
    int trimBottom_ = 0;
    bool firstLineOnly_ = false;
    int peek_ = 0;
    bool hasAnimatedElements_ = false;
    std::function<void()> onHeightChanged_;
    std::function<void(const Link &)> onLinkClicked_;
    TooltipWidget *tooltip_ = nullptr;
};

/// Fades the scrolled message into the banner background at the edges where
/// more text continues, so it reads as running on rather than being cut off.
/// Each edge fades in with how much is hidden past it, so the first and last
/// lines are untouched when scrolled all the way to them.
class PinnedMessageFade : public QWidget
{
public:
    explicit PinnedMessageFade(QWidget *parent)
        : QWidget(parent)
    {
        this->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->setAttribute(Qt::WA_NoSystemBackground);
    }

    void setColor(QColor color)
    {
        this->color_ = color;
        this->update();
    }

    /// @param hiddenAbove  pixels of message scrolled out at the top
    /// @param hiddenBelow  pixels of message still out of view at the bottom
    /// @param fadeHeight   how tall each fade is at full strength
    void setHidden(int hiddenAbove, int hiddenBelow, int fadeHeight,
                   qreal maxFraction)
    {
        this->hiddenAbove_ = hiddenAbove;
        this->hiddenBelow_ = hiddenBelow;
        this->fadeHeight_ = fadeHeight;
        this->maxFraction_ = maxFraction;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        if (this->fadeHeight_ <= 0 || !this->color_.isValid())
        {
            return;
        }

        QPainter painter(this);
        // capped to a share of the view, so short views still show their
        // content clearly
        const int height =
            std::min(this->fadeHeight_,
                     static_cast<int>(this->height() * this->maxFraction_));

        auto strength = [&](int hidden) {
            return std::clamp(qreal(hidden) / qreal(this->fadeHeight_), 0.0,
                              1.0);
        };
        auto paintFade = [&](qreal amount, int y, bool towardsTop) {
            if (amount <= 0 || height <= 0)
            {
                return;
            }
            QColor solid = this->color_;
            solid.setAlphaF(amount);
            QColor clear = this->color_;
            clear.setAlphaF(0);

            QLinearGradient gradient(0, y, 0, y + height);
            gradient.setColorAt(0, towardsTop ? solid : clear);
            gradient.setColorAt(1, towardsTop ? clear : solid);
            painter.fillRect(0, y, this->width(), height, gradient);
        };

        paintFade(strength(this->hiddenAbove_), 0, true);
        paintFade(strength(this->hiddenBelow_), this->height() - height, false);
    }

private:
    QColor color_;
    int hiddenAbove_ = 0;
    int hiddenBelow_ = 0;
    int fadeHeight_ = 0;
    qreal maxFraction_ = 1.0 / 3.0;
};

PinnedMessageWidget::PinnedMessageWidget(QWidget *parent)
    : BaseWidget(parent)
    , pinIcon_(new SvgButton(
          {
              .dark = u":/buttons/pushPin.svg"_s,
              .light = u":/buttons/pushPin.svg"_s,
          },
          this, {0, 0}))
    , headerView_(new PinnedMessageView(this))
    , countdownLabel_(new QLabel(this))
    , menuButton_(new DrawnButton(DrawnButton::Symbol::Kebab, {}, this))
    , expandButton_(new DrawnButton(DrawnButton::Symbol::ChevronDown, {}, this))
    , collapseButton_(new DrawnButton(DrawnButton::Symbol::ChevronUp, {}, this))
    , bodyViewport_(new QWidget(this))
    , bodyView_(new PinnedMessageView(this->bodyViewport_))
    , bodyScrollbar_(new Scrollbar(0, this))
    , bodyFade_(new PinnedMessageFade(this->bodyViewport_))
    , senderView_(new PinnedMessageView(this))
    , progressTimer_(new QTimer(this))
    , autoHideTimer_(new QTimer(this))
{
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto *outerBox = new QVBoxLayout(this);
    // 1px top and bottom for the borders painted in paintEvent
    outerBox->setContentsMargins(0, 1, 0, 1);
    outerBox->setSpacing(0);

    // Header: [pin] Pinned by [badge] name ........ [0:42] [⋮] [v]
    this->headerRow_ = new QHBoxLayout();
    auto *headerRow = this->headerRow_;
    headerRow->setContentsMargins(8, 2, 4, 0);

    this->pinIcon_->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerRow->addWidget(this->pinIcon_, 0, Qt::AlignVCenter);
    // a smaller line than chat, badge and text shrunk together, sitting right
    // after the icon and right above the message
    this->headerView_->setScaleFactor(HEADER_SCALE);
    this->headerView_->setTrim(8, 0, 4);
    headerRow->addWidget(this->headerView_, 0, Qt::AlignVCenter);
    headerRow->addStretch(1);
    headerRow->addWidget(this->countdownLabel_, 0, Qt::AlignVCenter);
    this->countdownLabel_->hide();

    this->menuButton_->setToolTip(u"Pin options"_s);
    this->menuButton_->hide();
    headerRow->addWidget(this->menuButton_, 0, Qt::AlignVCenter);

    this->expandButton_->setToolTip(u"Expand pinned message"_s);
    QObject::connect(this->expandButton_, &Button::leftClicked, this, [this] {
        this->setExpanded(true);
    });
    headerRow->addWidget(this->expandButton_, 0, Qt::AlignVCenter);

    this->collapseButton_->setToolTip(u"Collapse pinned message"_s);
    QObject::connect(this->collapseButton_, &Button::leftClicked, this, [this] {
        this->setExpanded(false);
    });
    this->collapseButton_->hide();
    headerRow->addWidget(this->collapseButton_, 0, Qt::AlignVCenter);

    outerBox->addLayout(headerRow);

    // Body: the message itself, one line collapsed, wrapped in full expanded.
    // The view is clipped by the viewport and moved by the scrollbar.
    this->bodyView_->setTrim(0, 4, 0);
    // The viewport's height is set by hand, so it has to follow the body when
    // the body relayouts by itself. Coalesced, and deferred so it never runs
    // inside updateMessageLayout's own resizing.
    this->bodyView_->setOnHeightChanged([this] {
        if (this->bodyLayoutQueued_)
        {
            return;
        }
        this->bodyLayoutQueued_ = true;
        QTimer::singleShot(0, this, [this] {
            this->bodyLayoutQueued_ = false;
            this->updateMessageLayout();
        });
    });
    this->bodyViewport_->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Fixed);
    this->bodyScrollbar_->setParent(this->bodyViewport_);
    this->bodyScrollbar_->setHideHighlights(true);
    this->bodyScrollbar_->hide();
    this->signalHolder_.managedConnect(
        this->bodyScrollbar_->getCurrentValueChanged(), [this] {
            this->applyBodyScroll();
        });
    outerBox->addWidget(this->bodyViewport_);

    // Sender (expanded only): [badges] name sent at 18:09
    // Same smaller scale as the "Pinned by" line. The scaled line has a
    // narrower built-in left margin, so the row makes up the difference to
    // start where the message text does.
    this->senderView_->setScaleFactor(HEADER_SCALE);
    this->senderView_->setTrim(0, 4, 0);
    this->senderView_->hide();
    this->senderRow_ = new QHBoxLayout();
    this->senderRow_->setSpacing(0);
    this->senderRow_->addWidget(this->senderView_);
    this->senderRow_->addStretch(1);
    outerBox->addLayout(this->senderRow_);

    // Countdown timer (fires every second)
    this->progressTimer_->setInterval(1s);
    QObject::connect(this->progressTimer_, &QTimer::timeout, this, [this] {
        this->tickProgress();
    });

    // auto-hide timer
    this->autoHideTimer_->setSingleShot(true);
    QObject::connect(this->autoHideTimer_, &QTimer::timeout, this, [this] {
        if (!this->userToggled_.value_or(false))
        {
            this->hide();
        }
    });

    for (auto *view : {this->headerView_, this->bodyView_, this->senderView_})
    {
        view->setOnLinkClicked([this](const Link &link) {
            this->openLink(link);
        });
    }

    this->scaleChangedEvent(this->scale());
    this->themeChangedEvent();
    this->hide();
}

void PinnedMessageWidget::openLink(const Link &link)
{
    switch (link.type)
    {
        case Link::UserWhisper:
        case Link::UserInfo: {
            Split *split = nullptr;
            for (auto *widget = this->parentWidget(); widget && !split;
                 widget = widget->parentWidget())
            {
                split = dynamic_cast<Split *>(widget);
            }
            if (!split || !this->channel_)
            {
                return;
            }

            auto *userPopup =
                new UserInfoPopup(getSettings()->autoCloseUserPopup, split);
            auto channel = this->channel_->sharedFromThis();
            userPopup->setData(link.value, channel, channel);

            QPoint offset(userPopup->width() / 3, userPopup->height() / 5);
            userPopup->moveTo(QCursor::pos() - offset,
                              widgets::BoundsChecking::CursorPosition);
            userPopup->show();
        }
        break;

        case Link::Url: {
            if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
            {
                openLinkIncognito(link.value);
            }
            else
            {
                QDesktopServices::openUrl(QUrl(link.value));
            }
        }
        break;

        default:
            break;
    }
}

void PinnedMessageWidget::tickProgress()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 endsMs = this->pinEndsAt_.toMSecsSinceEpoch();

    if (nowMs >= endsMs)
    {
        this->progressTimer_->stop();
        this->countdownLabel_->hide();
        if (this->channel_)
        {
            this->channel_->clearPinnedMessage();
        }
        return;
    }

    const qint64 remainingMs = endsMs - nowMs;
    const qint64 totalSecs = (remainingMs + 999) / 1000;  // round up
    const qint64 hours = totalSecs / 3600;
    const qint64 mins = (totalSecs % 3600) / 60;
    const qint64 secs = totalSecs % 60;

    QString timeStr;
    if (hours > 0)
    {
        timeStr = u"%1:%2:%3"_s.arg(hours)
                      .arg(mins, 2, 10, QChar(u'0'))
                      .arg(secs, 2, 10, QChar(u'0'));
    }
    else
    {
        timeStr = u"%1:%2"_s.arg(mins).arg(secs, 2, 10, QChar(u'0'));
    }

    this->countdownLabel_->setText(timeStr);
    this->countdownLabel_->setToolTip(u"Unpins automatically"_s);
    this->countdownLabel_->show();
}

void PinnedMessageWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    auto *theme = getTheme();

    // Fill background (same color as the split header above)
    painter.fillRect(event->rect(), theme->splits.header.background);

    // 1px borders: against the split header above and the chat view below
    painter.setPen(theme->splits.header.border);
    painter.drawLine(0, 0, this->width() - 1, 0);
    painter.drawLine(0, this->height() - 1, this->width() - 1,
                     this->height() - 1);
}

void PinnedMessageWidget::setChannel(TwitchChannel *channel)
{
    this->signalHolder_.clear();
    this->channel_ = channel;
    this->autoHideTimer_->stop();
    this->builtForMessageID_.clear();
    this->lastPinKey_.clear();
    this->setUnseenUpdate(false);

    if (channel)
    {
        this->signalHolder_.managedConnect(channel->pinnedMessageChanged,
                                           [this] {
                                               this->refresh();
                                           });
        this->signalHolder_.managedConnect(channel->userStateChanged, [this] {
            this->refresh();
        });
        // the original message can show up after the pin (history loading),
        // which lets the stand-in be replaced by the real thing
        this->signalHolder_.managedConnect(
            channel->messagesAddedAtStart,
            [this](std::vector<MessagePtr> & /*messages*/) {
                if (!this->builtFromOriginal_)
                {
                    this->refresh();
                }
            });
    }

    this->refresh();
}

std::unique_ptr<QMenu> PinnedMessageWidget::buildMenu()
{
    auto menu = std::make_unique<QMenu>(this);

    menu->addAction(u"Copy Message"_s, this, [this] {
        if (!this->channel_)
        {
            return;
        }
        if (const auto *pin = this->channel_->getPinnedMessage())
        {
            crossPlatformCopy(pin->messageText);
        }
    });

    menu->addAction(u"Hide for Yourself"_s, this, [this] {
        this->userToggled_ = false;
        this->hide();
    });

    if (!this->channel_ || !this->channel_->hasModRights())
    {
        return menu;
    }

    menu->addSeparator();

    menu->addAction(u"Unpin this Message"_s, this, [this] {
        if (this->channel_)
        {
            this->channel_->unpinCurrentMessage();
        }
    });

    auto *unpinAfterMenu = menu->addMenu(u"Unpin After"_s);

    const auto addDuration = [&](const QString &label,
                                 std::optional<std::chrono::seconds> duration) {
        unpinAfterMenu->addAction(label, this, [this, duration] {
            if (!this->channel_)
            {
                return;
            }
            const auto *pin = this->channel_->getPinnedMessage();
            if (!pin)
            {
                return;
            }
            auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();
            if (!currentAccount || currentAccount->isAnon())
            {
                return;
            }
            this->channel_->updatePinnedMessageAs(
                pin->messageID, duration, *currentAccount, pin->messageText);
        });
    };

    addDuration(u"1 minute"_s, 1min);
    addDuration(u"5 minutes"_s, 5min);
    addDuration(u"10 minutes"_s, 10min);
    addDuration(u"20 minutes"_s, 20min);
    addDuration(u"30 minutes"_s, 30min);
    unpinAfterMenu->addSeparator();
    addDuration(u"End of stream"_s, std::nullopt);

    return menu;
}

void PinnedMessageWidget::rebuildMessages(const HelixPinnedChatMessage &pin)
{
    auto &channel = *this->channel_;
    const auto mode = static_cast<UsernameDisplayMode>(
        getSettings()->usernameDisplayMode.getValue());

    // Header: "Pinned by" [role badge] name - all muted, like system text,
    // so the pinner doesn't compete with the message. Only moderators and the
    // broadcaster can pin, so their role badge is the one that matters.
    {
        auto header = std::make_shared<Message>();
        header->elements.push_back(std::make_unique<TextElement>(
            u"Pinned by"_s, MessageElementFlag::Text, MessageColor::System,
            FontStyle::ChatMedium));

        const bool isBroadcaster = pin.pinnedBy.id == channel.roomId();
        auto customModBadge = channel.ffzCustomModBadge();
        if (!isBroadcaster && customModBadge)
        {
            // the channel's FFZ mod badge, drawn over its green like in chat
            header->elements.push_back(std::make_unique<ModBadgeElement>(
                *customModBadge, MessageElementFlag::BadgeChannelAuthority));
        }
        else
        {
            const auto roleSet =
                isBroadcaster ? u"broadcaster"_s : u"moderator"_s;
            auto roleBadge = channel.twitchBadge(roleSet, u"1"_s);
            if (!roleBadge)
            {
                roleBadge = getApp()->getTwitchBadges()->badge(roleSet, u"1"_s);
            }
            if (roleBadge)
            {
                header->elements.push_back(std::make_unique<BadgeElement>(
                    *roleBadge, MessageElementFlag::BadgeChannelAuthority));
            }
        }

        auto pinner = std::make_unique<TextElement>(
            pin.pinnedBy.formatted(mode), MessageElementFlag::Text,
            MessageColor::System, FontStyle::ChatMedium);
        // clickable for their user card; stays grey, like system text
        pinner->setLink({Link::UserInfo, pin.pinnedBy.login});
        header->elements.push_back(std::move(pinner));

        this->headerView_->setMessage(
            header, {MessageElementFlag::Text,
                     MessageElementFlag::BadgeChannelAuthority});
    }

    // Body: the original chat message when we still have it (real badges,
    // Twitch emotes, reply context), otherwise a stand-in built from the pin
    MessagePtr body = channel.findMessageByID(pin.messageID);
    this->builtFromOriginal_ = body != nullptr;
    if (!body)
    {
        body = buildStandInMessage(channel, pin);
    }
    auto wordFlags = getApp()->getWindows()->getWordFlags();
    this->bodyView_->setMessage(body, bodyFlags(wordFlags));

    // Sender: [badges] name sent at <time>
    {
        auto sender = std::make_shared<Message>();
        sender->loginName = pin.sender.login;
        sender->displayName = pin.sender.displayName;
        sender->userID = pin.sender.id;

        if (body)
        {
            for (const auto &element : body->elements)
            {
                // clone keeps the badge's own kind: FFZ badges and custom
                // mod/VIP badges paint a background color behind the image
                if (dynamic_cast<const BadgeElement *>(element.get()))
                {
                    sender->elements.push_back(element->clone());
                }
            }
        }

        QColor nameColor = body && body->usernameColor.isValid()
                               ? body->usernameColor
                               : channel.getUserColor(pin.sender.login);
        auto name = std::make_unique<TextElement>(
            pin.sender.formatted(mode), MessageElementFlag::Username,
            nameColor.isValid() ? MessageColor(nameColor) : MessageColor::Text,
            FontStyle::ChatMediumBold);
        // a user link is what makes the layout apply their 7TV paint
        name->setLink({Link::UserInfo, pin.sender.login});
        sender->elements.push_back(std::move(name));

        // only the original knows when it was sent; a stand-in doesn't
        if (this->builtFromOriginal_ && body->serverReceivedTime.isValid())
        {
            sender->elements.push_back(std::make_unique<TextElement>(
                u"sent at "_s + body->serverReceivedTime.toLocalTime().toString(
                                    getSettings()->timestampFormat),
                MessageElementFlag::Text, MessageColor::System,
                FontStyle::ChatMedium));
        }

        this->senderView_->setMessage(sender, wordFlags |
                                                  MessageElementFlag::Username |
                                                  MessageElementFlag::Text);
    }

    this->builtForMessageID_ = pin.messageID;
}

void PinnedMessageWidget::refresh()
{
    if (!this->channel_)
    {
        this->progressTimer_->stop();
        this->autoHideTimer_->stop();
        this->hide();
        return;
    }

    const auto *pin = this->channel_->getPinnedMessage();
    if (!pin)
    {
        this->progressTimer_->stop();
        this->autoHideTimer_->stop();
        this->userToggled_ = std::nullopt;
        this->builtForMessageID_.clear();
        this->lastPinKey_.clear();
        this->setUnseenUpdate(false);
        this->hide();
        return;
    }

    const QString pinKey =
        pin->messageID + u'|' +
        (pin->endsAt ? pin->endsAt->toString(Qt::ISODate) : QString());
    const bool pinChanged = pinKey != this->lastPinKey_;
    this->lastPinKey_ = pinKey;

    const bool isNewPin = pin->messageID != this->builtForMessageID_;
    if (isNewPin || !this->builtFromOriginal_)
    {
        this->rebuildMessages(*pin);
    }
    if (isNewPin)
    {
        // every new pin starts as a one-line preview
        this->setExpanded(false);
    }

    this->progressTimer_->stop();
    this->countdownLabel_->hide();
    if (pin->endsAt.has_value() && pin->endsAt->isValid())
    {
        this->pinEndsAt_ = *pin->endsAt;
        this->tickProgress();  // set initial text immediately
        this->progressTimer_->start();
    }

    // mod rights can change (userStateChanged), so the menu is rebuilt
    this->menuButton_->setMenu(this->buildMenu());

    if (this->userToggled_.value_or(true))
    {
        this->show();
    }
    this->updateMessageLayout();

    this->autoHideTimer_->stop();
    if (!getSettings()->alwaysShowPinnedMessage &&
        !this->userToggled_.value_or(false))
    {
        this->autoHideTimer_->start(30s);
    }

    // A pin that changed while the banner is hidden (by the user, by the
    // auto-hide, or with its split in a background tab) goes unseen until the
    // banner is next shown
    if (pinChanged && !this->isVisible())
    {
        this->setUnseenUpdate(true);
    }
}

void PinnedMessageWidget::setExpanded(bool expanded)
{
    this->expanded_ = expanded;
    this->expandButton_->setVisible(!expanded);
    this->collapseButton_->setVisible(expanded);
    this->menuButton_->setVisible(expanded);
    this->senderView_->setVisible(expanded);
    this->bodyView_->setFirstLineOnly(!expanded, COLLAPSED_PEEK);
    // looking at the pin in detail keeps it around
    if (expanded)
    {
        this->autoHideTimer_->stop();
    }
    this->updateMessageLayout();
}

void PinnedMessageWidget::toggleUserPinned()
{
    if (this->isVisible())
    {
        this->userToggled_ = false;
        this->autoHideTimer_->stop();
        this->hide();
    }
    else
    {
        this->userToggled_ = true;
        this->autoHideTimer_->stop();
        this->show();
        this->updateMessageLayout();
    }
}

void PinnedMessageWidget::updateMessageLayout()
{
    const int fullWidth = this->width();
    if (fullWidth <= 0)
    {
        return;
    }

    // the header message gets whatever the icon and buttons leave over
    int buttonsWidth = this->pinIcon_->width() + 12;
    for (QWidget *widget : {static_cast<QWidget *>(this->countdownLabel_),
                            static_cast<QWidget *>(this->menuButton_),
                            static_cast<QWidget *>(this->expandButton_),
                            static_cast<QWidget *>(this->collapseButton_)})
    {
        if (!widget->isHidden())
        {
            buttonsWidth += widget->sizeHint().width();
        }
    }
    this->headerView_->setWidth(std::max(1, fullWidth - buttonsWidth));

    const int senderInset = this->senderRow_->contentsMargins().left();
    this->senderView_->setWidth(std::max(1, fullWidth - senderInset));

    // Like the chat, the scrollbar overlays the right edge (thin until
    // hovered) and the content gives up a little width to it.
    const int scrollbarWidth = static_cast<int>(16 * this->scale());
    const int scrollbarPadding = static_cast<int>(8 * this->scale());

    this->bodyView_->setWidth(fullWidth);
    int contentHeight = this->bodyView_->height();
    int shownHeight = this->expanded_
                          ? std::min(contentHeight, this->messageMaxHeight_)
                          : contentHeight;
    this->bodyScrollable_ = contentHeight > shownHeight;
    if (this->bodyScrollable_)
    {
        this->bodyView_->setWidth(fullWidth - scrollbarPadding);
        contentHeight = this->bodyView_->height();
        shownHeight = std::min(contentHeight, this->messageMaxHeight_);
        this->bodyScrollable_ = contentHeight > shownHeight;
    }
    this->bodyViewport_->setFixedHeight(std::max(1, shownHeight));

    if (this->bodyScrollable_)
    {
        this->bodyScrollbar_->setGeometry(fullWidth - scrollbarWidth, 0,
                                          scrollbarWidth, shownHeight);
        this->bodyScrollbar_->setMinimum(0);
        this->bodyScrollbar_->setMaximum(contentHeight);
        this->bodyScrollbar_->setPageSize(shownHeight);
        this->bodyScrollbar_->raise();
        this->bodyScrollbar_->show();
    }
    else
    {
        this->bodyScrollbar_->hide();
        this->bodyScrollbar_->setDesiredValue(0);
    }
    this->applyBodyScroll();

    // a one-line preview of a longer message can be expanded; one that fits
    // in a line has nothing more to show, except the sender line
    this->expandButton_->setToolTip(this->bodyView_->isClipped()
                                        ? u"Show full message"_s
                                        : u"Show sender"_s);
}

void PinnedMessageWidget::applyBodyScroll()
{
    const int offset =
        this->bodyScrollable_
            ? static_cast<int>(this->bodyScrollbar_->getCurrentValue())
            : 0;
    this->bodyView_->move(0, -offset);

    // Fades only where there's message past the edge, over the text but
    // under the scrollbar. Collapsed, the peek of the next line fades out.
    const int viewportHeight = this->bodyViewport_->height();
    int hiddenAbove = offset;
    int hiddenBelow = 0;
    int fadeHeight = static_cast<int>(18 * this->scale());
    qreal maxFraction = 1.0 / 3.0;
    if (this->bodyScrollable_)
    {
        hiddenBelow = this->bodyView_->height() - viewportHeight - offset;
    }
    else if (!this->expanded_ && this->bodyView_->isClipped())
    {
        hiddenBelow = std::numeric_limits<int>::max();
        fadeHeight = static_cast<int>((COLLAPSED_PEEK + 5) * this->scale());
        maxFraction = 0.5;
    }
    this->bodyFade_->setGeometry(0, 0, this->bodyViewport_->width(),
                                 viewportHeight);
    this->bodyFade_->setHidden(hiddenAbove, hiddenBelow, fadeHeight,
                               maxFraction);
    this->bodyFade_->raise();
    this->bodyScrollbar_->raise();
}

void PinnedMessageWidget::wheelEvent(QWheelEvent *event)
{
    if (!this->bodyScrollable_ || event->angleDelta().y() == 0 ||
        event->modifiers().testFlag(Qt::ControlModifier))
    {
        // ctrl+scroll zooms; let the parent have it
        event->ignore();
        return;
    }

    // same speed as scrolling the chat
    const qreal delta = event->angleDelta().y() * qreal(1.5) *
                        getSettings()->mouseScrollMultiplier;
    this->bodyScrollbar_->offset(-delta);
    event->accept();
}

void PinnedMessageWidget::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    this->updateMessageLayout();
}

bool PinnedMessageWidget::hasUnseenUpdate() const
{
    return this->unseenUpdate_;
}

void PinnedMessageWidget::setUnseenUpdate(bool unseen)
{
    if (this->unseenUpdate_ != unseen)
    {
        this->unseenUpdate_ = unseen;
        this->unseenUpdateChanged.invoke();
    }
}

void PinnedMessageWidget::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    this->setUnseenUpdate(false);
    this->updateMessageLayout();
    this->visibilityChanged.invoke();
}

void PinnedMessageWidget::hideEvent(QHideEvent *event)
{
    BaseWidget::hideEvent(event);
    this->visibilityChanged.invoke();
}

void PinnedMessageWidget::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();

    auto background = this->theme->splits.header.background;
    this->bodyFade_->setColor(background);
    this->headerView_->setBackground(background);
    this->bodyView_->setBackground(background);
    this->senderView_->setBackground(background);

    // muted, like the "Pinned by" text next to it
    this->pinIcon_->setColor(this->theme->messages.textColors.system);
    QPalette palette = this->countdownLabel_->palette();
    palette.setColor(QPalette::WindowText,
                     this->theme->messages.textColors.system);
    this->countdownLabel_->setPalette(palette);

    this->update();
}

void PinnedMessageWidget::scaleChangedEvent(float newScale)
{
    auto *fonts = getApp()->getFonts();
    this->countdownLabel_->setFont(
        fonts->getFont(FontStyle::ChatMediumSmall, newScale));

    const int iconSize = static_cast<int>(14 * newScale);
    this->pinIcon_->setFixedSize(iconSize, iconSize);
    for (auto *button :
         {this->menuButton_, this->expandButton_, this->collapseButton_})
    {
        button->setScaleIndependentSize(22, 22);
    }

    // the gap between the pin icon and "Pinned by"
    this->headerRow_->setSpacing(static_cast<int>(4 * newScale));

    // message text starts at an 8px margin, the scaled sender line at a
    // smaller one; line them up
    const int bodyMargin = static_cast<int>(8 * newScale);
    const int senderMargin = static_cast<int>(8 * newScale * HEADER_SCALE);
    this->senderRow_->setContentsMargins(bodyMargin - senderMargin, 0, 0, 0);

    this->messageMaxHeight_ = static_cast<int>(110 * newScale);
    this->updateMessageLayout();
}

void PinnedMessageWidget::mousePressEvent(QMouseEvent * /*event*/)
{
    // ignore to disable the parent's right click menu
}

}  // namespace chatterino
