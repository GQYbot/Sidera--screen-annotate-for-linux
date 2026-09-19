// Sidera - 程序入口
// Copyright (C) 2026 Carl_Jin   GNU GPL v3
#include "app.h"
#include "widget.h"

// 全局唯一实例
AppState g;

// ============================================================
// 3. 平台检测
// ============================================================
static QString detectPlatform() {
  QByteArray sessionType = qgetenv("XDG_SESSION_TYPE").toLower();
  if (sessionType == "wayland") {
    qWarning() << "[WARN] 检测到当前运行于 Wayland 会话环境，Sidera 将通过 XWayland 兼容层运行。";
    return "wayland";
  }
  return "x11";
}

// ============================================================
// 4. 软件渲染
// ============================================================
static void setupSoftwareRendering() {
  qputenv("QT_OPENGL", "software");
  qputenv("QT_QUICK_BACKEND", "software");
  qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
}

// ============================================================
// 11. X11 全局快捷键
// ============================================================
static void processX11Hotkeys();

static void setupX11GlobalHotkey() {
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) { g.hotkeyOk = false; return; }
  g.xDisplay = dpy; g.xRootWin = DefaultRootWindow(dpy);
  unsigned int mods = ControlMask | ShiftMask;
  KeyCode kc = XKeysymToKeycode(dpy, XK_D);
  if (kc) {
    XGrabKey(dpy, kc, mods, g.xRootWin, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, kc, mods|Mod2Mask|LockMask, g.xRootWin, True, GrabModeAsync, GrabModeAsync);
  }
  XFlush(dpy);
  int fd = ConnectionNumber(dpy);
  QSocketNotifier* n = new QSocketNotifier(fd, QSocketNotifier::Read);
  QObject::connect(n, &QSocketNotifier::activated, [](int){ processX11Hotkeys(); });
  g.hotkeyOk = true;
}

static void processX11Hotkeys() {
  if (!g.xDisplay) return;
  while (XPending(g.xDisplay)) {
    XEvent ev; XNextEvent(g.xDisplay, &ev);
    if (ev.type == KeyPress) {
      KeySym ks = XkbKeycodeToKeysym(g.xDisplay, ev.xkey.keycode, 0, 0);
      if (ks == XK_D) {
        if ((ev.xkey.state & (ControlMask|ShiftMask)) == (ControlMask|ShiftMask)) {
          if (g.currentMode != 0) switchToCursorMode();
          else switchToDrawMode(1);
        }
      }
    }
  }
}


// ============================================================
// 13. main
// ============================================================
int main(int argc, char* argv[]) {
  setupSoftwareRendering();
  QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false); // 触摸/鼠标移动不压缩，绘制更顺
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);   // 关闭设置窗口不会退出整个进程

  g.platform = detectPlatform();
  qDebug() << "[INFO] 平台:" << g.platform;

  // 单实例：先探测是否已有实例；有则唤醒其窗口并退出，无则清理残留后监听
  {
    const QString name = "sidera-single";
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(300)) {
      probe.write("show"); probe.flush(); probe.waitForBytesWritten(200);
      qWarning() << "[WARN] Sidera已在运行，退出新实例";
      return 0;
    }
    QLocalServer::removeServer(name);           // 仅清理上次崩溃的残留，不会误删运行中的实例
    QLocalServer* single = new QLocalServer(&app);
    if (!single->listen(name)) {               // 极端竞态：探测后又有人抢先
      QLocalSocket ping;
      ping.connectToServer(name);
      if (ping.waitForConnected(300)) { ping.write("show"); ping.flush(); }
      qWarning() << "[WARN] Sidera已在运行，退出新实例";
      return 0;
    }
    QObject::connect(single, &QLocalServer::newConnection, [single]() {
      QLocalSocket* c = single->nextPendingConnection();
      if (c) c->deleteLater();
      if (g.mainWidget) { g.mainWidget->show(); g.mainWidget->raise(); g.mainWidget->activateWindow(); }
      if (g.settingsWin) { g.settingsWin->show(); g.settingsWin->raise(); g.settingsWin->activateWindow(); }
    });
  }

  // 启动闪屏：延后 2 秒主界面初始化，期间显示进度动画
  showSplashFor(app);

  // 载入持久化设置（透明度/大小/wpsDebug），须在构建侧边栏前
  wpsLoadSettings();

  // 单窗口
  MainWidget* mw = new MainWidget();
  g.mainWidget = mw;

  // 初始：光标模式
  mw->show();
  {
    QRect scr = QGuiApplication::primaryScreen()->geometry();
    int iy = (scr.height() - sbHeight()) / 2;
    g.sidebarScreenPos  = QPoint(4, iy);
    g.sidebarScreenPosR = QPoint(scr.width() - sbWidth() - 4, iy);
  }

  // 合成器检测：无合成器时透明画布会失效（显示不透明/黑屏）
  if (!hasCompositor()) {
    qWarning() << "[WARN] 未检测到 X11 合成器！透明画布将无法显示，批注会被不透明背景遮挡。"
               << "请开启合成器，例如: picom &  或  xcompmgr &";
  }
  // 推迟到下一个事件循环：等 X11 处理完 show 再设输入形状
  QTimer::singleShot(200, []() {
    setInputShapeToSidebar();
  });

  setupX11GlobalHotkey();

  // 收缩态自动展开：仅当处于收缩时，2 分钟后自动展开（展开态不做任何自动化）
  g.sidebarIdleTimer = new QTimer(&app);
  g.sidebarIdleTimer->setSingleShot(true);
  QObject::connect(g.sidebarIdleTimer, &QTimer::timeout, []() { if (g.collapsed) expandSidebars(); });

  // WPS 全屏检测定时器（每 500ms 检查一次）
  QTimer* wpsTimer = new QTimer(&app);
  QObject::connect(wpsTimer, &QTimer::timeout, []() { checkWpsState(); });
  wpsTimer->start(500);
  checkWpsState();  // 立即执行一次

  QObject::connect(&app, &QApplication::aboutToQuit, []() {
    stopWpsApiServer();
    clearAllPages();
    clearUndo();
    delete g.canvas; g.canvas = nullptr;
    delete g.iconCursor; g.iconCursor = nullptr;
    delete g.iconPen; g.iconPen = nullptr;
    delete g.iconEraser; g.iconEraser = nullptr;

  });

  // 调试模式启动：已由 wpsLoadSettings 载入 g.wpsDebug；环境变量仅本次运行覆盖（不落盘）
  {
    QString env = QString::fromLocal8Bit(qgetenv("WPS_API_DEBUG"));
    if (!env.isEmpty()) setWpsDebug(env == "1", false);
    else                setWpsDebug(g.wpsDebug, false);
  }

  return app.exec();
}
