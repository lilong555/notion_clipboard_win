#include <windows.h>
#include <shellapi.h>

#include "app_icon.h"
#include "autostart.h"
#include "config.h"
#include "config_page.h"
#include "converter.h"
#include "hotkey.h"
#include "logger.h"
#include "queue.h"
#include "resource.h"
#include "upload_target.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
using ncw::CreateGeneratedAppIcon;
using ncw::LastErrorMessage;
using ncw::AppConfig;
using ncw::AtomicWriteFile;
using ncw::BuildTextBlocks;
using ncw::BuildTitleFromContent;
using ncw::CliOptions;
using ncw::CreateUploadTarget;
using ncw::CurrentHotkeyModifiers;
using ncw::ExtractCfHtmlFragment;
using ncw::Fnv1a64;
using ncw::HasEmptyMarkdownCodeFenceArtifact;
using ncw::Hex64;
using ncw::HtmlFragmentToMarkdown;
using ncw::HotkeySpec;
using ncw::HotkeySpecFromRecordedKey;
using ncw::IsAutoStartEnabled;
using ncw::IsModifierVirtualKey;
using ncw::Logger;
using ncw::LoadConfig;
using ncw::NowUnixMs;
using ncw::NormalizeLineEndings;
using ncw::ParseHotkeyOrThrow;
using ncw::ParseCli;
using ncw::PrintHelp;
using ncw::PersistentQueue;
using ncw::ReadWholeFile;
using ncw::RunDryRunText;
using ncw::RunConfigPageSelfTest;
using ncw::RunSelfTest;
using ncw::RunUploadTargetSelfTest;
using ncw::SetAutoStartEnabled;
using ncw::Trim;
using ncw::UpsertConfigValue;
using ncw::UploadFailure;
using ncw::UploadJob;
using ncw::UploadTarget;
using ncw::Utf8ToWide;
using ncw::ValidateConfigOrThrow;
using ncw::WideToUtf8;
using ncw::WriteConfigPage;

constexpr UINT_PTR kClipboardDebounceTimer = 1001;
constexpr UINT kUploadHotkeyId = 2001;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMenuUploadNow = 3001;
constexpr UINT kMenuHotkeyStatus = 3002;
constexpr UINT kMenuToggleHotkey = 3003;
constexpr UINT kMenuRecordHotkey = 3004;
constexpr UINT kMenuToggleNotifications = 3005;
constexpr UINT kMenuToggleAutoStart = 3006;
constexpr UINT kMenuToggleClipboardListener = 3007;
constexpr UINT kMenuOpenConfig = 3008;
constexpr UINT kMenuOpenLog = 3009;
constexpr UINT kMenuOpenStateDir = 3010;
constexpr UINT kMenuExit = 3011;
constexpr UINT kMenuOpenConfigPage = 3012;
constexpr const wchar_t *kAppDisplayName = L"Notion Clipboard Win";

#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif

HICON LoadApplicationIcon(HINSTANCE instance, int width, int height)
{
    HICON icon = reinterpret_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
    if (icon != nullptr)
    {
        return icon;
    }
    return CreateGeneratedAppIcon(width, height);
}

std::atomic<std::uint64_t> g_job_counter{0};

UploadJob MakeUploadJob(const std::string &content, const std::string &target)
{
    UploadJob job;
    job.created_at_ms = NowUnixMs();
    job.not_before_ms = 0;
    job.target = target;
    job.hash = Hex64(Fnv1a64(content));
    job.title = BuildTitleFromContent(content);
    job.content = content;

    std::ostringstream id;
    id << job.created_at_ms << "-" << job.hash.substr(0, 12) << "-" << GetCurrentProcessId() << "-"
       << g_job_counter.fetch_add(1);
    job.id = id.str();
    return job;
}

class UploadWorker
{
public:
    UploadWorker(PersistentQueue *queue, UploadTarget *target, Logger *logger)
        : queue_(queue), target_(target), logger_(logger)
    {
    }

    void Start()
    {
        stop_.store(false);
        thread_ = std::thread([this]
                              { Run(); });
    }

    void Stop()
    {
        stop_.store(true);
        cv_.notify_all();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    void Notify()
    {
        cv_.notify_all();
    }

private:
    void Run()
    {
        if (logger_ != nullptr)
        {
            logger_->Info("上传线程已启动");
        }

        while (!stop_.load())
        {
            std::uint64_t next_due = 0;
            auto item = queue_->NextDueJob(NowUnixMs(), &next_due, logger_);
            if (!item.has_value())
            {
                WaitForNextJob(next_due);
                continue;
            }

            UploadJob job = item->first;
            const fs::path path = item->second;
            try
            {
                target_->ProcessJob(&job, [&]
                                    { queue_->Update(path, job); });
                queue_->MarkSuccess(path);
                if (logger_ != nullptr)
                {
                    logger_->Info("上传成功: " + job.id + (job.remote_url.empty() ? "" : " -> " + job.remote_url));
                }
            }
            catch (const UploadFailure &ex)
            {
                queue_->MarkFailure(path, job, ex.what(), ex.retryable(), ex.retry_after_seconds(), logger_);
            }
            catch (const std::exception &ex)
            {
                queue_->MarkFailure(path, job, ex.what(), true, 0, logger_);
            }
        }

        if (logger_ != nullptr)
        {
            logger_->Info("上传线程已停止");
        }
    }

    void WaitForNextJob(std::uint64_t next_due)
    {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        if (next_due == 0)
        {
            cv_.wait_for(lock, std::chrono::seconds(30), [&]
                         { return stop_.load(); });
            return;
        }

        const std::uint64_t now = NowUnixMs();
        const std::uint64_t delay = next_due > now ? next_due - now : 0;
        cv_.wait_for(lock, std::chrono::milliseconds(delay), [&]
                     { return stop_.load(); });
    }

    PersistentQueue *queue_ = nullptr;
    UploadTarget *target_ = nullptr;
    Logger *logger_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::condition_variable cv_;
    std::mutex wait_mutex_;
};

class DisabledUploadTarget : public UploadTarget
{
public:
    explicit DisabledUploadTarget(std::string reason) : reason_(std::move(reason)) {}

    std::string Name() const override
    {
        return "disabled";
    }

    void Validate() override
    {
        throw UploadFailure(reason_, false, 0);
    }

    void ProcessJob(UploadJob *, const std::function<void()> &) override
    {
        throw UploadFailure(reason_, false, 0);
    }

private:
    std::string reason_;
};

class ClipboardReader
{
public:
    std::optional<std::string> ReadText(Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        const UINT html_format = RegisterClipboardFormatW(L"HTML Format");
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT) &&
            (html_format == 0 || !IsClipboardFormatAvailable(html_format)))
        {
            return std::nullopt;
        }

        bool opened = false;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            if (OpenClipboard(nullptr))
            {
                opened = true;
                break;
            }
            Sleep(static_cast<DWORD>(20 * (attempt + 1)));
        }

        if (!opened)
        {
            if (logger != nullptr)
            {
                logger->Warn("打开剪贴板失败: " + LastErrorMessage());
            }
            return std::nullopt;
        }

        struct ClipboardGuard
        {
            ~ClipboardGuard()
            {
                CloseClipboard();
            }
        } guard;

        const auto unicode_text = ReadUnicodeText(logger, max_clipboard_bytes);
        if (html_format != 0 && IsClipboardFormatAvailable(html_format))
        {
            const auto html_text = ReadHtmlText(html_format, logger, max_clipboard_bytes);
            if (html_text.has_value())
            {
                if (!HasEmptyMarkdownCodeFenceArtifact(*html_text))
                {
                    return html_text;
                }
                if (logger != nullptr)
                {
                    logger->Warn("HTML 剪贴板转换结果包含空代码块围栏，已回退到纯文本");
                }
            }
        }

        return unicode_text;
    }

private:
    std::optional<std::string> ReadHtmlText(UINT html_format, Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        HANDLE data = GetClipboardData(html_format);
        if (data == nullptr)
        {
            return std::nullopt;
        }

        const SIZE_T raw_bytes = GlobalSize(data);
        const std::uint64_t html_limit = std::min<std::uint64_t>(4ull * 1024ull * 1024ull, max_clipboard_bytes * 8ull);
        if (raw_bytes == 0 || raw_bytes > static_cast<SIZE_T>(html_limit))
        {
            if (logger != nullptr && raw_bytes > 0)
            {
                logger->Warn("HTML 剪贴板片段过大，回退到纯文本，bytes=" + std::to_string(raw_bytes));
            }
            return std::nullopt;
        }

        const char *raw = static_cast<const char *>(GlobalLock(data));
        if (raw == nullptr)
        {
            return std::nullopt;
        }
        std::string html(raw, raw + raw_bytes);
        GlobalUnlock(data);

        const auto fragment = ExtractCfHtmlFragment(html);
        if (!fragment.has_value())
        {
            return std::nullopt;
        }

        std::string converted = HtmlFragmentToMarkdown(*fragment);
        converted = Trim(converted);
        if (converted.empty())
        {
            return std::nullopt;
        }
        if (converted.size() > max_clipboard_bytes)
        {
            if (logger != nullptr)
            {
                logger->Warn("HTML 剪贴板转换后文本过大，已跳过，utf8_bytes=" + std::to_string(converted.size()));
            }
            return std::nullopt;
        }
        if (logger != nullptr)
        {
            logger->Info("已读取 HTML 剪贴板片段并转换为 Markdown-like 文本，bytes=" +
                         std::to_string(converted.size()));
        }
        return converted;
    }

    std::optional<std::string> ReadUnicodeText(Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data == nullptr)
        {
            return std::nullopt;
        }

        const SIZE_T raw_bytes = GlobalSize(data);
        if (raw_bytes > 0 && raw_bytes > static_cast<SIZE_T>(max_clipboard_bytes * 2))
        {
            if (logger != nullptr)
            {
                logger->Warn("剪贴板原始文本过大，已跳过，utf16_bytes=" + std::to_string(raw_bytes));
            }
            return std::nullopt;
        }

        const wchar_t *raw = static_cast<const wchar_t *>(GlobalLock(data));
        if (raw == nullptr)
        {
            return std::nullopt;
        }

        std::wstring wide(raw);
        GlobalUnlock(data);
        std::string utf8 = NormalizeLineEndings(WideToUtf8(wide));
        utf8 = Trim(utf8);
        if (utf8.empty())
        {
            return std::nullopt;
        }
        if (utf8.size() > max_clipboard_bytes)
        {
            if (logger != nullptr)
            {
                logger->Warn("剪贴板文本过大，已跳过，utf8_bytes=" + std::to_string(utf8.size()));
            }
            return std::nullopt;
        }
        return utf8;
    }
};

class TrayApplication
{
public:
    TrayApplication(const AppConfig *config, fs::path config_path, PersistentQueue *queue, UploadWorker *worker,
                    Logger *logger, std::string startup_config_error)
        : config_(config),
          config_path_(std::move(config_path)),
          queue_(queue),
          worker_(worker),
          logger_(logger),
          startup_config_error_(std::move(startup_config_error)),
          hotkey_spec_(ParseHotkeyOrThrow(config->hotkey)),
          hotkey_enabled_(config->enable_hotkey),
          notifications_enabled_(config->tray_notifications),
          auto_start_enabled_(config->start_with_windows),
          auto_start_configured_(config->start_with_windows_configured),
          clipboard_listener_enabled_(config->enable_clipboard_listener)
    {
    }

    int Run()
    {
        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        app_icon_ = LoadApplicationIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        tray_icon_ = LoadApplicationIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TrayApplication::WindowProc;
        wc.hInstance = instance;
        wc.hIcon = app_icon_ != nullptr ? app_icon_ : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = tray_icon_ != nullptr ? tray_icon_ : wc.hIcon;
        wc.lpszClassName = L"NotionClipboardWinTrayWindow";

        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, kAppDisplayName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr)
        {
            throw std::runtime_error("创建后台窗口失败: " + LastErrorMessage());
        }
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

        AddTrayIcon();
        SyncAutoStartSetting();
        if (hotkey_enabled_)
        {
            hotkey_enabled_ = RegisterUploadHotkey();
        }
        if (clipboard_listener_enabled_)
        {
            EnableClipboardListener(true);
        }

        if (startup_config_error_.empty() && config_->upload_initial_clipboard)
        {
            ProcessClipboard("启动读取", false);
        }

        if (logger_ != nullptr)
        {
            logger_->Info("托盘进程已启动，hotkey=" + hotkey_spec_.display +
                          "，clipboard_listener=" + (clipboard_listener_registered_ ? "on" : "off"));
        }
        ShowNotification(L"Notion Clipboard Win", L"后台进程已启动，按 " + Utf8ToWide(hotkey_spec_.display) + L" 上传剪贴板。");
        MaybeShowStartupConfigError();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Cleanup();
        if (logger_ != nullptr)
        {
            logger_->Info("托盘进程已停止");
        }
        return 0;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        TrayApplication *self = nullptr;
        if (message == WM_NCCREATE)
        {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            self = static_cast<TrayApplication *>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<TrayApplication *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self == nullptr)
        {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return self->HandleMessage(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wparam, LPARAM lparam)
    {
        TrayApplication *self = recording_instance_;
        if (code == HC_ACTION && self != nullptr && self->recording_hotkey_ &&
            (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN))
        {
            const auto *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lparam);
            const UINT vk = static_cast<UINT>(info->vkCode);
            if (vk == VK_ESCAPE)
            {
                self->CancelHotkeyRecording();
                return 1;
            }
            if (IsModifierVirtualKey(vk))
            {
                return CallNextHookEx(self->keyboard_hook_, code, wparam, lparam);
            }

            const auto spec = HotkeySpecFromRecordedKey(CurrentHotkeyModifiers(), vk);
            if (spec.has_value())
            {
                self->ApplyRecordedHotkey(*spec);
                return 1;
            }

            self->ShowNotification(L"Notion Clipboard Win", L"热键需要包含 Ctrl/Alt/Shift/Win 和支持的主按键。");
            return 1;
        }
        return CallNextHookEx(self == nullptr ? nullptr : self->keyboard_hook_, code, wparam, lparam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == taskbar_created_message_)
        {
            tray_icon_added_ = false;
            AddTrayIcon();
            return 0;
        }

        switch (message)
        {
        case WM_HOTKEY:
            if (wparam == kUploadHotkeyId)
            {
                ProcessClipboard("热键", true);
            }
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wparam));
            return 0;
        case kTrayCallbackMessage:
            if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP)
            {
                ShowContextMenu();
            }
            else if (LOWORD(lparam) == WM_LBUTTONDBLCLK)
            {
                ProcessClipboard("托盘双击", true);
            }
            return 0;
        case WM_CLIPBOARDUPDATE:
            if (clipboard_listener_registered_)
            {
                SetTimer(hwnd, kClipboardDebounceTimer, static_cast<UINT>(config_->debounce_ms), nullptr);
            }
            return 0;
        case WM_TIMER:
            if (wparam == kClipboardDebounceTimer)
            {
                KillTimer(hwnd, kClipboardDebounceTimer);
                ProcessClipboard("剪贴板事件", false);
            }
            return 0;
        case WM_CLOSE:
            Cleanup();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    void AddTrayIcon()
    {
        if (hwnd_ == nullptr)
        {
            return;
        }

        nid_ = {};
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = kTrayIconId;
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid_.uCallbackMessage = kTrayCallbackMessage;
        nid_.hIcon =
            tray_icon_ != nullptr ? tray_icon_ : (app_icon_ != nullptr ? app_icon_ : LoadIconW(nullptr, IDI_APPLICATION));
        wcsncpy_s(nid_.szTip, kAppDisplayName, _TRUNCATE);

        const DWORD action = tray_icon_added_ ? NIM_MODIFY : NIM_ADD;
        if (!Shell_NotifyIconW(action, &nid_))
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("托盘图标更新失败: " + LastErrorMessage());
            }
            return;
        }

        if (!tray_icon_added_)
        {
            nid_.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid_);
        }
        tray_icon_added_ = true;
    }

    void ShowNotification(const std::wstring &title, const std::wstring &message)
    {
        if (!notifications_enabled_ || !tray_icon_added_)
        {
            return;
        }

        NOTIFYICONDATAW notify = nid_;
        notify.uFlags = NIF_INFO;
        notify.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(notify.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(notify.szInfo, message.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &notify);
    }

    bool RegisterUploadHotkey()
    {
        if (hwnd_ == nullptr)
        {
            return false;
        }
        if (hotkey_registered_)
        {
            UnregisterHotKey(hwnd_, kUploadHotkeyId);
            hotkey_registered_ = false;
        }

        const UINT modifiers = hotkey_spec_.modifiers | MOD_NOREPEAT;
        if (!RegisterHotKey(hwnd_, kUploadHotkeyId, modifiers, hotkey_spec_.vk))
        {
            if (logger_ != nullptr)
            {
                logger_->Error("注册全局热键失败: " + hotkey_spec_.display + "，原因: " + LastErrorMessage());
            }
            ShowNotification(L"Notion Clipboard Win", L"注册全局热键失败，可能已被其他程序占用。");
            return false;
        }

        hotkey_registered_ = true;
        if (logger_ != nullptr)
        {
            logger_->Info("全局热键已注册: " + hotkey_spec_.display);
        }
        return true;
    }

    void EnableClipboardListener(bool enabled)
    {
        if (hwnd_ == nullptr)
        {
            return;
        }

        if (enabled && !clipboard_listener_registered_)
        {
            if (!AddClipboardFormatListener(hwnd_))
            {
                if (logger_ != nullptr)
                {
                    logger_->Error("注册剪贴板监听失败: " + LastErrorMessage());
                }
                ShowNotification(L"Notion Clipboard Win", L"注册剪贴板监听失败。");
                clipboard_listener_enabled_ = false;
                return;
            }
            clipboard_listener_registered_ = true;
            clipboard_listener_enabled_ = true;
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板监听已启动");
            }
        }
        else if (!enabled && clipboard_listener_registered_)
        {
            RemoveClipboardFormatListener(hwnd_);
            clipboard_listener_registered_ = false;
            clipboard_listener_enabled_ = false;
            KillTimer(hwnd_, kClipboardDebounceTimer);
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板监听已停止");
            }
        }
        else
        {
            clipboard_listener_enabled_ = enabled;
        }
    }

    void ShowContextMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const std::wstring hotkey_label = L"热键: " + Utf8ToWide(hotkey_spec_.display);
        AppendMenuW(menu, MF_STRING, kMenuUploadNow, L"上传当前剪贴板");
        AppendMenuW(menu, MF_GRAYED, kMenuHotkeyStatus, hotkey_label.c_str());
        AppendMenuW(menu, MF_STRING | (hotkey_enabled_ ? MF_CHECKED : MF_UNCHECKED), kMenuToggleHotkey, L"启用热键");
        AppendMenuW(menu, MF_STRING | (recording_hotkey_ ? MF_GRAYED : MF_ENABLED), kMenuRecordHotkey, L"录制热键...");
        AppendMenuW(menu, MF_STRING | (notifications_enabled_ ? MF_CHECKED : MF_UNCHECKED), kMenuToggleNotifications,
                    L"显示通知");
        AppendMenuW(menu, MF_STRING | (auto_start_enabled_ ? MF_CHECKED : MF_UNCHECKED), kMenuToggleAutoStart,
                    L"开机自动启动");
        AppendMenuW(menu, MF_STRING | (clipboard_listener_enabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kMenuToggleClipboardListener, L"自动监听剪贴板");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuOpenConfigPage, L"打开配置页面");
        AppendMenuW(menu, MF_STRING, kMenuOpenConfig, L"打开配置文件");
        AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"查看日志");
        AppendMenuW(menu, MF_STRING, kMenuOpenStateDir, L"打开状态目录");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

        POINT cursor;
        GetCursorPos(&cursor);
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void HandleCommand(UINT command)
    {
        switch (command)
        {
        case kMenuUploadNow:
            ProcessClipboard("托盘菜单", true);
            break;
        case kMenuToggleHotkey:
            ToggleHotkey();
            break;
        case kMenuRecordHotkey:
            StartHotkeyRecording();
            break;
        case kMenuToggleNotifications:
            ToggleNotifications();
            break;
        case kMenuToggleAutoStart:
            ToggleAutoStart();
            break;
        case kMenuToggleClipboardListener:
            EnableClipboardListener(!clipboard_listener_enabled_);
            break;
        case kMenuOpenConfig:
            OpenConfig();
            break;
        case kMenuOpenConfigPage:
            OpenConfigPage();
            break;
        case kMenuOpenLog:
            OpenPath(config_->state_dir / L"notion-clipboard-win.log");
            break;
        case kMenuOpenStateDir:
            OpenPath(config_->state_dir);
            break;
        case kMenuExit:
            Cleanup();
            DestroyWindow(hwnd_);
            break;
        default:
            break;
        }
    }

    void ToggleHotkey()
    {
        if (hotkey_enabled_)
        {
            if (hotkey_registered_)
            {
                UnregisterHotKey(hwnd_, kUploadHotkeyId);
                hotkey_registered_ = false;
            }
            hotkey_enabled_ = false;
            if (logger_ != nullptr)
            {
                logger_->Info("全局热键已暂停");
            }
            return;
        }

        hotkey_enabled_ = RegisterUploadHotkey();
    }

    bool PersistConfigValue(const std::string &key, const std::string &value)
    {
        try
        {
            UpsertConfigValue(config_path_, key, value);
            return true;
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("写入配置失败: " + key + "=" + value + "，原因: " + std::string(ex.what()));
            }
            return false;
        }
    }

    void SyncAutoStartSetting()
    {
        if (!auto_start_configured_)
        {
            auto_start_enabled_ = IsAutoStartEnabled(config_path_);
            return;
        }

        std::string error;
        if (!SetAutoStartEnabled(auto_start_enabled_, config_path_, &error))
        {
            auto_start_enabled_ = IsAutoStartEnabled(config_path_);
            if (logger_ != nullptr)
            {
                logger_->Warn(error);
            }
            ShowNotification(L"Notion Clipboard Win", L"同步开机自动启动设置失败，请查看日志。");
            return;
        }

        auto_start_enabled_ = IsAutoStartEnabled(config_path_);
        if (logger_ != nullptr)
        {
            logger_->Info(std::string("开机自动启动已同步: ") + (auto_start_enabled_ ? "on" : "off"));
        }
    }

    void StopHotkeyRecordingHook()
    {
        if (keyboard_hook_ != nullptr)
        {
            UnhookWindowsHookEx(keyboard_hook_);
            keyboard_hook_ = nullptr;
        }
        if (recording_instance_ == this)
        {
            recording_instance_ = nullptr;
        }
        recording_hotkey_ = false;
    }

    void StartHotkeyRecording()
    {
        if (recording_hotkey_)
        {
            return;
        }

        const int result = MessageBoxW(hwnd_,
                                       L"点击“确定”后按下新的全局热键。\n\n"
                                       L"要求：Ctrl/Alt/Shift/Win 至少一个修饰键 + 一个主按键。\n"
                                       L"按 Esc 取消录制。",
                                       L"录制热键", MB_OKCANCEL | MB_ICONINFORMATION | MB_TOPMOST);
        if (result != IDOK)
        {
            return;
        }

        restore_hotkey_enabled_after_recording_ = hotkey_enabled_;
        if (hotkey_registered_)
        {
            UnregisterHotKey(hwnd_, kUploadHotkeyId);
            hotkey_registered_ = false;
        }

        recording_hotkey_ = true;
        recording_instance_ = this;
        keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &TrayApplication::KeyboardHookProc,
                                           GetModuleHandleW(nullptr), 0);
        if (keyboard_hook_ == nullptr)
        {
            const std::string error = LastErrorMessage();
            StopHotkeyRecordingHook();
            if (restore_hotkey_enabled_after_recording_)
            {
                hotkey_enabled_ = RegisterUploadHotkey();
            }
            if (logger_ != nullptr)
            {
                logger_->Error("启动热键录制失败: " + error);
            }
            MessageBoxW(hwnd_, L"启动热键录制失败，请查看日志。", L"Notion Clipboard Win",
                        MB_OK | MB_ICONERROR | MB_TOPMOST);
            return;
        }

        if (logger_ != nullptr)
        {
            logger_->Info("开始录制全局热键");
        }
        ShowNotification(L"Notion Clipboard Win", L"正在录制热键，按 Esc 取消。");
    }

    void CancelHotkeyRecording()
    {
        StopHotkeyRecordingHook();
        if (restore_hotkey_enabled_after_recording_)
        {
            hotkey_enabled_ = RegisterUploadHotkey();
        }
        if (logger_ != nullptr)
        {
            logger_->Info("热键录制已取消");
        }
        ShowNotification(L"Notion Clipboard Win", L"热键录制已取消。");
    }

    void ApplyRecordedHotkey(const HotkeySpec &new_spec)
    {
        const HotkeySpec previous_spec = hotkey_spec_;
        const bool previous_enabled = restore_hotkey_enabled_after_recording_;
        StopHotkeyRecordingHook();

        hotkey_spec_ = new_spec;
        hotkey_enabled_ = true;
        if (!RegisterUploadHotkey())
        {
            hotkey_spec_ = previous_spec;
            hotkey_enabled_ = previous_enabled;
            if (previous_enabled)
            {
                hotkey_enabled_ = RegisterUploadHotkey();
            }
            MessageBoxW(hwnd_, L"新热键注册失败，可能已被其他程序占用；已恢复原热键。", L"Notion Clipboard Win",
                        MB_OK | MB_ICONWARNING | MB_TOPMOST);
            return;
        }

        PersistConfigValue("hotkey", hotkey_spec_.display);
        PersistConfigValue("enable_hotkey", "true");
        if (logger_ != nullptr)
        {
            logger_->Info("全局热键已更新: " + hotkey_spec_.display);
        }
        std::wstring message = L"热键已更新为 ";
        message += Utf8ToWide(hotkey_spec_.display);
        message += L"。";
        ShowNotification(L"Notion Clipboard Win", message);
    }

    void ToggleNotifications()
    {
        notifications_enabled_ = !notifications_enabled_;
        PersistConfigValue("tray_notifications", notifications_enabled_ ? "true" : "false");
        if (logger_ != nullptr)
        {
            logger_->Info(std::string("托盘通知已") + (notifications_enabled_ ? "启用" : "关闭"));
        }
        if (notifications_enabled_)
        {
            ShowNotification(L"Notion Clipboard Win", L"托盘通知已启用。");
        }
    }

    void ToggleAutoStart()
    {
        const bool next_enabled = !auto_start_enabled_;
        std::string error;
        if (!SetAutoStartEnabled(next_enabled, config_path_, &error))
        {
            if (logger_ != nullptr)
            {
                logger_->Error(error);
            }
            ShowNotification(L"Notion Clipboard Win", L"更新开机自动启动失败，请查看日志。");
            return;
        }

        auto_start_enabled_ = next_enabled;
        auto_start_configured_ = true;
        PersistConfigValue("start_with_windows", auto_start_enabled_ ? "true" : "false");
        if (logger_ != nullptr)
        {
            logger_->Info(std::string("开机自动启动已") + (auto_start_enabled_ ? "启用" : "关闭"));
        }
        ShowNotification(L"Notion Clipboard Win",
                         auto_start_enabled_ ? L"开机自动启动已启用。" : L"开机自动启动已关闭。");
    }

    void OpenConfig()
    {
        if (!fs::exists(config_path_))
        {
            try
            {
                const fs::path parent = config_path_.parent_path();
                if (!parent.empty())
                {
                    fs::create_directories(parent);
                }
                AtomicWriteFile(config_path_, "upload_target=notion\n"
                                             "markdown_output_dir=\n"
                                             "webhook_url=\n"
                                             "webhook_bearer_token=\n"
                                             "github_token=\n"
                                             "github_gist_public=false\n"
                                             "github_gist_filename_prefix=clipboard\n"
                                             "github_repo_owner=\n"
                                             "github_repo_name=\n"
                                             "github_repo_branch=\n"
                                             "github_repo_directory=clipboard\n"
                                             "github_repo_filename_prefix=clipboard\n"
                                             "yuque_token=\n"
                                             "yuque_namespace=\n"
                                             "yuque_slug_prefix=clipboard\n"
                                             "feishu_app_id=\n"
                                             "feishu_app_secret=\n"
                                             "feishu_folder_token=\n"
                                             "obsidian_vault_dir=\n"
                                             "obsidian_folder=Clipboard\n"
                                             "obsidian_filename_prefix=clipboard\n"
                                             "local_git_repo_dir=\n"
                                             "local_git_directory=clipboard\n"
                                             "local_git_filename_prefix=clipboard\n"
                                             "local_git_auto_commit=false\n"
                                             "notion_token=\n"
                                             "data_source_id=\n"
                                             "database_id=\n"
                                             "hotkey=Ctrl+Shift+B\n"
                                             "enable_hotkey=true\n"
                                             "enable_clipboard_listener=false\n"
                                             "tray_notifications=true\n"
                                             "start_with_windows=false\n");
            }
            catch (const std::exception &ex)
            {
                if (logger_ != nullptr)
                {
                    logger_->Warn("创建配置文件失败: " + std::string(ex.what()));
                }
            }
        }
        OpenPath(config_path_);
    }

    void OpenConfigPage()
    {
        try
        {
            OpenPath(WriteConfigPage(*config_, config_path_));
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("创建配置页面失败: " + std::string(ex.what()));
            }
            ShowNotification(L"Notion Clipboard Win", L"创建配置页面失败，请查看日志。");
        }
    }

    void MaybeShowStartupConfigError()
    {
        if (startup_config_error_.empty())
        {
            return;
        }
        if (logger_ != nullptr)
        {
            logger_->Warn("配置尚未可用: " + startup_config_error_);
        }
        ShowNotification(L"Notion Clipboard Win", L"配置尚未完成，已打开配置页面。");
        OpenConfigPage();
        MessageBoxW(hwnd_, (L"配置尚未完成，上传功能暂不可用。\n\n" + Utf8ToWide(startup_config_error_) +
                                L"\n\n请在配置页面填写必要项后重启程序。")
                               .c_str(),
                    L"Notion Clipboard Win", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }

    void OpenPath(const fs::path &path)
    {
        const std::wstring target = path.wstring();
        HINSTANCE result = ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("打开路径失败: " + WideToUtf8(target));
            }
            ShowNotification(L"Notion Clipboard Win", L"打开路径失败。");
        }
    }

    void ProcessClipboard(const char *trigger, bool user_initiated)
    {
        if (!startup_config_error_.empty())
        {
            if (logger_ != nullptr)
            {
                logger_->Warn(std::string(trigger) + "上传被跳过，配置尚未完成: " + startup_config_error_);
            }
            if (user_initiated)
            {
                ShowNotification(L"Notion Clipboard Win", L"配置尚未完成，已打开配置页面。");
                OpenConfigPage();
            }
            return;
        }

        const auto text = reader_.ReadText(logger_, config_->max_clipboard_bytes);
        if (!text.has_value())
        {
            if (user_initiated)
            {
                ShowNotification(L"Notion Clipboard Win", L"当前剪贴板没有可上传的文本。");
            }
            return;
        }

        UploadJob job = MakeUploadJob(*text, config_->upload_target);
        const std::uint64_t now_ms = NowUnixMs();
        if (config_->duplicate_suppression_ms > 0 && job.hash == last_hash_ &&
            now_ms - last_hash_at_ms_ <= static_cast<std::uint64_t>(config_->duplicate_suppression_ms))
        {
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板内容重复，已忽略");
            }
            if (user_initiated)
            {
                ShowNotification(L"Notion Clipboard Win", L"短时间内相同内容已忽略。");
            }
            return;
        }

        queue_->Enqueue(job);
        last_hash_ = job.hash;
        last_hash_at_ms_ = now_ms;
        worker_->Notify();
        if (logger_ != nullptr)
        {
            logger_->Info(std::string(trigger) + "已入队剪贴板内容: " + job.id +
                          "，bytes=" + std::to_string(text->size()));
        }
        if (user_initiated)
        {
            ShowNotification(L"Notion Clipboard Win", L"剪贴板内容已加入上传队列。");
        }
    }

    void Cleanup()
    {
        if (cleaned_up_)
        {
            return;
        }
        cleaned_up_ = true;
        StopHotkeyRecordingHook();

        if (hwnd_ != nullptr)
        {
            if (clipboard_listener_registered_)
            {
                RemoveClipboardFormatListener(hwnd_);
                clipboard_listener_registered_ = false;
            }
            if (hotkey_registered_)
            {
                UnregisterHotKey(hwnd_, kUploadHotkeyId);
                hotkey_registered_ = false;
            }
        }

        if (tray_icon_added_)
        {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            tray_icon_added_ = false;
        }
        if (tray_icon_ != nullptr)
        {
            DestroyIcon(tray_icon_);
            tray_icon_ = nullptr;
        }
        if (app_icon_ != nullptr)
        {
            DestroyIcon(app_icon_);
            app_icon_ = nullptr;
        }
    }

    const AppConfig *config_ = nullptr;
    fs::path config_path_;
    PersistentQueue *queue_ = nullptr;
    UploadWorker *worker_ = nullptr;
    Logger *logger_ = nullptr;
    std::string startup_config_error_;
    ClipboardReader reader_;
    HotkeySpec hotkey_spec_;
    std::string last_hash_;
    std::uint64_t last_hash_at_ms_ = 0;
    HWND hwnd_ = nullptr;
    HICON app_icon_ = nullptr;
    HICON tray_icon_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    UINT taskbar_created_message_ = 0;
    bool hotkey_enabled_ = true;
    bool hotkey_registered_ = false;
    bool notifications_enabled_ = true;
    bool auto_start_enabled_ = false;
    bool auto_start_configured_ = false;
    bool clipboard_listener_enabled_ = false;
    bool clipboard_listener_registered_ = false;
    bool tray_icon_added_ = false;
    bool cleaned_up_ = false;
    bool recording_hotkey_ = false;
    bool restore_hotkey_enabled_after_recording_ = false;
    HHOOK keyboard_hook_ = nullptr;
    static TrayApplication *recording_instance_;
};

TrayApplication *TrayApplication::recording_instance_ = nullptr;

DWORD g_main_thread_id = 0;

BOOL WINAPI ConsoleCtrlHandler(DWORD control_type)
{
    switch (control_type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_main_thread_id != 0)
        {
            PostThreadMessageW(g_main_thread_id, WM_QUIT, 0, 0);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

int RunOnce(const AppConfig &config, PersistentQueue *queue, UploadTarget *target, Logger *logger, bool dry_run)
{
    ClipboardReader reader;
    const auto text = reader.ReadText(logger, config.max_clipboard_bytes);
    if (!text.has_value())
    {
        throw std::runtime_error("当前剪贴板没有可上传的文本");
    }

    UploadJob job = MakeUploadJob(*text, config.upload_target);
    if (dry_run)
    {
        const std::vector<std::string> blocks = BuildTextBlocks(job.content);
        const auto equation_count = std::count_if(blocks.begin(), blocks.end(), [](const std::string &block)
                                                  { return block.find("\"type\":\"equation\"") != std::string::npos; });
        const auto code_count = std::count_if(blocks.begin(), blocks.end(), [](const std::string &block)
                                              { return block.find("\"type\":\"code\"") != std::string::npos; });
        logger->Info("dry-run: title=" + job.title + "，bytes=" + std::to_string(text->size()) +
                     "，blocks=" + std::to_string(blocks.size()) +
                     "，equations=" + std::to_string(static_cast<std::size_t>(equation_count)) +
                     "，code_blocks=" + std::to_string(static_cast<std::size_t>(code_count)));
        return 0;
    }

    try
    {
        target->ProcessJob(&job, [] {});
        logger->Info("上传成功: " + job.id + (job.remote_url.empty() ? "" : " -> " + job.remote_url));
        return 0;
    }
    catch (const UploadFailure &ex)
    {
        job.last_error = ex.what();
        queue->Enqueue(job);
        logger->Error("单次上传失败，任务已保存到队列: " + std::string(ex.what()));
        return ex.retryable() ? 2 : 3;
    }
}

int AppMain(int argc, wchar_t **argv)
{
#ifndef NOTION_CLIPBOARD_WIN_GUI
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    g_main_thread_id = GetCurrentThreadId();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    MSG bootstrap_msg;
    PeekMessageW(&bootstrap_msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const CliOptions cli = ParseCli(argc, argv);
    if (cli.help)
    {
        PrintHelp();
        return 0;
    }
    if (cli.self_test)
    {
        const int converter_result = RunSelfTest();
        if (converter_result != 0)
        {
            return converter_result;
        }
        const int target_result = RunUploadTargetSelfTest();
        if (target_result != 0)
        {
            return target_result;
        }
        return RunConfigPageSelfTest();
    }
    if (!cli.dry_run_file_path.empty())
    {
        return RunDryRunText(ReadWholeFile(cli.dry_run_file_path));
    }

    AppConfig config = LoadConfig(cli.config_path);
    std::string startup_config_error;
    try
    {
        ValidateConfigOrThrow(config);
    }
    catch (const std::exception &ex)
    {
        if (cli.validate_config || cli.once)
        {
            throw;
        }
        startup_config_error = ex.what();
    }
    fs::create_directories(config.state_dir);

#ifdef NOTION_CLIPBOARD_WIN_GUI
    const bool mirror_console = false;
#else
    const bool mirror_console = true;
#endif
    Logger logger(config.state_dir / L"notion-clipboard-win.log", mirror_console);
    logger.Info("程序启动，config=" + WideToUtf8(cli.config_path.wstring()));

    if (cli.validate_config)
    {
        std::unique_ptr<UploadTarget> upload_target = CreateUploadTarget(config, &logger);
        upload_target->Validate();
        logger.Info("配置验证通过");
        return 0;
    }

    std::unique_ptr<UploadTarget> upload_target = startup_config_error.empty()
                                                      ? CreateUploadTarget(config, &logger)
                                                      : std::make_unique<DisabledUploadTarget>(startup_config_error);
    PersistentQueue queue(config.state_dir, config.max_retry_attempts);

    if (cli.once)
    {
        return RunOnce(config, &queue, upload_target.get(), &logger, cli.dry_run);
    }

    UploadWorker worker(&queue, upload_target.get(), &logger);
    if (startup_config_error.empty())
    {
        worker.Start();
    }
    int code = 0;
    try
    {
        TrayApplication app(&config, cli.config_path, &queue, &worker, &logger, startup_config_error);
        code = app.Run();
    }
    catch (...)
    {
        if (startup_config_error.empty())
        {
            worker.Stop();
        }
        throw;
    }
    if (startup_config_error.empty())
    {
        worker.Stop();
    }
    return code;
}
} // 命名空间

#ifdef NOTION_CLIPBOARD_WIN_GUI
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try
    {
        return AppMain(argc, argv);
    }
    catch (const std::exception &ex)
    {
        MessageBoxW(nullptr, Utf8ToWide(ex.what()).c_str(), L"Notion Clipboard Win", MB_ICONERROR | MB_OK);
        return 1;
    }
}
#else
int wmain(int argc, wchar_t **argv)
{
    try
    {
        return AppMain(argc, argv);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "错误: " << ex.what() << "\n";
        return 1;
    }
}
#endif
