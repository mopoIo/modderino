// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QPointF>
#include <QSize>

class QScreen;
class QWidget;

namespace chatterino {

class Settings;

/// "Keep the same size on every monitor": gives each monitor a scale factor
/// of its resolution (its shorter side) over the smallest monitor's, so Qt
/// lays windows out once and renders them bigger on bigger monitors - a
/// window takes up the same share of every monitor.
///
/// Windows only. Call once the screens are known and before any window is
/// created. Qt's factors can't change under open windows without glitching,
/// so the setting takes effect on the next start.
void applyConsistentMonitorSize(const Settings &settings);

/// Whether monitors got their own scales for this run
bool monitorScalingActive();

/// Quits normally (saving settings and layout) and starts Modderino again
/// with the same arguments once this instance has exited
void restartModderino();

/// The factor currently applied to @a screen, 1 when the setting is off
qreal monitorScaleFactor(const QScreen *screen);

#ifdef Q_OS_WIN

/// Qt changes a window's scale when it moves to a monitor with a different
/// factor but keeps its size in pixels, so the window would lay out in fewer
/// (or more) logical pixels and wrap differently. This resizes it to keep its
/// logical size, so its layout is identical on every monitor.
///
/// A resize that would push the window's larger part back onto the monitor
/// it came from waits until the move settles, so it can't flip back and
/// forth at the border. While dragging, the grabbed point stays under the
/// cursor as the window changes size.
class MonitorSizeKeeper : public QObject
{
public:
    /// @pre @a window is shown (it has a native window)
    explicit MonitorSizeKeeper(QWidget *window);

    /// Takes part in the move loop. Returns true when @a message was
    /// handled and @a result set.
    bool nativeEvent(void *message, qintptr *result);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void tryRestoreSize();

    QWidget *window_;
    /// The factor the window's current size in pixels was made for
    qreal factor_ = 1;
    /// A restore is waiting for the window to move further onto its monitor
    bool pending_ = false;
    bool inMoveLoop_ = false;
    /// Where the window was grabbed, as a fraction of its size
    QPointF grab_{0.5, 0.5};
};

#endif

}  // namespace chatterino
