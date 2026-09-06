// ============================================================
// 屏幕批注联动桥（加载项内运行）
//   · 监听 WPS 放映事件 → 上报批注 app (127.0.0.1:16666 /push)
//   · 轮询 /poll 取 NEXT/PREV 指令（保留，默认 app 用虚拟键推进）
// 配合 app 的“WPS 接口调试模式”使用：批注缓存按真实页号驱动，
// 页内动画步不动批注。
// ============================================================
var BRIDGE_URL = 'http://127.0.0.1:16666';

function OnAddinLoad(ribbonUI) {
    if (ribbonUI && typeof window.Application.ribbonUI != 'object')
        window.Application.ribbonUI = ribbonUI
    startBridge()
    return true
}

function OnSaPing() {
    var st = bridgeState()
    alert('批注联动桥在线\npos=' + st.pos + ' click=' + st.click)
    return true
}

function bridgeView() {
    var a = window.Application ? window.Application.ActivePresentation : null
    var sw = a ? a.SlideShowWindow : null
    return sw ? sw.View : null
}
function bridgeState() {
    try {
        var v = bridgeView()
        if (!v) return { pos: -1, click: -1 }
        var pos = v.CurrentShowPosition
        var click = (typeof v.GetClickIndex == 'function') ? v.GetClickIndex() : -1
        return { pos: pos, click: click }
    } catch (e) { return { pos: -1, click: -1 } }
}
function bridgeFetch(path) {
    try {
        fetch(BRIDGE_URL + path)
            .then(function (r) { return r.text() })
            .then(function (t) {
                var c = (t || '').trim()
                if (c) bridgeCmd(c)
            })
            .catch(function () {})
    } catch (e) {}
}
function bridgeCmd(cmd) {
    try {
        var v = bridgeView()
        if (!v) return                       // 非放映状态不动作
        if (cmd == 'NEXT') v.Next()
        else if (cmd == 'PREV') v.Previous()
    } catch (e) {}
}
var BridgeSlideEvents = ['SlideShowBegin', 'SlideShowEnd', 'SlideShowNextSlide', 'SlideShowNextClick', 'SlideShowOnNext', 'SlideShowOnPrevious'];
function startBridge() {
    if (typeof window.Application == 'undefined' || !window.Application) {
        setTimeout(startBridge, 500)          // 等 Application 就绪
        return
    }
    try {
        if (!window.Application.ApiEvent) { setTimeout(startBridge, 500); return }
        for (var i = 0; i < BridgeSlideEvents.length; i++) {
            (function (ev) {
                window.Application.ApiEvent.AddApiEventListener(ev, function () {
                    var st = bridgeState()
                    bridgeFetch('/push?m=' + encodeURIComponent('EVENT ' + ev + ' pos=' + st.pos + ' click=' + st.click))
                })
            })(BridgeSlideEvents[i])
        }
        bridgeFetch('/hello?m=screen-annotate-bridge')
        setInterval(function () { bridgeFetch('/poll') }, 400)
    } catch (e) {}
}
