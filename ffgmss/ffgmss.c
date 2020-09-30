#include <stdlib.h>
#include <stdio.h>
#include <windows.h>

#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

typedef struct {
    mp3dec_t decoder;
    HANDLE   hFile, hMap;
    uint8_t *fbuf, *dbuf;
    int32_t  flen,  dlen;
    int      pcm_len, pcm_channels, pcm_samprate;
    int16_t  pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
} MP3DECODER;

static void mp3decoder_free(MP3DECODER *mp3dec)
{
    if (!mp3dec) return;
    if (mp3dec->fbuf ) UnmapViewOfFile(mp3dec->fbuf);
    if (mp3dec->hMap ) CloseHandle(mp3dec->hMap );
    if (mp3dec->hFile) CloseHandle(mp3dec->hFile);
    memset(mp3dec, 0, sizeof(MP3DECODER) - sizeof(mp3dec->pcm_buf));
}

static int mp3decoder_init(MP3DECODER *mp3dec, char *file)
{
    DWORD len = 0;
    mp3dec->hFile = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (mp3dec->hFile == INVALID_HANDLE_VALUE) return -1;

    if ((len = GetFileSize(mp3dec->hFile, NULL)) == 0) goto done;
    mp3dec->hMap = CreateFileMapping(mp3dec->hFile, NULL, PAGE_READONLY, 0, len, NULL);
    if (mp3dec->hMap  == INVALID_HANDLE_VALUE) goto done;
    mp3dec->flen = mp3dec->dlen = len;
    mp3dec->fbuf = mp3dec->dbuf = MapViewOfFile(mp3dec->hMap, FILE_MAP_READ, 0, 0, len);

done:
    if (!mp3dec->fbuf) {
        mp3decoder_free(mp3dec);
        return -1;
    }
    return 0;
}

static void mp3decoder_decode(MP3DECODER *mp3dec)
{
    mp3dec_frame_info_t info = {0};
    mp3dec->pcm_len      = mp3dec_decode_frame(&mp3dec->decoder, mp3dec->dbuf, mp3dec->dlen, mp3dec->pcm_buf, &info);
    mp3dec->dbuf        += info.frame_bytes;
    mp3dec->dlen        -= info.frame_bytes;
    mp3dec->pcm_channels = info.channels;
    mp3dec->pcm_samprate = info.hz;
}

#define DEF_ADEV_BUF_NUM  3
#define DEF_ADEV_BUF_LEN  2048
typedef struct {
    int      head;
    int      tail;
    int      bufnum;
    int      buflen;
    int      samprate;
    int      channels;
    HWAVEOUT hWaveOut;
    WAVEHDR *pWaveHdr;
    HANDLE   hWaveSem;
} ADEV;

static void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    ADEV *c = (ADEV*)dwInstance;
    switch (uMsg) {
    case WOM_DONE:
        if (++c->head == c->bufnum) c->head = 0;
        ReleaseSemaphore(c->hWaveSem, 1, NULL);
        break;
    }
}

static void adev_free(ADEV *adev)
{
    if (!adev) return;

    // close waveout
    if (adev->hWaveOut) {
        int  i;
        waveOutReset(adev->hWaveOut);
        for (i=0; i<adev->bufnum; i++) {
            waveOutUnprepareHeader(adev->hWaveOut, &adev->pWaveHdr[i], sizeof(WAVEHDR));
        }
        waveOutClose(adev->hWaveOut);
    }

    // close semaphore
    CloseHandle(adev->hWaveSem);

    // free memory
    free(adev->pWaveHdr);
    free(adev);
}

static ADEV* adev_init(int bufnum, int buflen, int samprate, int channels)
{
    WAVEFORMATEX wfx = {0};
    ADEV        *adev= NULL;
    BYTE        *wavbuf;
    MMRESULT     result;
    int          ok  = 1, i;

    adev = (ADEV*)calloc(1, sizeof(ADEV));
    if (!adev) {
        printf("failed to allocate adev context !\n");
        return NULL;
    }

    bufnum         = bufnum ? bufnum : DEF_ADEV_BUF_NUM;
    buflen         = buflen ? buflen : DEF_ADEV_BUF_LEN;
    adev->bufnum   = bufnum;
    adev->buflen   = buflen;
    adev->samprate = samprate;
    adev->channels = channels;
    adev->pWaveHdr = (WAVEHDR*)calloc(bufnum, (sizeof(WAVEHDR) + buflen));
    adev->hWaveSem = CreateSemaphore(NULL, bufnum, bufnum, NULL);
    if (!adev->pWaveHdr || !adev->hWaveSem) {
        printf("failed to allocate waveout buffer and waveout semaphore !\n");
        ok = 0; goto done;
    }

    wfx.cbSize          = sizeof(wfx);
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.wBitsPerSample  = 16;
    wfx.nSamplesPerSec  = samprate;
    wfx.nChannels       = channels;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
    result = waveOutOpen(&adev->hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)waveOutProc, (DWORD_PTR)adev, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        printf("waveout open device failed !\n");
        ok = 0; goto done;
    }

    wavbuf = (BYTE*)(adev->pWaveHdr + bufnum);
    for (i=0; i<bufnum; i++) {
        adev->pWaveHdr[i].lpData         = (LPSTR)(wavbuf + i * buflen);
        adev->pWaveHdr[i].dwBufferLength = buflen;
        waveOutPrepareHeader(adev->hWaveOut, &adev->pWaveHdr[i], sizeof(WAVEHDR));
    }

done:
    if (!ok) {
        adev_free(adev);
        adev = NULL;
    }
    return adev;
}

static void adev_write(ADEV *adev, uint8_t *buf, int len)
{
    int total = len, n;
    if (!adev) return;
    while (len) {
        WaitForSingleObject(adev->hWaveSem, -1);
        n = len < adev->buflen ? len : adev->buflen;
        memcpy(adev->pWaveHdr[adev->tail].lpData, buf, n);
        adev->pWaveHdr[adev->tail].dwBufferLength = n;
        waveOutWrite(adev->hWaveOut, &adev->pWaveHdr[adev->tail], sizeof(WAVEHDR));
        if (++adev->tail == adev->bufnum) adev->tail = 0;
        buf += n; len -= n;
    }
}

static void adev_pause(ADEV *adev, int pause)
{
    if (!adev) return;
    if (pause) waveOutPause  (adev->hWaveOut);
    else       waveOutRestart(adev->hWaveOut);
}

static void adev_reset(ADEV *adev)
{
    if (!adev) return;
    waveOutReset(adev->hWaveOut);
    adev->head = adev->tail = 0;
    ReleaseSemaphore(adev->hWaveSem, adev->bufnum, NULL);
}

typedef struct {
    int bgm_list_size;
    int bgm_list_play;
    int bgm_list_loop;

    int snd_queue_head;
    int snd_queue_tail;
    int snd_queue_size;

    MP3DECODER bgm_dec;
    MP3DECODER snd_dec;

    #define MAX_BGM_FILE_NUM  32
    #define MAX_SND_FILE_NUM  8
    char bgm_file_list [MAX_BGM_FILE_NUM][MAX_PATH];
    char snd_file_queue[MAX_SND_FILE_NUM][MAX_PATH];
} GMSS;

void* gmss_init (void);
void  gmss_exit (void *ctxt);
void  gmss_sound(void *ctxt, char *file);
void  gmss_music_play (void *ctxt, char *files[], int num, int idx);
void  gmss_music_pause(void *ctxt);
void  gmss_music_loop (void *ctxt, int loop);

int main(void)
{
    mp3dec_t dec = {0};
    mp3dec_frame_info_t info = {0};
    int16_t  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint8_t *pbuf = NULL, *ptmp = NULL;
    DWORD    len  = 0, ltmp;
    int      n;

    if (1) {
        HANDLE   hFile = NULL;
        HANDLE   hMap  = NULL;
        hFile = CreateFile(TEXT("test.mp3"), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) goto done;
        len  = GetFileSize(hFile, NULL);
        hMap = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, len, NULL);
        if (hMap == INVALID_HANDLE_VALUE) goto done;
        pbuf = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, len);
        ptmp = pbuf; ltmp = len;
        do {
            n = mp3dec_decode_frame(&dec, ptmp, ltmp, pcm, &info);
            ptmp += info.frame_bytes;
            ltmp -= info.frame_bytes;
            printf("n: %d\n", n);
        } while (n > 0);
done:
        if (pbuf ) UnmapViewOfFile(pbuf);
        if (hMap ) CloseHandle(hMap );
        if (hFile) CloseHandle(hFile);
    }
    return 0;
}
