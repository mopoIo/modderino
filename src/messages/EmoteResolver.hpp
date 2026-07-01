// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/EmojiText.hpp"  // EmojiTextRun, EmotePtr

#include <QString>

#include <memory>
#include <vector>

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;

/// Resolves an emote code that can be used in the given channel context, in the
/// same spirit as chat:
///   - `channel`'s FrankerFaceZ / BetterTTV / 7TV / Twitch channel emotes
///     (when it's a Twitch channel)
///   - the current Twitch account's emotes (global + owned/subscribed sets)
///   - 7TV personal emotes for the current Twitch user
///   - global BetterTTV / FrankerFaceZ / 7TV emotes
///
/// `channel` may be null, in which case only the account + global emotes are
/// considered. Returns nullptr if `code` isn't a usable emote.
EmotePtr resolveEmote(const ChannelPtr &channel, const QString &code);

/// Splits `text` into runs of plain text, emoji images, and emote images usable
/// in `channel`'s context. Whole (space-delimited) words that match an emote
/// code become emote runs; every other word is emoji-parsed (see
/// parseEmojiText). Whitespace is preserved as text runs so the runs can be laid
/// out on a single line.
std::vector<EmojiTextRun> parseEmotesAndEmojis(const QString &text,
                                               const ChannelPtr &channel = {});

}  // namespace chatterino
