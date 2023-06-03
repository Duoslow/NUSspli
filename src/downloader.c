/***************************************************************************
 * This file is part of NUSspli.                                           *
 * Copyright (c) 2019-2020 Pokes303                                        *
 * Copyright (c) 2020-2023 V10lator <v10lator@myway.de>                    *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 3 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, If not, see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/

#include <wut-fixups.h>

#include <dirent.h>
#include <netinet/tcp.h>

#include <config.h>
#include <crypto.h>
#include <downloader.h>
#include <file.h>
#include <filesystem.h>
#include <input.h>
#include <installer.h>
#include <ioQueue.h>
#include <localisation.h>
#include <menu/utils.h>
#include <queue.h>
#include <renderer.h>
#include <romfs.h>
#include <state.h>
#include <staticMem.h>
#include <thread.h>
#include <ticket.h>
#include <titles.h>
#include <tmd.h>
#include <utils.h>

#include <coreinit/filesystem_fsa.h>
#include <coreinit/memory.h>
#include <coreinit/time.h>
#include <curl/curl.h>
#include <nn/ac/ac_c.h>
#include <nn/result.h>
#include <nsysnet/_socket.h>

#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#define USERAGENT        "NUSspli/" NUSSPLI_VERSION
#define DLT_STACK_SIZE   0x8000
#define SMOOTHING_FACTOR 0.2f

static CURL *curl;
static char curlError[CURL_ERROR_SIZE];
static bool curlReuseConnection = true;

static void *cancelOverlay = NULL;

typedef struct
{
    bool running;
    CURLcode error;
    spinlock lock;
    OSTick ts;
    curl_off_t dltotal;
    curl_off_t dlnow;
} curlProgressData;

#define closeCancelOverlay()               \
    {                                      \
        removeErrorOverlay(cancelOverlay); \
        cancelOverlay = NULL;              \
    }

static int progressCallback(void *rawData, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    curlProgressData *data = (curlProgressData *)rawData;
    if(!AppRunning(false))
        data->error = CURLE_ABORTED_BY_CALLBACK;

    if(data->error != CURLE_OK)
        return 1;

    OSTick t = OSGetTick();
    if(spinTryLock(data->lock))
    {
        data->ts = t;
        data->dltotal = dltotal;
        data->dlnow = dlnow;
        spinReleaseLock(data->lock);
    }

    addEntropy(&dlnow, sizeof(curl_off_t));
    addEntropy(&t, sizeof(OSTick));
    return 0;
}

static int initSocket(void *ptr, curl_socket_t socket, curlsocktype type)
{
    int o = 1;

    // Activate WinScale
    int r = setsockopt(socket, SOL_SOCKET, SO_WINSCALE, &o, sizeof(o));
    if(r != 0)
    {
        debugPrintf("initSocket: Error settings WinScale: %d", r);
        return CURL_SOCKOPT_ERROR;
    }

    // Activate TCP SAck
    r = setsockopt(socket, SOL_SOCKET, SO_TCPSACK, &o, sizeof(o));
    if(r != 0)
    {
        debugPrintf("initSocket: Error settings TCP SAck: %d", r);
        return CURL_SOCKOPT_ERROR;
    }

    // Activate TCP nodelay - libCURL default
    r = setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &o, sizeof(o));
    if(r != 0)
    {
        debugPrintf("initSocket: Error settings TCP nodelay: %d", r);
        return CURL_SOCKOPT_ERROR;
    }

    // Disable slowstart. Should be more important fo a server but doesn't hurt a client, too
    r = setsockopt(socket, SOL_SOCKET, 0x4000, &o, sizeof(o));
    if(r != 0)
    {
        debugPrintf("initSocket: Error settings Noslowstart: %d", r);
        return CURL_SOCKOPT_ERROR;
    }

    o = 0;
    // Disable TCP keepalive - libCURL default
    r = setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &o, sizeof(o));
    if(r != 0)
    {
        debugPrintf("initSocket: Error settings TCP nodelay: %d", r);
        return CURL_SOCKOPT_ERROR;
    }

    o = IO_BUFSIZE;
    // Set send buffersize
    r = setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &o, sizeof(o));
    if(r != 0)
    {
        debugPrintf("initSocket: Error settings SBS: %d", r);
        return CURL_SOCKOPT_ERROR;
    }

    // Set receive buffersize
    r = setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &o, sizeof(o));
    if(r != 0)
    {
        debugPrintf("initSocket: Error settings RBS: %d", r);
        return CURL_SOCKOPT_ERROR;
    }

    return CURL_SOCKOPT_OK;
}

static CURLcode ssl_ctx_init(CURL *curl, void *sslctx, void *parm)
{
    mbedtls_ssl_conf_rng((mbedtls_ssl_config *)sslctx, NUSrng, NULL);
    return CURLE_OK;
}

#define initNetwork() (curlReuseConnection = false)

// We call AC functions here without calling ACInitialize() / ACFinalize() as WUT should call these for us.
#define resetNetwork()                                      \
    {                                                       \
        ACConfigId networkCfg;                              \
        if(NNResult_IsSuccess(ACGetStartupId(&networkCfg))) \
            ACConnectWithConfigId(networkCfg);              \
                                                            \
        initNetwork();                                      \
    }

bool initDownloader()
{
    initNetwork();

    char *fn = getStaticPathBuffer(2);
    strcpy(fn, ROMFS_PATH "ca-certificates/");
    struct curl_blob blob = { .data = NULL, .len = 0, .flags = CURL_BLOB_COPY };

#ifndef NUSSPLI_HBL
    FSADirectoryHandle dir;
    if(FSAOpenDir(getFSAClient(), fn, &dir) == FS_ERROR_OK)
#else
    DIR *dir = opendir(fn);
    if(dir != NULL)
#endif
    {
        char *ptr = fn + strlen(fn);
        void *buf;
        size_t bufsize;
        size_t oldcertsize = 0;
        void *tmp;
#ifndef NUSSPLI_HBL
        FSADirectoryEntry entry;
        while(FSAReadDir(getFSAClient(), dir, &entry) == FS_ERROR_OK)
        {
            if(entry.name[0] == '.')
                continue;

            strcpy(ptr, entry.name);
#else
        for(struct dirent *entry = readdir(dir); entry != NULL; entry = readdir(dir))
        {
            if(entry->d_name[0] == '.')
                continue;

            strcpy(ptr, entry->d_name);
#endif
            bufsize = readFile(fn, &buf);
            if(buf == NULL)
                continue;

            oldcertsize = blob.len;
            blob.len += bufsize;
            tmp = blob.data;
            blob.data = MEMAllocFromDefaultHeap(blob.len);
            if(blob.data != NULL)
            {
                if(tmp != NULL)
                    OSBlockMove(blob.data, tmp, oldcertsize, false);

                OSBlockMove(blob.data + oldcertsize, buf, bufsize, false);
            }
            else
            {
                blob.data = tmp;
                blob.len = oldcertsize;
            }

            MEMFreeToDefaultHeap(buf);
        }

#ifndef NUSSPLI_HBL
        FSACloseDir(getFSAClient(), dir);
#else
        closedir(dir);
#endif
    }
    else
        debugPrintf("Error opening %s!", fn);

    CURLcode ret = curl_global_init(CURL_GLOBAL_DEFAULT & ~(CURL_GLOBAL_SSL));
    if(ret == CURLE_OK)
    {
        curl = curl_easy_init();
        if(curl != NULL)
        {
#ifdef NUSSPLI_DEBUG
            curlError[0] = '\0';
            ret = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
            if(ret == CURLE_OK)
            {
#endif
                ret = curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, initSocket);
                if(ret == CURLE_OK)
                {
                    ret = curl_easy_setopt(curl, CURLOPT_USERAGENT, USERAGENT);
                    if(ret == CURLE_OK)
                    {
                        ret = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
                        if(ret == CURLE_OK)
                        {
                            ret = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                            if(ret == CURLE_OK)
                            {
                                ret = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                if(ret == CURLE_OK)
                                {
                                    ret = curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_init);
                                    if(ret == CURLE_OK)
                                    {
                                        ret = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, blob);
                                        if(ret == CURLE_OK)
                                        {
                                            ret = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
                                            if(ret == CURLE_OK)
                                            {
                                                ret = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
                                                if(ret == CURLE_OK)
                                                {
                                                    if(blob.data != NULL)
                                                        MEMFreeToDefaultHeap(blob.data);

                                                    return true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
#ifdef NUSSPLI_DEBUG
            }
            debugPrintf("curl_easy_setopt() failed: %s (%d)", curlError, ret);
#endif
            curl_easy_cleanup(curl);
            curl = NULL;
        }
#ifdef NUSSPLI_DEBUG
        else
            debugPrintf("curl_easy_init() failed!");
#endif
        curl_global_cleanup();
    }

    if(blob.data != NULL)
        MEMFreeToDefaultHeap(blob.data);

    return false;
}

void deinitDownloader()
{
    if(curl != NULL)
    {
        curl_easy_cleanup(curl);
        curl = NULL;
    }
    curl_global_cleanup();
}

static int dlThreadMain(int argc, const char **argv)
{
    debugPrintf("Download thread spawned!");
    int ret = curl_easy_perform(curl);
    ((curlProgressData *)argv[0])->running = false;
    return ret;
}

static const char *translateCurlError(CURLcode err, const char *curlError)
{
    switch(err)
    {
        case CURLE_COULDNT_RESOLVE_HOST:
            return "Couldn't resolve hostname";
        case CURLE_COULDNT_CONNECT:
            return "Couldn't connect to server";
        case CURLE_OPERATION_TIMEDOUT:
            return "Operation timed out";
        case CURLE_GOT_NOTHING:
            return "The server didn't return any data";
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_PARTIAL_FILE:
            return "I/O error";
        case CURLE_PEER_FAILED_VERIFICATION:
            return "Verification failed";
        case CURLE_SSL_CONNECT_ERROR:
            return "Handshake failed";
        case CURLE_FAILED_INIT:
        case CURLE_READ_ERROR:
        case CURLE_OUT_OF_MEMORY:
            return "Internal error";
        default:
            return curlError[0] == '\0' ? "Unknown libcurl error" : curlError;
    }
}

static void drawStatLine(int line, curl_off_t totalSize, curl_off_t currentSize, float bps, uint32_t *eta)
{
    if(currentSize)
    {
        float tmp = currentSize;
        tmp /= totalSize;
        barToFrame(line, 0, 29, tmp);
        if(totalSize)
            *eta = (totalSize - currentSize) / bps;
    }
    else
        barToFrame(line, 0, 29, 0.0D);

    char *toScreen = getToFrameBuffer();
    humanize(currentSize, toScreen);
    char *ptr = toScreen + strlen(toScreen);
    strcpy(ptr, " / ");
    ptr += 3;
    humanize(totalSize, ptr);
    textToFrame(line, 30, toScreen);

    secsToTime(*eta, toScreen);
    textToFrame(line, ALIGNED_RIGHT, toScreen);
}

int downloadFile(const char *url, char *file, downloadData *data, FileType type, bool resume, QUEUE_DATA *queueData, RAMBUF *rambuf)
{
    // Results: 0 = OK | 1 = Error | 2 = No ticket aviable | 3 = Exit
    // Types: 0 = .app | 1 = .h3 | 2 = title.tmd | 3 = tilte.tik

    debugPrintf("Download URL: %s", url);
    debugPrintf("Download PATH: %s", rambuf ? "<RAM>" : file);

    char *name;
    if(rambuf)
        name = file;
    else
    {
        size_t haystack;
        for(haystack = strlen(file); file[haystack] != '/'; haystack--)
            ;
        name = file + haystack + 1;
    }

    char *toScreen = getToFrameBuffer();
    void *fp;
    size_t fileSize;
    if(rambuf)
    {
        fp = (void *)open_memstream(&rambuf->buf, &rambuf->size);
        fileSize = 0;
    }
    else
    {
        if(resume && fileExists(file))
        {
            fileSize = getFilesize(file);
            if(fileSize != 0)
            {
                if(data != NULL && data->cs)
                {
                    if(fileSize == data->cs)
                    {
                        sprintf(toScreen, "Download %s skipped!", name);
                        addToScreenLog(toScreen);
                        data->dlnow += fileSize;
                        if(queueData != NULL)
                            queueData->downloaded += fileSize;

                        return 0;
                    }
                    if(fileSize > data->cs)
                        return downloadFile(url, file, data, type, false, queueData, rambuf);
                }

                fp = (void *)openFile(file, "a", 0);
            }
            else
                fp = (void *)openFile(file, "w", data == NULL ? 0 : data->cs);
        }
        else
        {
            fp = (void *)openFile(file, "w", data == NULL ? 0 : data->cs);
            fileSize = 0;
        }
    }

    if(fp == NULL)
        return 1;

    curlError[0] = '\0';
    volatile curlProgressData cdata = {
        .running = true,
        .error = CURLE_OK,
        .dlnow = 0.0D,
        .dltotal = 0.0D,
    };
    spinCreateLock((cdata.lock), SPINLOCK_FREE);

    CURLcode ret = curl_easy_setopt(curl, CURLOPT_URL, url);
    if(ret == CURLE_OK)
    {
        if(curlReuseConnection)
            ret = curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);
        else
        {
            ret = curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
            curlReuseConnection = true;
        }
        if(ret == CURLE_OK)
        {
            ret = curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)fileSize);
            if(ret == CURLE_OK)
            {
                ret = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, rambuf ? fwrite : (size_t(*)(const void *, size_t, size_t, FILE *))addToIOQueue);
                if(ret == CURLE_OK)
                {
                    ret = curl_easy_setopt(curl, CURLOPT_WRITEDATA, (FILE *)fp);
                    if(ret == CURLE_OK)
                        ret = curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cdata);
                }
            }
        }
    }

    if(ret != CURLE_OK)
    {
        if(rambuf)
            fclose((FILE *)fp);
        else
            addToIOQueue(NULL, 0, 0, (FSAFileHandle)fp);

        debugPrintf("curl_easy_setopt error: %s (%d / %ud)", curlError, ret, fileSize);
        return 1;
    }

    debugPrintf("Calling curl_easy_perform()");
    OSTime t = OSGetSystemTime();

    char *argv[1] = { (char *)&cdata };
    OSThread *dlThread = startThread("NUSspli downloader", THREAD_PRIORITY_HIGH, DLT_STACK_SIZE, dlThreadMain, 1, (char *)argv, OS_THREAD_ATTRIB_AFFINITY_CPU2);
    if(dlThread == NULL)
        return 1;

    OSTick ts;
    OSTick lastTransfair = OSGetTick();
    size_t dltotal; // We use size_t instead of curl_off_t as filesizes are limitted to 4 GB anyway,
    size_t dlnow;
    size_t downloaded;
    size_t tmp;
    float bps;
    float oldBps = 0.0D;
    int frames = 1;
    int line;
    while(cdata.running && AppRunning(true))
    {
        if(--frames == 0)
        {
            if(!spinTryLock(cdata.lock))
            {
                frames = 1;
                continue;
            }

            ts = cdata.ts;
            dltotal = cdata.dltotal;
            dlnow = cdata.dlnow;
            spinReleaseLock(cdata.lock);

            bps = dlnow - downloaded;
            downloaded = dlnow;
            dlnow += fileSize;

            // Calculate download speed
            if(bps != 0.0f)
            {
                if(dltotal)
                {
                    tmp = OSTicksToMilliseconds(ts - lastTransfair); // sample duration in milliseconds
                    if(tmp)
                    {
                        bps *= 1000.0f; // secs to ms.
                        bps /= tmp; // byte/s

                        // Smoothing
                        bps *= 1.0f - SMOOTHING_FACTOR;
                        oldBps *= SMOOTHING_FACTOR;
                        bps += oldBps;
                        oldBps = bps;
                    }
                    else
                        bps = 0.0f;
                }
                else
                    bps = 0.0f;
            }

            lastTransfair = ts;
            startNewFrame();

            if(data != NULL)
            {
                if(queueData != NULL)
                {
                    sprintf(toScreen, "%s (%d/%d)", data->name, queueData->current, queueData->packages);
                    line = textToFrameMultiline(0, ALIGNED_CENTER, toScreen, MAX_CHARS);
                }
                else
                    line = textToFrameMultiline(0, ALIGNED_CENTER, data->name, MAX_CHARS);

                drawStatLine(line++, data->dltotal, data->dlnow + dlnow, bps, &data->eta);

                if(queueData != NULL)
                    drawStatLine(line++, queueData->dlSize, queueData->downloaded + dlnow, bps, &queueData->eta);

                lineToFrame(line++, SCREEN_COLOR_WHITE);

                sprintf(toScreen, "(%d/%d)", data->dcontent + 1, data->contents);
                textToFrame(line, ALIGNED_CENTER, toScreen);
            }
            else
                line = 0;

            if(dltotal)
            {
                if(!rambuf)
                    checkForQueueErrors();

                frames = 60;
                dltotal += fileSize;

                strcpy(toScreen, gettext("Downloading"));
                strcat(toScreen, " ");
                strcat(toScreen, name);
                textToFrame(line, 0, toScreen);

                getSpeedString(bps, toScreen);
                textToFrame(line, ALIGNED_RIGHT, toScreen);

                drawStatLine(++line, dltotal, dlnow, bps, &tmp);
            }
            else
            {
                frames = 1;
                strcpy(toScreen, gettext("Preparing"));
                strcat(toScreen, " ");
                strcat(toScreen, name);
                textToFrame(line++, 0, toScreen);
            }

            writeScreenLog(++line);
            drawFrame();
        }

        showFrame();

        if(cancelOverlay == NULL)
        {
            if(vpad.trigger & VPAD_BUTTON_B)
            {
                strcpy(toScreen, gettext("Do you really want to cancel?"));
                strcat(toScreen, "\n\n" BUTTON_A " ");
                strcat(toScreen, gettext("Yes"));
                strcat(toScreen, " || " BUTTON_B " ");
                strcat(toScreen, gettext("No"));
                cancelOverlay = addErrorOverlay(toScreen);
            }
        }
        else
        {
            if(vpad.trigger & VPAD_BUTTON_A)
            {
                cdata.error = CURLE_ABORTED_BY_CALLBACK;
                closeCancelOverlay();
                break;
            }
            if(vpad.trigger & VPAD_BUTTON_B)
                closeCancelOverlay();
        }
    }

    stopThread(dlThread, (int *)&ret);

    t = OSGetSystemTime() - t;
    addEntropy(&t, sizeof(OSTime));
    if(data == NULL && cancelOverlay != NULL)
        closeCancelOverlay();

    debugPrintf("curl_easy_perform() returned: %d", ret);

    if(rambuf)
        fclose((FILE *)fp);
    else
        addToIOQueue(NULL, 0, 0, (FSAFileHandle)fp);

    if(!AppRunning(true))
        return 1;

    if(ret != CURLE_OK)
    {
        debugPrintf("curl_easy_perform returned an error: %s (%d/%d)\nFile: %s", curlError, ret, cdata.error, rambuf ? "<RAM>" : file);

        if(ret == CURLE_ABORTED_BY_CALLBACK)
        {
            switch(cdata.error)
            {
                case CURLE_ABORTED_BY_CALLBACK:
                    return 1;
                case CURLE_OK:
                    break;
                default:
                    ret = cdata.error;
            }
        }

        const char *te = translateCurlError(ret, curlError);
        switch(ret)
        {
            case CURLE_RANGE_ERROR:
                if(rambuf && rambuf->buf)
                {
                    MEMFreeToDefaultHeap(rambuf->buf);
                    rambuf->buf = NULL;
                    rambuf->size = 0;
                }
                int r = downloadFile(url, file, data, type, false, queueData, rambuf);
                curlReuseConnection = false;
                return r;
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_CONNECT:
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_GOT_NOTHING:
            case CURLE_SEND_ERROR:
            case CURLE_RECV_ERROR:
            case CURLE_PARTIAL_FILE:
                sprintf(toScreen, "%s:\n\t%s\n\n%s", "Network error", te, "check the network settings and try again");
                break;
            case CURLE_PEER_FAILED_VERIFICATION:
            case CURLE_SSL_CONNECT_ERROR:
                sprintf(toScreen, "%s:\n\t%s!\n\n%s", "SSL error", te, "check your Wii Us date and time settings");
                break;
            default:
                sprintf(toScreen, "%s:\n\t%d %s", te, ret, curlError);
                break;
        }

        if(data != NULL && cancelOverlay != NULL)
            closeCancelOverlay();

        int os;

        char *p;
        if(autoResumeEnabled())
        {
            os = 9 * 60; // 9 seconds with 60 FPS
            frames = os;
            strcat(toScreen, "\n\n");
            p = toScreen + strlen(toScreen);
            const char *pt = gettext("Next try in _ seconds.");
            strcpy(p, pt);
            const char *n = strchr(pt, '_');
            p += n - pt;
        }
        else
            drawErrorFrame(toScreen, B_RETURN | Y_RETRY);

        int s;
        while(AppRunning(true))
        {
            if(app == APP_STATE_BACKGROUND)
                continue;
            else if(app == APP_STATE_RETURNING)
                drawErrorFrame(toScreen, B_RETURN | Y_RETRY);

            if(autoResumeEnabled())
            {
                s = frames / 60;
                if(s != os)
                {
                    *p = '1' + s;
                    os = s;
                    drawErrorFrame(toScreen, B_RETURN | Y_RETRY);
                }
            }

            showFrame();

            if(vpad.trigger & VPAD_BUTTON_B)
                break;
            if(vpad.trigger & VPAD_BUTTON_Y || (autoResumeEnabled() && --frames == 0))
            {
                flushIOQueue(); // We flush here so the last file is completely on disc and closed before we retry.
                resetNetwork(); // Recover from network errors.
                return downloadFile(url, file, data, type, resume, queueData, rambuf);
            }
        }
        resetNetwork();
        return 1;
    }
    debugPrintf("curl_easy_perform executed successfully");

    long resp;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp);
    if(resp == 206) // Resumed download OK
        resp = 200;

    debugPrintf("The download returned: %u", resp);
    if(resp != 200)
    {
        if(!rambuf)
        {
            flushIOQueue();
            char *newFile = getStaticPathBuffer(2);
            strcpy(newFile, file);
            FSARemove(getFSAClient(), newFile);
        }

        if(resp == 404 && (type & FILE_TYPE_TMD) == FILE_TYPE_TMD) // Title.tmd not found
        {
            strcpy(toScreen, gettext("The download of title.tmd failed with error: 404"));
            strcat(toScreen, "\n\n");
            strcat(toScreen, gettext("The title cannot be found on the NUS, maybe the provided title ID doesn't exists or\nthe TMD was deleted"));
            drawErrorFrame(toScreen, B_RETURN | Y_RETRY);

            while(AppRunning(true))
            {
                if(app == APP_STATE_BACKGROUND)
                    continue;
                if(app == APP_STATE_RETURNING)
                    drawErrorFrame(toScreen, B_RETURN | Y_RETRY);

                showFrame();

                if(vpad.trigger & VPAD_BUTTON_B)
                    break;
                if(vpad.trigger & VPAD_BUTTON_Y)
                {
                    if(rambuf && rambuf->buf)
                    {
                        MEMFreeToDefaultHeap(rambuf->buf);
                        rambuf->buf = NULL;
                        rambuf->size = 0;
                    }
                    return downloadFile(url, file, data, type, resume, queueData, rambuf);
                }
            }
            return 1;
        }
        else if(resp == 404 && (type & FILE_TYPE_TIK) == FILE_TYPE_TIK)
        { // Fake ticket needed
            return 2;
        }
        else
        {
            sprintf(toScreen, "%s: %ld\n%s: %s\n\n", gettext("The download returned a result different to 200 (OK)"), resp, gettext("File"), rambuf ? file : prettyDir(file));
            if(resp == 400)
            {
                strcat(toScreen, gettext("Request failed. Try again"));
                strcat(toScreen, "\n\n");
            }

            drawErrorFrame(toScreen, B_RETURN | Y_RETRY);

            while(AppRunning(true))
            {
                if(app == APP_STATE_BACKGROUND)
                    continue;
                if(app == APP_STATE_RETURNING)
                    drawErrorFrame(toScreen, B_RETURN | Y_RETRY);

                showFrame();

                if(vpad.trigger & VPAD_BUTTON_B)
                    break;
                if(vpad.trigger & VPAD_BUTTON_Y)
                    return downloadFile(url, file, data, type, resume, queueData, rambuf);
            }
            return 1;
        }
    }

    if(data != NULL)
    {
        curl_off_t dld;
        ret = curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dld);
        if(ret != CURLE_OK)
            dld = 0;

        if(fileSize)
            dld += fileSize;

        data->dlnow += dld;
        if(queueData != NULL)
            queueData->downloaded += dld;
    }

    sprintf(toScreen, "Download %s finished!", name);
    addToScreenLog(toScreen);
    return 0;
}

#ifndef NUSSPLI_LITE
bool downloadTitle(const TMD *tmd, size_t tmdSize, const TitleEntry *titleEntry, const char *titleVer, char *folderName, bool inst, NUSDEV dlDev, bool toUSB, bool keepFiles, QUEUE_DATA *queueData)
{
    char tid[17];
    hex(tmd->tid, 16, tid);
    debugPrintf("Downloading title... tID: %s, tVer: %s, name: %s, folder: %s", tid, titleVer, titleEntry->name, folderName);

    char downloadUrl[256];
    strcpy(downloadUrl, DOWNLOAD_URL);
    strcat(downloadUrl, tid);
    strcat(downloadUrl, "/");

    if(folderName[0] == '\0')
    {
        for(int i = 0; i < strlen(titleEntry->name); ++i)
            folderName[i] = isAllowedInFilename(titleEntry->name[i]) ? titleEntry->name[i] : '_';
    }
    strcpy(folderName + strlen(titleEntry->name), " [");
    strcat(folderName, tid);
    strcat(folderName, "]");

    if(strlen(titleVer) > 0)
    {
        strcat(folderName, " v");
        strcat(folderName, titleVer);
    }

    char *installDir = getStaticPathBuffer(3);
    strcpy(installDir, dlDev == NUSDEV_USB01 ? INSTALL_DIR_USB1 : (dlDev == NUSDEV_USB02 ? INSTALL_DIR_USB2 : (dlDev == NUSDEV_SD ? INSTALL_DIR_SD : INSTALL_DIR_MLC)));
    if(!dirExists(installDir))
    {
        debugPrintf("Creating directory \"%s\"", installDir);
        FSStatus err = createDirectory(installDir);
        if(err == FS_STATUS_OK)
            addToScreenLog("Install directory successfully created");
        else
        {
            char *toScreen = getToFrameBuffer();
            strcpy(toScreen, translateFSErr(err));
            showErrorFrame(toScreen);
            return false;
        }
    }

    strcat(installDir, folderName);
    strcat(installDir, "/");

    addToScreenLog("Started the download of \"%s\"", titleEntry->name);
    addToScreenLog("The content will be saved on \"%s\"", prettyDir(installDir));

    if(!dirExists(installDir))
    {
        debugPrintf("Creating directory \"%s\"", installDir);
        FSStatus err = createDirectory(installDir);
        if(err == FS_STATUS_OK)
            addToScreenLog("Download directory successfully created");
        else
        {
            char *toScreen = getToFrameBuffer();
            strcpy(toScreen, translateFSErr(err));
            showErrorFrame(toScreen);
            return false;
        }
    }
    else
        addToScreenLog("WARNING: The download directory already exists");

    char *idp = installDir + strlen(installDir);
    strcpy(idp, "title.tmd");

    FSAFileHandle fp = openFile(installDir, "w", tmdSize);
    if(fp == 0)
    {
        showErrorFrame("Can't save title.tmd file!");
        return false;
    }

    addToIOQueue(tmd, 1, tmdSize, fp);
    addToIOQueue(NULL, 0, 0, fp);
    addToScreenLog("title.tmd saved");

    char *toScreen = getToFrameBuffer();
    strcpy(toScreen, "=>Title type: ");
    bool hasDependencies;
    switch(getTidHighFromTid(tmd->tid)) // Title type
    {
        case TID_HIGH_GAME:
            strcat(toScreen, "eShop or Packed");
            hasDependencies = false;
            break;
        case TID_HIGH_DEMO:
            strcat(toScreen, "eShop/Kiosk demo");
            hasDependencies = false;
            break;
        case TID_HIGH_DLC:
            strcat(toScreen, "eShop DLC");
            hasDependencies = true;
            break;
        case TID_HIGH_UPDATE:
            strcat(toScreen, "eShop Update");
            hasDependencies = true;
            break;
        case TID_HIGH_SYSTEM_APP:
            strcat(toScreen, "System Application");
            hasDependencies = false;
            break;
        case TID_HIGH_SYSTEM_DATA:
            strcat(toScreen, "System Data Archive");
            hasDependencies = false;
            break;
        case TID_HIGH_SYSTEM_APPLET:
            strcat(toScreen, "Applet");
            hasDependencies = false;
            break;
        // vWii //
        case TID_HIGH_VWII_IOS:
            strcat(toScreen, "Wii IOS");
            hasDependencies = false;
            break;
        case TID_HIGH_VWII_SYSTEM_APP:
            strcat(toScreen, "vWii System Application");
            hasDependencies = false;
            break;
        case TID_HIGH_VWII_SYSTEM:
            strcat(toScreen, "vWii System Channel");
            hasDependencies = false;
            break;
        default:
            sprintf(toScreen + strlen(toScreen), "Unknown (0x%08X)", getTidHighFromTid(tmd->tid));
            hasDependencies = false;
            break;
    }
    addToScreenLog(toScreen);

    char *dup = downloadUrl + strlen(downloadUrl);
    strcpy(dup, "cetk");
    strcpy(idp, "title.tik");

    downloadData data = {
        .name = titleEntry->name,
        .contents = tmd->num_contents + 1,
        .dcontent = 0,
        .dlnow = 0,
        .dltotal = 0,
        .eta = -1,
    };

    if(!fileExists(installDir))
    {
        RAMBUF *tikBuf = allocRamBuf();
        if(tikBuf == NULL)
        {
            return false;
        }

        data.cs = 0;
        int tikRes = downloadFile(downloadUrl, installDir, &data, FILE_TYPE_TIK | FILE_TYPE_TORAM, false, queueData, tikBuf);
        switch(tikRes)
        {
            case 2:
                if(generateTik(installDir, titleEntry, tmd))
                    addToScreenLog("Fake ticket created successfully");
                else
                    return false;

                break;
            case 1:
                freeRamBuf(tikBuf);
                return false;
            default:
                break;
        }

        fp = openFile(installDir, "w", tmdSize);
        if(fp == 0)
        {
            freeRamBuf(tikBuf);
            showErrorFrame("Can't save title.tmd file!");
            return false;
        }

        addToIOQueue(tikBuf->buf, 1, tikBuf->size, fp);
        addToIOQueue(NULL, 0, 0, fp);

        ++data.dcontent;
        strcpy(idp, "title.cert");
        if(!fileExists(installDir))
        {
            if(generateCert(tmd, (TICKET *)tikBuf->buf, tikBuf->size, installDir))
                addToScreenLog("Cert created!");
            else
            {
                freeRamBuf(tikBuf);
                return false;
            }
        }
        else
            addToScreenLog("Cert skipped!");

        freeRamBuf(tikBuf);
    }
    else
        addToScreenLog("title.tik skipped!");

    if(!AppRunning(true))
        return false;

    // Get .app and .h3 files
    curl_off_t as;
    for(int i = 0; i < tmd->num_contents; ++i)
    {
        as = tmd->contents[i].size;
        data.dltotal += as;
        if(tmd->contents[i].type & TMD_CONTENT_TYPE_HASHED)
        {
            ++data.contents;
            data.dltotal += getH3size(as);
        }
    }

    char *dupp = dup + 8;
    char *idpp = idp + 8;
    for(int i = 0; i < tmd->num_contents && AppRunning(true); ++i)
    {
        hex(tmd->contents[i].cid, 8, dup);
        OSBlockMove(idp, dup, 8, false);
        strcpy(idpp, ".app");

        data.cs = tmd->contents[i].size;
        if(downloadFile(downloadUrl, installDir, &data, FILE_TYPE_APP, true, queueData, NULL) == 1)
            return false;

        ++data.dcontent;

        if(tmd->contents[i].type & TMD_CONTENT_TYPE_HASHED)
        {
            strcpy(dupp, ".h3");
            strcpy(idpp, ".h3");
            data.cs = getH3size(tmd->contents[i].size);

            if(downloadFile(downloadUrl, installDir, &data, FILE_TYPE_H3, true, queueData, NULL) == 1)
                return false;

            ++data.dcontent;
        }
    }

    if(cancelOverlay != NULL)
        closeCancelOverlay();

    if(!AppRunning(true))
        return false;

    bool ret;
    if(inst)
    {
        *idp = '\0';
        ret = install(titleEntry->name, hasDependencies, dlDev, installDir, toUSB, keepFiles, tmd);
    }
    else
        ret = true;

    return ret;
}
#endif

RAMBUF *allocRamBuf()
{
    RAMBUF *ret = MEMAllocFromDefaultHeap(sizeof(RAMBUF));
    if(ret == NULL)
        return NULL;

    ret->buf = NULL;
    ret->size = 0;
    return ret;
}

void freeRamBuf(RAMBUF *rambuf)
{
    if(rambuf->buf != NULL)
        MEMFreeToDefaultHeap(rambuf->buf);

    MEMFreeToDefaultHeap(rambuf);
}
