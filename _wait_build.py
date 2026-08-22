"""Wait for the tenvi-build build_mp.yml run to finish. Polls every 20s.
Usage: python _wait_build.py <TOKEN>"""
import json
import sys
import time
import urllib.request
import urllib.error

TOKEN = sys.argv[1]
API = "https://api.github.com"
H = {"Authorization": "Bearer " + TOKEN, "Accept": "application/vnd.github+json"}
URL = "%s/repos/ETboy-debug/tenvi-build/actions/workflows/build_mp.yml/runs?per_page=5" % API

deadline = time.time() + 720  # 12 min cap


def get(url):
    req = urllib.request.Request(url, headers=H)
    return json.loads(urllib.request.urlopen(req, timeout=60).read().decode())


while True:
    try:
        d = get(URL)
    except Exception as e:  # noqa
        print("POLL ERR", repr(e)[:120])
        d = {"workflow_runs": []}
    runs = d.get("workflow_runs", [])
    # pick the most recent run that was triggered by our dispatch
    if runs:
        r = runs[0]
        st = r.get("status")
        cn = r.get("conclusion")
        print("t=%ds run=%s status=%s conclusion=%s" % (int(time.time() - (deadline - 720)), r["id"], st, cn))
        if st == "completed":
            print("BUILD_DONE conclusion=%s" % cn)
            sys.exit(0 if cn == "success" else 3)
    else:
        print("t=%ds no runs yet" % int(time.time() - (deadline - 720)))
    if time.time() > deadline:
        print("TIMEOUT waiting for build")
        sys.exit(2)
    time.sleep(20)
