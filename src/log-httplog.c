/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * Implements http logging portion of the engine.
 */

#include <htp/dslib.h>

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "pkt-var.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-print.h"
#include "util-unittest.h"

#include "util-debug.h"

#include "output.h"
#include "log-httplog.h"
#include "app-layer-htp.h"
#include "app-layer.h"
#include "util-privs.h"

#define DEFAULT_LOG_FILENAME "http.log"

#define MODULE_NAME "LogHttpLog"

TmEcode LogHttpLog (ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode LogHttpLogIPv4(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode LogHttpLogIPv6(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode LogHttpLogThreadInit(ThreadVars *, void *, void **);
TmEcode LogHttpLogThreadDeinit(ThreadVars *, void *);
void LogHttpLogExitPrintStats(ThreadVars *, void *);
int LogHttpLogOpenFileCtx(LogFileCtx* , const char *, const char *);
static void LogHttpLogDeInitCtx(OutputCtx *);

void TmModuleLogHttpLogRegister (void) {
    tmm_modules[TMM_LOGHTTPLOG].name = MODULE_NAME;
    tmm_modules[TMM_LOGHTTPLOG].ThreadInit = LogHttpLogThreadInit;
    tmm_modules[TMM_LOGHTTPLOG].Func = LogHttpLog;
    tmm_modules[TMM_LOGHTTPLOG].ThreadExitPrintStats = LogHttpLogExitPrintStats;
    tmm_modules[TMM_LOGHTTPLOG].ThreadDeinit = LogHttpLogThreadDeinit;
    tmm_modules[TMM_LOGHTTPLOG].RegisterTests = NULL;
    tmm_modules[TMM_LOGHTTPLOG].cap_flags = 0;

    OutputRegisterModule(MODULE_NAME, "http-log", LogHttpLogInitCtx);
}

void TmModuleLogHttpLogIPv4Register (void) {
    tmm_modules[TMM_LOGHTTPLOG4].name = "LogHttpLogIPv4";
    tmm_modules[TMM_LOGHTTPLOG4].ThreadInit = LogHttpLogThreadInit;
    tmm_modules[TMM_LOGHTTPLOG4].Func = LogHttpLogIPv4;
    tmm_modules[TMM_LOGHTTPLOG4].ThreadExitPrintStats = LogHttpLogExitPrintStats;
    tmm_modules[TMM_LOGHTTPLOG4].ThreadDeinit = LogHttpLogThreadDeinit;
    tmm_modules[TMM_LOGHTTPLOG4].RegisterTests = NULL;
}

void TmModuleLogHttpLogIPv6Register (void) {
    tmm_modules[TMM_LOGHTTPLOG6].name = "LogHttpLogIPv6";
    tmm_modules[TMM_LOGHTTPLOG6].ThreadInit = LogHttpLogThreadInit;
    tmm_modules[TMM_LOGHTTPLOG6].Func = LogHttpLogIPv6;
    tmm_modules[TMM_LOGHTTPLOG6].ThreadExitPrintStats = LogHttpLogExitPrintStats;
    tmm_modules[TMM_LOGHTTPLOG6].ThreadDeinit = LogHttpLogThreadDeinit;
    tmm_modules[TMM_LOGHTTPLOG6].RegisterTests = NULL;
}

typedef struct LogHttpLogThread_ {
    LogFileCtx *file_ctx;
    /** LogFileCtx has the pointer to the file and a mutex to allow multithreading */
    uint32_t uri_cnt;
} LogHttpLogThread;

static void CreateTimeString (const struct timeval *ts, char *str, size_t size) {
    time_t time = ts->tv_sec;
    struct tm local_tm;
    struct tm *t = (struct tm *)localtime_r(&time, &local_tm);

    snprintf(str, size, "%02d/%02d/%02d-%02d:%02d:%02d.%06u",
        t->tm_mon + 1, t->tm_mday, t->tm_year + 1900, t->tm_hour,
            t->tm_min, t->tm_sec, (uint32_t) ts->tv_usec);
}

TmEcode LogHttpLogIPv4(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    SCEnter();
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    char timebuf[64];
    size_t idx = 0;

    /* no flow, no htp state */
    if (p->flow == NULL) {
        SCReturnInt(TM_ECODE_OK);
    }

    /* check if we have HTTP state or not */
#ifdef __tile__
    tmc_spin_queued_mutex_lock(&p->flow->m);
#else
    SCMutexLock(&p->flow->m);
#endif
    uint16_t proto = AppLayerGetProtoFromPacket(p);
    if (proto != ALPROTO_HTTP)
        goto end;

    int r = AppLayerTransactionGetLoggedId(p->flow);
    if (r < 0) {
        goto end;
    }
    size_t logged = (size_t)r;

    r = AppLayerTransactionGetLoggableId(p->flow);
    if (r < 0) {
        goto end;
    }
    size_t loggable = (size_t)r;

    HtpState *htp_state = (HtpState *)AppLayerGetProtoStateFromPacket(p);
    if (htp_state == NULL) {
        SCLogDebug("no http state, so no request logging");
        goto end;
    }

    if (htp_state->connp == NULL)
        goto end;

    htp_tx_t *tx = NULL;

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    char srcip[16], dstip[16];
    Port sp, dp;
    if ((PKT_IS_TOSERVER(p))) {
        PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), srcip, sizeof(srcip));
        PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), dstip, sizeof(dstip));
        sp = p->sp;
        dp = p->dp;
    } else {
        PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), srcip, sizeof(srcip));
        PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), dstip, sizeof(dstip));
        sp = p->dp;
        dp = p->sp;
    }

#ifdef __tile__
    tmc_spin_queued_mutex_lock(&aft->file_ctx->fp_mutex);
#else
    SCMutexLock(&aft->file_ctx->fp_mutex);
#endif
    for (idx = logged; idx < loggable; idx++)
    {
        tx = list_get(htp_state->connp->conn->transactions, idx);
        if (tx == NULL) {
            SCLogDebug("tx is NULL not logging !!");
            continue;
        }

        SCLogDebug("got a HTTP request and now logging !!");
        /* time */
        fprintf(aft->file_ctx->fp, "%s ", timebuf);

        /* hostname */
        if (tx->parsed_uri != NULL &&
                tx->parsed_uri->hostname != NULL)
        {
            PrintRawUriFp(aft->file_ctx->fp,
                    (uint8_t *)bstr_ptr(tx->parsed_uri->hostname),
                    bstr_len(tx->parsed_uri->hostname));
        } else {
            fprintf(aft->file_ctx->fp, "<hostname unknown>");
        }
        fprintf(aft->file_ctx->fp, " [**] ");

        /* uri */
        if (tx->request_uri != NULL) {
            PrintRawUriFp(aft->file_ctx->fp,
                            (uint8_t *)bstr_ptr(tx->request_uri),
                            bstr_len(tx->request_uri));
        }
        fprintf(aft->file_ctx->fp, " [**] ");

        /* user agent */
        htp_header_t *h_user_agent = table_getc(tx->request_headers, "user-agent");
        if (h_user_agent != NULL) {
            PrintRawUriFp(aft->file_ctx->fp,
                            (uint8_t *)bstr_ptr(h_user_agent->value),
                            bstr_len(h_user_agent->value));
        } else {
            fprintf(aft->file_ctx->fp, "<useragent unknown>");
        }

        /* ip/tcp header info */
        fprintf(aft->file_ctx->fp, " [**] %s:%" PRIu16 " -> %s:%" PRIu16 "\n",
                srcip, sp, dstip, dp);

        aft->uri_cnt ++;

        AppLayerTransactionUpdateLoggedId(p->flow);
    }
    fflush(aft->file_ctx->fp);
#ifdef __tile__
    tmc_spin_queued_mutex_unlock(&aft->file_ctx->fp_mutex);
#else
    SCMutexUnlock(&aft->file_ctx->fp_mutex);
#endif

end:
#ifdef __tile__
    tmc_spin_queued_mutex_unlock(&p->flow->m);
#else
    SCMutexUnlock(&p->flow->m);
#endif
    SCReturnInt(TM_ECODE_OK);
}

TmEcode LogHttpLogIPv6(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    SCEnter();
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    char timebuf[64];
    size_t idx = 0;

    /* no flow, no htp state */
    if (p->flow == NULL) {
        SCReturnInt(TM_ECODE_OK);
    }

    /* check if we have HTTP state or not */
#ifdef __tile__
    tmc_spin_queued_mutex_lock(&p->flow->m);
#else
    SCMutexLock(&p->flow->m);
#endif
    uint16_t proto = AppLayerGetProtoFromPacket(p);
    if (proto != ALPROTO_HTTP)
        goto end;

    int r = AppLayerTransactionGetLoggedId(p->flow);
    if (r < 0) {
        goto end;
    }
    size_t logged = (size_t)r;

    r = AppLayerTransactionGetLoggableId(p->flow);
    if (r < 0) {
        goto end;
    }
    size_t loggable = (size_t)r;

    HtpState *htp_state = (HtpState *)AppLayerGetProtoStateFromPacket(p);
    if (htp_state == NULL) {
        SCLogDebug("no http state, so no request logging");
        goto end;
    }

    if (htp_state->connp == NULL)
        goto end;

    htp_tx_t *tx = NULL;

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    char srcip[46], dstip[46];
    Port sp, dp;
    if ((PKT_IS_TOSERVER(p))) {
        PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
        PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));
        sp = p->sp;
        dp = p->dp;
    } else {
        PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
        PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));
        sp = p->dp;
        dp = p->sp;
    }
#ifdef __tile__
    tmc_spin_queued_mutex_lock(&aft->file_ctx->fp_mutex);
#else
    SCMutexLock(&aft->file_ctx->fp_mutex);
#endif
    for (idx = logged; idx < loggable; idx++)
    {
        tx = list_get(htp_state->connp->conn->transactions, idx);
        if (tx == NULL) {
            SCLogDebug("tx is NULL not logging !!");
            continue;
        }

        SCLogDebug("got a HTTP request and now logging !!");
        /* time */
        fprintf(aft->file_ctx->fp, "%s ", timebuf);

        /* hostname */
        if (tx->parsed_uri != NULL &&
                tx->parsed_uri->hostname != NULL)
        {
            PrintRawUriFp(aft->file_ctx->fp,
                    (uint8_t *)bstr_ptr(tx->parsed_uri->hostname),
                    bstr_len(tx->parsed_uri->hostname));
        } else {
            fprintf(aft->file_ctx->fp, "<hostname unknown>");
        }
        fprintf(aft->file_ctx->fp, " [**] ");

        /* uri */
        if (tx->request_uri != NULL) {
            PrintRawUriFp(aft->file_ctx->fp,
                            (uint8_t *)bstr_ptr(tx->request_uri),
                            bstr_len(tx->request_uri));
        }
        fprintf(aft->file_ctx->fp, " [**] ");

        /* user agent */
        htp_header_t *h_user_agent = table_getc(tx->request_headers, "user-agent");
        if (h_user_agent != NULL) {
            PrintRawUriFp(aft->file_ctx->fp,
                            (uint8_t *)bstr_ptr(h_user_agent->value),
                            bstr_len(h_user_agent->value));
        } else {
            fprintf(aft->file_ctx->fp, "<useragent unknown>");
        }

        /* ip/tcp header info */
        fprintf(aft->file_ctx->fp, " [**] %s:%" PRIu16 " -> %s:%" PRIu16 "\n",
                srcip, sp, dstip, dp);

        aft->uri_cnt++;

        AppLayerTransactionUpdateLoggedId(p->flow);
    }
    fflush(aft->file_ctx->fp);
#ifdef __tile__
    tmc_spin_queued_mutex_unlock(&aft->file_ctx->fp_mutex);
#else
    SCMutexUnlock(&aft->file_ctx->fp_mutex);
#endif

end:
#ifdef __tile__
    tmc_spin_queued_mutex_unlock(&p->flow->m);
#else
    SCMutexUnlock(&p->flow->m);
#endif
    SCReturnInt(TM_ECODE_OK);
}

TmEcode LogHttpLog (ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    SCEnter();

    /* no flow, no htp state */
    if (p->flow == NULL) {
        SCReturnInt(TM_ECODE_OK);
    }

    if (!(PKT_IS_TCP(p))) {
        SCReturnInt(TM_ECODE_OK);
    }

    if (PKT_IS_IPV4(p)) {
        SCReturnInt(LogHttpLogIPv4(tv, p, data, pq, postpq));
    } else if (PKT_IS_IPV6(p)) {
        SCReturnInt(LogHttpLogIPv6(tv, p, data, pq, postpq));
    }

    SCReturnInt(TM_ECODE_OK);
}

TmEcode LogHttpLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    LogHttpLogThread *aft = SCMalloc(sizeof(LogHttpLogThread));
    if (aft == NULL)
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(LogHttpLogThread));

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for HTTPLog.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }
    /* Use the Ouptut Context (file pointer and mutex) */
    aft->file_ctx= ((OutputCtx *)initdata)->data;

    /* enable the logger for the app layer */
    AppLayerRegisterLogger(ALPROTO_HTTP);

    *data = (void *)aft;
    return TM_ECODE_OK;
}

TmEcode LogHttpLogThreadDeinit(ThreadVars *t, void *data)
{
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    /* clear memory */
    memset(aft, 0, sizeof(LogHttpLogThread));

    SCFree(aft);
    return TM_ECODE_OK;
}

void LogHttpLogExitPrintStats(ThreadVars *tv, void *data) {
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    if (aft == NULL) {
        return;
    }

    SCLogInfo("(%s) HTTP requests %" PRIu32 "", tv->name, aft->uri_cnt);
}

/** \brief Create a new http log LogFileCtx.
 *  \param conf Pointer to ConfNode containing this loggers configuration.
 *  \return NULL if failure, LogFileCtx* to the file_ctx if succesful
 * */
OutputCtx *LogHttpLogInitCtx(ConfNode *conf)
{
    int ret=0;

    LogFileCtx* file_ctx=LogFileNewCtx();

    if(file_ctx == NULL)
    {
        SCLogError(SC_ERR_HTTP_LOG_GENERIC, "LogHttpLogInitCtx: Couldn't "
                   "create new file_ctx");
        return NULL;
    }

    const char *filename = ConfNodeLookupChildValue(conf, "filename");
    if (filename == NULL)
        filename = DEFAULT_LOG_FILENAME;

    const char *mode = ConfNodeLookupChildValue(conf, "append");
    if (mode == NULL)
        mode = DEFAULT_LOG_MODE_APPEND;
    /** fill the new LogFileCtx with the specific LogHttpLog configuration */
    ret=LogHttpLogOpenFileCtx(file_ctx, filename, mode);

    if(ret < 0)
        return NULL;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (output_ctx == NULL)
        return NULL;
    output_ctx->data = file_ctx;
    output_ctx->DeInit = LogHttpLogDeInitCtx;

    SCLogInfo("HTTP log output initialized, filename: %s", filename);

    return output_ctx;
}

static void LogHttpLogDeInitCtx(OutputCtx *output_ctx)
{
    LogFileCtx *logfile_ctx = (LogFileCtx *)output_ctx->data;
    LogFileFreeCtx(logfile_ctx);
    SCFree(output_ctx);
}

/** \brief Read the config set the file pointer, open the file
 *  \param file_ctx pointer to a created LogFileCtx using LogFileNewCtx()
 *  \param filename name of log file
 *  \param mode append mode (bool)
 *  \return -1 if failure, 0 if succesful
 * */
int LogHttpLogOpenFileCtx(LogFileCtx *file_ctx, const char *filename, const
                            char *mode)
{
    char log_path[PATH_MAX];
    char *log_dir;

    if (ConfGet("default-log-dir", &log_dir) != 1)
        log_dir = DEFAULT_LOG_DIR;

    snprintf(log_path, PATH_MAX, "%s/%s", log_dir, filename);

    if (ConfValIsTrue(mode)) {
        file_ctx->fp = fopen(log_path, "a");
    } else {
        file_ctx->fp = fopen(log_path, "w");
    }

    if (file_ctx->fp == NULL) {
        SCLogError(SC_ERR_FOPEN, "failed to open %s: %s", log_path,
            strerror(errno));
        return -1;
    }

    return 0;
}

