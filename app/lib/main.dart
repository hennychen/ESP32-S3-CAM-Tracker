import 'package:flutter/material.dart';
import 'package:wakelock_plus/wakelock_plus.dart';
import 'screens/connection_screen.dart';
import 'screens/monitor_screen.dart';
import 'services/esp32_service.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const DmsApp());
}

class DmsApp extends StatelessWidget {
  const DmsApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DMS 疲劳监测',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF00BCD4),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
        appBarTheme: const AppBarTheme(centerTitle: true),
      ),
      home: const EntryPoint(),
    );
  }
}

/// 入口：检查上次连接的 ESP32 地址，有则直接进监控页
class EntryPoint extends StatefulWidget {
  const EntryPoint({super.key});

  @override
  State<EntryPoint> createState() => _EntryPointState();
}

class _EntryPointState extends State<EntryPoint> {
  @override
  void initState() {
    super.initState();
    _checkLastConnection();
  }

  Future<void> _checkLastConnection() async {
    final url = await Esp32Service.getLastUrl();
    if (!mounted) return;
    if (url != null) {
      // 上次有连接记录，直接进监控页
      Navigator.of(context).pushReplacement(
        MaterialPageRoute(
          builder: (_) => MonitorScreen(esp32Url: url),
        ),
      );
    }
    // 否则留在连接页（默认路由）
  }

  @override
  Widget build(BuildContext context) {
    return const ConnectionScreen();
  }
}
