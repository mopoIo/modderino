// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/Button.hpp"

#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>

namespace chatterino {

/// @brief A button with a label on top.
///
/// The content can be set with #setText(), #setPixmap(), and #setMovie().
class LabelButton : public Button
{
public:
    LabelButton(const QString &text = {}, BaseWidget *parent = nullptr,
                QSize padding = {6, 0});

    /// @brief Returns the current text
    ///
    /// If no text has been set this will return an empty string.
    [[nodiscard]] QString text() const;

    /// @brief Sets the label's text to @a text.
    ///
    /// Any previous content is cleared.
    /// Supports Qt's HTML subset.
    void setText(const QString &text);

    /// @brief Returns the padding inside the button.
    ///
    /// `width` is the padding applied horizontally (left and right).
    /// `height` is the padding applied vertically (top and bottom).
    ///
    /// By default, the button has no vertical padding and a horizontal padding of 6.
    [[nodiscard]] QSize padding() const noexcept;

    /// Setter for #padding()
    void setPadding(QSize padding);

    /// Sets the label to display rich text (Qt's HTML subset)
    void enableRichText();

    /// @brief Sets the label's alignment.
    ///
    /// By default the label is centered. Left-aligned labels clip on the
    /// right when there isn't enough room, instead of on both sides.
    void setLabelAlignment(Qt::Alignment alignment);

    /// @brief Elide the text with "…" when there isn't enough room.
    ///
    /// Not supported together with rich text.
    void setElide(bool elide);

    QSize sizeHint() const override;

protected:
    void paintContent(QPainter &painter) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updatePadding();
    void applyElidedText();
    QFontMetricsF deviceMetrics() const;

    QHBoxLayout layout_;
    QLabel label_;
    QString fullText_;
    bool elide_ = false;
    QSize padding_;
};

}  // namespace chatterino
