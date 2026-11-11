/*
 * Mise à jour de l'appli 3DS depuis les releases GitHub (libcurl + mbedTLS).
 *
 * 1. GET https://api.github.com/repos/<dépôt>/releases/latest
 * 2. Compare "tag_name" à la version en cours.
 * 3. Télécharge l'asset ScreenLink3DS.3dsx à côté de l'appli (.new),
 *    vérifie l'en-tête 3DSX, puis remplace l'ancien fichier.
 */
#include <3ds.h>
#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update.h"

#define UPD_REPO   "Heivoli-dev/3ds-screen-link"
#define UPD_API    "https://api.github.com/repos/" UPD_REPO "/releases/latest"
#define UPD_ASSET  "ScreenLink3DS.3dsx"
#define UPD_STACK  (256 * 1024)

static volatile UpdState s_state = UPD_IDLE;
static volatile int s_progress = 0;
static volatile bool s_abort = false;
static char s_current[32];
static char s_version[32];
static char s_url[512];
static char s_target[256];
static char s_error[96];
static Thread s_thread = NULL;
static bool s_curlOk = false;

typedef struct {
    char* data;
    size_t len, cap;
} MemBuf;

static void setError(const char* msg)
{
    snprintf(s_error, sizeof(s_error), "%s", msg);
    s_state = UPD_ERROR;
}

static size_t memWrite(void* ptr, size_t size, size_t n, void* ud)
{
    MemBuf* m = (MemBuf*)ud;
    size_t total = size * n;
    if (m->len + total + 1 > m->cap) {
        size_t cap = (m->len + total + 1) * 2;
        if (cap > 1024 * 1024) return 0;              /* réponse anormalement grosse */
        char* d = (char*)realloc(m->data, cap);
        if (!d) return 0;
        m->data = d;
        m->cap = cap;
    }
    memcpy(m->data + m->len, ptr, total);
    m->len += total;
    m->data[m->len] = 0;
    return total;
}

static int onProgress(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ud; (void)ultotal; (void)ulnow;
    if (dltotal > 0) s_progress = (int)(dlnow * 100 / dltotal);
    return s_abort ? 1 : 0;                           /* non nul = annuler */
}

static CURL* makeCurl(const char* url)
{
    CURL* c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "ScreenLink3DS");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 100L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64 * 1024L);
    /* La 3DS n'a pas de magasin de certificats utilisable par curl */
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    return c;
}

/* Copie la chaîne JSON qui suit "key": dans out. Renvoie le pointeur après la valeur. */
static const char* jsonString(const char* from, const char* key, char* out, size_t outSize)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(from, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    p = strchr(p, '"');
    if (!p) return NULL;
    p++;
    const char* e = strchr(p, '"');
    if (!e) return NULL;
    size_t n = (size_t)(e - p);
    if (n >= outSize) n = outSize - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return e + 1;
}

static void parseVersion(const char* v, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]);
}

static bool isNewer(const char* latest, const char* current)
{
    int a[3], b[3];
    parseVersion(latest, a);
    parseVersion(current, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

static void checkThread(void* arg)
{
    (void)arg;
    MemBuf m = {0};
    CURL* c = makeCurl(UPD_API);
    if (!c) { setError("Erreur interne (curl)"); return; }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, memWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK || !m.data) {
        free(m.data);
        setError("Pas de connexion à GitHub");
        return;
    }

    char tag[32];
    if (!jsonString(m.data, "tag_name", tag, sizeof(tag))) {
        free(m.data);
        setError("Réponse GitHub illisible");
        return;
    }

    /* Cherche l'URL de l'asset .3dsx parmi les fichiers de la release */
    bool found = false;
    const char* p = m.data;
    char url[512];
    while ((p = jsonString(p, "browser_download_url", url, sizeof(url))) != NULL) {
        size_t n = strlen(url), an = strlen(UPD_ASSET);
        if (n > an && strcmp(url + n - an, UPD_ASSET) == 0 && url[n - an - 1] == '/') {
            found = true;
            break;
        }
    }
    free(m.data);

    snprintf(s_version, sizeof(s_version), "%s", tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
    if (!isNewer(tag, s_current)) {
        s_state = UPD_UP_TO_DATE;
        return;
    }
    if (!found) {
        setError("Fichier .3dsx absent de la release");
        return;
    }
    snprintf(s_url, sizeof(s_url), "%s", url);
    s_state = UPD_AVAILABLE;
}

static void downloadThread(void* arg)
{
    (void)arg;
    char tmp[272];
    snprintf(tmp, sizeof(tmp), "%s.new", s_target);

    FILE* f = fopen(tmp, "wb");
    if (!f) { setError("Impossible d'écrire sur la carte SD"); return; }

    CURL* c = makeCurl(s_url);
    if (!c) { fclose(f); remove(tmp); setError("Erreur interne (curl)"); return; }
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);       /* écriture par défaut : fwrite */
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    fclose(f);

    if (rc != CURLE_OK) {
        remove(tmp);
        setError(s_abort ? "Téléchargement annulé" : "Téléchargement échoué");
        return;
    }

    /* Vérifie que c'est bien un exécutable 3DSX complet avant de remplacer */
    f = fopen(tmp, "rb");
    char magic[4] = {0};
    long size = 0;
    if (f) {
        fread(magic, 1, 4, f);
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    if (memcmp(magic, "3DSX", 4) != 0 || size < 64 * 1024) {
        remove(tmp);
        setError("Fichier téléchargé invalide");
        return;
    }

    remove(s_target);
    if (rename(tmp, s_target) != 0) {
        setError("Impossible de remplacer l'appli");
        return;
    }
    s_progress = 100;
    s_state = UPD_DONE;
}

static void startThread(ThreadFunc fn)
{
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_thread = threadCreate(fn, NULL, UPD_STACK, prio + 1, -2, false);
    if (!s_thread) setError("Impossible de lancer la mise à jour");
}

void updateInit(void)
{
    s_curlOk = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void updateStartCheck(const char* currentVersion)
{
    if (!s_curlOk) { setError("Erreur interne (curl)"); return; }
    if (s_state == UPD_CHECKING || s_state == UPD_DOWNLOADING) return;
    snprintf(s_current, sizeof(s_current), "%s", currentVersion);
    s_state = UPD_CHECKING;
    startThread(checkThread);
}

void updateStartDownload(const char* targetPath)
{
    if (s_state != UPD_AVAILABLE) return;
    snprintf(s_target, sizeof(s_target), "%s", targetPath);
    s_progress = 0;
    s_state = UPD_DOWNLOADING;
    startThread(downloadThread);
}

UpdState updateState(void) { return s_state; }
const char* updateVersion(void) { return s_version; }
int updateProgress(void) { return s_progress; }
const char* updateError(void) { return s_error; }

void updateExit(void)
{
    s_abort = true;
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    if (s_curlOk) curl_global_cleanup();
}
