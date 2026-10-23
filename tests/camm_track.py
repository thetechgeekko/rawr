"""MovMuxer's CAMM motion track: packets, timing and recovery must round-trip, and the A/V tracks stay intact."""
from pathlib import Path
import json
import math
import struct
import subprocess
import sys
import tempfile

helper, patch, ffmpeg, ffprobe, build = sys.argv[1:]
TIMESCALE = 100_000  # MovMuxer kMotionTimescale


def run(*args):
    try:
        return subprocess.check_output([str(x) for x in args], stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as error:
        sys.stderr.buffer.write(error.stderr)
        raise


def probe(file, *args):
    return json.loads(run(ffprobe, '-v', 'error', *args, '-of', 'json', file))


def boxes(data, start, end):
    """(type, payload_start, box_end) for each box in data[start:end]."""
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack('>I4s', data[pos:pos + 8])
        header = 8
        if size == 1:
            size = struct.unpack('>Q', data[pos + 8:pos + 16])[0]
            header = 16
        elif size == 0:
            size = end - pos
        assert size >= header and pos + size <= end, (kind, size)
        yield kind.decode('latin-1'), pos + header, pos + size
        pos += size


def child(data, start, end, kind):
    found = [b for b in boxes(data, start, end) if b[0] == kind]
    assert len(found) == 1, (kind, [b[0] for b in boxes(data, start, end)])
    return found[0]


def camm_samples(path):
    """Decoded (pts_seconds, type, x, y, z) from the file's single CAMM track."""
    data = path.read_bytes()
    _, moov, moov_end = child(data, 0, len(data), 'moov')
    tracks = []
    for kind, start, end in boxes(data, moov, moov_end):
        if kind != 'trak':
            continue
        _, mdia, mdia_end = child(data, start, end, 'mdia')
        _, hdlr, _ = child(data, mdia, mdia_end, 'hdlr')
        if data[hdlr + 8:hdlr + 12] != b'meta':
            continue
        _, stbl, stbl_end = child(data, *child(data, mdia, mdia_end, 'minf')[1:], 'stbl')
        tracks.append((start, end, mdia, mdia_end, stbl, stbl_end))
    assert len(tracks) == 1, f'expected one CAMM track, found {len(tracks)}'
    trak, trak_end, mdia, mdia_end, stbl, stbl_end = tracks[0]
    _, mdhd, _ = child(data, mdia, mdia_end, 'mdhd')
    timescale = struct.unpack('>I', data[mdhd + 12:mdhd + 16])[0]
    assert timescale == TIMESCALE, timescale
    _, stsd, _ = child(data, stbl, stbl_end, 'stsd')
    assert struct.unpack('>I', data[stsd + 4:stsd + 8])[0] == 1
    assert data[stsd + 12:stsd + 16] == b'camm', data[stsd + 12:stsd + 16]
    _, stsz, _ = child(data, stbl, stbl_end, 'stsz')
    sample_size, count = struct.unpack('>II', data[stsz + 4:stsz + 12])
    assert sample_size == 16, sample_size
    _, stts, _ = child(data, stbl, stbl_end, 'stts')
    deltas = []
    for i in range(struct.unpack('>I', data[stts + 4:stts + 8])[0]):
        n, d = struct.unpack('>II', data[stts + 8 + 8 * i:stts + 16 + 8 * i])
        assert d > 0, 'zero-duration CAMM sample'
        deltas += [d] * n
    assert len(deltas) == count, (len(deltas), count)
    _, stsc, _ = child(data, stbl, stbl_end, 'stsc')
    runs = [struct.unpack('>III', data[stsc + 8 + 12 * i:stsc + 20 + 12 * i])
            for i in range(struct.unpack('>I', data[stsc + 4:stsc + 8])[0])]
    _, co64, _ = child(data, stbl, stbl_end, 'co64')
    offsets = [struct.unpack('>Q', data[co64 + 8 + 8 * i:co64 + 16 + 8 * i])[0]
               for i in range(struct.unpack('>I', data[co64 + 4:co64 + 8])[0])]
    per_chunk = []
    for chunk in range(len(offsets)):
        per_chunk.append(next(s for first, s, _ in reversed(runs) if first <= chunk + 1))
    assert sum(per_chunk) == count, (sum(per_chunk), count)
    # Empty edit = track start in movie time (ms).
    _, edts, edts_end = child(data, trak, trak_end, 'edts')
    _, elst, _ = child(data, edts, edts_end, 'elst')
    entries = struct.unpack('>I', data[elst + 4:elst + 8])[0]
    start_ms = struct.unpack('>I', data[elst + 8:elst + 12])[0] if entries == 2 else 0
    out, t = [], 0
    index = 0
    for offset, n in zip(offsets, per_chunk):
        for k in range(n):
            reserved, kind = struct.unpack('<HH', data[offset + 16 * k:offset + 16 * k + 4])
            x, y, z = struct.unpack('<3f', data[offset + 16 * k + 4:offset + 16 * k + 16])
            assert reserved == 0
            out.append((start_ms / 1000 + t / timescale, kind, x, y, z))
            t += deltas[index]
            index += 1
    return out


def expected_kept(motion):
    """MovMuxer keeps packets, in delivery order, whose PTS never goes backwards."""
    kept, last = [], -1
    for _, pts, kind, values in sorted(motion, key=lambda m: m[0]):
        if pts < 0 or pts < last:
            continue
        kept.append((pts, kind, values))
        last = pts
    return kept


with tempfile.TemporaryDirectory(prefix="camm-", dir=build) as directory:
    root = Path(directory)
    fps = 30
    source = root/'source.mp4'
    run(ffmpeg, '-y', '-v', 'error', '-f', 'lavfi', '-i', f'testsrc2=size=160x96:rate={fps}:duration=2',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=2', '-ac', '2',
        '-pix_fmt', 'yuv420p', '-c:v', 'libx265', '-x265-params', f'log-level=error:keyint={fps}:bframes=0:aud=1',
        '-c:a', 'aac', '-b:a', '128k', '-shortest', source)
    hevc, adts = root/'v.hevc', root/'a.aac'
    run(ffmpeg, '-y', '-v', 'error', '-i', source, '-map', '0:v', '-c', 'copy', '-bsf:v', 'hevc_mp4toannexb', '-f', 'hevc', hevc)
    run(ffmpeg, '-y', '-v', 'error', '-i', source, '-map', '0:a', '-c', 'copy', '-f', 'adts', adts)
    units, config = [], b''
    stream = hevc.read_bytes()
    starts, i = [], 0
    while (i := stream.find(b'\x00\x00\x01', i)) >= 0:
        starts.append(i + 3); i += 3
    for n, start in enumerate(starts):
        end = starts[n + 1] - 3 if n + 1 < len(starts) else len(stream)
        while end > start and stream[end - 1] == 0: end -= 1
        nal = stream[start:end]
        kind = (nal[0] >> 1) & 63
        if kind == 35:
            if not units or units[-1]: units.append(b'')
            continue
        if not units: units.append(b'')
        if kind in (32, 33, 34) and len(units) == 1: config += b'\x00\x00\x00\x01' + nal
        units[-1] += b'\x00\x00\x00\x01' + nal
    adts_bytes, frames, i, asc = adts.read_bytes(), [], 0, None
    while i + 7 <= len(adts_bytes):
        length = ((adts_bytes[i + 3] & 3) << 11) | (adts_bytes[i + 4] << 3) | (adts_bytes[i + 5] >> 5)
        header = 7 if adts_bytes[i + 1] & 1 else 9
        if asc is None:
            profile, rate = (adts_bytes[i + 2] >> 6) + 1, (adts_bytes[i + 2] >> 2) & 15
            channels = ((adts_bytes[i + 2] & 1) << 2) | (adts_bytes[i + 3] >> 6)
            asc = struct.pack('>H', (profile << 11) | (rate << 7) | (channels << 3))
        frames.append(adts_bytes[i + header:i + length]); i += length
    (root/'csd').write_bytes(config); (root/'asc').write_bytes(asc)

    # 200 Hz gyro and accelerometer, sharing timestamps (one IMU read), plus a late
    # out-of-order packet and a pre-origin packet the muxer must drop.
    motion = []
    for k in range(400):
        pts = 10_000 + k * 5_000
        # Delivered in 20 ms batches, as the sensor queue does. Same-PTS pairs keep list order.
        deliver = (pts // 20_000 + 1) * 0.02 + k * 1e-9
        motion.append((deliver, pts, 2, (math.sin(k / 10), math.cos(k / 7) * 0.5, k * 1e-3)))
        motion.append((deliver + 1e-10, pts, 3, (0.1 * k, -9.81, 0.25)))
        if k == 100:
            motion.append((deliver + 2e-10, pts - 20_000, 2, (99.0, 99.0, 99.0)))  # out of order: dropped
    motion.insert(0, (0.0, -5_000, 2, (7.0, 7.0, 7.0)))  # before the origin: dropped
    kept = expected_kept(motion)

    def blob(with_motion, audio=None):
        audio = frames if audio is None else audio
        samples = [(i / fps, 0, True if i % fps == 0 else False, 260_000 + i * 1_000_000 // fps, u)
                   for i, u in enumerate(units)]
        samples += [(n * 1024 / 48000, 1, True, n * 1024 * 1_000_000 // 48000, f) for n, f in enumerate(audio)]
        if with_motion:
            samples += [(deliver, 2, False, pts, struct.pack('<H3f', kind, *values))
                        for deliver, pts, kind, values in motion]
        ordered = sorted(enumerate(samples), key=lambda s: (s[1][0], s[1][1], s[0]))
        return b''.join(struct.pack('<BBqI', track, sync, pts, len(data)) + data
                        for _, (_, track, sync, pts, data) in ordered)

    def check(out, crash=False):
        got = camm_samples(out)
        want = kept[:len(got)] if crash else kept
        assert (0 < len(got) < len(kept)) if crash else len(got) == len(kept), (len(got), len(kept))
        for (t, kind, *values), (pts, wkind, wvalues) in zip(got, want):
            assert kind == wkind, (t, kind, wkind)
            assert all(math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-6) for a, b in zip(values, wvalues)), (t, values, wvalues)
            # Edit list is in ms; samples that share a tick are spread by one 10 us tick.
            assert abs(t - pts / 1e6) < 0.0012, (t, pts)
        info = probe(out, '-show_streams')
        data_streams = [s for s in info['streams'] if s['codec_type'] == 'data']
        assert [s.get('codec_tag_string') for s in data_streams] == ['camm'], data_streams
        assert {s['codec_type'] for s in info['streams']} == {'video', 'audio', 'data'}
        run(ffmpeg, '-v', 'error', '-xerror', '-i', out, '-map', '0:v', '-map', '0:a', '-f', 'null', '-')
        return got

    raw = root/'pcm16.raw'
    run(ffmpeg, '-y', '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=1000:sample_rate=48000:duration=2',
        '-ac', '2', '-f', 's16le', raw)
    pcm = raw.read_bytes()
    pcm_blocks = [pcm[i:i + 4096] for i in range(0, len(pcm), 4096)]  # 1024 stereo s16 frames
    for bits, name in ((0, 'gyro.mp4'), (16, 'gyro.mov')):
        (root/'samples').write_bytes(blob(True, pcm_blocks if bits else None))
        out = root/name
        run(helper, out, root/'csd', root/'asc', root/'samples', 160, 96, fps, 48000, 2, 90, bits)
        got = check(out)
        # The metadata patcher rewrites moov (and every trak for LOG); CAMM must survive both.
        for args in ((), ('--log',)):
            patched = root/f'patched-{len(args)}-{name}'
            patched.write_bytes(out.read_bytes())
            run(patch, patched, *args)
            assert camm_samples(patched) == got, f'patcher changed CAMM samples ({args})'
        print(name, 'ok', len(got), 'CAMM packets')

    # Interrupted recording: the motion track is rebuilt from the sample index.
    (root/'samples').write_bytes(blob(True))
    out = root/'crash.mp4'
    run(helper, out, root/'csd', root/'asc', root/'samples', 160, 96, fps, 48000, 2, 90, 0, root/'index', 700)
    got = check(out, crash=True)
    print('crash ok: recovered', len(got), 'of', len(kept), 'CAMM packets')

    # No motion records: the file must not gain a motion track.
    (root/'samples').write_bytes(blob(False))
    out = root/'plain.mp4'
    run(helper, out, root/'csd', root/'asc', root/'samples', 160, 96, fps, 48000, 2, 90, 0)
    assert not [s for s in probe(out, '-show_streams')['streams'] if s['codec_type'] == 'data']
    print('no-motion ok')
