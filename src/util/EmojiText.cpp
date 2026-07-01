// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/EmojiText.hpp"

#include "Application.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "providers/emoji/Emojis.hpp"
#include "util/Variant.hpp"

#include <QFontMetricsF>
#include <QPainter>
#include <QRectF>
#include <QTimer>
#include <QWidget>

#include <optional>
#include <variant>

namespace {

using namespace chatterino;

/// Returns the (square) side length used for an emoji laid out on a line with
/// the given font metrics.
qreal emojiSizeFor(const QFontMetricsF &metrics)
{
    return metrics.height();
}

/// Returns the drawn width of an image run whose height is `size`. Emojis are
/// square, but emotes can be wider/taller, so we keep the image's aspect ratio.
/// While the image is still loading (aspect unknown) we reserve a square.
qreal imageRunWidth(const EmotePtr &emote, qreal size)
{
    if (emote)
    {
        const auto &image = emote->images.getImage1();
        if (image)
        {
            if (auto pixmap = image->pixmapOrLoad())
            {
                if (pixmap->height() > 0)
                {
                    return size * (static_cast<qreal>(pixmap->width()) /
                                   static_cast<qreal>(pixmap->height()));
                }
            }
        }
    }
    return size;
}

/// Returns the longest prefix of `text` (respecting surrogate pairs) that fits
/// within `maxWidth`.
QString trimTextToWidth(const QFontMetricsF &metrics, const QString &text,
                        qreal maxWidth)
{
    QString result;
    qreal width = 0;
    qsizetype i = 0;
    while (i < text.size())
    {
        qsizetype charLen = 1;
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() &&
            text.at(i + 1).isLowSurrogate())
        {
            charLen = 2;
        }

        QString piece = text.mid(i, charLen);
        qreal pieceWidth = metrics.horizontalAdvance(piece);
        if (width + pieceWidth > maxWidth)
        {
            break;
        }

        result += piece;
        width += pieceWidth;
        i += charLen;
    }
    return result;
}

}  // namespace

namespace chatterino {

std::vector<EmojiTextRun> parseEmojiText(const QString &text)
{
    std::vector<EmojiTextRun> runs;

    auto *emojis = getApp()->getEmotes()->getEmojis();
    for (const auto &variant : emojis->parse(text))
    {
        std::visit(variant::Overloaded{
                       [&](const EmotePtr &emote) {
                           runs.push_back({{}, emote});
                       },
                       [&](QStringView textRun) {
                           if (!textRun.isEmpty())
                           {
                               runs.push_back({textRun.toString(), nullptr});
                           }
                       },
                   },
                   variant);
    }

    return runs;
}

bool emojiTextRunsHaveEmoji(const std::vector<EmojiTextRun> &runs)
{
    for (const auto &run : runs)
    {
        if (run.isEmoji())
        {
            return true;
        }
    }
    return false;
}

qreal emojiTextWidth(const QFontMetricsF &metrics,
                     const std::vector<EmojiTextRun> &runs)
{
    qreal emojiSize = emojiSizeFor(metrics);
    qreal width = 0;
    for (const auto &run : runs)
    {
        if (run.isEmoji())
        {
            width += imageRunWidth(run.emote, emojiSize);
        }
        else
        {
            width += metrics.horizontalAdvance(run.text);
        }
    }
    return width;
}

qreal emojiTextWidth(const QFontMetricsF &metrics, const QString &text)
{
    return emojiTextWidth(metrics, parseEmojiText(text));
}

std::vector<EmojiTextRun> elideEmojiTextRuns(const QFontMetricsF &metrics,
                                             std::vector<EmojiTextRun> runs,
                                             qreal maxWidth)
{
    if (emojiTextWidth(metrics, runs) <= maxWidth)
    {
        return runs;
    }

    const QString ellipsis = QStringLiteral("…");
    qreal ellipsisWidth = metrics.horizontalAdvance(ellipsis);
    qreal emojiSize = emojiSizeFor(metrics);
    // reserve room for the trailing ellipsis
    qreal budget = maxWidth - ellipsisWidth;

    std::vector<EmojiTextRun> out;
    qreal used = 0;
    for (const auto &run : runs)
    {
        if (used >= budget)
        {
            break;
        }

        if (run.isEmoji())
        {
            qreal runWidth = imageRunWidth(run.emote, emojiSize);
            if (used + runWidth > budget)
            {
                break;
            }
            out.push_back(run);
            used += runWidth;
        }
        else
        {
            qreal runWidth = metrics.horizontalAdvance(run.text);
            if (used + runWidth <= budget)
            {
                out.push_back(run);
                used += runWidth;
            }
            else
            {
                QString fitted =
                    trimTextToWidth(metrics, run.text, budget - used);
                if (!fitted.isEmpty())
                {
                    out.push_back({fitted, nullptr});
                }
                break;
            }
        }
    }

    out.push_back({ellipsis, nullptr});
    return out;
}

bool drawEmojiText(QPainter &painter, const std::vector<EmojiTextRun> &runs,
                   const QFontMetricsF &metrics, const QRectF &rect,
                   Qt::Alignment alignment)
{
    qreal emojiSize = emojiSizeFor(metrics);
    qreal total = emojiTextWidth(metrics, runs);

    qreal x = rect.left();
    Qt::Alignment horizontal = alignment & Qt::AlignHorizontal_Mask;
    if (horizontal & Qt::AlignRight)
    {
        x = rect.right() - total;
    }
    else if (horizontal & Qt::AlignHCenter)
    {
        x = rect.left() + (rect.width() - total) / 2.0;
    }
    // Never start drawing before the rect when the text overflows.
    if (x < rect.left())
    {
        x = rect.left();
    }

    qreal baseline =
        rect.center().y() + (metrics.ascent() - metrics.descent()) / 2.0;
    qreal emojiTop = rect.center().y() - emojiSize / 2.0;

    bool allReady = true;

    painter.save();
    painter.setClipRect(rect, Qt::IntersectClip);
    // Emoji source images are 64px; enable smooth scaling so they look crisp
    // when drawn at line height.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const auto &run : runs)
    {
        if (run.isEmoji())
        {
            qreal runWidth = imageRunWidth(run.emote, emojiSize);
            const auto &image = run.emote->images.getImage1();
            std::optional<QPixmap> pixmap;
            if (image)
            {
                pixmap = image->pixmapOrLoad();
            }

            if (pixmap)
            {
                QRectF target(x, emojiTop, runWidth, emojiSize);
                painter.drawPixmap(target, *pixmap, QRectF{(*pixmap).rect()});
            }
            else
            {
                // The image is still loading; its space is reserved but nothing
                // is drawn yet. The caller should repaint once it's ready.
                allReady = false;
            }
            x += runWidth;
        }
        else
        {
            painter.drawText(QPointF(x, baseline), run.text);
            x += metrics.horizontalAdvance(run.text);
        }
    }
    painter.restore();

    return allReady;
}

void scheduleEmojiRepaint(QWidget *widget, int &attemptsRemaining, bool ready)
{
    if (ready)
    {
        // Everything is loaded; refill the budget for the next time an
        // unloaded emoji shows up.
        attemptsRemaining = EMOJI_LOAD_REPAINT_ATTEMPTS;
        return;
    }

    if (attemptsRemaining <= 0)
    {
        return;
    }
    attemptsRemaining--;

    // ~50ms * EMOJI_LOAD_REPAINT_ATTEMPTS gives images a couple of seconds to
    // arrive before we stop retrying.
    QTimer::singleShot(50, widget, [widget] {
        widget->update();
    });
}

}  // namespace chatterino
