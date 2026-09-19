// Sidera - WPS 联动/缓存/HTTP 服务
// Copyright (C) 2026 Carl_Jin   GNU GPL v3
#include "app.h"

// 当前生效的笔迹缓存：白板模式与普通/放映模式各自独立
QMap<int, QPixmap*>& activeCache() {
  return g.whiteboard ? g.whiteboardCache : g.slideCache;
}

// ============================================================
// 多页缓存：翻页时保存/加载笔迹
// ============================================================
void clearAllPages() {
  qDebug() << "[INFO] clearAllPages 被调用，清空 普通" << g.slideCache.size()
           << "+ 白板" << g.whiteboardCache.size() << "页缓存 + 画布";
  for (auto* pix : g.slideCache) delete pix;
  g.slideCache.clear();
  for (auto* pix : g.whiteboardCache) delete pix;
  g.whiteboardCache.clear();
  g.currentSlide = 1;

  // 重置绘图状态，防止残留（幽灵线 / 未完成的笔画）
  g.isDrawing   = false;
  g.lastPt  = QPoint();

  // 清空当前画布
  if (g.canvas) g.canvas->fill(Qt::transparent);
  clearUndo();

  // 异步重绘（不用 repaint：同步重绘可能阻塞事件循环，导致翻页按钮点击丢失）
  if (g.mainWidget) g.mainWidget->update();
}

void saveCurrentPage() {
  if (!g.canvas) return;
  QMap<int, QPixmap*>& cache = activeCache();
  // 限制缓存页数
  if (cache.size() >= g.maxCachePages && !cache.contains(g.currentSlide)) return;
  // 深拷贝当前画布
  delete cache.value(g.currentSlide);
  cache[g.currentSlide] = new QPixmap(*g.canvas);
}

void loadPage(int page) {
  if (!g.canvas) return;
  g.canvas->fill(Qt::transparent);
  QPixmap* cached = activeCache().value(page, nullptr);
  if (cached) {
    QPainter p(g.canvas);
    p.drawPixmap(0, 0, *cached);
    p.end();
  }
  clearUndo();                       // 换页后撤回栈失效
  if (g.mainWidget) g.mainWidget->update();
}

// ============================================================
// WPS 接口调试模式（实验）：本地 HTTP 16666 + 日志
// 核心思路：触发“下一步”的机制无所谓（假键/官方键都是同一件事），
//   真正的区别在缓存时机。
//   默认模式（调试关）：每点一次按钮 = 存一页并刷新（近似行为，多动画会错位）；
//   调试模式（开 + 加载项已连）：按钮只发 Up/Down 推进放映，不做缓存；
//     缓存改由加载项回传的真实页号事件驱动——真实换页才存旧页/载新页，
//     页内动画步不动缓存 → 批注一直跟着真页走，动画不错位。
//   没客户端连接时自动退回默认行为（点击即缓存），保证按钮始终可用。
// ============================================================
QString wpsLogFile() {
  QString dir = QDir::homePath();
  if (!QFileInfo(dir).isWritable()) dir = "/tmp";
  return dir + "/wps-api-debug.log";
}

void wpsLog(const QString& msg) {
  QString line = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz ") + msg;
  qDebug().noquote() << "[WPSAPI]" << msg;
  QString path = wpsLogFile();
  // 超过 1MB 自动重开，防日志无限增长
  if (QFileInfo(path).size() > 1024 * 1024) QFile::remove(path);
  QFile f(path);
  if (f.open(QIODevice::Append | QIODevice::Text)) {
    QTextStream ts(&f);
    ts << line << "\n";
    f.close();
  }
}

// ---------------- 设置持久化（wpsDebug / 侧边栏透明度 / 大小） ----------------
static QString wpsConfigFile() {
  return QDir::homePath() + "/.config/sidera/config";
}
static bool wpsConfigExists() { return QFile::exists(wpsConfigFile()); }
void wpsSaveSettings() {
  QDir().mkpath(QFileInfo(wpsConfigFile()).absolutePath());
  QFile f(wpsConfigFile());
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
  QTextStream ts(&f);
  ts << "wpsDebug=" << (g.wpsDebug ? 1 : 0) << "\n";
  ts << "sbScale=" << QString::number(g.sbScale, 'f', 2) << "\n";
  ts << "sidebarAlpha=" << g.sidebarAlpha << "\n";
  ts << "rightClickCursor=" << (g.rightClickCursorOn ? 1 : 0) << "\n";
  ts << "eraseByFinger=" << (g.eraseByFinger ? 1 : 0) << "\n";
  ts << "largeTouchThreshold=" << g.largeTouchThreshold << "\n";
  ts << "largeEraseScale10=" << g.largeEraseScale10 << "\n";
  f.close();
}
void wpsLoadSettings() {
  QFile f(wpsConfigFile());
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
  while (!f.atEnd()) {
    QString line = QString::fromUtf8(f.readLine()).trimmed();
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    QString k = line.left(eq);
    QString v = line.mid(eq + 1);
    if (k == "wpsDebug") g.wpsDebug = (v == "1");
    else if (k == "sbScale") g.sbScale = qBound(0.6, v.toDouble(), 1.4);
    else if (k == "sidebarAlpha") g.sidebarAlpha = qBound(30, v.toInt(), 255);
    else if (k == "rightClickCursor") g.rightClickCursorOn = (v == "1");
    else if (k == "eraseByFinger") g.eraseByFinger = (v == "1");
    else if (k == "largeTouchThreshold") g.largeTouchThreshold = qBound(20, v.toInt(), 260);
    else if (k == "largeEraseScale10") g.largeEraseScale10 = qBound(8, v.toInt(), 25);
  }
  f.close();
}

// ---------------- 加载项自动注册（写当前用户 jsaddons/publish.xml） ----------------
static void ensureWpsAddinRegistered() {
  QString path = QDir::homePath() + "/.local/share/Kingsoft/wps/jsaddons/publish.xml";
  QString entry = "  <jspluginonline name=\"sidera-bridge\" type=\"wpp\" "
                  "url=\"http://127.0.0.1:16666/\" debug=\"\" enable=\"enable\" install=\"null\"/>\n";
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  QString content;
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    content = QString::fromUtf8(f.readAll());
    f.close();
  }
  if (content.contains("sidera-bridge") || content.contains("screen-annotate-bridge")) return;   // 已登记，静默
  if (!content.isEmpty() && content.contains("<jsplugins>") && content.contains("</jsplugins>"))
    content.replace("</jsplugins>", entry + "</jsplugins>");
  else
    content = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<jsplugins>\n"
            + entry + "</jsplugins>\n";
  if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QTextStream ts(&f);
    ts << content;
    f.close();
    wpsLog("已自动登记加载项 → " + path);
  }
}

static void wpsHandleHttp(QTcpSocket* s);
static void wpsHttpReply(QTcpSocket* s, const QString& body);
static void wpsServeAddinFile(QTcpSocket* s, const QString& path);
static void wpsTouch();

// 加载项事件上报 → 同步批注页缓存。核心规则：
//   真实页号变化(pos != 已知) → 保存旧页、载入新页；
//   页号不变(动画步) → 缓存不动。
static void wpsOnRealPos(int pos) {
  if (pos <= 0) return;
  if (g.wpsRealPos == pos) return;            // 还在同一页（动画步）
  if (g.wpsRealPos > 0) {                     // 从旧页切走：保存旧页笔迹
    g.currentSlide = g.wpsRealPos;
    saveCurrentPage();
  }
  g.wpsRealPos = pos;
  g.currentSlide = pos;
  loadPage(pos);
}

static void wpsHandleLine(const QString& raw) {
  if (g.whiteboard) return;                    // 白板模式：与外界完全隔离，忽略 WPS 事件
  QString line = raw.trimmed();
  if (line.isEmpty()) return;
  wpsLog("收 << " + line);
  if (!line.startsWith("EVENT ")) return;
  int pos = -1, click = -1;
  QRegExp rxPos("pos=(\\d+)"), rxClick("click=(\\d+)");
  if (rxPos.indexIn(line) >= 0) pos = rxPos.cap(1).toInt();
  if (rxClick.indexIn(line) >= 0) click = rxClick.cap(1).toInt();
  QString name = line.section(' ', 1, 1);
  wpsLog(QString("事件 %1 pos=%2 click=%3").arg(name).arg(pos).arg(click));
  if (name == "SlideShowBegin") {             // 放映开始：清空并落到第 1 页
    clearAllPages();
    g.wpsRealPos = pos > 0 ? pos : 1;
    g.currentSlide = g.wpsRealPos;
    if (g.mainWidget) g.mainWidget->update();
    return;
  }
  if (pos > 0) wpsOnRealPos(pos);
}

static void wpsCloseClient() {
  if (g.wpsSock) {
    g.wpsSock->abort();
    g.wpsSock->deleteLater();
    g.wpsSock = nullptr;
  }
  if (g.wpsConnected) {
    g.wpsConnected = false;
    wpsLog("客户端断开");
  }
}

static void startWpsApiServer() {
  if (g.wpsServer) return;                     // 已在跑
  QTcpServer* srv = new QTcpServer();
  g.wpsServer = srv;
  QObject::connect(srv, &QTcpServer::newConnection, []() {
    while (g.wpsServer && g.wpsServer->hasPendingConnections()) {
      QTcpSocket* s = g.wpsServer->nextPendingConnection();
      QObject::connect(s, &QTcpSocket::readyRead, [s]() { wpsHandleHttp(s); });
      QObject::connect(s, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
                       [s](QAbstractSocket::SocketError) { s->deleteLater(); });
    }
  });
  if (!srv->listen(QHostAddress::LocalHost, 16666)) {
    wpsLog("HTTP 16666 绑定失败: " + srv->errorString());
    srv->deleteLater();
    g.wpsServer = nullptr;
    return;
  }
  wpsLog("调试服务已启动，监听 127.0.0.1:16666 (HTTP)");
  // 心跳：3 秒无请求视为加载项离线
  QTimer* t = new QTimer(srv);
  g.wpsPingTimer = t;
  QObject::connect(t, &QTimer::timeout, []() {
    if (g.wpsConnected &&
        QDateTime::currentMSecsSinceEpoch() - g.wpsLastSeen > 3000) {
      g.wpsConnected = false;
      g.wpsRealPos = -1;                        // 离线：清掉旧“真实页号”，避免重连后误存错页
      wpsLog("客户端离线（3s 无请求）");
    }
  });
  t->start(1000);
}

void stopWpsApiServer() {
  bool wasRunning = g.wpsServer || g.wpsConnected || !g.wpsCmdQueue.isEmpty() || g.wpsPingTimer;
  if (g.wpsServer) {
    g.wpsServer->close();
    g.wpsServer->deleteLater();
    g.wpsServer = nullptr;
  }
  g.wpsPingTimer = nullptr;
  g.wpsConnected = false;
  g.wpsCmdQueue.clear();
  if (wasRunning) wpsLog("调试服务已停止");
}

void setWpsDebug(bool on, bool persist) {
  if (g.wpsDebug == on) {                     // 状态没变
    if (on && !g.wpsServer) startWpsApiServer();
    if (on && g.wpsServer) ensureWpsAddinRegistered();
    if (persist) wpsSaveSettings();
    return;
  }
  g.wpsDebug = on;
  if (on) {
    startWpsApiServer();
    if (g.wpsServer) ensureWpsAddinRegistered();
  } else {
    stopWpsApiServer();
    g.wpsRealPos = -1;
  }
  if (persist) wpsSaveSettings();
}

// ============================================================
// WPS 接口调试 HTTP 端点（供 Chromium 里的加载项调用）
//   /hello?m=xx     加载项上线问候
//   /push?m=<EVENT> 加载项上报事件（走 wpsHandleLine）
//   /poll           加载项取走一条待执行指令（NEXT/PREV，无则空）
//   跨域(CORS)已放开；任意请求即刷新“在线”心跳
// ============================================================
static void wpsTouch() {
  g.wpsLastSeen = QDateTime::currentMSecsSinceEpoch();
  if (!g.wpsConnected) { g.wpsConnected = true; wpsLog("客户端接入"); }
}

static void wpsHttpReply(QTcpSocket* s, const QString& body) {
  QByteArray b = body.toUtf8();
  QString resp =
      "HTTP/1.1 200 OK\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      "Access-Control-Allow-Headers: *\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Length: " + QString::number(b.size()) + "\r\n"
      "Connection: close\r\n\r\n";
  s->write(resp.toUtf8() + b);
  s->flush();
  s->disconnectFromHost();
  s->deleteLater();
}

static void wpsHandleHttp(QTcpSocket* s) {
  if (!s->canReadLine()) return;               // 还没收到完整请求行
  QList<QByteArray> parts = s->readLine().split(' ');
  if (parts.size() < 2) { s->deleteLater(); return; }
  QByteArray method = parts[0];
  QUrl url = QUrl::fromEncoded("http://x" + parts[1]);
  QString path = url.path();
  QString m = QUrlQuery(url).queryItemValue("m");
  wpsTouch();                                   // 任意请求都算在线
  if (method == "OPTIONS") { wpsHttpReply(s, ""); return; }
  if (path == "/hello") {
    wpsLog("加载项问候: " + m);
    wpsHttpReply(s, "OK sidera");
    return;
  }
  if (path == "/push" && !m.isEmpty()) {
    wpsHandleLine(m);
    wpsHttpReply(s, "OK");
    return;
  }
  if (path == "/poll") {
    QString cmd = g.wpsCmdQueue.isEmpty() ? QString() : g.wpsCmdQueue.dequeue();
    wpsHttpReply(s, cmd);
    return;
  }
  // 其余路径：从加载项目录提供静态文件（manifest.xml/ribbon.xml/main.js/...）
  wpsServeAddinFile(s, path);
}

// 加载项内容目录：环境变量 > 程序所在目录 > 系统安装目录
static QString wpsAddinDir() {
  QString env = QString::fromLocal8Bit(qgetenv("WPS_ADDIN_DIR"));
  if (!env.isEmpty() && QFile::exists(env + "/manifest.xml")) return env;
  QString app = QCoreApplication::applicationDirPath() + "/wps-addin";
  if (QFile::exists(app + "/manifest.xml")) return app;
  QString sys = "/usr/share/sidera/wps-addin";
  if (QFile::exists(sys + "/manifest.xml")) return sys;
  return QString();
}

static void wpsServeAddinFile(QTcpSocket* s, const QString& path) {
  QString dir = wpsAddinDir();
  if (dir.isEmpty()) { wpsHttpReply(s, "OK"); return; }
  QString rel = path.mid(1);                     // 去掉开头 '/'
  if (rel.isEmpty()) rel = "manifest.xml";
  // 防目录穿越：只允许纯文件名/一级 js/
  if (rel.contains("..") || rel.contains("//")) { wpsHttpReply(s, "OK"); return; }
  QString fp = dir + "/" + rel;
  if (!QFile::exists(fp) || QFileInfo(fp).isDir()) { wpsHttpReply(s, "OK"); return; }
  QFile f(fp);
  if (!f.open(QIODevice::ReadOnly)) { wpsHttpReply(s, "OK"); return; }
  QByteArray body = f.readAll();
  f.close();
  // 简单 Content-Type
  QString ct = "text/plain";
  if (rel.endsWith(".xml")) ct = "application/xml";
  else if (rel.endsWith(".js")) ct = "text/javascript";
  else if (rel.endsWith(".html") || rel.endsWith(".htm")) ct = "text/html";
  else if (rel.endsWith(".svg")) ct = "image/svg+xml";
  else if (rel.endsWith(".json")) ct = "application/json";
  QString resp =
      "HTTP/1.1 200 OK\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Content-Type: " + ct + "; charset=utf-8\r\n"
      "Content-Length: " + QString::number(body.size()) + "\r\n"
      "Connection: close\r\n\r\n";
  s->write(resp.toUtf8() + body);
  s->flush();
  s->disconnectFromHost();
  s->deleteLater();
}

void goToPrevPage() {
  // 白板模式：完全本地翻页，绝不发送任何虚拟按键，与外界隔离
  if (g.whiteboard) {
    saveCurrentPage();
    if (g.currentSlide > 1) g.currentSlide--;
    loadPage(g.currentSlide);
    return;
  }
  // 调试模式：只“推进一步”，缓存由加载项回传的真实页号事件驱动（动画步不动缓存）
  bool debugNoCache = g.wpsDebug && g.wpsConnected;   // 无客户端时静默回退“点击即缓存”
  if (debugNoCache) {
    wpsLog("调试：入队 PREV 指令，等待加载项轮询执行");
    g.wpsCmdQueue.enqueue("PREV");
  }
  if (!debugNoCache) {
    saveCurrentPage();
    if (g.currentSlide > 1) g.currentSlide--;
  }
  // 始终发送 Up 键（与假键语义一致：有动画时它就是“下一步”）
  {
    Display* dpy = g.xDisplay;
    bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
    if (dpy) { sendXTestKey(dpy, XK_Up); if (nc) XCloseDisplay(dpy); }
  }
  if (!debugNoCache) loadPage(g.currentSlide);
}

void goToNextPage() {
  // 白板模式：完全本地翻页，绝不发送任何虚拟按键，与外界隔离
  if (g.whiteboard) {
    saveCurrentPage();
    g.currentSlide++;
    loadPage(g.currentSlide);
    return;
  }
  // 调试模式：只“推进一步”，缓存由加载项回传的真实页号事件驱动（动画步不动缓存）
  bool debugNoCache = g.wpsDebug && g.wpsConnected;   // 无客户端时静默回退“点击即缓存”
  if (debugNoCache) {
    wpsLog("调试：入队 NEXT 指令，等待加载项轮询执行");
    g.wpsCmdQueue.enqueue("NEXT");
  }
  if (!debugNoCache) {
    saveCurrentPage();
    g.currentSlide++;
  }
  // 始终发送 Down 键（与假键语义一致：有动画时它就是“下一步”）
  {
    Display* dpy = g.xDisplay;
    bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
    if (dpy) { sendXTestKey(dpy, XK_Down); if (nc) XCloseDisplay(dpy); }
  }
  if (!debugNoCache) loadPage(g.currentSlide);
}

/*
 * 判断窗口是否属于 WPS / OnlyOffice / LibreOffice
 * （用 WM_CLASS 的 res_name / res_class 匹配，小写 contains）
 */
static bool isOfficeWindow(Display* dpy, Window w) {
  XClassHint cls;
  if (!XGetClassHint(dpy, w, &cls)) return false;
  QString name  = QString::fromLocal8Bit(cls.res_name).toLower();
  QString klass = QString::fromLocal8Bit(cls.res_class).toLower();
  XFree(cls.res_name); XFree(cls.res_class);

  // WPS 各组件 + OnlyOffice + LibreOffice
  const char* keys[] = {
    "wps", "wpp", "et", "wpspdf",          // WPS Office
    "onlyoffice", "desktopeditors",        // OnlyOffice
    "soffice", "impress", "libreoffice",   // LibreOffice
    nullptr
  };
  for (int i = 0; keys[i]; i++)
    if (name.contains(keys[i]) || klass.contains(keys[i])) return true;
  return false;
}

/*
 * 全屏放映检测（组合方案 + 递归遍历）：
 *   1. 递归遍历所有顶层/嵌套窗口（WPS 放映窗口是嵌套窗口，XQueryTree(root) 只查直接子节点会漏掉）
 *   2. 只检查 WPS/OnlyOffice/LibreOffice 窗口（WM_CLASS 白名单）——彻底隔离 ClassIsland 等悬浮窗
 *   3. 对这些窗口：_NET_WM_STATE_FULLSCREEN 标志 或 几何尺寸铺满屏幕，任一命中即放映
 *
 * 这样无论 WPS 在 kwin 下是设 FULLSCREEN 还是只铺满屏幕、窗口是否嵌套都能检测到。
 */
static bool isPresentationFullscreen(Display* dpy) {
  // 静态缓存 Atom，避免每 500ms 重复 Intern
  static Atom netWmState = 0, netWmFullscreen = 0;
  if (!netWmState) {
    netWmState      = XInternAtom(dpy, "_NET_WM_STATE", False);
    netWmFullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
  }

  // 自己的窗口 XID
  Window selfWin = 0;
  if (g.mainWidget) {
    QWindow* wh = g.mainWidget->windowHandle();
    if (wh) selfWin = (Window)wh->winId();
    if (!selfWin) selfWin = (Window)g.mainWidget->winId();
  }

  QRect scr = QGuiApplication::primaryScreen()->geometry();
  bool found = false;

  std::function<void(Window)> walk = [&](Window w) {
    if (found) return;
    if (selfWin && w == selfWin) return;  // 排除自己

    // 对当前窗口：若是办公软件且全屏 → 判定放映
    if (isOfficeWindow(dpy, w)) {
      XWindowAttributes attrs;
      if (XGetWindowAttributes(dpy, w, &attrs) && attrs.map_state == IsViewable) {
        // FULLSCREEN 标志
        bool fullscreen = false;
        Atom at; int af; unsigned long ni, ba; unsigned char* pr = nullptr;
        if (XGetWindowProperty(dpy, w, netWmState, 0, 1024, False, XA_ATOM,
                               &at, &af, &ni, &ba, &pr) == Success && pr) {
          Atom* a = (Atom*)pr;
          for (unsigned long j = 0; j < ni; j++) if (a[j] == netWmFullscreen) { fullscreen = true; break; }
          XFree(pr);
        }
        // 几何尺寸兜底（kwin 下 WPS 可能不设 FULLSCREEN，只铺满屏幕）
        if (!fullscreen) {
          if (attrs.width >= scr.width() - 8 && attrs.height >= scr.height() - 8) {
            fullscreen = true;
          }
        }
        if (fullscreen) { found = true; return; }
      }
    }

    // 递归子窗口（放映窗口可能嵌套多层，必须深入遍历）
    Window r, p, *ch; unsigned int n;
    if (XQueryTree(dpy, w, &r, &p, &ch, &n) && ch) {
      for (unsigned int i = 0; i < n; i++) walk(ch[i]);
      XFree(ch);
    }
  };

  walk(DefaultRootWindow(dpy));
  return found;
}

/*
 * 定时检查 WPS 全屏状态，控制翻页按钮可见性。
 */
void checkWpsState() {
  Display* dpy = g.xDisplay;
  bool needClose = false;
  if (!dpy) { dpy = XOpenDisplay(nullptr); needClose = true; }
  if (!dpy) return;

  bool was = g.wpsFullscreen;
  g.wpsFullscreen = isPresentationFullscreen(dpy);

  if (needClose) XCloseDisplay(dpy);

  if (was != g.wpsFullscreen) {
    if (g.whiteboard) {
      // 白板模式：与外界隔离，仅更新缓存上限，不清空白板笔迹
      g.maxCachePages = g.wpsFullscreen ? 30 : 2;
    } else if (g.wpsFullscreen) {
      // 进入全屏放映：清空所有笔迹 + 缓存上限 30 页
      qDebug() << "[INFO] 进入全屏放映，清空笔迹";
      clearAllPages();
      g.maxCachePages = 30;
    } else {
      // 退出全屏放映：清空所有笔迹 + 缓存上限 2 页
      qDebug() << "[INFO] 退出全屏放映，清空笔迹";
      clearAllPages();
      g.maxCachePages = 2;
    }
  }

  // 翻页按钮始终显示
  if (g.prevBtn) g.prevBtn->setVisible(!g.collapsed);
  if (g.nextBtn) g.nextBtn->setVisible(!g.collapsed);

  // 光标模式下定期刷新输入区域，确保侧边栏可点击稳定（XShape 可能因时序/位置变化偶发失效）
  if (g.currentMode == 0) setInputShapeToSidebar();
}

