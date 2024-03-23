import 'dart:io' show Platform, sleep;
// import 'dart:js_interop';
import 'dart:math';
import 'dart:async';

import 'package:flutter_neumorphic/flutter_neumorphic.dart';
import 'package:provider/provider.dart';

import 'package:magnet_app/provider/ble_provider.dart';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
// import 'package:location_permissions/location_permissions.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:cbl/cbl.dart';
import 'package:observable_ish/observable_ish.dart';


/**
 * 
 *
 */
class AppState extends BLEProvider {
  //Application States

  bool treat_state = false;
  bool isPaused = false;
  String? app_screen = "splash";
  String? prev_screen = null;
  bool timerStatus = false;
  AsyncDatabase? _db;

  RxList<Map> _devices = RxList<Map>(); /* = [

  ]; */
  
  Timer? btnStatusTimer;
  

  bool getButtonState() {
    return treat_state;
  }

  String? getCurrentScreen() {
    return app_screen;
  }

  //BLE getters
  bool getDeviceConnectionStatus() {
    return connected;
  }

  bool getBleIsScanning() {
    return scanStarted;
  }

  Future initDB() async {
    _db = await Database.openAsync('magnetdb-schema1');
  }

  AppState() {
    Timer(const Duration(seconds: 5), () {
      // scanAndConnectToDevice();
      print("Move to home screen");
      app_screen = "home";
      notifyListeners();
    });
    
  }

  //Scan And Connect Methods
  void startScan() async {
    if (_db == null) {
      initDB();
    }
    bool permGranted = false;
    scanStarted = true;
    notifyListeners();

    PermissionStatus permission;

    if (Platform.isIOS) {
      permGranted = true;
    } else if (Platform.isAndroid) {
      // permission = await LocationPermissions().requestPermissions();
      // if (permission == PermissionStatus.granted) permGranted = true;
      // final ph = Permission.byValue(Permission.location.value);
      // final requested = await ph.request();
      // final alwaysGranted = requested[PermissionGroup.locationAlways] == PermissionStatus.granted;
      // final whenInUseGranted = requested[PermissionGroup.locationWhenInUse] == PermissionStatus.granted;
      // permGranted = requested.isGranted;

      // [Permission.bluetooth, Permission.bluetoothScan, Permission.bluetoothConnect].request().then((status) async {
      [Permission.bluetoothScan, Permission.bluetoothConnect]
          .request()
          .then((status) async {
        status.forEach((key, status) async {
          if (status.isDenied ||
              status.isPermanentlyDenied ||
              status.isLimited ||
              status.isRestricted) {
            // if (status != null && status) {
            openAppSettings();
            // }
          }
        });
      });
      permGranted = true;
    }

    //Scanning Logic
    if (permGranted) {
      print("Scanning ...");
      scanDeviceStream = ble.scanForDevices(
          withServices: [], // [serviceUuid]
          scanMode: ScanMode.lowLatency).listen((device) async {
        print("examining new device");
        print("---------------------");
        
        if (device.id == deviceId) {
          print("--------------> Found match!!!!");
        } else {
          // Create a new document.
          final adoc = await _db!.document(device.id);
          if (adoc == null) {
            final mutableDocumemt = MutableDocument.withId('hotel::1')
              ..setInteger(1, key: 'scans')
              ..setString('todo', key: 'name')
              ..setDate(DateTime.now(), key: 'createdAt');
            await _db?.saveDocument(mutableDocumemt);
          }
          

          /*
          device.serviceData.forEach((k, v) {
            print('{ key: $k, value: $v }');
          });
          */
          // _genericBleDevices_connected.addIf(!_genericBleDevices_connected.contains(device),device);
          // int sz = _devices.length;
          _devices.addIf(!_devices.contains(device), {
            'id': device.id,
            'name': device.name,
            'type':  device.manufacturerData.toString()
          });
          
          // if (sz <  _devices.length)
          print(device.name +
              " UUIDs: " +
              device.serviceUuids.toString() +
              " MfgData: " +
              device.manufacturerData.toString() +
              " ID: " +
              device.id);
        }
        // print(device.id);
        // XXX TODO: add a proper target name to match
        if (device.name == "target") {
          discoveredDevice = device;
          foundDeviceWaitingToConnect = true;
          notifyListeners();
          print("Device is found");

          //Stop the scan
          scanDeviceStream.cancel();

          _connectToDevice();
        }
      }, onError: (Object e) {
        print('BLE Scan Error $e');
      });

      await Future.delayed(
          const Duration(seconds: BLEProvider.BLE_SCAN_TIMEOUT), (() {
        scanDeviceStream.cancel();
        print("scan stopped");
        scanStarted = false;

        if (!connected) {
          print("Not connected");
          // XXX TODO: update screen at end of scan
          // _bleUpdateScreenState("disconnected");
        }

        notifyListeners();
      }));
    } else {
      print("Permission is not grantted, TOOD: Fix this, unhandled");
    }
    return;
  }

  //Application Handlers

  void onDisconnectedHandler() {
    connected = false;
    notifyListeners();
    _bleUpdateScreenState("disconnected");
  }

  RxList<Map> getDevices() {
    return _devices;
  }
  
  void scanAndConnectToDevice() async {
    //Update Screen
    // XXX TODO: show progress on scan
    // _bleUpdateScreenState("connecting");

    startScan();

    // XXX Why do we need this?
    if (connected) {
      print("scanAndConnectToDevice is connected");
    } else {
      print("Is not connected");
    }
  }

  void _connectToDevice() async {
    Stream<ConnectionStateUpdate> _currentConnnectionStream =
        ble.connectToDevice(
            id: discoveredDevice.id,
            servicesWithCharacteristicsToDiscover: {
              serviceUuid: [txUUID, rxUUID]
            },
            connectionTimeout:
                const Duration(seconds: BLEProvider.BLE_CONNECTION_TIMEOUT));
    //Device Listener
    _currentConnnectionStream.listen((event) async {
      switch (event.connectionState) {
        //Connected
        case DeviceConnectionState.connected:
          {
            rxCharacterisitic = QualifiedCharacteristic(
                characteristicId: rxUUID,
                serviceId: serviceUuid,
                deviceId: event.deviceId);
            txCharacterisitic = QualifiedCharacteristic(
                characteristicId: txUUID,
                serviceId: serviceUuid,
                deviceId: event.deviceId);
            // Start handling updates
            ble.subscribeToCharacteristic(txCharacterisitic).listen((data) {
              // code to handle incoming data
              try {
                String datastr = new String.fromCharCodes(data);
                print(datastr);
              } catch (o) {
                print("txCharacteristic parse error caught ${o.runtimeType}");
              }
            }, onError: (dynamic error) {
              // code to handle errors
              // Ignoring errors
            });
            foundDeviceWaitingToConnect = false;
            connected = true;
            scanStarted = false;

            notifyListeners();
            _bleUpdateScreenState("connected");

            // Write an initial value to support pairing
            String data = "\$off/#"; // TODO: read this from app state
            await ble.writeCharacteristicWithoutResponse(rxCharacterisitic,
                value: data.codeUnits);

            btnStatusTimer = Timer.periodic(const Duration(seconds: 1),
                (Timer t) async => handleButtonStateBLEWrite());
            // btnStatusTimer = Timer.periodic(
            //     Duration(seconds: 1),
            //     (Timer t) async => await ble.writeCharacteristicWithoutResponse(
            //         rxCharacterisitic,
            //         value: "\$$buttonToggleState/#".codeUnits));
            break;
          }
        case DeviceConnectionState.disconnected:
          {
            connected = false;
            notifyListeners();
            btnStatusTimer!.cancel();
            _bleUpdateScreenState("disconnected");
            //reconnectToDevice();
            break;
          }
        default:
      }
    });
  }

  Future<void> handlePowerOff() async {
    // Write an initial value to support pairing
    print("---->handlePowerOff called in 10s");
    String data = "\$pwr/#"; // TODO: read this from app state
    await ble.writeCharacteristicWithoutResponse(rxCharacterisitic,
        value: data.codeUnits);
  }

  void handleButtonStateBLEWrite() {
    // callback function
    try {
      // ble.writeCharacteristicWithoutResponse(rxCharacterisitic, value: "\$$buttonToggleState/#".codeUnits);
    } catch (o) {
      print("chandleButtonStateBLEWrite caught ${o.runtimeType}");
    }
  }

  void _timerState(String? screen) {
    if (screen == "disconnected") {

    }
  }

  //BLE screen update handler
  void _bleUpdateScreenState(String? screen) {
    if (screen != null) {
      //connecting/disconnected/connected
      if (screen == "connecting" ||
          screen == "disconnected" ||
          screen == "connected" ||
          screen == "done" ||
          screen == "about" ||
          screen == "terms") {
        //Check if device is already connected

        if (connected) {
            app_screen = screen;

            // Delay switching to ActiveTreatmentPage for 2.5 seconds.
            Future.delayed(const Duration(milliseconds: 500), () {
              app_screen = 'treatment';
            });
        //  }
        } else {
          //Assign app screen to app
          app_screen = screen;
        }
      }

      notifyListeners();
    } else {
      throw Exception("argument is null, cannot change screen");
    }
  }
}
