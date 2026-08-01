// ignore: unused_import
import 'package:intl/intl.dart' as intl;
import 'app_localizations.dart';

// ignore_for_file: type=lint

/// The translations for English (`en`).
class AppLocalizationsEn extends AppLocalizations {
  AppLocalizationsEn([String locale = 'en']) : super(locale);

  @override
  String get appTitle => 'Responsive IoT 2026';

  @override
  String get aboutSubtitle => 'BLE & WiFi device scanner';

  @override
  String aboutVersion(String version) {
    return 'Version $version';
  }

  @override
  String get aboutDescription =>
      'An open Flutter starter for Bluetooth LE and WiFi scanning.';

  @override
  String get aboutCopyright => '© IoTone Japan, 2024–2026';

  @override
  String get aboutLicense => 'MIT License';

  @override
  String get aboutMadeWith => 'Built with Flutter.';

  @override
  String get tabDashboard => 'Dashboard';

  @override
  String get tabRadar => 'Radar';

  @override
  String get tabDevices => 'Devices';

  @override
  String get tabSettings => 'Settings';

  @override
  String get quickNav => 'Quick navigation';

  @override
  String get commonCancel => 'Cancel';

  @override
  String get commonClose => 'Close';

  @override
  String get commonRetry => 'Retry';

  @override
  String get commonCopy => 'Copy';

  @override
  String get commonCopied => 'Copied';

  @override
  String get scanStart => 'Start scan';

  @override
  String get scanStop => 'Stop scan';

  @override
  String get scanScanning => 'Scanning…';

  @override
  String get scanIdle => 'Idle';

  @override
  String get bluetoothOn => 'Bluetooth ready';

  @override
  String get bluetoothOff => 'Bluetooth is off';

  @override
  String get bluetoothUnauthorized => 'Bluetooth permission needed';

  @override
  String get bluetoothUnknown => 'Bluetooth state unknown';

  @override
  String get dashStatus => 'Status';

  @override
  String get dashBleDevices => 'BLE devices';

  @override
  String get dashWifiNetworks => 'WiFi networks';

  @override
  String get dashRadio => 'Radio';

  @override
  String get dashScanHint =>
      'Tap Start to scan for nearby Bluetooth and WiFi devices.';

  @override
  String get radarEmpty => 'No devices in range yet.';

  @override
  String get radarSelf => 'You';

  @override
  String get radarLegendBle => 'Bluetooth device';

  @override
  String get radarLegendWifi => 'WiFi network';

  @override
  String get radarLegendRings => 'Rings = approximate signal distance (rough).';

  @override
  String get devicesEmpty => 'Nothing found yet — start a scan.';

  @override
  String get devicesSectionBle => 'Bluetooth';

  @override
  String get devicesSectionWifi => 'WiFi';

  @override
  String get deviceUnnamed => '(unnamed)';

  @override
  String rssiDbm(int rssi) {
    return '$rssi dBm';
  }

  @override
  String metersApprox(String meters) {
    return '≈ $meters m';
  }

  @override
  String get signalStrong => 'Strong';

  @override
  String get signalMedium => 'Medium';

  @override
  String get signalWeak => 'Weak';

  @override
  String get deviceDetailTitle => 'Device detail';

  @override
  String get deviceDetailId => 'Identifier';

  @override
  String get deviceDetailSignal => 'Signal';

  @override
  String get deviceDetailVendor => 'Vendor';

  @override
  String get deviceDetailVendorUnknown => 'Unknown vendor';

  @override
  String get deviceDetailServices => 'Services';

  @override
  String get deviceDetailFirstSeen => 'First seen';

  @override
  String get deviceDetailLastSeen => 'Last seen';

  @override
  String get deviceConnect => 'Connect';

  @override
  String get deviceDisconnect => 'Disconnect';

  @override
  String get deviceConnecting => 'Connecting…';

  @override
  String get deviceConnected => 'Connected';

  @override
  String get deviceConnectFailed => 'Connection failed';

  @override
  String get deviceGattServices => 'GATT services';

  @override
  String wifiChannel(int channel) {
    return 'ch $channel';
  }

  @override
  String get wifiCurrentOnly =>
      'On iOS only the connected network is visible — Apple blocks third-party AP scanning.';

  @override
  String get wifiThrottleNote =>
      'Android throttles WiFi scans, so results refresh slowly.';

  @override
  String wifiProxyConnected(String host) {
    return 'Using relay proxy: $host';
  }

  @override
  String get settingsAppearance => 'Appearance';

  @override
  String get settingsTheme => 'Theme';

  @override
  String get settingsFontScale => 'Text size';

  @override
  String get settingsReduceMotion => 'Reduce motion';

  @override
  String get settingsHighContrast => 'High contrast';

  @override
  String get settingsLanguage => 'Language';

  @override
  String get settingsLanguageSystem => 'System default';

  @override
  String get settingsPermissions => 'Permissions';

  @override
  String get settingsDiagnostics => 'Diagnostics';

  @override
  String get settingsDiagnosticsSub => 'Scan event log';

  @override
  String get settingsAbout => 'About';

  @override
  String get settingsScanner => 'Scanner';

  @override
  String get settingsScanWifi => 'Include WiFi in scans';

  @override
  String get settingsScanWifiSub =>
      'Android only; iOS shows the connected network.';

  @override
  String get settingsRssiFloor => 'Signal floor';

  @override
  String settingsRssiFloorValue(int dbm) {
    return 'Hide signals weaker than $dbm dBm';
  }

  @override
  String get settingsRssiFloorOff => 'Show all signals';

  @override
  String get settingsWifiProxy => 'WiFi relay proxy';

  @override
  String get settingsWifiProxySub =>
      'Discover a Mac/Linux proxy on your network to scan nearby WiFi (useful on iOS).';

  @override
  String get permBle => 'Bluetooth';

  @override
  String get permLocation => 'Location';

  @override
  String get permLocationWhy =>
      'Android requires location permission to scan for nearby devices.';

  @override
  String get permGranted => 'Granted';

  @override
  String get permDenied => 'Denied';

  @override
  String get permRequest => 'Request';

  @override
  String get permOpenSettings => 'Open settings';

  @override
  String get diagEmpty => 'No scan activity yet.';

  @override
  String get diagClear => 'Clear';

  @override
  String get diagCopy => 'Copy log';

  @override
  String get firstRunTitle => 'Welcome';

  @override
  String get firstRunBody =>
      'This app scans for nearby Bluetooth and WiFi devices. It needs a couple of permissions to do that.';

  @override
  String get firstRunPermBle =>
      'Bluetooth — to discover and connect to BLE devices.';

  @override
  String get firstRunPermLocation =>
      'Location — Android requires it for Bluetooth and WiFi scanning.';

  @override
  String get firstRunGrant => 'Grant permissions';

  @override
  String get firstRunSkip => 'Skip for now';

  @override
  String get langEnglish => 'English';

  @override
  String get langJapanese => '日本語';
}
