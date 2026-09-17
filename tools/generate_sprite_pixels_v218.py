#!/usr/bin/env python3
"""Reproduce the original v2.18 Sun Route sprite and particle PNG pixels."""
from __future__ import annotations

import argparse
import base64
import binascii
from pathlib import Path
import struct
import zlib

ASSETS = {
    'v218_sun_route_cast_index.png': (256, 16, 'c-rlm!H&Zq3`7yzlK=nWwuhp6EiiU$x?!1-d<e-9l=uZ=Qi31|`mLXo9yE6ILlF2+kHg8|ihlm>uzo|lehBiayz-5Q)|(r-n>X@<|Nk?G-8!N5mrh)reM>v||M+r;-P(uLkJrz2e_c!CtS_n$YaX8UMcsc#{^{6S&u%pK`g}4A?Q>h%pBuhb%<@O_-^y7}AAiBW_x{oR9~bApj;r<DX`3|u-T6P>!Tq#-mcPX-4Q=j$|NZ8FKJQZ&ZsfZ#S_iDmZ+$ms{Pp~Z-(Ejnzn1?4{Dc3&++XvN=O;Z6{&n}&_T%+q{@;^-saB&2@DKjMzq}9Q**}=QZ*w*Nr8<_S{!YVE!=l$l-CyIE_@!eS@h^=(8|NR?|E|w}5!MKri00q>|4-i=&wp#qM)ChGf9~+h+h+N({};diw+sJK_5YUtEdNp){LpNDYyHpb0@h=mAJ$`OAHcf*5B|YF_+OWQiSc4&IEw$s8p-UOS$+IaeEmw}FP*ix_W15R*8G<sGa&Fk-@lTcEs}wh'),
    'v218_sun_route_cast_preview.png': (256, 16, 'c-rlmv2nvN3`IRs=E$W@=OR<M&L|lnvv>(l;F><=jtPW75FmiEY5sqQLX<v5(|Do;9u5!$L316yOQA8EAA-Pt`B8+w9{v2;;eCcg{SXw@`{lFnuzFuUzW(Lz)HaGA{6Ehcc0Ij{Vf9NKd04s^`<{02|L2puv=6PHsGsY4ek(M#`m*})=HXUf*8NB1pN>tMyS1^``Kh^D!#Zz0`?+T+TKO~imvT1J$36Iu-aniFNsr8b9hWrkv`riT==@i4KW%U2mw2UN%{}lx-ulnieab@py_WZYp85UFl@0!SevRLyexiOW|C7K!_}A9{nvXm`>3Q&PyRWsMs2}V9nEY$C8cTqG@DKj=eVEApLF;{+N8?|sV|nWDvz&L-u;{f}_qX^perI#O!oN0t&vOv<zdrb1dOxs6HviG@fBL`i{P*T;7XN$s`L&XNt^ODBWB)II|8Erjv+Dnne=Gl58~jjf?e+f8>jK_mo*&+0Z6Cn9{}2AbKltyLe~oc#-7=g1%o<7Soauf1P=5W&;^%C6kEQo@&#~pZ1epPW|Mvc#bOS2zTCD'),
    'v218_sun_route_particles.png': (32, 8, 'c-muN7(RfNf#E+4lOs<EumSbKFg~-f*+EDi*$zT>qq_l{*~oSfk|)N0#M;kJz<zcDVK9*X9}MwN@AMA<xl?7Y'),
    'v218_sun_route_particles_preview.png': (32, 8, 'c-muN7(U?ZC7=H=OpZJu5c<5~KMdnD8=D=3<dN+lWH-7Su$hf)2O)W4{70<)Ab`()Z1RJ}|AQg^>7D)o=Xfj@'),
}

def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF)

def encode_rgba_png(width: int, height: int, pixels: bytes) -> bytes:
    if len(pixels) != width * height * 4:
        raise ValueError("RGBA payload length does not match dimensions")
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)
        rows.extend(pixels[y * stride:(y + 1) * stride])
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", nargs="?", default=".")
    args = parser.parse_args()
    directory = Path(args.root) / "assets" / "sprites"
    directory.mkdir(parents=True, exist_ok=True)
    for name, (width, height, encoded) in ASSETS.items():
        pixels = zlib.decompress(base64.b85decode(encoded.encode("ascii")))
        (directory / name).write_bytes(encode_rgba_png(width, height, pixels))
    print(f"generated {len(ASSETS)} v2.18 sprite PNG assets")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
