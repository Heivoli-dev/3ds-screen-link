/*
 * Mise à jour de l'appli 3DS depuis les releases GitHub.
 * Tout se passe dans un thread : l'interface lit l'état avec updateState().
 */
#ifndef UPDATE_H
#define UPDATE_H

typedef enum {
    UPD_IDLE,         /* rien de lancé */
    UPD_CHECKING,     /* interrogation de GitHub */
    UPD_UP_TO_DATE,   /* déjà la dernière version */
    UPD_AVAILABLE,    /* nouvelle version trouvée */
    UPD_DOWNLOADING,  /* téléchargement en cours (voir updateProgress) */
    UPD_DONE,         /* installée : il faut relancer l'appli */
    UPD_ERROR,        /* voir updateError */
} UpdState;

void updateInit(int core);  /* cœur du thread : 1 ou 2, ou -2 pour celui par défaut */
void updateStartCheck(const char* currentVersion);
void updateStartDownload(const char* targetPath);
UpdState updateState(void);
const char* updateVersion(void);   /* version trouvée, ex. "1.4.0" */
int updateProgress(void);          /* 0..100 */
unsigned updateBytes(void);        /* octets reçus */
unsigned updateTotal(void);        /* taille totale (0 si inconnue) */
const char* updateError(void);
void updateExit(void);

#endif
