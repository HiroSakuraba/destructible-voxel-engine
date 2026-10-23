from PIL import Image,ImageDraw,ImageFont
from pathlib import Path
import sys
if len(sys.argv)!=3:
    raise SystemExit('usage: material_surface_contact_sheet.py gallery-directory output.png')
src=Path(sys.argv[1])
output=Path(sys.argv[2])
rows=[('wood.raw','Raw wood'),('wood.sanded','Sanded wood'),('wood.varnished','Varnished wood'),('leather.smooth','Smooth leather'),('leather.pebbled','Pebbled leather'),('paper.kraft','Opaque kraft'),('paper.cardboard','Cardboard'),('mineral.chalk','Chalk'),('mineral.concrete','Concrete'),('mineral.ceramic-glazed','Glazed ceramic'),('paint.lacquer','Lacquer'),('glass.amber','Amber glass'),('glass.smoked','Smoked glass'),('liquid.water','Clear water')]
font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',20)
small=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',15)
canvas=Image.new('RGB',(860,94+len(rows)*144),(11,13,18));d=ImageDraw.Draw(canvas)
d.text((24,12),'DVE · Materials Batch 2 · CPU reference studio',font=font,fill='#e2e9f2')
for col,label in enumerate(['Sphere','Flat patch','Rounded box','Patch: grain +90°']):d.text((240+col*152,52),label,font=small,fill='#a7b7cf')
for row,(preset,label) in enumerate(rows):
 y=86+row*144;d.text((24,y+42),label,font=font,fill='#e2e9f2')
 for col in range(4):
  stem='dve.'+preset+('' if col==0 else '-'+str(col))
  im=Image.open(src/(stem+'.bmp')).resize((128,128),Image.Resampling.NEAREST)
  canvas.paste(im,(240+col*152,y))
output.parent.mkdir(parents=True,exist_ok=True);canvas.save(output)
print(output)
