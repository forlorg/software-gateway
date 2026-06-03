#!/usr/bin/env python3
"""
Enumerate host serial ports and log names plus available attributes (pyserial).
Run: python scripts/serial_ports_enumeration_test.py
"""

from __future__ import annotations

import logging
import sys
from typing import Any


def _configure_logging() -> None:
    if sys.platform == "win32":
        for stream in (sys.stdout, sys.stderr):
            reconf = getattr(stream, "reconfigure", None)
            if callable(reconf):
                try:
                    reconf(encoding="utf-8", errors="replace")
                except (OSError, ValueError):
                    pass
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )


def _safe_attr(obj: Any, name: str) -> str:
    try:
        val = getattr(obj, name)
        if callable(val):
            return "<callable>"
        return repr(val)
    except Exception as exc:  # noqa: BLE001 — diagnostic script
        return f"<error: {exc}>"


def enumerate_ports() -> int:
    try:
        from serial.tools import list_ports
    except ImportError:
        logging.error(
            "缺少 pyserial，请先安装: pip install pyserial",
        )
        return 2

    ports = list(list_ports.comports())
    logging.info("发现串口数量: %d", len(ports))

    if not ports:
        logging.warning("未检测到任何串口设备")
        return 0

    for idx, p in enumerate(ports, start=1):
        logging.info("======== 串口 #%d ========", idx)
        # 常见公开字段优先打印，便于阅读
        for key in (
            "device",
            "name",
            "description",
            "manufacturer",
            "product",
            "serial_number",
            "vid",
            "pid",
            "hwid",
            "location",
            "interface",
        ):
            if hasattr(p, key):
                logging.info("  %s: %s", key, _safe_attr(p, key))

        # 其余非私有、非已列出的属性
        extra = sorted(
            n
            for n in dir(p)
            if not n.startswith("_")
            and n not in {
                "device",
                "name",
                "description",
                "manufacturer",
                "product",
                "serial_number",
                "vid",
                "pid",
                "hwid",
                "location",
                "interface",
            }
        )
        for name in extra:
            val = _safe_attr(p, name)
            if val == "<callable>":
                continue
            logging.info("  %s: %s", name, val)

    return 0


def main() -> int:
    _configure_logging()
    logging.info("开始串口枚举测试")
    code = enumerate_ports()
    logging.info("串口枚举测试结束, exit=%s", code)
    return code


if __name__ == "__main__":
    sys.exit(main())
