<img width="365" height="224" alt="image" src="https://github.com/user-attachments/assets/aacca568-d586-4d3b-93d5-7b545732af7e" />

## Shutdown Timer

Run `ShutdownTimer.exe` on Windows to schedule or cancel a shutdown. The native Win32
application is a standalone executable and does not require the .NET runtime.
It includes a clock-and-power icon in the window and executable.
The app shows the estimated local shutdown time and lets you replace an active timer
using the **Reschedule shutdown** button.
Active schedules are remembered in `%APPDATA%\Shutdown Timer` so reopening the app
restores the countdown and reschedule action. Windows shutdown timers do not survive
a Windows restart.

The C++ source is in `cpp-win32/main.cpp`.
