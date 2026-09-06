# 屏幕批注 - WPS 加载项部署包

作用：让 WPS 演示(放映)期间，把“真实页号变化”回传给批注 app，
使得批注缓存只在**真换页**时切换；页内动画步不动批注。
app 侧需开启 `设置 → WPS 接口调试模式`（内容由此服务的 127.0.0.1:16666 提供）。

## 部署（每台机器一次，按当前用户）
```bash
./install.sh            # 或安装 deb 后用: screen-annotate-wps-addin-install
```

## 使用顺序
1. 启动屏幕批注 app，并在设置里开启“WPS 接口调试”（或 WPS_API_DEBUG=1 启动）。
2. 再打开 WPS 演示 → 工具栏出现“批注联动”标签即加载成功（可点“状态”确认）。
3. F5 放映：点批注侧边栏 ▼/▲ 推进，观察批注是否只在真实换页时切换。

## 文件说明
- manifest.xml / ribbon.xml / main.js / js/bridge.js ：加载项本体
- publish.xml ：登记文件（install.sh 拷入 ~/.local/share/Kingsoft/wps/jsaddons/）
- 内容由 app 的 HTTP 服务(16666)实时提供，无需额外服务器

## 卸载
删除 ~/.local/share/Kingsoft/wps/jsaddons/publish.xml 中对应条目即可。
