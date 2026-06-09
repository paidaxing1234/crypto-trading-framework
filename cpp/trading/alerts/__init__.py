#!/usr/bin/env python3
"""
告警服务模块 - 风控系统

提供统一的告警接口，支持告警渠道：
- 邮件告警 (email_alert)
- 飞书告警 (lark_alert)

使用示例：
    from alerts import AlertManager, AlertLevel

    manager = AlertManager()
    manager.send_alert("策略异常", AlertLevel.CRITICAL)

    from alerts import EmailAlertService, LarkAlertService
"""

from enum import Enum
from typing import Dict, List
import threading
from datetime import datetime

from .email_alert import EmailAlertService, AlertLevel as EmailAlertLevel, create_service_with_preset, SMTP_PRESETS
from .lark_alert import LarkAlertService, AlertLevel as LarkAlertLevel


class AlertLevel(Enum):
    """告警级别"""
    INFO = 1
    WARNING = 2
    CRITICAL = 3


class AlertManager:
    """
    统一告警管理器

    使用示例：
        manager = AlertManager()
        manager.add_email_service(EmailAlertService(...))
        manager.add_lark_service(LarkAlertService(...))
        manager.send_alert("策略异常", AlertLevel.CRITICAL)
    """

    def __init__(self):
        self._email_services: List[EmailAlertService] = []
        self._lark_services: List[LarkAlertService] = []

        self._routing: Dict[AlertLevel, List[str]] = {
            AlertLevel.INFO: ["lark", "email"],
            AlertLevel.WARNING: ["lark", "email"],
            AlertLevel.CRITICAL: ["lark", "email"],
        }

        self._alert_history: List[Dict] = []
        self._lock = threading.Lock()

    def add_email_service(self, service: EmailAlertService):
        self._email_services.append(service)

    def add_lark_service(self, service: LarkAlertService):
        self._lark_services.append(service)

    def set_routing(self, routing: Dict[AlertLevel, List[str]]):
        self._routing = routing

    def send_alert(
        self,
        message: str,
        level: AlertLevel = AlertLevel.WARNING,
        alert_type: str = "default",
        title: str = "",
        force: bool = False,
    ) -> Dict[str, bool]:
        channels = self._routing.get(level, [])
        results = {}
        level_value = level.value

        if "email" in channels:
            for service in self._email_services:
                email_level = EmailAlertLevel(level_value)
                success = service.send_alert(message, email_level, alert_type, title, force)
                results["email"] = results.get("email", False) or success

        if "lark" in channels:
            for service in self._lark_services:
                lark_level = LarkAlertLevel(level_value)
                success = service.send_alert(message, lark_level, alert_type, title, force)
                results["lark"] = results.get("lark", False) or success

        with self._lock:
            self._alert_history.append({
                "time": datetime.now().isoformat(),
                "message": message, "level": level.name,
                "type": alert_type, "results": results,
            })
            if len(self._alert_history) > 1000:
                self._alert_history = self._alert_history[-500:]

        return results

    def send_alert_async(self, message: str, level: AlertLevel = AlertLevel.WARNING,
                         alert_type: str = "default", title: str = ""):
        threading.Thread(target=self.send_alert, args=(message, level, alert_type, title), daemon=True).start()

    def get_alert_history(self, limit: int = 100) -> List[Dict]:
        with self._lock:
            return self._alert_history[-limit:]

    def get_status(self) -> Dict:
        return {
            "email_services": len(self._email_services),
            "lark_services": len(self._lark_services),
            "routing": {k.name: v for k, v in self._routing.items()},
            "alert_history_count": len(self._alert_history),
        }


def create_alert_manager_from_env() -> AlertManager:
    """从环境变量创建告警管理器"""
    import os
    manager = AlertManager()

    if os.getenv("SMTP_HOST") or os.getenv("SENDGRID_API_KEY"):
        manager.add_email_service(EmailAlertService())

    if os.getenv("LARK_WEBHOOK_URL"):
        manager.add_lark_service(LarkAlertService())

    return manager


__all__ = [
    "AlertLevel",
    "AlertManager",
    "create_alert_manager_from_env",
    "EmailAlertService",
    "LarkAlertService",
    "create_service_with_preset",
    "SMTP_PRESETS",
]
