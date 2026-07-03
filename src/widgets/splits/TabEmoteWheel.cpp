#include "widgets/splits/TabEmoteWheel.hpp"

#include "Application.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>

namespace chatterino {

namespace {

/// At most this many emote slots are visible; the rest scroll through.
constexpr int MAX_VISIBLE_SLOTS = 7;

}  // namespace

TabEmoteWheel::TabEmoteWheel(QWidget *parent)
    : BasePopup({BasePopup::EnableCustomFrame, BasePopup::Frameless,
                 BasePopup::DontFocus, BaseWindow::DisableLayoutSave},
                parent)
{
    QObject::connect(&this->redrawTimer_, &QTimer::timeout, this, [this] {
        if (this->isVisible())
        {
            this->update();
        }
    });
    this->redrawTimer_.setInterval(33);
}

void TabEmoteWheel::setMatches(std::vector<completion::EmoteItem> matches)
{
    this->matches_ = std::move(matches);
    this->selected_ = 0;
    this->updateSize();
    this->update();
}

void TabEmoteWheel::setSelected(int index)
{
    this->selected_ = index;
    this->update();
}

int TabEmoteWheel::selected() const
{
    return this->selected_;
}

const std::vector<completion::EmoteItem> &TabEmoteWheel::matches() const
{
    return this->matches_;
}

int TabEmoteWheel::visibleSlotCount() const
{
    auto count = static_cast<int>(this->matches_.size());
    if (count <= 1)
    {
        return count;
    }
    // 2 matches show 3 slots, 3 show 5, 4+ fill all 7 - the selection sits
    // in the middle and neighbors repeat around it as it wraps.
    return std::min(MAX_VISIBLE_SLOTS, (count - 1) * 2 + 1);
}

TabEmoteWheel::WheelLayout TabEmoteWheel::computeLayout() const
{
    auto scale = this->scale();

    WheelLayout layout;
    layout.pad = 6 * scale;
    layout.arrowWidth = 16 * scale;
    layout.slotSize = 40 * scale;
    layout.labelHeight = 16 * scale;

    auto count = static_cast<int>(this->matches_.size());
    auto slots = this->visibleSlotCount();
    auto half = slots / 2;

    // the label can make the popup wider than the slot row; center the row
    auto rowWidth = 2 * layout.arrowWidth + slots * layout.slotSize;
    qreal x = std::max(layout.pad, (this->width() - rowWidth) / 2);
    qreal y = layout.pad;
    layout.leftArrow = QRectF(x, y, layout.arrowWidth, layout.slotSize);
    x += layout.arrowWidth;

    for (int i = -half; i <= half; i++)
    {
        auto index = ((this->selected_ + i) % count + count) % count;
        layout.slots.emplace_back(
            QRectF(x, y, layout.slotSize, layout.slotSize), index);
        x += layout.slotSize;
    }

    layout.rightArrow = QRectF(x, y, layout.arrowWidth, layout.slotSize);

    return layout;
}

void TabEmoteWheel::updateSize()
{
    if (this->matches_.empty())
    {
        return;
    }

    auto scale = this->scale();
    auto pad = 6 * scale;
    auto rowWidth = 2 * (16 * scale) +
                    this->visibleSlotCount() * (40 * scale) + 2 * pad;

    // with few slots the label ("name · provider · 1/1") drives the width
    auto font =
        getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall, scale);
    QFontMetricsF metrics(font);
    auto counter = QStringLiteral("%1/%2")
                       .arg(this->matches_.size())
                       .arg(this->matches_.size());
    qreal maxLabelWidth = 0;
    for (const auto &item : this->matches_)
    {
        maxLabelWidth = std::max(
            maxLabelWidth, metrics.horizontalAdvance(item.displayName + " · " +
                                                     item.providerName +
                                                     " · " + counter));
    }
    maxLabelWidth =
        std::min(maxLabelWidth + 4 * pad, static_cast<qreal>(360 * scale));

    auto width = std::max(static_cast<qreal>(rowWidth), maxLabelWidth);
    auto height = (40 * scale) + (16 * scale) + 2 * pad + 2 * scale;
    this->setFixedSize(static_cast<int>(width), static_cast<int>(height));
}

void TabEmoteWheel::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    auto *theme = getTheme();
    auto scale = this->scale();

    QColor borderColor = theme->isLightTheme() ? QColor("#ccc")
                                               : QColor("#333");
    painter.setBrush(theme->splits.input.background);
    painter.setPen(borderColor);
    painter.drawRoundedRect(QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            4 * scale, 4 * scale);

    if (this->matches_.empty())
    {
        return;
    }

    auto layout = this->computeLayout();

    auto drawArrow = [&](const QRectF &rect, bool left) {
        auto cx = rect.center().x();
        auto cy = rect.center().y();
        auto w = 3.5 * scale;
        auto h = 5 * scale;
        QPainterPath path;
        if (left)
        {
            path.moveTo(cx + w / 2, cy - h);
            path.lineTo(cx - w / 2, cy);
            path.lineTo(cx + w / 2, cy + h);
        }
        else
        {
            path.moveTo(cx - w / 2, cy - h);
            path.lineTo(cx + w / 2, cy);
            path.lineTo(cx - w / 2, cy + h);
        }
        path.closeSubpath();
        painter.setPen(Qt::NoPen);
        QColor arrowColor = theme->splits.input.text;
        arrowColor.setAlphaF(0.6);
        painter.setBrush(arrowColor);
        painter.drawPath(path);
    };
    drawArrow(layout.leftArrow, true);
    drawArrow(layout.rightArrow, false);

    auto centerSlot = static_cast<size_t>(layout.slots.size() / 2);
    for (size_t i = 0; i < layout.slots.size(); i++)
    {
        const auto &[rect, matchIndex] = layout.slots[i];
        bool isSelected = i == centerSlot;

        if (isSelected)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme->tabs.selected.backgrounds.regular);
            painter.drawRoundedRect(rect.adjusted(2 * scale, 2 * scale,
                                                  -2 * scale, -2 * scale),
                                    3 * scale, 3 * scale);
        }

        const auto &item =
            this->matches_[static_cast<size_t>(matchIndex)];
        if (!item.emote)
        {
            continue;
        }
        const auto &image = item.emote->images.getImageOrLoaded(scale);
        if (!image || image->isEmpty())
        {
            continue;
        }
        auto pixmap = image->pixmapOrLoad();
        if (!pixmap || pixmap->isNull())
        {
            // still loading; the redraw timer picks it up
            continue;
        }

        auto maxSize = 32 * scale;
        auto pixSize = QSizeF(pixmap->deviceIndependentSize());
        pixSize.scale(maxSize, maxSize, Qt::KeepAspectRatio);
        QRectF target(0, 0, pixSize.width(), pixSize.height());
        target.moveCenter(rect.center());

        painter.setOpacity(isSelected ? 1.0 : 0.5);
        painter.drawPixmap(target, *pixmap, pixmap->rect());
        painter.setOpacity(1.0);
    }

    const auto &selectedItem =
        this->matches_[static_cast<size_t>(this->selected_)];
    auto font =
        getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall, scale);
    painter.setFont(font);
    QFontMetricsF metrics(font);

    QColor textColor = theme->splits.input.text;
    QColor dimmed = textColor;
    dimmed.setAlphaF(0.5);

    const auto separator = QStringLiteral(" · ");
    auto counter = QStringLiteral("%1/%2")
                       .arg(this->selected_ + 1)
                       .arg(this->matches_.size());

    auto available = this->width() - 4 * layout.pad;
    auto fixedWidth = metrics.horizontalAdvance(separator) * 2 +
                      metrics.horizontalAdvance(counter);

    // the provider gets at most half the leftover space
    auto provider =
        metrics.elidedText(selectedItem.providerName, Qt::ElideRight,
                           (available - fixedWidth) / 2);
    auto name = metrics.elidedText(
        selectedItem.displayName, Qt::ElideRight,
        available - fixedWidth - metrics.horizontalAdvance(provider));

    struct Segment {
        QString text;
        QColor color;
    };
    std::array<Segment, 2> segments{{
        {name, textColor},
        {separator + provider + separator + counter, dimmed},
    }};

    qreal labelWidth = 0;
    for (const auto &segment : segments)
    {
        labelWidth += metrics.horizontalAdvance(segment.text);
    }

    auto x = (this->width() - labelWidth) / 2;
    auto labelY = layout.pad + layout.slotSize +
                  (layout.labelHeight + metrics.ascent() -
                   metrics.descent()) /
                      2;

    for (const auto &segment : segments)
    {
        painter.setPen(segment.color);
        painter.drawText(QPointF(x, labelY), segment.text);
        x += metrics.horizontalAdvance(segment.text);
    }
}

void TabEmoteWheel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || this->matches_.empty())
    {
        return;
    }

    auto layout = this->computeLayout();
    auto pos = event->position();

    if (layout.leftArrow.contains(pos))
    {
        if (this->onNavigate)
        {
            this->onNavigate(-1);
        }
        return;
    }
    if (layout.rightArrow.contains(pos))
    {
        if (this->onNavigate)
        {
            this->onNavigate(1);
        }
        return;
    }
    for (const auto &[rect, matchIndex] : layout.slots)
    {
        if (rect.contains(pos))
        {
            if (this->onSelect)
            {
                this->onSelect(matchIndex);
            }
            return;
        }
    }
}

void TabEmoteWheel::showEvent(QShowEvent * /*event*/)
{
    this->redrawTimer_.start();
}

void TabEmoteWheel::hideEvent(QHideEvent * /*event*/)
{
    this->redrawTimer_.stop();
}

}  // namespace chatterino
