/*
 * iq_saver.c
 *
 * Description : IQ frame disk saver — remplace iq_server dans la chaîne DAQ.
 *               Lit les trames CF32 depuis la mémoire partagée du Delay Synchronizer
 *               et les écrit en binaire brut dans des fichiers .dat horodatés.
 *
 * Format de sortie (CF32 interleaved, sans en-tête) :
 *   Par trame : [Re_ch0_s0, Im_ch0_s0, Re_ch0_s1, Im_ch0_s1, ...,
 *                Re_ch1_s0, Im_ch1_s0, ...,
 *                Re_ch(N-1)_s0, Im_ch(N-1)_s0, ...]
 *   Layout : channel-major, sample-minor.
 *   Taille : num_ch * cpi_size * 2 * sizeof(float) octets / trame.
 *
 *   Lecture MATLAB :
 *     fid  = fopen('iq_20250416_143022.dat','rb');
 *     raw  = fread(fid, [2*num_ch, cpi_size], 'float32');   % (2*M x N)
 *     iq   = complex(raw(1:2:end,:), raw(2:2:end,:));        % (M x N)
 *     fclose(fid);
 *
 * Contrôle via FIFO (_data_control/iq_saver_control) :
 *   echo "START" > _data_control/iq_saver_control   → démarre l'enregistrement
 *   echo "STOP"  > _data_control/iq_saver_control   → stoppe et ferme le fichier
 *   echo "EXIT"  > _data_control/iq_saver_control   → termine le processus
 *
 * Ring buffer :
 *   Quand l'espace total dans _daq_raw/ dépasse ring_size_mb,
 *   les fichiers .dat les plus anciens sont supprimés automatiquement.
 *   Quand un segment dépasse max_segment_mb, un nouveau fichier est ouvert.
 *
 * Seules les trames DATA (frame_type=0) avec sample_sync=1 ET iq_sync=1 sont
 * enregistrées — les trames de calibration et dummy sont ignorées.
 *
 * Project : HeIMDALL DAQ Firmware — KrakenSDR variant
 * License : GNU GPL V3
 * Author  : (adapté de iq_server.c par Tamas Peto)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <dirent.h>
#include <errno.h>

#include "ini.h"
#include "log.h"
#include "sh_mem_util.h"
#include "iq_header.h"
#include "rtl_daq.h"

/* ─── Constantes ─────────────────────────────────────────────────────────── */
#define INI_FNAME           "daq_chain_config.ini"
#define SAVER_CTL_FIFO      "_data_control/iq_saver_control"
#define DEFAULT_OUTPUT_DIR  "_daq_raw"
#define MAX_PATH_LEN        512
#define MAX_DAT_FILES       4096   /* max fichiers .dat scannés pour ring mgmt */

/* Types de trames (définis dans iq_header.h — redéfinis en garde) */
#ifndef FRAME_TYPE_DATA
#define FRAME_TYPE_DATA  0
#endif
#ifndef FRAME_TYPE_DUMMY
#define FRAME_TYPE_DUMMY 1
#endif

#define FATAL_ERR(l) log_fatal(l); return -1;

/* ─── Configuration ──────────────────────────────────────────────────────── */
typedef struct {
    int  num_ch;
    int  cpi_size;
    int  log_level;
    int  ring_size_mb;      /* espace disque max dans output_dir (MB)          */
    int  max_segment_mb;    /* taille max d'un fichier .dat (MB)               */
    int  auto_start;        /* 1 = démarre l'enregistrement sans attendre START */
    char output_dir[MAX_PATH_LEN];
} configuration;

static configuration g_cfg;

/* ─── État global ────────────────────────────────────────────────────────── */
static volatile int       g_recording = 0;
static volatile int       g_running   = 1;
static FILE*              g_out_file  = NULL;
static long long          g_seg_bytes = 0;
static pthread_mutex_t    g_file_mtx  = PTHREAD_MUTEX_INITIALIZER;

/* ─── Parseur INI ────────────────────────────────────────────────────────── */
static int ini_handler(void* conf, const char* section,
                       const char* name, const char* value)
{
    configuration* c = (configuration*) conf;
    #define MATCH(s,n) (strcmp(section,(s))==0 && strcmp(name,(n))==0)
    if      (MATCH("hw",            "num_ch"))         c->num_ch         = atoi(value);
    else if (MATCH("pre_processing","cpi_size"))        c->cpi_size       = atoi(value);
    else if (MATCH("daq",           "log_level"))       c->log_level      = atoi(value);
    else if (MATCH("iq_saver",      "ring_size_mb"))    c->ring_size_mb   = atoi(value);
    else if (MATCH("iq_saver",      "max_segment_mb"))  c->max_segment_mb = atoi(value);
    else if (MATCH("iq_saver",      "auto_start"))      c->auto_start     = atoi(value);
    else if (MATCH("iq_saver",      "output_dir"))
        strncpy(c->output_dir, value, MAX_PATH_LEN - 1);
    return 0;  /* champs inconnus ignorés silencieusement */
}

/* ─── Gestion du ring buffer ─────────────────────────────────────────────── */

/* Compare deux noms de fichiers pour qsort (ordre lexicographique = chronologique
   car les noms sont de la forme iq_YYYYMMDD_HHMMSS.dat) */
static int cmp_fname(const void* a, const void* b)
{
    return strcmp(*(const char**)a, *(const char**)b);
}

/*
 * Scanne output_dir, calcule l'espace total occupé par les .dat,
 * supprime les fichiers les plus anciens jusqu'à repasser sous ring_size_mb.
 */
static void ring_enforce(void)
{
    DIR* d = opendir(g_cfg.output_dir);
    if (!d) {
        log_warn("iq_saver: cannot open output dir for ring check: %s", strerror(errno));
        return;
    }

    static char* names[MAX_DAT_FILES];
    int   n_files    = 0;
    long long total  = 0;
    struct dirent* ent;
    struct stat    st;
    char path[MAX_PATH_LEN];

    while ((ent = readdir(d)) != NULL && n_files < MAX_DAT_FILES) {
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcmp(ent->d_name + len - 4, ".dat") != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", g_cfg.output_dir, ent->d_name);
        if (stat(path, &st) == 0) {
            total += (long long)st.st_size;
            names[n_files++] = strdup(ent->d_name);
        }
    }
    closedir(d);

    /* Tri par nom → ordre chronologique */
    qsort(names, n_files, sizeof(char*), cmp_fname);

    long long limit = (long long)g_cfg.ring_size_mb * 1024 * 1024;
    int i = 0;
    while (total > limit && i < n_files) {
        snprintf(path, sizeof(path), "%s/%s", g_cfg.output_dir, names[i]);
        struct stat st2;
        if (stat(path, &st2) == 0) {
            if (remove(path) == 0) {
                log_info("iq_saver: ring — deleted %s (freed %.1f MB)",
                         names[i], st2.st_size / 1048576.0);
                total -= (long long)st2.st_size;
            } else {
                log_warn("iq_saver: ring — cannot delete %s: %s",
                         names[i], strerror(errno));
            }
        }
        i++;
    }

    for (int j = 0; j < n_files; j++) free(names[j]);
    log_info("iq_saver: ring check done — %.1f MB / %d MB used",
             total / 1048576.0, g_cfg.ring_size_mb);
}

/* Ouvre un nouveau segment horodaté. Retourne 0 si OK, -1 si erreur. */
static int open_segment(void)
{
    char path[MAX_PATH_LEN];
    time_t now = time(NULL);
    struct tm* t = localtime(&now);

    snprintf(path, sizeof(path),
             "%s/iq_%04d%02d%02d_%02d%02d%02d.dat",
             g_cfg.output_dir,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    g_out_file = fopen(path, "wb");
    if (!g_out_file) {
        log_error("iq_saver: cannot open segment %s: %s", path, strerror(errno));
        return -1;
    }
    g_seg_bytes = 0;
    log_info("iq_saver: opened segment → %s", path);
    return 0;
}

/* Ferme le segment courant et applique le ring buffer. */
static void close_segment(void)
{
    if (!g_out_file) return;
    fflush(g_out_file);
    fclose(g_out_file);
    g_out_file = NULL;
    log_info("iq_saver: segment closed (%.2f MB)", g_seg_bytes / 1048576.0);
    ring_enforce();
}

/* ─── Thread de contrôle FIFO ────────────────────────────────────────────── */
/*
 * Attend des commandes texte sur SAVER_CTL_FIFO.
 * Utilise select() avec timeout pour pouvoir vérifier g_running périodiquement.
 */
static void* control_thread(void* arg)
{
    (void)arg;
    char cmd[64];

    /*
     * O_RDWR : maintient le FIFO ouvert même sans writer côté bash,
     * ce qui évite un EOF permanent au premier read().
     */
    int fd = open(SAVER_CTL_FIFO, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        log_warn("iq_saver: cannot open control FIFO %s: %s",
                 SAVER_CTL_FIFO, strerror(errno));
        return NULL;
    }

    /* Repasser en mode bloquant — select() gérera le timeout */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    log_info("iq_saver: control FIFO ready — send START | STOP | EXIT");

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 }; /* 200 ms */

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            log_error("iq_saver: select() error on control FIFO: %s", strerror(errno));
            break;
        }
        if (ret == 0) continue;  /* timeout — reboucle pour vérifier g_running */

        memset(cmd, 0, sizeof(cmd));
        ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        if (n <= 0) {
            usleep(10000);
            continue;
        }
        /* Nettoyage du newline */
        for (int i = 0; i < n; i++)
            if (cmd[i] == '\n' || cmd[i] == '\r') cmd[i] = '\0';

        log_info("iq_saver: command received → [%s]", cmd);

        if (strcmp(cmd, "START") == 0) {
            pthread_mutex_lock(&g_file_mtx);
            if (!g_recording) {
                if (open_segment() == 0)
                    g_recording = 1;
                else
                    log_error("iq_saver: START failed — cannot open segment");
            } else {
                log_warn("iq_saver: already recording, START ignored");
            }
            pthread_mutex_unlock(&g_file_mtx);
        }
        else if (strcmp(cmd, "STOP") == 0) {
            pthread_mutex_lock(&g_file_mtx);
            if (g_recording) {
                g_recording = 0;
                close_segment();
            } else {
                log_warn("iq_saver: not recording, STOP ignored");
            }
            pthread_mutex_unlock(&g_file_mtx);
        }
        else if (strcmp(cmd, "EXIT") == 0) {
            g_running = 0;
        }
        else {
            log_warn("iq_saver: unknown command [%s] — valid: START | STOP | EXIT", cmd);
        }
    }

    close(fd);
    return NULL;
}

/* ─── Gestionnaire de signaux ────────────────────────────────────────────── */
static void sighandler(int sig)
{
    log_info("iq_saver: signal %d received — shutting down gracefully", sig);
    g_running = 0;
}

/* ─── Main ───────────────────────────────────────────────────────────────── */
int main(int argc, char* argv[])
{
    (void)argc; (void)argv;

    /* Valeurs par défaut (overridées par le .ini si [iq_saver] est présent) */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.ring_size_mb   = 2048;
    g_cfg.max_segment_mb = 512;
    g_cfg.auto_start     = 1;
    strncpy(g_cfg.output_dir, DEFAULT_OUTPUT_DIR, MAX_PATH_LEN - 1);

    log_set_level(LOG_TRACE);
    signal(SIGINT,  sighandler);
    signal(SIGTERM, sighandler);

    /* Chargement de la configuration */
    if (ini_parse(INI_FNAME, ini_handler, &g_cfg) < 0) {
        FATAL_ERR("iq_saver: configuration could not be loaded, exiting");
    }
    log_set_level(g_cfg.log_level);

    log_info("iq_saver: num_ch=%d  cpi_size=%d  ring=%dMB  segment=%dMB  auto_start=%d",
             g_cfg.num_ch, g_cfg.cpi_size,
             g_cfg.ring_size_mb, g_cfg.max_segment_mb, g_cfg.auto_start);
    log_info("iq_saver: frame size = %.1f MB",
             (double)g_cfg.num_ch * g_cfg.cpi_size * 2 * sizeof(float) / 1048576.0);

    /* Création du dossier de sortie */
    if (mkdir(g_cfg.output_dir, 0755) < 0 && errno != EEXIST) {
        log_error("iq_saver: cannot create output dir %s: %s",
                  g_cfg.output_dir, strerror(errno));
        return -1;
    }

    /* Création du FIFO de contrôle */
    mkfifo(SAVER_CTL_FIFO, 0666);

    /* Démarrage du thread de contrôle */
    pthread_t ctl_tid;
    if (pthread_create(&ctl_tid, NULL, control_thread, NULL) != 0) {
        log_warn("iq_saver: could not create control thread — FIFO control disabled");
    }

    /* ── Initialisation de la mémoire partagée (identique à iq_server.c) ── */
    struct iq_frame_struct_32* iq_frame =
        calloc(1, sizeof(struct iq_frame_struct_32));
    struct shmem_transfer_struct* sm =
        calloc(1, sizeof(struct shmem_transfer_struct));

    sm->shared_memory_size =
        MAX_IQFRAME_PAYLOAD_SIZE * g_cfg.num_ch * 4 * 2 + IQ_HEADER_LENGTH;
    sm->io_type = 1;  /* input */

    strcpy(sm->shared_memory_names[0], DELAY_SYNC_IQ_SM_NAME_A);
    strcpy(sm->shared_memory_names[1], DELAY_SYNC_IQ_SM_NAME_B);
    strcpy(sm->fw_ctr_fifo_name,       DELAY_SYNC_IQ_FW_FIFO);
    strcpy(sm->bw_ctr_fifo_name,       DELAY_SYNC_IQ_BW_FIFO);

    int ret = init_in_sm_buffer(sm);
    if (ret != 0) {
        FATAL_ERR("iq_saver: failed to init shared memory interface");
    }
    log_info("iq_saver: shared memory interface initialized");

    /* Auto-start : ouvre un premier segment immédiatement */
    if (g_cfg.auto_start) {
        pthread_mutex_lock(&g_file_mtx);
        if (open_segment() == 0)
            g_recording = 1;
        pthread_mutex_unlock(&g_file_mtx);
        log_info("iq_saver: auto-start — recording in progress");
    } else {
        log_info("iq_saver: waiting for START command on %s", SAVER_CTL_FIFO);
    }

    /* ── Boucle principale d'acquisition ──────────────────────────────────── */
    long long frames_written  = 0;
    long long frames_skipped  = 0;

    while (g_running) {
        int buf_idx = wait_buff_ready(sm);
        if (buf_idx < 0) {
            log_error("iq_saver: wait_buff_ready returned %d — exiting loop", buf_idx);
            break;
        }

        /* Reconstruction du frame depuis la mémoire partagée */
        iq_frame->header  = (struct iq_header_struct*) sm->shm_ptr[buf_idx];
        iq_frame->payload = ((float*) sm->shm_ptr[buf_idx])
                            + IQ_HEADER_LENGTH / sizeof(float);

        /* Vérification du mot de synchronisation */
        CHK_SYNC_WORD(check_sync_word(iq_frame->header));

        iq_frame->payload_size =
            iq_frame->header->cpi_length * iq_frame->header->active_ant_chs;

        /* ── Filtrage : uniquement trames DATA synchronisées ──────────────── */
        int frame_type  = iq_frame->header->frame_type;
        int samp_sync   = iq_frame->header->sample_sync_flag;
        int iq_sync_ok  = iq_frame->header->iq_sync_flag;

        if (!g_recording || frame_type != FRAME_TYPE_DATA || !samp_sync || !iq_sync_ok) {
            if (g_recording && frame_type == FRAME_TYPE_DATA)
                frames_skipped++;
            send_ctr_buff_free(sm, buf_idx);
            continue;
        }

        /* ── Écriture CF32 brut (payload uniquement, sans en-tête) ─────────── */
        size_t payload_bytes =
            (size_t)iq_frame->payload_size * sizeof(float) * 2;

        pthread_mutex_lock(&g_file_mtx);
        if (g_out_file) {
            size_t n_written = fwrite(iq_frame->payload, 1,
                                      payload_bytes, g_out_file);
            if (n_written != payload_bytes) {
                log_error("iq_saver: write error (wrote %zu / %zu bytes): %s",
                          n_written, payload_bytes, strerror(errno));
            } else {
                g_seg_bytes += (long long)n_written;
                frames_written++;

                /* Log périodique */
                if (frames_written % 10 == 0)
                    log_info("iq_saver: %lld frames written — segment %.1f MB",
                             frames_written, g_seg_bytes / 1048576.0);

                /* Rotation si le segment est plein */
                long long seg_limit =
                    (long long)g_cfg.max_segment_mb * 1024 * 1024;
                if (g_seg_bytes >= seg_limit) {
                    log_info("iq_saver: segment full — rotating");
                    close_segment();
                    if (open_segment() != 0) {
                        log_error("iq_saver: cannot open new segment — recording paused");
                        g_recording = 0;
                    }
                }
            }
        }
        pthread_mutex_unlock(&g_file_mtx);

        /* Libération du buffer mémoire partagée */
        send_ctr_buff_free(sm, buf_idx);
    }

    /* ── Nettoyage ────────────────────────────────────────────────────────── */
    log_info("iq_saver: shutting down — %lld frames written, %lld skipped",
             frames_written, frames_skipped);

    pthread_mutex_lock(&g_file_mtx);
    g_recording = 0;
    close_segment();
    pthread_mutex_unlock(&g_file_mtx);

    destory_sm_buffer(sm);   /* typo conservé pour compatibilité avec sh_mem_util */
    free(iq_frame);
    free(sm);

    pthread_join(ctl_tid, NULL);
    log_info("iq_saver: exited cleanly.");
    return 0;
}
