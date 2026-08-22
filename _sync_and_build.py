"""v188: gzip the two edited sources, verify round-trip, upload only those .gz,
then dispatch the cloud build. Usage: python _sync_and_build.py <TOKEN>"""
import base64
import gzip
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request

TOKEN = sys.argv[1]
SRC_REPO = "ETboy-debug/tenvi-mp"
BUILD_REPO = "ETboy-debug/tenvi-build"
API = "https://api.github.com"
H = {
    "Authorization": "Bearer " + TOKEN,
    "Accept": "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
}

FILES = [
    "AutoResponse/FakeServer.cpp",
    "AutoResponse/AutoResponse.cpp",
    "StandaloneServer/StandaloneServer.cpp",
]

# [v194] per-file marker: FakeServer carries the MP-FWD mark, AutoResponse
# (the client DLL) carries the MP-19DIAG recorder tag, StandaloneServer the
# CP-SNIFF tag. Each uploaded .gz must round-trip AND contain its marker,
# otherwise the cloud would compile a stale copy.
FILE_MARKS = {
    "AutoResponse/FakeServer.cpp": b"MP-FWD v204",
    "AutoResponse/AutoResponse.cpp": b"MP-19DIAG",
    "StandaloneServer/StandaloneServer.cpp": b"CP-SNIFF",
}


def api(method, url, body=None):
    data = json.dumps(body).encode() if body is not None else None
    hdr = dict(H)
    if data:
        hdr["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=hdr, method=method)
    try:
        return urllib.request.urlopen(req, timeout=90).read().decode()
    except urllib.error.HTTPError as e:
        return ("ERR", e.code, e.read().decode()[:300])
    except Exception as e:  # noqa: BLE001
        return ("ERR", 0, str(e)[:300])


def md5(b):
    return hashlib.md5(b).hexdigest()[:12]


# 1) gzip + round-trip verify
for rel in FILES:
    plain = open(rel, "rb").read()
    gzpath = rel + ".gz"
    with gzip.GzipFile(gzpath, "wb", compresslevel=9, mtime=0) as f:
        f.write(plain)
    back = gzip.open(gzpath, "rb").read()
    assert back == plain, "ROUNDTRIP FAIL " + rel
    print("GZ OK  %s  plain=%d md5=%s" % (rel, len(plain), md5(plain)))

# 2) upload only the .gz files
for rel in FILES:
    gzrel = rel + ".gz"
    data = open(gzrel, "rb").read()
    r = api("GET", "%s/repos/%s/contents/%s?ref=master" % (API, SRC_REPO, gzrel))
    sha = None
    if isinstance(r, str):
        try:
            sha = json.loads(r).get("sha")
        except Exception:  # noqa: BLE001
            sha = None
    body = {
        "message": "v188 " + gzrel,
        "content": base64.b64encode(data).decode(),
        "branch": "master",
    }
    if sha:
        body["sha"] = sha
    res = api("PUT", "%s/repos/%s/contents/%s" % (API, SRC_REPO, gzrel), body)
    if isinstance(res, tuple):
        print("PUT ERR", gzrel, res)
        sys.exit(1)
    print("PUT OK", gzrel, len(data), "bytes")

# 3) verify what the remote .gz now decompresses to (catch stale-upload traps)
for rel in FILES:
    gzrel = rel + ".gz"
    r = api("GET", "%s/repos/%s/contents/%s?ref=master" % (API, SRC_REPO, gzrel))
    remote = gzip.decompress(base64.b64decode(json.loads(r)["content"]))
    local = open(rel, "rb").read()
    mark = FILE_MARKS.get(rel, b"")
    ok = remote == local and (not mark or mark in remote)
    print("REMOTE %s  match=%s  marker(%s)=%s" % (
        gzrel, remote == local, mark.decode(errors="replace"), mark in remote))
    if not ok:
        print("ABORT: remote copy does not match local edit")
        sys.exit(2)

# 4) dispatch the build
res = api(
    "POST",
    "%s/repos/%s/actions/workflows/build_mp.yml/dispatches" % (API, BUILD_REPO),
    {"ref": "main"},
)
print("DISPATCH:", "OK" if not isinstance(res, tuple) else res)
