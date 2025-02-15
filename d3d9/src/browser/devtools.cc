#include "../internal.h"

#include <shlobj.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

// BROWSER PROCESS ONLY.

extern HWND rclient_window_;
extern cef_browser_t *browser_;
extern UINT REMOTE_DEBUGGING_PORT;

static HWND devtools_window_;
static std::string REMOTE_DEVTOOLS_URL;

static cef_client_t *CreateDevToolsClient();

bool IsWindowsLightTheme();
void ForceDarkTheme(HWND hwnd);

void OpenDevTools_Internal(bool remote)
{
    if (remote)
    {
        if (REMOTE_DEBUGGING_PORT == 0) return;
        if (REMOTE_DEVTOOLS_URL.empty()) return;

        ShellExecuteA(NULL, "open",
            REMOTE_DEVTOOLS_URL.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    else if (browser_ != nullptr)
    {
        // This function can be called from non-UI thread,
        // so CefBrowserHost::HasDevTools has no effect.

        DWORD processId;
        GetWindowThreadProcessId(devtools_window_, &processId);

        if (processId == GetCurrentProcessId())
        {
            // Restore if minimized.
            if (IsIconic(devtools_window_))
                ShowWindow(devtools_window_, SW_RESTORE);
            else
                ShowWindow(devtools_window_, SW_SHOWNORMAL);

            SetForegroundWindow(devtools_window_);
        }
        else
        {
            cef_window_info_t wi{};
            wi.x = CW_USEDEFAULT;
            wi.y = CW_USEDEFAULT;
            wi.width = CW_USEDEFAULT;
            wi.height = CW_USEDEFAULT;
            wi.ex_style = WS_EX_APPWINDOW;
            wi.style = WS_OVERLAPPEDWINDOW
                | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE;
            wi.window_name = CefStr(L"DevTools - League Client").forawrd();

            cef_browser_settings_t settings{};
            auto host = browser_->get_host(browser_);
            host->show_dev_tools(host, &wi, CreateDevToolsClient(), &settings, nullptr);
            //                              ^--- We use new client to keep DevTools
            //                                   from being scaled by League Client (e.g 0.8, 1.6).
        }
    }
}

static void PrepareDevTools_Thread()
{
    if (REMOTE_DEBUGGING_PORT != 0)
    {
        HINTERNET hInit, hConn, hFile;

        hInit = InternetOpenA("HTTPGET", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        hConn = InternetConnectA(hInit, "localhost", REMOTE_DEBUGGING_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        hFile = HttpOpenRequestA(hConn, NULL, "/json/list", "HTTP/1.1", NULL, NULL,
            INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS, NULL);

        if (HttpSendRequestA(hFile, NULL, 0, NULL, 0)) {
            CHAR content[1024];
            DWORD contentLength = 0;

            InternetReadFile(hFile, content, 1024, &contentLength);

            if (contentLength > 0) {
                static CHAR pattern[] = "\"devtoolsFrontendUrl\": \"";
                auto pos = strstr(content, pattern);

                if (pos) {
                    auto start = pos + sizeof(pattern) - 1;
                    auto end = strstr(start, "\"");

                    std::string link = "http://127.0.0.1:";
                    link.append(std::to_string(REMOTE_DEBUGGING_PORT));
                    link.append(std::string(start, end - start));

                    REMOTE_DEVTOOLS_URL = link;
                }
            }
        }

        InternetCloseHandle(hInit);
        InternetCloseHandle(hConn);
        InternetCloseHandle(hFile);
    }
}

void PrepareDevTools()
{
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&PrepareDevTools_Thread, NULL, 0, NULL);
}

class DevToolsDownloadHandler : public CefRefCount<cef_download_handler_t>
{
public:
    DevToolsDownloadHandler() : CefRefCount(this)
    {
        cef_download_handler_t::on_before_download = on_before_download;
        cef_download_handler_t::on_download_updated = on_download_updated;
    }

    static void CEF_CALLBACK on_before_download(
        struct _cef_download_handler_t* self,
        struct _cef_browser_t* browser,
        struct _cef_download_item_t* download_item,
        const cef_string_t* suggested_name,
        struct _cef_before_download_callback_t* callback)
    {
        wstring path = L"C:\\";

        WCHAR personalPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, personalPath)))
        {
            path.append(personalPath);
            path.append(L"\\Downloads\\");
        }

        if (suggested_name != nullptr)
            path.append(suggested_name->str, suggested_name->length);

        callback->cont(callback, &CefStr(path), true);
    }

    static void CEF_CALLBACK on_download_updated(
        struct _cef_download_handler_t* self,
        struct _cef_browser_t* browser,
        struct _cef_download_item_t* download_item,
        struct _cef_download_item_callback_t* callback)
    {
    }
};

class DevToolsLifeSpanHandler : public CefRefCount<cef_life_span_handler_t>
{
public:
    DevToolsLifeSpanHandler() : CefRefCount(this)
    {
        cef_life_span_handler_t::on_before_popup = on_before_popup;
        cef_life_span_handler_t::on_after_created = on_after_created;
        cef_life_span_handler_t::do_close = do_close;
        cef_life_span_handler_t::on_before_close = on_before_close;
    }

    static int CEF_CALLBACK on_before_popup(
        struct _cef_life_span_handler_t* self,
        struct _cef_browser_t* browser,
        struct _cef_frame_t* frame,
        const cef_string_t* target_url,
        const cef_string_t* target_frame_name,
        cef_window_open_disposition_t target_disposition,
        int user_gesture,
        const struct _cef_popup_features_t* popupFeatures,
        struct _cef_window_info_t* windowInfo,
        struct _cef_client_t** client,
        struct _cef_browser_settings_t* settings,
        struct _cef_dictionary_value_t** extra_info,
        int* no_javascript_access)
    {
        return 0;
    }

    static void CEF_CALLBACK on_after_created(struct _cef_life_span_handler_t* self,
        struct _cef_browser_t* browser)
    {
        auto host = browser->get_host(browser);
        auto hwnd = host->get_window_handle(host);

        // Get League icon.
        HICON icon = (HICON)SendMessageW(rclient_window_, WM_GETICON, ICON_BIG, 0);

        // Set window icon.
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);

        if (!IsWindowsLightTheme())
        {
            // Force dark theme.
            ForceDarkTheme(hwnd);

            RECT rc;
            GetClientRect(hwnd, &rc);
            // Fix titlebar issue.
            SetWindowPos(hwnd, NULL, 0, 0, rc.right - 5, rc.bottom, SWP_NOMOVE | SWP_FRAMECHANGED);
        }

        devtools_window_ = hwnd;
    }

    static int CEF_CALLBACK do_close(struct _cef_life_span_handler_t* self,
        struct _cef_browser_t* browser)
    {
        return 0;
    }

    static void CEF_CALLBACK on_before_close(struct _cef_life_span_handler_t* self,
        struct _cef_browser_t* browser)
    {
        devtools_window_ = nullptr;
    }
};

class DevToolsClient : public CefRefCount<cef_client_t>
{
public:
    DevToolsClient() : CefRefCount(this)
    {
        cef_client_t::get_audio_handler = get_audio_handler;
        cef_client_t::get_context_menu_handler = get_context_menu_handler;
        cef_client_t::get_dialog_handler = get_dialog_handler;
        cef_client_t::get_display_handler = get_display_handler;
        cef_client_t::get_download_handler = get_download_handler;
        cef_client_t::get_drag_handler = get_drag_handler;
        cef_client_t::get_find_handler = get_find_handler;
        cef_client_t::get_focus_handler = get_focus_handler;
        cef_client_t::get_jsdialog_handler = get_jsdialog_handler;
        cef_client_t::get_keyboard_handler = get_keyboard_handler;
        cef_client_t::get_life_span_handler = get_life_span_handler;
        cef_client_t::get_load_handler = get_load_handler;
        cef_client_t::get_print_handler = get_print_handler;
        cef_client_t::get_render_handler = get_render_handler;
        cef_client_t::get_request_handler = get_request_handler;
        cef_client_t::on_process_message_received = on_process_message_received;
    }

private:
    static struct _cef_audio_handler_t* CEF_CALLBACK get_audio_handler(struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_context_menu_handler_t* CEF_CALLBACK get_context_menu_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_dialog_handler_t* CEF_CALLBACK get_dialog_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_display_handler_t* CEF_CALLBACK get_display_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_download_handler_t* CEF_CALLBACK get_download_handler(
        struct _cef_client_t* self)
    {
        return new DevToolsDownloadHandler();
    }

    static struct _cef_drag_handler_t* CEF_CALLBACK get_drag_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_find_handler_t* CEF_CALLBACK get_find_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_focus_handler_t* CEF_CALLBACK get_focus_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_jsdialog_handler_t* CEF_CALLBACK get_jsdialog_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_keyboard_handler_t* CEF_CALLBACK get_keyboard_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_life_span_handler_t* CEF_CALLBACK get_life_span_handler(
        struct _cef_client_t* self)
    {
        return new DevToolsLifeSpanHandler();
    }

    static struct _cef_load_handler_t* CEF_CALLBACK get_load_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_print_handler_t* CEF_CALLBACK get_print_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_render_handler_t* CEF_CALLBACK get_render_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static struct _cef_request_handler_t* CEF_CALLBACK get_request_handler(
        struct _cef_client_t* self)
    {
        return nullptr;
    }

    static int CEF_CALLBACK on_process_message_received(
        struct _cef_client_t* self,
        struct _cef_browser_t* browser,
        struct _cef_frame_t* frame,
        cef_process_id_t source_process,
        struct _cef_process_message_t* message)
    {
        return 0;
    }
};

static cef_client_t *CreateDevToolsClient()
{
    return new DevToolsClient();
}