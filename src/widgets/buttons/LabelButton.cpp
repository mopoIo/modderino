// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/LabelButton.hpp"

namespace chatterino {

LabelButton::LabelButton(const QString &text, BaseWidget *parent, QSize padding)
    : Button(parent)
    , layout_(this)
    , label_(text)
    , fullText_(text)
    , padding_(padding)
{
    this->layout_.setContentsMargins(0, 0, 0, 0);
    this->layout_.addWidget(&this->label_);
    this->label_.setAttribute(Qt::WA_TransparentForMouseEvents);
    this->label_.setAlignment(Qt::AlignCenter);

    this->updatePadding();
}

void LabelButton::setText(const QString &text)
{
    this->fullText_ = text;
    if (this->elide_)
    {
        this->applyElidedText();
        this->updateGeometry();
    }
    else
    {
        this->label_.setText(text);
    }
}

QString LabelButton::text() const
{
    return this->fullText_;
}

QSize LabelButton::padding() const noexcept
{
    return this->padding_;
}

void LabelButton::setPadding(QSize padding)
{
    if (this->padding_ == padding)
    {
        return;
    }

    this->padding_ = padding;
    this->updatePadding();
}

void LabelButton::enableRichText()
{
    this->label_.setTextFormat(Qt::RichText);
}

void LabelButton::setLabelAlignment(Qt::Alignment alignment)
{
    this->label_.setAlignment(alignment);
}

void LabelButton::setElide(bool elide)
{
    if (this->elide_ == elide)
    {
        return;
    }

    this->elide_ = elide;
    if (elide)
    {
        this->applyElidedText();
    }
    else
    {
        this->label_.setText(this->fullText_);
    }
    this->updateGeometry();
}

QSize LabelButton::sizeHint() const
{
    auto hint = Button::sizeHint();
    if (this->elide_)
    {
        // The label may currently show an elided text; ask for enough room
        // for the full text so we grow back when space becomes available.
        auto textWidth = static_cast<int>(
            this->deviceMetrics().horizontalAdvance(this->fullText_));
        hint.setWidth(textWidth + 2 * this->padding_.width() + 4);
    }
    return hint;
}

QFontMetricsF LabelButton::deviceMetrics() const
{
    // Measure against the label as a paint device; plain QFontMetrics can
    // report a different (primary-screen) DPI than the one the text is
    // actually rendered at, which under-measures and clips the text.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return QFontMetricsF(this->label_.font(),
                         const_cast<QLabel *>(&this->label_));
}

void LabelButton::resizeEvent(QResizeEvent *event)
{
    Button::resizeEvent(event);
    if (this->elide_)
    {
        this->applyElidedText();
    }
}

void LabelButton::changeEvent(QEvent *event)
{
    Button::changeEvent(event);
    if (this->elide_ && event->type() == QEvent::FontChange)
    {
        this->applyElidedText();
        this->updateGeometry();
    }
}

void LabelButton::applyElidedText()
{
    auto available = this->width() - 2 * this->padding_.width();
    this->label_.setText(this->deviceMetrics().elidedText(
        this->fullText_, Qt::ElideRight, available));
}

void LabelButton::paintContent(QPainter &painter)
{
}

void LabelButton::updatePadding()
{
    auto x = this->padding_.width();
    auto y = this->padding_.height();
    this->label_.setContentsMargins(x, y, x, y);
}

}  // namespace chatterino
