#!/usr/bin/env python3
"""Refresh data/follows.json with the channels a Twitch account follows.

Needs a Twitch app (client id + secret) and a refresh token for the account
with the `user:read:follows` scope. Env vars:
  TWITCH_CLIENT_ID, TWITCH_CLIENT_SECRET, TWITCH_REFRESH_TOKEN
Exits 0 without touching anything when they are missing, so the workflow is
a harmless no-op until you add the secrets. Stdlib only.
"""
import json, os, sys, urllib.parse, urllib.request, datetime

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "follows.json")
cid = os.environ.get("TWITCH_CLIENT_ID")
secret = os.environ.get("TWITCH_CLIENT_SECRET")
refresh = os.environ.get("TWITCH_REFRESH_TOKEN")
if not (cid and secret and refresh):
    print("twitch secrets not set; skipping")
    sys.exit(0)

def call(url, data=None, headers=None):
    req = urllib.request.Request(url, data=data, headers=headers or {})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)

tok = call("https://id.twitch.tv/oauth2/token", urllib.parse.urlencode({
    "grant_type": "refresh_token", "refresh_token": refresh,
    "client_id": cid, "client_secret": secret}).encode())
H = {"Authorization": "Bearer " + tok["access_token"], "Client-Id": cid}

me = call("https://api.twitch.tv/helix/users", headers=H)["data"][0]
follows, cursor = [], None
while True:
    q = {"user_id": me["id"], "first": "100"}
    if cursor: q["after"] = cursor
    d = call("https://api.twitch.tv/helix/channels/followed?" + urllib.parse.urlencode(q), headers=H)
    follows += d["data"]
    cursor = d.get("pagination", {}).get("cursor")
    if not cursor: break

avatars = {}
for i in range(0, len(follows), 100):
    chunk = follows[i:i + 100]
    q = urllib.parse.urlencode([("id", f["broadcaster_id"]) for f in chunk])
    for u in call("https://api.twitch.tv/helix/users?" + q, headers=H)["data"]:
        avatars[u["id"]] = u["profile_image_url"]

streamers = [{"login": f["broadcaster_login"], "name": f["broadcaster_name"],
              "profile_image_url": avatars.get(f["broadcaster_id"], "")} for f in follows]
streamers.sort(key=lambda s: s["login"])

old = None
try: old = json.load(open(OUT)).get("streamers")
except Exception: pass
if old == streamers:
    print("no change"); sys.exit(0)
with open(OUT, "w", encoding="utf-8") as f:
    json.dump({"generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
               "streamers": streamers}, f, indent=2, ensure_ascii=False)
    f.write("\n")
print(f"wrote {len(streamers)} streamers")
