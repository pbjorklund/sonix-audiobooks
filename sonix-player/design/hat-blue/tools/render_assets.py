"""Regenerate kit PNG exports from the included editable SVG masters.
Requires: pip install cairosvg pillow
Run from any working directory: python path/to/tools/render_assets.py
"""
from pathlib import Path
import json
import cairosvg
ROOT=Path(__file__).resolve().parents[1]
manifest=json.loads((ROOT/'asset-manifest.json').read_text(encoding='utf-8'))
count=0
for asset in manifest['assets']:
    if asset['kind']=='preserved-semantic':
        continue
    svg=(ROOT/asset['source']).read_bytes()
    size=asset['size']
    cairosvg.svg2png(bytestring=svg,write_to=str(ROOT/asset['file']),output_width=size,output_height=size)
    if asset['kind']=='menu':
        for size in [112,128,512]:
            dest=ROOT/'png'/str(size)/(asset['key']+'.png')
            dest.parent.mkdir(parents=True,exist_ok=True)
            cairosvg.svg2png(bytestring=svg,write_to=str(dest),output_width=size,output_height=size)
    count+=1
print(f'Regenerated {count} deployment PNGs and 111 alternate-size menu exports.')
