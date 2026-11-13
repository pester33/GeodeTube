"""Builds data/feed.json for GeodeTube.

- "latest": newest videos from the channels in channels.txt (YouTube's public RSS feeds, no API key needed)
- "gdgl":   record videos from the GD Gaming List (List-Mod backup data)
"""
import json, os, re, sys, urllib.request, xml.etree.ElementTree as ET
from datetime import datetime, timezone

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) GeodeTubeFeed/1.0",
      "Accept-Language": "en-US,en;q=0.8"}
NS = {"a": "http://www.w3.org/2005/Atom", "yt": "http://www.youtube.com/xml/schemas/2015",
      "media": "http://search.yahoo.com/mrss/"}
GDGL_DATA = "https://raw.githubusercontent.com/pester33/List-Mod/main/data/levels.json"


def get(url):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read().decode("utf-8", "replace")


def channel_id(handle):
    if handle.startswith("UC"):
        return handle
    page = get(f"https://www.youtube.com/{handle}")
    for pat in (r'<link rel="canonical" href="https://www\.youtube\.com/channel/(UC[\w-]{22})"',
                r'"externalId":"(UC[\w-]{22})"', r'"channelId":"(UC[\w-]{22})"'):
        m = re.search(pat, page)
        if m:
            return m.group(1)
    return None


def channel_videos(cid):
    root = ET.fromstring(get(f"https://www.youtube.com/feeds/videos.xml?channel_id={cid}"))
    out = []
    for e in root.findall("a:entry", NS):
        vid = e.findtext("yt:videoId", default="", namespaces=NS)
        if not vid:
            continue
        stats = e.find("media:group/media:community/media:statistics", NS)
        link = e.find("a:link", NS)
        href = link.get("href", "") if link is not None else ""
        out.append({
            "id": vid,
            "title": e.findtext("a:title", default="", namespaces=NS),
            "channel": e.findtext("a:author/a:name", default="", namespaces=NS),
            "published": e.findtext("a:published", default="", namespaces=NS),
            "views": int(stats.get("views", 0)) if stats is not None else 0,
            "short": "/shorts/" in href,
            "category": "latest",
        })
    return out


def youtube_id(url):
    m = re.search(r"(?:v=|youtu\.be/|/shorts/|/embed/|/live/)([\w-]{11})", url or "")
    return m.group(1) if m else None


def gdgl_videos():
    levels = json.loads(get(GDGL_DATA))
    out = []
    for lvl in levels:
        vid = youtube_id(lvl.get("videoUrl", ""))
        if not vid:
            continue
        section = "" if lvl.get("bucket") == "main" else f"{str(lvl.get('bucket', '')).capitalize()} "
        byline = lvl.get("byline") or ""
        verifier = byline.split("verified by ")[-1] if "verified by " in byline else lvl.get("creator", "")
        out.append({
            "id": vid,
            "title": f"{section}#{lvl.get('position')} {lvl.get('name')}",
            "channel": verifier,
            "published": "",
            "views": 0,
            "short": False,
            "category": "gdgl",
            "rank": lvl.get("position"),
        })
    return out


def main():
    handles = [l.strip() for l in open("channels.txt", encoding="utf-8")
               if l.strip() and not l.strip().startswith("#")]
    latest, channels = [], []
    for h in handles:
        try:
            cid = channel_id(h)
            if not cid:
                print(f"::warning::{h}: channel not found")
                continue
            vids = channel_videos(cid)
            latest += vids
            channels.append({"handle": h, "id": cid, "name": vids[0]["channel"] if vids else h})
            print(f"::notice::{h} -> {cid}: {len(vids)} videos")
        except Exception as ex:
            print(f"::warning::{h}: {ex}")

    seen, uniq = set(), []
    for v in sorted(latest, key=lambda v: v["published"], reverse=True):
        if v["id"] not in seen:
            seen.add(v["id"])
            uniq.append(v)
    latest = uniq[:80]

    try:
        gdgl = gdgl_videos()
        print(f"+ GDGL: {len(gdgl)} record videos")
    except Exception as ex:
        print(f"::warning::GDGL: {ex}")
        gdgl = []

    if not latest and not gdgl:
        print("Nothing loaded, keeping the old feed")
        return
    feed = {
        "updated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "channels": channels,
        "videos": latest + gdgl,
    }
    os.makedirs("data", exist_ok=True)
    with open("data/feed.json", "w", encoding="utf-8") as f:
        json.dump(feed, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print(f"::notice::Saved {len(latest)} latest + {len(gdgl)} GDGL videos")


if __name__ == "__main__":
    main()
