"""
Vérifie que le décodeur ADPCM de la 3DS (3ds/source/adpcm.h) est compatible
avec l'encodeur utilisé par l'appli PC (audioop.lin2adpcm).

    gcc -O2 -o adpcm_decode tests/adpcm_decode.c
    python tests/test_adpcm.py ./adpcm_decode
"""
import array
import math
import struct
import subprocess
import sys

INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]


def ref_encode(samples, state=None):
    """Encodeur de référence, même algorithme qu'audioop.lin2adpcm."""
    valpred, index = state or (0, 0)
    step = STEP[index]
    out, buf, high = bytearray(), 0, True
    for val in samples:
        diff = val - valpred
        sign = 8 if diff < 0 else 0
        if sign:
            diff = -diff
        delta, vpdiff = 0, step >> 3
        if diff >= step:
            delta, diff, vpdiff = 4, diff - step, vpdiff + step
        step >>= 1
        if diff >= step:
            delta |= 2
            diff -= step
            vpdiff += step
        step >>= 1
        if diff >= step:
            delta |= 1
            vpdiff += step
        valpred = valpred - vpdiff if sign else valpred + vpdiff
        valpred = max(-32768, min(32767, valpred))
        delta |= sign
        index = max(0, min(88, index + INDEX[delta]))
        step = STEP[index]
        if high:
            buf = (delta << 4) & 0xF0
        else:
            out.append(buf | (delta & 0x0F))
        high = not high
    return bytes(out), (valpred, index)


def c_decode(exe, data, state):
    inp = struct.pack(">hB", state[0], state[1]) + data
    raw = subprocess.run([exe], input=inp, capture_output=True, check=True).stdout
    return array.array("h", raw)


def main():
    exe = sys.argv[1]
    rate = 32000
    # musique de test : accords + bruit + silence + transitoire
    sig = []
    for i in range(rate):
        t = i / rate
        v = 9000 * math.sin(2 * math.pi * 220 * t) + 6000 * math.sin(2 * math.pi * 1375 * t)
        v += 3000 * math.sin(2 * math.pi * 5100 * t) * (1 if i % 4000 < 2000 else 0)
        if 20000 < i < 21000:
            v = 32000 if i % 50 < 25 else -32000
        sig.append(int(max(-32768, min(32767, v))))
    chunk = 640
    try:
        import audioop
    except ImportError:
        audioop = None
        print("audioop absent : comparaison avec l'encodeur de référence uniquement")

    state = None
    err2 = sig2 = 0
    for off in range(0, len(sig), chunk):
        part = sig[off:off + chunk]
        pcm = array.array("h", part).tobytes()
        st = state or (0, 0)
        enc, new_state = ref_encode(part, state)
        if audioop:
            a_enc, a_state = audioop.lin2adpcm(pcm, 2, state)
            assert a_enc == enc and tuple(a_state) == tuple(new_state), f"encodeur différent à {off}"
            a_dec, _ = audioop.adpcm2lin(enc, 2, st)
            ref_dec = array.array("h", a_dec)
        dec = c_decode(exe, enc, st)
        if audioop:
            assert dec == ref_dec, f"décodeur 3DS différent d'audioop à {off}"
        for i, (a, b) in enumerate(zip(part, dec)):
            if not 19000 <= off + i < 24000:   # qualité mesurée hors du créneau extrême
                err2 += (a - b) ** 2
                sig2 += a * a
        assert dec[-1] == new_state[0], f"décodeur désynchronisé de l'encodeur à {off}"
        state = new_state
    snr = 10 * math.log10(sig2 / max(1, err2))
    print(f"OK : décodeur 3DS {'identique à audioop, ' if audioop else ''}rapport signal/bruit {snr:.1f} dB")
    assert snr > 20, "qualité ADPCM anormalement basse"


if __name__ == "__main__":
    main()
