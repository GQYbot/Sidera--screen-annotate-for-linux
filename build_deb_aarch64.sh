#!/bin/bash
# arm64 deb 打包脚本
# 用法: ./build_deb_aarch64.sh
# 产物: screen-annotate_2.1-Geo_arm64.deb

set -e

BINARY="annotate_aarch64"
PKG_NAME="screen-annotate"
VERSION="2.2-Geo-unstable"
ARCH="arm64"

if [ ! -f "$BINARY" ]; then
  echo "错误: 找不到 $BINARY，请先编译"
  exit 1
fi

PKG_DIR="${PKG_NAME}_${VERSION}_${ARCH}"

# 清理并创建目录结构
rm -rf "$PKG_DIR"
mkdir -p "${PKG_DIR}/DEBIAN"
mkdir -p "${PKG_DIR}/usr/bin"
mkdir -p "${PKG_DIR}/usr/share/applications"
mkdir -p "${PKG_DIR}/usr/share/icons/hicolor/64x64/apps"

# 拷贝二进制
cp "$BINARY" "${PKG_DIR}/usr/bin/screen-annotate"
chmod 755 "${PKG_DIR}/usr/bin/screen-annotate"

# ---------- 拷贝 WPS 加载项部署包（本 deb 已含桥接加载项 + 安装脚本）----------
mkdir -p "${PKG_DIR}/usr/share/screen-annotate/wps-addin"
cp -r wps-addin/* "${PKG_DIR}/usr/share/screen-annotate/wps-addin/"
chmod 755 "${PKG_DIR}/usr/share/screen-annotate/wps-addin/install.sh"
install -m 755 wps-addin/install.sh "${PKG_DIR}/usr/bin/screen-annotate-wps-addin-install"

# ---------- DEBIAN/control ----------
cat > "${PKG_DIR}/DEBIAN/control" << EOF
Package: ${PKG_NAME}
Version: ${VERSION}
Section: graphics
Priority: optional
Architecture: ${ARCH}
Maintainer: User <user@localhost>
Depends: libqt5core5a (>= 5.12), libqt5gui5 (>= 5.12), libqt5widgets5 (>= 5.12), libx11-6, libxcb1, libxtst6, libxext6
Description: 屏幕批注软件
 全屏透明画布批注工具，支持画笔/橡皮擦/直线，触控屏友好。
 启动后显示左右两个胶囊形侧边栏，侧边栏 ⛶ 按钮可退出全屏。
 附带 WPS 演示联动加载项：设置里开启“WPS接口调试”后，
 批注缓存随真实换页驱动（页内动画不动批注）。
 安装后执行一次 screen-annotate-wps-addin-install 注册加载项。
EOF

# ---------- desktop 入口文件 ----------
cat > "${PKG_DIR}/usr/share/applications/screen-annotate.desktop" << EOF
[Desktop Entry]
Type=Application
Name=屏幕批注
Comment=屏幕批注软件
Exec=screen-annotate
Icon=screen-annotate
Categories=Graphics;Utility;
Terminal=false
EOF

# ---------- 最小合法 PNG 图标 (1x1 透明) ----------
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\rIDATx\x9cc\xf8\xcf\xc0\xf0\x1f\x00\x05\x05\x02\x00\x8f\x1e\x1f\x03\x99\x00\x00\x00\x00IEND\xaeB\x60\x82' > "${PKG_DIR}/usr/share/icons/hicolor/64x64/apps/screen-annotate.png"

# ---------- 打包（--root-owner-group 消除 owner 警告）----------
dpkg-deb --build --root-owner-group "${PKG_DIR}"

echo ""
echo "========== 打包完成 =========="
ls -lh "${PKG_DIR}.deb"
echo ""
echo "安装: sudo dpkg -i ${PKG_DIR}.deb"
echo "卸载: sudo dpkg -r ${PKG_NAME}"
