// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QPointer>
#include <QWidget>

#include <memory>

class QLineEdit;
class QCompleter;

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;

class SvgButton;
class InputCompletionPopup;
class EmotePopup;

/// A single-line text input with the same emote-entry helpers as the chat
/// input: an emote-picker button, the `:`-triggered emote/emoji completion
/// popup, and Tab auto-completion. All completion is done in the context of the
/// given channel (its emotes + the account's emotes + globals + emojis).
class EmoteInputLineEdit : public QWidget
{
    Q_OBJECT

public:
    explicit EmoteInputLineEdit(ChannelPtr channel, QWidget *parent = nullptr);

    QString text() const;
    void setText(const QString &text);
    void setPlaceholderText(const QString &text);
    void selectAll();

    /// The underlying line edit (e.g. to connect to returnPressed).
    QLineEdit *lineEdit() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // ':' completion popup
    void updateCompletionPopup();
    void showCompletionPopup(const QString &text);
    void hideCompletionPopup();
    void insertCompletionText(const QString &text);

    // emote picker button
    void openEmotePopup();
    void positionEmoteButton();

    // Tab completion
    bool handleTabCompletion(bool forward);
    void insertTabCompletion(const QString &completion);
    QString wordUnderCursor(int *startOut = nullptr) const;
    bool isFirstWord() const;

    ChannelPtr channel_;
    QLineEdit *lineEdit_ = nullptr;
    SvgButton *emoteButton_ = nullptr;
    QCompleter *completer_ = nullptr;
    QPointer<InputCompletionPopup> completionPopup_;
    QPointer<EmotePopup> emotePopup_;
    bool completionInProgress_ = false;
};

}  // namespace chatterino
