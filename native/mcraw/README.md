# mcraw

Writer for MotionCam's `.mcraw` RAW video container (version 3) and its
lossless RAW codec (`compressionType` 7).

There is no published specification. The layout follows MotionCam's
Apache-2.0 reference decoder,
[mirsadm/motioncam-decoder](https://github.com/mirsadm/motioncam-decoder),
which is also what the host tests decode with when it is available
(`-DRAWR_MOTIONCAM_DECODER_DIR=<checkout>`).

- `McrawEncoder.h`: `encodeFrame()` / `decodeFrame()` for Bayer RAW16.
  Frames need an even width and a height that is a multiple of 4.
- `McrawContainer.h`: `ContainerWriter` for the header, frames, audio,
  gyroscope and accelerometer streams, the tail indexes, and `recover()` for
  interrupted files.

Metadata JSON is the caller's: the container and frame keys the app writes
are documented in `app/src/main/cpp/video/McrawMetadata.h`.
