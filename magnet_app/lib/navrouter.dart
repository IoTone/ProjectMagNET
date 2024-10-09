
import 'package:flutter/widgets.dart';
import 'package:magnet_app/about.dart';
import 'package:magnet_app/app_state_model.dart';
import 'package:magnet_app/main.dart';
import 'package:magnet_app/splash_page.dart';

Widget switchScreen(AppState app_provider) {
  String title;
  print("app_screen: $app_provider.app_screen");
  switch (app_provider.app_screen) {
    case "splash":
      title = "Splash";
      return const SplashPageWidget();
    case "home":
      title ="Home";
      return MyHomePage();
    /* case "connecting":
      title = "Connecting";
      return ConnectingPage(title: title); */
    /* case "connected":
      title = "Connected";
      return ConnectedPage(title: title); */
    case "about":
      title = "About";
      return const AboutPageWidget();
    /* case "terms":
      title = "Terms";
      return TermsPage(title: title); */
    /* case "privacy":
      title = "Privacy";
      return PrivacyPolicyPage(title: title); */
    /* case "disconnected":
      title = "Disconnected";
      return ConnectionErrorPage(title: title); */
    default:
      throw Exception("$app_provider.app_screen is not a valid screen state");
  }
}