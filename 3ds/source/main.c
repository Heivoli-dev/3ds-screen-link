/*
 * 3DS Screen Link - application New 3DS / 3DS
 *
 * - Écran du haut : IP + port en attente, puis copie de l'écran du PC.
 * - Écran du bas  : clavier tactile AZERTY.
 * - Boutons / Circle Pad / C-Stick : envoyés au PC (manette virtuelle).
 *
 * La 3DS est le serveur TCP : elle écoute sur LISTEN_PORT et le PC s'y connecte.
 *
 * Protocole (tous les paquets) : [type:u8][taille:u32 big-endian][données]
 *   PC  -> 3DS : 0x10 HELLO, 0x11 FRAME (JPEG 400x240), 0x12 PING, 0x13 BYE,
 *                0x14 AUDIO (IMA ADPCM 32 kHz, voir audioPacket)
 *   3DS -> PC  : 0x01 HELLO (version), 0x02 INPUT, 0x03 KEY, 0x04 ACK (image affichée)
 */

#include <3ds.h>
#include <citro2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <setjmp.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <jpeglib.h>

#include "adpcm.h"
#include "assets.h"
#include "update.h"

#define APP_VERSION     "1.0.0"
#define LISTEN_PORT     8334

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define FRAME_W 400
#define FRAME_H 240
#define TEX_W   512
#define TEX_H   256

#define MAX_PAYLOAD     (512 * 1024)
#define MAX_JPEG_WIDTH  2048
#define NET_TIMEOUT_MS  5000

/* Paquets PC -> 3DS */
#define PKT_PC_HELLO  0x10
#define PKT_PC_FRAME  0x11
#define PKT_PC_PING   0x12
#define PKT_PC_BYE    0x13
#define PKT_PC_AUDIO  0x14
/* Paquets 3DS -> PC */
#define PKT_HELLO     0x01
#define PKT_INPUT     0x02
#define PKT_KEY       0x03
#define PKT_ACK       0x04

/* Modificateurs du clavier tactile */
#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04

/* ------------------------------------------------------------------ */
/* État partagé entre le thread principal et le thread réseau          */
/* ------------------------------------------------------------------ */

typedef enum { ST_WAITING, ST_CONNECTED } AppState;

static volatile bool     g_running = true;
static volatile AppState g_state = ST_WAITING;
static volatile int      g_client = -1;
static int               g_listen = -1;

static LightLock g_sendLock;
static LightLock g_frameLock;

static u32* g_frontBuf;               /* dernière image décodée (RGBA8 linéaire, 24 bits de couleur) */
static u32* g_backBuf;                /* image en cours de décodage */
static bool g_isNew3DS = false;       /* décodage précis sur New 3DS, rapide sur 3DS */
static volatile bool g_frameReady = false;

static char g_peer[32]   = "";
static char g_status[96] = "";

static u8 g_jpegRow[MAX_JPEG_WIDTH * 3];

/* ------------------------------------------------------------------ */
/* Réseau                                                              */
/* ------------------------------------------------------------------ */

static int sendAll(int fd, const u8* buf, u32 len)
{
    while (len > 0) {
        int n = send(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

/* Envoie un petit paquet (< 64 octets de données). Thread-safe. */
static bool sendPacket(u8 type, const void* data, u32 len)
{
    u8 pkt[5 + 64];
    if (len > 64) return false;

    pkt[0] = type;
    pkt[1] = (len >> 24) & 0xFF;
    pkt[2] = (len >> 16) & 0xFF;
    pkt[3] = (len >> 8) & 0xFF;
    pkt[4] = len & 0xFF;
    if (len) memcpy(pkt + 5, data, len);

    bool ok = false;
    LightLock_Lock(&g_sendLock);
    if (g_client >= 0)
        ok = sendAll(g_client, pkt, 5 + len) == 0;
    LightLock_Unlock(&g_sendLock);
    return ok;
}

/* Reçoit exactement len octets. 0 = ok, -1 = erreur/fermé, -2 = délai dépassé */
static int recvAll(int fd, u8* buf, u32 len, u64* lastActivity)
{
    u32 got = 0;
    while (got < len) {
        if (!g_running) return -1;

        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&p, 1, 100);
        if (r < 0) return -1;
        if (r == 0) {
            if (osGetTime() - *lastActivity > NET_TIMEOUT_MS) return -2;
            continue;
        }
        int n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
        *lastActivity = osGetTime();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Son : ADPCM -> PCM 16 bits stéréo, joué par le DSP (NDSP)           */
/* ------------------------------------------------------------------ */

#define AUDIO_CH          0
#define AUDIO_RATE        32000
#define AUDIO_BUFS        16       /* tampons de ~20 ms */
#define AUDIO_MAX_SAMPLES 2048     /* échantillons par canal et par paquet */
#define AUDIO_PREBUFFER   3        /* tampons d'avance avant de jouer (~60 ms) */
#define AUDIO_MAX_QUEUED  12       /* au-delà, on jette pour ne pas prendre de retard */

static bool g_audioOk = false;
static ndspWaveBuf g_waveBufs[AUDIO_BUFS];
static s16* g_audioMem = NULL;
static int g_waveNext = 0;
static volatile u32 g_audioPkts = 0;      /* paquets son joués (diagnostic) */

static bool audioInit(void)
{
    if (R_FAILED(ndspInit())) return false;   /* sdmc:/3ds/dspfirm.cdc absent */

    g_audioMem = (s16*)linearAlloc(AUDIO_BUFS * AUDIO_MAX_SAMPLES * 2 * sizeof(s16));
    if (!g_audioMem) {
        ndspExit();
        return false;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(AUDIO_CH);
    ndspChnSetInterp(AUDIO_CH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CH, AUDIO_RATE);
    ndspChnSetFormat(AUDIO_CH, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {0};
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(AUDIO_CH, mix);

    memset(g_waveBufs, 0, sizeof(g_waveBufs));
    for (int i = 0; i < AUDIO_BUFS; i++)
        g_waveBufs[i].data_pcm16 = g_audioMem + i * AUDIO_MAX_SAMPLES * 2;
    ndspChnSetPaused(AUDIO_CH, true);
    return true;
}

static int audioQueued(void)
{
    int n = 0;
    for (int i = 0; i < AUDIO_BUFS; i++)
        if (g_waveBufs[i].status == NDSP_WBUF_QUEUED || g_waveBufs[i].status == NDSP_WBUF_PLAYING)
            n++;
    return n;
}

static void audioReset(void)
{
    if (!g_audioOk) return;
    ndspChnWaveBufClear(AUDIO_CH);
    ndspChnSetPaused(AUDIO_CH, true);
}

/*
 * Paquet AUDIO : [canaux u8] puis pour chaque canal [valpred s16 BE][index u8],
 * puis les données ADPCM de chaque canal à la suite (même taille pour chacun).
 */
static void audioPacket(const u8* p, u32 len)
{
    if (!g_audioOk || len < 1) return;
    u32 ch = p[0];
    if (ch != 1 && ch != 2) return;
    u32 hdr = 1 + 3 * ch;
    if (len <= hdr) return;
    u32 perCh = (len - hdr) / ch;
    u32 samples = perCh * 2;
    if (samples == 0 || samples > AUDIO_MAX_SAMPLES) return;

    int queued = audioQueued();
    if (queued >= AUDIO_MAX_QUEUED) return;           /* trop d'avance : on rattrape */

    ndspWaveBuf* wb = &g_waveBufs[g_waveNext];
    if (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING) return;

    s16* out = wb->data_pcm16;
    for (u32 c = 0; c < ch; c++) {
        const u8* st = p + 1 + 3 * c;
        int valpred = (s16)((st[0] << 8) | st[1]);
        adpcmDecode(p + hdr + c * perCh, perCh, out + c, 2, valpred, st[2]);
    }
    if (ch == 1)
        for (u32 i = 0; i < samples; i++) out[i * 2 + 1] = out[i * 2];

    DSP_FlushDataCache(out, samples * 2 * sizeof(s16));

    /* Plus rien à jouer : on remet de l'avance avant de reprendre (évite les craquements) */
    if (queued == 0) ndspChnSetPaused(AUDIO_CH, true);

    wb->nsamples = samples;
    wb->looping = false;
    ndspChnWaveBufAdd(AUDIO_CH, wb);
    g_audioPkts++;
    g_waveNext = (g_waveNext + 1) % AUDIO_BUFS;

    if (queued + 1 >= AUDIO_PREBUFFER && ndspChnIsPaused(AUDIO_CH))
        ndspChnSetPaused(AUDIO_CH, false);
}

static void audioExit(void)
{
    if (!g_audioOk) return;
    ndspChnWaveBufClear(AUDIO_CH);
    ndspExit();
    linearFree(g_audioMem);
    g_audioOk = false;
}

/* ------------------------------------------------------------------ */
/* Décodage JPEG (libjpeg-turbo)                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
} JpegErr;

static void jpegErrorExit(j_common_ptr cinfo)
{
    JpegErr* e = (JpegErr*)cinfo->err;
    longjmp(e->jb, 1);
}

static void jpegSilent(j_common_ptr cinfo) { (void)cinfo; }

static bool decodeJpeg(const u8* data, u32 size, u32* out)
{
    struct jpeg_decompress_struct cinfo;
    JpegErr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;
    jerr.pub.output_message = jpegSilent;

    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char*)data, size);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.out_color_space = JCS_RGB;
    cinfo.dct_method = g_isNew3DS ? JDCT_ISLOW : JDCT_IFAST;
    cinfo.do_fancy_upsampling = g_isNew3DS ? TRUE : FALSE;
    cinfo.do_block_smoothing = FALSE;

    jpeg_start_decompress(&cinfo);

    if (cinfo.output_width > MAX_JPEG_WIDTH || cinfo.output_components != 3) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    u32 w = cinfo.output_width;
    u32 h = cinfo.output_height;
    u32 cw = w < FRAME_W ? w : FRAME_W;

    if (w < FRAME_W || h < FRAME_H)
        for (u32 i = 0; i < FRAME_W * FRAME_H; i++) out[i] = 0x000000FF;

    while (cinfo.output_scanline < h) {
        u32 y = cinfo.output_scanline;
        JSAMPROW rp = g_jpegRow;
        jpeg_read_scanlines(&cinfo, &rp, 1);
        if (y >= FRAME_H) continue;

        u32* dst = out + y * FRAME_W;
        const u8* src = g_jpegRow;
        for (u32 x = 0; x < cw; x++, src += 3)
            dst[x] = ((u32)src[0] << 24) | ((u32)src[1] << 16) | ((u32)src[2] << 8) | 0xFF;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

/* ------------------------------------------------------------------ */
/* Thread réseau : accepte le PC, reçoit et décode les images          */
/* ------------------------------------------------------------------ */

static void netThread(void* arg)
{
    (void)arg;
    u8* payload = (u8*)malloc(MAX_PAYLOAD);
    if (!payload) {
        snprintf(g_status, sizeof(g_status), "Erreur : mémoire insuffisante");
        return;
    }

    while (g_running) {
        struct pollfd p = { .fd = g_listen, .events = POLLIN, .revents = 0 };
        if (poll(&p, 1, 200) <= 0) continue;

        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int c = accept(g_listen, (struct sockaddr*)&caddr, &clen);
        if (c < 0) continue;

        snprintf(g_peer, sizeof(g_peer), "%s", inet_ntoa(caddr.sin_addr));
        snprintf(g_status, sizeof(g_status), "Connexion de %s...", g_peer);

        LightLock_Lock(&g_sendLock);
        g_client = c;
        LightLock_Unlock(&g_sendLock);

        u64 lastActivity = osGetTime();
        bool alive = true;

        while (g_running && alive) {
            u8 hdr[5];
            int r = recvAll(c, hdr, 5, &lastActivity);
            if (r != 0) {
                snprintf(g_status, sizeof(g_status), "%s",
                         r == -2 ? "Le PC ne répond plus" : "Connexion fermée par le PC");
                break;
            }

            u32 len = ((u32)hdr[1] << 24) | ((u32)hdr[2] << 16) | ((u32)hdr[3] << 8) | hdr[4];
            if (len > MAX_PAYLOAD) {
                snprintf(g_status, sizeof(g_status), "Erreur : paquet trop gros (%lu)", (unsigned long)len);
                break;
            }
            if (len && recvAll(c, payload, len, &lastActivity) != 0) {
                snprintf(g_status, sizeof(g_status), "Connexion perdue");
                break;
            }

            switch (hdr[0]) {
            case PKT_PC_HELLO:
                g_state = ST_CONNECTED;
                snprintf(g_status, sizeof(g_status), "Connecté à %s", g_peer);
                sendPacket(PKT_HELLO, APP_VERSION, strlen(APP_VERSION));
                break;

            case PKT_PC_FRAME:
                if (g_state == ST_CONNECTED && decodeJpeg(payload, len, g_backBuf)) {
                    LightLock_Lock(&g_frameLock);
                    u32* t = g_frontBuf;
                    g_frontBuf = g_backBuf;
                    g_backBuf = t;
                    g_frameReady = true;
                    LightLock_Unlock(&g_frameLock);
                }
                /* On acquitte toujours pour que le PC envoie l'image suivante */
                sendPacket(PKT_ACK, NULL, 0);
                break;

            case PKT_PC_AUDIO:
                if (g_state == ST_CONNECTED) audioPacket(payload, len);
                break;

            case PKT_PC_BYE:
                snprintf(g_status, sizeof(g_status), "Le PC s'est déconnecté");
                alive = false;
                break;

            case PKT_PC_PING:
            default:
                break;
            }
        }

        LightLock_Lock(&g_sendLock);
        g_client = -1;
        LightLock_Unlock(&g_sendLock);
        close(c);
        g_state = ST_WAITING;
        audioReset();
    }

    free(payload);
}

/* ------------------------------------------------------------------ */
/* Texture de l'écran du haut                                          */
/* ------------------------------------------------------------------ */

static C3D_Tex g_tex;
static const Tex3DS_SubTexture g_subTex = {
    FRAME_W, FRAME_H,
    0.0f, 1.0f,
    FRAME_W / (float)TEX_W, 1.0f - FRAME_H / (float)TEX_H
};

/* Copie une image RGBA8 linéaire (w x h, 'stride' pixels par ligne) dans une
 * texture RGBA8, au format "tuilé" du GPU 3DS */
static void uploadRGBA(C3D_Tex* tex, const u32* src, u32 stride, u32 w, u32 h)
{
    u32* dst = (u32*)tex->data;
    for (u32 y = 0; y < h; y++) {
        const u32* row = src + y * stride;
        u32 tileRow = (y >> 3) * (tex->width >> 3);
        u32 ym = ((y & 1) << 1) | ((y & 2) << 2) | ((y & 4) << 3);
        for (u32 x = 0; x < w; x++) {
            u32 xm = (x & 1) | ((x & 2) << 1) | ((x & 4) << 2);
            dst[((tileRow + (x >> 3)) << 6) + (xm | ym)] = row[x];
        }
    }
    C3D_TexFlush(tex);
}

static void uploadFrame(const u32* src)
{
    uploadRGBA(&g_tex, src, FRAME_W, FRAME_W, FRAME_H);
}

/* ------------------------------------------------------------------ */
/* Design de l'écran d'attente (fonds + chiffres dans la police du design) */
/* ------------------------------------------------------------------ */

static C3D_Tex g_bgTopTex, g_bgBotTex, g_digitsTex;
static bool g_bgOk = false;
static const Tex3DS_SubTexture g_bgTopSub = { 400, 240, 0.0f, 1.0f, 400 / 512.0f, 1.0f - 240 / 256.0f };
static const Tex3DS_SubTexture g_bgBotSub = { 320, 240, 0.0f, 1.0f, 320 / 512.0f, 1.0f - 240 / 256.0f };

static bool loadDesign(u32* scratch)
{
    C3D_TexInit(&g_bgTopTex, 512, 256, GPU_RGBA8);
    C3D_TexInit(&g_bgBotTex, 512, 256, GPU_RGBA8);
    C3D_TexInit(&g_digitsTex, DIGITS_TEX_W, DIGITS_TEX_H, GPU_RGBA8);
    C3D_TexSetFilter(&g_digitsTex, GPU_NEAREST, GPU_NEAREST);

    if (!decodeJpeg(ASSET_TOP_JPG, sizeof(ASSET_TOP_JPG), scratch)) return false;
    uploadRGBA(&g_bgTopTex, scratch, FRAME_W, 400, 240);
    if (!decodeJpeg(ASSET_BOTTOM_JPG, sizeof(ASSET_BOTTOM_JPG), scratch)) return false;
    uploadRGBA(&g_bgBotTex, scratch, FRAME_W, 320, 240);

    /* chiffres : noir + transparence */
    for (u32 i = 0; i < DIGITS_TEX_W * DIGITS_TEX_H; i++)
        scratch[i] = ASSET_DIGITS_ALPHA[i];
    uploadRGBA(&g_digitsTex, scratch, DIGITS_TEX_W, DIGITS_TEX_W, DIGITS_TEX_H);
    return true;
}

/* Écrit une IP / un port dans la police du design. baseline = bas des chiffres. */
static void drawDigits(const char* str, float x, float baseline)
{
    for (; *str; str++) {
        const DigitGlyph* g = NULL;
        for (int i = 0; i < DIGIT_GLYPH_COUNT; i++)
            if (DIGIT_GLYPHS[i].c == *str) { g = &DIGIT_GLYPHS[i]; break; }
        if (!g) { x += 8; continue; }

        Tex3DS_SubTexture sub = {
            g->w, DIGITS_TEX_H,
            g->x / (float)DIGITS_TEX_W, 1.0f,
            (g->x + g->w) / (float)DIGITS_TEX_W, 0.0f
        };
        C2D_Image img = { &g_digitsTex, &sub };
        C2D_DrawImageAt(img, (int)x, (int)(baseline - DIGITS_BASELINE), 0.5f, NULL, 1.0f, 1.0f);
        x += g->advance;
    }
}

/* ------------------------------------------------------------------ */
/* Clavier tactile AZERTY                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* label;
    const char* shiftLabel;   /* NULL = même que label */
    const char* name;         /* nom envoyé au PC */
    float units;              /* largeur relative */
    u8 mod;                   /* 0 ou MOD_xxx pour une touche modificatrice */
} KeyDef;

typedef struct {
    const KeyDef* def;
    float x, y, w, h;
} Key;

#define KB_TOP      30.0f
#define KB_ROWS     5
#define KB_UNITS    12.0f

static const KeyDef ROW0[] = {
    {"Esc", NULL, "esc", 1, 0},
    {"1", NULL, "1", 1, 0}, {"2", NULL, "2", 1, 0}, {"3", NULL, "3", 1, 0},
    {"4", NULL, "4", 1, 0}, {"5", NULL, "5", 1, 0}, {"6", NULL, "6", 1, 0},
    {"7", NULL, "7", 1, 0}, {"8", NULL, "8", 1, 0}, {"9", NULL, "9", 1, 0},
    {"0", NULL, "0", 1, 0},
    {"Eff", NULL, "backspace", 1, 0},
};
static const KeyDef ROW1[] = {
    {"Tab", NULL, "tab", 1, 0},
    {"a", "A", "a", 1, 0}, {"z", "Z", "z", 1, 0}, {"e", "E", "e", 1, 0},
    {"r", "R", "r", 1, 0}, {"t", "T", "t", 1, 0}, {"y", "Y", "y", 1, 0},
    {"u", "U", "u", 1, 0}, {"i", "I", "i", 1, 0}, {"o", "O", "o", 1, 0},
    {"p", "P", "p", 1, 0},
    {"-", "_", "-", 1, 0},
};
static const KeyDef ROW2[] = {
    {"q", "Q", "q", 1, 0}, {"s", "S", "s", 1, 0}, {"d", "D", "d", 1, 0},
    {"f", "F", "f", 1, 0}, {"g", "G", "g", 1, 0}, {"h", "H", "h", 1, 0},
    {"j", "J", "j", 1, 0}, {"k", "K", "k", 1, 0}, {"l", "L", "l", 1, 0},
    {"m", "M", "m", 1, 0},
    {"Entrée", NULL, "enter", 2, 0},
};
static const KeyDef ROW3[] = {
    {"Maj", NULL, "shift", 2, MOD_SHIFT},
    {"w", "W", "w", 1, 0}, {"x", "X", "x", 1, 0}, {"c", "C", "c", 1, 0},
    {"v", "V", "v", 1, 0}, {"b", "B", "b", 1, 0}, {"n", "N", "n", 1, 0},
    {",", "?", ",", 1, 0}, {".", ":", ".", 1, 0},
    {"Suppr", NULL, "delete", 2, 0},
};
static const KeyDef ROW4[] = {
    {"Ctrl", NULL, "ctrl", 1.5f, MOD_CTRL},
    {"Alt", NULL, "alt", 1.5f, MOD_ALT},
    {"Espace", NULL, "space", 5, 0},
    {"<", NULL, "left", 1, 0}, {"^", NULL, "up", 1, 0},
    {"v", NULL, "down", 1, 0}, {">", NULL, "right", 1, 0},
};

static const KeyDef* const ROWS[KB_ROWS] = { ROW0, ROW1, ROW2, ROW3, ROW4 };
static const int ROW_LEN[KB_ROWS] = {
    sizeof(ROW0) / sizeof(KeyDef), sizeof(ROW1) / sizeof(KeyDef),
    sizeof(ROW2) / sizeof(KeyDef), sizeof(ROW3) / sizeof(KeyDef),
    sizeof(ROW4) / sizeof(KeyDef),
};

#define MAX_KEYS 80
static Key g_keys[MAX_KEYS];
static int g_keyCount = 0;

static void buildKeyboard(void)
{
    const float rowH = (240.0f - KB_TOP) / KB_ROWS;
    const float unitW = 320.0f / KB_UNITS;
    g_keyCount = 0;
    for (int r = 0; r < KB_ROWS; r++) {
        float x = 0;
        for (int i = 0; i < ROW_LEN[r] && g_keyCount < MAX_KEYS; i++) {
            Key* k = &g_keys[g_keyCount++];
            k->def = &ROWS[r][i];
            k->x = x;
            k->y = KB_TOP + r * rowH;
            k->w = ROWS[r][i].units * unitW;
            k->h = rowH;
            x += k->w;
        }
    }
}

static int keyAt(int px, int py)
{
    for (int i = 0; i < g_keyCount; i++) {
        const Key* k = &g_keys[i];
        if (px >= k->x && px < k->x + k->w && py >= k->y && py < k->y + k->h)
            return i;
    }
    return -1;
}

/* Envoie une touche : action 1 = appui, 0 = relâchement */
static void sendKey(u8 action, u8 mods, const char* name)
{
    u8 buf[40];
    size_t n = strlen(name);
    if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[0] = action;
    buf[1] = mods;
    memcpy(buf + 2, name, n);
    sendPacket(PKT_KEY, buf, 2 + n);
}

/* ------------------------------------------------------------------ */
/* Dessin                                                              */
/* ------------------------------------------------------------------ */

static C2D_TextBuf g_textBuf;

static void drawText(const char* str, float x, float y, float scale, u32 color, u32 flags)
{
    C2D_Text t;
    C2D_TextParse(&t, g_textBuf, str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
}

static void drawTextCentered(const char* str, float cx, float cy, float scale, u32 color)
{
    C2D_Text t;
    float w, h;
    C2D_TextParse(&t, g_textBuf, str);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    C2D_DrawText(&t, C2D_WithColor, cx - w / 2, cy - h / 2, 0.5f, scale, scale, color);
}

static void drawKeyboard(int pressedKey, u8 mods)
{
    const u32 colBorder  = C2D_Color32(0x10, 0x12, 0x18, 0xFF);
    const u32 colKey     = C2D_Color32(0x3A, 0x3F, 0x4B, 0xFF);
    const u32 colSpecial = C2D_Color32(0x2B, 0x2F, 0x38, 0xFF);
    const u32 colPressed = C2D_Color32(0x4C, 0x8B, 0xF5, 0xFF);
    const u32 colModOn   = C2D_Color32(0xE0, 0x8A, 0x1E, 0xFF);
    const u32 colText    = C2D_Color32(0xF0, 0xF0, 0xF0, 0xFF);

    for (int i = 0; i < g_keyCount; i++) {
        const Key* k = &g_keys[i];
        const KeyDef* d = k->def;
        bool special = d->units != 1.0f || d->mod || strlen(d->name) > 1;

        u32 fill = special ? colSpecial : colKey;
        if (d->mod && (mods & d->mod)) fill = colModOn;
        if (i == pressedKey) fill = colPressed;

        C2D_DrawRectSolid(k->x, k->y, 0.5f, k->w, k->h, colBorder);
        C2D_DrawRectSolid(k->x + 1.5f, k->y + 1.5f, 0.5f, k->w - 3, k->h - 3, fill);

        const char* label = ((mods & MOD_SHIFT) && d->shiftLabel) ? d->shiftLabel : d->label;
        float scale = strlen(label) > 2 ? 0.42f : 0.6f;
        drawTextCentered(label, k->x + k->w / 2, k->y + k->h / 2, scale, colText);
    }
}

/* ------------------------------------------------------------------ */
/* Programme principal                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv)
{

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    osSetSpeedupEnable(true);

    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_textBuf = C2D_TextBufNew(4096);

    LightLock_Init(&g_sendLock);
    LightLock_Init(&g_frameLock);

    g_frontBuf = (u32*)linearAlloc(FRAME_W * FRAME_H * sizeof(u32));
    g_backBuf  = (u32*)linearAlloc(FRAME_W * FRAME_H * sizeof(u32));
    for (u32 i = 0; i < FRAME_W * FRAME_H; i++) g_frontBuf[i] = 0x000000FF;

    C3D_TexInit(&g_tex, TEX_W, TEX_H, GPU_RGBA8);
    C3D_TexSetFilter(&g_tex, GPU_LINEAR, GPU_LINEAR);
    memset(g_tex.data, 0, g_tex.size);
    C3D_TexFlush(&g_tex);
    C2D_Image screenImg = { &g_tex, &g_subTex };

    buildKeyboard();
    g_audioOk = audioInit();
    g_bgOk = loadDesign(g_backBuf);

    /* Chemin de l'appli sur la carte SD (donné par le Homebrew Launcher) pour la mise à jour */
    char selfPath[256] = "sdmc:/3ds/ScreenLink3DS.3dsx";
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0)
        snprintf(selfPath, sizeof(selfPath), "%s", argv[0]);

    /* --- Réseau --- */
    char ipStr[32] = "?";
    char netError[96] = "";
    u32* socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    bool socOk = socBuf && R_SUCCEEDED(socInit(socBuf, SOC_BUFFERSIZE));

    if (!socOk) {
        snprintf(netError, sizeof(netError), "Réseau indisponible");
    } else {
        struct in_addr a;
        a.s_addr = gethostid();
        if (a.s_addr == 0) {
            snprintf(netError, sizeof(netError), "Wi-Fi désactivé");
        } else {
            snprintf(ipStr, sizeof(ipStr), "%s", inet_ntoa(a));

            g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(LISTEN_PORT);
            addr.sin_addr.s_addr = INADDR_ANY;

            if (g_listen < 0 ||
                bind(g_listen, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
                listen(g_listen, 1) < 0) {
                snprintf(netError, sizeof(netError), "Port %d indisponible", LISTEN_PORT);
                if (g_listen >= 0) { close(g_listen); g_listen = -1; }
            }
        }
    }

    /* Thread réseau sur un autre cœur : cœur 2 sur New 3DS, cœur 1 sur 3DS */
    Thread netTh = NULL;
    if (g_listen >= 0) {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        g_isNew3DS = isNew;
        if (!isNew) APT_SetAppCpuTimeLimit(80);

        netTh = threadCreate(netThread, NULL, 64 * 1024, prio - 1, isNew ? 2 : 1, false);
        if (!netTh)
            netTh = threadCreate(netThread, NULL, 64 * 1024, prio - 1, -2, false);
        if (!netTh)
            snprintf(netError, sizeof(netError), "Erreur interne (thread)");
    }

    /* Recherche d'une mise à jour sur GitHub (en arrière-plan, sur l'autre cœur) */
    {
        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        if (!isNew) APT_SetAppCpuTimeLimit(80);
        updateInit(isNew ? 2 : 1);
    }
    if (socOk && !netError[0])
        updateStartCheck(APP_VERSION);

    /* --- Boucle principale --- */
    int pressedKey = -1;
    u8 mods = 0;
    u8 pressedMods = 0;
    u8 lastInput[12] = {0};
    u64 lastInputSend = 0;
    u32 fps = 0, fpsCount = 0, audioRate = 0, audioLast = 0;
    u64 fpsTime = osGetTime();

    const u32 colBg     = C2D_Color32(0x14, 0x16, 0x1C, 0xFF);
    const u32 colTitle  = C2D_Color32(0x4C, 0x8B, 0xF5, 0xFF);
    const u32 colText   = C2D_Color32(0xF0, 0xF0, 0xF0, 0xFF);
    const u32 colDim    = C2D_Color32(0x90, 0x96, 0xA3, 0xFF);
    const u32 colErr    = C2D_Color32(0xF0, 0x5A, 0x5A, 0xFF);
    const u32 colOk     = C2D_Color32(0x4C, 0xD1, 0x7A, 0xFF);
    const u32 colInk    = C2D_Color32(0x22, 0x22, 0x22, 0xFF);   /* texte sur le design blanc */
    const u32 colGrey   = C2D_Color32(0x9A, 0x9A, 0x9A, 0xFF);
    const u32 colOkDark = C2D_Color32(0x1E, 0x9E, 0x50, 0xFF);
    const u32 colAccent = C2D_Color32(0x2F, 0x6F, 0xE0, 0xFF);

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        bool connected = (g_state == ST_CONNECTED);

        /* Quitter : START en attente, L + R + SELECT une fois connecté */
        if (!connected && (kDown & KEY_START)) break;
        if (!connected && (kDown & KEY_Y) && updateState() == UPD_AVAILABLE)
            updateStartDownload(selfPath);
        if ((kHeld & KEY_L) && (kHeld & KEY_R) && (kDown & KEY_SELECT)) break;

        if (connected) {
            /* --- Manette --- */
            circlePosition cp, cs;
            hidCircleRead(&cp);
            hidCstickRead(&cs);
            u32 held = kHeld & ~KEY_TOUCH;

            u8 in[12];
            in[0] = held >> 24; in[1] = held >> 16; in[2] = held >> 8; in[3] = held;
            in[4] = (u16)cp.dx >> 8; in[5] = (u16)cp.dx;
            in[6] = (u16)cp.dy >> 8; in[7] = (u16)cp.dy;
            in[8] = (u16)cs.dx >> 8; in[9] = (u16)cs.dx;
            in[10] = (u16)cs.dy >> 8; in[11] = (u16)cs.dy;

            u64 now = osGetTime();
            if (memcmp(in, lastInput, sizeof(in)) != 0 || now - lastInputSend > 100) {
                if (sendPacket(PKT_INPUT, in, sizeof(in))) {
                    memcpy(lastInput, in, sizeof(in));
                    lastInputSend = now;
                }
            }

            /* --- Clavier tactile --- */
            if (kDown & KEY_TOUCH) {
                touchPosition tp;
                hidTouchRead(&tp);
                int k = keyAt(tp.px, tp.py);
                if (k >= 0) {
                    const KeyDef* d = g_keys[k].def;
                    if (d->mod) {
                        mods ^= d->mod;           /* modificateur collant */
                    } else {
                        pressedKey = k;
                        pressedMods = mods;
                        sendKey(1, pressedMods, d->name);
                    }
                }
            } else if (pressedKey >= 0 && !(kHeld & KEY_TOUCH)) {
                sendKey(0, pressedMods, g_keys[pressedKey].def->name);
                pressedKey = -1;
                mods = 0;                         /* un modificateur s'applique à une seule touche */
            }
        } else {
            pressedKey = -1;
            mods = 0;
            memset(lastInput, 0, sizeof(lastInput));
        }

        /* --- Rendu --- */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(g_textBuf);

        LightLock_Lock(&g_frameLock);
        if (g_frameReady) {
            uploadFrame(g_frontBuf);
            g_frameReady = false;
            fpsCount++;
        }
        LightLock_Unlock(&g_frameLock);

        u64 now = osGetTime();
        if (now - fpsTime >= 1000) {
            fps = fpsCount;
            fpsCount = 0;
            u32 a = g_audioPkts;
            audioRate = a - audioLast;
            audioLast = a;
            fpsTime = now;
        }

        char line[128];

        /* Écran du haut */
        C2D_TargetClear(top, connected ? C2D_Color32(0, 0, 0, 0xFF) : colBg);
        C2D_SceneBegin(top);
        if (connected) {
            C2D_DrawImageAt(screenImg, 0, 0, 0.5f, NULL, 1.0f, 1.0f);
        } else if (g_bgOk) {
            /* Design : « IP » et « Port » sont dans l'image, on écrit les valeurs à côté */
            C2D_DrawImageAt((C2D_Image){ &g_bgTopTex, &g_bgTopSub }, 0, 0, 0.4f, NULL, 1.0f, 1.0f);
            if (netError[0]) {
                drawText(netError, 140, 66, 0.75f, colErr, 0);
            } else {
                drawDigits(ipStr, 140, 92);
            }
            snprintf(line, sizeof(line), "%d", LISTEN_PORT);
            drawDigits(line, 192, 145);
            snprintf(line, sizeof(line), "v%s", APP_VERSION);
            drawText(line, 396, 3, 0.4f, colGrey, C2D_AlignRight);
            drawText("\xC2\xA9 heivoli-network.fr", 4, 3, 0.4f, colGrey, 0);
        } else {
            drawTextCentered("3DS Screen Link", 200, 40, 1.0f, colTitle);
            if (netError[0]) {
                drawTextCentered(netError, 200, 120, 0.55f, colErr);
            } else {
                snprintf(line, sizeof(line), "IP : %s", ipStr);
                drawTextCentered(line, 200, 100, 0.9f, colText);
                snprintf(line, sizeof(line), "Port : %d", LISTEN_PORT);
                drawTextCentered(line, 200, 135, 0.9f, colText);
            }
        }

        /* Écran du bas */
        C2D_TargetClear(bot, (connected || !g_bgOk) ? colBg : C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        C2D_SceneBegin(bot);
        if (connected) {
            if (g_audioOk)
                snprintf(line, sizeof(line), "%s | %lu i/s | son %lu/s", g_peer,
                         (unsigned long)fps, (unsigned long)audioRate);
            else
                snprintf(line, sizeof(line), "%s | %lu i/s | pas de son", g_peer, (unsigned long)fps);
            drawText(line, 6, 7, 0.45f, colOk, 0);
            drawText("L+R+SELECT : quitter", 314, 7, 0.4f, colDim, C2D_AlignRight);
            drawKeyboard(pressedKey, mods);
        } else {
            /* Design : « en attente du pc » et « START: Exit » sont dans l'image */
            u32 txt = g_bgOk ? colInk : colText;
            if (g_bgOk)
                C2D_DrawImageAt((C2D_Image){ &g_bgBotTex, &g_bgBotSub }, 0, 0, 0.4f, NULL, 1.0f, 1.0f);
            else
                drawTextCentered("En attente du PC...", 160, 78, 0.7f, colText);

            if (g_status[0])
                drawTextCentered(g_status, 160, 116, 0.5f, txt);
            drawTextCentered(g_audioOk ? "Son : activé" : "Son indisponible (dspfirm.cdc manquant)",
                             160, 138, 0.45f, g_audioOk ? colOkDark : colErr);

            UpdState us = updateState();
            if (us == UPD_AVAILABLE) {
                snprintf(line, sizeof(line), "Mise à jour v%s disponible !", updateVersion());
                drawTextCentered(line, 160, 166, 0.55f, colAccent);
                drawTextCentered("Appuie sur Y pour l'installer", 160, 186, 0.45f, txt);
            } else if (us == UPD_DOWNLOADING) {
                int pct = updateProgress();
                unsigned kb = updateBytes() / 1024, tot = updateTotal() / 1024;
                if (tot)
                    snprintf(line, sizeof(line), "Téléchargement de la v%s : %u / %u Ko", updateVersion(), kb, tot);
                else
                    snprintf(line, sizeof(line), "Connexion à GitHub pour la v%s...", updateVersion());
                drawTextCentered(line, 160, 166, 0.5f, txt);
                C2D_DrawRectSolid(60, 182, 0.5f, 200, 8, C2D_Color32(0xDD, 0xDD, 0xDD, 0xFF));
                C2D_DrawRectSolid(60, 182, 0.5f, 2.0f * pct, 8, colAccent);
            } else if (us == UPD_DONE) {
                drawTextCentered("Mise à jour installée !", 160, 166, 0.55f, colOkDark);
                drawTextCentered("Quitte avec START puis relance l'appli", 160, 186, 0.45f, txt);
            } else if (us == UPD_ERROR) {
                snprintf(line, sizeof(line), "Mise à jour : %s", updateError());
                drawTextCentered(line, 160, 172, 0.42f, colErr);
            } else if (us == UPD_CHECKING) {
                drawTextCentered("Recherche de mise à jour...", 160, 172, 0.42f, colGrey);
            }
            if (!g_bgOk)
                drawTextCentered("START : quitter", 160, 210, 0.5f, colDim);
        }

        C3D_FrameEnd(0);
    }

    /* --- Nettoyage --- */
    g_running = false;
    if (netTh) {
        threadJoin(netTh, U64_MAX);
        threadFree(netTh);
    }
    if (g_listen >= 0) close(g_listen);
    updateExit();
    audioExit();
    if (socOk) socExit();
    free(socBuf);

    C3D_TexDelete(&g_tex);
    C3D_TexDelete(&g_bgTopTex);
    C3D_TexDelete(&g_bgBotTex);
    C3D_TexDelete(&g_digitsTex);
    linearFree(g_frontBuf);
    linearFree(g_backBuf);
    C2D_TextBufDelete(g_textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
