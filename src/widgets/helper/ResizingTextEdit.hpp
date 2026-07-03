// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QCompleter>
#include <QKeyEvent>
#include <QTextEdit>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class Image;
using ImagePtr = std::shared_ptr<Image>;

class ResizingTextEdit : public QTextEdit
{
public:
    ResizingTextEdit();

    struct InlineEmote {
        /// The text this image serializes back to (emote name/unicode emoji)
        QString text;
        EmotePtr emote;
        bool isEmoji = false;
    };
    using InlineEmoteResolver =
        std::function<std::optional<InlineEmote>(const QString &word)>;

    /// Inline emote rendering is inactive without a resolver.
    void setInlineEmoteResolver(InlineEmoteResolver resolver);

    /// The document's text with inline emote images replaced by their
    /// original text (emote name or unicode emoji).
    QString serializedText() const;

    /// Try to convert the word directly before the cursor (skipping trailing
    /// spaces) into an inline emote image.
    void tryConvertWordBeforeCursor();

    /// Convert every word in [start, end) that matches an emote name into
    /// an inline emote image (used for pasted text). The range is expanded
    /// to word boundaries.
    void convertInlineEmotesInRange(int start, int end);

    /// Begin a live emote preview over [wordStart, wordEnd): the range's
    /// contents get swapped by updateInlineEmotePreview while the
    /// Tab-completion wheel cycles through matches.
    void beginInlineEmotePreview(int wordStart, int wordEnd);
    /// Swap the previewed range to show `emote`.
    void updateInlineEmotePreview(const InlineEmote &emote);
    /// End the preview. With `restoreText` the range is replaced by that
    /// text (cancel); otherwise the previewed emote is kept, followed by a
    /// space when `addSpace` is set. The caret ends up after the result.
    void finishInlineEmotePreview(const std::optional<QString> &restoreText,
                                  bool addSpace);

    QSize sizeHint() const override;

    bool hasHeightForWidth() const override;
    bool isFirstWord() const;

    pajlada::Signals::Signal<QKeyEvent *> keyPressed;
    pajlada::Signals::NoArgSignal focused;
    pajlada::Signals::NoArgSignal focusLost;
    pajlada::Signals::Signal<const QMimeData *> imagePasted;
    pajlada::Signals::Signal<QMenu *, QPoint> contextMenuRequested;

    struct InlineEmoteTooltip {
        ImagePtr image;
        QString text;
    };
    /// Fired on every mouse move over an inline emote image.
    pajlada::Signals::Signal<const InlineEmoteTooltip &, QPoint>
        inlineEmoteHovered;
    pajlada::Signals::NoArgSignal inlineEmoteHoverEnded;

    void setCompleter(QCompleter *c);
    /**
     * Resets a completion for this text if one was is progress.
     * See `completionInProgress_`.
     */
    void resetCompletion();

protected:
    int heightForWidth(int) const override;
    void keyPressEvent(QKeyEvent *event) override;

    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    QMimeData *createMimeDataFromSelection() const override;

    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    // hadSpace is set to true in case the "textUnderCursor" word was after a
    // space
    QString textUnderCursor(bool *hadSpace = nullptr) const;

    QString serializeRange(int start, int end) const;

    /// Replace `cursor`'s selection with an inline emote image; queued for
    /// retry if the image isn't loaded yet.
    void insertInlineEmote(QTextCursor cursor, const InlineEmote &emote);

    /// Returns false when the image isn't loaded yet.
    bool tryInsertLoadedInlineEmote(QTextCursor &cursor,
                                    const InlineEmote &emote);

    /// Backspace after an emote image turns it back into its typed name.
    bool tryUnwrapInlineEmote();

    void checkPendingInlineEmotes();

    /// Convert unicode emoji in recently inserted text to inline images.
    void scanForEmoji();

    /// Driven by the global GIF timer.
    void updateAnimatedInlineImages();

    bool inlineEmotesEnabled() const;

    InlineEmoteResolver inlineEmoteResolver_;

    struct PendingInlineEmote {
        QTextCursor cursor;
        QString originalText;
        InlineEmote emote;
        int attemptsLeft = 0;
    };
    std::vector<PendingInlineEmote> pendingInlineEmotes_;
    QTimer pendingInlineEmoteTimer_;

    void cancelPendingInlineEmotes(int start, int end);

    /// Live-preview range for the Tab emote wheel. The start marker keeps
    /// its position on inserts, the end marker moves with them, so the
    /// pair keeps spanning the preview across content swaps.
    QTextCursor previewStart_;
    QTextCursor previewEnd_;
    bool previewActive_ = false;

    std::vector<QTextCursor> pendingEmojiScans_;
    bool emojiScanQueued_ = false;

    struct AnimatedInlineImage {
        ImagePtr image;
        /// Device pixels
        int deviceHeight = 0;
    };
    /// Keyed by document resource name
    std::map<QString, AnimatedInlineImage> animatedInlineImages_;
    /// Keyed by document resource name
    std::map<QString, InlineEmoteTooltip> inlineEmoteTooltips_;
    QString hoveredInlineEmote_;

    pajlada::Signals::SignalHolder managedConnections_;

    /// Guards our own document edits from re-triggering conversion scans.
    bool convertingInlineEmote_ = false;

    QCompleter *completer_ = nullptr;
    /**
     * This is true if a completion was done but the user didn't type yet,
     * and might want to press `Tab` again to get the next completion
     * on the original text.
     *
     * For example:
     *
     * input: "pog"
     * `Tab` pressed:
     *   - complete to "PogBones"
     *   - retain "pog" for next completion
     *   - set `completionInProgress_ = true`
     * `Tab` pressed again:
     *   - complete ["pog"] to "PogChamp"
     *
     * [other key] pressed or cursor moved - updating the input text:
     *   - set `completionInProgress_ = false`
     */
    bool completionInProgress_ = false;

    bool eventFilter(QObject *obj, QEvent *event) override;

private Q_SLOTS:
    void insertCompletion(const QString &completion);
};

}  // namespace chatterino
