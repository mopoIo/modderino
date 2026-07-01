// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/EmoteResolver.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <utility>

namespace chatterino {

EmotePtr resolveEmote(const ChannelPtr &channel, const QString &code)
{
    if (code.isEmpty())
    {
        return nullptr;
    }

    EmoteName name{code};
    auto *app = getApp();

    // Channel-specific emotes (only for Twitch channels).
    if (const auto *tc = dynamic_cast<const TwitchChannel *>(channel.get()))
    {
        if (auto e = tc->ffzEmote(name))
        {
            return *e;
        }
        if (auto e = tc->bttvEmote(name))
        {
            return *e;
        }
        if (auto e = tc->seventvEmote(name))
        {
            return *e;
        }
    }

    // Current Twitch account: its own emotes + 7TV personal emotes.
    auto account = app->getAccounts()->twitch.getCurrent();
    if (account)
    {
        {
            auto guard = account->accessEmotes();
            // copy the shared_ptr so the map stays alive after the guard is
            // released
            auto emotes = *guard;
            if (emotes)
            {
                auto it = emotes->find(name);
                if (it != emotes->end())
                {
                    return it->second;
                }
            }
        }

        if (auto personal =
                app->getSeventvPersonalEmotes()->getEmoteForTwitchUser(
                    account->getUserId(), name))
        {
            return personal;
        }
    }

    // Global third-party emotes.
    if (auto e = app->getBttvEmotes()->emote(name))
    {
        return *e;
    }
    if (auto e = app->getFfzEmotes()->emote(name))
    {
        return *e;
    }
    if (auto e = app->getSeventvEmotes()->globalEmote(name))
    {
        return *e;
    }

    return nullptr;
}

std::vector<EmojiTextRun> parseEmotesAndEmojis(const QString &text,
                                               const ChannelPtr &channel)
{
    std::vector<EmojiTextRun> runs;

    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n)
    {
        if (text.at(i).isSpace())
        {
            qsizetype start = i;
            while (i < n && text.at(i).isSpace())
            {
                ++i;
            }
            runs.push_back({text.mid(start, i - start), nullptr});
            continue;
        }

        qsizetype start = i;
        while (i < n && !text.at(i).isSpace())
        {
            ++i;
        }
        QString word = text.mid(start, i - start);

        // A whole word matching an emote code becomes an emote image, mirroring
        // how emotes are matched in chat.
        if (auto emote = resolveEmote(channel, word))
        {
            runs.push_back({{}, emote});
            continue;
        }

        // Otherwise, look for unicode emoji inside the word.
        for (auto &run : parseEmojiText(word))
        {
            runs.push_back(std::move(run));
        }
    }

    return runs;
}

}  // namespace chatterino
