# Sidera 教室部署说明（2.6-Geo-testing）

## 装（管理员）
```bash
sudo dpkg -i sidera_2.6-Geo-testing_arm64.deb    # arm64 教室机
# x86 机器用 _amd64.deb
```

## 无需手动配置项（本包已自动处理）
- 调试模式默认开启（重启后仍保持）；可在 设置→WPS 接口调试 关闭，状态会保存。
- 首次以调试模式运行时，会**自动把加载项写入当前用户** `~/.local/share/Kingsoft/wps/jsaddons/publish.xml`
  （已有文件只插入本加载项，不影响其它项）。
- app 内「开机自启动」开启后：登录即起服务（127.0.0.1:16666），WPS 加载项内容由此服务提供。

## 使用顺序（每节课）
1. 确保Sidera已运行（自启后即好）。
2. **后**打开 WPS 演示 → 功能区出现「批注联动」标签（可点「状态」自检）。
3. F5 放映：用批注侧边栏 ▼/▲ 推进；**批注缓存只在真实换页时切换**，页内动画步不动批注。

## 卸载
```bash
sudo dpkg -r sidera
# 如需清掉加载项登记：删除 ~/.local/share/Kingsoft/wps/jsaddons/publish.xml 中
# sidera-bridge 对应条目（或整个文件）。
```

## 日志
`~/wps-api-debug.log`：服务/连接/事件/降级记录，排查用。
