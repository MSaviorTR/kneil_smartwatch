"""
Generate the home-therapy voice clips offline with the Windows speech engine (System.Speech / SAPI).
   make_voice_clips 1.0, 24 Nov 2026

Input:  tools/voice_clips.csv   (id,text; {n} and {m} expand to the rep number and reps per set)
Output: main/therapy_voice.bin  (raw PCM: 16 kHz, mono, 16-bit signed little-endian, concatenated)
        main/therapy_voice.h    (clip enum, offsets and lengths for the C code)
        cache/voice_clips/*.wav (the same clips, for listening; not needed by the firmware)

The firmware embeds therapy_voice.bin with CMake EMBED_FILES. Re-run this script after changing the wording
or the rep count in therapy_config.h. Nothing is downloaded and no package is installed.
"""
import base64
import csv
import os
import re
import struct
import subprocess
import sys
import wave

VERSION = 'make_voice_clips 1.0'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = os.path.join(ROOT, 'tools', 'voice_clips.csv')
BIN = os.path.join(ROOT, 'main', 'therapy_voice.bin')
HDR = os.path.join(ROOT, 'main', 'therapy_voice.h')
DATA = os.path.join(ROOT, 'main', 'therapy_voice_data.h')
CACHE = os.path.join(ROOT, 'cache', 'voice_clips')
LOG = os.path.join(CACHE, 'make_voice_clips_log.txt')
FS = 16000
MAX_TEXT = 20          # rep numbers to generate
REPS = 10


def log(msg):
    os.makedirs(CACHE, exist_ok=True)
    line = msg
    print(line, flush=True)
    with open(LOG, 'a', encoding='utf-8') as f:
        f.write(line + '\n')


def read_reps():
    try:
        txt = open(os.path.join(ROOT, 'main', 'therapy_config.h'), encoding='utf-8').read()
        m = re.search(r'#define\s+THERAPY_REPS\s+(\d+)', txt)
        if m:
            return int(m.group(1))
    except OSError:
        pass
    return 10


def say(text, path):
    """Synthesize one text to a 16 kHz mono 16-bit WAV with the Windows SAPI voice."""
    b64 = base64.b64encode(text.encode('utf-8')).decode('ascii')
    ps = (
        "Add-Type -AssemblyName System.Speech; "
        "$t=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s')); "
        "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        "$f=New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,"
        "[System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,"
        "[System.Speech.AudioFormat.AudioChannel]::Mono); "
        "$s.SetOutputToWaveFile('%s',$f); $s.Speak($t); $s.SetOutputToNull(); $s.Dispose();"
    ) % (b64, path.replace("'", "''"))
    r = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(path) or os.path.getsize(path) < 100:
        raise RuntimeError(f'SAPI failed for {text!r}: {r.stderr.strip() or r.stdout.strip()}')


def beep(path):
    """1 kHz, 0.18 s, with short fades, written as a WAV."""
    n = int(0.18 * FS)
    data = bytearray()
    for i in range(n):
        env = min(1.0, i / (0.01 * FS), (n - 1 - i) / (0.01 * FS))
        data += struct.pack('<h', int(12000 * env * __import__('math').sin(2 * 3.14159265358979 * 1000 * i / FS)))
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes(bytes(data))


def pcm(path):
    with wave.open(path, 'rb') as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != FS:
            raise RuntimeError(f'{path}: {w.getnchannels()} ch, {w.getsampwidth()*8} bit, {w.getframerate()} Hz')
        data = w.readframes(w.getnframes())
    s = struct.unpack('<%dh' % (len(data) // 2), data)
    # trim silence (30 ms pad); keep at least 0.1 s
    lim = 250
    first = next((i for i, v in enumerate(s) if abs(v) > lim), 0)
    last = next((len(s) - 1 - i for i, v in enumerate(reversed(s)) if abs(v) > lim), len(s) - 1)
    pad = int(0.03 * FS)
    a, b = max(0, first - pad), min(len(s), last + pad + 1)
    if b - a < int(0.1 * FS):
        a, b = 0, len(s)
    return struct.pack('<%dh' % (b - a), *s[a:b])


def cname(cid):
    return 'THERAPY_CLIP_' + re.sub(r'[^A-Za-z0-9]+', '_', cid).upper()


def main():
    global REPS
    os.makedirs(CACHE, exist_ok=True)
    if os.path.exists(LOG):
        os.remove(LOG)
    log(f'{VERSION} | {__import__("datetime").datetime.now().isoformat(timespec="seconds")}')
    REPS = read_reps()

    rows = []
    with open(CSV, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            rows.append((r['id'].strip(), r['text'].strip()))

    clips = []
    for cid, text in rows:
        if '{n}' in text or '{m}' in text:
            for n in range(1, MAX_TEXT + 1):
                clips.append((f'{cid}_{n:02d}', text.replace('{n}', str(n)).replace('{m}', str(REPS))))
        else:
            clips.append((cid, text))

    blob = bytearray()
    index = []
    for cid, text in clips:
        wav = os.path.join(CACHE, cid + '.wav')
        if text == '[beep]':
            beep(wav)
        else:
            say(text, wav)
        p = pcm(wav)
        index.append((cid, len(blob), len(p)))
        blob += p
        log(f'{cid:26s} {len(p)//2:6d} samples  {len(p)/2/FS:5.2f} s  {text}')

    with open(BIN, 'wb') as f:
        f.write(blob)

    lines = ['/* Generated by tools/make_voice_clips.py - do not edit. */',
             '#pragma once', '', '#include <stdint.h>', '',
             f'#define THERAPY_VOICE_SAMPLE_RATE {FS}',
             f'#define THERAPY_VOICE_REPS {REPS}',
             f'#define THERAPY_VOICE_COUNT {len(index)}', '',
             'typedef enum', '{']
    for i, (cid, _, _) in enumerate(index):
        lines.append(f'    {cname(cid)} = {i},')
    lines += ['} therapy_clip_t;', '']
    with open(HDR, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))

    data = ['/* Generated by tools/make_voice_clips.py - do not edit. */',
            '#pragma once', '', '#include <stdint.h>', '',
            '#include "therapy_voice.h"', '',
            'static const uint32_t therapy_voice_offset[THERAPY_VOICE_COUNT] = {']
    data.append('    ' + ', '.join(str(o) for _, o, _ in index))
    data += ['};', '', 'static const uint32_t therapy_voice_len[THERAPY_VOICE_COUNT] = {']
    data.append('    ' + ', '.join(str(n) for _, _, n in index))
    data += ['};', '']
    with open(DATA, 'w', encoding='utf-8') as f:
        f.write('\n'.join(data))

    log(f'clips {len(index)}, PCM {len(blob)} bytes ({len(blob)/1024:.1f} KiB), '
        f'{len(blob)/2/FS:.1f} s total')
    log(f'saved {BIN}, {HDR} and {DATA}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
