// Sidera - 启动闪屏
// Copyright (C) 2026 Carl_Jin   GNU GPL v3
#include "app.h"

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
void showSplashFor(QApplication& app) {
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

