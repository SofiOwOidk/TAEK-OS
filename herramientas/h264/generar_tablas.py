"""Transcribe datos numéricos normativos, no código de bibliotecas.

Fuente: ITU-T H.264 (08/2024), tablas 9-12..9-24, 9-44 y 9-45.
Entrada: texto extraído con pypdf (extraction_mode='layout') de la norma.
Las tablas iniciales se transcriben explícitamente porque el PDF parte
algunos números en dos líneas. Las tablas por filas se validan completas.
"""
from pathlib import Path
import re
import sys


def generar(texto):
    texto = texto.replace('\u2212', '-')
    contextos = [[[0, 0] for _ in range(460)] for _ in range(4)]

    def serie(ids, modos, m, n):
        assert len(ids) == len(m) == len(n)
        for modo in modos:
            for i, a, b in zip(ids, m, n):
                contextos[modo][i] = [a, b]

    serie(list(range(11)), range(4),
          [20, 2, 3, 20, 2, 3, -28, -23, -6, -1, 7],
          [-15, 54, 74, -15, 54, 74, 127, 104, 53, 54, 51])
    for modo, m, n in [
        (1, [23,23,21,1,0,-37,5,-13,-11,1,12,-4,17], [33,2,0,9,49,118,57,78,65,62,49,73,50]),
        (2, [22,34,16,-2,4,-29,2,-6,-13,5,9,-3,10], [25,0,0,9,41,118,65,71,79,52,50,70,54]),
        (3, [29,25,14,-10,-3,-27,26,-4,-24,5,6,-17,14], [16,0,0,51,62,99,16,85,102,57,57,73,57]),
    ]: serie(list(range(11,24)), [modo], m, n)
    for modo, m, n in [
        (1, [18,9,29,26,16,9,-46,-20,1,-13,-11,1,-6,-17,-6,9], [64,43,0,67,90,104,127,104,67,78,65,62,86,95,61,45]),
        (2, [26,19,40,57,41,26,-45,-15,-4,-6,-13,5,6,-13,0,8], [34,22,0,2,36,69,127,101,76,71,79,52,69,90,52,43]),
        (3, [20,20,29,54,37,12,-32,-22,-2,-4,-24,5,-6,-14,-6,4], [40,10,0,0,42,97,127,117,74,85,102,57,93,88,44,55]),
    ]: serie(list(range(24,40)), [modo], m, n)
    for modo, m, n in [
        (1, [-3,-6,-11,6,7,-5,2,0,-3,-10,5,4,-3,0], [69,81,96,55,67,86,88,58,76,94,54,69,81,88]),
        (2, [-2,-5,-10,2,2,-3,-3,1,-3,-6,0,-3,-7,-5], [69,82,96,59,75,87,100,56,74,85,59,81,86,95]),
        (3, [-11,-15,-21,19,20,4,6,1,-5,-13,5,6,-3,-1], [89,103,116,57,58,84,96,63,85,106,63,75,90,101]),
    ]: serie(list(range(40,54)), [modo], m, n)
    serie([399,400,401], [0], [31,31,25], [21,31,50])
    for modo, m, n in [
        (1, [-7,-5,-4,-5,-7,1,12,11,14], [67,74,74,80,72,58,40,51,59]),
        (2, [-1,-1,1,-2,-5,0,25,21,21], [66,77,70,86,72,61,32,49,54]),
        (3, [3,-4,-2,-12,-7,1,21,19,17], [55,79,75,97,50,60,33,50,61]),
    ]: serie([54,55,56,57,58,59,399,400,401], [modo], m, n)
    serie(list(range(60,70)), range(4), [0,0,0,0,-9,4,0,-7,13,3], [41,63,63,63,83,86,97,72,41,62])
    inicio = texto.rindex('Table 9-18 – Values')
    fin = texto.index('Table 9-25 – Values', inicio)
    vistos = set()
    for linea in texto[inicio:fin].splitlines():
        if not re.fullmatch(r'[\s\d-]+', linea) or not linea.strip():
            continue
        nums = [int(x) for x in linea.split()]
        if len(nums) not in (9,18):
            continue
        for j in range(0, len(nums), 9):
            ctx = nums[j]
            assert 70 <= ctx < 460 and ctx not in vistos, (ctx, linea)
            vistos.add(ctx)
            for modo in range(4):
                contextos[modo][ctx] = nums[j+1+modo*2:j+3+modo*2]
    esperados = set(range(70,276)) | set(range(277,399)) | set(range(402,460))
    assert vistos == esperados, ('Contextos ausentes', sorted(esperados-vistos))
    rango = [None]*64
    a = texto.rindex('Table 9-44 – Specification')
    b = texto.index('Table 9-45 – State transition', a)
    for linea in texto[a:b].splitlines():
        if not re.fullmatch(r'[\s\d]+', linea) or not linea.strip(): continue
        nums = [int(x) for x in linea.split()]
        if len(nums) == 10:
            for j in (0,5):
                assert rango[nums[j]] is None
                rango[nums[j]] = nums[j+1:j+5]
    assert all(v is not None for v in rango)
    trans = []
    for linea in texto[b:b+7000].splitlines():
        if linea.strip().startswith('transIdxLPS'):
            trans.extend(map(int, linea.split()[1:]))
    assert len(trans) == 64
    out = ['/* Datos normativos ITU-T H.264 (08/2024), tablas 9-12..24, 9-44/45.',
           ' * Generados por herramientas/h264/generar_tablas.py. Sin código de bibliotecas. */',
           '#ifndef TAEK_H264_TABLAS_CABAC_H', '#define TAEK_H264_TABLAS_CABAC_H',
           'static const int8_t cabac_mn[4][460][2] = {']
    for modo in range(4):
        out.append('    { /* ' + ('I' if modo == 0 else f'cabac_init_idc={modo-1}') + ' */')
        for i in range(0,460,10):
            out.append('        '+','.join('{%d,%d}'%tuple(x) for x in contextos[modo][i:i+10])+f', /* {i} */')
        out.append('    },')
    out.extend(['};', 'static const uint8_t cabac_rango_lps[64][4] = {'])
    out.extend('    {'+','.join(map(str,x))+'},' for x in rango)
    out.extend(['};', 'static const uint8_t cabac_trans_lps[64] = {'])
    out.extend('    '+','.join(map(str,trans[i:i+16]))+',' for i in range(0,64,16))
    out.extend(['};','#endif',''])
    return '\n'.join(out)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Uso: generar_tablas.py norma.txt salida.h')
    Path(sys.argv[2]).write_text(generar(Path(sys.argv[1]).read_text(encoding='utf-8')), encoding='utf-8')
