#!/usr/bin/env python3
"""Build a self-contained local HTML index for T6 PNG and CSV artifacts.

No external dependencies and no network access. All links are relative so the
downloaded GitHub Actions artifact works directly in a local browser.
"""
import csv
import html
import os
from pathlib import Path

folder = Path(os.environ.get("DVE_T6_CAPTURE_DIR", "build-t6/t6-artifacts"))
with (folder / "metrics.csv").open(newline="", encoding="utf-8") as source:
    cases = list(csv.DictReader(source))
if not cases:
    raise SystemExit("T6 gallery requires nonempty metrics.csv")

rows = []
for case in cases:
    key = case["case"]
    safe = html.escape(key)
    panels = []
    for label, suffix in (("RGBA8 reference", "_rgba8.png"),
                          ("Encoded, decoded to RGBA8", "_encoded.png"),
                          ("Absolute difference (6×)", "_difference_x6.png")):
        filename = key + suffix
        if not (folder / filename).is_file():
            raise SystemExit(f"missing T6 diagnostic image: {filename}")
        panels.append(f'<figure><img src="{html.escape(filename, quote=True)}" '
                      f'alt="{safe} {html.escape(label)}" loading="lazy">'
                      f'<figcaption>{html.escape(label)}</figcaption></figure>')
    summary = " · ".join(
        f'{html.escape(label)}: <strong>{html.escape(case[col])}</strong>'
        for label, col in (
            ("PSNR dB", "psnr_db"), ("MAE", "mae_rgb"),
            ("p95 channel", "p95_max_channel"),
            ("Mean normal angle °", "normal_mean_deg"),
            ("Border MAE", "border_mae")))
    rows.append(f'<section id="{safe}"><h2>{safe}</h2><p>{summary}</p>'
                f'<div class="images">{"".join(panels)}</div></section>')

page = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVE T6 Texture Compression Render Comparison</title>
<style>
:root{color-scheme:dark;background:#10141c;color:#e8edf7;font:15px/1.5 system-ui}
body{max-width:1370px;margin:0 auto;padding:28px}
h1{font-size:27px;margin-bottom:8px}
h2{font-size:18px;letter-spacing:.02em;margin:0 0 6px}
p{color:#b5c3db}
header{border-bottom:1px solid #45536a;padding-bottom:14px;margin-bottom:22px}
section{padding:17px 0 24px;border-bottom:1px solid #2c3951}
.images{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:15px}
figure{margin:0;background:#1a2433;border:1px solid #3b4b62;border-radius:9px;overflow:hidden}
img{display:block;width:100%;height:auto;image-rendering:auto}
figcaption{font-size:13px;color:#c1cee3;padding:9px 12px}
a{color:#92c6ff}
@media(max-width:720px){.images{grid-template-columns:1fr}}
</style></head><body>
<header><h1>DVE T6 — Compressed Texture Image Quality</h1>
<p>CPU reference polygon renderer • authentic DVE material-bake output •
native BC5/BC7 blocks decoded to RGBA8 before CPU rendering.
This is <strong>not</strong> a Vulkan GPU sampling test.
Difference images are amplified 6× to reveal small artifacts.</p>
<p><a href="metrics.csv">Download complete metrics CSV</a></p></header>
""" + "\n".join(rows) + "</body></html>"

(folder / "index.html").write_text(page, encoding="utf-8")
print(f"T6 HTML image review: {len(cases)} cases indexed in {folder / 'index.html'}")
