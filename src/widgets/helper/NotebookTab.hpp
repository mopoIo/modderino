// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Common.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Notebook.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QMenu>
#include <QPropertyAnimation>

namespace chatterino {

inline constexpr int NOTEBOOK_TAB_HEIGHT = 28;

class SplitContainer;
class Channel;
using ChannelPtr = std::shared_ptr<Channel>;

class NotebookTab : public Button
{
    Q_OBJECT

public:
    explicit NotebookTab(Notebook *notebook);

    void updateSize();

    QWidget *page{};

    void setCustomTitle(const QString &title);
    void resetCustomTitle();
    bool hasCustomTitle() const;
    const QString &getCustomTitle() const;
    void setDefaultTitle(const QString &title);
    const QString &getDefaultTitle() const;
    const QString &getTitle() const;

    bool isSelected() const;
    void setSelected(bool value);

    void setInLastRow(bool value);
    void setTabLocation(NotebookTabLocation location);

    /**
     * @brief Sets the live status of this tab
     *
     * Returns true if the live status was changed, false if nothing changed.
     **/
    bool setLive(bool isLive);

    /**
     * @brief Sets the rerun status of this tab
     *
     * Returns true if the rerun status was changed, false if nothing changed.
     **/
    bool setRerun(bool isRerun);

    /**
     * @brief Returns true if any split in this tab is live
     **/
    bool isLive() const;

    /**
     * @brief Sets the highlight state of this tab clearing highlight sources
     *
     * Obeys the HighlightsEnabled setting and highlight states hierarchy
     */
    void setHighlightState(HighlightState style);
    /**
     * @brief Updates the highlight state and highlight sources of this tab
     *
     * Obeys the HighlightsEnabled setting and the highlight state hierarchy and tracks the highlight state update sources
     */
    void updateHighlightState(HighlightState style,
                              const ChannelView &channelViewSource);
    void copyHighlightStateAndSourcesFrom(const NotebookTab *sourceTab);
    void setHighlightsEnabled(const bool &newVal);
    void newHighlightSourceAdded(const ChannelView &channelViewSource);
    bool hasHighlightsEnabled() const;
    HighlightState highlightState() const;

    void moveAnimated(QPoint targetPos, bool animated = true);

    QRect getDesiredRect() const;
    void tabSizeChanged();

    void growWidth(int width);
    int normalTabWidth() const;

protected:
    void themeChangedEvent() override;

    void paintEvent(QPaintEvent *) override;
    void paintContent(QPainter &painter) override
    {
    }

    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

    /// This exists as an alias to its base classes update, and is virtual
    /// to allow for mocking
    virtual void update();

private:
    void showRenameDialog();

    /// The channel used as emote context for this tab's title (its selected
    /// split's channel, or an empty channel). Used for rendering emote codes in
    /// the title and for the rename dialog's emote input.
    ChannelPtr channelForEmotes() const;

    bool hasXButton() const;
    bool shouldDrawXButton() const;
    QRect getXRect() const;
    void titleUpdated();

    int normalTabWidthForHeight(int height) const;

    bool shouldMessageHighlight(const ChannelView &channelViewSource) const;

    using HighlightSources =
        std::unordered_map<ChannelView::ChannelViewID, HighlightState>;
    HighlightSources highlightSources_;

    void removeHighlightStateChangeSources(const HighlightSources &toRemove);
    void removeHighlightSource(const ChannelView::ChannelViewID &source);
    void updateHighlightStateDueSourcesChange();

    void recreateCloseMultipleTabsMenu(NotebookTabLocation tabLocation);

    QPropertyAnimation positionChangedAnimation_;
    QPoint positionAnimationDesiredPoint_;

    Notebook *notebook_;

    QString customTitle_;
    QString defaultTitle_;

    /// Remaining bounded repaint retries while the title's emoji images load.
    int emojiRepaintsRemaining_ = 40;

    /// Whether the current title contains an animated emote/emoji image;
    /// updated on paint, drives GIF-timer repaints
    bool titleAnimated_ = false;

    /// Whether title images were still loading during the last paint
    bool titleImagesPending_ = false;

    bool selected_{};
    bool mouseOver_{};
    bool mouseDown_{};
    bool mouseOverX_{};
    bool mouseDownX_{};
    bool isInLastRow_{};
    int mouseWheelDelta_ = 0;
    NotebookTabLocation tabLocation_ = NotebookTabLocation::Top;

    HighlightState highlightState_ = HighlightState::None;
    bool highlightEnabled_ = true;
    QAction *highlightNewMessagesAction_;

    bool isLive_{};
    bool isRerun_{};

    int growWidth_ = 0;

    QMenu menu_;
    QMenu *closeMultipleTabsMenu_{};
    QAction *closeTabsBeforeSelectedAction_{};
    QAction *closeTabsAfterSelectedAction_{};

    pajlada::Signals::SignalHolder managedConnections_;
};

}  // namespace chatterino
