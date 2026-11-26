"""Regenerate synthetic media fixtures; FFmpeg is needed only to regenerate these files."""
from pathlib import Path
import subprocess
root = Path(__file__).parent

def run(*args):
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', *args], check=True)
for name, codec, extra in [
    ('red.mp4', 'libx264', []),
    ('red-hevc.mp4', 'libx265', ['-x265-params', 'pools=none:frame-threads=1:log-level=error']),
    ('red-vp9.webm', 'libvpx-vp9', []),
    ('red-av1.mkv', 'libaom-av1', ['-cpu-used', '8']),
]:
    run('-f','lavfi','-i','color=c=red:s=96x48:r=5:d=0.4', '-an', '-c:v',codec,'-threads','1',*extra,str(root/name))
run('-display_rotation:v:0','90','-i',str(root/'red.mp4'),'-c','copy',str(root/'rotated.mp4'))
run('-f','lavfi','-i','color=c=black:s=96x48:r=5:d=2', '-c:v','libx264','-threads','1',str(root/'black.mp4'))
run('-f','lavfi','-i','color=c=black:s=96x48:r=5:d=1', '-f','lavfi','-i','color=c=red:s=96x48:r=5:d=3',
    '-filter_complex','[0:v][1:v]concat=n=2:v=1:a=0', '-c:v','libx264','-threads','1',str(root/'fade.mp4'))
run('-f', 'lavfi', '-i', 'color=c=blue:s=96x48:r=10:d=0.5',
    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=0.5',
    '-c:v', 'libx264', '-threads', '1', '-c:a', 'aac', '-ac', '2',
    '-metadata', 'title=Metadata sample', '-metadata', 'artist=Rummage tests',
    str(root / 'metadata.mp4'))

# MP4 edit list: video starts at zero; AAC begins two seconds later. HD frames expose
# accidental read-ahead beyond the decoder's bounded video queue.
run('-f', 'lavfi', '-i', 'color=red:size=1280x720:rate=30',
    '-itsoffset', '2', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
    '-t', '5', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
    str(root / 'delayed-audio.mp4'))

# Audio-only metadata fixture, independent of FFmpeg's encoder defaults.
import wave
with wave.open(str(root / 'silence.wav'), 'wb') as out:
    out.setnchannels(1)
    out.setsampwidth(2)
    out.setframerate(8000)
    out.writeframes(b'\0' * 16000)
