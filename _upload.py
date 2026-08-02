import os, base64, urllib.request, json, sys

TOKEN = sys.argv[1]
REPO = "ETboy-debug/tenvi-mp"
ROOT = os.getcwd()  # 由 Bash 先 cd 进入 build_src
API = "https://api.github.com"
H = {
    "Authorization": f"Bearer {TOKEN}",
    "Accept": "application/vnd.github+json",
    "Content-Type": "application/json",
    "X-GitHub-Api-Version": "2022-11-28",
}

def api(method, url, body=None):
    req = urllib.request.Request(url, data=json.dumps(body).encode() if body else None, headers=H, method=method)
    try:
        return urllib.request.urlopen(req, timeout=60).read().decode()
    except urllib.error.HTTPError as e:
        return ("ERR", e.code, e.read().decode()[:200])
    except Exception as e:
        return ("ERR", 0, str(e)[:200])

print("ROOT =", ROOT)
count = 0
for dp, _, fs in os.walk(ROOT):
    if ".git" in dp.split(os.sep):
        continue
    for f in fs:
        path = os.path.join(dp, f)
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except Exception as e:
            print("READ ERR", rel, e)
            continue
        b64 = base64.b64encode(data).decode()
        r = api("GET", f"{API}/repos/{REPO}/contents/{rel}?ref=master")
        sha = None
        if isinstance(r, str):
            try:
                sha = json.loads(r).get("sha")
            except Exception:
                sha = None
        body = {"message": f"add {rel}", "content": b64, "branch": "master"}
        if sha:
            body["sha"] = sha
        res = api("PUT", f"{API}/repos/{REPO}/contents/{rel}", body)
        if isinstance(res, tuple):
            print("PUT ERR", rel, res)
        else:
            count += 1
            if count % 10 == 0:
                print(f"OK {count} {rel}")
print("DONE total=", count)
