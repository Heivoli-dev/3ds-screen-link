/*
 * Décodeur IMA ADPCM, identique à audioop.adpcm2lin de Python
 * (2 échantillons par octet, poids fort en premier).
 * Sans dépendance à la 3DS : testé sur PC par tests/test_adpcm.py.
 */
#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>

static const int8_t ADPCM_INDEX[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t ADPCM_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

/*
 * Décode nbytes octets (soit 2 * nbytes échantillons) à partir de l'état
 * (valpred, index). Chaque échantillon est écrit dans out, puis out avance
 * de 'stride' (2 pour remplir un canal d'un tampon stéréo entrelacé).
 */
static inline void adpcmDecode(const uint8_t* in, uint32_t nbytes, int16_t* out, int stride,
                               int valpred, int index)
{
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    int step = ADPCM_STEP[index];

    for (uint32_t i = 0; i < nbytes * 2; i++) {
        int delta = (i & 1) ? (in[i >> 1] & 0x0F) : (in[i >> 1] >> 4);

        index += ADPCM_INDEX[delta];
        if (index < 0) index = 0;
        else if (index > 88) index = 88;

        int sign = delta & 8;
        delta &= 7;

        int vpdiff = step >> 3;
        if (delta & 4) vpdiff += step;
        if (delta & 2) vpdiff += step >> 1;
        if (delta & 1) vpdiff += step >> 2;

        if (sign) valpred -= vpdiff;
        else valpred += vpdiff;

        if (valpred > 32767) valpred = 32767;
        else if (valpred < -32768) valpred = -32768;

        step = ADPCM_STEP[index];
        *out = (int16_t)valpred;
        out += stride;
    }
}

#endif
