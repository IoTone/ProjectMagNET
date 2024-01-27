import 'package:flutter_neumorphic/flutter_neumorphic.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';



import 'package:magnet_app/splash_page.dart';
import 'package:magnet_app/about.dart';
import 'package:magnet_app/app_state_model.dart';
import 'package:magnet_app/provider/ble_provider.dart';


void main() {
  WidgetsFlutterBinding.ensureInitialized();
  SystemChrome.setPreferredOrientations([
    DeviceOrientation.portraitUp,
    DeviceOrientation.portraitDown,
  ]).then((value) {
    runApp(
      MultiProvider(
        providers: [
          // ChangeNotifierProvider(create: (context) => TimerModel()),
          ChangeNotifierProvider(create: (context) => AppState()),
        ],
        child: const NeumorphicApp(
          home: MyApp(),
        ),
      ),
    );
  });
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});
  // This widget is the root of your application.
  @override
  Widget build(BuildContext context) {
    return Consumer(
    builder:(context, app, child) => NeumorphicApp(
      debugShowCheckedModeBanner: false,
      title: 'Flutter Demo',
      themeMode: ThemeMode.light,
      theme: NeumorphicThemeData(
        baseColor: Color(0xFFFFFFFF),
        lightSource: LightSource.topLeft,
        depth: 10,
      ),
      darkTheme: NeumorphicThemeData(
        baseColor: Color(0xFF3E3E3E),
        lightSource: LightSource.topLeft,
        depth: 6,
      ),
      home: switchScreen(Provider.of<AppState>(context))),// MyHomePage(),
    );
  }
}

class MyHomePage extends StatelessWidget {
  MyHomePage({super.key});

  Widget build(BuildContext context) {
    return Scaffold(
      floatingActionButton: NeumorphicFloatingActionButton(
        child: Icon(Icons.add, size: 30),
        onPressed: () {},
      ),
      backgroundColor: NeumorphicTheme.baseColor(context),
      body: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            NeumorphicButton(
              onPressed: () {
                print("onClick");
              },
              style: NeumorphicStyle(
                shape: NeumorphicShape.flat,
                boxShape: NeumorphicBoxShape.circle(),
              ),
              padding: const EdgeInsets.all(12.0),
              child: Icon(
                Icons.favorite_border,
                color: _iconsColor(context),
              ),
            ),
            NeumorphicButton(
                margin: EdgeInsets.only(top: 12),
                onPressed: () {
                  NeumorphicTheme.of(context)!.themeMode =
                      NeumorphicTheme.isUsingDark(context)
                          ? ThemeMode.light
                          : ThemeMode.dark;
                },
                style: NeumorphicStyle(
                  shape: NeumorphicShape.flat,
                  boxShape:
                      NeumorphicBoxShape.roundRect(BorderRadius.circular(8)),
                ),
                padding: const EdgeInsets.all(12.0),
                child: Text(
                  "Toggle Theme",
                  style: TextStyle(color: _textColor(context)),
                )),
            NeumorphicButton(
                margin: EdgeInsets.only(top: 12),
                onPressed: () {
                  Navigator.of(context)
                      .pushReplacement(MaterialPageRoute(builder: (context) {
                    return SplashPageWidget();
                  }));
                },
                style: NeumorphicStyle(
                  shape: NeumorphicShape.flat,
                  boxShape:
                      NeumorphicBoxShape.roundRect(BorderRadius.circular(8)),
                  //border: NeumorphicBorder()
                ),
                padding: const EdgeInsets.all(12.0),
                child: Text(
                  "Go to full sample",
                  style: TextStyle(color: _textColor(context)),
                )),
          ],
        ),
      ),
    );
  }

  Color? _iconsColor(BuildContext context) {
    final theme = NeumorphicTheme.of(context);
    if (theme!.isUsingDark) {
      return theme.current!.accentColor;
    } else {
      return null;
    }
  }

  Color _textColor(BuildContext context) {
    if (NeumorphicTheme.isUsingDark(context)) {
      return Colors.white;
    } else {
      return Colors.black;
    }
  }
}

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