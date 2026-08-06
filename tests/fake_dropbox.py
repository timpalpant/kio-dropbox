#!/usr/bin/env python3
"""A stand-in for the Dropbox v2 API, just complete enough to drive the worker.

Backed by an in-memory tree. Speaks the subset of endpoints kio-dropbox uses,
including the error shapes (HTTP 409 plus an error_summary) that the worker maps
onto KIO errors. Started by run_tests.sh; prints its port on stdout.
"""

import json
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ACCESS_TOKEN = "fake-access-token"
REFRESH_TOKEN = "fake-refresh-token"
APP_KEY = "fake-app-key"

# path -> {"type": "folder"} or {"type": "file", "data": bytes, "modified": iso}
TREE = {}
LOCK = threading.Lock()

# Force these behaviors on the next matching request, to exercise retry paths.
FAULTS = {"rate_limit_once": False, "expire_token_once": False}


def reset_tree():
    with LOCK:
        TREE.clear()
        TREE["/documents"] = {"type": "folder"}
        TREE["/documents/notes.txt"] = {
            "type": "file",
            "data": b"hello from dropbox\n",
            "modified": "2026-01-02T03:04:05Z",
        }
        TREE["/documents/résumé — draft.pdf"] = {
            "type": "file",
            "data": b"%PDF-1.4 fake\n",
            "modified": "2026-02-03T04:05:06Z",
        }
        TREE["/photos"] = {"type": "folder"}
        for i in range(2500):  # forces list_folder pagination
            TREE["/photos/img%04d.jpg" % i] = {
                "type": "file",
                "data": b"\xff\xd8\xff" + bytes([i % 256]) * 10,
                "modified": "2026-03-04T05:06:07Z",
            }
        TREE["/big.bin"] = {"type": "file", "data": bytes(range(256)) * 4096, "modified": "2026-04-05T06:07:08Z"}


def metadata(path):
    node = TREE[path]
    name = path.rsplit("/", 1)[1]
    if node["type"] == "folder":
        return {".tag": "folder", "name": name, "path_lower": path.lower(), "path_display": path, "id": "id:" + path}
    return {
        ".tag": "file",
        "name": name,
        "path_lower": path.lower(),
        "path_display": path,
        "id": "id:" + path,
        "size": len(node["data"]),
        "client_modified": node["modified"],
        "server_modified": node["modified"],
        "rev": "0123456789abcdef",
    }


def children(folder):
    prefix = (folder + "/") if folder else "/"
    out = []
    for path in sorted(TREE):
        if path.startswith(prefix) and "/" not in path[len(prefix):]:
            out.append(path)
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass  # keep the test output readable

    # --- plumbing ---------------------------------------------------------

    def respond(self, status, body=b"", headers=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def json_ok(self, obj, headers=None):
        self.respond(200, json.dumps(obj), {**(headers or {}), "Content-Type": "application/json"})

    def api_error(self, summary, status=409):
        self.respond(status, json.dumps({"error_summary": summary, "error": {".tag": summary.split("/")[0]}}),
                     {"Content-Type": "application/json"})

    def authorized(self):
        if FAULTS["expire_token_once"]:
            FAULTS["expire_token_once"] = False
            self.respond(401, json.dumps({"error_summary": "expired_access_token/"}),
                         {"Content-Type": "application/json"})
            return False
        if self.headers.get("Authorization") != "Bearer " + ACCESS_TOKEN:
            self.respond(401, json.dumps({"error_summary": "invalid_access_token/"}),
                         {"Content-Type": "application/json"})
            return False
        if FAULTS["rate_limit_once"]:
            FAULTS["rate_limit_once"] = False
            self.respond(429, json.dumps({"error_summary": "too_many_requests/"}),
                         {"Content-Type": "application/json", "Retry-After": "1"})
            return False
        return True

    def read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def api_arg(self):
        raw = self.headers.get("Dropbox-API-Arg", "{}")
        # Dropbox requires this header to be ASCII with \u escapes; make sure the
        # worker actually sent it that way before decoding.
        assert all(ord(c) < 128 for c in raw), "Dropbox-API-Arg must be ASCII: %r" % raw
        return json.loads(raw)

    # --- routing ----------------------------------------------------------

    def do_POST(self):
        path = self.path
        body = self.read_body()

        if path == "/oauth2/token":
            return self.token(body)
        if path == "/__control/reset":
            reset_tree()
            return self.json_ok({"ok": True})
        if path.startswith("/__control/fault/"):
            FAULTS[path.rsplit("/", 1)[1]] = True
            return self.json_ok({"ok": True})

        if not self.authorized():
            return

        with LOCK:
            if path.startswith("/2/"):
                return self.rpc(path[len("/2/"):], json.loads(body or b"null"))
            if path.startswith("/content/2/"):
                return self.content(path[len("/content/2/"):], body)
        self.respond(404, "no such endpoint")

    def token(self, body):
        form = dict(pair.split("=", 1) for pair in body.decode().split("&") if "=" in pair)
        if form.get("grant_type") == "refresh_token" and form.get("refresh_token") == REFRESH_TOKEN:
            return self.json_ok({"access_token": ACCESS_TOKEN, "token_type": "bearer", "expires_in": 14400})
        if form.get("grant_type") == "authorization_code":
            if not form.get("code_verifier"):
                return self.respond(400, json.dumps({"error": "invalid_request"}))
            return self.json_ok({"access_token": ACCESS_TOKEN, "refresh_token": REFRESH_TOKEN,
                                 "token_type": "bearer", "expires_in": 14400, "account_id": "dbid:fake"})
        self.respond(400, json.dumps({"error": "invalid_grant"}))

    def rpc(self, endpoint, args):
        if endpoint == "users/get_current_account":
            return self.json_ok({"account_id": "dbid:fake", "email": "tester@example.com",
                                 "name": {"display_name": "Test User"}})

        if endpoint == "users/get_space_usage":
            return self.json_ok({"used": 1234567,
                                 "allocation": {".tag": "individual", "allocated": 2 * 1024 ** 3}})

        if endpoint == "files/list_folder":
            folder = args["path"]
            if folder and folder not in TREE:
                return self.api_error("path/not_found/")
            if folder and TREE[folder]["type"] != "folder":
                return self.api_error("path/not_folder/")
            return self.page(children(folder), 0)

        if endpoint == "files/list_folder/continue":
            folder, offset = json.loads(args["cursor"])
            return self.page(children(folder), offset)

        if endpoint == "files/get_metadata":
            path = args["path"]
            if path not in TREE:
                return self.api_error("path/not_found/")
            return self.json_ok(metadata(path))

        if endpoint == "files/create_folder_v2":
            path = args["path"]
            if path in TREE:
                return self.api_error("path/conflict/folder/")
            if path.rsplit("/", 1)[0] and path.rsplit("/", 1)[0] not in TREE:
                return self.api_error("path/conflict/not_found/")
            TREE[path] = {"type": "folder"}
            return self.json_ok({"metadata": metadata(path)})

        if endpoint == "files/delete_v2":
            path = args["path"]
            if path not in TREE:
                return self.api_error("path_lookup/not_found/")
            meta = metadata(path)
            for key in [k for k in TREE if k == path or k.startswith(path + "/")]:
                del TREE[key]  # delete_v2 is recursive
            return self.json_ok({"metadata": meta})

        if endpoint in ("files/move_v2", "files/copy_v2"):
            src, dest = args["from_path"], args["to_path"]
            if src not in TREE:
                return self.api_error("from_lookup/not_found/")
            if dest in TREE:
                return self.api_error("to/conflict/file/")
            moving = endpoint == "files/move_v2"
            for key in [k for k in TREE if k == src or k.startswith(src + "/")]:
                TREE[dest + key[len(src):]] = TREE[key] if moving else dict(TREE[key])
                if moving:
                    del TREE[key]
            return self.json_ok({"metadata": metadata(dest)})

        self.respond(404, "unknown rpc " + endpoint)

    def page(self, paths, offset):
        chunk = paths[offset:offset + 1000]
        has_more = offset + len(chunk) < len(paths)
        folder = paths[0].rsplit("/", 1)[0] if paths else ""
        return self.json_ok({
            "entries": [metadata(p) for p in chunk],
            "cursor": json.dumps([folder, offset + len(chunk)]),
            "has_more": has_more,
        })

    def content(self, endpoint, body):
        args = self.api_arg()

        if endpoint == "files/download":
            path = args["path"]
            if path not in TREE:
                return self.api_error("path/not_found/")
            if TREE[path]["type"] != "file":
                return self.api_error("path/not_file/")
            return self.respond(200, TREE[path]["data"], {
                "Content-Type": "application/octet-stream",
                "Dropbox-API-Result": json.dumps(metadata(path)),
            })

        if endpoint == "files/upload":
            return self.commit(args, body)

        if endpoint == "files/upload_session/start":
            session = "session-%d" % len(SESSIONS)
            SESSIONS[session] = bytearray(body)
            return self.json_ok({"session_id": session})

        if endpoint == "files/upload_session/append_v2":
            cursor = args["cursor"]
            buf = SESSIONS[cursor["session_id"]]
            if cursor["offset"] != len(buf):
                return self.api_error("incorrect_offset/")
            buf.extend(body)
            return self.respond(200, b"", {"Content-Type": "application/json"})

        if endpoint == "files/upload_session/finish":
            cursor = args["cursor"]
            buf = SESSIONS.pop(cursor["session_id"])
            if cursor["offset"] != len(buf):
                return self.api_error("lookup_failed/incorrect_offset/")
            buf.extend(body)
            return self.commit(args["commit"], bytes(buf))

        self.respond(404, "unknown content endpoint " + endpoint)

    def commit(self, args, data):
        path = args["path"]
        parent = path.rsplit("/", 1)[0]
        if parent and parent not in TREE:
            return self.api_error("path/conflict/not_found/")
        if path in TREE and args.get("mode") != "overwrite":
            return self.api_error("path/conflict/file/")
        TREE[path] = {"type": "file", "data": data, "modified": "2026-06-07T08:09:10Z"}
        return self.json_ok(metadata(path))


SESSIONS = {}

if __name__ == "__main__":
    reset_tree()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print(server.server_address[1], flush=True)
    server.serve_forever()
