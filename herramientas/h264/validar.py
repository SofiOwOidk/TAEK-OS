#!/usr/bin/env python3
"""Comparación exacta de YUV contra FFmpeg, sin almacenar el video expandido.

FFmpeg actúa exclusivamente como oráculo en el host. El ejecutable examinado
enlaza los mismos fuentes del decodificador que se compilan para el núcleo.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def leer(pipe, cantidad):
    partes = bytearray()
    while len(partes) < cantidad:
        trozo = pipe.read(cantidad - len(partes))
        if not trozo:
            break
        partes.extend(trozo)
    return partes


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("video")
    p.add_argument("--decoder", default="build/h264-pruebas/decodificar")
    p.add_argument("--fallos", default="build/h264-pruebas")
    args = p.parse_args()
    info = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
        "stream=width,height,pix_fmt", "-of", "json", args.video]))["streams"][0]
    w, h = info["width"], info["height"]
    if info["pix_fmt"] != "yuv420p":
        raise SystemExit("Se requiere una fuente YUV420 de ocho bits.")
    tam = w * h * 3 // 2
    ref = subprocess.Popen([
        "ffmpeg", "-v", "error", "-ignore_editlist", "1", "-i", args.video,
        "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "yuv420p",
        "-f", "rawvideo", "pipe:1"], stdout=subprocess.PIPE)
    taek = subprocess.Popen([args.decoder, args.video, "/dev/stdout"], stdout=subprocess.PIPE)
    n = 0
    try:
        while True:
            a, b = leer(taek.stdout, tam), leer(ref.stdout, tam)
            if not a and not b:
                break
            if len(a) != tam or len(b) != tam:
                print(f"Longitud distinta en fotograma {n}: TAEK={len(a)}, referencia={len(b)}")
                return 1
            if a != b:
                diferencias = sum(x != y for x, y in zip(a, b))
                primero = next(i for i, (x, y) in enumerate(zip(a, b)) if x != y)
                destino = Path(args.fallos)
                destino.mkdir(parents=True, exist_ok=True)
                (destino / "diferencia_taek.yuv").write_bytes(a)
                (destino / "diferencia_ref.yuv").write_bytes(b)
                print(f"FALLO fotograma {n} (desde cero): {diferencias} muestras distintas; "
                      f"primera={primero}, TAEK={a[primero]}, referencia={b[primero]}", flush=True)
                return 1
            n += 1
        rt, rr = taek.wait(), ref.wait()
        print(f"{n} fotogramas idénticos byte por byte; procesos TAEK={rt}, FFmpeg={rr}", flush=True)
        return int(rt != 0 or rr != 0 or n == 0)
    finally:
        for proc in (taek, ref):
            proc.stdout.close()
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    sys.exit(main())
