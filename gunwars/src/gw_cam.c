/* gw_cam.c -- the NamCam from a WEBCAM (the user's idea): Windows Media Foundation, the system's own capture (no downloads).
 * A thread opens the first video capture device, asks for RGB32 (Media Foundation converts the camera's own format), and keeps the
 * latest picture centre-cropped to a square and scaled to 128 x 128 (the photo page's player slot, src/gw_game.c). Other systems: none. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

static HANDLE th;
static volatile LONG want, alive, failed;
static CRITICAL_SECTION cs; static int cs_ok;
static uint32_t latest[128 * 128];
static volatile LONG have;

static DWORD WINAPI cam_thread(void *u)
{
    (void)u;
    IMFAttributes *a = NULL, *ra = NULL; IMFActivate **devs = NULL; UINT32 n = 0;
    IMFMediaSource *src = NULL; IMFSourceReader *rd = NULL; IMFMediaType *mt = NULL, *ct = NULL;
    int ok = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) goto out;
    if (FAILED(MFCreateAttributes(&a, 1))) goto out;
    IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(MFEnumDeviceSources(a, &devs, &n)) || n == 0) { fprintf(stderr, "[CAM] no webcam found\n"); goto out; }
    { WCHAR *name = NULL; UINT32 len = 0;
      if (SUCCEEDED(IMFActivate_GetAllocatedString(devs[0], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) { fprintf(stderr, "[CAM] webcam: %ls\n", name); CoTaskMemFree(name); } }
    if (FAILED(IMFActivate_ActivateObject(devs[0], &IID_IMFMediaSource, (void **)&src))) goto out;
    /* the camera's own formats (a source reader may show only its default, 160 x 120 on the user's laptop): the widest up to 1280 */
    { IMFPresentationDescriptor *pd = NULL; IMFStreamDescriptor *sd = NULL; IMFMediaTypeHandler *mh = NULL; BOOL sel = FALSE; DWORD cnt = 0;
      { static int lg = -1; if (lg < 0) lg = getenv("GW_CAMLOG") != NULL;
        if (lg) { fprintf(stderr, "[CAM] %u capture devices\n", (unsigned)n);
                  IMFPresentationDescriptor *p2 = NULL; DWORD ns = 0;
                  if (SUCCEEDED(IMFMediaSource_CreatePresentationDescriptor(src, &p2))) { IMFPresentationDescriptor_GetStreamDescriptorCount(p2, &ns); fprintf(stderr, "[CAM] %lu streams\n", (unsigned long)ns);
                      for (DWORD k = 0; k < ns; k++) { IMFStreamDescriptor *s2 = NULL; IMFMediaTypeHandler *h2 = NULL; BOOL se = FALSE; DWORD c2 = 0;
                          if (SUCCEEDED(IMFPresentationDescriptor_GetStreamDescriptorByIndex(p2, k, &se, &s2)) && SUCCEEDED(IMFStreamDescriptor_GetMediaTypeHandler(s2, &h2)) && SUCCEEDED(IMFMediaTypeHandler_GetMediaTypeCount(h2, &c2)))
                              for (DWORD i = 0; i < c2; i++) { IMFMediaType *t = NULL; if (SUCCEEDED(IMFMediaTypeHandler_GetMediaTypeByIndex(h2, i, &t))) { UINT64 f = 0; IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &f); fprintf(stderr, "[CAM] stream %lu (selected %d) format %lu: %u x %u\n", (unsigned long)k, se, (unsigned long)i, (UINT32)(f >> 32), (UINT32)f); IMFMediaType_Release(t); } }
                          if (h2) IMFMediaTypeHandler_Release(h2); if (s2) IMFStreamDescriptor_Release(s2); }
                      IMFPresentationDescriptor_Release(p2); } } }
      if (SUCCEEDED(IMFMediaSource_CreatePresentationDescriptor(src, &pd)) && SUCCEEDED(IMFPresentationDescriptor_GetStreamDescriptorByIndex(pd, 0, &sel, &sd))
          && SUCCEEDED(IMFStreamDescriptor_GetMediaTypeHandler(sd, &mh)) && SUCCEEDED(IMFMediaTypeHandler_GetMediaTypeCount(mh, &cnt))) {
          IMFMediaType *best = NULL; UINT32 bw = 0;
          for (DWORD i = 0; i < cnt; i++) { IMFMediaType *t = NULL;
              if (FAILED(IMFMediaTypeHandler_GetMediaTypeByIndex(mh, i, &t))) continue;
              UINT64 f = 0; IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &f); const UINT32 tw = (UINT32)(f >> 32);
              { static int lg = -1; if (lg < 0) lg = getenv("GW_CAMLOG") != NULL; if (lg) fprintf(stderr, "[CAM] device format %lu: %u x %u\n", (unsigned long)i, tw, (UINT32)f); }
              if (tw <= 1280 && tw > bw) { if (best) IMFMediaType_Release(best); best = t; bw = tw; } else IMFMediaType_Release(t); }
          if (best) { IMFMediaTypeHandler_SetCurrentMediaType(mh, best); IMFMediaType_Release(best); }
      }
      if (mh) IMFMediaTypeHandler_Release(mh); if (sd) IMFStreamDescriptor_Release(sd); if (pd) IMFPresentationDescriptor_Release(pd); }
    if (FAILED(MFCreateAttributes(&ra, 1))) goto out;
    IMFAttributes_SetUINT32(ra, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   /* YUY2 / NV12 / MJPG -> RGB32 for us */
    if (FAILED(MFCreateSourceReaderFromMediaSource(src, ra, &rd))) goto out;
    { IMFMediaType *best = NULL; UINT32 bw = 0;
      for (DWORD i = 0; ; i++) { IMFMediaType *t = NULL;
          if (FAILED(IMFSourceReader_GetNativeMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) break;
          UINT64 f = 0; IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &f); const UINT32 tw = (UINT32)(f >> 32);
          { static int lg = -1; if (lg < 0) lg = getenv("GW_CAMLOG") != NULL; if (lg) fprintf(stderr, "[CAM] native type %lu: %u x %u\n", (unsigned long)i, tw, (UINT32)f); }
          if (tw <= 1280 && tw > bw) { if (best) IMFMediaType_Release(best); best = t; bw = tw; } else IMFMediaType_Release(t); }
      if (best) { IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, best); IMFMediaType_Release(best); } }
    if (FAILED(MFCreateMediaType(&mt))) goto out;
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    { IMFMediaType *cur = NULL; UINT64 f = 0;   /* the same size as the native format just chosen */
      if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) { if (SUCCEEDED(IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &f))) IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, f); IMFMediaType_Release(cur); } }
    if (FAILED(IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt))) { fprintf(stderr, "[CAM] the webcam gives no RGB picture\n"); goto out; }
    if (FAILED(IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &ct))) goto out;
    UINT64 fs = 0; IMFMediaType_GetUINT64(ct, &MF_MT_FRAME_SIZE, &fs);
    const int w = (int)(fs >> 32), h = (int)(fs & 0xFFFFFFFFu);
    UINT32 sv = 0; INT32 stride = SUCCEEDED(IMFMediaType_GetUINT32(ct, &MF_MT_DEFAULT_STRIDE, &sv)) ? (INT32)sv : w * 4;
    if (w <= 0 || h <= 0) goto out;
    fprintf(stderr, "[CAM] capturing %d x %d\n", w, h);
    ok = 1;
    static uint32_t tmp[128 * 128];
    while (want) {
        DWORD idx = 0, flags = 0; LONGLONG ts = 0; IMFSample *s = NULL;
        if (FAILED(IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        IMFMediaBuffer *b = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &b))) {
            BYTE *p = NULL; DWORD len = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) {
                const int side = w < h ? w : h, ox = (w - side) / 2, oy = (h - side) / 2;
                const int astr = stride < 0 ? -stride : stride;
                for (int y = 0; y < 128; y++) {
                    const int sy = oy + y * side / 128;
                    const BYTE *row = stride < 0 ? p + (size_t)(h - 1 - sy) * (size_t)astr : p + (size_t)sy * (size_t)astr;   /* bottom-up when negative */
                    if ((size_t)(row - p) + (size_t)w * 4 > len) continue;
                    for (int x = 0; x < 128; x++) {
                        const uint32_t px = ((const uint32_t *)row)[ox + x * side / 128];   /* as the camera sees it: the game's camera screen draws
                                                                                                 * the box mirrored itself (a selfie view), the photo the right way */
                        tmp[y * 128 + x] = px & 0xFFFFFF;
                    }
                }
                IMFMediaBuffer_Unlock(b);
                EnterCriticalSection(&cs); memcpy(latest, tmp, sizeof latest); LeaveCriticalSection(&cs);
                InterlockedExchange(&have, 1);
            }
            IMFMediaBuffer_Release(b);
        }
        IMFSample_Release(s);
    }
out:
    if (!ok) InterlockedExchange(&failed, 1);
    if (ct) IMFMediaType_Release(ct);
    if (mt) IMFMediaType_Release(mt);
    if (rd) IMFSourceReader_Release(rd);
    if (src) { IMFMediaSource_Shutdown(src); IMFMediaSource_Release(src); }
    if (devs) { for (UINT32 i = 0; i < n; i++) IMFActivate_Release(devs[i]); CoTaskMemFree(devs); }
    if (ra) IMFAttributes_Release(ra);
    if (a) IMFAttributes_Release(a);
    MFShutdown(); CoUninitialize();
    InterlockedExchange(&alive, 0);
    return 0;
}
/* start capturing (once; the thread keeps running until gw_cam_stop) */
void gw_cam_start(void)
{
    if (!cs_ok) { InitializeCriticalSection(&cs); cs_ok = 1; }
    if (alive || failed) return;
    InterlockedExchange(&want, 1); InterlockedExchange(&alive, 1); InterlockedExchange(&have, 0);
    if (th) CloseHandle(th);
    th = CreateThread(NULL, 0, cam_thread, NULL, 0, NULL);
    if (!th) { InterlockedExchange(&alive, 0); InterlockedExchange(&failed, 1); }
}
void gw_cam_stop(void) { InterlockedExchange(&want, 0); }   /* the thread ends after its current frame and frees the camera */
/* the latest 128 x 128 picture (0x00RRGGBB); 0 = none yet */
int gw_cam_frame(uint32_t *dst)
{
    if (!have || !cs_ok) return 0;
    EnterCriticalSection(&cs); memcpy(dst, latest, sizeof latest); LeaveCriticalSection(&cs);
    return 1;
}
int gw_cam_failed(void) { return failed != 0; }

/* THE PHOTO FILE through Windows' own image decoders (WIC): JPG, PNG, BMP, GIF, TIFF, and AVIF / WebP / HEIC where Windows has those codecs
 * (the user's first photo.bmp was an AVIF under a .bmp name -- the content decides, not the name). Centre-cropped to a square, 128 x 128
 * into dst (0x00RRGGBB). 1 = loaded */
#include <wincodec.h>
int gw_image_load(const char *path, uint32_t *dst)
{
    WCHAR wp[MAX_PATH]; if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, MAX_PATH)) return 0;
    const HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *f = NULL; IWICBitmapDecoder *d = NULL; IWICBitmapFrameDecode *fr = NULL; IWICFormatConverter *cv = NULL;
    uint32_t *pix = NULL; int ok = 0; UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) goto out;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(f, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d))) goto out;
    if (FAILED(IWICBitmapDecoder_GetFrame(d, 0, &fr))) goto out;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv))) goto out;
    if (FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) goto out;
    if (FAILED(IWICFormatConverter_GetSize(cv, &w, &h)) || !w || !h || (unsigned long long)w * h > 64000000ull) goto out;
    if (!(pix = malloc((size_t)w * h * 4))) goto out;
    if (FAILED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * h * 4, (BYTE *)pix))) goto out;
    { const UINT side = w < h ? w : h, ox = (w - side) / 2, oy = (h - side) / 2;
      for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) dst[y * 128 + x] = pix[(size_t)(oy + y * side / 128) * w + ox + x * side / 128] & 0xFFFFFF; }
    ok = 1;
out:
    free(pix);
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (d) IWICBitmapDecoder_Release(d);
    if (f) IWICImagingFactory_Release(f);
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}
#else
void gw_cam_start(void) {}
void gw_cam_stop(void) {}
int gw_cam_frame(uint32_t *dst) { (void)dst; return 0; }
int gw_cam_failed(void) { return 1; }
int gw_image_load(const char *path, uint32_t *dst) { (void)path; (void)dst; return 0; }
#endif
