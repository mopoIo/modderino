// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"

#include <QApplication>
#include <QDebug>
#include <QPainter>
#include <QtGlobal>
#include <QThread>

#include <optional>

namespace chatterino {

namespace {

QColor blendColors(const QColor &base, const QColor &apply)
{
    const qreal &alpha = apply.alphaF();
    QColor result;
    result.setRgbF(base.redF() * (1 - alpha) + apply.redF() * alpha,
                   base.greenF() * (1 - alpha) + apply.greenF() * alpha,
                   base.blueF() * (1 - alpha) + apply.blueF() * alpha);
    return result;
}

QString currentUserLogin()
{
    return getApp()->getAccounts()->twitch.getCurrent()->getUserName();
}

}  // namespace

MessageLayout::MessageLayout(MessagePtr message)
    : message_(std::move(message))
{
    DebugCount::increase(DebugObject::MessageLayout);
}

MessageLayout::~MessageLayout()
{
    DebugCount::decrease(DebugObject::MessageLayout);
}

const Message *MessageLayout::getMessage()
{
    return this->message_.get();
}

const MessagePtr &MessageLayout::getMessagePtr() const
{
    return this->message_;
}

// Height
int MessageLayout::getHeight() const
{
    return static_cast<int>(this->container_.getHeight());
}

int MessageLayout::getWidth() const
{
    return static_cast<int>(this->container_.getWidth());
}

// Layout
// return true if redraw is required
bool MessageLayout::layout(const MessageLayoutContext &ctx,
                           bool shouldInvalidateBuffer)
{
    //    BenchmarkGuard benchmark("MessageLayout::layout()");

    bool layoutRequired = false;

    // check if width changed
    bool widthChanged = ctx.width != this->currentLayoutWidth_;
    layoutRequired |= widthChanged;
    this->currentLayoutWidth_ = ctx.width;

    // check if layout state changed
    const auto layoutGeneration = getApp()->getWindows()->getGeneration();
    if (this->layoutState_ != layoutGeneration)
    {
        layoutRequired = true;
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        this->layoutState_ = layoutGeneration;
    }

    // check if work mask changed
    layoutRequired |= this->currentWordFlags_ != ctx.flags;
    this->currentWordFlags_ = ctx.flags;  // getSettings()->getWordTypeMask();

    // check if layout was requested manually
    layoutRequired |= this->flags.has(MessageLayoutFlag::RequiresLayout);
    this->flags.unset(MessageLayoutFlag::RequiresLayout);

    // check if dpi changed
    layoutRequired |= this->scale_ != ctx.scale;
    this->scale_ = ctx.scale;
    layoutRequired |= this->imageScale_ != ctx.imageScale;
    this->imageScale_ = ctx.imageScale;

    if (!layoutRequired)
    {
        if (shouldInvalidateBuffer)
        {
            this->invalidateBuffer();
            return true;
        }
        return false;
    }

    qreal oldHeight = this->container_.getHeight();
    this->actuallyLayout(ctx);
    if (widthChanged || this->container_.getHeight() != oldHeight)
    {
        this->deleteBuffer();
    }
    this->invalidateBuffer();

    return true;
}

void MessageLayout::actuallyLayout(const MessageLayoutContext &ctx)
{
#ifdef FOURTF
    this->layoutCount_++;
#endif

    auto messageFlags = this->message_->flags;

    if (this->flags.has(MessageLayoutFlag::Expanded) ||
        (ctx.flags.has(MessageElementFlag::ModeratorTools) &&
         !this->message_->flags.has(MessageFlag::Disabled)))
    {
        messageFlags.unset(MessageFlag::Collapsed);
    }

    bool hideModerated = getSettings()->hideModerated;
    bool hideModerationActions = getSettings()->hideModerationActions;
    bool hideBlockedTermAutomodMessages =
        getSettings()->showBlockedTermAutomodMessages.getEnum() ==
        ShowModerationState::Never;
    bool hideSimilar = getSettings()->hideSimilar;
    bool hideReplies = !ctx.flags.has(MessageElementFlag::RepliedMessage);

    auto layoutPass = [&](qreal extraTopPadding) {
        this->container_.beginLayout(ctx.width, this->scale_,
                                     this->imageScale_, messageFlags,
                                     extraTopPadding);

        this->addElementsToContainer(ctx, hideModerated, hideModerationActions,
                                     hideBlockedTermAutomodMessages,
                                     hideSimilar, hideReplies);

        this->container_.endLayout();
    };

    layoutPass(0);

    // If content would sit underneath the 7TV highlight corner label,
    // re-layout with enough top padding to clear it.
    if (!this->seventvStacked_)
    {
        auto style = seventvHighlightStyle(
            *this->message_,
            this->flags.has(MessageLayoutFlag::IgnoreHighlights),
            currentUserLogin());
        if (style)
        {
            auto labelFont = getApp()->getFonts()->getFont(
                FontStyle::ChatSmall, this->scale_);
            labelFont.setBold(true);
            // Measure against a pixmap so the metrics match the DPI the
            // buffer is painted at (plain metrics can differ on
            // multi-monitor setups)
            static QPixmap fontProbe(1, 1);
            QFontMetricsF labelMetrics(labelFont, &fontProbe);

            // matches the label placement in updateBuffer; the label is
            // uppercase, so its glyphs end at the baseline (ascent)
            qreal labelWidth =
                labelMetrics.horizontalAdvance(style->label) +
                9 * this->scale_;
            qreal labelGlyphBottom =
                2 * this->scale_ + labelMetrics.ascent();
            QRectF labelZone(ctx.width - labelWidth, 0, labelWidth,
                             labelGlyphBottom + this->scale_);

            if (this->container_.anyElementIntersects(labelZone))
            {
                qreal padding = labelMetrics.ascent();

                if (!this->container_.anyImageElementIntersects(labelZone))
                {
                    // text glyphs start below the top of their line box,
                    // unlike emote images which fill theirs
                    QFontMetricsF textMetrics(
                        getApp()->getFonts()->getFont(FontStyle::ChatMedium,
                                                      this->scale_),
                        &fontProbe);
                    padding -= std::max<qreal>(
                        0, textMetrics.ascent() - textMetrics.capHeight() -
                               2 * this->scale_);
                }

                layoutPass(std::max<qreal>(0, padding));
            }
        }
    }

    if (this->height_ != this->container_.getHeight())
    {
        this->deleteBuffer();
    }

    this->height_ = this->container_.getHeight();

    // collapsed state
    this->flags.unset(MessageLayoutFlag::Collapsed);
    if (this->container_.isCollapsed())
    {
        this->flags.set(MessageLayoutFlag::Collapsed);
    }
}

void MessageLayout::addElementsToContainer(
    const MessageLayoutContext &ctx, bool hideModerated,
    bool hideModerationActions, bool hideBlockedTermAutomodMessages,
    bool hideSimilar, bool hideReplies)
{
    for (const auto &element : this->message_->elements)
    {
        if (hideModerated && this->message_->flags.has(MessageFlag::Disabled))
        {
            continue;
        }

        if (hideBlockedTermAutomodMessages &&
            this->message_->flags.has(MessageFlag::AutoModBlockedTerm))
        {
            // NOTE: This hides the message but it will make the message re-appear if moderation message hiding is no longer active, and the layout is re-laid-out.
            // This is only the case for the moderation messages that don't get filtered during creation.
            // We should decide which is the correct method & apply that everywhere
            continue;
        }

        if (this->message_->flags.has(MessageFlag::RestrictedMessage))
        {
            if (getApp()->getStreamerMode()->shouldHideRestrictedUsers())
            {
                // Message is being hidden because the source is a
                // restricted user
                continue;
            }
        }

        if (this->message_->flags.has(MessageFlag::ModerationAction))
        {
            if (hideModerationActions ||
                getApp()->getStreamerMode()->shouldHideModActions())
            {
                // Message is being hidden because we consider the message
                // a moderation action (something a streamer is unlikely to
                // want to share if they briefly show their chat on stream)
                continue;
            }
        }

        if (hideSimilar && this->message_->flags.has(MessageFlag::Similar))
        {
            continue;
        }

        if (hideReplies &&
            element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }

        element->addToContainer(this->container_, ctx);
    }
}

void MessageLayout::setSeventvStacked(bool stacked)
{
    if (this->seventvStacked_ != stacked)
    {
        this->seventvStacked_ = stacked;
        this->flags.set(MessageLayoutFlag::RequiresLayout);
        this->invalidateBuffer();
    }
}

bool MessageLayout::isSeventvStacked() const
{
    return this->seventvStacked_;
}

// Painting
MessagePaintResult MessageLayout::paint(const MessagePaintContext &ctx)
{
    MessagePaintResult result;

    QPixmap *pixmap = this->ensureBuffer(ctx.painter, ctx.canvasWidth,
                                         ctx.messageColors.hasTransparency);

    if (!this->bufferValid_)
    {
        if (ctx.messageColors.hasTransparency)
        {
            pixmap->fill(Qt::transparent);
        }
        this->updateBuffer(pixmap, ctx);
    }

    // draw on buffer
    ctx.painter.drawPixmap(QPoint{0, ctx.y}, *pixmap);

    // draw gif emotes
    result.hasAnimatedElements =
        this->container_.paintAnimatedElements(ctx.painter, ctx.y);

    // draw disabled
    if (this->message_->flags.has(MessageFlag::Disabled))
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (this->message_->flags.has(MessageFlag::RecentMessage) &&
        ctx.preferences.fadeMessageHistory)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (!ctx.isMentions &&
        (this->message_->flags.has(MessageFlag::RedeemedChannelPointReward) ||
         this->message_->flags.has(MessageFlag::RedeemedHighlight)) &&
        ctx.preferences.enableRedeemedHighlight)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                static_cast<int>(this->scale_ * 4),
                pixmap->height(),
            },
            *ColorProvider::instance().color(ColorType::RedeemedHighlight));
    }

    // draw selection
    if (!ctx.selection.isEmpty())
    {
        this->container_.paintSelection(ctx.painter, ctx.messageIndex,
                                        ctx.selection, ctx.y);
    }

    // draw message seperation line
    if (ctx.preferences.separateMessages)
    {
        ctx.painter.fillRect(
            QRectF{
                0.0,
                static_cast<qreal>(ctx.y),
                this->container_.getWidth() + 64,
                1.0,
            },
            ctx.messageColors.messageSeperator);
    }

    // draw last read message line
    if (ctx.isLastReadMessage)
    {
        QColor color;
        if (ctx.preferences.lastMessageColor.isValid())
        {
            color = ctx.preferences.lastMessageColor;
        }
        else
        {
            color = ctx.isWindowFocused
                        ? ctx.messageColors.focusedLastMessageLine
                        : ctx.messageColors.unfocusedLastMessageLine;
        }

        QBrush brush(color, ctx.preferences.lastMessagePattern);

        ctx.painter.fillRect(
            QRectF{
                0,
                ctx.y + this->container_.getHeight() - 1,
                static_cast<qreal>(pixmap->width()),
                1,
            },
            brush);
    }

    this->bufferValid_ = true;

    return result;
}

QPixmap *MessageLayout::ensureBuffer(QPainter &painter, qreal width, bool clear)
{
    if (this->buffer_ != nullptr)
    {
        return this->buffer_.get();
    }

    // Create new buffer
    this->buffer_ = std::make_unique<QPixmap>(
        static_cast<int>(width * painter.device()->devicePixelRatioF()),
        static_cast<int>(this->container_.getHeight() *
                         painter.device()->devicePixelRatioF()));
    this->buffer_->setDevicePixelRatio(painter.device()->devicePixelRatioF());

    if (clear)
    {
        this->buffer_->fill(Qt::transparent);
    }

    this->bufferValid_ = false;
    DebugCount::increase(DebugObject::MessageDrawingBuffer);
    return this->buffer_.get();
}

void MessageLayout::updateBuffer(QPixmap *buffer,
                                 const MessagePaintContext &ctx)
{
    if (buffer->isNull())
    {
        return;
    }

    QPainter painter(buffer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // draw background
    QColor backgroundColor = [&] {
        if (ctx.preferences.alternateMessages &&
            this->flags.has(MessageLayoutFlag::AlternateBackground))
        {
            return ctx.messageColors.alternateBg;
        }

        return ctx.messageColors.regularBg;
    }();

    auto seventvStyle = seventvHighlightStyle(
        *this->message_, this->flags.has(MessageLayoutFlag::IgnoreHighlights),
        currentUserLogin());

    if (seventvStyle)
    {
        // ~10% tint of the accent color
        auto tint = seventvStyle->accent;
        tint.setAlpha(26);
        backgroundColor = blendColors(backgroundColor, tint);
    }
    else if (this->message_->flags.has(MessageFlag::ElevatedMessage) &&
             ctx.preferences.enableElevatedMessageHighlight)
    {
        backgroundColor = blendColors(
            backgroundColor,
            *ctx.colorProvider.color(ColorType::ElevatedMessageHighlight));
    }

    else if (this->message_->flags.has(MessageFlag::FirstMessage) &&
             ctx.preferences.enableFirstMessageHighlight)
    {
        backgroundColor = blendColors(
            backgroundColor,
            *ctx.colorProvider.color(ColorType::FirstMessageHighlight));
    }
    else if (this->message_->flags.has(MessageFlag::WatchStreak) &&
             ctx.preferences.enableWatchStreakHighlight)
    {
        backgroundColor = blendColors(
            backgroundColor, *ctx.colorProvider.color(ColorType::WatchStreak));
    }
    else if ((this->message_->flags.has(MessageFlag::Highlighted) ||
              this->message_->flags.has(MessageFlag::HighlightedWhisper)) &&
             !this->flags.has(MessageLayoutFlag::IgnoreHighlights))
    {
        assert(this->message_->highlightColor);
        if (this->message_->highlightColor)
        {
            // Blend highlight color with usual background color
            backgroundColor =
                blendColors(backgroundColor, *this->message_->highlightColor);
        }
    }
    else if (this->message_->flags.has(MessageFlag::Announcement) &&
             ctx.preferences.enableAnnouncementHighlight)
    {
        backgroundColor = blendColors(
            backgroundColor,
            *ctx.colorProvider.color(colorTypeFromHelixAnnouncementColor(
                this->message_->announcementColor,
                ctx.preferences.enableColoredAnnouncementHighlight)));
    }
    else if (this->message_->flags.has(MessageFlag::Subscription) &&
             ctx.preferences.enableSubHighlight)
    {
        // Blend highlight color with usual background color
        backgroundColor = blendColors(
            backgroundColor, *ctx.colorProvider.color(ColorType::Subscription));
    }
    else if ((this->message_->flags.has(MessageFlag::RedeemedHighlight) ||
              this->message_->flags.has(
                  MessageFlag::RedeemedChannelPointReward)) &&
             ctx.preferences.enableRedeemedHighlight)
    {
        // Blend highlight color with usual background color
        backgroundColor =
            blendColors(backgroundColor,
                        *ctx.colorProvider.color(ColorType::RedeemedHighlight));
    }
    else if (this->message_->flags.has(MessageFlag::AutoMod) ||
             this->message_->flags.has(MessageFlag::LowTrustUsers))
    {
        if (ctx.preferences.enableAutomodHighlight &&
            (this->message_->flags.has(MessageFlag::AutoModOffendingMessage) ||
             this->message_->flags.has(
                 MessageFlag::AutoModOffendingMessageHeader)))
        {
            backgroundColor = blendColors(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::AutomodHighlight));
        }
        else
        {
            backgroundColor = QColor("#404040");
        }
    }
    else if (this->message_->flags.has(MessageFlag::Debug))
    {
        backgroundColor = QColor("#4A273D");
    }
    else if (this->message_->flags.has(MessageFlag::UncategorizedNotification))
    {
        // TODO: Give this a better/its own color :-)
        backgroundColor = blendColors(
            backgroundColor, *ctx.colorProvider.color(ColorType::Subscription));
    }

    painter.fillRect(buffer->rect(), backgroundColor);

    // draw message
    this->container_.paintElements(painter, ctx);

    if (seventvStyle && !this->seventvStacked_)
    {
        // corner label; the side borders are drawn by the ChannelView so
        // they can sit at the very edge of the view
        auto scale = this->scale_ > 0 ? this->scale_ : 1.0F;
        int width = this->container_.getWidth();
        int height = this->container_.getHeight();

        auto font =
            getApp()->getFonts()->getFont(FontStyle::ChatSmall, scale);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(seventvStyle->accent);
        // 3px side border + 2px gap
        QRect labelRect(0, static_cast<int>(2 * scale),
                        width - static_cast<int>(5 * scale), height);
        painter.drawText(labelRect, Qt::AlignRight | Qt::AlignTop,
                         seventvStyle->label);
    }

#ifdef FOURTF
    // debug
    painter.setPen(QColor(255, 0, 0));
    painter.drawRect(buffer->rect().x(), buffer->rect().y(),
                     buffer->rect().width() - 1, buffer->rect().height() - 1);

    QTextOption option;
    option.setAlignment(Qt::AlignRight | Qt::AlignTop);

    painter.drawText(QRectF(1, 1, this->container_.getWidth() - 3, 1000),
                     QString::number(this->layoutCount_) + ", " +
                         QString::number(++this->bufferUpdatedCount_),
                     option);
#endif
}

void MessageLayout::invalidateBuffer()
{
    this->bufferValid_ = false;
}

void MessageLayout::deleteBuffer()
{
    if (this->buffer_ != nullptr)
    {
        DebugCount::decrease(DebugObject::MessageDrawingBuffer);

        this->buffer_ = nullptr;
    }
}

void MessageLayout::deleteCache()
{
    this->deleteBuffer();

#ifdef XD
    this->container_.clear();
#endif
}

// Elements
//    assert(QThread::currentThread() == QApplication::instance()->thread());

// returns nullptr if none was found

// fourtf: this should return a MessageLayoutItem
const MessageLayoutElement *MessageLayout::getElementAt(QPointF point) const
{
    // go through all words and return the first one that contains the point.
    return this->container_.getElementAt(point);
}

std::pair<int, int> MessageLayout::getWordBounds(
    const MessageLayoutElement *hoveredElement, QPointF relativePos) const
{
    // An element with wordId != -1 can be multiline, so we need to check all
    // elements in the container
    if (hoveredElement->getWordId() != -1)
    {
        return this->container_.getWordBounds(hoveredElement);
    }

    const auto wordStart = this->getSelectionIndex(relativePos) -
                           hoveredElement->getMouseOverIndex(relativePos);
    const auto selectionLength = hoveredElement->getSelectionIndexCount();
    const auto length = hoveredElement->hasTrailingSpace() ? selectionLength - 1
                                                           : selectionLength;

    return {wordStart, wordStart + length};
}

size_t MessageLayout::getLastCharacterIndex() const
{
    return this->container_.getLastCharacterIndex();
}

size_t MessageLayout::getFirstMessageCharacterIndex() const
{
    return this->container_.getFirstMessageCharacterIndex();
}

size_t MessageLayout::getSelectionIndex(QPointF position) const
{
    return this->container_.getSelectionIndex(position);
}

void MessageLayout::addSelectionText(QString &str, uint32_t from, uint32_t to,
                                     CopyMode copymode)
{
    this->container_.addSelectionText(str, from, to, copymode);
}

}  // namespace chatterino
