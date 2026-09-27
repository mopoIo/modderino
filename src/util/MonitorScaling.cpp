// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/MonitorScaling.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QHash>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#if defined(Q_OS_WIN) && __has_include(<private/qhighdpiscaling_p.h>)
// Private, but Windows builds ship the exact Qt they're built against
#    include <private/qhighdpiscaling_p.h>
#    define MODDERINO_MONITOR_SCALING
#endif

#ifdef Q_OS_WIN
#    include <Windows.h>
#endif

#include <algorithm>
#include <cmath>

namespace {

using namespace chatterino;
using namespace Qt::Literals;

bool active = false;

#ifdef MODDERINO_MONITOR_SCALING

/// The factors given to each screen, by name, to measure them without them
QHash<QString, qreal> appliedFactors;

/// The screen's shorter side in logical pixels, before our factor: its
/// height on a landscape screen, its width on a portrait one. Logical, so
/// Windows' own display scaling is already accounted for.
qreal baseSize(const QScreen *screen)
{
    const auto size = screen->geometry().size();
    return std::min(size.width(), size.height()) *
           appliedFactors.value(screen->name(), 1.0);
}

/// Gives every screen a factor of its size over the smallest one's, so a
/// window takes up the same share of any screen
void applyFactors()
{
    const auto screens = QGuiApplication::screens();

    qreal reference = 0;
    for (const auto *screen : screens)
    {
        const auto size = baseSize(screen);
        if (size > 0)
        {
            reference = reference == 0 ? size : std::min(reference, size);
        }
    }
    if (reference == 0)
    {
        return;
    }

    QHash<QString, qreal> factors;
    for (const auto *screen : screens)
    {
        factors.insert(screen->name(),
                       std::clamp(baseSize(screen) / reference, 1.0, 4.0));
    }
    for (auto *screen : screens)
    {
        QHighDpiScaling::setScreenFactor(screen, factors.value(screen->name()));
    }
    appliedFactors = factors;
    active = true;
}

#endif

#ifdef Q_OS_WIN

/// Whether Windows arranged the window (Snap), and so decides its size.
/// IsWindowArranged exists since Windows 10 1903; loaded dynamically.
bool isArranged(HWND hwnd)
{
    using IsWindowArrangedFn = BOOL(WINAPI *)(HWND);
    static const auto isWindowArranged = [] {
        auto *user32 = GetModuleHandleW(L"user32.dll");
        return user32 == nullptr
                   ? nullptr
                   : reinterpret_cast<IsWindowArrangedFn>(
                         GetProcAddress(user32, "IsWindowArranged"));
    }();
    return isWindowArranged != nullptr && isWindowArranged(hwnd) != FALSE;
}

#endif

}  // namespace

namespace chatterino {

void applyConsistentMonitorSize(const Settings &settings)
{
#ifdef MODDERINO_MONITOR_SCALING
    if (settings.consistentMonitorSize)
    {
        applyFactors();
    }

    // a monitor coming or going can change which one is the least dense
    QObject::connect(qApp, &QGuiApplication::screenAdded, qApp, [] {
        if (active)
        {
            applyFactors();
        }
    });
    QObject::connect(qApp, &QGuiApplication::screenRemoved, qApp, [] {
        if (active)
        {
            applyFactors();
        }
    });
#else
    (void)settings;
#endif
}

bool monitorScalingActive()
{
    return active;
}

void restartModderino()
{
#ifdef Q_OS_WIN
    // PowerShell literal strings: single quotes, doubled inside
    auto quote = [](QString text) {
        return u'\'' + text.replace(u'\'', u"''"_s) + u'\'';
    };

    QStringList arguments = QCoreApplication::arguments();
    arguments.removeFirst();  // the executable
    QString command = u"Wait-Process -Id %1 -ErrorAction SilentlyContinue; "
                      "Start-Process -FilePath %2 -WorkingDirectory %3"_s.arg(
                          QString::number(QCoreApplication::applicationPid()),
                          quote(QCoreApplication::applicationFilePath()),
                          quote(QDir::currentPath()));
    if (!arguments.isEmpty())
    {
        QStringList quoted;
        for (const auto &argument : arguments)
        {
            quoted.append(quote(argument));
        }
        command += u" -ArgumentList @(%1)"_s.arg(quoted.join(u", "_s));
    }

    // Starts this again once it has exited, so the new one reads settings
    // and layout this one saves while quitting
    QProcess helper;
    helper.setProgram(u"powershell.exe"_s);
    helper.setArguments({u"-NoProfile"_s, u"-NonInteractive"_s,
                         u"-WindowStyle"_s, u"Hidden"_s, u"-Command"_s,
                         command});
    helper.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
    if (!helper.startDetached())
    {
        return;
    }
#endif
    // the regular quit saves settings and window layout
    QCoreApplication::quit();
}

qreal monitorScaleFactor(const QScreen *screen)
{
#ifdef MODDERINO_MONITOR_SCALING
    if (active && screen != nullptr)
    {
        return appliedFactors.value(screen->name(), 1.0);
    }
#else
    (void)screen;
#endif
    return 1.0;
}

#ifdef Q_OS_WIN

MonitorSizeKeeper::MonitorSizeKeeper(QWidget *window)
    : QObject(window)
    , window_(window)
    , factor_(monitorScaleFactor(window->screen()))
{
    window->installEventFilter(this);
    QObject::connect(window->windowHandle(), &QWindow::screenChanged, this,
                     [this](QScreen * /*screen*/) {
                         this->tryRestoreSize();
                     });
}

bool MonitorSizeKeeper::eventFilter(QObject * /*watched*/, QEvent *event)
{
    if (event->type() == QEvent::DevicePixelRatioChange)
    {
        // Emote images are picked for the scale when laid out; lay out again
        // so they're sharp on this monitor. Queued, as the scale settles in
        // the same pass.
        QTimer::singleShot(0, qApp, [] {
            if (auto *app = tryGetApp())
            {
                app->getWindows()->forceLayoutChannelViews();
            }
        });
    }

    // the scale changed (the monitor changed), or a restore that had to wait
    // tries again as the window keeps moving
    if (event->type() == QEvent::DevicePixelRatioChange ||
        ((event->type() == QEvent::Move || event->type() == QEvent::Resize) &&
         this->pending_))
    {
        this->tryRestoreSize();
    }
    return false;
}

void MonitorSizeKeeper::tryRestoreSize()
{
    // Qt keeps the window's size in pixels when our factor changes (and
    // doesn't update its logical size until the next resize), so scale the
    // pixels by the change in factor: the logical size stays what it was.
    // Windows' own scaling isn't included - Qt resizes for that itself.
    const auto factor = monitorScaleFactor(this->window_->screen());
    if (qFuzzyCompare(factor, this->factor_))
    {
        this->pending_ = false;
        return;
    }
    auto *hwnd = reinterpret_cast<HWND>(this->window_->winId());
    if (this->window_->isMaximized() || this->window_->isFullScreen() ||
        isArranged(hwnd))
    {
        // those take the size Windows gives them (maximized, snapped)
        this->factor_ = factor;
        this->pending_ = false;
        return;
    }

    // Scale only the client area: the frame around it (the invisible resize
    // border) is a fixed number of pixels, and scaling it too would make the
    // window creep smaller or bigger with every move between monitors
    RECT rect;
    RECT client;
    GetWindowRect(hwnd, &rect);
    GetClientRect(hwnd, &client);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int frameWidth = width - client.right;
    const int frameHeight = height - client.bottom;
    const auto scale = factor / this->factor_;
    const int newWidth =
        static_cast<int>(std::lround(client.right * scale)) + frameWidth;
    const int newHeight =
        static_cast<int>(std::lround(client.bottom * scale)) + frameHeight;

    // grow or shrink around the grabbed point while dragging, around the
    // middle otherwise
    QPointF fraction(0.5, 0.5);
    QPointF anchor(rect.left + width / 2.0, rect.top + height / 2.0);
    if (this->inMoveLoop_)
    {
        POINT cursor;
        GetCursorPos(&cursor);
        fraction = this->grab_;
        anchor = {double(cursor.x), double(cursor.y)};
    }
    RECT next;
    next.left =
        static_cast<LONG>(std::lround(anchor.x() - fraction.x() * newWidth));
    next.top =
        static_cast<LONG>(std::lround(anchor.y() - fraction.y() * newHeight));
    next.right = next.left + newWidth;
    next.bottom = next.top + newHeight;

    // Resized, would most of it be back on the monitor it came from? Then
    // wait for the move to go further, or it'd switch back and forth.
    if (MonitorFromRect(&next, MONITOR_DEFAULTTONEAREST) !=
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST))
    {
        this->pending_ = true;
        return;
    }

    this->pending_ = false;
    this->factor_ = factor;
    SetWindowPos(hwnd, nullptr, next.left, next.top, newWidth, newHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

bool MonitorSizeKeeper::nativeEvent(void *message, qintptr *result)
{
    const auto *msg = static_cast<MSG *>(message);
    switch (msg->message)
    {
        case WM_ENTERSIZEMOVE: {
            this->inMoveLoop_ = true;
            RECT rect;
            POINT cursor;
            GetWindowRect(msg->hwnd, &rect);
            GetCursorPos(&cursor);
            const double width = std::max(1L, rect.right - rect.left);
            const double height = std::max(1L, rect.bottom - rect.top);
            this->grab_ = {(cursor.x - rect.left) / width,
                           (cursor.y - rect.top) / height};
            return false;
        }

        case WM_EXITSIZEMOVE:
            this->inMoveLoop_ = false;
            if (this->pending_)
            {
                this->tryRestoreSize();
            }
            return false;

        case WM_MOVING: {
            if (!this->inMoveLoop_)
            {
                return false;
            }
            // Windows keeps the offset from the cursor in pixels, which is
            // off once the window changed size; keep the grabbed point under
            // the cursor instead
            // The proposed rect has the size from when the drag started; use
            // the current one, or a resize made during the drag is undone
            auto *rect = reinterpret_cast<RECT *>(msg->lParam);
            RECT current;
            GetWindowRect(msg->hwnd, &current);
            const LONG width = current.right - current.left;
            const LONG height = current.bottom - current.top;
            POINT cursor;
            GetCursorPos(&cursor);
            rect->left = cursor.x - static_cast<LONG>(
                                        std::lround(this->grab_.x() * width));
            rect->top = cursor.y - static_cast<LONG>(
                                       std::lround(this->grab_.y() * height));
            rect->right = rect->left + width;
            rect->bottom = rect->top + height;
            *result = TRUE;
            return true;
        }

        default:
            return false;
    }
}

#endif

}  // namespace chatterino
