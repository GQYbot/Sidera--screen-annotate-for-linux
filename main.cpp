/*
 * 屏幕批注软件 v3 —— 单窗口架构 (Wayland + X11 统一)
 * Qt 5.12 / CPU 软件渲染
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
#include <QWindow>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XTest.h>

#include <cstdlib>
#include <functional>

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
};

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
static void forceStayOnTop();
static void setInputShapeToSidebar();
static void resetInputShape();
static void sendXTestKey(Display* dpy, KeySym ks);
static void goToPrevPage();
static void goToNextPage();
static void clearAllPages();
static QWidget* createPopup(int w, int h);

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
      p.setBrush(QColor(42,42,50,240));
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
static int sbWidth()  { return int(64 * g.sbScale); }
static int sbBtn()    { return int(38 * g.sbScale); }
static int sbIcon()   { return int(28 * g.sbScale); }
static int sbDot()    { return int(22 * g.sbScale); }
static int sbRadius() { return sbWidth() / 2; }
// 高度 = 固定 margins/spacing + 7 个按钮（间距 6 个 + 上下边距 18/14）
static int sbHeight() { return 18 + 14 + 7 * sbBtn() + 6 * 8; }

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
      QString("#sidebarArea {"
      "  background-color: rgba(42,42,50,240);"
      "  border: 2px solid #666666;"
      "  border-radius: %1px;"
      "}").arg(sbRadius())
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

    updateSidebarStyles();
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
    else g.currentMode = 3;
    updateSidebarStyles();
  }

  // ===== 绘制 =====
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);

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
    p.end();
  }

  void mousePressEvent(QMouseEvent* ev) override {
    if (g.currentMode == 3) {
      if (ev->button() == Qt::LeftButton) {
        g.lineStart = ev->pos();
        g.linePreview = true;
        g.lastPt = ev->pos();
        grabMouse();  // 锁定鼠标，VM 下光标出界也能持续收事件
      }
      return;
    }
    if (g.currentMode == 0 || !g.canvas) return;
    if (ev->button() == Qt::LeftButton) {
      g.isDrawing = true;
      g.lastPt   = ev->pos();
      grabMouse();
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
    QPoint prev = g.lastPt;  // 保留旧点用于计算包围盒
    {
      QPainter p(g.canvas);
      if (g.currentMode == 2) {
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        QPen ep(Qt::transparent, g.eraserWidth(), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(ep);
      } else {
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        QPen pen(g.penColor(), g.penWidth(), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setRenderHint(QPainter::Antialiasing, true);
      }
      p.drawLine(prev, cur);
      p.end();
    }
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
      releaseMouse();
      return;
    }
    Q_UNUSED(ev);
    if (g.isDrawing) { g.isDrawing = false; update(); }
    releaseMouse();
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

  qDebug() << "[INFO] 绘画模式:" << (mode == 1 ? "画笔" : "橡皮擦");
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
// WPS 联动：翻页 + 全屏检测 + 强制置顶
// ============================================================
/*
 * _NET_WM_STATE_ABOVE：比 WindowStaysOnTopHint 更强的置顶层
 * Plasma 点了别的窗口也不会把我们的窗口压下去
 */
static void forceStayOnTop() {
  if (!g.mainWidget || !g.mainWidget->windowHandle() || !g.mainWidget->isVisible()) return;
  Display* dpy = g.xDisplay;
  bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (!dpy) return;
  Atom netWmState = XInternAtom(dpy, "_NET_WM_STATE", False);
  Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
  XChangeProperty(dpy, g.mainWidget->winId(), netWmState, XA_ATOM, 32,
                  PropModeReplace, (unsigned char*)&above, 1);
  XFlush(dpy);
  if (nc) XCloseDisplay(dpy);
}

/*
 * XShape 输入区域：光标模式只让侧边栏 + 可见弹窗可点击，其余区域穿透桌面
 */
static void setInputShapeToSidebar() {
  if (!g.mainWidget || !g.mainWidget->isVisible()) return;
  Display* dpy = g.xDisplay;
  bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (!dpy) return;
  XRectangle rects[8];
  int n = 0;
  auto addRect = [&](QWidget* w) {
    if (!w || !w->isVisible()) return;
    QPoint p = w->pos();
    // 四周各留 6px 余量，兼容 VM/不同驱动的 XShape 边界偏差
    rects[n].x = (short)qMax(0, p.x() - 6);
    rects[n].y = (short)qMax(0, p.y() - 6);
    rects[n].width  = (unsigned short)(w->width() + 12);
    rects[n].height = (unsigned short)(w->height() + 12);
    n++;
  };
  addRect(g.sidebarArea);
  addRect(g.sidebarAreaRight);
  addRect(g.penPopup);
  addRect(g.eraserPopup);
  XShapeCombineRectangles(dpy, g.mainWidget->winId(), ShapeInput, 0, 0, rects, n, ShapeSet, YXBanded);
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
  // 同时清空当前画布上的笔迹
  if (g.canvas) g.canvas->fill(Qt::transparent);
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

static void goToPrevPage() {
  saveCurrentPage();
  if (g.currentSlide > 1) g.currentSlide--;
  // 全屏模式：发送 Up 键到放映窗口
  if (g.wpsFullscreen) {
    Display* dpy = g.xDisplay;
    bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
    if (dpy) { sendXTestKey(dpy, XK_Up); if (nc) XCloseDisplay(dpy); }
  }
  loadPage(g.currentSlide);
}

static void goToNextPage() {
  saveCurrentPage();
  g.currentSlide++;
  // 全屏模式：发送 Down 键到放映窗口
  if (g.wpsFullscreen) {
    Display* dpy = g.xDisplay;
    bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
    if (dpy) { sendXTestKey(dpy, XK_Down); if (nc) XCloseDisplay(dpy); }
  }
  loadPage(g.currentSlide);
}

/*
 * 全屏放映检测：扫描所有顶层窗口，找任何铺满屏幕的窗口
 * （不限于 WPS，OnlyOffice/LibreOffice 全屏也生效）
 *
 * 排除策略（防止把自己的全屏窗口误判，导致状态永不变化）：
 *   1. XID 匹配
 *   2. WM_CLASS 含 "annotate" / "screen-annotate"
 *   3. override_redirect 窗口（我们的窗口是 override-redirect，WPS 全屏不是）
 */
static bool isPresentationFullscreen(Display* dpy) {
  Window selfWin = 0;
  if (g.mainWidget) {
    QWindow* wh = g.mainWidget->windowHandle();
    if (wh) selfWin = (Window)wh->winId();
    if (!selfWin) selfWin = (Window)g.mainWidget->winId();
  }

  Window root = DefaultRootWindow(dpy);
  Window rootRet, parentRet, *children;
  unsigned int nChildren;
  if (!XQueryTree(dpy, root, &rootRet, &parentRet, &children, &nChildren) || !children) return false;

  QRect scr = QGuiApplication::primaryScreen()->geometry();
  bool found = false;

  for (unsigned int i = 0; i < nChildren; i++) {
    Window w = children[i];
    // 1. XID 排除自己
    if (selfWin && w == selfWin) continue;

    // 2. WM_CLASS 排除自己（XID 可能因 override-redirect 不匹配）
    XClassHint cls;
    if (XGetClassHint(dpy, w, &cls)) {
      QString name = QString::fromLocal8Bit(cls.res_name).toLower();
      QString klass = QString::fromLocal8Bit(cls.res_class).toLower();
      XFree(cls.res_name); XFree(cls.res_class);
      if (name.contains("annotate") || klass.contains("annotate") ||
          name.contains("screen-annotate") || klass.contains("screen-annotate"))
        continue;
    }

    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, w, &attrs)) continue;
    // 3. 跳过未映射的窗口
    if (attrs.map_state != IsViewable) continue;
    // 4. 跳过 override_redirect 窗口（我们的窗口；桌面组件也可能是）
    if (attrs.override_redirect) continue;
    // 5. 跳过太小 / 太小的窗口（面板、图标等）
    if (attrs.width < scr.width() - 8 || attrs.height < scr.height() - 8) continue;

    found = true; break;
  }

  XFree(children);
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

  // 调试日志：每次检测都打印状态（方便确认检测是否工作）
  qDebug() << "[WPS-DBG] was:" << was << "now:" << g.wpsFullscreen;

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
  QApplication app(argc, argv);

  g.platform = detectPlatform();
  qDebug() << "[INFO] 平台:" << g.platform;

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
    clearAllPages();
    delete g.canvas; g.canvas = nullptr;
    delete g.iconCursor; g.iconCursor = nullptr;
    delete g.iconPen; g.iconPen = nullptr;
    delete g.iconEraser; g.iconEraser = nullptr;
    delete g.iconLine; g.iconLine = nullptr;
  });

  return app.exec();
}
