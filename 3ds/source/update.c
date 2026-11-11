/*
 * Mise à jour de l'appli 3DS depuis les releases GitHub (libcurl + mbedTLS).
 *
 * La compilation GitHub range chaque version dans la branche "builds" du dépôt
 * (version.txt + ScreenLink3DS.3dsx). On les lit par l'API "contents" de
 * api.github.com, qui renvoie le fichier directement : pas de redirection vers
 * le stockage Azure des releases, que la 3DS n'arrive pas à télécharger.
 *
 * 1. Lit version.txt et compare à la version en cours.
 * 2. Télécharge le .3dsx en mémoire, vérifie l'en-tête 3DSX,
 *    l'écrit à côté de l'appli (.new) puis remplace l'ancien fichier.
 */
#include <3ds.h>
#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update.h"

#define UPD_REPO   "Heivoli-dev/3ds-screen-link"
#define UPD_RAW(f) "https://api.github.com/repos/" UPD_REPO "/contents/" f "?ref=builds"
#define UPD_VERSION_URL UPD_RAW("version.txt")
#define UPD_FILE_URL    UPD_RAW("ScreenLink3DS.3dsx")
#define UPD_MAX_SIZE    (16 * 1024 * 1024)
#define UPD_STACK  (256 * 1024)

static volatile UpdState s_state = UPD_IDLE;
static volatile int s_progress = 0;
static volatile u32 s_bytes = 0, s_total = 0;
static int s_core = -2;
static volatile bool s_abort = false;
static char s_current[32];
static char s_version[32];
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
        if (cap > UPD_MAX_SIZE) return 0;             /* réponse anormalement grosse */
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
    s_bytes = (u32)dlnow;
    if (dltotal > 0) {
        s_total = (u32)dltotal;
        s_progress = (int)(dlnow * 100 / dltotal);
    }
    return s_abort ? 1 : 0;                           /* non nul = annuler */
}

static struct curl_slist* s_headers = NULL;

static CURL* makeCurl(const char* url)
{
    CURL* c = curl_easy_init();
    if (!c) return NULL;
    if (!s_headers) {
        s_headers = curl_slist_append(NULL, "Accept: application/vnd.github.raw");
        s_headers = curl_slist_append(s_headers, "X-GitHub-Api-Version: 2022-11-28");
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, s_headers);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "ScreenLink3DS");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 100L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64 * 1024L);
    /* La 3DS n'a pas de magasin de certificats utilisable par curl */
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    return c;
}

static void parseVersion(const char* v, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    while (*v == ' ' || *v == '\n' || *v == '\r') v++;
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

/* Télécharge une URL en mémoire. Renvoie CURLE_OK ou le code d'erreur. */
static CURLcode fetch(const char* url, MemBuf* m, bool progress, long* httpCode)
{
    CURL* c = makeCurl(url);
    if (!c) return CURLE_FAILED_INIT;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, memWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, m);
    if (!progress) curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
    CURLcode rc = curl_easy_perform(c);
    *httpCode = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, httpCode);
    curl_easy_cleanup(c);
    return rc;
}

static void setCurlError(const char* what, CURLcode rc, long http)
{
    char msg[96];
    if (rc == CURLE_HTTP_RETURNED_ERROR)
        snprintf(msg, sizeof(msg), "%s (HTTP %ld)", what, http);
    else
        snprintf(msg, sizeof(msg), "%s (%s)", what, curl_easy_strerror(rc));
    setError(msg);
}

static void checkThread(void* arg)
{
    (void)arg;
    MemBuf m = {0};
    long http = 0;
    CURLcode rc = fetch(UPD_VERSION_URL, &m, false, &http);
    if (rc != CURLE_OK || !m.data) {
        free(m.data);
        setCurlError("GitHub injoignable", rc, http);
        return;
    }

    int v[3];
    parseVersion(m.data, v);
    snprintf(s_version, sizeof(s_version), "%d.%d.%d", v[0], v[1], v[2]);
    free(m.data);

    s_state = isNewer(s_version, s_current) ? UPD_AVAILABLE : UPD_UP_TO_DATE;
}

static void downloadThread(void* arg)
{
    (void)arg;
    MemBuf m = {0};
    long http = 0;
    CURLcode rc = fetch(UPD_FILE_URL, &m, true, &http);
    if (rc != CURLE_OK || !m.data) {
        free(m.data);
        if (s_abort) setError("Téléchargement annulé");
        else setCurlError("Téléchargement échoué", rc, http);
        return;
    }

    /* Vérifie que c'est bien un exécutable 3DSX complet avant de remplacer */
    if (m.len < 64 * 1024 || memcmp(m.data, "3DSX", 4) != 0) {
        free(m.data);
        setError("Fichier téléchargé invalide");
        return;
    }

    char tmp[272];
    snprintf(tmp, sizeof(tmp), "%s.new", s_target);
    FILE* f = fopen(tmp, "wb");
    if (!f) {
        free(m.data);
        setError("Impossible d'écrire sur la carte SD");
        return;
    }
    size_t written = fwrite(m.data, 1, m.len, f);
    fclose(f);
    size_t expected = m.len;
    free(m.data);
    if (written != expected) {
        remove(tmp);
        setError("Carte SD pleine ?");
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
    /* Sur l'autre cœur que l'interface : le TLS est gourmand en calcul */
    s_thread = threadCreate(fn, NULL, UPD_STACK, prio + 1, s_core, false);
    if (!s_thread && s_core != -2)
        s_thread = threadCreate(fn, NULL, UPD_STACK, prio + 1, -2, false);
    if (!s_thread) setError("Impossible de lancer la mise à jour");
}

void updateInit(int core)
{
    s_core = core;
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
    s_bytes = s_total = 0;
    s_state = UPD_DOWNLOADING;
    startThread(downloadThread);
}

UpdState updateState(void) { return s_state; }
const char* updateVersion(void) { return s_version; }
int updateProgress(void) { return s_progress; }
unsigned updateBytes(void) { return s_bytes; }
unsigned updateTotal(void) { return s_total; }
const char* updateError(void) { return s_error; }

void updateExit(void)
{
    s_abort = true;
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    if (s_headers) curl_slist_free_all(s_headers);
    if (s_curlOk) curl_global_cleanup();
}
