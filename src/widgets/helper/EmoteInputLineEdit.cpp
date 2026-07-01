// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/EmoteInputLineEdit.hpp"

#include "common/Channel.hpp"
#include "controllers/completion/CompletionModel.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "messages/Link.hpp"
#include "singletons/Settings.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/EmotePopup.hpp"
#include "widgets/splits/InputCompletionPopup.hpp"

#include <QCompleter>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>

#include <algorithm>
#include <tuple>

namespace chatterino {

EmoteInputLineEdit::EmoteInputLineEdit(ChannelPtr channel, QWidget *parent)
    : QWidget(parent)
    , channel_(std::move(channel))
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    this->lineEdit_ = new QLineEdit(this);
    // A little extra height so the overlaid emote button has room at the
    // bottom-right, like the chat input.
    this->lineEdit_->setMinimumHeight(30);
    layout->addWidget(this->lineEdit_, 1);

    // The emote button is overlaid inside the line edit at the bottom-right,
    // like the emote button in the chat "send message" box. The emote SVG is
    // square, so the button is square too (with small, even padding).
    this->emoteButton_ = new SvgButton(
        {
            .dark = ":/buttons/emote.svg",
            .light = ":/buttons/emoteDark.svg",
        },
        nullptr, QSize{3, 3});
    this->emoteButton_->setParent(this->lineEdit_);
    this->emoteButton_->setFixedSize(20, 20);
    this->emoteButton_->raise();
    // Reserve room on the right so typed text doesn't run under the button.
    this->lineEdit_->setTextMargins(0, 0, this->emoteButton_->width() + 6, 0);

    // Tab completion via the channel's completion model (same as chat).
    if (this->channel_ && this->channel_->completionModel)
    {
        this->completer_ =
            new QCompleter(this->channel_->completionModel, this);
        this->completer_->setWidget(this->lineEdit_);
        this->completer_->setCompletionMode(QCompleter::InlineCompletion);
        this->completer_->setCaseSensitivity(Qt::CaseInsensitive);
        QObject::connect(
            this->completer_,
            QOverload<const QString &>::of(&QCompleter::highlighted), this,
            [this](const QString &completion) {
                this->insertTabCompletion(completion);
            });
    }

    QObject::connect(this->lineEdit_, &QLineEdit::textEdited, this, [this] {
        this->completionInProgress_ = false;
        this->updateCompletionPopup();
    });
    QObject::connect(this->lineEdit_, &QLineEdit::cursorPositionChanged, this,
                     [this] {
                         this->updateCompletionPopup();
                     });
    QObject::connect(this->emoteButton_, &Button::leftClicked, this, [this] {
        this->openEmotePopup();
    });

    this->lineEdit_->installEventFilter(this);
    this->setFocusProxy(this->lineEdit_);
}

QString EmoteInputLineEdit::text() const
{
    return this->lineEdit_->text();
}

void EmoteInputLineEdit::setText(const QString &text)
{
    this->lineEdit_->setText(text);
}

void EmoteInputLineEdit::setPlaceholderText(const QString &text)
{
    this->lineEdit_->setPlaceholderText(text);
}

void EmoteInputLineEdit::selectAll()
{
    this->lineEdit_->selectAll();
}

QLineEdit *EmoteInputLineEdit::lineEdit() const
{
    return this->lineEdit_;
}

bool EmoteInputLineEdit::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->lineEdit_ && event->type() == QEvent::Resize)
    {
        this->positionEmoteButton();
    }

    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);

        // While the completion popup is open, let it navigate/select.
        if (auto *popup = this->completionPopup_.data())
        {
            if (popup->isVisible() && popup->eventFilter(nullptr, event))
            {
                return true;
            }
        }

        if ((keyEvent->key() == Qt::Key_Tab ||
             keyEvent->key() == Qt::Key_Backtab) &&
            (keyEvent->modifiers() & Qt::ControlModifier) == Qt::NoModifier)
        {
            if (this->handleTabCompletion(keyEvent->key() == Qt::Key_Tab))
            {
                return true;
            }
        }
        else if (!keyEvent->text().isEmpty())
        {
            this->completionInProgress_ = false;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void EmoteInputLineEdit::updateCompletionPopup()
{
    if (!getSettings()->emoteCompletionWithColon)
    {
        this->hideCompletionPopup();
        return;
    }

    auto text = this->lineEdit_->text();
    int position = this->lineEdit_->cursorPosition() - 1;

    if (text.isEmpty() || position == -1)
    {
        this->hideCompletionPopup();
        return;
    }

    for (int i = std::clamp(position, 0, static_cast<int>(text.length()) - 1);
         i >= 0; i--)
    {
        if (text[i] == ' ')
        {
            this->hideCompletionPopup();
            return;
        }

        if (text[i] == ':')
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1));
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }
    }

    this->hideCompletionPopup();
}

void EmoteInputLineEdit::showCompletionPopup(const QString &text)
{
    if (this->completionPopup_.isNull())
    {
        this->completionPopup_ = new InputCompletionPopup(this);
        this->completionPopup_->setInputAction(
            [this](const QString &chosen) mutable {
                this->insertCompletionText(chosen);
                this->hideCompletionPopup();
            });
    }

    auto *popup = this->completionPopup_.data();
    popup->updateCompletion(text, CompletionKind::Emote, this->channel_);

    auto pos =
        this->lineEdit_->mapToGlobal(QPoint(0, this->lineEdit_->height()));
    popup->move(pos);
    popup->show();
}

void EmoteInputLineEdit::hideCompletionPopup()
{
    if (auto *popup = this->completionPopup_.data())
    {
        popup->hide();
    }
}

void EmoteInputLineEdit::insertCompletionText(const QString &input_)
{
    auto input = input_ + ' ';
    auto text = this->lineEdit_->text();
    int position = this->lineEdit_->cursorPosition() - 1;

    for (int i = std::clamp(position, 0, static_cast<int>(text.length()) - 1);
         i >= 0; i--)
    {
        if (text[i] == ':')
        {
            text.remove(i, position - i + 1);
            text.insert(i, input);

            QSignalBlocker block(this->lineEdit_);
            this->lineEdit_->setText(text);
            this->lineEdit_->setCursorPosition(i + input.size());
            break;
        }
    }
}

void EmoteInputLineEdit::positionEmoteButton()
{
    if (this->emoteButton_ == nullptr)
    {
        return;
    }

    const int margin = 3;
    int x = this->lineEdit_->width() - this->emoteButton_->width() - margin;
    int y = this->lineEdit_->height() - this->emoteButton_->height() - margin;
    this->emoteButton_->move(x, y);
}

void EmoteInputLineEdit::openEmotePopup()
{
    if (!this->emotePopup_)
    {
        this->emotePopup_ = new EmotePopup(this);
        this->emotePopup_->setAttribute(Qt::WA_DeleteOnClose);

        std::ignore =
            this->emotePopup_->linkClicked.connect([this](const Link &link) {
                if (link.type != Link::InsertText)
                {
                    return;
                }

                QString textToInsert(link.value + " ");
                int pos = this->lineEdit_->cursorPosition();
                auto text = this->lineEdit_->text();
                if (pos > 0 && !text[pos - 1].isSpace())
                {
                    textToInsert = " " + textToInsert;
                }
                this->lineEdit_->insert(textToInsert);
                this->lineEdit_->setFocus();
            });
    }

    this->emotePopup_->loadChannel(this->channel_);
    this->emotePopup_->show();
    this->emotePopup_->raise();
    this->emotePopup_->activateWindow();
}

bool EmoteInputLineEdit::handleTabCompletion(bool forward)
{
    if (this->completer_ == nullptr || !this->channel_ ||
        this->channel_->completionModel == nullptr)
    {
        return false;
    }

    QString current = this->wordUnderCursor();
    if (current.size() <= 1)
    {
        return false;
    }

    if (!this->completionInProgress_)
    {
        this->completer_->setModel(this->channel_->completionModel);
        this->channel_->completionModel->updateResults(
            current, this->lineEdit_->text(),
            this->lineEdit_->cursorPosition(), this->isFirstWord());
        this->completionInProgress_ = true;
        this->completer_->complete();
        return true;
    }

    if (forward)
    {
        if (!this->completer_->setCurrentRow(this->completer_->currentRow() + 1))
        {
            this->completer_->setCurrentRow(0);
        }
    }
    else
    {
        if (!this->completer_->setCurrentRow(this->completer_->currentRow() - 1))
        {
            this->completer_->setCurrentRow(
                this->completer_->completionCount() - 1);
        }
    }

    this->completer_->complete();
    return true;
}

void EmoteInputLineEdit::insertTabCompletion(const QString &completion)
{
    int start = 0;
    this->wordUnderCursor(&start);
    int position = this->lineEdit_->cursorPosition();

    auto text = this->lineEdit_->text();
    text.remove(start, position - start);
    text.insert(start, completion);

    QSignalBlocker block(this->lineEdit_);
    this->lineEdit_->setText(text);
    this->lineEdit_->setCursorPosition(start + completion.size());
}

QString EmoteInputLineEdit::wordUnderCursor(int *startOut) const
{
    auto text = this->lineEdit_->text();
    int position = this->lineEdit_->cursorPosition();
    int start = position;
    while (start > 0 && !text[start - 1].isSpace())
    {
        start--;
    }
    if (startOut != nullptr)
    {
        *startOut = start;
    }
    return text.mid(start, position - start);
}

bool EmoteInputLineEdit::isFirstWord() const
{
    auto text = this->lineEdit_->text();
    int start = 0;
    this->wordUnderCursor(&start);
    for (int i = 0; i < start && i < text.length(); i++)
    {
        if (!text[i].isSpace())
        {
            return false;
        }
    }
    return true;
}

}  // namespace chatterino
