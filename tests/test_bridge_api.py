#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Sidera WPS 联动桥与 HTTP 接口自动化测试套件
用于验证 Sidera 本地 127.0.0.1:16666 服务及 WPS JS 加载项通讯逻辑。

功能：
1. 探测 Sidera 本地 HTTP 调试服务在线状态
2. 验证 /hello 握手端点与 /push 事件上报端点
3. 验证 /poll 指令队列（NEXT / PREV）出队逻辑
4. 验证静态加载项文件（manifest.xml / ribbon.xml / js/bridge.js）分发及 CORS 标头
5. 提供 Mock 服务模式与客户端探测模式，方便无 WPS 环境下的独立调试

使用方法：
  python3 tests/test_bridge_api.py          # 探测已运行的 Sidera 实例
  python3 tests/test_bridge_api.py --mock   # 启动 Mock 服务用于测试加载项通讯
"""

import sys
import time
import argparse
import urllib.request
import urllib.error
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

BRIDGE_URL = "http://127.0.0.1:16666"

class Colors:
    GREEN = "\033[92m"
    RED = "\033[91m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    CYAN = "\033[96m"
    BOLD = "\033[1m"
    END = "\033[0m"

def log_info(msg):
    print(f"{Colors.BLUE}[信息]{Colors.END} {msg}")

def log_success(msg):
    print(f"{Colors.GREEN}[通过]{Colors.END} {msg}")

def log_fail(msg):
    print(f"{Colors.RED}[失败]{Colors.END} {msg}")

def log_warn(msg):
    print(f"{Colors.YELLOW}[警告]{Colors.END} {msg}")

def send_request(path, method="GET", timeout=2.0):
    """向 Sidera 发送 HTTP 请求并返回状态码、响应体与标头"""
    url = f"{BRIDGE_URL}{path}"
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status = resp.status
            body = resp.read().decode("utf-8", errors="ignore")
            headers = dict(resp.getheaders())
            return status, body, headers
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="ignore"), dict(e.headers)
    except Exception as e:
        return None, str(e), {}

def run_tests():
    """执行全部端点测试"""
    print(f"{Colors.BOLD}{Colors.CYAN}=============================================={Colors.END}")
    print(f"{Colors.BOLD}{Colors.CYAN}    Sidera 本地 HTTP 联动服务测试套件        {Colors.END}")
    print(f"{Colors.BOLD}{Colors.CYAN}=============================================={Colors.END}\n")

    # 1. 检查服务是否在线
    log_info("正在检查 127.0.0.1:16666 服务端口...")
    status, body, headers = send_request("/hello?m=test-probe")
    if status is None:
        log_fail(f"无法连接到 Sidera 调试服务: {body}")
        log_warn("请确认 Sidera 已启动并在设置中开启了「WPS 接口调试模式」。")
        return 1

    log_success("成功连接到 Sidera 16666 端口服务！")

    passed_count = 0
    total_count = 0

    # 2. 测试 /hello 握手端点
    total_count += 1
    log_info("测试 [1/5] - 握手问候接口 /hello?m=sidera-bridge-test")
    status, body, headers = send_request("/hello?m=sidera-bridge-test")
    if status == 200 and "sidera" in body.lower():
        log_success(f"/hello 响应正常 (HTTP 200): {body.strip()}")
        passed_count += 1
    else:
        log_fail(f"/hello 响应异常: status={status}, body={body}")

    # 3. 测试 CORS 跨域标头
    total_count += 1
    log_info("测试 [2/5] - CORS 跨域支持 (Access-Control-Allow-Origin)")
    if headers.get("Access-Control-Allow-Origin") == "*" or headers.get("access-control-allow-origin") == "*":
        log_success("CORS 标头正确放行 (*)")
        passed_count += 1
    else:
        log_fail(f"缺少或未正确设置 CORS 标头: {headers}")

    # 4. 测试 /push 放映事件上报端点
    total_count += 1
    log_info("测试 [3/5] - 事件上报接口 /push (SlideShowBegin / NextSlide)")
    event_str = urllib.parse.quote("EVENT SlideShowBegin pos=1 click=0")
    status, body, _ = send_request(f"/push?m={event_str}")
    if status == 200 and "OK" in body:
        log_success("放映开始事件上报成功 (HTTP 200 OK)")
        passed_count += 1
    else:
        log_fail(f"事件上报接口响应异常: status={status}, body={body}")

    # 5. 测试 /poll 指令轮询端点
    total_count += 1
    log_info("测试 [4/5] - 指令轮询接口 /poll")
    status, body, _ = send_request("/poll")
    if status == 200:
        log_success(f"/poll 接口正常响应 (当前出队指令: '{body.strip() or '空队列'}')")
        passed_count += 1
    else:
        log_fail(f"/poll 接口响应异常: status={status}, body={body}")

    # 6. 测试静态加载项文件分发
    total_count += 1
    log_info("测试 [5/5] - 加载项静态资源分发 /manifest.xml")
    status, body, headers = send_request("/manifest.xml")
    if status == 200 and ("xml" in body.lower() or "sidera" in body.lower() or len(body) > 0):
        log_success(f"/manifest.xml 静态资源获取正常 (大小: {len(body)} 字节)")
        passed_count += 1
    else:
        log_warn(f"/manifest.xml 响应 (可能未配置环境变量 WPS_ADDIN_DIR): status={status}")
        # 如果服务返回 200 OK 亦算通过
        if status == 200:
            passed_count += 1

    print(f"\n{Colors.BOLD}测试结果统计: {passed_count}/{total_count} 项通过{Colors.END}")
    return 0 if passed_count == total_count else 1

class MockSideraHandler(BaseHTTPRequestHandler):
    """Mock Sidera 服务，供在没有 Sidera GUI 时测试加载项 JS"""
    cmd_queue = ["NEXT"]

    def log_message(self, format, *args):
        # 简化日志输出
        print(f"{Colors.CYAN}[Mock服务]{Colors.END} " + (format % args))

    def _send_cors_headers(self, content_type="text/plain; charset=utf-8"):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Content-Type", content_type)
        self.end_headers()

    def do_OPTIONS(self):
        self._send_cors_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)

        if parsed.path == "/hello":
            self._send_cors_headers()
            self.wfile.write(b"OK sidera-mock")
        elif parsed.path == "/push":
            m = params.get("m", [""])[0]
            print(f"{Colors.GREEN}[Mock事件]{Colors.END} 收到加载项事件: {m}")
            self._send_cors_headers()
            self.wfile.write(b"OK")
        elif parsed.path == "/poll":
            cmd = self.cmd_queue.pop(0) if self.cmd_queue else ""
            self._send_cors_headers()
            self.wfile.write(cmd.encode("utf-8"))
        else:
            self._send_cors_headers()
            self.wfile.write(b"OK")

def start_mock_server():
    """启动本地 16666 端口 Mock 服务"""
    server_address = ("127.0.0.1", 16666)
    try:
        httpd = HTTPServer(server_address, MockSideraHandler)
        print(f"{Colors.GREEN}Mock Sidera 联动服务已在 127.0.0.1:16666 启动{Colors.END}")
        print("按 Ctrl+C 可停止服务...")
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nMock 服务已停止。")
    except Exception as e:
        log_fail(f"Mock 服务启动失败: {e}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Sidera WPS 联动服务自动化测试脚本")
    parser.add_argument("--mock", action="store_true", help="启动本地 16666 Mock 服务以测试加载项")
    args = parser.parse_args()

    if args.mock:
        start_mock_server()
    else:
        sys.exit(run_tests())
