# starlet::thp

THP movies: the video decoder, the metadata parsers and the ADPCM audio decoder, on movie bytes as
read from the disc (big-endian). Moved from the Super Mario Galaxy port's platform layer.

- `VideoDecoder::decode`: one frame of baseline JPEG with 4:2:0 sampling into the GX I8 Y, U and V
  tiles THPDraw samples (8x4 texels, row-major tile order). As in the SDK's THPVideoDecode, the
  entropy-coded data has no 0xFF00 byte stuffing and no restart markers: a restart interval only
  realigns the bit stream to a byte and resets the DC predictions. Huffman decoding follows T.81
  F.2.2.3 with a 9-bit lookup; the inverse DCT is the AAN float algorithm, within one sample step of
  FFmpeg's THP decoder. Blocks go into decoder scratch and reach the planes only once the frame has
  decoded completely; packet, dimension and destination bounds are checked. `frame_size` reads a
  frame's dimensions from its start-of-frame segment.
- `read_header`, `read_components`, `read_video_info`, `read_audio_info`, `read_frame_sizes`: the
  movie's metadata in the SDK's layouts (`include/starlet/sdk/thp.h`), checked against the file's
  size, the decoders' limits and each frame's extent.
- `decode_audio`: one record (a 0x50-byte header with the right channel's offset, samples per
  channel, both channels' predictor coefficients and history, then 8-byte ADPCM frames) with the
  SDK's arithmetic: 64-bit, rounded at bit 16, clamped to 32 bits. Output is right then left,
  interleaved or planar; mono data fills both.

Malformed input throws `starlet::thp::Error`. A port with its own player links `starlet::thp`;
`starlet::thp_sdk` adds the SDK's `THPInit`, `THPVideoDecode` and `THPAudioDecode` for ports that
keep the SDK's player. `THPVideoDecode` gets no frame length, as in the SDK: the frame ends with its
scan, bounded at 4 MiB.

Tests: `tests/thp` (bounds, every Huffman path, zero runs, restarts, partial MCU rows and malformed
frames against a reference IDCT; metadata bounds; audio ordering and rounding). Super Mario Galaxy's
tests also compare the audio decoder with the decompiled SDK's on random records.
