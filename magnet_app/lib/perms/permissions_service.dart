// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:permission_handler/permission_handler.dart' as ph;

/// R21 / U12 — small interface in front of `permission_handler` so:
/// (a) the OS prompts can be opted out of in tests, and
/// (b) screens can ask for one capability at a time with friendly
/// fallback copy when the OS says no (or "ask me later").
///
/// Concrete implementations: [PlatformPermissionsService] uses the
/// real package; [NoopPermissionsService] is the default fake for
/// unit/widget tests so they never block on platform channels.
enum PermissionResult {
  granted,
  denied,
  permanentlyDenied,
  restricted,
  /// The capability isn't required by this OS/platform combo (e.g.
  /// POST_NOTIFICATIONS on iOS — handled by `flutter_local_notifications`
  /// during scheduling, not as a runtime permission).
  notApplicable,
}

abstract class PermissionsService {
  Future<bool> bleGranted();
  Future<PermissionResult> requestBle();
  Future<bool> notificationsGranted();
  Future<PermissionResult> requestNotifications();

  /// R22 / U13 — when-in-use location for the phone-GPS one-shot
  /// fallback. Maps to `ACCESS_FINE_LOCATION` (Android) /
  /// `NSLocationWhenInUseUsageDescription` (iOS).
  Future<bool> locationGranted();
  Future<PermissionResult> requestLocation();

  /// Opens the OS app-settings page so the user can flip a
  /// permanently-denied permission back on.
  Future<void> openAppSettingsPage();
}

/// Default fake used in tests + as the value when no `PermissionsService`
/// is provided — never invokes the platform layer.
class NoopPermissionsService implements PermissionsService {
  NoopPermissionsService({
    this.ble = true,
    this.notifications = true,
    this.location = true,
  });
  bool ble;
  bool notifications;
  bool location;

  @override
  Future<bool> bleGranted() async => ble;
  @override
  Future<PermissionResult> requestBle() async =>
      ble ? PermissionResult.granted : PermissionResult.denied;
  @override
  Future<bool> notificationsGranted() async => notifications;
  @override
  Future<PermissionResult> requestNotifications() async =>
      notifications ? PermissionResult.granted : PermissionResult.denied;
  @override
  Future<bool> locationGranted() async => location;
  @override
  Future<PermissionResult> requestLocation() async =>
      location ? PermissionResult.granted : PermissionResult.denied;
  @override
  Future<void> openAppSettingsPage() async {}
}

/// Real implementation backed by `permission_handler`.
class PlatformPermissionsService implements PermissionsService {
  const PlatformPermissionsService();

  /// On Android 12+ (API 31+) the granular `BLUETOOTH_SCAN` /
  /// `BLUETOOTH_CONNECT` runtime permissions are what we actually
  /// need. The legacy `Permission.bluetooth` maps to the pre-31
  /// install-time `android.permission.BLUETOOTH` — which our
  /// manifest declares with `maxSdkVersion="30"`, so on modern
  /// Android requesting it makes `permission_handler` log
  /// "Bluetooth permission missing in manifest" and surface a UI
  /// error. We omit it; pre-31 devices are still covered by the
  /// manifest's install-time `BLUETOOTH` / `BLUETOOTH_ADMIN`.
  static const List<ph.Permission> _bleSet = <ph.Permission>[
    ph.Permission.bluetoothScan,
    ph.Permission.bluetoothConnect,
  ];

  @override
  Future<bool> bleGranted() async {
    for (final ph.Permission p in _bleSet) {
      final ph.PermissionStatus s = await p.status;
      if (!_isGranted(s)) return false;
    }
    return true;
  }

  @override
  Future<PermissionResult> requestBle() async {
    final Map<ph.Permission, ph.PermissionStatus> res =
        await _bleSet.request();
    return _worst(res.values);
  }

  @override
  Future<bool> notificationsGranted() async {
    final ph.PermissionStatus s = await ph.Permission.notification.status;
    return _isGranted(s);
  }

  @override
  Future<PermissionResult> requestNotifications() async {
    final ph.PermissionStatus s =
        await ph.Permission.notification.request();
    return _map(s);
  }

  @override
  Future<bool> locationGranted() async {
    final ph.PermissionStatus s =
        await ph.Permission.locationWhenInUse.status;
    return _isGranted(s);
  }

  @override
  Future<PermissionResult> requestLocation() async {
    final ph.PermissionStatus s =
        await ph.Permission.locationWhenInUse.request();
    return _map(s);
  }

  @override
  Future<void> openAppSettingsPage() async {
    await ph.openAppSettings();
  }

  bool _isGranted(ph.PermissionStatus s) =>
      s == ph.PermissionStatus.granted ||
      s == ph.PermissionStatus.limited ||
      s == ph.PermissionStatus.provisional;

  PermissionResult _map(ph.PermissionStatus s) {
    if (_isGranted(s)) return PermissionResult.granted;
    if (s == ph.PermissionStatus.permanentlyDenied) {
      return PermissionResult.permanentlyDenied;
    }
    if (s == ph.PermissionStatus.restricted) {
      return PermissionResult.restricted;
    }
    return PermissionResult.denied;
  }

  PermissionResult _worst(Iterable<ph.PermissionStatus> ss) {
    PermissionResult best = PermissionResult.granted;
    int rank(PermissionResult r) => switch (r) {
          PermissionResult.granted => 0,
          PermissionResult.notApplicable => 1,
          PermissionResult.denied => 2,
          PermissionResult.restricted => 3,
          PermissionResult.permanentlyDenied => 4,
        };
    for (final ph.PermissionStatus s in ss) {
      final PermissionResult r = _map(s);
      if (rank(r) > rank(best)) best = r;
    }
    return best;
  }
}
