#!/usr/bin/env python3
"""
邮件告警组件 - 风控系统

支持的发送方式：
- smtp: 标准 SMTP 协议（支持 Gmail, QQ邮箱, 163邮箱等）
- sendgrid: SendGrid API
- mailgun: Mailgun API

使用方式：
1. Python 直接调用:
    from email_alert import EmailAlertService, AlertLevel
    email = EmailAlertService(smtp_host="smtp.qq.com", ...)
    email.send_alert("策略异常", AlertLevel.WARNING)

2. 命令行调用 (供 C++ 使用):
    python email_alert.py --message "策略异常" --level warning --subject "交易告警"
"""

import os
import sys
import json
import argparse
import threading
import smtplib
import socket
import requests
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from email.header import Header
from email.utils import formataddr
from datetime import datetime
from enum import Enum
from typing import Dict, List, Optional


# DNS 解析失败时的 SMTP 主机名到 IP 的回退映射
SMTP_HOST_FALLBACK_IPS = {
    "smtp.qq.com": ["183.47.101.192"],
    "smtp.163.com": ["220.181.12.18"],
}


class AlertLevel(Enum):
    """告警级别"""
    INFO = 1       # 信息通知
    WARNING = 2    # 警告
    CRITICAL = 3   # 严重告警


class EmailAlertService:
    """
    邮件告警服务组件

    使用示例（SMTP）：
        email_service = EmailAlertService(
            provider="smtp",
            smtp_host="smtp.qq.com",
            smtp_port=465,
            smtp_user="your_email@qq.com",
            smtp_password="your_auth_code",
            from_email="your_email@qq.com",
            to_emails=["alert@example.com"]
        )
        email_service.send_alert("策略异常：持仓超限", AlertLevel.WARNING)
    """

    def __init__(
        self,
        provider: str = "smtp",
        # SMTP 配置
        smtp_host: str = "",
        smtp_port: int = 465,
        smtp_user: str = "",
        smtp_password: str = "",
        smtp_use_ssl: bool = True,
        smtp_use_tls: bool = False,
        from_email: str = "",
        from_name: str = "交易系统告警",
        to_emails: List[str] = None,
        # SendGrid 配置
        sendgrid_api_key: str = "",
        # Mailgun 配置
        mailgun_api_key: str = "",
        mailgun_domain: str = "",
        # 通用配置
        enabled: bool = True,
        min_alert_interval: int = 30,  # 最小告警间隔（秒）
        alert_level_threshold: AlertLevel = AlertLevel.INFO,
        default_subject: str = "[交易告警]",
        config_file: str = "",
    ):
        if config_file and os.path.exists(config_file):
            self._load_config(config_file)
        else:
            self.provider = provider
            self.smtp_host = smtp_host
            self.smtp_port = smtp_port
            self.smtp_user = smtp_user
            self.smtp_password = smtp_password
            self.smtp_use_ssl = smtp_use_ssl
            self.smtp_use_tls = smtp_use_tls
            self.from_email = from_email
            self.from_name = from_name
            self.to_emails = to_emails or []

            self.sendgrid_api_key = sendgrid_api_key
            self.mailgun_api_key = mailgun_api_key
            self.mailgun_domain = mailgun_domain

            self.enabled = enabled
            self.min_alert_interval = min_alert_interval
            self.alert_level_threshold = alert_level_threshold
            self.default_subject = default_subject

        self._load_from_env()

        self._last_alert_time: Dict[str, datetime] = {}
        self._alert_history: List[Dict] = []
        self._lock = threading.Lock()

    def _load_config(self, config_file: str):
        """从配置文件加载"""
        with open(config_file, 'r') as f:
            config = json.load(f)

        self.provider = config.get("provider", "smtp")
        self.smtp_host = config.get("smtp_host", "")
        self.smtp_port = config.get("smtp_port", 465)
        self.smtp_user = config.get("smtp_user", "")
        self.smtp_password = config.get("smtp_password", "")
        self.smtp_use_ssl = config.get("smtp_use_ssl", True)
        self.smtp_use_tls = config.get("smtp_use_tls", False)
        self.from_email = config.get("from_email", "")
        self.from_name = config.get("from_name", "交易系统告警")
        self.to_emails = config.get("to_emails", [])

        self.sendgrid_api_key = config.get("sendgrid_api_key", "")
        self.mailgun_api_key = config.get("mailgun_api_key", "")
        self.mailgun_domain = config.get("mailgun_domain", "")

        self.enabled = config.get("enabled", True)
        self.min_alert_interval = config.get("min_alert_interval", 30)
        threshold = config.get("alert_level_threshold", "INFO")
        self.alert_level_threshold = AlertLevel[threshold]
        self.default_subject = config.get("default_subject", "[交易告警]")

    def _load_from_env(self):
        """从环境变量加载配置"""
        # SMTP
        if os.getenv("SMTP_HOST"):
            self.smtp_host = os.getenv("SMTP_HOST")
        if os.getenv("SMTP_PORT"):
            self.smtp_port = int(os.getenv("SMTP_PORT"))
        if os.getenv("SMTP_USER"):
            self.smtp_user = os.getenv("SMTP_USER")
        if os.getenv("SMTP_PASSWORD"):
            self.smtp_password = os.getenv("SMTP_PASSWORD")
        if os.getenv("EMAIL_FROM"):
            self.from_email = os.getenv("EMAIL_FROM")
        if os.getenv("EMAIL_TO"):
            self.to_emails = os.getenv("EMAIL_TO").split(",")

        # SendGrid
        if os.getenv("SENDGRID_API_KEY"):
            self.sendgrid_api_key = os.getenv("SENDGRID_API_KEY")

        # Mailgun
        if os.getenv("MAILGUN_API_KEY"):
            self.mailgun_api_key = os.getenv("MAILGUN_API_KEY")
        if os.getenv("MAILGUN_DOMAIN"):
            self.mailgun_domain = os.getenv("MAILGUN_DOMAIN")

        # 通用
        if os.getenv("EMAIL_ALERT_PROVIDER"):
            self.provider = os.getenv("EMAIL_ALERT_PROVIDER")

    def send_alert(
        self,
        message: str,
        level: AlertLevel = AlertLevel.WARNING,
        alert_type: str = "default",
        subject: str = "",
        force: bool = False,
        html_content: str = ""
    ) -> bool:
        """
        发送邮件告警

        Args:
            message: 告警消息（纯文本）
            level: 告警级别
            alert_type: 告警类型
            subject: 邮件主题（可选）
            force: 强制发送
            html_content: HTML 内容（可选）
        """
        if not self.enabled:
            print(f"[EmailAlert] 邮件服务已禁用")
            return False

        if level.value < self.alert_level_threshold.value:
            print(f"[EmailAlert] 告警级别 {level.name} 低于阈值 {self.alert_level_threshold.name}")
            return False

        if not force:
            with self._lock:
                last_time = self._last_alert_time.get(alert_type)
                if last_time:
                    elapsed = (datetime.now() - last_time).total_seconds()
                    if elapsed < self.min_alert_interval:
                        print(f"[EmailAlert] 告警间隔不足，距上次 {elapsed:.0f}s")
                        return False

        # 构建主题
        level_prefix = {
            AlertLevel.INFO: "[INFO]",
            AlertLevel.WARNING: "[WARNING]",
            AlertLevel.CRITICAL: "[CRITICAL]"
        }
        full_subject = f"{self.default_subject} {level_prefix[level]} {subject or message[:30]}"

        # 构建 HTML 内容
        if not html_content:
            html_content = self._build_html_content(message, level, alert_type)

        alert_record = {
            "time": datetime.now().isoformat(),
            "message": message,
            "level": level.name,
            "type": alert_type,
            "subject": full_subject,
            "sent": False
        }

        print(f"[EmailAlert] 发送邮件: {full_subject}")
        success = self._send_email(full_subject, message, html_content)
        alert_record["sent"] = success

        with self._lock:
            self._alert_history.append(alert_record)
            if len(self._alert_history) > 1000:
                self._alert_history = self._alert_history[-500:]
            if success:
                self._last_alert_time[alert_type] = datetime.now()

        return success

    def _build_html_content(self, message: str, level: AlertLevel, alert_type: str) -> str:
        """构建 HTML 邮件内容"""
        level_colors = {
            AlertLevel.INFO: "#17a2b8",
            AlertLevel.WARNING: "#ffc107",
            AlertLevel.CRITICAL: "#dc3545"
        }
        color = level_colors[level]

        html = f"""
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="UTF-8">
            <style>
                body {{ font-family: Arial, sans-serif; margin: 0; padding: 20px; background: #f5f5f5; }}
                .container {{ max-width: 600px; margin: 0 auto; background: white; border-radius: 8px; overflow: hidden; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }}
                .header {{ background: {color}; color: white; padding: 20px; text-align: center; }}
                .header h1 {{ margin: 0; font-size: 24px; }}
                .content {{ padding: 30px; }}
                .message {{ background: #f8f9fa; padding: 20px; border-radius: 4px; border-left: 4px solid {color}; margin: 20px 0; }}
                .info {{ color: #666; font-size: 14px; }}
                .footer {{ background: #f8f9fa; padding: 15px; text-align: center; color: #999; font-size: 12px; }}
            </style>
        </head>
        <body>
            <div class="container">
                <div class="header">
                    <h1>交易系统告警</h1>
                </div>
                <div class="content">
                    <p class="info">
                        <strong>告警级别:</strong> {level.name}<br>
                        <strong>告警类型:</strong> {alert_type}<br>
                        <strong>时间:</strong> {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
                    </p>
                    <div class="message">
                        <p>{message}</p>
                    </div>
                </div>
                <div class="footer">
                    此邮件由交易系统自动发送，请勿直接回复
                </div>
            </div>
        </body>
        </html>
        """
        return html

    def _send_email(self, subject: str, text_content: str, html_content: str) -> bool:
        """发送邮件"""
        try:
            if self.provider == "smtp":
                return self._send_smtp(subject, text_content, html_content)
            elif self.provider == "sendgrid":
                return self._send_sendgrid(subject, text_content, html_content)
            elif self.provider == "mailgun":
                return self._send_mailgun(subject, text_content, html_content)
            else:
                print(f"[EmailAlert] 不支持的服务商: {self.provider}")
                return False
        except Exception as e:
            print(f"[EmailAlert] 发送失败: {e}")
            return False

    def _send_smtp(self, subject: str, text_content: str, html_content: str) -> bool:
        """通过 SMTP 发送邮件"""
        if not all([self.smtp_host, self.smtp_user, self.smtp_password, self.to_emails]):
            print("[EmailAlert] SMTP 配置不完整")
            print(f"  smtp_host: {'已设置' if self.smtp_host else '未设置'}")
            print(f"  smtp_user: {'已设置' if self.smtp_user else '未设置'}")
            print(f"  smtp_password: {'已设置' if self.smtp_password else '未设置'}")
            print(f"  to_emails: {self.to_emails}")
            return False

        try:
            # 创建邮件
            msg = MIMEMultipart('alternative')
            msg['Subject'] = Header(subject, 'utf-8')
            from_addr = self.from_email or self.smtp_user
            if self.from_name:
                msg['From'] = formataddr((self.from_name, from_addr))
            else:
                msg['From'] = from_addr
            msg['To'] = ', '.join(self.to_emails)

            # 添加纯文本和 HTML 内容
            msg.attach(MIMEText(text_content, 'plain', 'utf-8'))
            msg.attach(MIMEText(html_content, 'html', 'utf-8'))

            # 尝试连接 SMTP：先用域名，DNS 失败则 fallback 到已知 IP
            smtp_targets = [self.smtp_host]
            if self.smtp_host in SMTP_HOST_FALLBACK_IPS:
                smtp_targets.extend(SMTP_HOST_FALLBACK_IPS[self.smtp_host])

            server = None
            last_error = None
            for target in smtp_targets:
                try:
                    if self.smtp_use_ssl:
                        server = smtplib.SMTP_SSL(target, self.smtp_port, timeout=30)
                    else:
                        server = smtplib.SMTP(target, self.smtp_port, timeout=30)
                        if self.smtp_use_tls:
                            server.starttls()
                    if target != self.smtp_host:
                        print(f"[EmailAlert] DNS 回退: 使用 IP {target} 代替 {self.smtp_host}")
                    break
                except (socket.gaierror, OSError) as e:
                    last_error = e
                    print(f"[EmailAlert] 连接 {target}:{self.smtp_port} 失败: {e}")
                    server = None
                    continue

            if server is None:
                print(f"[EmailAlert] 所有 SMTP 连接均失败: {last_error}")
                return False

            server.login(self.smtp_user, self.smtp_password)
            server.sendmail(
                self.from_email or self.smtp_user,
                self.to_emails,
                msg.as_string()
            )
            server.quit()

            print(f"[EmailAlert] SMTP 发送成功: {self.to_emails}")
            return True

        except Exception as e:
            print(f"[EmailAlert] SMTP 发送失败: {e}")
            return False

    def _send_sendgrid(self, subject: str, text_content: str, html_content: str) -> bool:
        """通过 SendGrid 发送邮件"""
        if not all([self.sendgrid_api_key, self.from_email, self.to_emails]):
            print("[EmailAlert] SendGrid 配置不完整")
            return False

        try:
            url = "https://api.sendgrid.com/v3/mail/send"
            headers = {
                "Authorization": f"Bearer {self.sendgrid_api_key}",
                "Content-Type": "application/json"
            }

            data = {
                "personalizations": [{"to": [{"email": email} for email in self.to_emails]}],
                "from": {"email": self.from_email, "name": self.from_name},
                "subject": subject,
                "content": [
                    {"type": "text/plain", "value": text_content},
                    {"type": "text/html", "value": html_content}
                ]
            }

            response = requests.post(url, headers=headers, json=data, timeout=30)
            if response.status_code in [200, 202]:
                print(f"[EmailAlert] SendGrid 发送成功")
                return True
            else:
                print(f"[EmailAlert] SendGrid 发送失败: {response.status_code}, {response.text[:100]}")
                return False

        except Exception as e:
            print(f"[EmailAlert] SendGrid 发送异常: {e}")
            return False

    def _send_mailgun(self, subject: str, text_content: str, html_content: str) -> bool:
        """通过 Mailgun 发送邮件"""
        if not all([self.mailgun_api_key, self.mailgun_domain, self.from_email, self.to_emails]):
            print("[EmailAlert] Mailgun 配置不完整")
            return False

        try:
            url = f"https://api.mailgun.net/v3/{self.mailgun_domain}/messages"

            response = requests.post(
                url,
                auth=("api", self.mailgun_api_key),
                data={
                    "from": f"{self.from_name} <{self.from_email}>",
                    "to": self.to_emails,
                    "subject": subject,
                    "text": text_content,
                    "html": html_content
                },
                timeout=30
            )

            if response.status_code == 200:
                print(f"[EmailAlert] Mailgun 发送成功")
                return True
            else:
                print(f"[EmailAlert] Mailgun 发送失败: {response.status_code}, {response.text[:100]}")
                return False

        except Exception as e:
            print(f"[EmailAlert] Mailgun 发送异常: {e}")
            return False

    def send_alert_async(
        self,
        message: str,
        level: AlertLevel = AlertLevel.WARNING,
        alert_type: str = "default",
        subject: str = ""
    ):
        """异步发送邮件"""
        thread = threading.Thread(
            target=self.send_alert,
            args=(message, level, alert_type, subject),
            daemon=True
        )
        thread.start()

    def get_alert_history(self, limit: int = 100) -> List[Dict]:
        """获取告警历史"""
        with self._lock:
            return self._alert_history[-limit:]

    def test_email(self) -> bool:
        """测试邮件功能"""
        print("[EmailAlert] 开始测试邮件功能...")
        return self.send_alert(
            "这是一条测试邮件，请忽略。\n\nThis is a test email, please ignore.",
            AlertLevel.INFO,
            "test",
            "测试邮件",
            force=True
        )

    def get_config_status(self) -> Dict:
        """获取配置状态"""
        return {
            "provider": self.provider,
            "enabled": self.enabled,
            "to_emails": self.to_emails,
            "from_email": self.from_email,
            "min_alert_interval": self.min_alert_interval,
            "alert_level_threshold": self.alert_level_threshold.name,
            "smtp_configured": bool(self.smtp_host and self.smtp_user and self.smtp_password),
            "sendgrid_configured": bool(self.sendgrid_api_key),
            "mailgun_configured": bool(self.mailgun_api_key and self.mailgun_domain),
        }


# 常用邮箱 SMTP 配置预设
SMTP_PRESETS = {
    "qq": {
        "smtp_host": "smtp.qq.com",
        "smtp_port": 465,
        "smtp_use_ssl": True,
        "smtp_use_tls": False,
    },
    "163": {
        "smtp_host": "smtp.163.com",
        "smtp_port": 465,
        "smtp_use_ssl": True,
        "smtp_use_tls": False,
    },
    "gmail": {
        "smtp_host": "smtp.gmail.com",
        "smtp_port": 587,
        "smtp_use_ssl": False,
        "smtp_use_tls": True,
    },
    "outlook": {
        "smtp_host": "smtp.office365.com",
        "smtp_port": 587,
        "smtp_use_ssl": False,
        "smtp_use_tls": True,
    },
    "aliyun": {
        "smtp_host": "smtp.aliyun.com",
        "smtp_port": 465,
        "smtp_use_ssl": True,
        "smtp_use_tls": False,
    },
}


def create_service_with_preset(preset: str, **kwargs) -> EmailAlertService:
    """使用预设配置创建服务"""
    if preset not in SMTP_PRESETS:
        raise ValueError(f"未知的预设: {preset}，可用: {list(SMTP_PRESETS.keys())}")

    config = SMTP_PRESETS[preset].copy()
    config.update(kwargs)
    return EmailAlertService(**config)


def main():
    """命令行入口"""
    parser = argparse.ArgumentParser(description="邮件告警服务")
    parser.add_argument("--provider", default="smtp", choices=["smtp", "sendgrid", "mailgun"])
    parser.add_argument("--preset", choices=["qq", "163", "gmail", "outlook", "aliyun"],
                        help="使用预设的 SMTP 配置")
    parser.add_argument("--message", "-m", required=True, help="告警消息")
    parser.add_argument("--subject", "-s", default="", help="邮件主题")
    parser.add_argument("--level", "-l", default="warning", choices=["info", "warning", "critical"])
    parser.add_argument("--type", "-t", default="default", help="告警类型")
    parser.add_argument("--force", "-f", action="store_true", help="强制发送")
    parser.add_argument("--config", "-c", default="", help="配置文件路径")
    parser.add_argument("--to", default="", help="收件人邮箱，多个用逗号分隔（覆盖配置文件）")
    parser.add_argument("--test", action="store_true", help="测试模式")
    parser.add_argument("--status", action="store_true", help="显示配置状态")

    args = parser.parse_args()

    # 创建服务
    if args.preset:
        service = create_service_with_preset(args.preset, config_file=args.config)
    else:
        service = EmailAlertService(provider=args.provider, config_file=args.config)

    # 命令行指定的收件人覆盖配置文件
    if args.to:
        service.to_emails = [email.strip() for email in args.to.split(",") if email.strip()]

    if args.status:
        status = service.get_config_status()
        print(json.dumps(status, indent=2, ensure_ascii=False))
        print("\n可用的 SMTP 预设:")
        for name, config in SMTP_PRESETS.items():
            print(f"  {name}: {config['smtp_host']}:{config['smtp_port']}")
        return 0

    if args.test:
        success = service.test_email()
        return 0 if success else 1

    level_map = {"info": AlertLevel.INFO, "warning": AlertLevel.WARNING, "critical": AlertLevel.CRITICAL}
    success = service.send_alert(args.message, level_map[args.level], args.type, args.subject, args.force)
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
