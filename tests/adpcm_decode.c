/* Outil de test : décode un flux ADPCM (stdin) avec le décodeur de la 3DS.
 * Entrée  : valpred (s16 big-endian), index (u8), puis les octets ADPCM.
 * Sortie  : échantillons s16 little-endian (stdout). */
#include <stdio.h>
#include <stdlib.h>
#include "../3ds/source/adpcm.h"

int main(void)
{
    static uint8_t in[1 << 20];
    static int16_t out[1 << 21];
    size_t n = fread(in, 1, sizeof(in), stdin);
    if (n < 3) return 1;
    int valpred = (int16_t)((in[0] << 8) | in[1]);
    int index = in[2];
    uint32_t nbytes = (uint32_t)(n - 3);
    adpcmDecode(in + 3, nbytes, out, 1, valpred, index);
    fwrite(out, sizeof(int16_t), nbytes * 2, stdout);
    return 0;
}
