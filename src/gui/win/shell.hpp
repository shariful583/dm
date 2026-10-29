// SPDX-License-Identifier: GPL-3.0-or-later
// Windows shell integration for the GUI (window/taskbar bits live in gui/).
#pragma once

#include <memory>
#include <vector>

#include <QObject>
#include <QString>
#include <QWindow>

namespace lanes::gui::win {

// Notification-area icon (the exe's icon resource IDI_ICON1) with a native context menu and balloon
// notifications, which Windows 10/11 show as toasts. Re-adds itself when Explorer restarts.
// GUI thread only (its hidden window receives the shell's messages through Qt's event loop).
class TrayIcon : public QObject {
  Q_OBJECT

public:
  struct MenuItem {
    int id = 0; // 0 with empty text = separator
    QString text;
    bool checked = false;
    bool enabled = true;
    std::vector<MenuItem> children; // non-empty = submenu
  };

  explicit TrayIcon(const QString &tooltip, QObject *parent = nullptr);
  ~TrayIcon() override;

  void setTooltip(const QString &tooltip);
  // The menu shown on right-click; rebuilt by the caller when checks change.
  void setMenu(std::vector<MenuItem> items);
  // Balloon / toast; `tag` comes back with messageClicked.
  void showMessage(const QString &title, const QString &text, qint64 tag);

signals:
  void activated(); // left click or keyboard selection
  void menuTriggered(int id);
  void messageClicked(qint64 tag);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Taskbar button progress (ITaskbarList3). GUI thread only.
class TaskbarProgress {
public:
  TaskbarProgress();
  ~TaskbarProgress();
  TaskbarProgress(const TaskbarProgress &) = delete;
  TaskbarProgress &operator=(const TaskbarProgress &) = delete;

  // progress 0-1, or < 0 for no progress shown. Re-sent periodically by the caller is fine.
  void set(QWindow *window, double progress, bool paused);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Dark/light title bar following `dark`; with `mica`, the Windows 11 Mica backdrop behind the
// whole window (needs a transparent window background). Returns true if Mica is in use (Windows
// 11 22H2+). Call again when the color scheme changes.
bool style_window(QWindow *window, bool dark, bool mica);

// Opens Explorer at the file's folder with the file selected (the folder if the file is gone).
void show_in_folder(const QString &path);
// Opens the file with its associated program. False if Windows could not start it.
bool open_with_default_app(const QString &path);

// Lets the process that owns the engine bring its window to the front (called by a second
// launch before forwarding its arguments), and brings `window` up from the tray / minimized.
void allow_foreground_handoff();
void bring_to_front(QWindow *window);

// Native message box for errors before any window exists.
void error_box(const QString &title, const QString &text);

} // namespace lanes::gui::win
