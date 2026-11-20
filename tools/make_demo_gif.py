"""Turns tools/demo's raw frames into demo.gif: the game's own 256-colour palette, 2x nearest-neighbour."""
import sys
from PIL import Image

frames_path, palette_path, out_path = sys.argv[1:4]
W = H = 128
SCALE = 2
palette = open(palette_path, 'rb').read()
data = open(frames_path, 'rb').read()
assert len(palette) == 768 and data and len(data) % (W * H) == 0, 'bad demo output'

frames = []
for i in range(0, len(data), W * H):
    img = Image.frombytes('P', (W, H), data[i:i + W * H])
    img.putpalette(palette)
    frames.append(img.resize((W * SCALE, H * SCALE), Image.NEAREST))

frames[0].save(out_path, save_all=True, append_images=frames[1:], duration=50, loop=0, optimize=False)
print(f'{out_path}: {len(frames)} frames')
