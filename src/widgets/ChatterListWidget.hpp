// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;

namespace chatterino {

class TwitchChannel;

class ChatterListWidget : public BaseWindow
{
    Q_OBJECT

public:
    ChatterListWidget(const TwitchChannel *twitchChannel, QWidget *parent);

    Q_SIGNAL void userClicked(QString userLogin);

protected:
    void scaleChangedEvent(float newScale) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // kept to re-apply the font when the zoom changes
    QLineEdit *searchBar_ = nullptr;
    QLabel *loadingLabel_ = nullptr;
    QListWidget *chattersList_ = nullptr;
    QListWidget *resultList_ = nullptr;
};

}  // namespace chatterino
