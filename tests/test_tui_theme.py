"""The terminal theme uses only red, grey, black, and the terminal's own colors."""
import locale
import os
import re
import subprocess
import unittest

from test_tui import BINARY, TerminalSession, TerminalTestCase

SGR = re.compile(rb"\x1b\[([0-9;]*)m")
# Red 196, grey 244, black 16, plus the terminal's default foreground/background.
ALLOWED_256 = {"38;5;196", "38;5;244", "38;5;16", "48;5;196", "39", "49"}


def utf8_locale():
    for name in ("C.UTF-8", "en_US.UTF-8"):
        try:
            current = locale.setlocale(locale.LC_CTYPE)
            locale.setlocale(locale.LC_CTYPE, name)
            locale.setlocale(locale.LC_CTYPE, current)
            return name
        except locale.Error:
            continue
    return None


class ThemedSession(TerminalSession):
    """A terminal session with colors enabled and an explicit locale."""

    def __init__(self, home, language, colors=True):
        self.master, self.slave = os.openpty()
        self.original = __import__("termios").tcgetattr(self.slave)
        self.resize(24, 80)
        env = {key: value for key, value in os.environ.items() if key not in ("NO_COLOR", "LANG", "LC_ALL", "LC_CTYPE")}
        env.update(TERM="xterm-256color", LC_ALL=language)
        if not colors:
            env["NO_COLOR"] = "1"
        self.process = subprocess.Popen([BINARY, "--home", str(home)], stdin=self.slave, stdout=self.slave,
                                        stderr=self.slave, env=env, close_fds=True)
        self.output = b""


def color_codes(output):
    codes = set()
    for match in SGR.finditer(output):
        parts = match.group(1).decode().split(";")
        index = 0
        while index < len(parts):
            part = parts[index]
            if part in ("38", "48") and index + 2 < len(parts) and parts[index + 1] == "5":
                codes.add(";".join(parts[index:index + 3])); index += 3; continue
            if part and (30 <= int(part) <= 49 or 90 <= int(part) <= 107):
                codes.add(part)
            index += 1
    return codes


class ThemeTests(TerminalTestCase):
    def setUp(self):
        super().setUp()
        for name, agent in (("alpha", "codex"), ("beta", None)):
            self.run_cli("mcp", "add", name, "--command", "/usr/bin/true")
            if agent:
                self.run_cli("mcp", "enable", name, "--agent", agent)

    def session(self, language, colors=True):
        terminal = ThemedSession(self.home, language, colors)
        self.addCleanup(terminal.close)
        terminal.wait_text("1 MCPs")
        terminal.send(b"j")
        terminal.read_for(0.5)
        return terminal

    def finish(self, terminal):
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)

    def test_colors_are_limited_to_red_grey_black_and_terminal_defaults(self):
        terminal = self.session("C")
        terminal.send(b"a")
        terminal.wait_text("Assign agents")
        terminal.send(b"\x1b")
        terminal.read_for(0.3)
        self.finish(terminal)
        codes = color_codes(terminal.output)
        self.assertIn("38;5;196", codes, "red accent is used")
        self.assertIn("38;5;244", codes, "grey secondary text is used")
        self.assertTrue(codes <= ALLOWED_256, f"unexpected colors: {sorted(codes - ALLOWED_256)}")

    def test_no_color_disables_every_color_sequence(self):
        terminal = self.session("C", colors=False)
        self.finish(terminal)
        self.assertEqual(color_codes(terminal.output) - {"39", "49"}, set())

    def test_client_cells_use_symbols_in_utf8_and_brackets_otherwise(self):
        language = utf8_locale()
        if not language:
            self.skipTest("no UTF-8 locale available")
        terminal = self.session(language)
        self.finish(terminal)
        self.assertIn("■".encode(), terminal.output)
        self.assertNotIn(b"[x]", terminal.output)
        ascii_terminal = self.session("C")
        self.finish(ascii_terminal)
        self.assertIn(b"[x]", ascii_terminal.output)


if __name__ == "__main__":
    unittest.main()
