#!/bin/bash
# 把 WPS 加载项登记文件 publish.xml 写入当前用户 WPS 目录
# 前提：批注 app 已带“WPS 接口调试模式”运行（内容由 app 的 16666 服务提供）
set -e
DST="${HOME}/.local/share/Kingsoft/wps/jsaddons"
SRC="$(dirname "$(readlink -f "$0")")/publish.xml"
if [ ! -f "$SRC" ]; then
  SRC="/usr/share/screen-annotate/wps-addin/publish.xml"
fi
mkdir -p "$DST"
cp -f "$SRC" "$DST/publish.xml"
echo "已写入: $DST/publish.xml"
echo "提示：请先启动“屏幕批注”并开启 设置→WPS接口调试，再重新打开 WPS 演示。"
echo "      放映时加载项会把真实页号回传，批注缓存随真实换页切换。"
