import 'package:flutter_neumorphic/flutter_neumorphic.dart';
import 'package:provider/provider.dart';

// import 'package:magnet_app/connecting.dart';
import 'package:magnet_app/app_state_model.dart';
import 'package:magnet_app/navrouter.dart';

// XXX TODO: redo this with Neumorphic
class AboutPageWidget extends StatefulWidget {
  const AboutPageWidget({super.key});

  @override
  State<AboutPageWidget> createState() => _AboutPageWidgetState();
}

class _AboutPageWidgetState extends State<AboutPageWidget> {

  @override
  Widget build(BuildContext context) {
     final appState = Provider.of<AppState>(context, listen: false);

    return Scaffold(
      backgroundColor: const Color(0xFFFFFFFF),
      appBar: NeumorphicAppBar( // AppBar(
        centerTitle: true,
        
        // toolbarHeight: 80,
        // backgroundColor: const Color(0xFF4bb0c9),
        title: Image.asset('assets/images/magnet_icon.png'),
        ),
            drawer: Drawer(
              child: ListView(
                children: [
                  ListTile(
                    title: const Text('Home'),
                    onTap: () {
                      appState.app_screen = "home";
                      Navigator.of(context)
                        .push(MaterialPageRoute(builder: (context) => switchScreen(appState),
                        ),
                      );
                    },
                  ),
                  ListTile(
                    title: const Text('Privacy'),
                    onTap: () {
                      appState.app_screen = "privacy";

                      // Navigator.of(context).push(
                      //   MaterialPageRoute(
                      //     builder: (context) => const AboutPageWidget(),
                      //   ),
                      // );
                      
                    },
                  ),
                            ListTile(
                    title: const Text('Terms'),
                    onTap: () {
                      appState.app_screen = "terms";

                      // Navigator.of(context).push(
                      //   MaterialPageRoute(
                      //     builder: (context) => const AboutPageWidget(),
                      //   ),
                      // );
                      
                    },
                  )
                ],
              ),
            ),
        body: const Center(
          child: Column(
          mainAxisAlignment: MainAxisAlignment.spaceEvenly,
          children: <Widget>[
            Flexible(child:
            Image(image: AssetImage('assets/images/magnet_splash.jpg'))
            ),
                            // Image(image: AssetImage('assets/images/logo.png')),
            // XXX TODO: pull app version from metadata
            Text('App v1.0.0 \nCopyright IoTone, Inc. 2024', textAlign: TextAlign.center ,style: TextStyle(color: Color(0xFF1B68AF),
                    fontSize: 32, 
            ),
           )
          ],
        ),
        ),
    );
  }
}