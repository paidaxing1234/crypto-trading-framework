#!/usr/bin/env python3
"""
飞书(Lark)告警组件 - 风控系统

支持两种发送模式：
1. Webhook 机器人（群通知）
2. Open API 单聊（私信到个人，支持富文本卡片）

使用方式：
1. Webhook 模式:
    lark = LarkAlertService(webhook_url="https://open.feishu.cn/open-apis/bot/v2/hook/xxx")
    lark.send_alert("策略异常", AlertLevel.WARNING)

2. Open API 单聊模式:
    lark = LarkAlertService(
        app_id="cli_xxx", app_secret="xxx",
        notify_emails=["user@example.com"]
    )
    lark.send_alert("策略异常", AlertLevel.CRITICAL)

3. 命令行 (供 C++ 使用):
    python lark_alert.py -m "策略异常" -l critical
"""

import os
import sys
import json
import argparse
import threading
import requests
import hashlib
import hmac
import base64
import time
import socket
from datetime import datetime
from enum import Enum
from typing import Dict, List, Optional


# DNS 解析失败时的域名到 IP 回退映射
_DNS_FALLBACK = {
    "open.larksuite.com": ["58.215.79.229", "58.215.79.231", "58.215.79.230"],
    "open.feishu.cn": ["58.215.79.229", "58.215.79.231", "58.215.79.230"],
}

# Monkey-patch socket.getaddrinfo 以支持 DNS fallback
_original_getaddrinfo = socket.getaddrinfo

def _patched_getaddrinfo(host, port, *args, **kwargs):
    try:
        return _original_getaddrinfo(host, port, *args, **kwargs)
    except socket.gaierror:
        if host in _DNS_FALLBACK:
            fallback_ip = _DNS_FALLBACK[host][0]
            print(f"[Lark] DNS 回退: {host} -> {fallback_ip}")
            return _original_getaddrinfo(fallback_ip, port, *args, **kwargs)
        raise

socket.getaddrinfo = _patched_getaddrinfo


class AlertLevel(Enum):
    """告警级别"""
    INFO = 1
    WARNING = 2
    CRITICAL = 3


# 飞书 API 域名
FEISHU_DOMAIN = "https://open.feishu.cn"
LARK_DOMAIN = "https://open.larksuite.com"


class LarkAlertService:
    """
    飞书告警服务 - 同时支持 Webhook + Open API 单聊

    优先级：如果配置了 app_id/app_secret + notify_emails，走 Open API 单聊；
    否则走 webhook 群通知。两者可以同时启用。
    """

    def __init__(
        self,
        webhook_url: str = "",
        secret: str = "",
        app_id: str = "",
        app_secret: str = "",
        notify_emails: Optional[List[str]] = None,
        notify_phones: Optional[List[str]] = None,
        domain: str = "",
        enabled: bool = True,
        min_alert_interval: int = 10,
        alert_level_threshold: AlertLevel = AlertLevel.INFO,
        config_file: str = "",
    ):
        if config_file and os.path.exists(config_file):
            self._load_config(config_file)
        else:
            self.webhook_url = webhook_url
            self.secret = secret
            self.app_id = app_id
            self.app_secret = app_secret
            self.notify_emails = notify_emails or []
            self.notify_phones = notify_phones or []
            self.domain = domain or FEISHU_DOMAIN
            self.enabled = enabled
            self.min_alert_interval = min_alert_interval
            self.alert_level_threshold = alert_level_threshold

        self._load_from_env()

        # token 缓存
        self._tenant_token: str = ""
        self._token_expire_at: float = 0

        self._last_alert_time: Dict[str, datetime] = {}
        self._alert_history: List[Dict] = []
        self._lock = threading.Lock()

    def _load_config(self, config_file: str):
        """从配置文件加载"""
        with open(config_file, 'r') as f:
            config = json.load(f)
        self.webhook_url = config.get("webhook_url", "")
        self.secret = config.get("secret", "")
        self.app_id = config.get("app_id", "")
        self.app_secret = config.get("app_secret", "")
        self.notify_emails = config.get("notify_emails", [])
        self.notify_phones = config.get("notify_phones", [])
        self.domain = config.get("domain", FEISHU_DOMAIN)
        self.enabled = config.get("enabled", True)
        self.min_alert_interval = config.get("min_alert_interval", 10)
        threshold = config.get("alert_level_threshold", "INFO")
        self.alert_level_threshold = AlertLevel[threshold]

    def _load_from_env(self):
        """从环境变量加载（优先级最高）"""
        if os.getenv("LARK_WEBHOOK_URL"):
            self.webhook_url = os.getenv("LARK_WEBHOOK_URL")
        if os.getenv("LARK_SECRET"):
            self.secret = os.getenv("LARK_SECRET")
        if os.getenv("LARK_APP_ID"):
            self.app_id = os.getenv("LARK_APP_ID")
        if os.getenv("LARK_APP_SECRET"):
            self.app_secret = os.getenv("LARK_APP_SECRET")
        if os.getenv("LARK_NOTIFY_EMAILS"):
            self.notify_emails = [e.strip() for e in os.getenv("LARK_NOTIFY_EMAILS").split(",") if e.strip()]
        if os.getenv("LARK_DOMAIN"):
            self.domain = os.getenv("LARK_DOMAIN")

    # ======================== Open API: Token ========================

    def _get_tenant_token(self) -> str:
        """获取 tenant_access_token（自动缓存，过期前5分钟刷新）"""
        if self._tenant_token and time.time() < self._token_expire_at - 300:
            return self._tenant_token

        url = f"{self.domain}/open-apis/auth/v3/tenant_access_token/internal"
        resp = requests.post(url, json={
            "app_id": self.app_id,
            "app_secret": self.app_secret,
        }, timeout=10)
        result = resp.json()

        if result.get("code") != 0:
            print(f"[Lark] 获取token失败: {result.get('msg', 'Unknown')}")
            return ""

        self._tenant_token = result["tenant_access_token"]
        self._token_expire_at = time.time() + result.get("expire", 7200)
        print(f"[Lark] token获取成功, 有效期 {result.get('expire', 7200)}s")
        return self._tenant_token

    @property
    def open_api_ready(self) -> bool:
        return bool(self.app_id and self.app_secret and (self.notify_emails or self.notify_phones))

    @property
    def webhook_ready(self) -> bool:
        return bool(self.webhook_url)

    # ======================== Webhook: 签名 ========================

    def _gen_sign(self, timestamp: str) -> str:
        """飞书签名: base64(hmac_sha256(timestamp + "\n" + secret))"""
        string_to_sign = f'{timestamp}\n{self.secret}'
        hmac_code = hmac.new(
            string_to_sign.encode('utf-8'),
            digestmod=hashlib.sha256
        ).digest()
        return base64.b64encode(hmac_code).decode('utf-8')

    # ======================== 卡片构建 ========================

    def _build_card(self, title: str, message: str, level: AlertLevel, color: str) -> dict:
        """构建飞书卡片消息体"""
        level_labels = {
            AlertLevel.INFO: "ℹ️ INFO",
            AlertLevel.WARNING: "⚠️ WARNING",
            AlertLevel.CRITICAL: "🚨 CRITICAL",
        }
        return {
            "header": {
                "title": {"tag": "plain_text", "content": title},
                "template": color,
            },
            "elements": [
                {
                    "tag": "div",
                    "fields": [
                        {"is_short": True, "text": {"tag": "lark_md", "content": f"**告警级别**\n{level_labels[level]}"}},
                        {"is_short": True, "text": {"tag": "lark_md", "content": f"**时间**\n{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}"}},
                    ],
                },
                {"tag": "hr"},
                {
                    "tag": "div",
                    "text": {"tag": "lark_md", "content": message},
                },
            ],
        }

    # ======================== 核心发送 ========================

    def send_alert(
        self,
        message: str,
        level: AlertLevel = AlertLevel.WARNING,
        alert_type: str = "default",
        title: str = "",
        force: bool = False
    ) -> bool:
        """发送飞书告警（同时走 webhook + Open API 单聊）"""
        if not self.enabled:
            print("[Lark] 飞书服务已禁用")
            return False

        if level.value < self.alert_level_threshold.value:
            return False

        if not force:
            with self._lock:
                last_time = self._last_alert_time.get(alert_type)
                if last_time:
                    elapsed = (datetime.now() - last_time).total_seconds()
                    if elapsed < self.min_alert_interval:
                        print(f"[Lark] 告警间隔不足，距上次 {elapsed:.0f}s")
                        return False

        level_colors = {
            AlertLevel.INFO: "blue",
            AlertLevel.WARNING: "orange",
            AlertLevel.CRITICAL: "red",
        }
        level_icons = {
            AlertLevel.INFO: "ℹ️",
            AlertLevel.WARNING: "⚠️",
            AlertLevel.CRITICAL: "🚨",
        }

        color = level_colors[level]
        icon = level_icons[level]
        title = title or f"{icon} 交易系统告警"

        print(f"[Lark] 发送告警: {message[:50]}...")

        success = False
        card = self._build_card(title, message, level, color)

        # 1) Webhook 群通知
        if self.webhook_ready:
            success = self._send_webhook(card) or success

        # 2) Open API 单聊私信
        if self.open_api_ready:
            success = self._send_open_api(card) or success

        if not self.webhook_ready and not self.open_api_ready:
            print("[Lark] 未配置任何发送通道（webhook / open_api）")

        with self._lock:
            self._alert_history.append({
                "time": datetime.now().isoformat(),
                "message": message, "level": level.name,
                "type": alert_type, "sent": success,
            })
            if len(self._alert_history) > 1000:
                self._alert_history = self._alert_history[-500:]
            if success:
                self._last_alert_time[alert_type] = datetime.now()

        return success

    # ======================== Webhook 发送 ========================

    def _send_webhook(self, card: dict) -> bool:
        """通过 Webhook 发送卡片到群"""
        data = {"msg_type": "interactive", "card": card}
        try:
            if self.secret:
                timestamp = str(int(time.time()))
                sign = self._gen_sign(timestamp)
                data["timestamp"] = timestamp
                data["sign"] = sign

            resp = requests.post(
                self.webhook_url,
                headers={"Content-Type": "application/json"},
                json=data, timeout=30,
            )
            result = resp.json()
            if result.get("code") == 0 or result.get("StatusCode") == 0:
                print("[Lark] Webhook发送成功")
                return True
            else:
                print(f"[Lark] Webhook发送失败: {result.get('msg', result.get('StatusMessage', 'Unknown'))}")
                return False
        except Exception as e:
            print(f"[Lark] Webhook异常: {e}")
            return False

    # ======================== Open API 单聊 ========================

    def _send_open_api(self, card: dict) -> bool:
        """通过 Open API 发送卡片私信给每个人"""
        token = self._get_tenant_token()
        if not token:
            return False

        any_success = False

        # 按 email 发送
        for email in self.notify_emails:
            ok = self._send_message_to(token, "email", email, card)
            any_success = any_success or ok

        # 按手机号发送
        for phone in self.notify_phones:
            ok = self._send_message_to(token, "phone", phone, card)
            any_success = any_success or ok

        return any_success

    def _send_message_to(self, token: str, id_type: str, receive_id: str, card: dict) -> bool:
        """发送卡片消息给单个用户"""
        url = f"{self.domain}/open-apis/im/v1/messages?receive_id_type={id_type}"
        headers = {
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
        }
        payload = {
            "receive_id": receive_id,
            "msg_type": "interactive",
            "content": json.dumps(card),
        }
        try:
            resp = requests.post(url, headers=headers, json=payload, timeout=30)
            result = resp.json()
            if result.get("code") == 0:
                print(f"[Lark] 单聊发送成功 -> {receive_id}")
                return True
            else:
                print(f"[Lark] 单聊发送失败 -> {receive_id}: {result.get('msg', 'Unknown')}")
                return False
        except Exception as e:
            print(f"[Lark] 单聊异常 -> {receive_id}: {e}")
            return False

    # ======================== 便捷方法 ========================

    def send_text(self, content: str) -> bool:
        """发送纯文本（仅 webhook）"""
        if not self.webhook_ready:
            print("[Lark] Webhook未配置，无法发送纯文本")
            return False
        data = {"msg_type": "text", "content": {"text": content}}
        try:
            if self.secret:
                timestamp = str(int(time.time()))
                sign = self._gen_sign(timestamp)
                data["timestamp"] = timestamp
                data["sign"] = sign
            resp = requests.post(self.webhook_url, json=data, timeout=30)
            result = resp.json()
            return result.get("code") == 0 or result.get("StatusCode") == 0
        except Exception as e:
            print(f"[Lark] 发送异常: {e}")
            return False

    def send_alert_async(self, message: str, level: AlertLevel = AlertLevel.WARNING,
                         alert_type: str = "default", title: str = ""):
        """异步发送告警"""
        threading.Thread(
            target=self.send_alert,
            args=(message, level, alert_type, title),
            daemon=True,
        ).start()

    def get_alert_history(self, limit: int = 100) -> List[Dict]:
        with self._lock:
            return self._alert_history[-limit:]

    def test_lark(self) -> bool:
        """测试飞书功能"""
        print("[Lark] 开始测试飞书功能...")
        return self.send_alert(
            "这是一条测试消息，请忽略。\n\nThis is a test message, please ignore.",
            AlertLevel.INFO, "test", "🔔 测试消息", force=True,
        )

    def get_config_status(self) -> Dict:
        return {
            "enabled": self.enabled,
            "webhook_configured": self.webhook_ready,
            "open_api_configured": self.open_api_ready,
            "app_id": self.app_id[:8] + "..." if self.app_id else "",
            "notify_emails": self.notify_emails,
            "notify_phones": self.notify_phones,
            "domain": self.domain,
            "secret_configured": bool(self.secret),
            "min_alert_interval": self.min_alert_interval,
            "alert_level_threshold": self.alert_level_threshold.name,
        }


def main():
    """命令行入口"""
    parser = argparse.ArgumentParser(description="飞书告警服务")
    parser.add_argument("--message", "-m", required=True, help="告警消息")
    parser.add_argument("--title", default="", help="消息标题")
    parser.add_argument("--level", "-l", default="warning", choices=["info", "warning", "critical"])
    parser.add_argument("--type", "-t", default="default", help="告警类型")
    parser.add_argument("--force", "-f", action="store_true", help="强制发送")
    parser.add_argument("--config", "-c", default="", help="配置文件路径")
    parser.add_argument("--test", action="store_true", help="测试模式")
    parser.add_argument("--status", action="store_true", help="显示配置状态")
    parser.add_argument("--text", action="store_true", help="发送纯文本消息")
    parser.add_argument("--to-emails", default="", help="覆盖私信收件人飞书邮箱（逗号分隔）")
    parser.add_argument("--to-phones", default="", help="覆盖私信收件人手机号（逗号分隔）")

    args = parser.parse_args()
    service = LarkAlertService(config_file=args.config)

    # 命令行指定的收件人覆盖配置文件
    if args.to_emails:
        service.notify_emails = [e.strip() for e in args.to_emails.split(",") if e.strip()]
    if args.to_phones:
        service.notify_phones = [p.strip() for p in args.to_phones.split(",") if p.strip()]

    if args.status:
        print(json.dumps(service.get_config_status(), indent=2, ensure_ascii=False))
        return 0
    if args.test:
        return 0 if service.test_lark() else 1
    if args.text:
        return 0 if service.send_text(args.message) else 1

    level_map = {"info": AlertLevel.INFO, "warning": AlertLevel.WARNING, "critical": AlertLevel.CRITICAL}
    success = service.send_alert(args.message, level_map[args.level], args.type, args.title, args.force)
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
