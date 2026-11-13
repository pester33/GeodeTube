# GeodeTube (Geode mod)

Scroll the newest GD YouTube videos and GD Gaming List records inside Geometry Dash.

## Download
- **GD 2.2091** (Geode 6): https://github.com/pester33/GeodeTube/releases/download/latest-build-2.2091/pester.geodetube.geode
- **GD 2.2081** (Geode 5.11.2): https://github.com/pester33/GeodeTube/releases/download/latest-build/pester.geodetube.geode

Put the `.geode` file in `Geometry Dash/geode/mods` and restart GD. The mod updates itself after that.

## How the feed works
- `channels.txt` lists the YouTube channels (by @handle). Edit it to add or remove channels.
- The **Update video feed** GitHub Action runs every 2 hours. It reads each channel's public YouTube feed
  (no API key needed) plus the GD Gaming List record videos, and saves everything to `data/feed.json`.
- The mod downloads `data/feed.json`. Pressing **Watch** opens the video on YouTube.
