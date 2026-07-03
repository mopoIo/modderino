#pragma once

#include "controllers/completion/sources/EmoteSource.hpp"
#include "widgets/BasePopup.hpp"

#include <QTimer>

#include <functional>
#include <utility>
#include <vector>

namespace chatterino {

/// The Tab-completion emote wheel: a strip of emote matches floating above
/// the input box, with the selection pinned to the center slot and
/// wrap-around scrolling. SplitInput drives it from the keyboard while a
/// live preview of the selection sits inline in the text; the arrows and
/// emote slots are also clickable.
class TabEmoteWheel : public BasePopup
{
public:
    TabEmoteWheel(QWidget *parent = nullptr);

    void setMatches(std::vector<completion::EmoteItem> matches);
    void setSelected(int index);
    int selected() const;
    const std::vector<completion::EmoteItem> &matches() const;

    /// Invoked with -1/+1 when an arrow is clicked.
    std::function<void(int)> onNavigate;
    /// Invoked with the match index when an emote slot is clicked.
    std::function<void(int)> onSelect;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct WheelLayout {
        qreal pad = 0;
        qreal arrowWidth = 0;
        qreal slotSize = 0;
        qreal labelHeight = 0;
        QRectF leftArrow;
        QRectF rightArrow;
        /// Visible slots: rect and the index into matches_ it shows.
        std::vector<std::pair<QRectF, int>> slots;
    };
    WheelLayout computeLayout() const;
    int visibleSlotCount() const;
    void updateSize();

    std::vector<completion::EmoteItem> matches_;
    int selected_ = 0;
    /// Repaints while visible so loading/animated emotes stay fresh.
    QTimer redrawTimer_;
};

}  // namespace chatterino
