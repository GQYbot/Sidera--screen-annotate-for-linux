// Sidera - 图标/绘制/撤回
// Copyright (C) 2026 Carl_Jin   GNU GPL v3
#include "app.h"

// ============================================================
// 5. 图标绘制
// ============================================================
QPixmap makeCursorIcon(int s) {
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
QPixmap makePenIcon(int s) {
  QPixmap p(s,s); p.fill(Qt::transparent);
  QPainter pt(&p); pt.setRenderHint(QPainter::Antialiasing,true);
  float m=s*0.15f;
  pt.setPen(QPen(QColor(255,200,100),3.5,Qt::SolidLine,Qt::RoundCap));
  pt.drawLine(QPointF(s-m,m),QPointF(m+2,s-m-2));
  pt.setPen(QPen(QColor(40,40,40),4.5,Qt::SolidLine,Qt::RoundCap));
  pt.drawLine(QPointF(m+2,s-m-2),QPointF(m*0.3f,s-m*0.3f));
  pt.end(); return p;
}
QPixmap makeEraserIcon(int s) {
  QPixmap p(s,s); p.fill(Qt::transparent);
  QPainter pt(&p); pt.setRenderHint(QPainter::Antialiasing,true);
  float m=s*0.18f;
  pt.setPen(QPen(QColor(255,180,180),2.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
  pt.setBrush(QColor(255,150,150,200));
  pt.drawRoundedRect(QRectF(m*1.5f,m,s-m*3,s-m*2),m*0.8f,m*0.8f);
  pt.end(); return p;
}

// ============================================================
// 5b. Sidera 图标（PNG，SVG 同图提取）：设置面板 + 启动闪屏展示
// ============================================================
QString sideraIconPath() {
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

QPixmap sideraIconPixmap(int px) {
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

void strokeSegment(QPoint a, QPoint b, bool erase, int width) {
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
  if (a == b) {
    // 同点：画/擦一个实心圆点，确保轻点一定可见
    p.setPen(Qt::NoPen);
    p.setBrush(erase ? QBrush(Qt::black) : QBrush(g.penColor()));
    p.drawEllipse(QPointF(a), width / 2.0, width / 2.0);
  } else {
    p.drawLine(a, b);
  }
  p.end();
}

// 画一段笔迹到 g.canvas（画笔/橡皮擦共用，鼠标和触摸都调用）
void strokeToCanvas(QPoint a, QPoint b) {
  if (g.currentMode == 2) strokeSegment(a, b, true, g.eraserWidth());
  else                    strokeSegment(a, b, false, g.penWidth());
}

// 按落笔时长计算笔迹宽度：起笔最细（当前宽度的 0.85 倍），随时间增粗到当前所选宽度
int timedPenWidth() {
  int base = g.penWidth();
  if (g.strokeStartMs <= 0) return base;
  int minw = qMax(1, int(qRound(base * kStrokeMinScale)));
  qint64 dt = QDateTime::currentMSecsSinceEpoch() - g.strokeStartMs;
  double t = qBound(0.0, double(dt) / double(kStrokeRampMs), 1.0);
  return minw + int(qRound((base - minw) * t));
}

// 宽度沿笔画渐变的画笔笔迹（像墨水由细到粗扩散）：把一段拆成若干子段，宽度线性插值
void strokeTapered(QPoint a, QPoint b, int wStart, int wEnd) {
  if (!g.canvas) return;
  QPainter p(g.canvas);
  p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setBrush(Qt::NoBrush);
  if (a == b) {
    QPen pen(g.penColor(), qMax(1, wStart), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.drawPoint(a);
    p.end();
    return;
  }
  int len = (QPoint(b.x() - a.x(), b.y() - a.y())).manhattanLength();
  int steps = qBound(2, len / 4 + 1, 16);
  for (int i = 0; i < steps; i++) {
    double t0 = double(i) / steps, t1 = double(i + 1) / steps;
    QPointF p0(a.x() + (b.x() - a.x()) * t0, a.y() + (b.y() - a.y()) * t0);
    QPointF p1(a.x() + (b.x() - a.x()) * t1, a.y() + (b.y() - a.y()) * t1);
    int w = qRound(wStart + (wEnd - wStart) * ((t0 + t1) / 2.0));
    QPen pen(g.penColor(), qMax(1, w), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.drawLine(p0, p1);
  }
  p.end();
}

// ===== 撤回：每笔落笔前存快照，撤回时恢复上一张 =====
void clearUndo() {
  for (QPixmap* p : g.undoStack) delete p;
  g.undoStack.clear();
}
void pushUndo() {
  if (!g.canvas) return;
  if (g.undoStack.size() >= kMaxUndo) delete g.undoStack.takeFirst();
  g.undoStack.append(new QPixmap(*g.canvas));
}
void undoLast() {
  if (g.undoStack.isEmpty()) return;
  QPixmap* prev = g.undoStack.takeLast();
  if (g.canvas) *g.canvas = *prev;
  delete prev;
  if (g.mainWidget) g.mainWidget->update();
  qDebug() << "[INFO] 撤回一步";
}

// 复位手势引擎状态（防止换模式/触摸被侧栏截走/手势取消后残留 → 幽灵线/卡橡皮）
void resetPalmGesture() {
  g.tPrevPos.clear();
  g.tPrevRole.clear();
  g.tPrevPreview.clear();
  g.tPalmLatched = false;
  g.tMoved = false;
  g.tBeginRole = 0;
  g.palmErasePreview.clear();
  g.palmEraseW = kPalmEraseWidth;
}

