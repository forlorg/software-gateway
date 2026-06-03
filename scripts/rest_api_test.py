#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
software_gateway REST API 黑盒测试（仅标准库）。

用法:
  python scripts/rest_api_test.py [--base URL] [--factory-reset]

默认 base: http://192.168.1.80
--factory-reset  会调用 POST /api/factory_reset（清空 NVS 并重启设备，慎用）。
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from typing import Any


def http_request(
    method: str,
    url: str,
    *,
    json_body: dict[str, Any] | None = None,
    timeout: float = 15.0,
) -> tuple[int, str, dict[str, str]]:
    data: bytes | None = None
    headers: dict[str, str] = {}
    if json_body is not None:
        data = json.dumps(json_body, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json; charset=utf-8"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            text = raw.decode("utf-8", errors="replace")
            hdrs = {k.lower(): v for k, v in resp.headers.items()}
            return resp.status, text, hdrs
    except urllib.error.HTTPError as e:
        raw = e.read() if e.fp else b""
        text = raw.decode("utf-8", errors="replace")
        hdrs = {k.lower(): v for k, v in (e.headers or {}).items()}
        return e.code, text, hdrs


def print_result(name: str, status: int, body: str, max_body: int = 600) -> None:
    preview = body if len(body) <= max_body else body[:max_body] + "\n... [truncated]"
    print(f"\n=== {name} ===")
    print(f"HTTP {status}")
    print(preview)


def try_parse_json(body: str) -> Any | None:
    try:
        return json.loads(body)
    except json.JSONDecodeError:
        return None


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8")
        except Exception:
            pass
    if hasattr(sys.stderr, "reconfigure"):
        try:
            sys.stderr.reconfigure(encoding="utf-8")
        except Exception:
            pass

    p = argparse.ArgumentParser(description="software_gateway REST API 测试")
    p.add_argument(
        "--base",
        default="http://192.168.1.80",
        help="网关根地址，默认 http://192.168.1.80",
    )
    p.add_argument(
        "--factory-reset",
        action="store_true",
        help="执行 POST /api/factory_reset（清空 NVS 并重启）",
    )
    p.add_argument("--timeout", type=float, default=15.0, help="单次请求超时秒数")
    args = p.parse_args()
    base: str = args.base.rstrip("/")
    timeout: float = args.timeout

    ok = True

    # --- GET（安全）---
    for path, label in [
        ("/api/live_state", "GET /api/live_state"),
        ("/api/status", "GET /api/status"),
        ("/api/wifi/provision", "GET /api/wifi/provision"),
        ("/api/wifi/scan", "GET /api/wifi/scan"),
    ]:
        try:
            st, body, _ = http_request("GET", base + path, timeout=timeout)
            print_result(label, st, body)
            if st not in (200, 202):
                ok = False
            if st == 200 and path in ("/api/live_state", "/api/status"):
                j = try_parse_json(body)
                if j is None:
                    print("[WARN] 响应非 JSON")
                    ok = False
        except urllib.error.URLError as e:
            print(f"\n=== {label} ===\n[FAIL] {e}", file=sys.stderr)
            ok = False

    # --- POST（非法配网体：应 400，且不写入有效 NVS）---
    try:
        st, body, _ = http_request(
            "POST",
            base + "/api/wifi/connect",
            json_body={},
            timeout=timeout,
        )
        print_result("POST /api/wifi/connect (空 JSON {} 期望 400)", st, body)
        if st != 400:
            print(f"[WARN] 期望 HTTP 400，实际 {st}")
    except urllib.error.URLError as e:
        print(f"\n=== POST /api/wifi/connect ===\n[FAIL] {e}", file=sys.stderr)
        ok = False

    if args.factory_reset:
        print("\n*** 即将执行工厂复位：将清空 NVS 并重启设备 ***\n")
        try:
            st, body, _ = http_request(
                "POST",
                base + "/api/factory_reset",
                json_body={},
                timeout=timeout,
            )
            print_result("POST /api/factory_reset", st, body)
            if st != 200:
                ok = False
            j = try_parse_json(body)
            if not isinstance(j, dict) or not j.get("ok"):
                print("[WARN] 期望 JSON {\"ok\":true}")
                ok = False
            print(
                "\n[NOTE] 设备应已重启；若 STA 凭据被清除，"
                "192.168.1.80 可能暂时不可达，请连设备热点重新配网。"
            )
        except urllib.error.URLError as e:
            # 复位后连接被掐断也属预期
            print(f"\n=== POST /api/factory_reset ===\n[NOTE] 连接异常（重启后常见）: {e}")
    else:
        print(
            "\n[INFO] 未执行工厂复位。若要实测 POST /api/factory_reset，请追加参数: --factory-reset"
        )

    print("\n=== 汇总 ===")
    print("PASS" if ok else "FAIL (部分检查未通过或网络错误)")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
