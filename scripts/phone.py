#!/usr/bin/env python3
"""A stand-in phone for the phone camera E2E on macOS.

A headless Chromium-family browser with a synthetic camera (green, with a moving circle)
opens the camera page and sends its picture over WebRTC, as a phone would. On rec's own page
(https://IP:port) it accepts only the certificate key the page has when it starts; a relay's
page has a real certificate, or is plain http: on this computer. This prints
"status=TEXT" each time the page's status line changes, read through the DevTools protocol,
until it is stopped with SIGTERM or SIGINT, or its parent process ends; then it closes the
browser and removes its profile. Exits 3 when no browser is found; REC_E2E_BROWSER names
one explicitly. --find prints the browser's name, or why there is none.

Usage: phone.py URL | --find
"""
import base64
import glob
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request


def find_browser():
    if os.environ.get("REC_E2E_BROWSER"):
        return os.environ["REC_E2E_BROWSER"]
    candidates = [
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
    ]
    # Playwright's Chrome for Testing, newest first.
    candidates += sorted(glob.glob(os.path.expanduser(
        "~/Library/Caches/ms-playwright/chromium-*/chrome-mac*/*.app/Contents/MacOS/*")), reverse=True)
    return next((c for c in candidates if os.access(c, os.X_OK)), None)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def evaluate(port, expression):
    """The value of expression in the camera page, or None before it has loaded."""
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=2) as response:
        targets = json.load(response)
    page = next((t for t in targets if t.get("type") == "page" and t.get("url", "").startswith(("https:", "http:"))), None)
    if not page:
        return None
    address, path = page["webSocketDebuggerUrl"][len("ws://"):].split("/", 1)
    host, ws_port = address.rsplit(":", 1)
    with socket.create_connection((host, int(ws_port)), timeout=3) as s:
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET /{path} HTTP/1.1\r\nHost: {address}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        stream = s.makefile("rb")
        if b" 101 " not in stream.readline():
            return None
        while stream.readline() not in (b"\r\n", b""):
            pass
        payload = json.dumps({"id": 1, "method": "Runtime.evaluate",
                              "params": {"expression": expression, "returnByValue": True}}).encode()
        # A client's frames are masked (RFC 6455).
        mask = os.urandom(4)
        length = bytes([0x80 | len(payload)]) if len(payload) < 126 else bytes([0x80 | 126]) + len(payload).to_bytes(2, "big")
        s.sendall(bytes([0x81]) + length + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        while True:
            message = b""
            while True:
                head = stream.read(2)
                if len(head) < 2:
                    return None
                size = head[1] & 0x7F
                if size == 126:
                    size = int.from_bytes(stream.read(2), "big")
                elif size == 127:
                    size = int.from_bytes(stream.read(8), "big")
                message += stream.read(size)
                if head[0] & 0x80:
                    break
            reply = json.loads(message)
            if reply.get("id") == 1:
                return reply.get("result", {}).get("result", {}).get("value")


# The start of a P-256 SubjectPublicKeyInfo in a certificate; the 65-byte point follows it.
SPKI_P256 = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


def key_pin(url, seconds=15):
    """The base64 SHA-256 of the page's public key, as --ignore-certificate-errors-spki-list takes.

    A browser pinned to it refuses any other key, as a real phone does after its person
    confirmed the certificate warning once.
    """
    parsed = urllib.parse.urlsplit(url)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    deadline = time.monotonic() + seconds
    while True:
        try:
            with socket.create_connection((parsed.hostname, parsed.port), timeout=3) as raw:
                with context.wrap_socket(raw, server_hostname=parsed.hostname) as tls:
                    der = tls.getpeercert(binary_form=True)
            at = der.index(SPKI_P256)
            spki = der[at:at + len(SPKI_P256) + 65]
            return base64.b64encode(hashlib.sha256(spki).digest()).decode()
        except (OSError, ValueError):
            if time.monotonic() > deadline:
                raise
            time.sleep(0.25)


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    browser = find_browser()
    if not browser:
        print("no Chromium-family browser: install Chrome, Edge or Chromium, or set REC_E2E_BROWSER", file=sys.stderr)
        return 3
    if sys.argv[1] == "--find":
        print(os.path.basename(browser))
        return 0
    parent = os.getppid()
    # Rec's own page needs its key pinned; a relay's page has a real certificate, or none.
    own_page = re.match(r"https://[0-9.]+:", sys.argv[1])
    pin = [f"--ignore-certificate-errors-spki-list={key_pin(sys.argv[1])}"] if own_page else []
    port = free_port()
    profile = tempfile.mkdtemp(prefix="rec-phone-")
    process = subprocess.Popen(
        [browser, "--headless=new", "--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream",
         *pin, "--incognito", "--no-first-run", "--no-default-browser-check",
         "--use-mock-keychain", "--disable-sync", f"--remote-debugging-port={port}", f"--user-data-dir={profile}",
         sys.argv[1]],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def finish(*_):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, finish)
    signal.signal(signal.SIGINT, finish)
    try:
        print(f"browser={os.path.basename(browser)}", flush=True)
        seen = None
        # A self-test that failed and exited must not leave the browser running.
        while process.poll() is None and os.getppid() == parent:
            try:
                status = evaluate(port, "document.getElementById('status').textContent")
            except (OSError, ValueError):
                status = None
            if status and status != seen:
                print(f"status={status}", flush=True)
                seen = status
            time.sleep(0.25)
        if os.getppid() != parent:
            return 0
        print("the browser exited", flush=True)
        return 1
    finally:
        process.terminate()
        try:
            process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
