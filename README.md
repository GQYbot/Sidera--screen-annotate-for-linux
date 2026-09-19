# Sidera (星罗) — Linux 屏幕批注与教学演示工具

<div align="center">

![Sidera Banner](sidera.png)

**专为 Linux 教学与演示设计的免费、轻量级屏幕批注工具，深度集成 WPS 演示联动**

[![Build & Verify](https://github.com/offduckling4825/Sidera--screen-annotate-for-linux/actions/workflows/ci.yml/badge.svg)](https://github.com/offduckling4825/Sidera--screen-annotate-for-linux/actions/workflows/ci.yml)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)](https://en.cppreference.com/)
[![GUI Framework](https://img.shields.io/badge/GUI-Qt5-green.svg)](https://www.qt.io/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20(X11)-orange.svg)](https://www.x.org/)

</div>

---

## 🌟 核心特性

- 🖊️ **轻量流畅的屏幕批注**：基于 C++17 与 Qt5 构建，低 CPU/内存占用，支持自由画笔、荧光笔、多种几何图形与色盘。
- 🖱️ **智能点击穿透（XShape 遮罩）**：在光标模式下只响应侧边栏与弹窗交互，全屏透明区域完全穿透，不影响底层桌面与课件操作。
- 📊 **WPS 演示深度联动**：
  - 内置本地 HTTP 桥接服务（`127.0.0.1:16666`）与专属 JS 加载项。
  - 支持侧边栏直接控制幻灯片翻页。
  - **批注分页缓存**：仅在真实换页时自动切换批注缓存，页内动画触发时不丢失笔迹。
- 🖥️ **跨架构支持**：原生支持 `x86_64 (amd64)` 与 `aarch64 (arm64)` 架构，适配国产化 Linux 教室机。

---

## 📦 安装与快速开始

### 方式一：安装预编译 DEB 包（推荐）

教室机或 Debian / Ubuntu 系发行版可直接安装对应的 release 包：

```bash
# amd64 (x86_64)
sudo dpkg -i sidera_2.6-Geo-stable_amd64.deb

# aarch64 (arm64)
sudo dpkg -i sidera_2.6-Geo-stable_arm64.deb
```

### 方式二：从源码编译

#### 1. 安装编译依赖

**Ubuntu / Debian / Deepin / UOS：**
```bash
sudo apt update
sudo apt install -y build-essential pkg-config \
    qtbase5-dev libqt5widgets5 libqt5network5 \
    libx11-dev libxcb1-dev libxext-dev libxtst-dev
```

**Arch Linux：**
```bash
sudo pacman -S --needed base-devel qt5-base libx11 libxcb libxext libxtst
```

#### 2. 编译

```bash
# 编译 amd64 二进制
make -j$(nproc)

# 运行
./annotate_amd64
```

---

## 🎓 教室与教学使用流程

1. **启动 Sidera**：登录系统后自动或手动启动 Sidera（可在设置中勾选「开机自启动」）。
2. **打开 WPS 演示**：打开课件，WPS 功能区会自动加载「批注联动」标签。
3. **F5 放映授课**：放映过程中使用 Sidera 侧边栏的 ▲/▼ 按钮或画笔工具进行课件讲解与板书。

> 更多教室部署细节请参考 [README.classroom.md](README.classroom.md)。

---

## 🛠️ 项目架构

```text
src/
├── main.cpp        # 程序入口、事件循环、全局热键管理
├── app.h           # 全局状态单例与结构体定义
├── widget.h/cpp    # 主画布与全屏覆盖层窗口
├── sidebar.cpp     # 侧边悬浮工具栏与按钮交互
├── drawing.cpp     # 绘图引擎、笔触渲染与撤销重做逻辑
├── modes.cpp       # 模式切换（画笔/光标）与 XShape 输入穿透区域计算
├── wps.cpp         # WPS 加载项本地 HTTP 服务与联动逻辑
├── popups.cpp      # 色盘与参数调整浮窗
├── splash.cpp      # 启动欢迎与加载引导界面
└── settings.cpp    # 设置窗口与系统环境诊断工具
```

---

## 🧭 路线规划与 Wayland 适配说明 (Roadmap)

当前版本原生针对 X11 显示服务进行了底层优化。针对社区关注的 **Wayland 适配**，建议演进路径如下：

- [ ] **WPS 联动解耦**：将当前的按键模拟（XTest）与窗口轮询（XQueryTree）全面升级为基于 WPS JS Add-in 的双向 HTTP/WebSocket 事件通知。
- [ ] **输入区域与置顶抽象**：在 Wayland 环境下适配 `wlr-layer-shell`（Overlay 层）与 `QWidget::setMask`。
- [ ] **全局快捷键迁移**：接入现代 Linux `org.freedesktop.portal.GlobalShortcuts` 协议。

---

## 🤝 贡献指南

欢迎提交 Issue 和 Pull Request！
- 提交 PR 前请确保本地编译通过：`make clean && make`
- 建议保持轻量无冗余依赖的设计理念。

---

## 📄 许可证

本项目遵循开源许可协议，详见 [LICENSE](LICENSE) 文件。
