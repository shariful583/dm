// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

namespace lanes::core::platform {

// Console helpers for the command-line front-end. Not thread-safe unless noted.

// True if standard error is an interactive console (not redirected to a file or pipe).
bool stderr_is_terminal();

// Visible width of the console in characters; 80 if unknown.
int terminal_columns();

// Makes the console interpret output as UTF-8 (Windows; no-op elsewhere). Call once at startup.
void use_utf8_console();

// Calls `handler` on Ctrl+C / Ctrl+Break instead of terminating the process. The handler runs on
// a separate thread and must be thread-safe. Calling again replaces the handler.
void on_console_interrupt(std::function<void()> handler);

} // namespace lanes::core::platform
