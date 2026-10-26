#!/usr/bin/env python3
"""Assemble the Vulkan surface-test PPMs (Pillow and DejaVu Sans required)."""
from pathlib import Path
import argparse
from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('gallery', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--font', type=Path, default=Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'))
    args = parser.parse_args()
    rows = [('wood.raw', 'Raw wood'), ('wood.sanded', 'Sanded wood'),
            ('wood.varnished', 'Varnished wood'), ('leather.smooth', 'Smooth leather'),
            ('leather.pebbled', 'Pebbled leather'), ('paper.kraft', 'Opaque kraft'),
            ('paper.cardboard', 'Cardboard'), ('mineral.chalk', 'Chalk'),
            ('mineral.concrete', 'Concrete'), ('mineral.ceramic-glazed', 'Glazed ceramic'),
            ('paint.lacquer', 'Lacquer')]
    columns = [('beauty', 'Flat patch'), ('sphere', 'Sphere'), ('rounded', 'Rounded box'),
               ('grazing', 'Grazing patch'), ('normal', 'Normal diagnostic')]
    font = ImageFont.truetype(str(args.font), 19)
    small = ImageFont.truetype(str(args.font), 14)
    canvas = Image.new('RGB', (980, 130 + len(rows) * 142), '#0b0d12')
    draw = ImageDraw.Draw(canvas)
    draw.text((24, 14), 'DVE · Materials Batch 3 · Vulkan polygon pass', font=font, fill='#e2e9f2')
    draw.text((24, 44), '48 × 48 fixtures · fixed studio IBL ×2 · exposure 0 · display sRGB', font=small, fill='#a7b7cf')
    draw.text((24, 65), 'Color/roughness/normal parity is tested separately; this gallery is a visual smoke test.', font=small, fill='#a7b7cf')
    for column, (_, label) in enumerate(columns):
        draw.text((220 + column * 150, 99), label, font=small, fill='#a7b7cf')
    def encode(value):
        linear = value / 255
        return round(255 * (12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055))
    display_lut = [encode(value) for value in range(256)]
    for row, (preset, label) in enumerate(rows):
        y = 126 + row * 142
        draw.text((24, y + 47), label, font=font, fill='#e2e9f2')
        for column, (suffix, _) in enumerate(columns):
            with Image.open(args.gallery / f'dve.{preset}.{suffix}.ppm') as source:
                image = source.convert('RGB')
                if suffix != 'normal':
                    image = image.point(display_lut * 3)
                canvas.paste(image.resize((128, 128), Image.Resampling.NEAREST), (220 + column * 150, y))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(args.output)
    print(args.output)


if __name__ == '__main__':
    main()
