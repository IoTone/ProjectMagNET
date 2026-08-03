import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:intl/intl.dart' as intl;

import 'app_localizations_en.dart';
import 'app_localizations_ja.dart';

// ignore_for_file: type=lint

/// Callers can lookup localized strings with an instance of AppLocalizations
/// returned by `AppLocalizations.of(context)`.
///
/// Applications need to include `AppLocalizations.delegate()` in their app's
/// `localizationDelegates` list, and the locales they support in the app's
/// `supportedLocales` list. For example:
///
/// ```dart
/// import 'gen/app_localizations.dart';
///
/// return MaterialApp(
///   localizationsDelegates: AppLocalizations.localizationsDelegates,
///   supportedLocales: AppLocalizations.supportedLocales,
///   home: MyApplicationHome(),
/// );
/// ```
///
/// ## Update pubspec.yaml
///
/// Please make sure to update your pubspec.yaml to include the following
/// packages:
///
/// ```yaml
/// dependencies:
///   # Internationalization support.
///   flutter_localizations:
///     sdk: flutter
///   intl: any # Use the pinned version from flutter_localizations
///
///   # Rest of dependencies
/// ```
///
/// ## iOS Applications
///
/// iOS applications define key application metadata, including supported
/// locales, in an Info.plist file that is built into the application bundle.
/// To configure the locales supported by your app, you’ll need to edit this
/// file.
///
/// First, open your project’s ios/Runner.xcworkspace Xcode workspace file.
/// Then, in the Project Navigator, open the Info.plist file under the Runner
/// project’s Runner folder.
///
/// Next, select the Information Property List item, select Add Item from the
/// Editor menu, then select Localizations from the pop-up menu.
///
/// Select and expand the newly-created Localizations item then, for each
/// locale your application supports, add a new item and select the locale
/// you wish to add from the pop-up menu in the Value field. This list should
/// be consistent with the languages listed in the AppLocalizations.supportedLocales
/// property.
abstract class AppLocalizations {
  AppLocalizations(String locale)
      : localeName = intl.Intl.canonicalizedLocale(locale.toString());

  final String localeName;

  static AppLocalizations of(BuildContext context) {
    return Localizations.of<AppLocalizations>(context, AppLocalizations)!;
  }

  static const LocalizationsDelegate<AppLocalizations> delegate =
      _AppLocalizationsDelegate();

  /// A list of this localizations delegate along with the default localizations
  /// delegates.
  ///
  /// Returns a list of localizations delegates containing this delegate along with
  /// GlobalMaterialLocalizations.delegate, GlobalCupertinoLocalizations.delegate,
  /// and GlobalWidgetsLocalizations.delegate.
  ///
  /// Additional delegates can be added by appending to this list in
  /// MaterialApp. This list does not have to be used at all if a custom list
  /// of delegates is preferred or required.
  static const List<LocalizationsDelegate<dynamic>> localizationsDelegates =
      <LocalizationsDelegate<dynamic>>[
    delegate,
    GlobalMaterialLocalizations.delegate,
    GlobalCupertinoLocalizations.delegate,
    GlobalWidgetsLocalizations.delegate,
  ];

  /// A list of this localizations delegate's supported locales.
  static const List<Locale> supportedLocales = <Locale>[
    Locale('en'),
    Locale('ja')
  ];

  /// No description provided for @appTitle.
  ///
  /// In en, this message translates to:
  /// **'MagNET'**
  String get appTitle;

  /// No description provided for @aboutSubtitle.
  ///
  /// In en, this message translates to:
  /// **'Hanasu mesh configurator & field console'**
  String get aboutSubtitle;

  /// No description provided for @aboutVersion.
  ///
  /// In en, this message translates to:
  /// **'Version {version}'**
  String aboutVersion(String version);

  /// No description provided for @aboutDescription.
  ///
  /// In en, this message translates to:
  /// **'Provision, watch, and talk to a MagNET Hanasu Thread mesh over BLE — the thing you hold while standing next to hardware.'**
  String get aboutDescription;

  /// No description provided for @aboutCopyright.
  ///
  /// In en, this message translates to:
  /// **'© 2024–2026 IoTone, Inc.'**
  String get aboutCopyright;

  /// No description provided for @aboutLicense.
  ///
  /// In en, this message translates to:
  /// **'MIT License'**
  String get aboutLicense;

  /// No description provided for @aboutProtocol.
  ///
  /// In en, this message translates to:
  /// **'MagNET HCP protocol {proto}'**
  String aboutProtocol(String proto);

  /// No description provided for @aboutLicensesButton.
  ///
  /// In en, this message translates to:
  /// **'Open-source licenses'**
  String get aboutLicensesButton;

  /// No description provided for @aboutMadeWith.
  ///
  /// In en, this message translates to:
  /// **'Built with Flutter.'**
  String get aboutMadeWith;

  /// No description provided for @tabDashboard.
  ///
  /// In en, this message translates to:
  /// **'Dashboard'**
  String get tabDashboard;

  /// No description provided for @tabRadar.
  ///
  /// In en, this message translates to:
  /// **'Radar'**
  String get tabRadar;

  /// No description provided for @tabDevices.
  ///
  /// In en, this message translates to:
  /// **'Devices'**
  String get tabDevices;

  /// No description provided for @tabSettings.
  ///
  /// In en, this message translates to:
  /// **'Settings'**
  String get tabSettings;

  /// No description provided for @quickNav.
  ///
  /// In en, this message translates to:
  /// **'Quick navigation'**
  String get quickNav;

  /// No description provided for @commonCancel.
  ///
  /// In en, this message translates to:
  /// **'Cancel'**
  String get commonCancel;

  /// No description provided for @commonClose.
  ///
  /// In en, this message translates to:
  /// **'Close'**
  String get commonClose;

  /// No description provided for @commonRetry.
  ///
  /// In en, this message translates to:
  /// **'Retry'**
  String get commonRetry;

  /// No description provided for @commonCopy.
  ///
  /// In en, this message translates to:
  /// **'Copy'**
  String get commonCopy;

  /// No description provided for @commonCopied.
  ///
  /// In en, this message translates to:
  /// **'Copied'**
  String get commonCopied;

  /// No description provided for @scanStart.
  ///
  /// In en, this message translates to:
  /// **'Start scan'**
  String get scanStart;

  /// No description provided for @scanStop.
  ///
  /// In en, this message translates to:
  /// **'Stop scan'**
  String get scanStop;

  /// No description provided for @scanScanning.
  ///
  /// In en, this message translates to:
  /// **'Scanning…'**
  String get scanScanning;

  /// No description provided for @scanIdle.
  ///
  /// In en, this message translates to:
  /// **'Idle'**
  String get scanIdle;

  /// No description provided for @bluetoothOn.
  ///
  /// In en, this message translates to:
  /// **'Bluetooth ready'**
  String get bluetoothOn;

  /// No description provided for @bluetoothOff.
  ///
  /// In en, this message translates to:
  /// **'Bluetooth is off'**
  String get bluetoothOff;

  /// No description provided for @bluetoothUnauthorized.
  ///
  /// In en, this message translates to:
  /// **'Bluetooth permission needed'**
  String get bluetoothUnauthorized;

  /// No description provided for @bluetoothUnknown.
  ///
  /// In en, this message translates to:
  /// **'Bluetooth state unknown'**
  String get bluetoothUnknown;

  /// No description provided for @dashStatus.
  ///
  /// In en, this message translates to:
  /// **'Status'**
  String get dashStatus;

  /// No description provided for @dashBleDevices.
  ///
  /// In en, this message translates to:
  /// **'BLE devices'**
  String get dashBleDevices;

  /// No description provided for @dashWifiNetworks.
  ///
  /// In en, this message translates to:
  /// **'WiFi networks'**
  String get dashWifiNetworks;

  /// No description provided for @dashRadio.
  ///
  /// In en, this message translates to:
  /// **'Radio'**
  String get dashRadio;

  /// No description provided for @dashScanHint.
  ///
  /// In en, this message translates to:
  /// **'Tap Start to scan for nearby Bluetooth and WiFi devices.'**
  String get dashScanHint;

  /// No description provided for @radarEmpty.
  ///
  /// In en, this message translates to:
  /// **'No devices in range yet.'**
  String get radarEmpty;

  /// No description provided for @radarSelf.
  ///
  /// In en, this message translates to:
  /// **'You'**
  String get radarSelf;

  /// No description provided for @radarLegendBle.
  ///
  /// In en, this message translates to:
  /// **'Bluetooth device'**
  String get radarLegendBle;

  /// No description provided for @radarLegendWifi.
  ///
  /// In en, this message translates to:
  /// **'WiFi network'**
  String get radarLegendWifi;

  /// No description provided for @radarLegendMagnet.
  ///
  /// In en, this message translates to:
  /// **'MagNET node'**
  String get radarLegendMagnet;

  /// No description provided for @radarLegendCompanion.
  ///
  /// In en, this message translates to:
  /// **'Companion (mesh window)'**
  String get radarLegendCompanion;

  /// No description provided for @radarLegendRings.
  ///
  /// In en, this message translates to:
  /// **'Rings = approximate signal distance (rough).'**
  String get radarLegendRings;

  /// No description provided for @devicesEmpty.
  ///
  /// In en, this message translates to:
  /// **'Nothing found yet — start a scan.'**
  String get devicesEmpty;

  /// No description provided for @devicesSectionBle.
  ///
  /// In en, this message translates to:
  /// **'Bluetooth'**
  String get devicesSectionBle;

  /// No description provided for @devicesSectionWifi.
  ///
  /// In en, this message translates to:
  /// **'WiFi'**
  String get devicesSectionWifi;

  /// No description provided for @deviceUnnamed.
  ///
  /// In en, this message translates to:
  /// **'(unnamed)'**
  String get deviceUnnamed;

  /// No description provided for @rssiDbm.
  ///
  /// In en, this message translates to:
  /// **'{rssi} dBm'**
  String rssiDbm(int rssi);

  /// No description provided for @metersApprox.
  ///
  /// In en, this message translates to:
  /// **'≈ {meters} m'**
  String metersApprox(String meters);

  /// No description provided for @signalStrong.
  ///
  /// In en, this message translates to:
  /// **'Strong'**
  String get signalStrong;

  /// No description provided for @signalMedium.
  ///
  /// In en, this message translates to:
  /// **'Medium'**
  String get signalMedium;

  /// No description provided for @signalWeak.
  ///
  /// In en, this message translates to:
  /// **'Weak'**
  String get signalWeak;

  /// No description provided for @deviceDetailTitle.
  ///
  /// In en, this message translates to:
  /// **'Device detail'**
  String get deviceDetailTitle;

  /// No description provided for @deviceDetailId.
  ///
  /// In en, this message translates to:
  /// **'Identifier'**
  String get deviceDetailId;

  /// No description provided for @deviceDetailSignal.
  ///
  /// In en, this message translates to:
  /// **'Signal'**
  String get deviceDetailSignal;

  /// No description provided for @deviceDetailVendor.
  ///
  /// In en, this message translates to:
  /// **'Vendor'**
  String get deviceDetailVendor;

  /// No description provided for @deviceDetailVendorUnknown.
  ///
  /// In en, this message translates to:
  /// **'Unknown vendor'**
  String get deviceDetailVendorUnknown;

  /// No description provided for @deviceDetailServices.
  ///
  /// In en, this message translates to:
  /// **'Services'**
  String get deviceDetailServices;

  /// No description provided for @deviceDetailFirstSeen.
  ///
  /// In en, this message translates to:
  /// **'First seen'**
  String get deviceDetailFirstSeen;

  /// No description provided for @deviceDetailLastSeen.
  ///
  /// In en, this message translates to:
  /// **'Last seen'**
  String get deviceDetailLastSeen;

  /// No description provided for @deviceConnect.
  ///
  /// In en, this message translates to:
  /// **'Connect'**
  String get deviceConnect;

  /// No description provided for @deviceDisconnect.
  ///
  /// In en, this message translates to:
  /// **'Disconnect'**
  String get deviceDisconnect;

  /// No description provided for @deviceConnecting.
  ///
  /// In en, this message translates to:
  /// **'Connecting…'**
  String get deviceConnecting;

  /// No description provided for @deviceConnected.
  ///
  /// In en, this message translates to:
  /// **'Connected'**
  String get deviceConnected;

  /// No description provided for @deviceConnectFailed.
  ///
  /// In en, this message translates to:
  /// **'Connection failed'**
  String get deviceConnectFailed;

  /// No description provided for @deviceGattServices.
  ///
  /// In en, this message translates to:
  /// **'GATT services'**
  String get deviceGattServices;

  /// No description provided for @wifiChannel.
  ///
  /// In en, this message translates to:
  /// **'ch {channel}'**
  String wifiChannel(int channel);

  /// No description provided for @wifiCurrentOnly.
  ///
  /// In en, this message translates to:
  /// **'On iOS only the connected network is visible — Apple blocks third-party AP scanning.'**
  String get wifiCurrentOnly;

  /// No description provided for @wifiThrottleNote.
  ///
  /// In en, this message translates to:
  /// **'Android throttles WiFi scans, so results refresh slowly.'**
  String get wifiThrottleNote;

  /// No description provided for @wifiProxyConnected.
  ///
  /// In en, this message translates to:
  /// **'Using relay proxy: {host}'**
  String wifiProxyConnected(String host);

  /// No description provided for @settingsAppearance.
  ///
  /// In en, this message translates to:
  /// **'Appearance'**
  String get settingsAppearance;

  /// No description provided for @settingsTheme.
  ///
  /// In en, this message translates to:
  /// **'Theme'**
  String get settingsTheme;

  /// No description provided for @settingsFontScale.
  ///
  /// In en, this message translates to:
  /// **'Text size'**
  String get settingsFontScale;

  /// No description provided for @settingsReduceMotion.
  ///
  /// In en, this message translates to:
  /// **'Reduce motion'**
  String get settingsReduceMotion;

  /// No description provided for @settingsHighContrast.
  ///
  /// In en, this message translates to:
  /// **'High contrast'**
  String get settingsHighContrast;

  /// No description provided for @settingsLanguage.
  ///
  /// In en, this message translates to:
  /// **'Language'**
  String get settingsLanguage;

  /// No description provided for @settingsLanguageSystem.
  ///
  /// In en, this message translates to:
  /// **'System default'**
  String get settingsLanguageSystem;

  /// No description provided for @settingsPermissions.
  ///
  /// In en, this message translates to:
  /// **'Permissions'**
  String get settingsPermissions;

  /// No description provided for @settingsDiagnostics.
  ///
  /// In en, this message translates to:
  /// **'Diagnostics'**
  String get settingsDiagnostics;

  /// No description provided for @settingsDiagnosticsSub.
  ///
  /// In en, this message translates to:
  /// **'Scan event log'**
  String get settingsDiagnosticsSub;

  /// No description provided for @settingsAbout.
  ///
  /// In en, this message translates to:
  /// **'About'**
  String get settingsAbout;

  /// No description provided for @settingsScanner.
  ///
  /// In en, this message translates to:
  /// **'Scanner'**
  String get settingsScanner;

  /// No description provided for @settingsScanWifi.
  ///
  /// In en, this message translates to:
  /// **'Include WiFi in scans'**
  String get settingsScanWifi;

  /// No description provided for @settingsScanWifiSub.
  ///
  /// In en, this message translates to:
  /// **'Android only; iOS shows the connected network.'**
  String get settingsScanWifiSub;

  /// No description provided for @settingsRssiFloor.
  ///
  /// In en, this message translates to:
  /// **'Signal floor'**
  String get settingsRssiFloor;

  /// No description provided for @settingsRssiFloorValue.
  ///
  /// In en, this message translates to:
  /// **'Hide signals weaker than {dbm} dBm'**
  String settingsRssiFloorValue(int dbm);

  /// No description provided for @settingsRssiFloorOff.
  ///
  /// In en, this message translates to:
  /// **'Show all signals'**
  String get settingsRssiFloorOff;

  /// No description provided for @settingsWifiProxy.
  ///
  /// In en, this message translates to:
  /// **'WiFi relay proxy'**
  String get settingsWifiProxy;

  /// No description provided for @settingsWifiProxySub.
  ///
  /// In en, this message translates to:
  /// **'Discover a Mac/Linux proxy on your network to scan nearby WiFi (useful on iOS).'**
  String get settingsWifiProxySub;

  /// No description provided for @permBle.
  ///
  /// In en, this message translates to:
  /// **'Bluetooth'**
  String get permBle;

  /// No description provided for @permLocation.
  ///
  /// In en, this message translates to:
  /// **'Location'**
  String get permLocation;

  /// No description provided for @permLocationWhy.
  ///
  /// In en, this message translates to:
  /// **'Android requires location permission to scan for nearby devices.'**
  String get permLocationWhy;

  /// No description provided for @permGranted.
  ///
  /// In en, this message translates to:
  /// **'Granted'**
  String get permGranted;

  /// No description provided for @permDenied.
  ///
  /// In en, this message translates to:
  /// **'Denied'**
  String get permDenied;

  /// No description provided for @permRequest.
  ///
  /// In en, this message translates to:
  /// **'Request'**
  String get permRequest;

  /// No description provided for @permOpenSettings.
  ///
  /// In en, this message translates to:
  /// **'Open settings'**
  String get permOpenSettings;

  /// No description provided for @diagEmpty.
  ///
  /// In en, this message translates to:
  /// **'No scan activity yet.'**
  String get diagEmpty;

  /// No description provided for @diagClear.
  ///
  /// In en, this message translates to:
  /// **'Clear'**
  String get diagClear;

  /// No description provided for @diagCopy.
  ///
  /// In en, this message translates to:
  /// **'Copy log'**
  String get diagCopy;

  /// No description provided for @firstRunTitle.
  ///
  /// In en, this message translates to:
  /// **'Welcome'**
  String get firstRunTitle;

  /// No description provided for @firstRunBody.
  ///
  /// In en, this message translates to:
  /// **'This app scans for nearby Bluetooth and WiFi devices. It needs a couple of permissions to do that.'**
  String get firstRunBody;

  /// No description provided for @firstRunPermBle.
  ///
  /// In en, this message translates to:
  /// **'Bluetooth — to discover and connect to BLE devices.'**
  String get firstRunPermBle;

  /// No description provided for @firstRunPermLocation.
  ///
  /// In en, this message translates to:
  /// **'Location — Android requires it for Bluetooth and WiFi scanning.'**
  String get firstRunPermLocation;

  /// No description provided for @firstRunGrant.
  ///
  /// In en, this message translates to:
  /// **'Grant permissions'**
  String get firstRunGrant;

  /// No description provided for @firstRunSkip.
  ///
  /// In en, this message translates to:
  /// **'Skip for now'**
  String get firstRunSkip;

  /// No description provided for @langEnglish.
  ///
  /// In en, this message translates to:
  /// **'English'**
  String get langEnglish;

  /// No description provided for @langJapanese.
  ///
  /// In en, this message translates to:
  /// **'日本語'**
  String get langJapanese;

  /// No description provided for @meshTitle.
  ///
  /// In en, this message translates to:
  /// **'Mesh'**
  String get meshTitle;

  /// No description provided for @meshDashboardCard.
  ///
  /// In en, this message translates to:
  /// **'Live mesh'**
  String get meshDashboardCard;

  /// No description provided for @meshDashboardHint.
  ///
  /// In en, this message translates to:
  /// **'Watch and talk to the mesh through the companion node'**
  String get meshDashboardHint;

  /// No description provided for @meshTabFeed.
  ///
  /// In en, this message translates to:
  /// **'Feed'**
  String get meshTabFeed;

  /// No description provided for @meshTabPeers.
  ///
  /// In en, this message translates to:
  /// **'Peers'**
  String get meshTabPeers;

  /// No description provided for @meshTabTopology.
  ///
  /// In en, this message translates to:
  /// **'Topology'**
  String get meshTabTopology;

  /// No description provided for @meshTabTest.
  ///
  /// In en, this message translates to:
  /// **'Test'**
  String get meshTabTest;

  /// No description provided for @meshPickTitle.
  ///
  /// In en, this message translates to:
  /// **'Choose a companion node'**
  String get meshPickTitle;

  /// No description provided for @meshPickHint.
  ///
  /// In en, this message translates to:
  /// **'The companion node keeps BLE on and acts as this phone\'s window into the mesh. Pick the node flashed with the resident build.'**
  String get meshPickHint;

  /// No description provided for @meshPickScan.
  ///
  /// In en, this message translates to:
  /// **'Scan for nodes'**
  String get meshPickScan;

  /// No description provided for @meshPickScanning.
  ///
  /// In en, this message translates to:
  /// **'Scanning…'**
  String get meshPickScanning;

  /// No description provided for @meshPickEmpty.
  ///
  /// In en, this message translates to:
  /// **'No MagNET nodes found. Is the companion powered and in range?'**
  String get meshPickEmpty;

  /// No description provided for @meshStateConnecting.
  ///
  /// In en, this message translates to:
  /// **'Connecting…'**
  String get meshStateConnecting;

  /// No description provided for @meshStateConnected.
  ///
  /// In en, this message translates to:
  /// **'Live'**
  String get meshStateConnected;

  /// No description provided for @meshStateDisconnected.
  ///
  /// In en, this message translates to:
  /// **'Disconnected'**
  String get meshStateDisconnected;

  /// No description provided for @meshStateError.
  ///
  /// In en, this message translates to:
  /// **'Connection lost'**
  String get meshStateError;

  /// No description provided for @meshReconnect.
  ///
  /// In en, this message translates to:
  /// **'Reconnect'**
  String get meshReconnect;

  /// No description provided for @meshChangeCompanion.
  ///
  /// In en, this message translates to:
  /// **'Change companion node'**
  String get meshChangeCompanion;

  /// No description provided for @meshComposerHint.
  ///
  /// In en, this message translates to:
  /// **'Message the channel…'**
  String get meshComposerHint;

  /// No description provided for @meshSend.
  ///
  /// In en, this message translates to:
  /// **'Send'**
  String get meshSend;

  /// No description provided for @meshFeedEmpty.
  ///
  /// In en, this message translates to:
  /// **'Nothing yet — traffic will appear here as it happens.'**
  String get meshFeedEmpty;

  /// No description provided for @meshNoPeers.
  ///
  /// In en, this message translates to:
  /// **'No peers seen yet.'**
  String get meshNoPeers;

  /// No description provided for @meshPeerLastSeen.
  ///
  /// In en, this message translates to:
  /// **'last seen {ago}s ago'**
  String meshPeerLastSeen(String ago);

  /// No description provided for @meshNeighbors.
  ///
  /// In en, this message translates to:
  /// **'Neighbors'**
  String get meshNeighbors;

  /// No description provided for @meshNoNeighbors.
  ///
  /// In en, this message translates to:
  /// **'No neighbors reported.'**
  String get meshNoNeighbors;

  /// No description provided for @meshRefresh.
  ///
  /// In en, this message translates to:
  /// **'Refresh'**
  String get meshRefresh;

  /// No description provided for @provisionLogTitle.
  ///
  /// In en, this message translates to:
  /// **'Provisioned nodes'**
  String get provisionLogTitle;

  /// No description provided for @provisionLogSub.
  ///
  /// In en, this message translates to:
  /// **'Every node this phone has put on a channel'**
  String get provisionLogSub;

  /// No description provided for @provisionLogEmpty.
  ///
  /// In en, this message translates to:
  /// **'Nothing provisioned from this phone yet. Nodes appear here the moment you give them a channel.'**
  String get provisionLogEmpty;
}

class _AppLocalizationsDelegate
    extends LocalizationsDelegate<AppLocalizations> {
  const _AppLocalizationsDelegate();

  @override
  Future<AppLocalizations> load(Locale locale) {
    return SynchronousFuture<AppLocalizations>(lookupAppLocalizations(locale));
  }

  @override
  bool isSupported(Locale locale) =>
      <String>['en', 'ja'].contains(locale.languageCode);

  @override
  bool shouldReload(_AppLocalizationsDelegate old) => false;
}

AppLocalizations lookupAppLocalizations(Locale locale) {
  // Lookup logic when only language code is specified.
  switch (locale.languageCode) {
    case 'en':
      return AppLocalizationsEn();
    case 'ja':
      return AppLocalizationsJa();
  }

  throw FlutterError(
      'AppLocalizations.delegate failed to load unsupported locale "$locale". This is likely '
      'an issue with the localizations generation tool. Please file an issue '
      'on GitHub with a reproducible sample app and the gen-l10n configuration '
      'that was used.');
}
