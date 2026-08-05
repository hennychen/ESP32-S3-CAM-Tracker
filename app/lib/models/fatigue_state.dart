/// 疲劳等级枚举
enum FatigueLevel {
  normal,   // 0: 正常
  notice,   // 1: 注意（闭眼0.5~1s）
  warning,  // 2: 疲劳（闭眼1~3s）
  danger,   // 3: 危险（微睡眠>3s）
}

extension FatigueLevelExt on FatigueLevel {
  int get value => index;

  String get label {
    switch (this) {
      case FatigueLevel.normal:  return '正常';
      case FatigueLevel.notice:  return '请注意';
      case FatigueLevel.warning: return '疲劳驾驶';
      case FatigueLevel.danger:  return '危险！唤醒';
    }
  }

  String get esp32Param {
    switch (this) {
      case FatigueLevel.normal:  return 'clear';
      case FatigueLevel.notice:  return 'clear';   // L1 不下发到 ESP32
      case FatigueLevel.warning: return 'warning';
      case FatigueLevel.danger:  return 'danger';
    }
  }

  Color get color {
    switch (this) {
      case FatigueLevel.normal:  return const Color(0xFF00E676);
      case FatigueLevel.notice:  return const Color(0xFFFFEB3B);
      case FatigueLevel.warning: return const Color(0xFFFF9800);
      case FatigueLevel.danger:  return const Color(0xFFF44336);
    }
  }
}

/// 单帧疲劳检测快照
class FatigueSnapshot {
  final double ear;              // 滤波后 EAR
  final double perclos;          // 60s PERCLOS (0.0~1.0)
  final bool eyesClosed;         // 当前帧是否闭眼
  final double closedSeconds;    // 连续闭眼秒数
  final FatigueLevel level;      // 综合疲劳等级
  final double score;            // 融合评分 0.0~1.0
  final bool baselineReady;      // 基线是否就绪
  final double fps;              // 当前帧率

  const FatigueSnapshot({
    this.ear = 0,
    this.perclos = 0,
    this.eyesClosed = false,
    this.closedSeconds = 0,
    this.level = FatigueLevel.normal,
    this.score = 0,
    this.baselineReady = false,
    this.fps = 0,
  });
}
