#!/usr/bin/env python3
"""Copy one file out of a Games on Demand package (an Xbox 360 hard drive install), for building from it.

    python3 dante/tools/god_xex.py <package folder> [name] <out file>      (name: default.xex)

The package folder is the one that holds the DataNNNN pieces, or any folder above it (454108CF/00007000/...). The
pieces are the disc image in 0x800-byte blocks, 0x14388 blocks per piece, with a 0x1000-byte hash table before every
0x198 blocks and one more at the start of each piece (the SVOD layout Xenia reads); the disc's file system (XDVDFS)
starts at block 0. Only the pieces the file lies in are read.
"""
import struct
import sys
from pathlib import Path

BLOQUES_POR_PIEZA = 0x14388


def situar(bloque):
    pieza, b = divmod(bloque, BLOQUES_POR_PIEZA)
    tablas0 = b // 0x198 + 1
    return pieza, b * 0x800 + tablas0 * 0x1000 + (tablas0 // 0xA1C4 + 1) * 0x1000


class Paquete:
    def __init__(self, carpeta: Path):
        piezas = sorted(carpeta.rglob("Data0000"))
        if not piezas:
            sys.exit(f"{carpeta}: no Games on Demand pieces (Data0000) found")
        self.dir = piezas[0].parent
        self.abiertos = {}

    def bloque(self, n):
        pieza, desp = situar(n)
        if pieza not in self.abiertos:
            ruta = self.dir / f"Data{pieza:04d}"
            if not ruta.is_file():
                sys.exit(f"{ruta} is missing")
            self.abiertos[pieza] = ruta.open("rb")
        f = self.abiertos[pieza]
        f.seek(desp)
        return f.read(0x800)

    def leer(self, sector, tamano):
        datos = bytearray()
        while len(datos) < tamano:
            datos += self.bloque(sector)
            sector += 1
        return bytes(datos[:tamano])

    def archivos(self, sector, tamano, prefijo=""):
        tabla = self.leer(sector, tamano)
        pendientes, vistos = [0], set()
        while pendientes:
            o = pendientes.pop()
            p = o * 4
            if o in vistos or p + 14 > len(tabla):
                continue
            vistos.add(o)
            izq, der, inicio, largo, attr, nlen = struct.unpack("<HHIIBB", tabla[p:p + 14])
            if izq == 0xFFFF:
                continue
            nombre = tabla[p + 14:p + 14 + nlen].decode("latin1")
            if izq:
                pendientes.append(izq)
            if der:
                pendientes.append(der)
            if attr & 0x10:
                if largo:
                    yield from self.archivos(inicio, largo, prefijo + nombre + "/")
            else:
                yield prefijo + nombre, inicio, largo


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    nombre = sys.argv[2] if len(sys.argv) == 4 else "default.xex"
    paquete = Paquete(Path(sys.argv[1]))
    vd = paquete.leer(32, 0x800)
    if vd[:20] != b"MICROSOFT*XBOX*MEDIA":
        sys.exit("not a Games on Demand disc image (no XDVDFS volume at block 32)")
    raiz, tamano = struct.unpack("<II", vd[20:28])
    for ruta, inicio, largo in paquete.archivos(raiz, tamano):
        if ruta.lower() == nombre.lower():
            Path(sys.argv[-1]).write_bytes(paquete.leer(inicio, largo))
            print(f"{ruta}: {largo} bytes -> {sys.argv[-1]}")
            return
    sys.exit(f"{nombre} is not in the package")


if __name__ == "__main__":
    main()
