// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/completion/sources/EmoteSource.hpp"
#include "controllers/completion/strategies/Strategy.hpp"

namespace chatterino::completion {

class ClassicEmoteStrategy : public Strategy<EmoteItem>
{
    void apply(const std::vector<EmoteItem> &items,
               std::vector<EmoteItem> &output,
               const QString &query) const override;
};

class ClassicTabEmoteStrategy : public Strategy<EmoteItem>
{
public:
    /// @param includeEmojis Match emojis even when the query doesn't start
    /// with ':' (used by the tab emote wheel).
    ClassicTabEmoteStrategy(bool includeEmojis = false)
        : includeEmojis_(includeEmojis)
    {
    }

private:
    void apply(const std::vector<EmoteItem> &items,
               std::vector<EmoteItem> &output,
               const QString &query) const override;

    bool includeEmojis_;
};

}  // namespace chatterino::completion
