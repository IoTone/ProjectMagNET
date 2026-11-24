// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

/// Native (TCP) MQTT client for `mqtt://host:port` / `mqtts://host:port`.
MqttClient makeSentinelMqttClient(Uri broker, String clientId) {
  final bool tls = broker.scheme == 'mqtts';
  final MqttServerClient c = MqttServerClient.withPort(
      broker.host, clientId, broker.hasPort ? broker.port : (tls ? 8883 : 1883));
  c.secure = tls;
  return c;
}
