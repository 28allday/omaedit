# Decoder qualification sample

`qualification.mp4` is generated locally from FFmpeg's `testsrc2` pattern;
it contains no imported footage or third-party artwork.

```
ffmpeg -v error -f lavfi -i testsrc2=size=1280x720:rate=25 -frames:v 16 \
  -c:v libx264 -threads 2 -preset fast -crf 22 -g 8 -bf 2 -pix_fmt yuv420p \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 qualification.mp4
```

Startup checks compare canonical YUV samples, including planar/NV12 differences.
Three fresh opens and 16 decoded/downloaded frames per candidate include decoder
setup and transfers. Auto requires every hardware batch to beat every software
batch by at least 15%. This small workload is not a benchmark of full playback,
all codecs, resolutions, colour transforms, composition, or export.
