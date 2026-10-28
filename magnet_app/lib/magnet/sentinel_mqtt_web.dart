// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:mqtt_client/mqtt_browser_client.dart';
import 'package:mqtt_client/mqtt_client.dart';

/// Browser MQTT client. Browsers can only speak MQTT over WebSockets, so a
/// plain `mqtt://` broker is mapped to its secure WebSocket listener using
/// HiveMQ's convention (`wss://host:8884/mqtt`); `ws://` / `wss://` URIs
/// are used as given.
MqttClient makeSentinelMqttClient(Uri broker, String clientId) {
  final bool ws = broker.scheme == 'ws' || broker.scheme == 'wss';
  final String scheme = ws ? broker.scheme : 'wss';
  final int port = ws && broker.hasPort ? broker.port : 8884;
  final String path = ws && broker.path.isNotEmpty ? broker.path : '/mqtt';
  return MqttBrowserClient.withPort('$scheme://${broker.host}$path', clientId, port);
}
