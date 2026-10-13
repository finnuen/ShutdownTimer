#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <cwchar>
#include <cstring>

#define ID_DAYS 101
#define ID_HOURS 102
#define ID_MINUTES 103
#define ID_SECONDS 104
#define ID_DAYS_EDIT 201
#define ID_HOURS_EDIT 202
#define ID_MINUTES_EDIT 203
#define ID_SECONDS_EDIT 204
#define ID_SCHEDULE 105
#define ID_CANCEL 106
#define ID_STATUS 107
#define ID_CLEAR_ALL 108
#define ID_COUNTDOWN 1
#define IDI_SHUTDOWN_TIMER 1

static HWND g_timeEdits[4];
static HWND g_timeSpinners[4];
static HWND g_scheduleButton;
static HWND g_shutdownAt;
static HWND g_status;
static HFONT g_font;
static ULONGLONG g_scheduleStartedAt = 0;
static ULONGLONG g_deadline = 0;
static bool g_scheduled = false;
static int g_wheelRemainders[4] = {};
static wchar_t g_shutdownAtText[160] = L"Shutdown at: —";
static const DWORD g_maxShutdownSeconds = 315360000;
static const int g_timeMaximums[4] = {
    static_cast<int>(g_maxShutdownSeconds / 86400),
    static_cast<int>(g_maxShutdownSeconds / 3600),
    static_cast<int>(g_maxShutdownSeconds / 60),
    static_cast<int>(g_maxShutdownSeconds)
};
static const int g_timeInputLengths[4] = { 4, 5, 7, 9 };
static const DWORD g_scheduleStateMagic = 0x53544844;

struct ScheduleState
{
    DWORD magic;
    DWORD version;
    ULONGLONG bootTime;
    ULONGLONG startedAt;
    ULONGLONG deadline;
};

enum class ScheduleStateResult
{
    Missing,
    Loaded,
    Invalid,
    Error
};

static bool GetSystemBootTime(ULONGLONG* bootTime);

static bool GetScheduleStatePath(wchar_t* path, size_t pathLength)
{
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
        path) != S_OK ||
        wcscat_s(path, pathLength, L"\\Shutdown Timer") != 0)
        return false;

    if (!CreateDirectoryW(path, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        wcscat_s(path, pathLength, L"\\schedule.dat") != 0)
        return false;

    return true;
}

static bool SaveScheduleState(ULONGLONG startedAt, ULONGLONG deadline)
{
    wchar_t path[MAX_PATH] = {};
    if (!GetScheduleStatePath(path, _countof(path)))
        return false;

    wchar_t temporaryPath[MAX_PATH];
    if (wcscpy_s(temporaryPath, path) != 0 ||
        wcscat_s(temporaryPath, L".tmp") != 0)
        return false;

    HANDLE file = CreateFileW(temporaryPath, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    ULONGLONG bootTime;
    if (!GetSystemBootTime(&bootTime))
    {
        CloseHandle(file);
        DeleteFileW(temporaryPath);
        return false;
    }

    ScheduleState state = { g_scheduleStateMagic, 2, bootTime, startedAt, deadline };
    DWORD bytesWritten = 0;
    bool written = WriteFile(file, &state, sizeof(state), &bytesWritten, nullptr) &&
        bytesWritten == sizeof(state) && FlushFileBuffers(file);
    CloseHandle(file);

    if (!written || !MoveFileExW(temporaryPath, path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporaryPath);
        return false;
    }

    return true;
}

static bool DeleteScheduleState()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetScheduleStatePath(path, _countof(path)))
        return false;

    if (DeleteFileW(path))
        return true;

    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

static bool GetSystemBootTime(ULONGLONG* bootTime)
{
    FILETIME currentTime;
    GetSystemTimeAsFileTime(&currentTime);
    ULARGE_INTEGER current = {};
    current.LowPart = currentTime.dwLowDateTime;
    current.HighPart = currentTime.dwHighDateTime;

    ULONGLONG uptime = GetTickCount64() * 10000;
    if (current.QuadPart < uptime)
        return false;

    *bootTime = current.QuadPart - uptime;
    return true;
}

static ScheduleStateResult LoadScheduleState(ULONGLONG now, ScheduleState* state)
{
    wchar_t path[MAX_PATH] = {};
    if (!GetScheduleStatePath(path, _countof(path)))
        return ScheduleStateResult::Error;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return GetLastError() == ERROR_FILE_NOT_FOUND
            ? ScheduleStateResult::Missing
            : ScheduleStateResult::Error;
    }

    DWORD bytesRead = 0;
    ScheduleState loaded = {};
    bool read = ReadFile(file, &loaded, sizeof(loaded), &bytesRead, nullptr) &&
        bytesRead == sizeof(loaded);
    LARGE_INTEGER fileSize = {};
    bool sized = GetFileSizeEx(file, &fileSize) &&
        fileSize.QuadPart == sizeof(loaded);
    CloseHandle(file);

    ULONGLONG currentBootTime;
    if (!GetSystemBootTime(&currentBootTime))
        return ScheduleStateResult::Error;

    ULONGLONG bootTimeDifference = loaded.bootTime > currentBootTime
        ? loaded.bootTime - currentBootTime
        : currentBootTime - loaded.bootTime;
    if (!read || !sized || loaded.magic != g_scheduleStateMagic ||
        loaded.version != 2 || bootTimeDifference > 120ULL * 10000000 ||
        loaded.startedAt > now ||
        loaded.deadline <= now || loaded.deadline <= loaded.startedAt ||
        loaded.deadline - loaded.startedAt >
            static_cast<ULONGLONG>(g_maxShutdownSeconds) * 1000)
        return ScheduleStateResult::Invalid;

    *state = loaded;
    return ScheduleStateResult::Loaded;
}

static bool RunShutdownCommand(const wchar_t* arguments, DWORD* exitCode)
{
    wchar_t systemDirectory[MAX_PATH];
    UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    wchar_t application[MAX_PATH];
    if (wcscpy_s(application, systemDirectory) != 0 ||
        wcscat_s(application, L"\\shutdown.exe") != 0)
        return false;

    wchar_t commandLine[MAX_PATH + 64];
    wsprintfW(commandLine, L"\"%s\" %s", application, arguments);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(application, commandLine, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;

    DWORD waitResult = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    BOOL gotExitCode = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!gotExitCode)
        return false;

    *exitCode = result;
    return true;
}

static void SetStatus(const wchar_t* text)
{
    SetWindowTextW(g_status, text);
}

static bool ReadTimeValue(HWND edit, int maximum, int* value)
{
    wchar_t text[16];
    int length = GetWindowTextW(edit, text, static_cast<int>(_countof(text)));
    if (length == 0 || length >= static_cast<int>(_countof(text)))
        return false;

    wchar_t* end = nullptr;
    long currentValue = std::wcstol(text, &end, 10);
    if (end == text || *end != L'\0' || currentValue < 0 || currentValue > maximum)
        return false;

    *value = static_cast<int>(currentValue);
    return true;
}

static void SetTimeValue(int index, int value)
{
    wchar_t text[16];
    wsprintfW(text, L"%d", value);
    SetWindowTextW(g_timeEdits[index], text);
    SendMessageW(g_timeSpinners[index], UDM_SETPOS32, 0, value);
}

static void AdjustTimeValue(int index, int delta)
{
    int value;
    if (!ReadTimeValue(g_timeEdits[index], g_timeMaximums[index], &value))
        return;

    value += delta;
    if (value < 0)
        value = 0;
    if (value > g_timeMaximums[index])
        value = g_timeMaximums[index];
    SetTimeValue(index, value);
}

static void ClearAllTimeValues()
{
    for (int i = 0; i < 4; ++i)
        SetTimeValue(i, 0);
}

static bool FormatShutdownTime(DWORD seconds, wchar_t* text)
{
    FILETIME currentTime;
    GetSystemTimeAsFileTime(&currentTime);
    ULARGE_INTEGER shutdownTime = {};
    shutdownTime.LowPart = currentTime.dwLowDateTime;
    shutdownTime.HighPart = currentTime.dwHighDateTime;
    shutdownTime.QuadPart += static_cast<ULONGLONG>(seconds) * 10000000;

    FILETIME shutdownFileTime = {};
    shutdownFileTime.dwLowDateTime = shutdownTime.LowPart;
    shutdownFileTime.dwHighDateTime = shutdownTime.HighPart;
    SYSTEMTIME utcTime;
    SYSTEMTIME localTime;
    if (!FileTimeToSystemTime(&shutdownFileTime, &utcTime) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &localTime))
        return false;

    wchar_t formattedDate[64];
    wchar_t formattedTime[64];
    if (!GetDateFormatW(LOCALE_USER_DEFAULT, 0, &localTime,
        L"dddd, d MMMM, yyyy", formattedDate,
        static_cast<int>(_countof(formattedDate))) ||
        !GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &localTime,
            L"HH':'mm':'ss", formattedTime,
            static_cast<int>(_countof(formattedTime))))
        return false;

    wsprintfW(text, L"Shutdown at: %s, %s", formattedDate, formattedTime);
    return true;
}

static void SetScheduleButtonText()
{
    SetWindowTextW(g_scheduleButton,
        g_scheduled ? L"Reschedule shutdown" : L"Schedule shutdown");
}

static DWORD GetRemainingSeconds(ULONGLONG deadline, ULONGLONG now)
{
    return static_cast<DWORD>((deadline - now + 999) / 1000);
}

static void FormatRemainingDuration(DWORD seconds, wchar_t* text)
{
    DWORD days = seconds / 86400;
    DWORD hours = (seconds / 3600) % 24;
    DWORD minutes = (seconds / 60) % 60;
    DWORD remainingSeconds = seconds % 60;
    wsprintfW(text, L"Shutdown in: %lu day%s, %lu hour%s, %lu minute%s, %lu second%s",
        days, days == 1 ? L"" : L"s",
        hours, hours == 1 ? L"" : L"s",
        minutes, minutes == 1 ? L"" : L"s",
        remainingSeconds, remainingSeconds == 1 ? L"" : L"s");
}

static void SetScheduledUi(HWND window, DWORD remaining, const wchar_t* statusText)
{
    g_scheduled = true;
    SetTimer(window, ID_COUNTDOWN, 1000, nullptr);
    SetScheduleButtonText();

    SetTimeValue(0, static_cast<int>(remaining / 86400));
    SetTimeValue(1, static_cast<int>((remaining / 3600) % 24));
    SetTimeValue(2, static_cast<int>((remaining / 60) % 60));
    SetTimeValue(3, static_cast<int>(remaining % 60));

    if (FormatShutdownTime(remaining, g_shutdownAtText))
        SetWindowTextW(g_shutdownAt, g_shutdownAtText);
    else
    {
        wcscpy_s(g_shutdownAtText, L"Shutdown time unavailable");
        SetWindowTextW(g_shutdownAt, g_shutdownAtText);
    }

    SetStatus(statusText);
}

static void RestoreSchedule(HWND window)
{
    ScheduleState state = {};
    ScheduleStateResult result = LoadScheduleState(GetTickCount64(), &state);
    if (result == ScheduleStateResult::Missing)
        return;

    if (result == ScheduleStateResult::Error)
    {
        SetStatus(L"Could not read the saved shutdown schedule.");
        MessageBoxW(window,
            L"The saved shutdown schedule could not be read. You may not be able to reschedule it.",
            L"Shutdown Timer", MB_OK | MB_ICONWARNING);
        return;
    }

    if (result == ScheduleStateResult::Invalid)
    {
        if (!DeleteScheduleState())
        {
            SetStatus(L"An invalid saved shutdown schedule could not be removed.");
            MessageBoxW(window,
                L"The saved shutdown schedule is invalid and could not be removed.",
                L"Shutdown Timer", MB_OK | MB_ICONWARNING);
        }
        return;
    }

    g_scheduleStartedAt = state.startedAt;
    g_deadline = state.deadline;
    DWORD remaining = GetRemainingSeconds(g_deadline, GetTickCount64());
    wchar_t status[128];
    FormatRemainingDuration(remaining, status);
    SetScheduledUi(window, remaining, status);
}

static void Schedule(HWND window)
{
    int days;
    int hours;
    int minutes;
    int seconds;
    if (!ReadTimeValue(g_timeEdits[0], g_timeMaximums[0], &days) ||
        !ReadTimeValue(g_timeEdits[1], g_timeMaximums[1], &hours) ||
        !ReadTimeValue(g_timeEdits[2], g_timeMaximums[2], &minutes) ||
        !ReadTimeValue(g_timeEdits[3], g_timeMaximums[3], &seconds))
    {
        MessageBoxW(window,
            L"Days must be 0-3,650, hours 0-87,600, minutes 0-5,256,000, and seconds 0-315,360,000.",
            L"Shutdown Timer", MB_OK | MB_ICONWARNING);
        return;
    }

    ULONGLONG totalSecondsValue = static_cast<ULONGLONG>(days) * 86400 +
        static_cast<ULONGLONG>(hours) * 3600 +
        static_cast<ULONGLONG>(minutes) * 60 + seconds;

    if (totalSecondsValue == 0)
    {
        MessageBoxW(window, L"Choose a time greater than zero.", L"Shutdown Timer",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (totalSecondsValue > g_maxShutdownSeconds)
    {
        MessageBoxW(window,
            L"The total shutdown delay cannot exceed 315,360,000 seconds (10 years).",
            L"Shutdown Timer", MB_OK | MB_ICONWARNING);
        return;
    }

    DWORD totalSeconds = static_cast<DWORD>(totalSecondsValue);

    DWORD previousRemaining = 0;
    if (g_scheduled)
    {
        ULONGLONG now = GetTickCount64();
        previousRemaining = now < g_deadline
            ? static_cast<DWORD>((g_deadline - now + 999) / 1000)
            : 1;

        DWORD cancelExitCode = 1;
        if (!RunShutdownCommand(L"-a", &cancelExitCode) || cancelExitCode != 0)
        {
            MessageBoxW(window, L"Windows could not replace the existing shutdown timer.",
                L"Shutdown Timer", MB_OK | MB_ICONERROR);
            return;
        }
    }

    wchar_t arguments[64];
    wsprintfW(arguments, L"-s -f -t %lu", totalSeconds);
    DWORD exitCode = 1;
    if (!RunShutdownCommand(arguments, &exitCode) || exitCode != 0)
    {
        if (g_scheduled)
        {
            wchar_t restoreArguments[64];
            wsprintfW(restoreArguments, L"-s -f -t %lu", previousRemaining);
            DWORD restoreExitCode = 1;
            if (RunShutdownCommand(restoreArguments, &restoreExitCode) &&
                restoreExitCode == 0)
            {
                g_scheduleStartedAt = GetTickCount64();
                g_deadline = g_scheduleStartedAt +
                    static_cast<ULONGLONG>(previousRemaining) * 1000;
                bool saved = SaveScheduleState(g_scheduleStartedAt, g_deadline);
                SetStatus(saved
                    ? L"Reschedule failed; previous shutdown restored."
                    : L"Previous shutdown restored, but its saved state could not be updated.");
                MessageBoxW(window,
                    saved
                        ? L"Windows could not apply the new time. The previous shutdown was restored."
                        : L"Windows could not apply the new time. The previous shutdown was restored, but its saved state could not be updated.",
                    L"Shutdown Timer", MB_OK | MB_ICONERROR);
            }
            else
            {
                g_scheduled = false;
                KillTimer(window, ID_COUNTDOWN);
                SetScheduleButtonText();
                DeleteScheduleState();
                wcscpy_s(g_shutdownAtText, L"Shutdown at: —");
                SetWindowTextW(g_shutdownAt, g_shutdownAtText);
                SetStatus(L"Reschedule failed; no shutdown is scheduled.");
                MessageBoxW(window,
                    L"Windows could not apply the new time or restore the previous shutdown.",
                    L"Shutdown Timer", MB_OK | MB_ICONERROR);
            }
        }
        else
        {
            MessageBoxW(window, L"Windows could not schedule the shutdown.", L"Shutdown Timer",
                MB_OK | MB_ICONERROR);
        }
        return;
    }

    g_scheduleStartedAt = GetTickCount64();
    g_deadline = g_scheduleStartedAt + static_cast<ULONGLONG>(totalSeconds) * 1000;
    bool saved = SaveScheduleState(g_scheduleStartedAt, g_deadline);
    SetScheduledUi(window, totalSeconds, L"Shutdown scheduled.");
    if (!saved)
    {
        DeleteScheduleState();
        SetStatus(L"Shutdown scheduled, but its saved state could not be written.");
        MessageBoxW(window,
            L"Shutdown is scheduled, but the schedule could not be saved in %APPDATA%\\Shutdown Timer. If you reopen the app, it may not be able to reschedule this shutdown.",
            L"Shutdown Timer", MB_OK | MB_ICONWARNING);
    }
}

static void Cancel(HWND window)
{
    DWORD exitCode = 1;
    if (!RunShutdownCommand(L"-a", &exitCode) || exitCode != 0)
    {
        MessageBoxW(window, L"No active shutdown timer could be cancelled.", L"Shutdown Timer",
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    g_scheduled = false;
    g_scheduleStartedAt = 0;
    g_deadline = 0;
    KillTimer(window, ID_COUNTDOWN);
    SetScheduleButtonText();
    wcscpy_s(g_shutdownAtText, L"Shutdown at: —");
    SetWindowTextW(g_shutdownAt, g_shutdownAtText);
    if (DeleteScheduleState())
        SetStatus(L"Shutdown cancelled.");
    else
    {
        SetStatus(L"Shutdown cancelled, but its saved state could not be removed.");
        MessageBoxW(window,
            L"Shutdown was cancelled, but the saved schedule could not be removed. It may appear again the next time you open the app.",
            L"Shutdown Timer", MB_OK | MB_ICONWARNING);
    }
}

static HWND AddControl(HWND parent, const wchar_t* className, const wchar_t* text,
    DWORD style, int x, int y, int width, int height, int id)
{
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (control && g_font)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

static bool AdjustTimeAtCursor(int delta)
{
    POINT cursor;
    if (!GetCursorPos(&cursor))
        return false;

    for (int i = 0; i < 4; ++i)
    {
        RECT editRect;
        RECT spinnerRect;
        if (!GetWindowRect(g_timeEdits[i], &editRect) ||
            !GetWindowRect(g_timeSpinners[i], &spinnerRect))
            continue;

        if (!PtInRect(&editRect, cursor) && !PtInRect(&spinnerRect, cursor))
            continue;

        g_wheelRemainders[i] += delta;
        while (g_wheelRemainders[i] >= WHEEL_DELTA)
        {
            AdjustTimeValue(i, 1);
            g_wheelRemainders[i] -= WHEEL_DELTA;
        }
        while (g_wheelRemainders[i] <= -WHEEL_DELTA)
        {
            AdjustTimeValue(i, -1);
            g_wheelRemainders[i] += WHEEL_DELTA;
        }
        return true;
    }

    return false;
}

static LRESULT CALLBACK TimeInputSubclassProc(HWND control, UINT message,
    WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR);

static bool CanInsertTimeText(HWND edit, int index, const wchar_t* insertedText)
{
    wchar_t currentText[16] = {};
    int currentLength = GetWindowTextW(edit, currentText,
        static_cast<int>(_countof(currentText)));
    if (currentLength < 0 || currentLength >= static_cast<int>(_countof(currentText)))
        return false;

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart),
        reinterpret_cast<LPARAM>(&selectionEnd));
    if (selectionStart > static_cast<DWORD>(currentLength) ||
        selectionEnd > static_cast<DWORD>(currentLength))
        return false;

    size_t insertedLength = wcslen(insertedText);
    size_t candidateLength = static_cast<size_t>(currentLength) -
        (selectionEnd - selectionStart) + insertedLength;
    if (candidateLength > static_cast<size_t>(g_timeInputLengths[index]) ||
        candidateLength >= _countof(currentText))
        return false;

    wchar_t candidate[16];
    memcpy(candidate, currentText, selectionStart * sizeof(wchar_t));
    for (size_t i = 0; i < insertedLength; ++i)
    {
        if (insertedText[i] < L'0' || insertedText[i] > L'9')
            return false;
        candidate[selectionStart + i] = insertedText[i];
    }
    size_t suffixLength = static_cast<size_t>(currentLength) - selectionEnd;
    memcpy(candidate + selectionStart + insertedLength,
        currentText + selectionEnd, suffixLength * sizeof(wchar_t));
    candidate[candidateLength] = L'\0';

    wchar_t* end = nullptr;
    unsigned long value = wcstoul(candidate, &end, 10);
    return end != candidate && *end == L'\0' &&
        value <= static_cast<unsigned long>(g_timeMaximums[index]);
}

static LRESULT CALLBACK TimeInputSubclassProc(HWND control, UINT message,
    WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    int index = GetDlgCtrlID(control) - ID_DAYS_EDIT;
    if (index >= 0 && index < 4)
    {
        if (message == WM_CHAR && wParam >= L'0' && wParam <= L'9')
        {
            wchar_t digit[] = { static_cast<wchar_t>(wParam), L'\0' };
            if (!CanInsertTimeText(control, index, digit))
                return 0;
        }
        else if (message == WM_PASTE)
        {
            if (!OpenClipboard(control))
                return 0;

            HANDLE clipboardData = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* pastedText = clipboardData
                ? static_cast<const wchar_t*>(GlobalLock(clipboardData))
                : nullptr;
            bool allowed = pastedText &&
                CanInsertTimeText(control, index, pastedText);
            if (pastedText)
                GlobalUnlock(clipboardData);
            CloseClipboard();
            if (!allowed)
                return 0;
        }
    }

    if (message == WM_MOUSEWHEEL && AdjustTimeAtCursor(GET_WHEEL_DELTA_WPARAM(wParam)))
        return 0;

    return DefSubclassProc(control, message, wParam, lParam);
}

static HWND AddTimeInput(HWND parent, const wchar_t* label, int x,
    int initialValue, int editId, int spinId)
{
    int index = spinId - ID_DAYS;
    AddControl(parent, L"STATIC", label, 0, x, 22, 92, 20, 0);
    HWND edit = AddControl(parent, L"EDIT", L"",
        WS_BORDER | ES_NUMBER | ES_CENTER | ES_MULTILINE | ES_AUTOHSCROLL,
        x, 45, 76, 26, editId);
    SendMessageW(edit, EM_LIMITTEXT, g_timeInputLengths[index], 0);
    wchar_t value[16];
    wsprintfW(value, L"%d", initialValue);
    SetWindowTextW(edit, value);

    HWND spin = AddControl(parent, UPDOWN_CLASSW, L"",
        UDS_ARROWKEYS,
        x + 76, 45, 16, 26, spinId);
    SendMessageW(spin, UDM_SETRANGE32, 0, g_timeMaximums[index]);
    SendMessageW(spin, UDM_SETPOS32, 0, initialValue);
    g_timeEdits[index] = edit;
    g_timeSpinners[index] = spin;
    SetWindowSubclass(edit, TimeInputSubclassProc, spinId,
        reinterpret_cast<DWORD_PTR>(spin));
    SetWindowSubclass(spin, TimeInputSubclassProc, spinId,
        reinterpret_cast<DWORD_PTR>(spin));

    HDC dc = GetDC(edit);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    ReleaseDC(edit, dc);

    RECT formatRect;
    GetClientRect(edit, &formatRect);
    int verticalPadding = (formatRect.bottom - metrics.tmHeight) / 2;
    if (verticalPadding > 0)
    {
        formatRect.top = verticalPadding;
        SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatRect));
    }

    return spin;
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        AddTimeInput(window, L"Days", 8, 0, ID_DAYS_EDIT, ID_DAYS);
        AddTimeInput(window, L"Hours", 106, 0, ID_HOURS_EDIT, ID_HOURS);
        AddTimeInput(window, L"Minutes", 204, 0, ID_MINUTES_EDIT, ID_MINUTES);
        AddTimeInput(window, L"Seconds", 302, 0, ID_SECONDS_EDIT, ID_SECONDS);
        g_scheduleButton = AddControl(window, L"BUTTON", L"Schedule shutdown",
            BS_PUSHBUTTON, 12, 100, 120, 24, ID_SCHEDULE);
        AddControl(window, L"BUTTON", L"Clear all",
            BS_PUSHBUTTON, 140, 100, 120, 24, ID_CLEAR_ALL);
        AddControl(window, L"BUTTON", L"Cancel shutdown",
            BS_PUSHBUTTON, 268, 100, 120, 24, ID_CANCEL);
        AddControl(window, L"STATIC", L"Tip: Hover over a time field and scroll to adjust.",
            SS_LEFT, 12, 76, 376, 18, 107);
        g_shutdownAt = AddControl(window, L"STATIC", g_shutdownAtText,
            SS_LEFT, 12, 142, 376, 18, 108);
        g_status = AddControl(window, L"STATIC", L"No shutdown scheduled.",
            SS_LEFT, 12, 164, 376, 24, ID_STATUS);
        RestoreSchedule(window);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_SCHEDULE)
            Schedule(window);
        else if (LOWORD(wParam) == ID_CLEAR_ALL)
            ClearAllTimeValues();
        else if (LOWORD(wParam) == ID_CANCEL)
            Cancel(window);
        return 0;

    case WM_NOTIFY:
    {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->code == UDN_DELTAPOS)
        {
            int index = static_cast<int>(header->idFrom) - ID_DAYS;
            if (index >= 0 && index < 4)
                AdjustTimeValue(index,
                    reinterpret_cast<NMUPDOWN*>(lParam)->iDelta);
            return 1;
        }
        break;
    }

    case WM_MOUSEWHEEL:
        if (AdjustTimeAtCursor(GET_WHEEL_DELTA_WPARAM(wParam)))
            return 0;
        break;

    case WM_TIMER:
        if (wParam == ID_COUNTDOWN && g_scheduled)
        {
            ULONGLONG now = GetTickCount64();
            if (now >= g_deadline)
            {
                g_scheduled = false;
                KillTimer(window, ID_COUNTDOWN);
                g_scheduleStartedAt = 0;
                g_deadline = 0;
                SetScheduleButtonText();
                SetStatus(DeleteScheduleState()
                    ? L"Shutdown is starting."
                    : L"Shutdown is starting; saved schedule could not be removed.");
            }
            else
            {
                DWORD remaining = GetRemainingSeconds(g_deadline, now);
                wchar_t status[128];
                FormatRemainingDuration(remaining, status);
                SetStatus(status);
            }
        }
        return 0;

    case WM_DESTROY:
        KillTimer(window, ID_COUNTDOWN);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&controls);

    const wchar_t className[] = L"ShutdownTimerWindow";
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SHUTDOWN_TIMER));
    if (!windowClass.hIcon)
        return 1;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass))
        return 1;

    DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT windowRect = { 0, 0, 400, 194 };
    if (!AdjustWindowRectEx(&windowRect, windowStyle, FALSE, 0))
        return 1;

    RECT workArea;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
        return 1;

    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;
    int windowX = workArea.left + (workArea.right - workArea.left - windowWidth) / 2;
    int windowY = workArea.top + (workArea.bottom - workArea.top - windowHeight) / 2;
    HWND window = CreateWindowExW(0, className, L"Shutdown Timer v3.3",
        windowStyle, windowX, windowY, windowWidth, windowHeight,
        nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;

    SendMessageW(window, WM_SETICON, ICON_BIG,
        reinterpret_cast<LPARAM>(windowClass.hIcon));
    HICON smallIcon = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_SHUTDOWN_TIMER), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (smallIcon)
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
