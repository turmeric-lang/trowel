"""Fixtures for the Trowel smoke suite.

Each test drives a real Trowel process through the control socket. Tests
never sleep — every wait uses `wait.*` with an explicit timeout.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator

import pytest

REPO = Path(__file__).resolve().parents[2]

# POSITIONS ARE BYTE OFFSETS, not character indices.
#
# Every `pos` the control API takes or returns is a Scintilla position, which
# counts bytes. A Python `str.index` counts characters, and the two agree only
# while the file is pure ASCII — so a fixture containing one non-ASCII byte
# (`fixtures/symbols.tur` has a `§`) silently shifts every offset computed the
# easy way, and the symptom is not an error but a request answered about the
# wrong spot. That reads exactly like the feature being broken.
#
# Convert explicitly when a test derives an offset from text:
#
#     chars = text.index("needle")
#     pos = len(text[:chars].encode("utf-8"))


def _resolve_bin() -> Path:
    """Locate the built trowel binary for the host platform.

    Honors $TROWEL_BIN, then falls back to the per-preset build layout used by
    the Justfile: a macOS .app bundle on Darwin, a plain ELF binary elsewhere.
    """
    override = os.environ.get("TROWEL_BIN")
    if override:
        return Path(override)
    if sys.platform == "darwin":
        for preset in ("macos-debug", "macos-release"):
            cand = REPO / "build" / preset / "trowel.app" / "Contents" / "MacOS" / "trowel"
            if cand.exists():
                return cand
        preset = "macos-debug"
        return REPO / "build" / preset / "trowel.app" / "Contents" / "MacOS" / "trowel"
    for preset in ("linux-debug", "linux-release"):
        cand = REPO / "build" / preset / "trowel"
        if cand.exists():
            return cand
    return REPO / "build" / "linux-debug" / "trowel"


BIN = _resolve_bin()
FIXTURES = Path(__file__).parent / "fixtures"
ARTIFACTS = Path(__file__).parent / "artifacts"

sys.path.insert(0, str(REPO / "tests" / "support"))
from trowel_ctl import TrowelCtl, ControlError  # noqa: E402


# ---------- process launcher ----------


@dataclass
class TrowelProc:
    proc: subprocess.Popen
    socket_path: str
    stdout_path: Path
    home: Path

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)


def _wait_for_socket(path: str, timeout: float = 20.0) -> None:
    """Wait for the control socket, which Trowel creates AFTER constructing its
    window and starting its session (src/main.cpp).

    The timeout was 5s against a measured 3-4s cold start on an idle machine --
    about a second of headroom. On a loaded one (a concurrent `just build` is
    enough) startup reaches 8s and every test in the file fails in setup with
    "control socket never appeared", which looks like a product bug and is not
    one. 20s costs nothing on a healthy run, because this returns as soon as the
    socket shows up.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(path)
                s.close()
                return
            except OSError:
                pass
        time.sleep(0.05)
    raise TimeoutError(f"control socket never appeared at {path}")


def _launch_trowel(tmp_path: Path, args: list[str] | None = None) -> TrowelProc:
    if not BIN.exists():
        pytest.skip(f"trowel binary not built at {BIN}")

    sock = f"/tmp/trowel-smoke-{uuid.uuid4().hex}.sock"
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)
    stdout_path = tmp_path / "trowel.stdout"

    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["HOME"] = str(home)
    env["XDG_CONFIG_HOME"] = str(home / ".config")
    env["XDG_CACHE_HOME"] = str(home / ".cache")
    # QSettings isolation. Setting HOME is not enough on macOS, where QSettings
    # resolves through cfprefsd (keyed by real uid, not $HOME) and would read
    # the developer's real preferences — restoring their open buffers into a
    # test that expects an empty one. This pins settings to a per-test INI file.
    env["TROWEL_SETTINGS_DIR"] = str(home / "settings")
    # Settings.json isolation. Trowel reads its hand-editable settings from
    # settings.json in TROWEL_CONFIG_DIR. Pin it to a per-test directory so
    # tests start with no settings file.
    config_dir = home / "config"
    config_dir.mkdir(parents=True, exist_ok=True)
    env["TROWEL_CONFIG_DIR"] = str(config_dir)
    # Document-state.json isolation. Trowel writes per-file state to
    # document-state.json in TROWEL_DATA_DIR. Pin it to a per-test directory
    # so tests start with no state file.
    data_dir = home / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    env["TROWEL_DATA_DIR"] = str(data_dir)
    # Optional override for the `tur` the app resolves, seeded into
    # settings.json as "turmeric.path". `ResolveTurBinary()` checks that key
    # first, so this is the supported way to run the suite against a locally
    # built toolchain — which is what lets the timeline tests be exercised
    # before TROWEL_TURMERIC_VERSION moves, instead of skipping until then.
    # Merge into any existing settings.json rather than overwriting, so
    # tests that pre-write settings keep their keys.
    tur_override = os.environ.get("TROWEL_TEST_TUR")
    if tur_override:
        settings_path = config_dir / "settings.json"
        existing = {}
        if settings_path.exists():
            try:
                existing = json.loads(settings_path.read_text())
            except (json.JSONDecodeError, ValueError):
                existing = {}
        existing["turmeric.path"] = tur_override
        settings_path.write_text(json.dumps(existing))
    # Force English so REPL banners are predictable.
    env.setdefault("LC_ALL", "en_US.UTF-8")

    # Launched with ONE retry, on a fresh socket path.
    #
    # Startup is 3-4s on an idle machine and the window is built before the
    # socket is created (src/main.cpp), so a transient stall -- a cold font
    # cache, another build on the box, cfprefsd being slow -- pushes it past any
    # fixed deadline. Raising the deadline alone was tried: at 5s whole files
    # failed in setup, and at 20s it still lost one launch in roughly three
    # hundred, mid-run, with nothing else competing.
    #
    # A retry is the right shape for that, and it keeps the failure honest: this
    # still raises when BOTH attempts fail, which is what a real startup break
    # looks like. A flake that only ever cost wall-clock is not worth reporting
    # as a product failure.
    last: Exception | None = None
    for attempt in (1, 2):
        if attempt > 1:
            sock = f"/tmp/trowel-smoke-{uuid.uuid4().hex}.sock"
        proc = subprocess.Popen(
            [str(BIN), f"--control-socket-path={sock}", *(args or [])],
            stdout=stdout_path.open("ab"),
            stderr=subprocess.STDOUT,
            env=env,
        )
        try:
            _wait_for_socket(sock)
        except Exception as exc:  # noqa: BLE001 - re-raised below
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            last = exc
            continue
        return TrowelProc(proc=proc, socket_path=sock, stdout_path=stdout_path,
                          home=home)
    assert last is not None
    raise last


# ---------- helpers used inside fixtures ----------


class Client:
    """Convenience wrapper around TrowelCtl with dump-on-failure."""

    def __init__(self, ctl: TrowelCtl, name_hint: str) -> None:
        self._ctl = ctl
        self._name_hint = name_hint

    def call(self, cmd: str, args: dict[str, Any] | None = None) -> Any:
        return self._ctl.call(cmd, args)

    # focused shorthands (§2 harness)
    def type(self, text: str) -> None: self.call("editor.type", {"text": text})
    def press(self, key: str, mods: list[str] | None = None) -> None:
        args: dict[str, Any] = {"key": key}
        if mods: args["mods"] = mods
        self.call("editor.press", args)
    def send(self, text: str) -> None: self.call("repl.send", {"text": text})
    def repl_press(self, key: str, mods: list[str] | None = None) -> None:
        args: dict[str, Any] = {"key": key}
        if mods: args["mods"] = mods
        self.call("repl.press", args)

    def wait_output(self, pattern: str, timeout_ms: int = 3000, regex: bool = False) -> dict:
        return self.call("wait.repl_output",
                         {"pattern": pattern, "regex": regex, "timeout_ms": timeout_ms})
    def wait_idle(self, quiet_ms: int = 200, timeout_ms: int = 3000) -> None:
        self.call("wait.repl_idle", {"quiet_ms": quiet_ms, "timeout_ms": timeout_ms})

    def dump(self) -> dict:
        return {
            "geometry": self.call("window.geometry"),
            "editor": self.call("editor.get_text"),
            "cursor": self.call("editor.get_cursor"),
            "repl_running": self.call("repl.is_running"),
            "screen": self.call("repl.get_screen", {"lines": 40}),
        }


# ---------- fixtures ----------


@pytest.fixture(scope="function")
def fresh_trowel(tmp_path: Path, request) -> Iterator[Client]:
    tp = _launch_trowel(tmp_path)
    ctl = TrowelCtl(tp.socket_path)
    client = Client(ctl, request.node.name)
    request.node._trowel_client = client
    request.node._trowel_proc = tp
    try:
        yield client
    finally:
        try: ctl.close()
        except Exception: pass
        tp.stop()


# Session fixture disabled by default — most tests want a fresh REPL and
# QSettings. Kept as a function-scoped alias so tests can prefer the same
# name if we introduce a session-scoped variant later.
@pytest.fixture(scope="function")
def trowel(fresh_trowel: Client) -> Client:
    return fresh_trowel


class Session:
    """Launch Trowel repeatedly against one persistent HOME + settings dir.

    Session restore only means anything across a quit and a relaunch, which
    the single-shot `trowel` fixture cannot express.
    """

    def __init__(self, tmp_path: Path) -> None:
        self._tmp_path = tmp_path
        self._live: list[tuple[TrowelProc, TrowelCtl]] = []

    @property
    def settings_ini(self) -> Path:
        """Where TROWEL_SETTINGS_DIR puts the INI (see _launch_trowel)."""
        return self._tmp_path / "home" / "settings" / "turmeric" / "Trowel.ini"

    @property
    def settings_json(self) -> Path:
        """Where TROWEL_CONFIG_DIR puts settings.json (see _launch_trowel)."""
        p = self._tmp_path / "home" / "config" / "settings.json"
        p.parent.mkdir(parents=True, exist_ok=True)
        return p

    @property
    def experiments_file(self) -> Path:
        """Where XDG_CONFIG_HOME puts turmeric/experiments.tur."""
        p = self._tmp_path / "home" / ".config" / "turmeric" / "experiments.tur"
        p.parent.mkdir(parents=True, exist_ok=True)
        return p

    @property
    def doc_state_json(self) -> Path:
        """Where TROWEL_DATA_DIR puts document-state.json."""
        return self._tmp_path / "home" / "data" / "document-state.json"

    def launch(self, args: list[str] | None = None) -> Client:
        tp = _launch_trowel(self._tmp_path, args)
        ctl = TrowelCtl(tp.socket_path)
        client = Client(ctl, "session")
        self._live.append((tp, ctl))
        return client

    def quit(self, client: Client) -> None:
        """Quit through the File menu and wait for the process to exit.

        Persistence runs in closeEvent, so a test that kills the process
        instead would never see a session written.
        """
        tp, ctl = self._live[-1]
        try:
            client.call("menu.invoke", {"path": ["File", "Quit"]})
        except Exception:
            pass
        try:
            tp.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            tp.stop()
            raise AssertionError("Trowel did not exit on File > Quit")
        try:
            ctl.close()
        except Exception:
            pass
        self._live.pop()

    def cleanup(self) -> None:
        for tp, ctl in self._live:
            try:
                ctl.close()
            except Exception:
                pass
            tp.stop()


@pytest.fixture
def trowel_session(tmp_path: Path) -> Iterator[Session]:
    session = Session(tmp_path)
    try:
        yield session
    finally:
        session.cleanup()


@pytest.fixture
def tur_binary() -> str:
    """The bundled `tur`, resolved the way Trowel resolves it.

    For tests that need the TOOLCHAIN's own answer as an oracle -- "did the
    editor produce what `tur` produces" is a much stronger assertion than "did
    the editor change something".

    Both published archive shapes are probed, as src/repl/repl_session.cpp does:
    the released .tar.gz targets shipped `tur` at the archive root through
    v0.46.0 and under `bin/` from v0.47.0 on.
    """
    root = (BIN.parent.parent / "Resources" / "turmeric" if sys.platform == "darwin"
            else BIN.parent / "turmeric")
    exe = "tur.exe" if sys.platform == "win32" else "tur"
    for cand in (root / "bin" / exe, root / exe):
        if cand.exists():
            return str(cand)
    pytest.skip(f"no bundled tur under {root}")


@pytest.fixture
def fixture_files() -> Path:
    return FIXTURES


# ---------- diagnostics on failure ----------


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    if rep.when != "call" or not rep.failed:
        return
    client: Client | None = getattr(item, "_trowel_client", None)
    proc: TrowelProc | None = getattr(item, "_trowel_proc", None)
    ARTIFACTS.mkdir(exist_ok=True)
    base = ARTIFACTS / item.name
    try:
        if client is not None:
            (base.with_suffix(".state.json")).write_text(
                json.dumps(client.dump(), indent=2, default=str))
    except Exception as e:
        (base.with_suffix(".state.err")).write_text(f"dump failed: {e!r}\n")
    if proc is not None and proc.stdout_path.exists():
        shutil.copy(proc.stdout_path, base.with_suffix(".trowel.log"))
