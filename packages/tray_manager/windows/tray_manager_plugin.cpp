#include "include/tray_manager/tray_manager_plugin.h"

// This must be included before many other Windows headers.
#include <stdio.h>
#include <windows.h>

#include <shellapi.h>
#include <strsafe.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <codecvt>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#define WM_MYMESSAGE (WM_USER + 1)

namespace {

const flutter::EncodableValue* ValueOrNull(const flutter::EncodableMap& map,
                                           const char* key) {
  auto it = map.find(flutter::EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &(it->second);
}

// Undocumented uxtheme.dll APIs (Windows 10 1809+). Win32 popup menus are
// light-only unless the process opts in through these; they are also the only
// way to un-stick a mode another component already forced, since the setting
// is process-wide.
enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
typedef PreferredAppMode(WINAPI* FnSetPreferredAppMode)(PreferredAppMode);
typedef void(WINAPI* FnFlushMenuThemes)();
std::unique_ptr<
    flutter::MethodChannel<flutter::EncodableValue>,
    std::default_delete<flutter::MethodChannel<flutter::EncodableValue>>>
    channel = nullptr;

class TrayManagerPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  TrayManagerPlugin(flutter::PluginRegistrarWindows* registrar);

  virtual ~TrayManagerPlugin();

 private:
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> g_converter;

  flutter::PluginRegistrarWindows* registrar;
  NOTIFYICONDATA nid;
  NOTIFYICONIDENTIFIER niif;
  // do create pop-up menu only once.
  HMENU hMenu = CreatePopupMenu();
  // Menu items don't own their hbmpItem bitmaps — we do; freed on rebuild.
  std::vector<HBITMAP> menu_bitmaps;
  bool last_system_light_theme = false;
  bool tray_icon_setted = false;
  UINT windows_taskbar_created_message_id = 0;

  // The ID of the WindowProc delegate registration.
  int window_proc_id = -1;

  void TrayManagerPlugin::_CreateMenu(HMENU menu, flutter::EncodableMap args);
  HBITMAP TrayManagerPlugin::_LoadIconAsBitmap(const std::string& path);
  void TrayManagerPlugin::_ApplyIcon();
  bool TrayManagerPlugin::_IsSystemLightTheme();
  void TrayManagerPlugin::_UpdateMenuTheme();

  // Called for top-level WindowProc delegation.
  std::optional<LRESULT> TrayManagerPlugin::HandleWindowProc(HWND hwnd,
                                                             UINT message,
                                                             WPARAM wparam,
                                                             LPARAM lparam);
  HWND TrayManagerPlugin::GetMainWindow();
  void TrayManagerPlugin::Destroy(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void TrayManagerPlugin::SetIcon(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void TrayManagerPlugin::SetToolTip(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void TrayManagerPlugin::SetContextMenu(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void TrayManagerPlugin::PopUpContextMenu(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void TrayManagerPlugin::GetBounds(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

static bool plugin_already_registered = false;

// static
void TrayManagerPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  if (plugin_already_registered) {
    // Skip registration in subwindow
    return;
  }
  
  plugin_already_registered = true;
  
  channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(), "tray_manager",
      &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<TrayManagerPlugin>(registrar);

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

TrayManagerPlugin::TrayManagerPlugin(flutter::PluginRegistrarWindows* registrar)
    : registrar(registrar) {
  window_proc_id = registrar->RegisterTopLevelWindowProcDelegate(
      [this](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        return HandleWindowProc(hwnd, message, wparam, lparam);
      });
  windows_taskbar_created_message_id = RegisterWindowMessage(L"TaskbarCreated");
  last_system_light_theme = _IsSystemLightTheme();
}

TrayManagerPlugin::~TrayManagerPlugin() {
  registrar->UnregisterTopLevelWindowProcDelegate(window_proc_id);
}

void TrayManagerPlugin::_CreateMenu(HMENU menu, flutter::EncodableMap args) {
  flutter::EncodableList items = std::get<flutter::EncodableList>(
      args.at(flutter::EncodableValue("items")));

  int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; i++) {
    // always delete at 0 because they shift every time. DeleteMenu (unlike
    // RemoveMenu) also destroys submenu handles, which would leak otherwise.
    DeleteMenu(menu, 0, MF_BYPOSITION);
  }

  for (flutter::EncodableValue item_value : items) {
    flutter::EncodableMap item_map =
        std::get<flutter::EncodableMap>(item_value);
    int id = std::get<int>(item_map.at(flutter::EncodableValue("id")));
    std::string type =
        std::get<std::string>(item_map.at(flutter::EncodableValue("type")));
    std::string label =
        std::get<std::string>(item_map.at(flutter::EncodableValue("label")));
    auto* checked = std::get_if<bool>(ValueOrNull(item_map, "checked"));
    bool disabled =
        std::get<bool>(item_map.at(flutter::EncodableValue("disabled")));
    auto* iconPath = std::get_if<std::string>(ValueOrNull(item_map, "icon"));

    UINT_PTR item_id = id;
    UINT uFlags = MF_STRING;

    if (disabled) {
      uFlags |= MF_GRAYED;
    }

    if (type.compare("separator") == 0) {
      AppendMenuW(menu, MF_SEPARATOR, item_id, NULL);
    } else {
      if (type.compare("checkbox") == 0) {
        if (checked == nullptr) {
          // skip
        } else {
          uFlags |= (*checked == true ? MF_CHECKED : MF_UNCHECKED);
        }
      } else if (type.compare("submenu") == 0) {
        uFlags |= MF_POPUP;
        HMENU sub_menu = ::CreatePopupMenu();
        _CreateMenu(sub_menu, std::get<flutter::EncodableMap>(item_map.at(
                                  flutter::EncodableValue("submenu"))));
        item_id = reinterpret_cast<UINT_PTR>(sub_menu);
      }
      AppendMenuW(menu, uFlags, item_id, g_converter.from_bytes(label).c_str());
      
      // apply icon bitmap if provided.
      if (iconPath != nullptr && !iconPath->empty()) {
        HBITMAP hBmp = _LoadIconAsBitmap(*iconPath);
        if (hBmp != NULL) {
          MENUITEMINFO mii  = {};
          mii.cbSize        = sizeof(MENUITEMINFO);
          mii.fMask         = MIIM_BITMAP;
          mii.hbmpItem      = hBmp;
          // Use item position (count before this append) because submenus
          // changed item_id to the HMENU pointer — safer to use MF_BYPOSITION.
          SetMenuItemInfo(menu,
                          static_cast<UINT>(GetMenuItemCount(menu) - 1),
                          TRUE,  // fByPosition
                          &mii);
          menu_bitmaps.push_back(hBmp);
        }
      }
    }
  }
}

// Loads a .ico file and converts it to a 16x16 32bpp HBITMAP suitable
// for MENUITEMINFO.hbmpItem (which requires a bitmap, not an HICON).

HBITMAP TrayManagerPlugin::_LoadIconAsBitmap(const std::string& path) {
  std::wstring wpath = g_converter.from_bytes(path);

  HICON hIcon = static_cast<HICON>(
      LoadImage(NULL, wpath.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
  if (!hIcon) return NULL;

  // Create a 32bpp DIB section so alpha channel is preserved correctly.
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth       = 16;
  bmi.bmiHeader.biHeight      = -16; // top-down
  bmi.bmiHeader.biPlanes      = 1;
  bmi.bmiHeader.biBitCount    = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  HDC     hdc    = GetDC(NULL);
  HDC     hdcMem = CreateCompatibleDC(hdc);
  void*   bits   = nullptr;
  HBITMAP hBmp   = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);

  if (hBmp) {
    HBITMAP hOld = static_cast<HBITMAP>(SelectObject(hdcMem, hBmp));
    // Fill transparent first, then draw icon on top.
    RECT rc = {0, 0, 16, 16};
    FillRect(hdcMem, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    DrawIconEx(hdcMem, 0, 0, hIcon, 16, 16, 0, NULL, DI_NORMAL);
    SelectObject(hdcMem, hOld);
  }

  DeleteDC(hdcMem);
  ReleaseDC(NULL, hdc);
  DestroyIcon(hIcon);
  return hBmp;
}

bool TrayManagerPlugin::_IsSystemLightTheme() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  // The taskbar/tray follows "SystemUsesLightTheme", not "AppsUseLightTheme".
  LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return status == ERROR_SUCCESS && value == 1;
}

void TrayManagerPlugin::_UpdateMenuTheme() {
  static bool loaded = false;
  static FnSetPreferredAppMode set_preferred_app_mode = nullptr;
  static FnFlushMenuThemes flush_menu_themes = nullptr;
  if (!loaded) {
    loaded = true;
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr,
                                     LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme != nullptr) {
      set_preferred_app_mode = reinterpret_cast<FnSetPreferredAppMode>(
          GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
      flush_menu_themes = reinterpret_cast<FnFlushMenuThemes>(
          GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    }
  }
  if (set_preferred_app_mode == nullptr) {
    // Pre-1809 Windows 10: menus stay light, nothing to do.
    return;
  }
  // Process-wide; re-applied before every popup so it tracks live theme
  // changes and wins over any mode another plugin forced earlier.
  set_preferred_app_mode(_IsSystemLightTheme() ? PreferredAppMode::ForceLight
                                               : PreferredAppMode::ForceDark);
  if (flush_menu_themes != nullptr) {
    flush_menu_themes();
  }
}

std::optional<LRESULT> TrayManagerPlugin::HandleWindowProc(HWND hWnd,
                                                           UINT message,
                                                           WPARAM wParam,
                                                           LPARAM lParam) {
  std::optional<LRESULT> result;
  if (message == WM_DESTROY) {
    if (tray_icon_setted) {
      Shell_NotifyIcon(NIM_DELETE, &nid);
      DestroyIcon(nid.hIcon);
    }
  } else if (message == WM_COMMAND) {
    flutter::EncodableMap eventData = flutter::EncodableMap();
    eventData[flutter::EncodableValue("id")] =
        flutter::EncodableValue((int)wParam);

    channel->InvokeMethod("onTrayMenuItemClick",
                          std::make_unique<flutter::EncodableValue>(eventData));
  } else if (message == WM_MYMESSAGE) {
    switch (lParam) {
      case WM_LBUTTONUP:
        channel->InvokeMethod("onTrayIconMouseDown",
                              std::make_unique<flutter::EncodableValue>());
        break;
      case WM_RBUTTONUP:
        channel->InvokeMethod("onTrayIconRightMouseDown",
                              std::make_unique<flutter::EncodableValue>());
        break;
      default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    };
  } else if (message == WM_SETTINGCHANGE) {
    // Broadcast with "ImmersiveColorSet" whenever the system/apps theme
    // changes (several times per change, so dedupe against the last value).
    if (lParam != 0 &&
        lstrcmpiW(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") ==
            0) {
      bool is_light = _IsSystemLightTheme();
      if (is_light != last_system_light_theme) {
        last_system_light_theme = is_light;
        flutter::EncodableMap eventData = flutter::EncodableMap();
        eventData[flutter::EncodableValue("isLight")] =
            flutter::EncodableValue(is_light);
        channel->InvokeMethod(
            "onTrayThemeChanged",
            std::make_unique<flutter::EncodableValue>(eventData));
      }
    }
  } else if (message == windows_taskbar_created_message_id) {
    if (windows_taskbar_created_message_id != 0 && tray_icon_setted) {
      // restore the icon with the existing resource.
      tray_icon_setted = false;
      _ApplyIcon();
    }
  } else if (message == WM_POWERBROADCAST) {
    // Handle power management events (sleep/wake)
    switch (wParam) {
      case PBT_APMRESUMEAUTOMATIC:
      case PBT_APMRESUMESUSPEND:
        // System is resuming from sleep/hibernation
        if (tray_icon_setted) {
          // Restore the tray icon after system wakes up
          tray_icon_setted = false;
          _ApplyIcon();
        }
        break;
      default:
        break;
    }
  }
  return result;
}

HWND TrayManagerPlugin::GetMainWindow() {
  return ::GetAncestor(registrar->GetView()->GetNativeWindow(), GA_ROOT);
}

void TrayManagerPlugin::Destroy(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Shell_NotifyIcon(NIM_DELETE, &nid);
  DestroyIcon(nid.hIcon);
  tray_icon_setted = false;

  result->Success(flutter::EncodableValue(true));
}

void TrayManagerPlugin::SetIcon(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const flutter::EncodableMap& args =
      std::get<flutter::EncodableMap>(*method_call.arguments());

  std::string iconPath =
      std::get<std::string>(args.at(flutter::EncodableValue("iconPath")));

  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;

  if (nid.hIcon != nullptr) {
    DestroyIcon(nid.hIcon);
  }

  nid.hIcon = static_cast<HICON>(
      LoadImage(nullptr, (LPCWSTR)(converter.from_bytes(iconPath).c_str()),
                IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));

  _ApplyIcon();

  result->Success(flutter::EncodableValue(true));
}

void TrayManagerPlugin::_ApplyIcon() {
  if (tray_icon_setted) {
    Shell_NotifyIcon(NIM_MODIFY, &nid);
  } else {
    HICON hIconBackup = nid.hIcon;
    WCHAR szTipBackup[128];
    StringCchCopy(szTipBackup, _countof(szTipBackup), nid.szTip);
    
    ZeroMemory(&nid, sizeof(NOTIFYICONDATA));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = GetMainWindow();
    nid.uID = 1;
    nid.hIcon = hIconBackup;
    StringCchCopy(nid.szTip, _countof(nid.szTip), szTipBackup);
    nid.uCallbackMessage = WM_MYMESSAGE;
    nid.uFlags = NIF_MESSAGE | NIF_ICON;
    if (nid.szTip[0] != '\0') {
      nid.uFlags |= NIF_TIP;
    }
    Shell_NotifyIcon(NIM_ADD, &nid);
  }

  niif.cbSize = sizeof(NOTIFYICONIDENTIFIER);
  niif.hWnd = nid.hWnd;
  niif.uID = nid.uID;
  niif.guidItem = GUID_NULL;

  tray_icon_setted = true;
}

void TrayManagerPlugin::SetToolTip(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const flutter::EncodableMap& args =
      std::get<flutter::EncodableMap>(*method_call.arguments());

  std::string toolTip =
      std::get<std::string>(args.at(flutter::EncodableValue("toolTip")));

  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  StringCchCopy(nid.szTip, _countof(nid.szTip),
                converter.from_bytes(toolTip).c_str());
  Shell_NotifyIcon(NIM_MODIFY, &nid);

  result->Success(flutter::EncodableValue(true));
}

void TrayManagerPlugin::SetContextMenu(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const flutter::EncodableMap& args =
      std::get<flutter::EncodableMap>(*method_call.arguments());

  std::vector<HBITMAP> old_bitmaps = std::move(menu_bitmaps);
  menu_bitmaps.clear();

  _CreateMenu(hMenu, std::get<flutter::EncodableMap>(
                         args.at(flutter::EncodableValue("menu"))));

  // Free the previous generation only after the items referencing them are
  // gone — the menu can be rebuilt while it is visible.
  for (HBITMAP bmp : old_bitmaps) {
    DeleteObject(bmp);
  }

  result->Success(flutter::EncodableValue(true));
}

void TrayManagerPlugin::PopUpContextMenu(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

  HWND hWnd = GetMainWindow();

  double x, y;

  // RECT rect;
  // Shell_NotifyIconGetRect(&niif, &rect);

  // x = rect.left + ((rect.right - rect.left) / 2);
  // y = rect.top + ((rect.bottom - rect.top) / 2);

  POINT cursorPos;
  GetCursorPos(&cursorPos);
  x = cursorPos.x;
  y = cursorPos.y;

  // Match the menu theme to the current system (taskbar) theme.
  _UpdateMenuTheme();

  // Always required — TrackPopupMenu won't dismiss on outside
  // click unless the window is foreground, regardless of bringAppToFront.
  SetForegroundWindow(hWnd);

  TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, static_cast<int>(x),
                 static_cast<int>(y), 0, hWnd, NULL);
  // Required to flush the menu message loop properly —
  // without this the next right-click may not open the menu.
  PostMessageW(hWnd, WM_NULL, 0, 0);

  result->Success(flutter::EncodableValue(true));
}

void TrayManagerPlugin::GetBounds(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const flutter::EncodableMap& args =
      std::get<flutter::EncodableMap>(*method_call.arguments());

  if (!tray_icon_setted) {
    result->Success();
    return;
  }

  double devicePixelRatio =
      std::get<double>(args.at(flutter::EncodableValue("devicePixelRatio")));

  RECT rect;
  Shell_NotifyIconGetRect(&niif, &rect);
  flutter::EncodableMap resultMap = flutter::EncodableMap();

  double x = rect.left / devicePixelRatio * 1.0f;
  double y = rect.top / devicePixelRatio * 1.0f;
  double width = (rect.right - rect.left) / devicePixelRatio * 1.0f;
  double height = (rect.bottom - rect.top) / devicePixelRatio * 1.0f;

  resultMap[flutter::EncodableValue("x")] = flutter::EncodableValue(x);
  resultMap[flutter::EncodableValue("y")] = flutter::EncodableValue(y);
  resultMap[flutter::EncodableValue("width")] = flutter::EncodableValue(width);
  resultMap[flutter::EncodableValue("height")] =
      flutter::EncodableValue(height);

  result->Success(flutter::EncodableValue(resultMap));
}

void TrayManagerPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("destroy") == 0) {
    Destroy(method_call, std::move(result));
  } else if (method_call.method_name().compare("setIcon") == 0) {
    SetIcon(method_call, std::move(result));
  } else if (method_call.method_name().compare("setToolTip") == 0) {
    SetToolTip(method_call, std::move(result));
  } else if (method_call.method_name().compare("setContextMenu") == 0) {
    SetContextMenu(method_call, std::move(result));
  } else if (method_call.method_name().compare("popUpContextMenu") == 0) {
    PopUpContextMenu(method_call, std::move(result));
  } else if (method_call.method_name().compare("getBounds") == 0) {
    GetBounds(method_call, std::move(result));
  } else {
    result->NotImplemented();
  }
}

}  // namespace

void TrayManagerPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  TrayManagerPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
