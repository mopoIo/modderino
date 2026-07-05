// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayoutContext.hpp"

#include "messages/Message.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <algorithm>

namespace chatterino {

void MessageColors::applyTheme(Theme *theme, bool isOverlay,
                               int backgroundOpacity)
{
    auto applyColors = [this](const auto &src) {
        this->regularBg = src.backgrounds.regular;
        this->alternateBg = src.backgrounds.alternate;

        this->disabled = src.disabled;
        this->selection = src.selection;

        this->regularText = src.textColors.regular;
        this->linkText = src.textColors.link;
        this->systemText = src.textColors.system;
    };

    if (isOverlay)
    {
        this->channelBackground = theme->overlayMessages.background;
        this->channelBackground.setAlpha(std::clamp(backgroundOpacity, 0, 255));
        applyColors(theme->overlayMessages);
    }
    else
    {
        this->channelBackground = theme->splits.background;
        applyColors(theme->messages);
    }

    this->messageSeperator = theme->splits.messageSeperator;

    this->focusedLastMessageLine = theme->tabs.selected.backgrounds.regular;
    this->unfocusedLastMessageLine = theme->tabs.selected.backgrounds.unfocused;

    this->hasTransparency =
        this->regularBg.alpha() != 255 || this->alternateBg.alpha() != 255;
}

void MessagePreferences::connectSettings(Settings *settings,
                                         pajlada::Signals::SignalHolder &holder)
{
    settings->enableRedeemedHighlight.connect(
        [this](const auto &newValue) {
            this->enableRedeemedHighlight = newValue;
        },
        holder);

    settings->seventvStyledHighlights.connect(
        [this](const auto &newValue) {
            this->seventvStyledHighlights = newValue;
        },
        holder);

    settings->enableElevatedMessageHighlight.connect(
        [this](const auto &newValue) {
            this->enableElevatedMessageHighlight = newValue;
        },
        holder);

    settings->enableFirstMessageHighlight.connect(
        [this](const auto &newValue) {
            this->enableFirstMessageHighlight = newValue;
        },
        holder);

    settings->enableSubHighlight.connect(
        [this](const auto &newValue) {
            this->enableSubHighlight = newValue;
        },
        holder);

    settings->enableWatchStreakHighlight.connect(
        [this](const auto &newValue) {
            this->enableWatchStreakHighlight = newValue;
        },
        holder);

    settings->enableAutomodHighlight.connect(
        [this](const auto &newValue) {
            this->enableAutomodHighlight = newValue;
        },
        holder);

    settings->enableAnnouncementHighlight.connect(
        [this](const auto &newValue) {
            this->enableAnnouncementHighlight = newValue;
        },
        holder);
    settings->enableColoredAnnouncementHighlight.connect(
        [this](const auto &newValue) {
            this->enableColoredAnnouncementHighlight = newValue;
        },
        holder);

    settings->alternateMessages.connect(
        [this](const auto &newValue) {
            this->alternateMessages = newValue;
        },
        holder);

    settings->separateMessages.connect(
        [this](const auto &newValue) {
            this->separateMessages = newValue;
        },
        holder);

    settings->lastMessageColor.connect(
        [this](const auto &newValue) {
            if (newValue.isEmpty())
            {
                this->lastMessageColor = QColor();
            }
            else
            {
                this->lastMessageColor = QColor(newValue);
            }
        },
        holder);

    settings->lastMessagePattern.connect(
        [this](const auto &newValue) {
            this->lastMessagePattern = static_cast<Qt::BrushStyle>(newValue);
        },
        holder);

    settings->fadeMessageHistory.connect(
        [this](const auto &newValue) {
            this->fadeMessageHistory = newValue;
        },
        holder);
}

std::optional<SeventvHighlightStyle> seventvHighlightStyle(
    const Message &message, bool ignoreHighlights, const QString &currentLogin)
{
    auto *settings = getSettings();
    if (!settings->seventvStyledHighlights)
    {
        return std::nullopt;
    }

    const auto &flags = message.flags;

    if ((flags.has(MessageFlag::Highlighted) ||
         flags.has(MessageFlag::HighlightedWhisper)) &&
        !ignoreHighlights)
    {
        if (!currentLogin.isEmpty() &&
            message.messageText.contains(currentLogin, Qt::CaseInsensitive))
        {
            return SeventvHighlightStyle{QColor(0xe1, 0x32, 0x32),
                                         "MENTIONS YOU"};
        }
        return SeventvHighlightStyle{QColor(0x94, 0xb8, 0xff), "HIGHLIGHT"};
    }
    if (flags.has(MessageFlag::FirstMessage) &&
        settings->enableFirstMessageHighlight)
    {
        return SeventvHighlightStyle{QColor(0xc8, 0x32, 0xc8), "FIRST MESSAGE"};
    }
    if (flags.has(MessageFlag::Announcement) &&
        settings->enableAnnouncementHighlight)
    {
        return SeventvHighlightStyle{QColor(0x91, 0x46, 0xff), "ANNOUNCEMENT"};
    }
    if (flags.has(MessageFlag::Subscription) && settings->enableSubHighlight)
    {
        return SeventvHighlightStyle{QColor(0x91, 0x46, 0xff), "SUBSCRIBED"};
    }
    if (flags.has(MessageFlag::WatchStreak) &&
        settings->enableWatchStreakHighlight)
    {
        return SeventvHighlightStyle{QColor(0xc9, 0xa2, 0x27), "WATCH STREAK"};
    }
    if ((flags.has(MessageFlag::RedeemedHighlight) ||
         flags.has(MessageFlag::RedeemedChannelPointReward)) &&
        settings->enableRedeemedHighlight)
    {
        return SeventvHighlightStyle{QColor(0x94, 0xb8, 0xff), "HIGHLIGHTED"};
    }

    return std::nullopt;
}

}  // namespace chatterino
