// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <memory>
#include <vector>

class QPainter;
class QFontMetricsF;
class QRectF;
class QWidget;

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

/// How many times a widget will repaint itself waiting for emoji images to
/// load before giving up (a safety bound so a failing image can't spin forever).
constexpr int EMOJI_LOAD_REPAINT_ATTEMPTS = 40;

/// A single run of a piece of text that may contain emojis.
///
/// Exactly one of `text`/`emote` is set: text runs carry a (non-empty) `text`
/// and a null `emote`, emoji runs carry a non-null `emote` (an emoji image) and
/// an empty `text`.
struct EmojiTextRun {
    QString text;
    EmotePtr emote;

    bool isEmoji() const
    {
        return this->emote != nullptr;
    }
};

/// Splits `text` into runs of plain text interleaved with emoji images, using
/// the global emoji provider (`getApp()->getEmotes()->getEmojis()`).
std::vector<EmojiTextRun> parseEmojiText(const QString &text);

/// Returns true if any of the given runs is an emoji image.
bool emojiTextRunsHaveEmoji(const std::vector<EmojiTextRun> &runs);

/// Computes the pixel width of the given runs when laid out on a single line
/// with the given font metrics. Emoji are laid out as squares of the line
/// height.
qreal emojiTextWidth(const QFontMetricsF &metrics,
                     const std::vector<EmojiTextRun> &runs);

/// Convenience overload that parses `text` first.
qreal emojiTextWidth(const QFontMetricsF &metrics, const QString &text);

/// Elides the given runs from the right (adding a trailing ellipsis) so they
/// fit within `maxWidth`. Returns the runs to draw. If the runs already fit,
/// they are returned unchanged.
std::vector<EmojiTextRun> elideEmojiTextRuns(const QFontMetricsF &metrics,
                                             std::vector<EmojiTextRun> runs,
                                             qreal maxWidth);

/// Draws the given runs into `rect` on a single line, vertically centered,
/// using the painter's current pen for text. The horizontal bits of
/// `alignment` (AlignLeft / AlignRight / AlignHCenter / AlignCenter) decide
/// where the line is placed within `rect`. Drawing is clipped to `rect`.
///
/// Returns true if every emoji image was already loaded. If it returns false,
/// some emoji were still loading (their space was reserved but nothing was
/// drawn there yet) and the caller should schedule a repaint to pick them up.
bool drawEmojiText(QPainter &painter, const std::vector<EmojiTextRun> &runs,
                   const QFontMetricsF &metrics, const QRectF &rect,
                   Qt::Alignment alignment);

/// Schedules a delayed repaint of `widget` when `ready` is false (some emoji
/// images were still loading during the last paint), so the images appear once
/// they finish downloading.
///
/// `attemptsRemaining` bounds the number of retries to avoid an endless repaint
/// loop if an image never loads; it is reset back to `EMOJI_LOAD_REPAINT_ATTEMPTS`
/// whenever `ready` is true. Callers should also reset it when their text
/// changes.
void scheduleEmojiRepaint(QWidget *widget, int &attemptsRemaining, bool ready);

}  // namespace chatterino
