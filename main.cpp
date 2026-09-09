/*
 * Sidera —— 单窗口架构 Linux Sidera工具 (Wayland + X11 统一)
 * Qt 5.12 / CPU 软件渲染
 *
 * Copyright (C) 2026 Carl_Jin
 *
 * 本程序是自由软件：你可以依据自由软件基金会发布的 GNU 通用公共许可证
 * (GPL) 第 3 版（或按其约定可使用的任何更新版本）的条款，重新分发和/或
 * 修改本程序。详见 https://www.gnu.org/licenses/gpl-3.0.html
 *
 * Sidera（拉丁语：星星）
 *
 * 架构：
 *   单窗口 MainWidget，两种形态：
 *     光标模式 → 54×186 胶囊形侧边栏，桌面不受影响
 *     画笔/橡皮模式 → 全屏透明画布，侧边栏作为子控件嵌入
 *   弹窗也是子控件，无 z-order 问题。
 *
 * 编译：
 *   g++ -std=c++17 -O2 main.cpp $(pkg-config --cflags --libs Qt5Core Qt5Gui Qt5Widgets) -lX11 -lxcb -o annotate_amd64
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QPixmap>
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QIcon>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QTouchEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QSocketNotifier>
#include <QDebug>
#include <QColor>
#include <QPalette>
#include <QFrame>
#include <QTimer>
#include <QMap>
#include <QSlider>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QDialog>
#include <QTextEdit>
#include <QSysInfo>
#include <QWindow>
#include <QDateTime>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QAbstractSocket>
#include <QRegExp>
#include <QQueue>
#include <QUrl>
#include <QUrlQuery>
#include <QFileInfo>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QThread>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XTest.h>

#include <cstdlib>
#include <functional>
#include <QSet>

// ============================================================
// 1. 全局状态
// ============================================================
struct AppState {
  QWidget* mainWidget = nullptr;   // 单窗口
  QPixmap* canvas     = nullptr;   // 绘图缓存

  // 左侧边栏 + 按钮（原名不变）
  QWidget* sidebarArea     = nullptr;
  QPushButton* cursorBtn = nullptr;
  QPushButton* penBtn    = nullptr;
  QPushButton* eraserBtn = nullptr;
  QPushButton* lineBtn   = nullptr;   // 直线

  // 右侧镜像
  QWidget* sidebarAreaRight = nullptr;
  QPushButton *cursorBtnR = nullptr, *penBtnR = nullptr, *eraserBtnR = nullptr, *lineBtnR = nullptr;

  QWidget* penPopup      = nullptr;
  QWidget* eraserPopup   = nullptr;
  bool popupOnRight      = false;   // 弹窗从哪个侧边栏触发的

  // 模式
  int  currentMode = 0;           // 0=光标 1=画笔 2=橡皮擦
  bool penPopupVisible   = false;
  bool eraserPopupVisible = false;

  // 白板模式：透明可穿透画布 ↔ 不透明白板（全屏可输入）
  bool whiteboard = false;
  QPushButton* wbBtnL = nullptr;
  QPushButton* wbBtnR = nullptr;

  // 颜色 + 粗细
  QVector<QColor> colors     = { QColor(255,40,40), QColor(50,120,255), QColor(40,200,60), QColor(255,210,30), QColor(240,240,240) };
  QVector<int> penSizes      = { 3, 6, 10 };
  QVector<int> eraserSizes   = { 12, 24, 48 };
  int curColor   = 0;
  int curPen     = 1;
  int curEraser  = 1;

  QColor penColor()      const { return colors[curColor]; }
  int    penWidth()      const { return penSizes[curPen]; }
  int    eraserWidth()   const { return eraserSizes[curEraser]; }

  // 绘图状态
  bool   isDrawing = false;
  QPoint lastPt;
  // 直线模式
  QPoint lineStart;
  bool   linePreview = false;

  // 侧边栏屏幕坐标（手动跟踪，避免 mapToGlobal 的累积误差）
  QPoint sidebarScreenPos;
  QPoint sidebarScreenPosR;

  // 图标
  QPixmap* iconCursor = nullptr;
  QPixmap* iconPen    = nullptr;
  QPixmap* iconEraser = nullptr;
  QPixmap* iconLine   = nullptr;

  // 侧边栏缩放系数（0.6 ~ 1.4，1.0 为默认）
  double sbScale = 1.0;
  // 侧边栏背景透明度（30 ~ 255，255=不透明）
  int sidebarAlpha = 80;
  // 设置窗口指针
  QWidget* settingsWin = nullptr;

  // 平台
  QString platform  = "unknown";
  bool    hotkeyOk  = false;
  Display* xDisplay = nullptr;
  Window   xRootWin = 0;

  // WPS 联动
  bool wpsFullscreen = false;
  QPushButton* prevBtn = nullptr;
  QPushButton* nextBtn = nullptr;

  // 多页缓存
  QMap<int, QPixmap*> slideCache;  // 页码 → 笔迹
  int currentSlide  = 1;           // 当前页码
  int maxCachePages = 2;           // 非全屏 2 页，全屏 30 页

  // WPS 接口调试模式（教室默认开）：翻页走本地 HTTP 16666，WPS 加载项回传真实页号/事件
  bool         wpsDebug         = true;
  bool         wpsConnected     = false;
  QTcpServer*  wpsServer        = nullptr;
  QTcpSocket*  wpsSock          = nullptr;   // 单个请求连接（HTTP 场景下基本不用）
  QTimer*      wpsPingTimer     = nullptr;   // 3s 无请求判离线
  quint64      wpsLastSeen      = 0;
  QQueue<QString> wpsCmdQueue;               // 待加载项取走的 NEXT/PREV
  int          wpsRealPos       = -1;   // 加载项上报的真实页号（1 起）

  // 手掌自动橡皮（试验，默认开，设置里可关）：多点触控=手掌临时当橡皮
  bool palmEraseOn = true;
  QVector<QPoint> palmErasePreview;   // 当前作为“橡皮”的触点位置（画圆形预览用）
};

// 手掌橡皮直径（“大号”擦除尺寸）
static const int kPalmEraseWidth = 48;

static AppState g;

// ============================================================
// 2. 前向声明
// ============================================================
static void switchToCursorMode();
static void switchToDrawMode(int mode);
static void clearCanvas();
static void initCanvas();
static void closeAllPopups();
static void showPenPopup();
static void showEraserPopup();
static void repositionPopups();
static void updateSidebarStyles();
static void setInputShapeToSidebar();
static void resetInputShape();
static void toggleWhiteboard();
static void updateWhiteboardButtonStyles();
static void sendXTestKey(Display* dpy, KeySym ks);
static void goToPrevPage();
static void goToNextPage();
static void clearAllPages();
static QWidget* createPopup(int w, int h);
static void openSettings();
static void closeSettings();
static void showSystemInfo();

// ============================================================
// 3. 平台检测
// ============================================================
static QString detectPlatform() {
  // 只支持 X11
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
// 5. 图标绘制
// ============================================================
static QPixmap makeCursorIcon(int s) {
  QPixmap p(s,s); p.fill(Qt::transparent);
  QPainter pt(&p); pt.setRenderHint(QPainter::Antialiasing,true);
  QPen pen(Qt::white,2.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);
  pt.setPen(pen); pt.setBrush(QColor(255,255,255,200));
  QPainterPath path;
  float r=s*0.14f;
  path.moveTo(r*2,r); path.lineTo(s-r,s*0.55f);
  path.lineTo(s*0.55f,s*0.55f); path.lineTo(s*0.55f,s-r);
  path.closeSubpath();
  pt.drawPath(path); pt.end();
  return p;
}
static QPixmap makePenIcon(int s) {
  QPixmap p(s,s); p.fill(Qt::transparent);
  QPainter pt(&p); pt.setRenderHint(QPainter::Antialiasing,true);
  float m=s*0.15f;
  pt.setPen(QPen(QColor(255,200,100),3.5,Qt::SolidLine,Qt::RoundCap));
  pt.drawLine(QPointF(s-m,m),QPointF(m+2,s-m-2));
  pt.setPen(QPen(QColor(40,40,40),4.5,Qt::SolidLine,Qt::RoundCap));
  pt.drawLine(QPointF(m+2,s-m-2),QPointF(m*0.3f,s-m*0.3f));
  pt.end(); return p;
}
static QPixmap makeEraserIcon(int s) {
  QPixmap p(s,s); p.fill(Qt::transparent);
  QPainter pt(&p); pt.setRenderHint(QPainter::Antialiasing,true);
  float m=s*0.18f;
  pt.setPen(QPen(QColor(255,180,180),2.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
  pt.setBrush(QColor(255,150,150,200));
  pt.drawRoundedRect(QRectF(m*1.5f,m,s-m*3,s-m*2),m*0.8f,m*0.8f);
  pt.end(); return p;
}
static QPixmap makeLineIcon(int s) {
  QPixmap p(s,s); p.fill(Qt::transparent);
  QPainter pt(&p); pt.setRenderHint(QPainter::Antialiasing,true);
  pt.setPen(QPen(QColor(200,200,255),3,Qt::SolidLine,Qt::RoundCap));
  pt.drawLine(QPointF(s*0.2,s*0.8), QPointF(s*0.8,s*0.2));
  pt.end(); return p;
}

// ============================================================
// 5b. Sidera 图标（PNG，SVG 同图提取）：设置面板 + 启动闪屏展示
// ============================================================
static QString sideraIconPath() {
  // 优先 PNG（程序内不依赖 QtSvg，容器/教室只有 Qt5 基础模块）
  QStringList cands;
  QString env = QString::fromLocal8Bit(qgetenv("SIDERA_ICON"));
  if (!env.isEmpty()) cands << env;
  cands << QCoreApplication::applicationDirPath() + "/sidera.png";
  cands << QDir::currentPath() + "/sidera.png";
  cands << "/usr/share/sidera/sidera.png";
  cands << "/usr/share/icons/hicolor/256x256/apps/sidera.png";
  for (const QString& c : cands) if (QFile::exists(c)) return c;
  return QString();
}

static QPixmap sideraIconPixmap(int px) {
  QString p = sideraIconPath();
  QPixmap pm(px, px);
  pm.fill(Qt::transparent);
  if (!p.isEmpty()) {
    QImage img(p);
    if (!img.isNull())
      pm = QPixmap::fromImage(img.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }
  return pm;
}

// ============================================================
// 6. 侧边栏磨砂背景画笔
// ============================================================
class SidebarPainter : public QObject {
  QWidget* w;
public:
  explicit SidebarPainter(QWidget* widget) : QObject(widget), w(widget) {}
protected:
  bool eventFilter(QObject* obj, QEvent* ev) override {
    if (ev->type() == QEvent::Paint && obj == w) {
      QPainter p(w);
      p.setRenderHint(QPainter::Antialiasing, true);
      QRectF r = w->rect().adjusted(1,1,-1,-1);
      qreal rad = r.width()/2.0;
      p.setBrush(QColor(42,42,50,g.sidebarAlpha));
      p.setPen(Qt::NoPen);
      p.drawRoundedRect(r, rad, rad);
      QLinearGradient g(r.topLeft(), QPointF(r.center().x(), r.top()+r.height()*0.45));
      g.setColorAt(0.0, QColor(255,255,255,20));
      g.setColorAt(0.3, QColor(255,255,255,8));
      g.setColorAt(1.0, QColor(255,255,255,0));
      p.setBrush(g); p.setPen(Qt::NoPen);
      p.drawRoundedRect(r, rad, rad);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(QColor(90,90,100), 2.0));
      p.drawRoundedRect(r, rad, rad);
      p.end();
      return false;
    }
    return QObject::eventFilter(obj, ev);
  }
};

// ============================================================
// 7. 侧边栏尺寸参数（线性缩放，基准为 64px 宽 / 38px 按钮）
// ============================================================
static int sbWidth()  { return int(56 * g.sbScale); }
static int sbBtn()    { return int(34 * g.sbScale); }
static int sbIcon()   { return int(24 * g.sbScale); }
static int sbDot()    { return int(19 * g.sbScale); }
// 高度 = 固定 margins/spacing + 8 个按钮（间距 7 个 + 上下边距 18/14）
static int sbHeight() { return 18 + 14 + 9 * sbBtn() + 8 * 8; }

// 画/擦一段到 g.canvas（显式指定动作与宽度，触摸/鼠标共用）
static void strokeSegment(QPoint a, QPoint b, bool erase, int width) {
  if (!g.canvas) return;
  QPainter p(g.canvas);
  if (erase) {
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    QPen ep(Qt::transparent, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(ep);
  } else {
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    QPen pen(g.penColor(), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setRenderHint(QPainter::Antialiasing, true);
  }
  p.drawLine(a, b);
  p.end();
}

// 画一段笔迹到 g.canvas（画笔/橡皮擦共用，鼠标和触摸都调用）
static void strokeToCanvas(QPoint a, QPoint b) {
  if (g.currentMode == 2) strokeSegment(a, b, true, g.eraserWidth());
  else                    strokeSegment(a, b, false, g.penWidth());
}

// ============================================================
// 7b. 侧边栏拖动（子控件，在父窗口内自由移动）
// ============================================================
class SidebarDragFilter : public QObject {
  bool dragging = false;
  int  dragStartY = 0;
  int  dragGlobalYStart = 0;
  bool isRight;
public:
  explicit SidebarDragFilter(QObject* parent, bool isRight) : QObject(parent), isRight(isRight) {}
protected:
  bool eventFilter(QObject* obj, QEvent* ev) override {
    QWidget* w = qobject_cast<QWidget*>(obj);
    if (ev->type() == QEvent::MouseButtonPress) {
      QMouseEvent* me = static_cast<QMouseEvent*>(ev);
      // 点击侧边栏时终止主画布的进行中笔画（鼠标画线拖到侧边栏上松开时，release 事件给侧边栏，主画布收不到）
      g.isDrawing   = false;
      g.linePreview = false;
      if (qobject_cast<QPushButton*>(w ? w->childAt(me->pos()) : nullptr)) return false;
      if (me->button() == Qt::LeftButton) {
        dragging = true;
        dragStartY      = w ? w->y() : 0;
        dragGlobalYStart = me->globalY();
        if (w) w->grabMouse();
        return true;
      }
    } else if (ev->type() == QEvent::MouseMove && dragging) {
      QMouseEvent* me = static_cast<QMouseEvent*>(ev);
      int newY = dragStartY + (me->globalY() - dragGlobalYStart);
      int maxY = (g.mainWidget ? g.mainWidget->height() : 1080) - (w ? w->height() : sbHeight());
      newY = qMax(0, qMin(newY, maxY));
      int lockedX = isRight ? (g.mainWidget ? g.mainWidget->width() - sbWidth() - 4 : 2560-68) : 4;
      if (w) w->move(lockedX, newY);

      // 同步两侧边栏
      QWidget* other = isRight ? g.sidebarArea : g.sidebarAreaRight;
      if (other) {
        int otherX = isRight ? 4 : (g.mainWidget ? g.mainWidget->width() - sbWidth() - 4 : 2560-68);
        other->move(otherX, newY);
      }
      g.sidebarScreenPos  = QPoint(4, newY);
      g.sidebarScreenPosR = QPoint((g.mainWidget ? g.mainWidget->width() : 2560) - sbWidth() - 4, newY);
      repositionPopups();
      return true;
    } else if (ev->type() == QEvent::MouseButtonRelease && dragging) {
      dragging = false;
      if (w) w->releaseMouse();
      // 拖动后侧边栏位置变了，重新设置输入区域，避免点击穿透
      if (g.currentMode == 0) setInputShapeToSidebar();
      return true;
    }
    return QObject::eventFilter(obj, ev);
  }
};

// ============================================================
// 8. 单窗口类（两种形态）
// ============================================================
class MainWidget : public QWidget {
public:
  explicit MainWidget() {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);  // 接受触摸事件，手指触摸画线不依赖鼠标合成
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                   | Qt::WindowDoesNotAcceptFocus
                   | Qt::BypassWindowManagerHint);
    setMinimumSize(sbWidth(), sbHeight());
    // 初始全屏透明窗口，双侧边栏可见
    QRect scr = QGuiApplication::primaryScreen()->geometry();
    setGeometry(scr);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0,0,0,0));
    setPalette(pal);
    setAutoFillBackground(true);

    // 创建图标
    int isz = 28;
    g.iconCursor = new QPixmap(makeCursorIcon(isz));
    g.iconPen    = new QPixmap(makePenIcon(isz));
    g.iconEraser = new QPixmap(makeEraserIcon(isz));
    g.iconLine   = new QPixmap(makeLineIcon(isz));

    // 创建侧边栏（子控件）
    buildSidebar();

    // 画布缓存
    initCanvas();
  }

  QWidget* makeOneSidebar(QWidget* parent, bool isRight) {
    QWidget* sb = new QWidget(parent);
    sb->setFixedSize(sbWidth(), sbHeight());
    sb->setObjectName("sidebarArea");
    sb->setStyleSheet(
      "#sidebarArea { background: transparent; }"
    );
    sb->installEventFilter(new SidebarPainter(sb));

    QVBoxLayout* lay = new QVBoxLayout(sb);
    lay->setContentsMargins(12, 18, 12, 14);
    lay->setSpacing(8);

    QWidget* dragDot = new QWidget(sb);
    dragDot->setFixedSize(sbDot(), sbDot());
    dragDot->move((sb->width() - sbDot()) / 2, -sbDot()/4);
    dragDot->setStyleSheet(
      QString("background-color: rgba(80,85,100,240);"
      "border-radius: %1px;"
      "border: 1.5px solid #888888;").arg(sbDot()/2)
    );
    dragDot->show();

    auto mk = [](const QPixmap& icon) {
      QPushButton* b = new QPushButton();
      b->setFixedSize(sbBtn(), sbBtn());
      b->setIcon(QIcon(icon));
      b->setIconSize(QSize(sbIcon(), sbIcon()));
      b->setStyleSheet(QString("QPushButton{background:transparent;border:2px solid transparent;border-radius:%1px;}"
                       "QPushButton:hover{background:rgba(255,255,255,0.1);}").arg(sbBtn()/2));
      return b;
    };

    QPushButton* cursorB = mk(*g.iconCursor);
    QObject::connect(cursorB, &QPushButton::clicked, [this, isRight]() { g.popupOnRight = isRight; switchToCursorMode(); });
    lay->addWidget(cursorB);

    QPushButton* penB = mk(*g.iconPen);
    QObject::connect(penB, &QPushButton::clicked, [this, isRight]() { g.popupOnRight = isRight; MainWidget::onPenClicked(); });
    lay->addWidget(penB);

    QPushButton* eraserB = mk(*g.iconEraser);
    QObject::connect(eraserB, &QPushButton::clicked, [this, isRight]() { g.popupOnRight = isRight; MainWidget::onEraserClicked(); });
    lay->addWidget(eraserB);

    QPushButton* lineB = mk(*g.iconLine);
    QObject::connect(lineB, &QPushButton::clicked, [this, isRight]() { g.popupOnRight = isRight; MainWidget::onLineClicked(); });
    lay->addWidget(lineB);

    sb->installEventFilter(new SidebarDragFilter(sb, isRight));

    if (isRight) {
      g.cursorBtnR = cursorB;
      g.penBtnR = penB;
      g.eraserBtnR = eraserB;
      g.lineBtnR = lineB;
      g.sidebarAreaRight = sb;
    } else {
      g.cursorBtn = cursorB;
      g.penBtn = penB;
      g.eraserBtn = eraserB;
      g.lineBtn = lineB;
      g.sidebarArea = sb;
    }

    return sb;
  }

  void buildSidebar() {
    // 左侧边栏
    QWidget* leftSb = makeOneSidebar(this, false);
    // 右侧边栏
    QWidget* rightSb = makeOneSidebar(this, true);

    // 定位到两边
    QRect scr = QGuiApplication::primaryScreen()->geometry();
    int iy = (scr.height() - sbHeight()) / 2;
    leftSb->move(4, iy);
    rightSb->move(scr.width() - sbWidth() - 4, iy);

    // nav 按钮——左侧边栏和右侧边栏各一份
    auto mkNav = [](const QString& arrow) {
      QPushButton* b = new QPushButton(arrow);
      b->setFixedSize(sbBtn(), sbBtn());
      b->setStyleSheet(QString("QPushButton{background:#3a3a4a;color:#ccccff;border:1.5px solid #6688cc;border-radius:%1px;font-size:%2px;font-weight:bold;}"
                       "QPushButton:hover{background:#5555aa;color:#ffffff;}").arg(sbBtn()/2).arg(sbBtn()*12/19));
      return b;
    };
    g.prevBtn = mkNav(QString::fromUtf8("\342\226\262"));
    g.nextBtn = mkNav(QString::fromUtf8("\342\226\274"));
    QObject::connect(g.prevBtn, &QPushButton::clicked, []() { goToPrevPage(); });
    QObject::connect(g.nextBtn, &QPushButton::clicked, []() { goToNextPage(); });
    g.prevBtn->setVisible(true);
    g.nextBtn->setVisible(true);
    qobject_cast<QVBoxLayout*>(leftSb->layout())->addWidget(g.prevBtn);
    qobject_cast<QVBoxLayout*>(leftSb->layout())->addWidget(g.nextBtn);

    QPushButton* prevB = mkNav(QString::fromUtf8("\342\226\262"));
    QPushButton* nextB = mkNav(QString::fromUtf8("\342\226\274"));
    QObject::connect(prevB, &QPushButton::clicked, []() { goToPrevPage(); });
    QObject::connect(nextB, &QPushButton::clicked, []() { goToNextPage(); });
    prevB->setVisible(true); nextB->setVisible(true);
    qobject_cast<QVBoxLayout*>(rightSb->layout())->addWidget(prevB);
    qobject_cast<QVBoxLayout*>(rightSb->layout())->addWidget(nextB);

    // 退出全屏按钮（⛶）——发送 ESC 键让焦点窗口退出全屏，左右各一个
    auto mkFexit = []() {
      QPushButton* b = new QPushButton(QString::fromUtf8("\342\233\266"));
      b->setFixedSize(sbBtn(), sbBtn());
      b->setStyleSheet(QString("QPushButton{background:#3a4a3a;color:#aaffaa;border:1.5px solid #66aa66;border-radius:%1px;font-size:%2px;font-weight:bold;}"
                       "QPushButton:hover{background:#446644;color:#ffffff;}").arg(sbBtn()/2).arg(sbBtn()*12/19));
      return b;
    };
    auto sendEsc = []() {
      Display* dpy = g.xDisplay;
      bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
      if (dpy) { sendXTestKey(dpy, XK_Escape); if (nc) XCloseDisplay(dpy); }
    };
    QPushButton* fexitL = mkFexit();
    QPushButton* fexitR = mkFexit();
    QObject::connect(fexitL, &QPushButton::clicked, sendEsc);
    QObject::connect(fexitR, &QPushButton::clicked, sendEsc);
    qobject_cast<QVBoxLayout*>(leftSb->layout())->addWidget(fexitL);
    qobject_cast<QVBoxLayout*>(rightSb->layout())->addWidget(fexitR);

    // 设置按钮（⚙）——打开独立设置窗口，左右各一个
    auto mkSet = []() {
      QPushButton* b = new QPushButton(QString::fromUtf8("\342\232\231"));
      b->setFixedSize(sbBtn(), sbBtn());
      b->setStyleSheet(QString("QPushButton{background:#3a4a4a;color:#aaddee;border:1.5px solid #55aacc;border-radius:%1px;font-size:%2px;font-weight:bold;}"
                       "QPushButton:hover{background:#446666;color:#ffffff;}").arg(sbBtn()/2).arg(sbBtn()*12/19));
      return b;
    };
    QPushButton* setL = mkSet();
    QPushButton* setR = mkSet();
    QObject::connect(setL, &QPushButton::clicked, []() { openSettings(); });
    QObject::connect(setR, &QPushButton::clicked, []() { openSettings(); });
    qobject_cast<QVBoxLayout*>(leftSb->layout())->addWidget(setL);
    qobject_cast<QVBoxLayout*>(rightSb->layout())->addWidget(setR);

    // 白板按钮（圆角矩形文字按钮）——左右各一个
    auto mkWB = [&](bool isRight) {
      QPushButton* b = new QPushButton(QString::fromUtf8("白板"));
      b->setFixedSize(sbBtn(), sbBtn());
      QObject::connect(b, &QPushButton::clicked, []() { toggleWhiteboard(); });
      if (isRight) g.wbBtnR = b; else g.wbBtnL = b;
      return b;
    };
    QPushButton* wbL = mkWB(false);
    QPushButton* wbR = mkWB(true);
    qobject_cast<QVBoxLayout*>(leftSb->layout())->addWidget(wbL);
    qobject_cast<QVBoxLayout*>(rightSb->layout())->addWidget(wbR);

    updateSidebarStyles();
    updateWhiteboardButtonStyles();
  }

  // ===== 按钮逻辑 =====
  static void onPenClicked() {
    if (g.currentMode == 1) {
      if (g.penPopupVisible) { closeAllPopups(); }
      else { closeAllPopups(); g.penPopupVisible = true; showPenPopup(); }
    } else {
      closeAllPopups();
      switchToDrawMode(1);
    }
  }
  static void onEraserClicked() {
    if (g.currentMode == 2) {
      if (g.eraserPopupVisible) { closeAllPopups(); }
      else { closeAllPopups(); g.eraserPopupVisible = true; showEraserPopup(); }
    } else {
      closeAllPopups();
      switchToDrawMode(2);
    }
  }
  static void onLineClicked() {
    if (g.currentMode == 3) { return; }
    closeAllPopups();
    if (g.currentMode == 0) switchToDrawMode(3);
    else {
      // 直接从画笔/橡皮切到直线：重置画线状态，防止残留
      g.isDrawing   = false;
      g.linePreview = false;
      g.currentMode = 3;
    }
    updateSidebarStyles();
  }

  // ===== 绘制 =====
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), g.whiteboard ? QColor(255, 255, 255) : Qt::transparent);

    // 始终画 Pixmap（光标模式下也可见）
    if (g.canvas) {
      p.setCompositionMode(QPainter::CompositionMode_SourceOver);
      p.drawPixmap(0, 0, *g.canvas);
    }
    if (g.currentMode == 3 && g.linePreview) {
      QPen pen(g.penColor(), g.penWidth(), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      p.setPen(pen);
      p.setRenderHint(QPainter::Antialiasing, true);
      p.drawLine(g.lineStart, g.lastPt);
    }
    // 手掌自动橡皮：圆形半透明预览（直径=擦除宽度）
    if (!g.palmErasePreview.isEmpty()) {
      int ew = (g.currentMode == 2) ? g.eraserWidth() : kPalmEraseWidth;
      p.setRenderHint(QPainter::Antialiasing, true);
      for (const QPoint& c : g.palmErasePreview) {
        QRectF cr(c.x() - ew / 2.0, c.y() - ew / 2.0, ew, ew);
        p.setBrush(QColor(255, 150, 190, 55));
        p.setPen(QPen(QColor(255, 255, 255, 190), 2));
        p.drawEllipse(cr);
      }
    }
    p.end();
  }

  void mousePressEvent(QMouseEvent* ev) override {
    if (g.currentMode == 3) {
      if (ev->button() == Qt::LeftButton) {
        g.lineStart = ev->pos();
        g.linePreview = true;
        g.lastPt = ev->pos();
      }
      return;
    }
    if (g.currentMode == 0 || !g.canvas) return;
    if (ev->button() == Qt::LeftButton) {
      g.isDrawing = true;
      g.lastPt   = ev->pos();
    }
  }

  void mouseMoveEvent(QMouseEvent* ev) override {
    if (g.currentMode == 3 && g.linePreview) {
      g.lastPt = ev->pos();
      update();
      return;
    }
    if (!g.isDrawing || !g.canvas || g.currentMode == 0) return;
    QPoint cur = ev->pos();
    QPoint prev = g.lastPt;
    strokeToCanvas(prev, cur);
    g.lastPt = cur;
    // 局部重绘：只刷新本段包围盒，大幅降低 VM 重绘开销
    int w = g.currentMode == 2 ? g.eraserWidth() : g.penWidth();
    QRect dirty(QPoint(qMin(prev.x(), cur.x()), qMin(prev.y(), cur.y())),
                QPoint(qMax(prev.x(), cur.x()), qMax(prev.y(), cur.y())));
    update(dirty.adjusted(-w, -w, w, w));
  }

  void mouseReleaseEvent(QMouseEvent* ev) override {
    if (g.currentMode == 3 && g.linePreview) {
      g.linePreview = false;
      QPainter p(g.canvas);
      QPen pen(g.penColor(), g.penWidth(), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      p.setPen(pen);
      p.setRenderHint(QPainter::Antialiasing, true);
      p.drawLine(g.lineStart, g.lastPt);
      p.end();
      update();
      return;
    }
    Q_UNUSED(ev);
    if (g.isDrawing) { g.isDrawing = false; update(); }
  }

  // 手指触摸画线：不经过 grabMouse（XGrabPointer 会让红外触摸的 move 事件丢失）
  // QWidget 没有 touchEvent 虚函数，触摸事件走 event()
  bool event(QEvent* ev) override {
    if (ev->type() == QEvent::TouchBegin || ev->type() == QEvent::TouchUpdate ||
        ev->type() == QEvent::TouchEnd) {
      QTouchEvent* te = static_cast<QTouchEvent*>(ev);
      const QList<QTouchEvent::TouchPoint>& pts = te->touchPoints();
      QPoint cur = pts.isEmpty() ? QPoint() : pts.first().pos().toPoint();

      // 触摸点在侧边栏/弹窗上 → 不画线，返回 false 让 Qt 合成鼠标给正确的子控件（按钮）
      // 同时终止画线状态，防止画到侧边栏时状态泄漏（残留 isDrawing 导致合成鼠标继续画线）
      auto inWidget = [&](QWidget* w) {
        return w && w->isVisible() && w->geometry().contains(cur);
      };
      if (inWidget(g.sidebarArea) || inWidget(g.sidebarAreaRight) ||
          inWidget(g.penPopup) || inWidget(g.eraserPopup)) {
        g.isDrawing   = false;
        g.linePreview = false;
        return false;
      }

      // 光标模式或画布无效 → 不画线，返回 false（Qt 合成鼠标或穿透桌面）
      if (g.currentMode == 0 || !g.canvas) return false;

      // 手掌自动橡皮（试验）：开启时走多点手势引擎；关闭走原逻辑
      if (g.palmEraseOn) { handlePalmTouch(te); return true; }

      switch (ev->type()) {
        case QEvent::TouchBegin: {
          if (g.currentMode == 3) {
            g.lineStart = cur;
            g.linePreview = true;
            g.lastPt = cur;
          } else {
            g.isDrawing = true;
            g.lastPt = cur;
          }
          break;
        }
        case QEvent::TouchUpdate: {
          if (g.currentMode == 3) {
            if (g.linePreview) { g.lastPt = cur; update(); }
          } else if (g.isDrawing) {
            QPoint prev = g.lastPt;
            strokeToCanvas(prev, cur);
            g.lastPt = cur;
            int w = g.currentMode == 2 ? g.eraserWidth() : g.penWidth();
            QRect dirty(QPoint(qMin(prev.x(), cur.x()), qMin(prev.y(), cur.y())),
                        QPoint(qMax(prev.x(), cur.x()), qMax(prev.y(), cur.y())));
            update(dirty.adjusted(-w, -w, w, w));
          }
          break;
        }
        case QEvent::TouchEnd: {
          if (g.currentMode == 3 && g.linePreview) {
            g.linePreview = false;
            strokeToCanvas(g.lineStart, g.lastPt);
            update();
          } else if (g.isDrawing) {
            g.isDrawing = false;
            update();
          }
          break;
        }
        default: break;
      }
      return true;  // 接受触摸，避免再合成鼠标事件导致重复处理
    }
    return QWidget::event(ev);
  }

  void keyPressEvent(QKeyEvent* ev) override {
    QWidget::keyPressEvent(ev);
  }

  void resizeEvent(QResizeEvent* ev) override {
    QWidget::resizeEvent(ev);
    // 画布尺寸始终跟随窗口（分辨率变化时任意模式都不错位）
    if (g.canvas && g.canvas->size() != size()) {
      QPixmap* old = g.canvas;
      g.canvas = new QPixmap(size());
      g.canvas->fill(Qt::transparent);
      QPainter p(g.canvas); p.drawPixmap(0,0,*old); p.end();
      delete old;
    }
    repositionPopups();
  }

  // ---- 手掌自动橡皮（试验）：单点=笔；一旦多点，整体当橡皮（不“边写边擦”） ----
  QMap<int, QPoint> m_tPrevPos;
  QMap<int, int>    m_tPrevRole;      // 1=笔, 2=橡皮
  QVector<QPoint>  m_prevPreview;

  void handlePalmTouch(QTouchEvent* te) {
    g.isDrawing = false;
    const QList<QTouchEvent::TouchPoint>& pts = te->touchPoints();
    QMap<int, QPoint> now;
    for (const QTouchEvent::TouchPoint& tp : pts)
      if (tp.state() != Qt::TouchPointReleased) now.insert(tp.id(), tp.pos().toPoint());

    const int mode = g.currentMode;
    const bool modeEraseOnly = (mode == 2);
    const int n = now.size();
    // 角色：仅剩 1 点才可能当笔；≥2 点全部当橡皮（直接切换，无并发画笔）
    bool anyPen = (!modeEraseOnly && n == 1);

    QMap<int,int> curRoleMap;
    for (auto it = now.begin(); it != now.end(); ++it) {
      int id = it.key();
      QPoint pos = it.value();
      int curRole = anyPen ? 1 : 2;
      curRoleMap[id] = curRole;

      int oldRole = m_tPrevRole.value(id, -1);
      QPoint oldPos = m_tPrevPos.value(id);
      bool haveOld = m_tPrevPos.contains(id);

      if (curRole != oldRole || !haveOld) {   // 新触点或角色变化：记基准，不连笔/连擦
        m_tPrevPos[id] = pos;
        m_tPrevRole[id] = curRole;
        continue;
      }
      if (oldPos == pos) continue;

      if (curRole == 1) {
        if (mode == 1) {
          strokeSegment(oldPos, pos, false, g.penWidth());
          int w = g.penWidth();
          QRect d(QRect(QPoint(qMin(oldPos.x(),pos.x()), qMin(oldPos.y(),pos.y())),
                        QPoint(qMax(oldPos.x(),pos.x()), qMax(oldPos.y(),pos.y()))).adjusted(-w,-w,w,w));
          update(d);
        } else if (mode == 3) {
          if (!g.linePreview) { g.lineStart = oldPos; g.linePreview = true; g.lastPt = oldPos; }
          g.lastPt = pos;
        }
      } else {                                 // 橡皮（多点=手掌 / 橡皮模式）
        int ew = (mode == 2) ? g.eraserWidth() : kPalmEraseWidth;
        strokeSegment(oldPos, pos, true, ew);
        QRect d(QRect(QPoint(qMin(oldPos.x(),pos.x()), qMin(oldPos.y(),pos.y())),
                      QPoint(qMax(oldPos.x(),pos.x()), qMax(oldPos.y(),pos.y()))).adjusted(-ew,-ew,ew,ew));
        update(d);
      }
      m_tPrevPos[id] = pos;
      m_tPrevRole[id] = curRole;
    }

    // 进入多点时取消直线预览（整体切橡皮）
    if (n >= 2) g.linePreview = false;

    // 清理抬起的触点
    for (auto it = m_tPrevPos.begin(); it != m_tPrevPos.end();) {
      if (!now.contains(it.key())) it = m_tPrevPos.erase(it); else ++it;
    }
    for (auto it = m_tPrevRole.begin(); it != m_tPrevRole.end();) {
      if (!now.contains(it.key())) it = m_tPrevRole.erase(it); else ++it;
    }

    // 圆形橡皮预览
    QVector<QPoint> prev = m_prevPreview;
    g.palmErasePreview.clear();
    for (auto it = now.begin(); it != now.end(); ++it)
      if (curRoleMap.value(it.key(), 2) == 2) g.palmErasePreview << it.value();
    QVector<QPoint> all = prev; for (const QPoint& q : g.palmErasePreview) all << q;
    if (!all.isEmpty()) {
      int pad = (mode == 2 ? g.eraserWidth() : kPalmEraseWidth) / 2 + 4;
      QRect u(all.first(), all.first());
      for (const QPoint& q : all) u = u.united(QRect(q, q));
      update(u.adjusted(-pad, -pad, pad, pad));
    }
    m_prevPreview = g.palmErasePreview;

    if (now.isEmpty()) {
      g.palmErasePreview.clear();
      if (mode == 3 && g.linePreview) {          // 结束直线
        g.linePreview = false;
        strokeSegment(g.lineStart, g.lastPt, false, g.penWidth());
      }
      m_prevPreview.clear();
      update();
    }
  }
};

// ============================================================
// 8. 弹窗基类（子控件，在 mainWidget 内）
// ============================================================
static QWidget* createPopup(int w, int h) {
  QWidget* pop = new QWidget(g.mainWidget);
  pop->setFixedSize(w, h);
  pop->setObjectName("popup");
  pop->setStyleSheet(
    "#popup { background-color: rgba(42,42,50,245); border: 2px solid #666666; border-radius: 14px; }"
    "QPushButton { background-color: #444444; color: #ffffff; border: 2px solid #666666; border-radius: 6px;"
    "font-size: 14px; font-weight: bold; padding: 8px; }"
    "QPushButton:hover { background-color: #555555; }"
    "QLabel { color: #cccccc; font-size: 12px; font-weight: bold; }"
  );
  pop->hide();
  return pop;
}

static void showPenPopup() {
  if (g.penPopup) { g.penPopup->deleteLater(); g.penPopup = nullptr; }
  QWidget* pop = createPopup(220, 170);
  g.penPopup = pop;
  QVBoxLayout* lay = new QVBoxLayout(pop);
  lay->setContentsMargins(12,10,12,10); lay->setSpacing(8);

  lay->addWidget(new QLabel(QString::fromUtf8("画笔颜色")));
  QHBoxLayout* cr = new QHBoxLayout(); cr->setSpacing(8);
  for (int i=0;i<g.colors.size();i++) {
    QPushButton* cb = new QPushButton(); cb->setFixedSize(30,30);
    QString hex = g.colors[i].name();
    cb->setStyleSheet(QString("QPushButton{background:%1;border:%2px solid %3;border-radius:15px;}")
      .arg(hex).arg(i==g.curColor?3:2).arg(i==g.curColor?"#ffffff":"#666666"));
    QObject::connect(cb, &QPushButton::clicked, [i](){ g.curColor=i; showPenPopup(); repositionPopups(); });
    cr->addWidget(cb);
  }
  cr->addStretch(); lay->addLayout(cr);

  lay->addWidget(new QLabel(QString::fromUtf8("画笔粗细")));
  QHBoxLayout* sr = new QHBoxLayout(); sr->setSpacing(8);
  QStringList sl = {QString::fromUtf8("细 3"), QString::fromUtf8("中 6"), QString::fromUtf8("粗 10")};
  for (int i=0;i<g.penSizes.size();i++) {
    QPushButton* sb = new QPushButton(sl[i]); sb->setFixedHeight(36);
    if (i==g.curPen) sb->setStyleSheet("QPushButton{background:#3377cc;color:#fff;border:2px solid #5599ff;border-radius:6px;font-size:14px;font-weight:bold;}");
    QObject::connect(sb, &QPushButton::clicked, [i](){ g.curPen=i; showPenPopup(); repositionPopups(); });
    sr->addWidget(sb);
  }
  lay->addLayout(sr);
  pop->show(); pop->raise();
  repositionPopups();
  if (g.currentMode == 0) setInputShapeToSidebar();
}

static void showEraserPopup() {
  if (g.eraserPopup) { g.eraserPopup->deleteLater(); g.eraserPopup = nullptr; }
  QWidget* pop = createPopup(220, 175);
  g.eraserPopup = pop;
  QVBoxLayout* lay = new QVBoxLayout(pop);
  lay->setContentsMargins(12,10,12,10); lay->setSpacing(8);

  lay->addWidget(new QLabel(QString::fromUtf8("橡皮擦大小")));
  QHBoxLayout* sr = new QHBoxLayout(); sr->setSpacing(8);
  QStringList sl = {QString::fromUtf8("小 12"), QString::fromUtf8("中 24"), QString::fromUtf8("大 48")};
  for (int i=0;i<g.eraserSizes.size();i++) {
    QPushButton* sb = new QPushButton(sl[i]); sb->setFixedHeight(36);
    if (i==g.curEraser) sb->setStyleSheet("QPushButton{background:#ff8800;color:#000;border:2px solid #ffaa00;border-radius:6px;font-size:14px;font-weight:bold;}");
    QObject::connect(sb, &QPushButton::clicked, [i](){ g.curEraser=i; showEraserPopup(); repositionPopups(); });
    sr->addWidget(sb);
  }
  lay->addLayout(sr);
  lay->addSpacing(6);
  QPushButton* cb = new QPushButton(QString::fromUtf8("清除全部"));
  cb->setFixedHeight(38);
  cb->setStyleSheet("QPushButton{background:#555;font-size:14px;font-weight:bold;border-radius:6px;}QPushButton:hover{background:#666;}");
  QObject::connect(cb, &QPushButton::clicked, [](){ clearCanvas(); });
  lay->addWidget(cb);
  pop->show(); pop->raise();
  repositionPopups();
  if (g.currentMode == 0) setInputShapeToSidebar();
}

static void closeAllPopups() {
  g.penPopupVisible = g.eraserPopupVisible = false;
  if (g.penPopup) { g.penPopup->hide(); g.penPopup->deleteLater(); g.penPopup = nullptr; }
  if (g.eraserPopup) { g.eraserPopup->hide(); g.eraserPopup->deleteLater(); g.eraserPopup = nullptr; }
  // 光标模式下重新计算输入区域（去掉弹窗矩形）
  if (g.currentMode == 0) setInputShapeToSidebar();
}

static void repositionPopups() {
  QWidget* ref = g.popupOnRight ? g.sidebarAreaRight : g.sidebarArea;
  if (!ref) return;
  QPoint sb = ref->pos();
  int sbW = ref->width(), sbH = ref->height();
  int parentW = g.mainWidget ? g.mainWidget->width() : 1920;
  auto pos = [&](QWidget* p) {
    if (!p || !p->isVisible()) return;
    int y = sb.y() + (sbH - p->height())/2;
    y = qMax(0, qMin(y, (g.mainWidget?g.mainWidget->height():1080) - p->height()));
    int x = g.popupOnRight ? (sb.x() - p->width() - 8) : (sb.x() + sbW + 8);
    p->move(qMax(0, qMin(x, parentW - p->width())), y);
  };
  pos(g.penPopup); pos(g.eraserPopup);
}

// ============================================================
// 9. 模式切换
// ============================================================
/*
 * 光标模式：小窗口（54×186），桌面可正常操作
 * 绘画模式：全屏窗口，侧边栏嵌入内部
 */
static void switchToCursorMode() {
  closeAllPopups();
  g.currentMode = 0;
  if (!g.mainWidget || !g.sidebarArea) return;

  // 清空绘图状态并释放鼠标抓取（中断笔画也不残留）
  g.isDrawing = false;
  g.linePreview = false;
  g.mainWidget->releaseMouse();

  // 只切模式，光标模式只侧边栏可点击，其余穿透桌面
  updateSidebarStyles();
  setInputShapeToSidebar();

  qDebug() << "[INFO] 光标模式";
}

static void switchToDrawMode(int mode) {
  closeAllPopups();
  bool wasAlreadyDrawing = (g.currentMode != 0);
  g.currentMode = mode;
  if (!g.mainWidget || !g.sidebarArea) return;

  // 清空绘图状态，避免残留的 isDrawing/linePreview 导致幽灵线
  g.isDrawing = false;
  g.linePreview = false;

  // 只在窗口非全屏时扩窗（首次启动或从缩小的光标模式进入）
  QRect scr = QGuiApplication::primaryScreen()->geometry();
  bool needExpand = (g.mainWidget->size() != scr.size());

  if (!wasAlreadyDrawing && needExpand) {
    QPoint targetScreenPos = g.sidebarScreenPos;
    QPoint targetScreenPosR = g.sidebarScreenPosR;

    g.mainWidget->hide();

    g.mainWidget->setMinimumSize(0, 0);
    g.mainWidget->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    g.mainWidget->setGeometry(scr);

    g.sidebarArea->move(targetScreenPos);
    if (g.sidebarAreaRight) g.sidebarAreaRight->move(targetScreenPosR);
    g.sidebarArea->raise();

    initCanvas();

    g.mainWidget->show();
  }

  updateSidebarStyles();
  if (g.prevBtn) g.prevBtn->setVisible(true);
  if (g.nextBtn) g.nextBtn->setVisible(true);
  resetInputShape();

  qDebug() << "[INFO] 绘画模式:" << (mode == 1 ? "画笔" : (mode == 2 ? "橡皮擦" : "直线"));
}

static void updateSidebarStyles() {
  auto style = [](int m) {
    if (g.currentMode == m) {
      if (m==0) return QString("QPushButton{background:rgba(255,255,255,0.2);border:2px solid #aaa;border-radius:19px;}");
      if (m==1) return QString("QPushButton{background:rgba(255,100,100,0.3);border:2px solid #f66;border-radius:19px;}");
      if (m==2) return QString("QPushButton{background:rgba(255,150,100,0.3);border:2px solid #f84;border-radius:19px;}");
      return QString("QPushButton{background:rgba(255,255,255,0.2);border:2px solid #c9f;border-radius:19px;}");
    }
    return QString("QPushButton{background:transparent;border:2px solid transparent;border-radius:19px;}"
                   "QPushButton:hover{background:rgba(255,255,255,0.1);}");
  };
  if (g.cursorBtn) g.cursorBtn->setStyleSheet(style(0));
  if (g.penBtn)    g.penBtn->setStyleSheet(style(1));
  if (g.eraserBtn) g.eraserBtn->setStyleSheet(style(2));
  if (g.lineBtn)   g.lineBtn->setStyleSheet(style(3));
  if (g.cursorBtnR) g.cursorBtnR->setStyleSheet(style(0));
  if (g.penBtnR)    g.penBtnR->setStyleSheet(style(1));
  if (g.eraserBtnR) g.eraserBtnR->setStyleSheet(style(2));
  if (g.lineBtnR)   g.lineBtnR->setStyleSheet(style(3));
}

// ============================================================
// 10. 画布操作
// ============================================================
static void initCanvas() {
  QSize sz = QGuiApplication::primaryScreen()->size();
  if (g.canvas && g.canvas->size() == sz) return;  // 尺寸没变，复用
  if (g.canvas) delete g.canvas;
  g.canvas = new QPixmap(sz);
  g.canvas->fill(Qt::transparent);
}

static void clearCanvas() {
  if (g.canvas) g.canvas->fill(Qt::transparent);
  if (g.mainWidget) g.mainWidget->update();
}

// ============================================================
// WPS 联动：翻页 + 全屏检测
// ============================================================

/*
 * XShape 输入区域：光标模式只让侧边栏 + 可见弹窗可点击，其余区域穿透桌面
 *
 * 用 XShapeCombineMask（1bit 位图）实现：黑=穿透，白=可输入。
 * 比 XShapeCombineRectangles 更基础可靠——后者在部分驱动下矩形合并/排序会静默失败，
 * 导致覆盖漂移（侧边栏部分按键穿透 / 画布偶尔不穿透）。
 */
static void setInputShapeToSidebar() {
  if (!g.mainWidget || !g.mainWidget->isVisible()) return;
  if (g.whiteboard) return;            // 白板模式：全屏可输入，不做侧边栏穿透限制
  Display* dpy = g.xDisplay;
  bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (!dpy) return;

  Window wnd = (Window)g.mainWidget->winId();
  // 真实 X 窗口物理尺寸。显示缩放/DPI>100% 时它 > Qt 逻辑尺寸（winId 窗口 =
  // Qt 逻辑尺寸 × scale factor）。掩码必须按物理尺寸建并换算坐标，否则 1bit 位图
  // 只覆盖窗口左上部分，右侧/底部无输入区域 → 点击穿透。
  Window rootRet = None;
  int rootX = 0, rootY = 0;
  unsigned int XW = 0, XH = 0, bw = 0, depth = 0;
  if (!XGetGeometry(dpy, wnd, &rootRet, &rootX, &rootY, &XW, &XH, &bw, &depth) ||
      XW == 0 || XH == 0) {
    if (nc) XCloseDisplay(dpy);
    return;
  }
  int lw = g.mainWidget->width();
  int lh = g.mainWidget->height();
  if (lw <= 0 || lh <= 0) { if (nc) XCloseDisplay(dpy); return; }
  // 逻辑像素 → 物理像素比例（1:1 缩放时为 1.0，行为与修复前一致）
  double sx = double(XW) / double(lw);
  double sy = double(XH) / double(lh);

  // 创建 1bit 掩码位图（物理尺寸）
  Pixmap mask = XCreatePixmap(dpy, wnd, XW, XH, 1);
  GC gc = XCreateGC(dpy, mask, 0, nullptr);
  // 全部置黑（默认穿透）
  XSetForeground(dpy, gc, 0);
  XFillRectangle(dpy, mask, gc, 0, 0, XW, XH);
  // 侧边栏/弹窗区域置白（可输入）：子控件 pos/size 是逻辑值，换算成物理像素
  XSetForeground(dpy, gc, 1);
  auto fillWhite = [&](QWidget* w) {
    if (!w || !w->isVisible()) return;
    QPoint p = w->pos();
    // 四周各留 4px 逻辑余量，随缩放换算
    int x0 = qMax(0, qRound((p.x() - 4) * sx));
    int y0 = qMax(0, qRound((p.y() - 4) * sy));
    int ww = qMin(int(XW) - x0, qRound((w->width() + 8) * sx));
    int wh = qMin(int(XH) - y0, qRound((w->height() + 8) * sy));
    if (ww <= 0 || wh <= 0) return;
    XFillRectangle(dpy, mask, gc, x0, y0, ww, wh);
  };
  fillWhite(g.sidebarArea);
  fillWhite(g.sidebarAreaRight);
  fillWhite(g.penPopup);
  fillWhite(g.eraserPopup);
  XFreeGC(dpy, gc);

  XShapeCombineMask(dpy, wnd, ShapeInput, 0, 0, mask, ShapeSet);
  XFreePixmap(dpy, mask);
  XFlush(dpy);
  if (nc) XCloseDisplay(dpy);
}

static void resetInputShape() {
  if (!g.mainWidget || !g.mainWidget->isVisible()) return;
  Display* dpy = g.xDisplay;
  bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (!dpy) return;
  XShapeCombineMask(dpy, g.mainWidget->winId(), ShapeInput, 0, 0, None, ShapeSet);
  XFlush(dpy);
  if (nc) XCloseDisplay(dpy);
}

// ============================================================
// 白板模式：整窗不透明（白底）且全屏可输入；再点还原透明可穿透
// ============================================================
static void updateWhiteboardButtonStyles() {
  auto paint = [](QPushButton* b) {
    if (!b) return;
    if (g.whiteboard)
      b->setStyleSheet(QString("QPushButton{background:#f4f4f4;color:#111;border:2px solid #ff9800;border-radius:%1px;font-weight:bold;font-size:%2px;}"
                       "QPushButton:hover{background:#ffffff;}").arg(int(sbBtn()*0.35)).arg(sbBtn()*16/38));
    else
      b->setStyleSheet(QString("QPushButton{background:#555;color:#eee;border:2px solid #999;border-radius:%1px;font-size:%2px;}"
                       "QPushButton:hover{background:#777;}").arg(int(sbBtn()*0.35)).arg(sbBtn()*16/38));
  };
  paint(g.wbBtnL);
  paint(g.wbBtnR);
}

static void toggleWhiteboard() {
  g.whiteboard = !g.whiteboard;
  updateWhiteboardButtonStyles();
  if (g.mainWidget) {
    if (g.whiteboard) resetInputShape();          // 全屏可输入
    else if (g.currentMode == 0) setInputShapeToSidebar();
    else resetInputShape();
    g.mainWidget->update();                       // 触发白底/透明重绘
  }
  qDebug() << (g.whiteboard ? "[INFO] 白板模式开启" : "[INFO] 白板模式关闭");
}

/*
 * XTEST 假键盘。Up/Down 方向键 GNOME 不全局抓取。
 */
static void sendXTestKey(Display* dpy, KeySym ks) {
  KeyCode kc = XKeysymToKeycode(dpy, ks);
  if (kc == 0) return;
  XTestFakeKeyEvent(dpy, kc, True,  0);
  XTestFakeKeyEvent(dpy, kc, False, 0);
  XFlush(dpy);
}

// ============================================================
// 合成器检测：无合成器时 X11 透明窗口会失效（显示不透明）
// ============================================================
static bool hasCompositor() {
  Display* dpy = g.xDisplay;
  bool nc = false;
  if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (!dpy) return false;
  Window cm = XGetSelectionOwner(dpy, XInternAtom(dpy, "_NET_WM_CM_S0", False));
  if (nc) XCloseDisplay(dpy);
  return cm != None;
}

// ============================================================
// 多页缓存：翻页时保存/加载笔迹
// ============================================================
static void clearAllPages() {
  qDebug() << "[INFO] clearAllPages 被调用，清空" << g.slideCache.size() << "页缓存 + 画布";
  for (auto* pix : g.slideCache) delete pix;
  g.slideCache.clear();
  g.currentSlide = 1;

  // 重置绘图状态，防止残留（幽灵线 / 未完成的笔画）
  g.isDrawing   = false;
  g.linePreview = false;
  g.lastPt  = QPoint();
  g.lineStart = QPoint();

  // 清空当前画布
  if (g.canvas) g.canvas->fill(Qt::transparent);

  // 异步重绘（不用 repaint：同步重绘可能阻塞事件循环，导致翻页按钮点击丢失）
  if (g.mainWidget) g.mainWidget->update();
}

static void saveCurrentPage() {
  if (!g.canvas) return;
  // 限制缓存页数
  if (g.slideCache.size() >= (unsigned)g.maxCachePages && !g.slideCache.contains(g.currentSlide)) return;
  // 深拷贝当前画布
  delete g.slideCache.value(g.currentSlide);
  g.slideCache[g.currentSlide] = new QPixmap(*g.canvas);
}

static void loadPage(int page) {
  if (!g.canvas) return;
  g.canvas->fill(Qt::transparent);
  QPixmap* cached = g.slideCache.value(page, nullptr);
  if (cached) {
    QPainter p(g.canvas);
    p.drawPixmap(0, 0, *cached);
    p.end();
  }
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
static QString wpsLogFile() {
  QString dir = QDir::homePath();
  if (!QFileInfo(dir).isWritable()) dir = "/tmp";
  return dir + "/wps-api-debug.log";
}

static void wpsLog(const QString& msg) {
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
static void wpsSaveSettings() {
  QDir().mkpath(QFileInfo(wpsConfigFile()).absolutePath());
  QFile f(wpsConfigFile());
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
  QTextStream ts(&f);
  ts << "wpsDebug=" << (g.wpsDebug ? 1 : 0) << "\n";
  ts << "sbScale=" << QString::number(g.sbScale, 'f', 2) << "\n";
  ts << "sidebarAlpha=" << g.sidebarAlpha << "\n";
  ts << "palmErase=" << (g.palmEraseOn ? 1 : 0) << "\n";
  f.close();
}
static void wpsLoadSettings() {
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
    else if (k == "palmErase") g.palmEraseOn = (v == "1");
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

static void stopWpsApiServer() {
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

static void setWpsDebug(bool on, bool persist = true) {
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

static void goToPrevPage() {
  // 调试模式：只“推进一步”，缓存由加载项回传的真实页号事件驱动（动画步不动缓存）
  bool debugNoCache = g.wpsDebug && g.wpsConnected;   // 无客户端时静默回退“点击即缓存”
  if (debugNoCache) wpsLog("调试：按钮仅发送 Up，缓存等待真实页号事件");
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

static void goToNextPage() {
  // 调试模式：只“推进一步”，缓存由加载项回传的真实页号事件驱动（动画步不动缓存）
  bool debugNoCache = g.wpsDebug && g.wpsConnected;   // 无客户端时静默回退“点击即缓存”
  if (debugNoCache) wpsLog("调试：按钮仅发送 Down，缓存等待真实页号事件");
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
static void checkWpsState() {
  Display* dpy = g.xDisplay;
  bool needClose = false;
  if (!dpy) { dpy = XOpenDisplay(nullptr); needClose = true; }
  if (!dpy) return;

  bool was = g.wpsFullscreen;
  g.wpsFullscreen = isPresentationFullscreen(dpy);

  if (needClose) XCloseDisplay(dpy);

  if (was != g.wpsFullscreen) {
    if (g.wpsFullscreen) {
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
  if (g.prevBtn) g.prevBtn->setVisible(true);
  if (g.nextBtn) g.nextBtn->setVisible(true);

  // 光标模式下定期刷新输入区域，确保侧边栏可点击稳定（XShape 可能因时序/位置变化偶发失效）
  if (g.currentMode == 0) setInputShapeToSidebar();
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
// 12. 设置面板（普通置顶窗口，打开时画布临时隐藏）
// ============================================================

// 重建两侧边栏（大小改变时调用），保留当前位置
static void rebuildSidebars() {
  if (!g.mainWidget) return;
  int keepY = g.sidebarScreenPos.y();
  MainWidget* mw = static_cast<MainWidget*>(g.mainWidget);
  // 删除旧侧边栏
  delete g.sidebarArea; g.sidebarArea = nullptr;
  delete g.sidebarAreaRight; g.sidebarAreaRight = nullptr;
  g.cursorBtn = g.penBtn = g.eraserBtn = g.lineBtn = nullptr;
  g.cursorBtnR = g.penBtnR = g.eraserBtnR = g.lineBtnR = nullptr;
  g.prevBtn = g.nextBtn = nullptr;
  g.wbBtnL = g.wbBtnR = nullptr;
  // 重建
  mw->buildSidebar();
  // 恢复贴边位置（Y 保留原值，约束在新高度内）
  QRect scr = QGuiApplication::primaryScreen()->geometry();
  int iy = qMin(qMax(keepY, 0), scr.height() - sbHeight());
  g.sidebarArea->move(4, iy);
  g.sidebarAreaRight->move(scr.width() - sbWidth() - 4, iy);
  g.sidebarScreenPos  = QPoint(4, iy);
  g.sidebarScreenPosR = QPoint(scr.width() - sbWidth() - 4, iy);
  if (g.currentMode == 0) setInputShapeToSidebar();
}

// 开机自启动：检测 .desktop 是否存在于 autostart
static QString autoStartPath() {
  QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";
  QDir().mkpath(dir);
  return dir + "/sidera.desktop";
}
static bool isAutoStart() {
  return QFile::exists(autoStartPath());
}
static void setAutoStart(bool on) {
  QString path = autoStartPath();
  if (on) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
      QTextStream ts(&f);
      ts << "[Desktop Entry]\n"
         << "Type=Application\n"
         << "Name=Sidera\n"
         << "Comment=Sidera软件\n"
         << "Exec=sidera\n"
         << "Terminal=false\n";
      f.close();
    }
  } else {
    QFile::remove(path);
  }
}

// 恢复被设置窗口隐藏的主画布并重建输入区域
// 设置窗口可能以多种方式关闭：点标题栏 X、点“完成”。统一走这里收尾，
// 否则主画布会一直隐藏、应用看起来“消失”。
static void restoreCanvasAfterSettings() {
  if (!g.mainWidget) return;
  g.mainWidget->show();
  if (g.currentMode == 0) setInputShapeToSidebar();
  else resetInputShape();
}

// 设置窗口
// 滑块行：滑块 + 数值标签
static QHBoxLayout* makeSliderRow(QSlider* slider, QLabel* val) {
  QHBoxLayout* row = new QHBoxLayout();
  row->addWidget(slider, 1);
  val->setFixedWidth(36);
  val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  val->setStyleSheet("color:#aaddff;font-weight:bold;");
  row->addWidget(val);
  return row;
}

static void openSettings() {
  if (g.settingsWin) { g.settingsWin->raise(); g.settingsWin->activateWindow(); return; }
  if (g.mainWidget) g.mainWidget->hide();  // 隐藏画布，让普通设置窗口不被 override-redirect 盖住

  QWidget* win = new QWidget();
  g.settingsWin = win;
  // 关闭即销毁：标题栏 X 也会触发 destroyed，从而执行恢复画布的回调
  win->setAttribute(Qt::WA_DeleteOnClose, true);
  win->setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
  win->setWindowTitle(QString::fromUtf8("Sidera - 设置"));
  win->setFixedWidth(300);
  win->setStyleSheet(
    "QWidget{background:#2b2b33;color:#eee;font-size:14px;}"
    "QLabel{color:#ccc;}"
    "QPushButton{background:#444;color:#fff;border:1px solid #666;border-radius:6px;padding:8px;}"
    "QPushButton:hover{background:#555;}"
    "QSlider::groove:horizontal{height:6px;background:#444;border-radius:3px;}"
    "QSlider::sub-page:horizontal{background:#3377cc;border-radius:3px;}"
    "QSlider::handle:horizontal{width:16px;margin:-5px 0;background:#fff;border-radius:8px;}"
  );

  QVBoxLayout* lay = new QVBoxLayout(win);
  lay->setContentsMargins(16, 16, 16, 12);
  lay->setSpacing(10);

  // 顶部居中：Sidera 图标
  {
    QLabel* iconLbl = new QLabel();
    iconLbl->setPixmap(sideraIconPixmap(56));
    iconLbl->setAlignment(Qt::AlignCenter);
    lay->addWidget(iconLbl);
  }

  // 透明度
  lay->addWidget(new QLabel(QString::fromUtf8("侧边栏透明度")));
  QSlider* alphaSlider = new QSlider(Qt::Horizontal);
  alphaSlider->setRange(30, 255);
  alphaSlider->setValue(g.sidebarAlpha);
  QLabel* alphaVal = new QLabel(QString::number(g.sidebarAlpha));
  QObject::connect(alphaSlider, &QSlider::valueChanged, [alphaVal](int v) {
    g.sidebarAlpha = v;
    alphaVal->setText(QString::number(v));
    // 触发两侧边栏重绘（背景由 SidebarPainter 用 g.sidebarAlpha 画）
    if (g.sidebarArea) g.sidebarArea->update();
    if (g.sidebarAreaRight) g.sidebarAreaRight->update();
    wpsSaveSettings();
  });
  lay->addLayout(makeSliderRow(alphaSlider, alphaVal));

  // 大小
  lay->addWidget(new QLabel(QString::fromUtf8("侧边栏大小")));
  QSlider* sizeSlider = new QSlider(Qt::Horizontal);
  sizeSlider->setRange(60, 140);
  sizeSlider->setValue(int(g.sbScale * 100));
  QLabel* sizeVal = new QLabel(QString::number(g.sbScale, 'f', 1));
  QObject::connect(sizeSlider, &QSlider::valueChanged, [sizeVal](int v) {
    g.sbScale = v / 100.0;
    sizeVal->setText(QString::number(g.sbScale, 'f', 1));
    rebuildSidebars();
    wpsSaveSettings();
  });
  lay->addLayout(makeSliderRow(sizeSlider, sizeVal));

  lay->addSpacing(4);

  // 开机自启动切换按键
  QPushButton* autoBtn = new QPushButton();
  auto updateAutoBtn = [autoBtn]() {
    bool on = isAutoStart();
    autoBtn->setText(on ? QString::fromUtf8("开机自启动: 开") : QString::fromUtf8("开机自启动: 关"));
    autoBtn->setStyleSheet(on ? "QPushButton{background:#2a6e3f;color:#fff;border:1px solid #3a8f55;border-radius:6px;padding:8px;}"
                              : "QPushButton{background:#444;color:#fff;border:1px solid #666;border-radius:6px;padding:8px;}");
  };
  updateAutoBtn();
  QObject::connect(autoBtn, &QPushButton::clicked, [autoBtn, updateAutoBtn]() {
    setAutoStart(!isAutoStart());
    updateAutoBtn();
  });
  lay->addWidget(autoBtn);

  lay->addSpacing(6);

  // 系统诊断按钮（用于定位旧驱动下的侧边栏/XShape 问题）
  QPushButton* infoBtn = new QPushButton(QString::fromUtf8("系统诊断信息"));
  infoBtn->setStyleSheet("QPushButton{background:#444;color:#ccc;padding:8px;border-radius:6px;}"
                         "QPushButton:hover{background:#555;}");
  QObject::connect(infoBtn, &QPushButton::clicked, []() { showSystemInfo(); });
  lay->addWidget(infoBtn);

  lay->addSpacing(6);

  // WPS 接口调试模式（实验）：翻页走本地 TCP 16666 + 加载项回传真实页号
  QPushButton* wpsBtn = new QPushButton();
  QLabel* wpsHint = new QLabel();
  wpsHint->setWordWrap(true);
  wpsHint->setStyleSheet("color:#667;font-size:11px;");
  auto updateWpsBtn = [wpsBtn, wpsHint]() {
    bool on = g.wpsDebug;
    wpsBtn->setText(on ? QString::fromUtf8("WPS 接口调试: 开") : QString::fromUtf8("WPS 接口调试: 关"));
    wpsBtn->setStyleSheet(on ? "QPushButton{background:#6a3f2a;color:#fff;border:1px solid #aa7a4a;border-radius:6px;padding:8px;}"
                             : "QPushButton{background:#444;color:#fff;border:1px solid #666;border-radius:6px;padding:8px;}");
    wpsHint->setText(on ? QString::fromUtf8("开启中：按钮仍发虚拟键推进放映，但缓存改为按 WPS 加载项回传的"
                        "真实页号驱动——动画步不动缓存，只有真换页才存/载批注。日志: %1").arg(wpsLogFile())
                        : QString::fromUtf8("关闭=点击一次存一页（原逻辑，兼容无加载项环境）。默认开启，状态会保存。"));
  };
  updateWpsBtn();
  QObject::connect(wpsBtn, &QPushButton::clicked, [wpsBtn, updateWpsBtn]() {
    setWpsDebug(!g.wpsDebug);
    updateWpsBtn();
  });
  lay->addWidget(wpsBtn);
  lay->addWidget(wpsHint);

  // 手掌自动橡皮（试验，默认开）
  QPushButton* palmBtn = new QPushButton();
  auto updatePalmBtn = [palmBtn]() {
    bool on = g.palmEraseOn;
    palmBtn->setText(on ? QString::fromUtf8("手掌自动橡皮(试验): 开") : QString::fromUtf8("手掌自动橡皮(试验): 关"));
    palmBtn->setStyleSheet(on ? "QPushButton{background:#4a3f6a;color:#fff;border:1px solid #8a6adf;border-radius:6px;padding:8px;}"
                              : "QPushButton{background:#444;color:#fff;border:1px solid #666;border-radius:6px;padding:8px;}");
  };
  updatePalmBtn();
  QObject::connect(palmBtn, &QPushButton::clicked, [palmBtn, updatePalmBtn]() {
    g.palmEraseOn = !g.palmEraseOn;
    g.palmErasePreview.clear();
    wpsSaveSettings();
    updatePalmBtn();
  });
  lay->addWidget(palmBtn);
  QLabel* palmHint = new QLabel(QString::fromUtf8("画笔/直线模式下手掌(多点)临时当大号橡皮，单点恢复笔；橡皮/光标模式不受影响。"));
  palmHint->setWordWrap(true);
  palmHint->setStyleSheet("color:#667;font-size:11px;");
  lay->addWidget(palmHint);

  // 清空调试日志（防止不熟悉的人让日志越积越多）
  QPushButton* clearLogBtn = new QPushButton(QString::fromUtf8("清空调试日志"));
  clearLogBtn->setStyleSheet("QPushButton{background:#444;color:#ccc;padding:8px;border-radius:6px;font-size:13px;}"
                             "QPushButton:hover{background:#555;}");
  QObject::connect(clearLogBtn, &QPushButton::clicked, [clearLogBtn]() {
    QFile::remove(wpsLogFile());
    clearLogBtn->setText(QString::fromUtf8("已清空 ✓"));
    QTimer::singleShot(1200, [clearLogBtn]() { clearLogBtn->setText(QString::fromUtf8("清空调试日志")); });
  });
  lay->addWidget(clearLogBtn);

  // 版权信息
  QLabel* creditLbl = new QLabel(QString::fromUtf8("Sidera 2.3-Geo   © 2026 Carl_Jin\nGNU GPL v3"));
  creditLbl->setAlignment(Qt::AlignCenter);
  creditLbl->setStyleSheet("color:#556;font-size:11px;");
  lay->addWidget(creditLbl);

  lay->addSpacing(6);

  QPushButton* doneBtn = new QPushButton(QString::fromUtf8("完成"));
  doneBtn->setStyleSheet("QPushButton{background:#3377cc;color:#fff;font-weight:bold;padding:10px;border-radius:6px;}"
                         "QPushButton:hover{background:#4488dd;}");
  QObject::connect(doneBtn, &QPushButton::clicked, []() { closeSettings(); });
  lay->addWidget(doneBtn);

  // 窗口关闭（点 X / 完成）恢复画布并复位指针
  QObject::connect(win, &QWidget::destroyed, []() {
    g.settingsWin = nullptr;
    restoreCanvasAfterSettings();
  });

  // 屏幕居中
  win->adjustSize();
  win->move(QGuiApplication::primaryScreen()->geometry().center() - win->rect().center());
  win->show();
}

static void closeSettings() {
  if (g.settingsWin) g.settingsWin->close();
  g.settingsWin = nullptr;
  restoreCanvasAfterSettings();
}

// ============================================================
// 系统诊断信息（用于定位旧驱动下 XShape/侧边栏问题）
// ============================================================
static QString collectSystemInfo() {
  QString s;
  s += "=== 系统环境诊断 ===\n";
  QFile osf("/etc/os-release");
  if (osf.open(QIODevice::ReadOnly)) {
    while (!osf.atEnd()) {
      QString line = QString::fromLocal8Bit(osf.readLine()).trimmed();
      if (line.startsWith("PRETTY_NAME=")) s += "系统: " + line.mid(13) + "\n";
    }
    osf.close();
  }
  s += "架构: " + QString(QSysInfo::currentCpuArchitecture()) + "\n";
  s += "内核: " + QString(QSysInfo::kernelType()) + " " + QString(QSysInfo::kernelVersion()) + "\n";
  s += "Qt: " + QString(qVersion()) + "\n";
  s += "XDG_SESSION_TYPE: " + QString::fromLocal8Bit(qgetenv("XDG_SESSION_TYPE")) + "\n";
  s += "DISPLAY: " + QString::fromLocal8Bit(qgetenv("DISPLAY")) + "\n";
  s += "合成器: " + QString(hasCompositor() ? "有" : "无") + "\n";

  // X11 / XShape 扩展检查
  Display* dpy = g.xDisplay;
  bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (dpy) {
    int evBase, errBase, major, minor;
    if (XShapeQueryExtension(dpy, &evBase, &errBase) && XShapeQueryVersion(dpy, &major, &minor)) {
      s += "XShape: 扩展可用 v" + QString::number(major) + "." + QString::number(minor) + "\n";
    } else {
      s += "XShape: 扩展不可用!\n";
    }
    if (nc) XCloseDisplay(dpy);
  }

  QScreen* sc = QGuiApplication::primaryScreen();
  if (sc) s += "屏幕: " + QString::number(sc->geometry().width()) + "x" + QString::number(sc->geometry().height()) + "\n";

  if (g.mainWidget) {
    s += "模式: " + QString(g.currentMode == 0 ? "光标" : (g.currentMode == 1 ? "画笔" : (g.currentMode == 2 ? "橡皮" : "直线"))) + "\n";
    s += "画布: 可见=" + QString(g.mainWidget->isVisible() ? "是" : "否")
       + " pos=(" + QString::number(g.mainWidget->pos().x()) + "," + QString::number(g.mainWidget->pos().y()) + ")"
       + " size=(" + QString::number(g.mainWidget->width()) + "x" + QString::number(g.mainWidget->height()) + ")"
       + " override_redirect 窗口\n";
  }
  auto sbInfo = [&](const char* tag, QWidget* w) {
    if (w)
      s += QString(tag) + ": pos=(" + QString::number(w->pos().x()) + "," + QString::number(w->pos().y()) + ")"
         + " size=(" + QString::number(w->width()) + "x" + QString::number(w->height()) + ")"
         + " 可见=" + QString(w->isVisible() ? "是" : "否") + "\n";
    else
      s += QString(tag) + ": (空/未创建)\n";
  };
  sbInfo("左侧边栏", g.sidebarArea);
  sbInfo("右侧边栏", g.sidebarAreaRight);
  return s;
}

static void showSystemInfo() {
  QDialog* dlg = new QDialog(g.settingsWin);
  dlg->setWindowTitle(QString::fromUtf8("系统诊断信息"));
  dlg->resize(500, 360);
  QVBoxLayout* lay = new QVBoxLayout(dlg);
  QTextEdit* te = new QTextEdit(dlg);
  te->setReadOnly(true);
  te->setPlainText(collectSystemInfo());
  lay->addWidget(te);
  QPushButton* close = new QPushButton(QString::fromUtf8("关闭"), dlg);
  QObject::connect(close, &QPushButton::clicked, dlg, &QDialog::close);
  lay->addWidget(close);
  dlg->exec();
  delete dlg;
}

// ============================================================
// 启动闪屏：屏幕中央小窗、青→粉渐变、底部进度条、右下角 byline
// ============================================================
class SplashWindow : public QWidget {
public:
  explicit SplashWindow() {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(460, 210);
    bar = new QProgressBar(this);
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setFixedHeight(10);
    bar->setTextVisible(false);
    bar->setStyleSheet(
      "QProgressBar{background:rgba(255,255,255,45);border:none;border-radius:5px;}"
      "QProgressBar::chunk{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
      "stop:0 #00e5ff,stop:1 #ff80ab);border-radius:5px;}");
  }
  void setProgress(int v) { bar->setValue(v); }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRectF r = rect().adjusted(1.5, 1.5, -1.5, -1.5);
    QLinearGradient g(r.topLeft(), r.bottomRight());
    g.setColorAt(0.0, QColor(0, 200, 255, 245));    // cyan
    g.setColorAt(1.0, QColor(255, 120, 180, 245));  // pink
    p.setBrush(g);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1.5));
    p.drawRoundedRect(r, 20, 20);

    // 图标（顶部居中）
    static const QPixmap ico = sideraIconPixmap(84);
    if (!ico.isNull()) {
      int ix = (width() - ico.width()) / 2;
      p.drawPixmap(ix, 22, ico);
    }

    // 名字
    QFont nf = p.font();
    nf.setPointSize(38);
    nf.setBold(true);
    p.setFont(nf);
    p.setPen(Qt::white);
    p.drawText(QRect(0, 108, width(), 54),
               Qt::AlignCenter, QStringLiteral("Sidera"));

    // 右下角 byline
    QFont bf = p.font();
    bf.setPointSize(10);
    bf.setBold(false);
    p.setFont(bf);
    p.setPen(QColor(255, 255, 255, 210));
    p.drawText(QRect(0, int(height() * 0.74), width() - 22, 24),
               Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("developed by jinyicheng"));
    p.end();
  }

  void resizeEvent(QResizeEvent*) override {
    if (bar) bar->setGeometry(24, height() - 30, width() - 48, 10);
  }

private:
  QProgressBar* bar = nullptr;
};

// 阻塞显示 2 秒启动闪屏（进度条 0→100），随后主程序继续初始化
static void showSplashFor(QApplication& app) {
  SplashWindow splash;
  QRect scr = QGuiApplication::primaryScreen()->geometry();
  splash.move(scr.center() - splash.rect().center());
  splash.show();
  splash.setProgress(0);
  QElapsedTimer t;
  t.start();
  const int dur = 2000;
  while (t.elapsed() < dur) {
    int p = qMin(100, int(t.elapsed() * 100 / dur));
    splash.setProgress(p);
    app.processEvents();
    QThread::msleep(16);
  }
  splash.setProgress(100);
  app.processEvents();
  splash.close();
}

// ============================================================
// 13. main
// ============================================================
int main(int argc, char* argv[]) {
  setupSoftwareRendering();
  QApplication app(argc, argv);

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

  // WPS 全屏检测定时器（每 500ms 检查一次）
  QTimer* wpsTimer = new QTimer(&app);
  QObject::connect(wpsTimer, &QTimer::timeout, []() { checkWpsState(); });
  wpsTimer->start(500);
  checkWpsState();  // 立即执行一次

  QObject::connect(&app, &QApplication::aboutToQuit, []() {
    stopWpsApiServer();
    clearAllPages();
    delete g.canvas; g.canvas = nullptr;
    delete g.iconCursor; g.iconCursor = nullptr;
    delete g.iconPen; g.iconPen = nullptr;
    delete g.iconEraser; g.iconEraser = nullptr;
    delete g.iconLine; g.iconLine = nullptr;
  });

  // 调试模式启动：已由 wpsLoadSettings 载入 g.wpsDebug；环境变量仅本次运行覆盖（不落盘）
  {
    QString env = QString::fromLocal8Bit(qgetenv("WPS_API_DEBUG"));
    if (!env.isEmpty()) setWpsDebug(env == "1", false);
    else                setWpsDebug(g.wpsDebug, false);
  }

  return app.exec();
}
