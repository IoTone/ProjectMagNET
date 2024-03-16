import 'package:flutter_neumorphic/flutter_neumorphic.dart';
// XXX These are all here for debug dev purposes to allow testing
// of specific views
// import 'package:magnet_app/about.dart';
// import 'package:magnet_app/bluetooth_disabled.dart';
// import 'package:magnet_app/connected.dart';
// import 'package:magnet_app/connecting.dart';
// import 'package:magnet_app/connection_error.dart';
// import 'package:magnet_app/active_treatment.dart';
// import 'package:magnet_app/done_treatment.dart';
import 'package:audioplayers/audioplayers.dart';

// XXX TODO: redo this with Neumorphic
class SplashPageWidget extends StatefulWidget {
  const SplashPageWidget({super.key});

  @override
  State<SplashPageWidget> createState() => _SplashPageWidgetState();
}

class _SplashPageWidgetState extends State<SplashPageWidget> {
  final player = AudioPlayer();

  @override
  void initState() {
    super.initState();
   // _navigatetohome();
   
  }
  Widget _letter(String letter) {
    return Text(letter,
        style: const TextStyle(
        color: Colors.black,
        fontWeight: FontWeight.w700,
        fontFamily: 'Samsung',
        fontSize: 62));
  }
  
  @override
  Widget build(BuildContext context) {
    player.play(AssetSource('audio/splash.wav'));
    return Scaffold(
      /*
      floatingActionButton: NeumorphicFloatingActionButton(
        child: const Icon(Icons.add, size: 30),
        onPressed: () {},
      ),*/
      backgroundColor: NeumorphicTheme.baseColor(context),
      appBar: AppBar(
        centerTitle: true,
        toolbarHeight: 150,
        backgroundColor: NeumorphicTheme.baseColor(context),
        title: _letter("MagNET"),
        // title: Image.asset('assets/images/mobilelogo.png'),
      ),
      body: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Padding(
              padding: EdgeInsets.only(top: 50.0),
              child: Image(image: AssetImage('assets/images/magnet_splash.jpg')))
            /*
            NeumorphicButton(
              onPressed: () {
                print("onClick");
              },
              style: const NeumorphicStyle(
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
                margin: const EdgeInsets.only(top: 12),
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
                margin: const EdgeInsets.only(top: 12),
                onPressed: () {
                  Navigator.of(context)
                      .pushReplacement(MaterialPageRoute(builder: (context) {
                    return FullSampleHomePage();
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
                */
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
  /*
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF4bb0c9),
      appBar: AppBar(
        centerTitle: true,
        toolbarHeight: 150,
        backgroundColor: const Color(0xFF4bb0c9),
        // title: Image.asset('assets/images/mobilelogo.png'),
      ),
      body: const Center(
          child: Column(
        mainAxisAlignment: MainAxisAlignment.start,
        children: <Widget>[
          Padding(
              padding: EdgeInsets.only(top: 50.0),
              child: Image(image: AssetImage('assets/images/magnet_splash.jpg')))
        ],
      )),
    );
  }
  */
}

