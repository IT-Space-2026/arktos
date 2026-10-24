# CEF spike (CEF01) — gate evidence + bootstrap proof

Companion doc: `.devin/CEF-SPIKE.md` (RC11 analysis). This directory holds
the executable probe; the verdict + extension-test evidence lives in the
CEF01 row of `.devin/Arora-Task-RustCore.md`. Nothing here is wired into
the top-level SUBDIRS build — it is gate evidence, not product code.

## Version pinning

CEF releases are pinned as
`cef_binary_<chromium_major>.<cef_branch>.<cef_patch>+g<cef_sha>+chromium-<full_chromium_version>_<platform>.tar.bz2`
— the Chromium major is the leading component (CEF 152.x == Chromium
152), and the `+g<sha>` pins the exact CEF commit. Fetch URLs are
`https://cef-builds.spotifycdn.com/<url-encoded name>`; the build index
is `https://cef-builds.spotifycdn.com/index.json` (machine-readable —
channel `stable`, file `type` `standard` for the full SDK, `client` for
the prebuilt cefsimple sample).

Pinned for this spike: `cef_binary_152.0.15+gb3d72fb+chromium-152.0.7977.169`
(stable channel, linux64). Distribution lives outside the repo at
`~/cef-cache/cef_binary_152.0.15+gb3d72fb+chromium-152.0.7977.169_linux64`
(~2 GB extracted — never committed).

## Bootstrap steps (reproduce)

```sh
# 1. fetch + extract the standard distribution (674 MB tarball)
curl -LO "https://cef-builds.spotifycdn.com/cef_binary_152.0.15%2Bgb3d72fb%2Bchromium-152.0.7977.169_linux64.tar.bz2"
tar -xjf cef_binary_*_linux64.tar.bz2

# 2. libcef_dll_wrapper (the C++ shim all consumers link)
#    needs cmake >= 3.19 — no system cmake on this host; a user-local
#    Kitware binary lives at ~/cef-cache/cmake/bin/cmake
cmake -S cef_binary_*_linux64 -B cef_binary_*_linux64/build \
      -DCMAKE_BUILD_TYPE=Release
cmake --build cef_binary_*_linux64/build --target libcef_dll_wrapper -j4

# 3. the Qt probe
source .devin/qt-env.sh
export CEF_ROOT=$PWD/cef_binary_*_linux64   # adjust to the real path
cd spikes/cef/qcefprobe && ./build-probe.sh && ./run-render.sh
```

`run-render.sh` proves both embed shapes and writes `cef-osr.png`,
`cef-windowed-chrome.png`, `cef-windowed-alloy.png` next to itself.

## Codec verdict (CEF01 deliverable)

Spotify-builds binaries are compiled `ffmpeg_branding=chromium` —
**open codecs only** (VP8/VP9/AV1 + Opus/Vorbis/FLAC/WAV; no H.264,
no AAC, no proprietary MPEG/H.265). QtWebEngine's own builds ship the
same open-codec set by default, so **open-codecs-only is the honest
baseline**: it matches what Arktos users already get from the Qt
builds it links. A proprietary-codec CEF build is a self-hosted
Chromium compile (~hours, ~100 GB depot_tools checkout) and belongs —
if ever — to CEF14's packaging row, not the gate. Decision recorded in
the task details: ship open codecs; do not block the port on codecs.
