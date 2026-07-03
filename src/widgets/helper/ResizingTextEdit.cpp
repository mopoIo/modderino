// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ResizingTextEdit.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/emoji/Emojis.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "singletons/Settings.hpp"

#include <QAbstractTextDocumentLayout>
#include <QMenu>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QObject>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextImageFormat>
#include <QUrl>

#include <algorithm>
#include <set>
#include <utility>
#include <variant>

using namespace Qt::Literals;

namespace chatterino {

namespace {

/// The text an inline emote image serializes back to
constexpr int INLINE_EMOTE_TEXT = QTextFormat::UserProperty + 71;
/// Emoji delete like a regular character instead of unwrapping to a name
constexpr int INLINE_EMOTE_IS_EMOJI = QTextFormat::UserProperty + 72;

/// Image-load retries, at 100ms per attempt
constexpr int INLINE_EMOTE_LOAD_ATTEMPTS = 50;

}  // namespace

ResizingTextEdit::ResizingTextEdit()
{
    auto sizePolicy = this->sizePolicy();
    sizePolicy.setHeightForWidth(true);
    sizePolicy.setVerticalPolicy(QSizePolicy::Preferred);
    this->setSizePolicy(sizePolicy);
    this->setAcceptRichText(false);

    QObject::connect(this, &QTextEdit::textChanged, this,
                     &QWidget::updateGeometry);

    QObject::connect(this, &QTextEdit::cursorPositionChanged, [this]() {
        // If tab was pressed and we're completing/replacing the current word,
        // this code will not even be called, see ResizingTextEdit::keyPressEvent

        if (!this->completionInProgress_)
        {
            return;
        }
        qCDebug(chatterinoCommon)
            << "Finishing completion because cursor moved";
        this->completionInProgress_ = false;
    });

    // Whenever the setting for emote completion changes, force a
    // refresh on the completion model the next time "Tab" is pressed
    getSettings()->prefixOnlyEmoteCompletion.connect([this] {
        this->completionInProgress_ = false;
    });

    this->setFocusPolicy(Qt::ClickFocus);
    this->installEventFilter(this);
    // hover detection for inline emote tooltips
    this->viewport()->setMouseTracking(true);

    // Mutating the document inside contentsChange is unsafe; remember the
    // inserted range (QTextCursor tracks edits) and convert emoji after.
    QObject::connect(
        this->document(), &QTextDocument::contentsChange, this,
        [this](int position, int /*charsRemoved*/, int charsAdded) {
            if (charsAdded <= 0 || this->convertingInlineEmote_ ||
                !this->inlineEmoteResolver_ || !this->inlineEmotesEnabled())
            {
                return;
            }
            QTextCursor range(this->document());
            range.setPosition(position);
            range.setPosition(
                std::min(position + charsAdded,
                         this->document()->characterCount() - 1),
                QTextCursor::KeepAnchor);
            this->pendingEmojiScans_.push_back(range);
            if (!this->emojiScanQueued_)
            {
                this->emojiScanQueued_ = true;
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        this->scanForEmoji();
                    },
                    Qt::QueuedConnection);
            }
        });

    // Typing after an emote image must not inherit the image format (the
    // typed text would serialize as the emote). Never reset while a
    // selection exists: setCurrentCharFormat would apply the empty format
    // to the selection, stripping any images in it.
    QObject::connect(this, &QTextEdit::currentCharFormatChanged, this,
                     [this](const QTextCharFormat &format) {
                         if (format.hasProperty(INLINE_EMOTE_TEXT) &&
                             !this->textCursor().hasSelection())
                         {
                             this->setCurrentCharFormat(QTextCharFormat());
                         }
                     });

    this->pendingInlineEmoteTimer_.setInterval(100);
    QObject::connect(&this->pendingInlineEmoteTimer_, &QTimer::timeout, this,
                     [this] {
                         this->checkPendingInlineEmotes();
                     });

    // The chat's GIF timer also carries the "animate emotes" settings
    this->managedConnections_.managedConnect(
        getApp()->getEmotes()->getGIFTimer()->signal, [this] {
            this->updateAnimatedInlineImages();
        });
}

void ResizingTextEdit::setInlineEmoteResolver(InlineEmoteResolver resolver)
{
    this->inlineEmoteResolver_ = std::move(resolver);
}

bool ResizingTextEdit::inlineEmotesEnabled() const
{
    return getSettings()->inlineEmotesInInput;
}

QString ResizingTextEdit::serializedText() const
{
    return this->serializeRange(0, this->document()->characterCount() - 1);
}

QString ResizingTextEdit::serializeRange(int start, int end) const
{
    const auto *doc = this->document();
    QString out;
    // set after an emote name, which needs a space to tokenize again
    bool needSep = false;
    bool appendedBlock = false;

    auto appendText = [&](QString text) {
        // never leak stray object-replacement chars into a message
        text.remove(QChar(QChar::ObjectReplacementCharacter));
        if (text.isEmpty())
        {
            return;
        }
        if (needSep && !text.startsWith(' '))
        {
            out += ' ';
        }
        out += text;
        needSep = false;
    };

    for (auto block = doc->begin(); block != doc->end(); block = block.next())
    {
        auto blockStart = block.position();
        auto blockEnd = blockStart + block.length();
        if (blockEnd <= start || blockStart >= end)
        {
            continue;
        }

        if (appendedBlock)
        {
            out += '\n';
            needSep = false;
        }
        appendedBlock = true;

        for (auto it = block.begin(); !it.atEnd(); ++it)
        {
            auto fragment = it.fragment();
            if (!fragment.isValid())
            {
                continue;
            }
            auto fragStart = fragment.position();
            auto from = std::max(fragStart, start);
            auto to = std::min(fragStart + fragment.length(), end);
            if (from >= to)
            {
                continue;
            }

            auto format = fragment.charFormat();
            auto text = fragment.text().mid(from - fragStart, to - from);
            if (!format.hasProperty(INLINE_EMOTE_TEXT))
            {
                appendText(text);
                continue;
            }

            auto emoteText = format.property(INLINE_EMOTE_TEXT).toString();
            bool isEmoji = format.boolProperty(INLINE_EMOTE_IS_EMOJI);
            for (auto ch : text)
            {
                if (ch != QChar::ObjectReplacementCharacter)
                {
                    appendText(ch);
                    continue;
                }
                if (isEmoji)
                {
                    appendText(emoteText);
                }
                else
                {
                    if (!out.isEmpty() && !out.endsWith(' '))
                    {
                        out += ' ';
                    }
                    out += emoteText;
                    needSep = true;
                }
            }
        }
    }

    return out;
}

void ResizingTextEdit::tryConvertWordBeforeCursor()
{
    if (!this->inlineEmoteResolver_ || !this->inlineEmotesEnabled())
    {
        return;
    }

    auto cursor = this->textCursor();
    if (cursor.hasSelection())
    {
        return;
    }

    const auto *doc = this->document();
    int end = cursor.position();
    while (end > 0 && doc->characterAt(end - 1) == ' ')
    {
        end--;
    }
    int start = end;
    while (start > 0)
    {
        QChar ch = doc->characterAt(start - 1);
        if (ch.isSpace() || ch == QChar::ObjectReplacementCharacter)
        {
            break;
        }
        start--;
    }
    if (start >= end)
    {
        return;
    }

    QTextCursor word(this->document());
    word.setPosition(start);
    word.setPosition(end, QTextCursor::KeepAnchor);

    auto resolved = this->inlineEmoteResolver_(word.selectedText());
    if (!resolved)
    {
        return;
    }
    this->insertInlineEmote(word, *resolved);
}

void ResizingTextEdit::convertInlineEmotesInRange(int start, int end)
{
    if (!this->inlineEmoteResolver_ || !this->inlineEmotesEnabled())
    {
        return;
    }

    const auto *doc = this->document();
    auto isBoundary = [](QChar ch) {
        return ch.isSpace() || ch == QChar::ObjectReplacementCharacter;
    };

    // expand so a paste joining with existing text still sees whole words
    while (start > 0 && !isBoundary(doc->characterAt(start - 1)))
    {
        start--;
    }
    int docEnd = doc->characterCount() - 1;
    end = std::min(end, docEnd);
    while (end < docEnd && !isBoundary(doc->characterAt(end)))
    {
        end++;
    }

    // back-to-front: a conversion shrinks the word to one character, which
    // would shift the positions of any words after it
    int wordEnd = end;
    while (wordEnd > start)
    {
        while (wordEnd > start && isBoundary(doc->characterAt(wordEnd - 1)))
        {
            wordEnd--;
        }
        int wordStart = wordEnd;
        while (wordStart > start && !isBoundary(doc->characterAt(wordStart - 1)))
        {
            wordStart--;
        }
        if (wordStart < wordEnd)
        {
            QTextCursor word(this->document());
            word.setPosition(wordStart);
            word.setPosition(wordEnd, QTextCursor::KeepAnchor);
            if (auto resolved = this->inlineEmoteResolver_(word.selectedText()))
            {
                this->insertInlineEmote(word, *resolved);
            }
        }
        wordEnd = wordStart;
    }
}

void ResizingTextEdit::insertInlineEmote(QTextCursor cursor,
                                         const InlineEmote &emote)
{
    if (!emote.emote || !cursor.hasSelection())
    {
        return;
    }
    if (!this->tryInsertLoadedInlineEmote(cursor, emote))
    {
        this->pendingInlineEmotes_.push_back({
            .cursor = cursor,
            .originalText = cursor.selectedText(),
            .emote = emote,
            .attemptsLeft = INLINE_EMOTE_LOAD_ATTEMPTS,
        });
        this->pendingInlineEmoteTimer_.start();
    }
}

bool ResizingTextEdit::tryInsertLoadedInlineEmote(QTextCursor &cursor,
                                                  const InlineEmote &emote)
{
    auto dpr = this->devicePixelRatioF();
    const auto &image =
        emote.emote->images.getImageOrLoaded(static_cast<float>(dpr));
    if (!image || image->isEmpty())
    {
        // Nothing to load; leave the text as-is
        return true;
    }

    auto pixmap = image->pixmapOrLoad();
    if (!pixmap)
    {
        return false;
    }
    if (pixmap->isNull())
    {
        return true;
    }

    QFontMetricsF metrics(this->font());
    qreal targetHeight = metrics.height();
    if (!emote.isEmoji)
    {
        targetHeight *= 1.25;
    }

    QPixmap scaled = pixmap->scaledToHeight(
        std::max(1, qRound(targetHeight * dpr)), Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);

    QString resource = u"c7-inline-emote:%1:%2"_s.arg(
        QString::number(scaled.height()), image->url().string);
    this->document()->addResource(QTextDocument::ImageResource,
                                  QUrl(resource), scaled);

    if (image->animated())
    {
        this->animatedInlineImages_[resource] = {
            .image = image,
            .deviceHeight = scaled.height(),
        };
    }

    this->inlineEmoteTooltips_[resource] = {
        .image = emote.emote->images.getImage(3.0),
        .text = emote.emote->tooltip.string,
    };

    auto logicalSize = scaled.deviceIndependentSize();
    QTextImageFormat format;
    format.setName(resource);
    format.setWidth(logicalSize.width());
    format.setHeight(logicalSize.height());
    // AlignBaseline: image bottom on the text's descent line, excess height
    // above the line, none below (AlignMiddle would push half the image
    // under the baseline). The format font is what that descent is
    // computed from.
    format.setFont(this->font());
    format.setVerticalAlignment(QTextCharFormat::AlignBaseline);
    format.setToolTip(emote.text);
    format.setProperty(INLINE_EMOTE_TEXT, emote.text);
    format.setProperty(INLINE_EMOTE_IS_EMOJI, emote.isEmoji);

    this->convertingInlineEmote_ = true;
    cursor.insertImage(format);
    this->convertingInlineEmote_ = false;

    // don't let typing inherit the image format; never with a selection
    // active (setCurrentCharFormat would reformat it)
    if (this->currentCharFormat().hasProperty(INLINE_EMOTE_TEXT) &&
        !this->textCursor().hasSelection())
    {
        this->setCurrentCharFormat(QTextCharFormat());
    }
    return true;
}

void ResizingTextEdit::cancelPendingInlineEmotes(int start, int end)
{
    std::erase_if(this->pendingInlineEmotes_, [&](const auto &pending) {
        return pending.cursor.selectionStart() < end &&
               pending.cursor.selectionEnd() > start;
    });
}

void ResizingTextEdit::beginInlineEmotePreview(int wordStart, int wordEnd)
{
    this->previewStart_ = QTextCursor(this->document());
    this->previewStart_.setPosition(wordStart);
    this->previewStart_.setKeepPositionOnInsert(true);

    this->previewEnd_ = QTextCursor(this->document());
    this->previewEnd_.setPosition(wordEnd);

    this->previewActive_ = true;
}

void ResizingTextEdit::updateInlineEmotePreview(const InlineEmote &emote)
{
    if (!this->previewActive_)
    {
        return;
    }
    auto start = this->previewStart_.position();
    auto end = this->previewEnd_.position();
    if (start >= end)
    {
        return;
    }

    // a queued load of the previous preview must not fire after the swap
    this->cancelPendingInlineEmotes(start, end);

    QTextCursor selection(this->document());
    selection.setPosition(start);
    selection.setPosition(end, QTextCursor::KeepAnchor);
    this->insertInlineEmote(selection, emote);

    auto caret = this->textCursor();
    caret.setPosition(this->previewEnd_.position());
    this->setTextCursor(caret);
}

void ResizingTextEdit::finishInlineEmotePreview(
    const std::optional<QString> &restoreText, bool addSpace)
{
    if (!this->previewActive_)
    {
        return;
    }
    this->previewActive_ = false;

    auto start = this->previewStart_.position();
    auto end = this->previewEnd_.position();

    QTextCursor cursor(this->document());
    if (restoreText && start < end)
    {
        this->cancelPendingInlineEmotes(start, end);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        this->convertingInlineEmote_ = true;
        cursor.insertText(*restoreText, QTextCharFormat());
        this->convertingInlineEmote_ = false;
        this->setTextCursor(cursor);
    }
    else if (!this->textCursor().hasSelection())
    {
        // keeping the preview must not clobber a selection the user made
        // meanwhile (e.g. Ctrl+A) - typing then replaces the selection
        cursor.setPosition(end);
        if (addSpace)
        {
            cursor.insertText(QStringLiteral(" "), QTextCharFormat());
        }
        this->setTextCursor(cursor);
    }

    this->previewStart_ = QTextCursor();
    this->previewEnd_ = QTextCursor();
}

void ResizingTextEdit::checkPendingInlineEmotes()
{
    auto pending = std::move(this->pendingInlineEmotes_);
    this->pendingInlineEmotes_.clear();

    for (auto &p : pending)
    {
        if (p.cursor.isNull() || !p.cursor.hasSelection() ||
            p.cursor.selectedText() != p.originalText)
        {
            // edited in the meantime
            continue;
        }
        if (!this->tryInsertLoadedInlineEmote(p.cursor, p.emote))
        {
            p.attemptsLeft--;
            if (p.attemptsLeft > 0)
            {
                this->pendingInlineEmotes_.push_back(p);
            }
        }
    }

    if (this->pendingInlineEmotes_.empty())
    {
        this->pendingInlineEmoteTimer_.stop();
    }
}

bool ResizingTextEdit::tryUnwrapInlineEmote()
{
    auto cursor = this->textCursor();
    if (cursor.hasSelection() || cursor.position() == 0)
    {
        return false;
    }

    // charFormat() is the format of the character before the cursor
    auto format = cursor.charFormat();
    if (!format.hasProperty(INLINE_EMOTE_TEXT) ||
        format.boolProperty(INLINE_EMOTE_IS_EMOJI))
    {
        // Emoji images delete like a regular character
        return false;
    }

    cursor.movePosition(QTextCursor::PreviousCharacter,
                        QTextCursor::KeepAnchor);
    if (cursor.selectedText() !=
        QString(QChar(QChar::ObjectReplacementCharacter)))
    {
        return false;
    }

    this->convertingInlineEmote_ = true;
    cursor.insertText(format.property(INLINE_EMOTE_TEXT).toString(),
                      QTextCharFormat());
    this->convertingInlineEmote_ = false;
    this->setTextCursor(cursor);
    return true;
}

void ResizingTextEdit::updateAnimatedInlineImages()
{
    if (this->animatedInlineImages_.empty())
    {
        return;
    }

    // Emotes not currently in the document are skipped, not erased: undo
    // can bring them back and they must resume animating.
    std::set<QString> inUse;
    for (auto block = this->document()->begin();
         block != this->document()->end(); block = block.next())
    {
        for (auto it = block.begin(); !it.atEnd(); ++it)
        {
            auto fragment = it.fragment();
            if (!fragment.isValid())
            {
                continue;
            }
            auto format = fragment.charFormat();
            if (format.isImageFormat() &&
                format.hasProperty(INLINE_EMOTE_TEXT))
            {
                inUse.insert(format.toImageFormat().name());
            }
        }
    }

    auto dpr = this->devicePixelRatioF();
    bool updatedAny = false;
    for (const auto &[resource, info] : this->animatedInlineImages_)
    {
        if (!inUse.contains(resource))
        {
            continue;
        }

        auto pixmap = info.image->pixmapOrLoad();
        if (pixmap && !pixmap->isNull())
        {
            QPixmap scaled = pixmap->scaledToHeight(info.deviceHeight,
                                                    Qt::SmoothTransformation);
            scaled.setDevicePixelRatio(dpr);
            this->document()->addResource(QTextDocument::ImageResource,
                                          QUrl(resource), scaled);
            updatedAny = true;
        }
    }

    if (updatedAny)
    {
        this->viewport()->update();
    }
}

void ResizingTextEdit::scanForEmoji()
{
    this->emojiScanQueued_ = false;
    auto ranges = std::move(this->pendingEmojiScans_);
    this->pendingEmojiScans_.clear();

    const auto *emojis = getApp()->getEmotes()->getEmojis();

    for (auto &range : ranges)
    {
        if (range.isNull() || !range.hasSelection())
        {
            continue;
        }

        // Widen to word boundaries so emoji arriving in multiple input
        // events (e.g. surrogate halves, ZWJ sequences) still match.
        const auto *doc = this->document();
        int start = range.selectionStart();
        int end = range.selectionEnd();
        while (start > 0)
        {
            QChar ch = doc->characterAt(start - 1);
            if (ch.isSpace() || ch == QChar::ObjectReplacementCharacter)
            {
                break;
            }
            start--;
        }
        int docEnd = doc->characterCount() - 1;
        while (end < docEnd)
        {
            QChar ch = doc->characterAt(end);
            if (ch.isSpace() || ch == QChar::ObjectReplacementCharacter)
            {
                break;
            }
            end++;
        }
        range.setPosition(start);
        range.setPosition(end, QTextCursor::KeepAnchor);

        QString text = range.selectedText();
        auto parts = emojis->parse(text);
        bool hasEmoji = std::ranges::any_of(parts, [](const auto &part) {
            return std::holds_alternative<EmotePtr>(part);
        });
        if (!hasEmoji)
        {
            continue;
        }

        auto caretPos = this->textCursor().position();
        bool caretInRange = caretPos >= start && caretPos <= end;
        int caretOffset = caretPos - start;

        // Rebuild the range; emoji become glyph placeholders first so a
        // pending image load has a real selection to track.
        std::vector<std::pair<QTextCursor, InlineEmote>> conversions;

        this->convertingInlineEmote_ = true;
        range.beginEditBlock();
        range.removeSelectedText();
        for (const auto &part : parts)
        {
            if (const auto *textPart = std::get_if<QStringView>(&part))
            {
                range.insertText(textPart->toString(), QTextCharFormat());
                continue;
            }
            const auto &emote = std::get<EmotePtr>(part);
            const auto &glyph = emote->name.string;
            int glyphStart = range.position();
            range.insertText(glyph, QTextCharFormat());

            QTextCursor selection(this->document());
            selection.setPosition(glyphStart);
            selection.setPosition(glyphStart + glyph.size(),
                                  QTextCursor::KeepAnchor);
            conversions.emplace_back(
                selection, InlineEmote{.text = glyph,
                                       .emote = emote,
                                       .isEmoji = true});
        }
        range.endEditBlock();
        this->convertingInlineEmote_ = false;

        if (caretInRange)
        {
            // the rebuilt range holds (nearly) the same text; keep the offset
            auto caret = this->textCursor();
            caret.setPosition(std::min(start + caretOffset, range.position()));
            this->setTextCursor(caret);
        }

        for (auto &[selection, emote] : conversions)
        {
            this->insertInlineEmote(selection, emote);
        }
    }
}

QSize ResizingTextEdit::sizeHint() const
{
    return QSize(this->width(), this->heightForWidth(this->width()));
}

bool ResizingTextEdit::hasHeightForWidth() const
{
    return true;
}

bool ResizingTextEdit::isFirstWord() const
{
    QString plainText = this->toPlainText();
    QString portionBeforeCursor = plainText.left(this->textCursor().position());
    return !portionBeforeCursor.contains(' ');
};

int ResizingTextEdit::heightForWidth(int) const
{
    auto margins = this->contentsMargins();

    return margins.top() + this->document()->size().height() +
           margins.bottom() + 5;
}

QString ResizingTextEdit::textUnderCursor(bool *hadSpace) const
{
    auto currentText = this->toPlainText();
    // Inline emote images act as word boundaries
    currentText.replace(QChar(QChar::ObjectReplacementCharacter), QChar(' '));

    QTextCursor tc = this->textCursor();

    auto textUpToCursor = currentText.left(tc.selectionStart());

    auto words = QStringView{textUpToCursor}.split(' ');
    if (words.size() == 0)
    {
        return QString();
    }

    bool first = true;
    QString lastWord;
    for (auto it = words.crbegin(); it != words.crend(); ++it)
    {
        auto word = *it;

        if (first && word.isEmpty())
        {
            first = false;
            if (hadSpace != nullptr)
            {
                *hadSpace = true;
            }
            continue;
        }

        lastWord = word.toString();
        break;
    }

    if (lastWord.isEmpty())
    {
        return QString();
    }

    return lastWord;
}

bool ResizingTextEdit::eventFilter(QObject *obj, QEvent *event)
{
    (void)obj;  // unused

    // makes QShortcuts work in the ResizingTextEdit
    if (event->type() != QEvent::ShortcutOverride)
    {
        return false;
    }
    auto *ev = static_cast<QKeyEvent *>(event);
    ev->ignore();
    if ((ev->key() == Qt::Key_C || ev->key() == Qt::Key_Insert) &&
        ev->modifiers() == Qt::ControlModifier)
    {
        return false;
    }
    return true;
}
void ResizingTextEdit::keyPressEvent(QKeyEvent *event)
{
    event->ignore();

    this->keyPressed.invoke(event);

    bool doComplete =
        (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) &&
        (event->modifiers() & Qt::ControlModifier) == Qt::NoModifier &&
        !event->isAccepted();

    if (doComplete)
    {
        // check if there is a completer
        if (!this->completer_)
        {
            return;
        }

        QString currentCompletion = this->textUnderCursor();

        // check if there is something to complete
        if (currentCompletion.size() <= 1)
        {
            return;
        }

        // always expected to be TabCompletionModel
        auto *completionModel =
            dynamic_cast<TabCompletionModel *>(this->completer_->model());
        assert(completionModel != nullptr);

        if (!this->completionInProgress_)
        {
            // First type pressing tab after modifying a message, we refresh our
            // completion model
            this->completer_->setModel(completionModel);
            completionModel->updateResults(
                currentCompletion, this->toPlainText(),
                this->textCursor().position(), this->isFirstWord());
            this->completionInProgress_ = true;
            {
                // this blocks cursor movement events from resetting tab completion
                QSignalBlocker dontTriggerCursorMovement(this);
                this->completer_->complete();
            }
            this->textChanged();
            return;
        }

        // scrolling through selections
        if (event->key() == Qt::Key_Tab)
        {
            if (!this->completer_->setCurrentRow(
                    this->completer_->currentRow() + 1))
            {
                // wrap over and start again
                this->completer_->setCurrentRow(0);
            }
        }
        else
        {
            if (!this->completer_->setCurrentRow(
                    this->completer_->currentRow() - 1))
            {
                // wrap over and start again
                this->completer_->setCurrentRow(
                    this->completer_->completionCount() - 1);
            }
        }

        {
            // this blocks cursor movement events from updating tab completion
            QSignalBlocker dontTriggerCursorMovement(this);
            this->completer_->complete();
        }
        this->textChanged();
        return;
    }

    if (!event->text().isEmpty())
    {
        // converting during Tab-cycling would break the cycling
        if (this->completionInProgress_ && event->text().at(0).isPrint() &&
            event->key() != Qt::Key_Space)
        {
            this->tryConvertWordBeforeCursor();
        }
        this->completionInProgress_ = false;
    }

    if (!event->isAccepted())
    {
        if (this->inlineEmoteResolver_ && this->inlineEmotesEnabled())
        {
            if (event->key() == Qt::Key_Backspace &&
                event->modifiers() == Qt::NoModifier &&
                this->tryUnwrapInlineEmote())
            {
                event->accept();
                return;
            }

            // before the space is inserted
            if (event->key() == Qt::Key_Space)
            {
                this->tryConvertWordBeforeCursor();
            }
        }

        QTextEdit::keyPressEvent(event);

        // Closing the second colon of ":name:" converts instantly
        if (event->text().contains(':') && this->inlineEmoteResolver_ &&
            this->inlineEmotesEnabled())
        {
            this->tryConvertWordBeforeCursor();
        }
    }
}

void ResizingTextEdit::focusInEvent(QFocusEvent *event)
{
    QTextEdit::focusInEvent(event);

    if (event->gotFocus())
    {
        this->focused.invoke();
    }
}

void ResizingTextEdit::focusOutEvent(QFocusEvent *event)
{
    QTextEdit::focusOutEvent(event);

    if (event->lostFocus())
    {
        this->focusLost.invoke();
    }
}

void ResizingTextEdit::setCompleter(QCompleter *c)
{
    delete this->completer_;

    this->completer_ = c;

    if (!this->completer_)
    {
        return;
    }

    this->completer_->setWidget(this);
    this->completer_->setCompletionMode(QCompleter::InlineCompletion);
    this->completer_->setCaseSensitivity(Qt::CaseInsensitive);

    QObject::connect(this->completer_,
                     static_cast<void (QCompleter::*)(const QString &)>(
                         &QCompleter::highlighted),
                     this, &ResizingTextEdit::insertCompletion);
}

void ResizingTextEdit::resetCompletion()
{
    this->completionInProgress_ = false;
}

void ResizingTextEdit::insertCompletion(const QString &completion)
{
    if (this->completer_->widget() != this)
    {
        return;
    }

    bool hadSpace = false;
    auto prefix = this->textUnderCursor(&hadSpace);

    int prefixSize = prefix.size();

    if (hadSpace)
    {
        ++prefixSize;
    }

    QTextCursor tc = this->textCursor();
    int completionStart = tc.position() - prefixSize;
    tc.setPosition(completionStart, QTextCursor::KeepAnchor);
    tc.insertText(completion);
    this->setTextCursor(tc);
    this->updateGeometry();
}

bool ResizingTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    if (source->hasImage() || source->hasFormat("text/plain"))
    {
        return true;
    }
    return QTextEdit::canInsertFromMimeData(source);
}

void ResizingTextEdit::insertFromMimeData(const QMimeData *source)
{
    if (getSettings()->imageUploaderEnabled)
    {
        if (source->hasImage())
        {
            this->imagePasted.invoke(source);
            return;
        }

        if (source->hasUrls())
        {
            bool hasUploadable = false;
            auto mimeDb = QMimeDatabase();
            for (const QUrl &url : source->urls())
            {
                QMimeType mime = mimeDb.mimeTypeForUrl(url);
                if (mime.name().startsWith("image"))
                {
                    hasUploadable = true;
                    break;
                }
            }

            if (hasUploadable)
            {
                this->imagePasted.invoke(source);
                return;
            }
        }
    }

    // a paste replaces any selection; convert emote names in just its range
    int pasteStart = this->textCursor().selectionStart();
    insertPlainText(source->text());
    this->convertInlineEmotesInRange(pasteStart, this->textCursor().position());
}

void ResizingTextEdit::mouseMoveEvent(QMouseEvent *event)
{
    QTextEdit::mouseMoveEvent(event);

    QString resource;
    auto layoutPos = event->position() +
                     QPointF(this->horizontalScrollBar()->value(),
                             this->verticalScrollBar()->value());
    auto hit = this->document()->documentLayout()->hitTest(layoutPos,
                                                           Qt::ExactHit);
    if (hit >= 0 &&
        this->document()->characterAt(hit) ==
            QChar(QChar::ObjectReplacementCharacter))
    {
        QTextCursor cursor(this->document());
        cursor.setPosition(hit + 1);
        auto format = cursor.charFormat();
        if (format.isImageFormat() && format.hasProperty(INLINE_EMOTE_TEXT))
        {
            resource = format.toImageFormat().name();
        }
    }

    if (resource.isEmpty())
    {
        if (!this->hoveredInlineEmote_.isEmpty())
        {
            this->hoveredInlineEmote_.clear();
            this->inlineEmoteHoverEnded.invoke();
        }
        return;
    }

    auto it = this->inlineEmoteTooltips_.find(resource);
    if (it == this->inlineEmoteTooltips_.end())
    {
        return;
    }
    this->hoveredInlineEmote_ = resource;
    // every move, so the tooltip follows the cursor
    this->inlineEmoteHovered.invoke(it->second,
                                    event->globalPosition().toPoint());
}

void ResizingTextEdit::leaveEvent(QEvent *event)
{
    QTextEdit::leaveEvent(event);
    if (!this->hoveredInlineEmote_.isEmpty())
    {
        this->hoveredInlineEmote_.clear();
        this->inlineEmoteHoverEnded.invoke();
    }
}

QMimeData *ResizingTextEdit::createMimeDataFromSelection() const
{
    auto cursor = this->textCursor();
    if (!cursor.hasSelection())
    {
        return QTextEdit::createMimeDataFromSelection();
    }

    auto *mime = new QMimeData;
    mime->setText(
        this->serializeRange(cursor.selectionStart(), cursor.selectionEnd()));
    return mime;
}

void ResizingTextEdit::contextMenuEvent(QContextMenuEvent *event)
{
    QObjectPtr<QMenu> menu{this->createStandardContextMenu(event->pos())};
    this->contextMenuRequested.invoke(menu.get(), event->pos());
    menu->exec(event->globalPos());
}

}  // namespace chatterino
