"""Load styles.qss, resolving the tokens Qt's stylesheet syntax can't.

Same shape as ClipPortal's loader: the .qss on disk is a template, and the
accent colour is substituted here so the whole palette can be retuned from
one place while iterating on the design.
"""

from __future__ import annotations

import os

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
QSS_PATH = os.path.join(BASE_DIR, "styles.qss")

DEFAULT_ACCENT = "#8ab4ff"


def render(accent: str = DEFAULT_ACCENT) -> str:
    with open(QSS_PATH, "r", encoding="utf-8") as fh:
        return fh.read().replace("@ACCENT@", accent)


def apply_to(app, accent: str = DEFAULT_ACCENT) -> None:
    app.setStyleSheet(render(accent))
