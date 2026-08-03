// ignore: unused_import
import 'package:intl/intl.dart' as intl;
import 'app_localizations.dart';

// ignore_for_file: type=lint

/// The translations for Japanese (`ja`).
class AppLocalizationsJa extends AppLocalizations {
  AppLocalizationsJa([String locale = 'ja']) : super(locale);

  @override
  String get appTitle => 'MagNET';

  @override
  String get aboutSubtitle => 'Hanasu メッシュ設定・フィールドコンソール';

  @override
  String aboutVersion(String version) {
    return 'バージョン $version';
  }

  @override
  String get aboutDescription =>
      'BLE 経由で MagNET Hanasu Thread メッシュをプロビジョニングし、観察し、会話する — ハードウェアのそばで手に持つツール。';

  @override
  String get aboutCopyright => '© 2024–2026 IoTone, Inc.';

  @override
  String get aboutLicense => 'MIT ライセンス';

  @override
  String aboutProtocol(String proto) {
    return 'MagNET HCP プロトコル $proto';
  }

  @override
  String get aboutLicensesButton => 'オープンソースライセンス';

  @override
  String get aboutMadeWith => 'Flutter で構築。';

  @override
  String get tabDashboard => 'ダッシュボード';

  @override
  String get tabRadar => 'レーダー';

  @override
  String get tabDevices => 'デバイス';

  @override
  String get tabSettings => '設定';

  @override
  String get quickNav => 'クイックナビ';

  @override
  String get commonCancel => 'キャンセル';

  @override
  String get commonClose => '閉じる';

  @override
  String get commonRetry => '再試行';

  @override
  String get commonCopy => 'コピー';

  @override
  String get commonCopied => 'コピーしました';

  @override
  String get scanStart => 'スキャン開始';

  @override
  String get scanStop => 'スキャン停止';

  @override
  String get scanScanning => 'スキャン中…';

  @override
  String get scanIdle => '待機中';

  @override
  String get bluetoothOn => 'Bluetooth 準備完了';

  @override
  String get bluetoothOff => 'Bluetooth がオフです';

  @override
  String get bluetoothUnauthorized => 'Bluetooth の許可が必要です';

  @override
  String get bluetoothUnknown => 'Bluetooth の状態は不明です';

  @override
  String get dashStatus => '状態';

  @override
  String get dashBleDevices => 'BLE デバイス';

  @override
  String get dashWifiNetworks => 'WiFi ネットワーク';

  @override
  String get dashRadio => '無線';

  @override
  String get dashScanHint => '「開始」をタップして近くの Bluetooth・WiFi をスキャンします。';

  @override
  String get radarEmpty => '範囲内にデバイスがありません。';

  @override
  String get radarSelf => '自分';

  @override
  String get radarLegendBle => 'Bluetooth デバイス';

  @override
  String get radarLegendWifi => 'WiFi ネットワーク';

  @override
  String get radarLegendMagnet => 'MagNET ノード';

  @override
  String get radarLegendCompanion => 'コンパニオン（メッシュ窓口）';

  @override
  String get radarLegendRings => 'リング = おおよその信号距離（目安）。';

  @override
  String get devicesEmpty => 'まだ何も見つかりません — スキャンを開始してください。';

  @override
  String get devicesSectionBle => 'Bluetooth';

  @override
  String get devicesSectionWifi => 'WiFi';

  @override
  String get deviceUnnamed => '(名前なし)';

  @override
  String rssiDbm(int rssi) {
    return '$rssi dBm';
  }

  @override
  String metersApprox(String meters) {
    return '≈ $meters m';
  }

  @override
  String get signalStrong => '強';

  @override
  String get signalMedium => '中';

  @override
  String get signalWeak => '弱';

  @override
  String get deviceDetailTitle => 'デバイス詳細';

  @override
  String get deviceDetailId => '識別子';

  @override
  String get deviceDetailSignal => '信号';

  @override
  String get deviceDetailVendor => 'ベンダー';

  @override
  String get deviceDetailVendorUnknown => '不明なベンダー';

  @override
  String get deviceDetailServices => 'サービス';

  @override
  String get deviceDetailFirstSeen => '初回検出';

  @override
  String get deviceDetailLastSeen => '最終検出';

  @override
  String get deviceConnect => '接続';

  @override
  String get deviceDisconnect => '切断';

  @override
  String get deviceConnecting => '接続中…';

  @override
  String get deviceConnected => '接続済み';

  @override
  String get deviceConnectFailed => '接続に失敗しました';

  @override
  String get deviceGattServices => 'GATT サービス';

  @override
  String wifiChannel(int channel) {
    return 'ch $channel';
  }

  @override
  String get wifiCurrentOnly =>
      'iOS では接続中のネットワークのみ表示されます — Apple がサードパーティの AP スキャンを制限しています。';

  @override
  String get wifiThrottleNote => 'Android は WiFi スキャンを制限するため、更新は遅くなります。';

  @override
  String wifiProxyConnected(String host) {
    return '中継プロキシを使用中: $host';
  }

  @override
  String get settingsAppearance => '外観';

  @override
  String get settingsTheme => 'テーマ';

  @override
  String get settingsFontScale => '文字サイズ';

  @override
  String get settingsReduceMotion => '動きを減らす';

  @override
  String get settingsHighContrast => 'ハイコントラスト';

  @override
  String get settingsLanguage => '言語';

  @override
  String get settingsLanguageSystem => 'システム既定';

  @override
  String get settingsPermissions => '権限';

  @override
  String get settingsDiagnostics => '診断';

  @override
  String get settingsDiagnosticsSub => 'スキャンイベントログ';

  @override
  String get settingsAbout => '情報';

  @override
  String get settingsScanner => 'スキャナー';

  @override
  String get settingsScanWifi => 'スキャンに WiFi を含める';

  @override
  String get settingsScanWifiSub => 'Android のみ。iOS は接続中のネットワークを表示します。';

  @override
  String get settingsRssiFloor => '信号フロア';

  @override
  String settingsRssiFloorValue(int dbm) {
    return '$dbm dBm より弱い信号を非表示';
  }

  @override
  String get settingsRssiFloorOff => 'すべての信号を表示';

  @override
  String get settingsWifiProxy => 'WiFi 中継プロキシ';

  @override
  String get settingsWifiProxySub =>
      'ネットワーク上の Mac/Linux プロキシを検出して近くの WiFi をスキャン（iOS で有用）。';

  @override
  String get permBle => 'Bluetooth';

  @override
  String get permLocation => '位置情報';

  @override
  String get permLocationWhy => 'Android では近くのデバイスのスキャンに位置情報の許可が必要です。';

  @override
  String get permGranted => '許可済み';

  @override
  String get permDenied => '拒否';

  @override
  String get permRequest => 'リクエスト';

  @override
  String get permOpenSettings => '設定を開く';

  @override
  String get diagEmpty => 'スキャン活動はまだありません。';

  @override
  String get diagClear => 'クリア';

  @override
  String get diagCopy => 'ログをコピー';

  @override
  String get firstRunTitle => 'ようこそ';

  @override
  String get firstRunBody =>
      'このアプリは近くの Bluetooth・WiFi デバイスをスキャンします。そのためにいくつかの権限が必要です。';

  @override
  String get firstRunPermBle => 'Bluetooth — BLE デバイスの検出と接続のため。';

  @override
  String get firstRunPermLocation =>
      '位置情報 — Android では Bluetooth・WiFi スキャンに必要です。';

  @override
  String get firstRunGrant => '権限を許可';

  @override
  String get firstRunSkip => '後で';

  @override
  String get langEnglish => 'English';

  @override
  String get langJapanese => '日本語';

  @override
  String get meshTitle => 'メッシュ';

  @override
  String get meshDashboardCard => 'ライブメッシュ';

  @override
  String get meshDashboardHint => 'コンパニオンノード経由でメッシュを見て話す';

  @override
  String get meshTabFeed => 'フィード';

  @override
  String get meshTabPeers => 'ピア';

  @override
  String get meshTabTopology => 'トポロジー';

  @override
  String get meshTabTest => 'テスト';

  @override
  String get meshPickTitle => 'コンパニオンノードを選択';

  @override
  String get meshPickHint =>
      'コンパニオンノードはBLEを維持し、このスマートフォンのメッシュへの窓口になります。レジデントビルドを書き込んだノードを選んでください。';

  @override
  String get meshPickScan => 'ノードをスキャン';

  @override
  String get meshPickScanning => 'スキャン中…';

  @override
  String get meshPickEmpty => 'MagNETノードが見つかりません。コンパニオンの電源と距離を確認してください。';

  @override
  String get meshStateConnecting => '接続中…';

  @override
  String get meshStateConnected => 'ライブ';

  @override
  String get meshStateDisconnected => '切断';

  @override
  String get meshStateError => '接続が切れました';

  @override
  String get meshReconnect => '再接続';

  @override
  String get meshChangeCompanion => 'コンパニオンノードを変更';

  @override
  String get meshComposerHint => 'チャンネルへメッセージ…';

  @override
  String get meshSend => '送信';

  @override
  String get meshFeedEmpty => 'まだ何もありません — トラフィックが発生するとここに表示されます。';

  @override
  String get meshNoPeers => 'ピアはまだ見つかっていません。';

  @override
  String meshPeerLastSeen(String ago) {
    return '$ago秒前に確認';
  }

  @override
  String get meshNeighbors => '近隣ノード';

  @override
  String get meshNoNeighbors => '近隣ノードは報告されていません。';

  @override
  String get meshRefresh => '更新';

  @override
  String get provisionLogTitle => 'プロビジョニング済みノード';

  @override
  String get provisionLogSub => 'このスマートフォンからチャンネルに参加させたノード';

  @override
  String get provisionLogEmpty =>
      'まだこのスマートフォンからプロビジョニングしていません。ノードにチャンネルを設定するとここに表示されます。';
}
