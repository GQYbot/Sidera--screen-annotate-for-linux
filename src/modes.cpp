// Sidera - 模式切换/画布/输入区域/X11
// Copyright (C) 2026 Carl_Jin   GNU GPL v3
#include "app.h"

// ============================================================
// 9. 模式切换
// ============================================================
/*
 * 光标模式：小窗口（54×186），桌面可正常操作
 * 绘画模式：全屏窗口，侧边栏嵌入内部
 */
void switchToCursorMode() {
  closeAllPopups();
  g.currentMode = 0;
  if (!g.mainWidget || !g.sidebarArea) return;

  // 清空绘图状态并释放鼠标抓取（中断笔画也不残留）
  g.isDrawing = false;
  resetPalmGesture();
  g.mainWidget->releaseMouse();

  // 只切模式，光标模式只侧边栏可点击，其余穿透桌面
  updateSidebarStyles();
  setInputShapeToSidebar();

  qDebug() << "[INFO] 光标模式";
}

void switchToDrawMode(int mode) {
  closeAllPopups();
  bool wasAlreadyDrawing = (g.currentMode != 0);
  g.currentMode = mode;
  if (!g.mainWidget || !g.sidebarArea) return;

  // 进入绘画模式时若侧边栏是收缩态，先展开，保证模式键可点
  if (g.collapsed) expandSidebars();

  // 清空绘图状态，避免残留 isDrawing 导致幽灵线
  g.isDrawing = false;
  resetPalmGesture();

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
  if (g.prevBtn) g.prevBtn->setVisible(!g.collapsed);
  if (g.nextBtn) g.nextBtn->setVisible(!g.collapsed);
  resetInputShape();

  qDebug() << "[INFO] 绘画模式:" << (mode == 1 ? "画笔" : "橡皮擦");
}

// ============================================================
// 10. 画布操作
// ============================================================
void initCanvas() {
  QSize sz = QGuiApplication::primaryScreen()->size();
  if (g.canvas && g.canvas->size() == sz) return;  // 尺寸没变，复用
  if (g.canvas) delete g.canvas;
  g.canvas = new QPixmap(sz);
  g.canvas->fill(Qt::transparent);
}

void clearCanvas() {
  if (g.canvas) g.canvas->fill(Qt::transparent);
  clearUndo();                       // 清除语义统一：清空后不可再撤回回旧内容
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
void setInputShapeToSidebar() {
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

void resetInputShape() {
  if (!g.mainWidget || !g.mainWidget->isVisible()) return;
  Display* dpy = g.xDisplay;
  bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (!dpy) return;
  XShapeCombineMask(dpy, g.mainWidget->winId(), ShapeInput, 0, 0, None, ShapeSet);
  XFlush(dpy);
  if (nc) XCloseDisplay(dpy);
}

void exitPresentation() {
  if (g.whiteboard) {
    // 白板模式：该键改为切换白板背景色，不退出、不对外发键
    g.whiteboardBgIndex = (g.whiteboardBgIndex + 1) % kWhiteboardColorCount;
    updateExitButtons();
    if (g.mainWidget) g.mainWidget->update();
    qDebug() << "[INFO] 白板背景色切换为" << whiteboardBgColor().name();
    return;
  }
  if (g.currentMode != 0) switchToCursorMode();
  Display* dpy = g.xDisplay;
  bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (dpy) { sendXTestKey(dpy, XK_Escape); if (nc) XCloseDisplay(dpy); }
  qDebug() << "[INFO] 退出放映（已回光标模式并发送 ESC）";
}

// 当前模式归位光标、笔迹不清空 —— 见 collapseSidebars
/*
 * XTEST 假键盘。Up/Down 方向键 GNOME 不全局抓取。
 */
void sendXTestKey(Display* dpy, KeySym ks) {
  KeyCode kc = XKeysymToKeycode(dpy, ks);
  if (kc == 0) return;
  XTestFakeKeyEvent(dpy, kc, True,  0);
  XTestFakeKeyEvent(dpy, kc, False, 0);
  XFlush(dpy);
}

// 把指针移到指定屏幕坐标并注入一次鼠标右键（button 3）
// globalPos 为 Qt 逻辑坐标；XTest 需物理像素，故按 devicePixelRatio 换算
void doVirtualRightClick(const QPoint& globalPos) {
  Display* dpy = g.xDisplay;
  bool nc = false; if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (!dpy) return;
  double dpr = g.mainWidget ? g.mainWidget->devicePixelRatioF() : 1.0;
  if (dpr <= 0) dpr = 1.0;
  int px = qRound(globalPos.x() * dpr);
  int py = qRound(globalPos.y() * dpr);
  XTestFakeMotionEvent(dpy, -1, px, py, 0);
  XTestFakeButtonEvent(dpy, 3, True,  0);
  XTestFakeButtonEvent(dpy, 3, False, 0);
  XFlush(dpy);
  if (nc) XCloseDisplay(dpy);
  qDebug() << "[INFO] 虚拟右键已发送于" << globalPos << "(物理" << px << "," << py << ")";
}

// ============================================================
// 合成器检测：无合成器时 X11 透明窗口会失效（显示不透明）
// ============================================================
bool hasCompositor() {
  Display* dpy = g.xDisplay;
  bool nc = false;
  if (!dpy) { dpy = XOpenDisplay(nullptr); nc = true; }
  if (!dpy) return false;
  Window cm = XGetSelectionOwner(dpy, XInternAtom(dpy, "_NET_WM_CM_S0", False));
  if (nc) XCloseDisplay(dpy);
  return cm != None;
}

