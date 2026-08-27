import 'package:menu_base/menu_base.dart';

abstract mixin class TrayListener {
  /// Emitted when the mouse clicks the tray icon.
  void onTrayIconMouseDown() {}

  /// Emitted when the mouse is released from clicking the tray icon.
  void onTrayIconMouseUp() {}

  void onTrayIconRightMouseDown() {}

  void onTrayIconRightMouseUp() {}

  void onTrayMenuItemClick(MenuItem menuItem) {}

  /// Emitted when the Windows system (taskbar) theme switches between light
  /// and dark. Use it to reset theme depentant menu
  /// icons via [TrayManager.setContextMenu]. Windows only.
  void onTrayThemeChanged(bool isSystemThemeLight) {}
}
