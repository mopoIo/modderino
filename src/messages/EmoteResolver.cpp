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
#include "singletons/Paths.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include <utility>

namespace chatterino {

namespace {

/// Persisted alias -> image-URL cache of emotes recently resolved for HUD
/// text (tab titles, split headers, ...). At startup, before the emote
/// providers have loaded, resolveEmote() falls back to this so titles show
/// last session's image right away instead of text. Once the account's
/// emote sets are in, the live resolution always wins (and refreshes the
/// cache), so an alias that was rebound or deleted since last session
/// self-corrects. The pixmaps themselves come from the URL-keyed image
/// cache, so cached entries render without hitting the network.
class TitleEmoteCache
{
public:
    static TitleEmoteCache &instance()
    {
        static TitleEmoteCache cache;
        return cache;
    }

    /// Last-session fallback for `code`; nullptr if it was never cached.
    EmotePtr lookup(const QString &code)
    {
        this->load();

        auto cached = this->synthesized_.find(code);
        if (cached != this->synthesized_.end())
        {
            return cached.value();
        }

        auto it = this->entries_.find(code);
        if (it == this->entries_.end())
        {
            return nullptr;
        }

        auto emote = std::make_shared<const Emote>(Emote{
            EmoteName{code},
            ImageSet{Url{it->url1x}, Url{it->url2x}, Url{it->url3x}},
            Tooltip{code + "<br>Emote (cached)"},
            Url{},
        });
        this->synthesized_.insert(code, emote);
        return emote;
    }

    /// Remembers the live resolution of `code` for the next session.
    void store(const QString &code, const EmotePtr &emote)
    {
        this->load();

        Entry entry;
        auto urlOf = [](const ImagePtr &image) {
            return image ? image->url().string : QString();
        };
        entry.url1x = urlOf(emote->images.getImage1());
        entry.url2x = urlOf(emote->images.getImage2());
        entry.url3x = urlOf(emote->images.getImage3());
        if (entry.url1x.isEmpty())
        {
            return;
        }

        auto it = this->entries_.find(code);
        if (it != this->entries_.end() && it->url1x == entry.url1x &&
            it->url2x == entry.url2x && it->url3x == entry.url3x)
        {
            return;
        }

        // Keep the cache bounded; evicted codes just show as text for one
        // startup.
        while (this->entries_.size() >= 512)
        {
            this->entries_.erase(this->entries_.begin());
        }
        this->entries_.insert(code, entry);
        this->synthesized_.remove(code);
        this->scheduleSave();
    }

private:
    struct Entry {
        QString url1x;
        QString url2x;
        QString url3x;
    };

    QString filePath() const
    {
        return getApp()->getPaths().cacheFilePath("title-emotes.json");
    }

    void load()
    {
        if (this->loaded_)
        {
            return;
        }
        this->loaded_ = true;

        QFile file(this->filePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            return;
        }
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = root.begin(); it != root.end(); ++it)
        {
            const auto entry = it.value().toObject();
            this->entries_.insert(it.key(), {
                                                entry["1x"].toString(),
                                                entry["2x"].toString(),
                                                entry["3x"].toString(),
                                            });
        }
    }

    void scheduleSave()
    {
        if (this->savePending_)
        {
            return;
        }
        this->savePending_ = true;

        // Coalesce the burst of stores right after the emote providers load.
        QTimer::singleShot(5000, QCoreApplication::instance(), [this] {
            this->savePending_ = false;
            this->save();
        });
    }

    void save() const
    {
        QJsonObject root;
        for (auto it = this->entries_.begin(); it != this->entries_.end();
             ++it)
        {
            QJsonObject entry;
            entry["1x"] = it->url1x;
            if (!it->url2x.isEmpty())
            {
                entry["2x"] = it->url2x;
            }
            if (!it->url3x.isEmpty())
            {
                entry["3x"] = it->url3x;
            }
            root[it.key()] = entry;
        }

        QSaveFile file(this->filePath());
        if (!file.open(QIODevice::WriteOnly))
        {
            return;
        }
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }

    QHash<QString, Entry> entries_;
    /// Synthesized fallback emotes, memoized so repeated paints during
    /// startup reuse the same Emote/Image objects.
    QHash<QString, EmotePtr> synthesized_;
    bool loaded_ = false;
    bool savePending_ = false;
};

/// Whether the current account's Twitch emote sets have arrived. Until they
/// have, failed lookups may use last session's cache; afterwards the live
/// data is authoritative.
bool liveEmotesLoaded()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        // No account emotes will ever load; don't hold onto stale images.
        return true;
    }
    auto guard = account->accessEmotes();
    auto emotes = *guard;
    return emotes && !emotes->empty();
}

EmotePtr resolveEmoteLive(const ChannelPtr &channel, const QString &code)
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

}  // namespace

EmotePtr resolveEmote(const ChannelPtr &channel, const QString &code)
{
    if (auto emote = resolveEmoteLive(channel, code))
    {
        TitleEmoteCache::instance().store(code, emote);
        return emote;
    }

    if (!liveEmotesLoaded())
    {
        return TitleEmoteCache::instance().lookup(code);
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
