/*
 * Copyright 2020 Andrew Eikum for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "ole2.h"
#include "mmdeviceapi.h"
#include "mmsystem.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "spatialaudioclient.h"

#include "mmdevapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);

static UINT32 AudioObjectType_to_index(AudioObjectType type)
{
    UINT32 o = 0;
    while(type){
        type >>= 1;
        ++o;
    }
    return o - 2;
}

static const char *debugstr_fmtex(const WAVEFORMATEX *fmt)
{
    static char buf[2048];

    if (!fmt)
    {
        strcpy(buf, "(null)");
    }
    else if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
        snprintf(buf, sizeof(buf), "tag: 0x%x (%s), ch: %u (mask: 0x%lx), rate: %lu, depth: %u",
                fmt->wFormatTag, debugstr_guid(&fmtex->SubFormat),
                fmt->nChannels, fmtex->dwChannelMask, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    else
    {
        snprintf(buf, sizeof(buf), "tag: 0x%x, ch: %u, rate: %lu, depth: %u",
                fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    return buf;
}

static BOOL formats_equal(const WAVEFORMATEX *fmt1, const WAVEFORMATEX *fmt2)
{
    return !memcmp(fmt1, fmt2, sizeof(*fmt1)) && !memcmp(fmt1 + 1, fmt2 + 1, fmt1->cbSize);
}

/* Maximum number of dynamic spatial audio objects. On Windows a non-zero
 * count is only reported when a spatial sound renderer such as Windows Sonic
 * is enabled for the endpoint. WINE_SPATIAL_MAX_DYN overrides the default;
 * 0 disables dynamic spatial audio support entirely. */
#define DEFAULT_MAX_DYNAMIC_OBJECTS 112

static UINT32 spatial_max_dynamic_objects(void)
{
    static LONG max_objects = -1;

    if(max_objects < 0){
        LONG value = DEFAULT_MAX_DYNAMIC_OBJECTS;
        char buf[16];
        DWORD len;

        len = GetEnvironmentVariableA("WINE_SPATIAL_MAX_DYN", buf, sizeof(buf));
        if(len && len < sizeof(buf)){
            char *end;
            ULONG parsed = strtoul(buf, &end, 10);
            if(end != buf && !*end)
                value = parsed > 1024 ? 1024 : parsed;
            else
                WARN("Ignoring invalid WINE_SPATIAL_MAX_DYN=%s\n", debugstr_a(buf));
        }
        TRACE("max dynamic object count: %ld\n", value);
        max_objects = value;
    }

    return max_objects;
}

struct speaker_info {
    AudioObjectType type;
    DWORD speaker;
    float azimuth; /* degrees, clockwise, 0 is straight ahead of the listener */
    BOOL height;
    BOOL lfe;
};

/* ordered by speaker mask bit, i.e. by channel position within a stream */
static const struct speaker_info speaker_info_table[] = {
    { AudioObjectType_FrontLeft,     SPEAKER_FRONT_LEFT,      -30.0f, FALSE, FALSE },
    { AudioObjectType_FrontRight,    SPEAKER_FRONT_RIGHT,      30.0f, FALSE, FALSE },
    { AudioObjectType_FrontCenter,   SPEAKER_FRONT_CENTER,      0.0f, FALSE, FALSE },
    { AudioObjectType_LowFrequency,  SPEAKER_LOW_FREQUENCY,     0.0f, FALSE, TRUE  },
    { AudioObjectType_BackLeft,      SPEAKER_BACK_LEFT,      -145.0f, FALSE, FALSE },
    { AudioObjectType_BackRight,     SPEAKER_BACK_RIGHT,      145.0f, FALSE, FALSE },
    { AudioObjectType_BackCenter,    SPEAKER_BACK_CENTER,     180.0f, FALSE, FALSE },
    { AudioObjectType_SideLeft,      SPEAKER_SIDE_LEFT,       -90.0f, FALSE, FALSE },
    { AudioObjectType_SideRight,     SPEAKER_SIDE_RIGHT,       90.0f, FALSE, FALSE },
    { AudioObjectType_TopFrontLeft,  SPEAKER_TOP_FRONT_LEFT,  -45.0f, TRUE,  FALSE },
    { AudioObjectType_TopFrontRight, SPEAKER_TOP_FRONT_RIGHT,  45.0f, TRUE,  FALSE },
    { AudioObjectType_TopBackLeft,   SPEAKER_TOP_BACK_LEFT,  -135.0f, TRUE,  FALSE },
    { AudioObjectType_TopBackRight,  SPEAKER_TOP_BACK_RIGHT,  135.0f, TRUE,  FALSE },
};

typedef struct SpatialAudioImpl SpatialAudioImpl;
typedef struct SpatialAudioStreamImpl SpatialAudioStreamImpl;
typedef struct SpatialAudioObjectImpl SpatialAudioObjectImpl;

struct SpatialAudioObjectImpl {
    ISpatialAudioObject ISpatialAudioObject_iface;
    LONG ref;

    SpatialAudioStreamImpl *sa_stream;
    AudioObjectType type;
    UINT32 static_idx;

    BOOL updated;
    BOOL invalidated;
    BOOL mixed;
    BOOL dirty;
    BOOL lifetime_started;
    float position[3];
    float volume;
    float gain_cur[ARRAY_SIZE(speaker_info_table)];
    float gain_tgt[ARRAY_SIZE(speaker_info_table)];

    float *buf;

    struct list entry;
};

struct SpatialAudioStreamImpl {
    ISpatialAudioObjectRenderStream ISpatialAudioObjectRenderStream_iface;
    LONG ref;
    CRITICAL_SECTION lock;

    SpatialAudioImpl *sa_client;
    SpatialAudioObjectRenderStreamActivationParams params;

    IAudioClient *client;
    IAudioRenderClient *render;

    UINT32 period_frames, update_frames, buffer_frames;
    WAVEFORMATEXTENSIBLE stream_fmtex;

    float *buf;

    UINT32 static_object_map[17];

    const struct speaker_info *bed_speakers[ARRAY_SIZE(speaker_info_table)];
    BOOL has_height;
    BOOL started;
    UINT32 active_dynamic_count;

    struct list objects;
};

struct SpatialAudioImpl {
    ISpatialAudioClient ISpatialAudioClient_iface;
    IAudioFormatEnumerator IAudioFormatEnumerator_iface;
    IMMDevice *mmdev;
    LONG ref;
    WAVEFORMATEXTENSIBLE object_fmtex;
    DWORD native_channel_mask;
};

static inline SpatialAudioObjectImpl *impl_from_ISpatialAudioObject(ISpatialAudioObject *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioObjectImpl, ISpatialAudioObject_iface);
}

static inline SpatialAudioStreamImpl *impl_from_ISpatialAudioObjectRenderStream(ISpatialAudioObjectRenderStream *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioStreamImpl, ISpatialAudioObjectRenderStream_iface);
}

static inline SpatialAudioImpl *impl_from_ISpatialAudioClient(ISpatialAudioClient *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, ISpatialAudioClient_iface);
}

static inline SpatialAudioImpl *impl_from_IAudioFormatEnumerator(IAudioFormatEnumerator *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, IAudioFormatEnumerator_iface);
}

/* Caller must hold the stream lock. */
static void invalidate_object(SpatialAudioObjectImpl *object)
{
    if(object->invalidated)
        return;
    object->invalidated = TRUE;
    if(object->type == AudioObjectType_Dynamic)
        --object->sa_stream->active_dynamic_count;
}

static HRESULT WINAPI SAO_QueryInterface(ISpatialAudioObject *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObject)) {
        *ppv = &This->ISpatialAudioObject_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAO_AddRef(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAO_Release(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if(!ref){
        EnterCriticalSection(&This->sa_stream->lock);
        if(This->type == AudioObjectType_Dynamic && !This->invalidated)
            --This->sa_stream->active_dynamic_count;
        list_remove(&This->entry);
        LeaveCriticalSection(&This->sa_stream->lock);

        ISpatialAudioObjectRenderStream_Release(&This->sa_stream->ISpatialAudioObjectRenderStream_iface);
        free(This->buf);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAO_GetBuffer(ISpatialAudioObject *iface,
        BYTE **buffer, UINT32 *bytes)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p, %p)\n", This, buffer, bytes);

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->invalidated){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    /* the object's lifetime starts at its first GetBuffer */
    This->updated = TRUE;
    This->lifetime_started = TRUE;

    *buffer = (BYTE *)This->buf;
    *bytes = This->sa_stream->update_frames *
        This->sa_stream->sa_client->object_fmtex.Format.nBlockAlign;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_SetEndOfStream(ISpatialAudioObject *iface, UINT32 frames)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    SpatialAudioStreamImpl *stream = This->sa_stream;

    if(!spatial_max_dynamic_objects()){
        FIXME("(%p)->(%u)\n", This, frames);
        return E_NOTIMPL;
    }

    TRACE("(%p)->(%u)\n", This, frames);

    EnterCriticalSection(&stream->lock);

    if(This->invalidated){
        LeaveCriticalSection(&stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    if(stream->update_frames == ~0){
        LeaveCriticalSection(&stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    /* the frames beyond the end of the stream are not rendered */
    if(frames < stream->update_frames)
        memset(This->buf + frames, 0,
                (stream->update_frames - frames) * stream->sa_client->object_fmtex.Format.nBlockAlign);

    invalidate_object(This);

    LeaveCriticalSection(&stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_IsActive(ISpatialAudioObject *iface, BOOL *active)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    if(!spatial_max_dynamic_objects()){
        FIXME("(%p)->(%p)\n", This, active);
        return E_NOTIMPL;
    }

    TRACE("(%p)->(%p)\n", This, active);

    EnterCriticalSection(&This->sa_stream->lock);
    *active = !This->invalidated;
    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_GetAudioObjectType(ISpatialAudioObject *iface,
        AudioObjectType *type)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, type);

    *type = This->type;

    return S_OK;
}

static HRESULT WINAPI SAO_SetPosition(ISpatialAudioObject *iface, float x,
        float y, float z)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    if(!spatial_max_dynamic_objects()){
        FIXME("(%p)->(%f, %f, %f)\n", This, x, y, z);
        return E_NOTIMPL;
    }

    TRACE("(%p)->(%f, %f, %f)\n", This, x, y, z);

    if(This->type != AudioObjectType_Dynamic)
        return SPTLAUDCLNT_E_PROPERTY_NOT_SUPPORTED;

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->invalidated){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    This->position[0] = x;
    This->position[1] = y;
    This->position[2] = z;
    This->dirty = TRUE;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_SetVolume(ISpatialAudioObject *iface, float vol)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    if(!spatial_max_dynamic_objects()){
        FIXME("(%p)->(%f)\n", This, vol);
        return E_NOTIMPL;
    }

    TRACE("(%p)->(%f)\n", This, vol);

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->invalidated){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    This->volume = vol < 0.0f ? 0.0f : vol;
    This->dirty = TRUE;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static ISpatialAudioObjectVtbl ISpatialAudioObject_vtbl = {
    SAO_QueryInterface,
    SAO_AddRef,
    SAO_Release,
    SAO_GetBuffer,
    SAO_SetEndOfStream,
    SAO_IsActive,
    SAO_GetAudioObjectType,
    SAO_SetPosition,
    SAO_SetVolume,
};

static HRESULT WINAPI SAORS_QueryInterface(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStreamBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)) {
        *ppv = &This->ISpatialAudioObjectRenderStream_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAORS_AddRef(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAORS_Release(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if(!ref){
        IAudioClient_Stop(This->client);
        if(This->update_frames != ~0 && This->update_frames > 0)
            IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        IAudioRenderClient_Release(This->render);
        IAudioClient_Release(This->client);
        if(This->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_Release(This->params.NotifyObject);
        free((void*)This->params.ObjectFormat);
        CloseHandle(This->params.EventHandle);
        DeleteCriticalSection(&This->lock);
        ISpatialAudioClient_Release(&This->sa_client->ISpatialAudioClient_iface);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAORS_GetAvailableDynamicObjectCount(
        ISpatialAudioObjectRenderStream *iface, UINT32 *count)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);

    if(!spatial_max_dynamic_objects()){
        FIXME("(%p)->(%p)\n", This, count);
        *count = 0;
        return S_OK;
    }

    TRACE("(%p)->(%p)\n", This, count);

    EnterCriticalSection(&This->lock);
    *count = This->params.MaxDynamicObjectCount - This->active_dynamic_count;
    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI SAORS_GetService(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **service)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    FIXME("(%p)->(%s, %p)\n", This, debugstr_guid(riid), service);
    return E_NOTIMPL;
}

static HRESULT WINAPI SAORS_Start(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Start(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Start failed: %08lx\n", hr);
        return hr;
    }

    EnterCriticalSection(&This->lock);
    This->started = TRUE;
    LeaveCriticalSection(&This->lock);

    /* The Windows pipeline requests the first update as soon as the stream
     * starts; the period timer's first tick can be up to a period away, and
     * some clients treat an event that late as a dead stream. */
    SetEvent(This->params.EventHandle);

    return S_OK;
}

static HRESULT WINAPI SAORS_Stop(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Stop(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Stop failed: %08lx\n", hr);
        return hr;
    }

    EnterCriticalSection(&This->lock);
    This->started = FALSE;
    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI SAORS_Reset(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;

    if(!spatial_max_dynamic_objects()){
        FIXME("(%p)->()\n", This);
        return E_NOTIMPL;
    }

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    if(This->started){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_STREAM_NOT_STOPPED;
    }

    /* flush pending data and revoke all active objects */
    LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry)
        invalidate_object(object);
    This->update_frames = ~0;

    LeaveCriticalSection(&This->lock);

    IAudioClient_Reset(This->client);

    return S_OK;
}

static HRESULT WINAPI SAORS_BeginUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface,
        UINT32 *dyn_count, UINT32 *frames)
{
    static BOOL fixme_once = FALSE;
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", This, dyn_count, frames);

    EnterCriticalSection(&This->lock);

    if(This->update_frames != ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    This->update_frames = This->period_frames;

    if(This->update_frames > 0){
        hr = IAudioRenderClient_GetBuffer(This->render, This->update_frames, (BYTE **)&This->buf);
        if(FAILED(hr)){
            WARN("GetBuffer failed: %08lx\n", hr);
            This->update_frames = ~0;
            LeaveCriticalSection(&This->lock);
            return hr;
        }

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            object->updated = FALSE;
            if(!object->invalidated)
                memset(object->buf, 0, This->update_frames * This->sa_client->object_fmtex.Format.nBlockAlign);
        }
    }else if (!fixme_once){
        fixme_once = TRUE;
        FIXME("Zero frame update.\n");
    }

    /* Unlike GetAvailableDynamicObjectCount, this reports how many dynamic
     * objects can be rendered in this pass, not the remaining activation
     * headroom. We render every activated object each pass, so the whole
     * budget is always available; engines treat 0 as a dead renderer. */
    if(spatial_max_dynamic_objects())
        *dyn_count = This->params.MaxDynamicObjectCount;
    else
        *dyn_count = 0;
    *frames = This->update_frames;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static void mix_static_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float *in = object->buf, *out;
    UINT32 i;
    if(object->static_idx == ~0 ||
            stream->static_object_map[object->static_idx] == ~0){
        WARN("Got unmapped static object?! Not mixing. Type: 0x%x\n", object->type);
        return;
    }
    out = stream->buf + stream->static_object_map[object->static_idx];
    for(i = 0; i < stream->update_frames; ++i){
        *out += *in * object->volume;
        ++in;
        out += stream->stream_fmtex.Format.nChannels;
    }
}

/* The listener faces the main (or height) ring of bed speakers from its
 * center; a ring is described by the azimuth of each speaker. The LFE
 * channel is not part of any ring and never receives object audio. */
static void spread_ring(SpatialAudioStreamImpl *stream, BOOL height, float scale, float *gains)
{
    WORD c, count = 0, nch = stream->stream_fmtex.Format.nChannels;

    for(c = 0; c < nch; ++c){
        const struct speaker_info *spk = stream->bed_speakers[c];
        if(!spk->lfe && spk->height == height)
            ++count;
    }
    if(!count)
        return;

    /* no meaningful direction, spread the power evenly over the ring */
    scale /= sqrtf(count);
    for(c = 0; c < nch; ++c){
        const struct speaker_info *spk = stream->bed_speakers[c];
        if(!spk->lfe && spk->height == height)
            gains[c] += scale;
    }
}

static void pan_ring(SpatialAudioStreamImpl *stream, BOOL height, float azimuth,
        float scale, float *gains)
{
    WORD chans[ARRAY_SIZE(speaker_info_table)];
    float azimuths[ARRAY_SIZE(speaker_info_table)];
    WORD c, i, j, count = 0, nch = stream->stream_fmtex.Format.nChannels;
    float a1, a2, f;

    /* collect the ring's channels, sorted by azimuth */
    for(c = 0; c < nch; ++c){
        const struct speaker_info *spk = stream->bed_speakers[c];
        if(spk->lfe || spk->height != height)
            continue;
        for(i = 0; i < count; ++i){
            if(spk->azimuth < azimuths[i])
                break;
        }
        for(j = count; j > i; --j){
            azimuths[j] = azimuths[j - 1];
            chans[j] = chans[j - 1];
        }
        azimuths[i] = spk->azimuth;
        chans[i] = c;
        ++count;
    }

    if(!count)
        return;
    if(count == 1){
        gains[chans[0]] += scale;
        return;
    }

    azimuth = fmodf(azimuth - azimuths[0], 360.0f);
    if(azimuth < 0.0f)
        azimuth += 360.0f;
    azimuth += azimuths[0];

    for(i = 0; i < count; ++i){
        a1 = azimuths[i];
        a2 = i + 1 < count ? azimuths[i + 1] : azimuths[0] + 360.0f;
        if(azimuth <= a2 || i == count - 1){
            /* constant-power pan between the two nearest speakers */
            f = a2 > a1 ? (azimuth - a1) / (a2 - a1) : 0.0f;
            gains[chans[i]] += scale * cosf(f * (float)M_PI / 2.0f);
            gains[chans[i + 1 < count ? i + 1 : 0]] += scale * sinf(f * (float)M_PI / 2.0f);
            return;
        }
    }
}

static void compute_dynamic_object_gains(SpatialAudioStreamImpl *stream,
        SpatialAudioObjectImpl *object, float *gains)
{
    /* Windows' spatial sound coordinates are relative to the listener:
     * +x to the right, +y up, -z ahead */
    float x = object->position[0], y = object->position[1], z = object->position[2];
    float radius = sqrtf(x * x + z * z);
    float main_scale = 1.0f, height_scale = 0.0f;
    WORD c, nch = stream->stream_fmtex.Format.nChannels;
    float azimuth, elevation, t;

    for(c = 0; c < nch; ++c)
        gains[c] = 0.0f;

    /* if the bed has no height channels, elevation is collapsed onto the
     * main ring instead of splitting the power between the rings */
    if(stream->has_height && (radius > 0.0f || y > 0.0f)){
        elevation = atan2f(y, radius);
        if(elevation > 0.0f){
            /* the height ring receives all of the power at 45 degrees up */
            t = elevation / ((float)M_PI / 4.0f);
            if(t > 1.0f)
                t = 1.0f;
            main_scale = cosf(t * (float)M_PI / 2.0f);
            height_scale = sinf(t * (float)M_PI / 2.0f);
        }
    }

    if(radius < 1e-5f){
        if(main_scale > 0.0f)
            spread_ring(stream, FALSE, main_scale, gains);
        if(height_scale > 0.0f)
            spread_ring(stream, TRUE, height_scale, gains);
    }else{
        azimuth = atan2f(x, -z) * 180.0f / (float)M_PI;
        if(main_scale > 0.0f)
            pan_ring(stream, FALSE, azimuth, main_scale, gains);
        if(height_scale > 0.0f)
            pan_ring(stream, TRUE, azimuth, height_scale, gains);
    }

    for(c = 0; c < nch; ++c)
        gains[c] *= object->volume;
}

static void mix_dynamic_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    WORD c, nch = stream->stream_fmtex.Format.nChannels;
    float *in = object->buf, *out = stream->buf;
    float step = 1.0f / stream->update_frames;
    float f = 0.0f;
    UINT32 i;

    if(object->dirty){
        compute_dynamic_object_gains(stream, object, object->gain_tgt);
        object->dirty = FALSE;
    }

    /* don't ramp from the initial gains of a freshly activated object */
    if(!object->mixed)
        memcpy(object->gain_cur, object->gain_tgt, sizeof(object->gain_cur));

    /* ramp the gains linearly over the update to avoid zipper noise */
    for(i = 0; i < stream->update_frames; ++i){
        f += step;
        for(c = 0; c < nch; ++c)
            out[c] += *in * (object->gain_cur[c] + (object->gain_tgt[c] - object->gain_cur[c]) * f);
        ++in;
        out += nch;
    }

    memcpy(object->gain_cur, object->gain_tgt, sizeof(object->gain_cur));
    object->mixed = TRUE;
}

static HRESULT WINAPI SAORS_EndUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    if(This->update_frames == ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->update_frames > 0){
        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            if(spatial_max_dynamic_objects() && !object->updated){
                /* objects which miss an update cycle after their lifetime has
                 * started are deactivated; never-fed objects stay dormant */
                if(object->lifetime_started)
                    invalidate_object(object);
                continue;
            }
            if(object->type == AudioObjectType_Dynamic ||
                    object->type == AudioObjectType_None)
                mix_dynamic_object(This, object);
            else
                mix_static_object(This, object);
        }

        hr = IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        if(FAILED(hr))
            WARN("ReleaseBuffer failed: %08lx\n", hr);

        /* Windows keeps requesting updates back to back until the stream
         * buffer is full; only then does pacing fall to the period clock. */
        if(SUCCEEDED(hr)){
            UINT32 pad = 0;
            if(SUCCEEDED(IAudioClient_GetCurrentPadding(This->client, &pad)) &&
                    pad + This->period_frames <= This->buffer_frames)
                SetEvent(This->params.EventHandle);
        }
    }

    This->update_frames = ~0;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI SAORS_ActivateSpatialAudioObject(ISpatialAudioObjectRenderStream *iface,
        AudioObjectType type, ISpatialAudioObject **object)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *obj;

    TRACE("(%p)->(0x%x, %p)\n", This, type, object);

    if(type == AudioObjectType_Dynamic){
        EnterCriticalSection(&This->lock);
        if(!spatial_max_dynamic_objects() ||
                This->active_dynamic_count >= This->params.MaxDynamicObjectCount){
            LeaveCriticalSection(&This->lock);
            return SPTLAUDCLNT_E_NO_MORE_OBJECTS;
        }
        ++This->active_dynamic_count;
        LeaveCriticalSection(&This->lock);
    }else{
        if(type & ~This->params.StaticObjectTypeMask)
            return SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;

        LIST_FOR_EACH_ENTRY(obj, &This->objects, SpatialAudioObjectImpl, entry){
            if(obj->static_idx == AudioObjectType_to_index(type))
                return SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE;
        }
    }

    obj = calloc(1, sizeof(*obj));
    obj->ISpatialAudioObject_iface.lpVtbl = &ISpatialAudioObject_vtbl;
    obj->ref = 1;
    obj->type = type;
    obj->updated = TRUE;
    obj->dirty = TRUE;
    obj->volume = 1.0f;
    if(type == AudioObjectType_Dynamic){
        obj->static_idx = ~0;
    }else if(type == AudioObjectType_None){
        /* rendered without spatialization: mixed evenly over the main ring */
        obj->static_idx = ~0;
    }else{
        obj->static_idx = AudioObjectType_to_index(type);
    }

    obj->sa_stream = This;
    SAORS_AddRef(&This->ISpatialAudioObjectRenderStream_iface);

    obj->buf = calloc(This->period_frames, This->sa_client->object_fmtex.Format.nBlockAlign);

    EnterCriticalSection(&This->lock);

    list_add_tail(&This->objects, &obj->entry);

    LeaveCriticalSection(&This->lock);

    *object = &obj->ISpatialAudioObject_iface;

    return S_OK;
}

static ISpatialAudioObjectRenderStreamVtbl ISpatialAudioObjectRenderStream_vtbl = {
    SAORS_QueryInterface,
    SAORS_AddRef,
    SAORS_Release,
    SAORS_GetAvailableDynamicObjectCount,
    SAORS_GetService,
    SAORS_Start,
    SAORS_Stop,
    SAORS_Reset,
    SAORS_BeginUpdatingAudioObjects,
    SAORS_EndUpdatingAudioObjects,
    SAORS_ActivateSpatialAudioObject,
};

static HRESULT WINAPI SAC_QueryInterface(ISpatialAudioClient *iface, REFIID riid, void **ppv)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioClient)) {
        *ppv = &This->ISpatialAudioClient_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAC_AddRef(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAC_Release(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if (!ref) {
        IMMDevice_Release(This->mmdev);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAC_GetStaticObjectPosition(ISpatialAudioClient *iface,
        AudioObjectType type, float *x, float *y, float *z)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    FIXME("(%p)->(0x%x, %p, %p, %p)\n", This, type, x, y, z);
    return E_NOTIMPL;
}

static HRESULT WINAPI SAC_GetNativeStaticObjectTypeMask(ISpatialAudioClient *iface,
        AudioObjectType *mask)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    UINT32 native = AudioObjectType_None, i;

    if(!spatial_max_dynamic_objects()){
        FIXME("(%p)->(%p)\n", This, mask);
        return E_NOTIMPL;
    }

    TRACE("(%p)->(%p)\n", This, mask);

    for(i = 0; i < ARRAY_SIZE(speaker_info_table); ++i){
        if(This->native_channel_mask & speaker_info_table[i].speaker)
            native |= speaker_info_table[i].type;
    }
    *mask = native;

    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxDynamicObjectCount(ISpatialAudioClient *iface,
        UINT32 *value)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, value);

    *value = spatial_max_dynamic_objects();

    return S_OK;
}

static HRESULT WINAPI SAC_GetSupportedAudioObjectFormatEnumerator(
        ISpatialAudioClient *iface, IAudioFormatEnumerator **enumerator)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, enumerator);

    *enumerator = &This->IAudioFormatEnumerator_iface;
    SAC_AddRef(iface);

    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxFrameCount(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    /* FIXME: should get device period from the device */
    static const REFERENCE_TIME period = 100000;

    TRACE("(%p)->(%p, %p)\n", This, format, count);

    *count = MulDiv(period, format->nSamplesPerSec, 10000000);

    return S_OK;
}

static HRESULT WINAPI SAC_IsAudioObjectFormatSupported(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format)
{
    SpatialAudioImpl *sac = impl_from_ISpatialAudioClient(iface);

    TRACE("sac %p, format %s.\n", sac, debugstr_fmtex(format));

    if (!format)
        return E_POINTER;

    if (!formats_equal(&sac->object_fmtex.Format, format))
    {
        FIXME("Reporting format %s as unsupported.\n", debugstr_fmtex(format));
        return E_INVALIDARG;
    }

    return S_OK;
}

static HRESULT WINAPI SAC_IsSpatialAudioStreamAvailable(ISpatialAudioClient *iface,
        REFIID stream_uuid, const PROPVARIANT *info)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    if(!spatial_max_dynamic_objects()){
        FIXME("(%p)->(%s, %p)\n", This, debugstr_guid(stream_uuid), info);
        return E_NOTIMPL;
    }

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(stream_uuid), info);

    if(IsEqualIID(stream_uuid, &IID_ISpatialAudioObjectRenderStream))
        return S_OK;

    FIXME("Stream type %s not supported\n", debugstr_guid(stream_uuid));
    return SPTLAUDCLNT_E_STREAM_NOT_AVAILABLE;
}

static WAVEFORMATEX *clone_fmtex(const WAVEFORMATEX *src)
{
    WAVEFORMATEX *r = malloc(sizeof(WAVEFORMATEX) + src->cbSize);
    memcpy(r, src, sizeof(WAVEFORMATEX) + src->cbSize);
    return r;
}

static void static_mask_to_channels(AudioObjectType static_mask, WORD *count, DWORD *mask, UINT32 *map)
{
    UINT32 out_chan = 0, map_idx = 0;
    *count = 0;
    *mask = 0;
#define CONVERT_MASK(f, t) \
    if(static_mask & f){ \
        *count += 1; \
        *mask |= t; \
        map[map_idx++] = out_chan++; \
        TRACE("mapping 0x%x to %u\n", f, out_chan - 1); \
    }else{ \
        map[map_idx++] = ~0; \
    }
    CONVERT_MASK(AudioObjectType_FrontLeft, SPEAKER_FRONT_LEFT);
    CONVERT_MASK(AudioObjectType_FrontRight, SPEAKER_FRONT_RIGHT);
    CONVERT_MASK(AudioObjectType_FrontCenter, SPEAKER_FRONT_CENTER);
    CONVERT_MASK(AudioObjectType_LowFrequency, SPEAKER_LOW_FREQUENCY);
    CONVERT_MASK(AudioObjectType_SideLeft, SPEAKER_SIDE_LEFT);
    CONVERT_MASK(AudioObjectType_SideRight, SPEAKER_SIDE_RIGHT);
    CONVERT_MASK(AudioObjectType_BackLeft, SPEAKER_BACK_LEFT);
    CONVERT_MASK(AudioObjectType_BackRight, SPEAKER_BACK_RIGHT);
    CONVERT_MASK(AudioObjectType_TopFrontLeft, SPEAKER_TOP_FRONT_LEFT);
    CONVERT_MASK(AudioObjectType_TopFrontRight, SPEAKER_TOP_FRONT_RIGHT);
    CONVERT_MASK(AudioObjectType_TopBackLeft, SPEAKER_TOP_BACK_LEFT);
    CONVERT_MASK(AudioObjectType_TopBackRight, SPEAKER_TOP_BACK_RIGHT);
    CONVERT_MASK(AudioObjectType_BackCenter, SPEAKER_BACK_CENTER);
}

/* Lay out the stream channels in channel mask bit order, as expected by the
 * audio backend. When the stream renders dynamic objects the bed is widened
 * to cover the endpoint's native channels, so that objects can be panned
 * over the full speaker layout. */
static void build_stream_channels(SpatialAudioStreamImpl *stream)
{
    DWORD bed_mask = 0;
    WORD count = 0;
    UINT32 i;

    for(i = 0; i < ARRAY_SIZE(speaker_info_table); ++i){
        if(stream->params.StaticObjectTypeMask & speaker_info_table[i].type)
            bed_mask |= speaker_info_table[i].speaker;
    }

    if(stream->params.MaxDynamicObjectCount > 0){
        for(i = 0; i < ARRAY_SIZE(speaker_info_table); ++i){
            if(stream->sa_client->native_channel_mask & speaker_info_table[i].speaker)
                bed_mask |= speaker_info_table[i].speaker;
        }
        if(!(bed_mask & ~SPEAKER_LOW_FREQUENCY))
            bed_mask |= SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }

    for(i = 0; i < ARRAY_SIZE(stream->static_object_map); ++i)
        stream->static_object_map[i] = ~0;

    stream->has_height = FALSE;
    for(i = 0; i < ARRAY_SIZE(speaker_info_table); ++i){
        const struct speaker_info *spk = &speaker_info_table[i];
        if(!(bed_mask & spk->speaker))
            continue;
        stream->bed_speakers[count] = spk;
        if(spk->height)
            stream->has_height = TRUE;
        if(stream->params.StaticObjectTypeMask & spk->type){
            stream->static_object_map[AudioObjectType_to_index(spk->type)] = count;
            TRACE("mapping 0x%x to %u\n", spk->type, count);
        }
        ++count;
    }

    stream->stream_fmtex.Format.nChannels = count;
    stream->stream_fmtex.dwChannelMask = bed_mask;
}

static HRESULT activate_stream(SpatialAudioStreamImpl *stream)
{
    WAVEFORMATEXTENSIBLE *object_fmtex = (WAVEFORMATEXTENSIBLE *)stream->params.ObjectFormat;
    HRESULT hr;
    REFERENCE_TIME period;

    if(!(object_fmtex->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (object_fmtex->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 IsEqualGUID(&object_fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)))){
        FIXME("Only float formats are supported for now\n");
        return E_INVALIDARG;
    }

    hr = IMMDevice_Activate(stream->sa_client->mmdev, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&stream->client);
    if(FAILED(hr)){
        WARN("Activate failed: %08lx\n", hr);
        return hr;
    }

    hr = IAudioClient_GetDevicePeriod(stream->client, &period, NULL);
    if(FAILED(hr)){
        WARN("GetDevicePeriod failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    stream->stream_fmtex.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    if(spatial_max_dynamic_objects())
        build_stream_channels(stream);
    else
        static_mask_to_channels(stream->params.StaticObjectTypeMask,
                &stream->stream_fmtex.Format.nChannels, &stream->stream_fmtex.dwChannelMask,
                stream->static_object_map);
    stream->stream_fmtex.Format.nSamplesPerSec = stream->params.ObjectFormat->nSamplesPerSec;
    stream->stream_fmtex.Format.wBitsPerSample = stream->params.ObjectFormat->wBitsPerSample;
    stream->stream_fmtex.Format.nBlockAlign = (stream->stream_fmtex.Format.nChannels * stream->stream_fmtex.Format.wBitsPerSample) / 8;
    stream->stream_fmtex.Format.nAvgBytesPerSec = stream->stream_fmtex.Format.nSamplesPerSec * stream->stream_fmtex.Format.nBlockAlign;
    stream->stream_fmtex.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    stream->stream_fmtex.Samples.wValidBitsPerSample = stream->stream_fmtex.Format.wBitsPerSample;
    stream->stream_fmtex.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    hr = IAudioClient_Initialize(stream->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            period, 0, &stream->stream_fmtex.Format, NULL);
    if(FAILED(hr)){
        WARN("Initialize failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_SetEventHandle(stream->client, stream->params.EventHandle);
    if(FAILED(hr)){
        WARN("SetEventHandle failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_GetService(stream->client, &IID_IAudioRenderClient, (void**)&stream->render);
    if(FAILED(hr)){
        WARN("GetService(AudioRenderClient) failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    stream->period_frames = MulDiv(period, stream->stream_fmtex.Format.nSamplesPerSec, 10000000);

    hr = IAudioClient_GetBufferSize(stream->client, &stream->buffer_frames);
    if(FAILED(hr)){
        WARN("GetBufferSize failed: %08lx\n", hr);
        stream->buffer_frames = 3 * stream->period_frames;
    }

    return S_OK;
}

static HRESULT WINAPI SAC_ActivateSpatialAudioStream(ISpatialAudioClient *iface,
        const PROPVARIANT *prop, REFIID riid, void **stream)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    SpatialAudioObjectRenderStreamActivationParams *params;
    HRESULT hr;

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), stream);

    if(IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)){
        UINT32 max_dynamic = spatial_max_dynamic_objects();
        SpatialAudioStreamImpl *obj;

        if(!prop || prop->vt != VT_BLOB ||
                prop->blob.cbSize != sizeof(SpatialAudioObjectRenderStreamActivationParams) ||
                !prop->blob.pBlobData){
            WARN("Got invalid params\n");
            *stream = NULL;
            return E_INVALIDARG;
        }

        params = (SpatialAudioObjectRenderStreamActivationParams*) prop->blob.pBlobData;

        if(params->StaticObjectTypeMask & AudioObjectType_Dynamic){
            *stream = NULL;
            return E_INVALIDARG;
        }

        if(params->EventHandle == INVALID_HANDLE_VALUE ||
                params->EventHandle == 0){
            *stream = NULL;
            return E_INVALIDARG;
        }

        if(!(params->ObjectFormat && formats_equal(params->ObjectFormat, &This->object_fmtex.Format))) {
            *stream = NULL;
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }

        if(max_dynamic){
            if(params->MinDynamicObjectCount > params->MaxDynamicObjectCount){
                *stream = NULL;
                return E_INVALIDARG;
            }
            if(params->MinDynamicObjectCount > max_dynamic){
                WARN("Requested too many dynamic objects: %u\n", params->MinDynamicObjectCount);
                *stream = NULL;
                return SPTLAUDCLNT_E_NO_MORE_OBJECTS;
            }
        }

        obj = calloc(1, sizeof(SpatialAudioStreamImpl));

        obj->ISpatialAudioObjectRenderStream_iface.lpVtbl = &ISpatialAudioObjectRenderStream_vtbl;
        obj->ref = 1;
        memcpy(&obj->params, params, sizeof(obj->params));
        if(obj->params.MaxDynamicObjectCount > max_dynamic)
            obj->params.MaxDynamicObjectCount = max_dynamic;

        obj->update_frames = ~0;

        InitializeCriticalSection(&obj->lock);
        list_init(&obj->objects);

        obj->sa_client = This;
        SAC_AddRef(&This->ISpatialAudioClient_iface);

        obj->params.ObjectFormat = clone_fmtex(obj->params.ObjectFormat);

        DuplicateHandle(GetCurrentProcess(), obj->params.EventHandle,
                GetCurrentProcess(), &obj->params.EventHandle, 0, FALSE,
                DUPLICATE_SAME_ACCESS);

        if(obj->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_AddRef(obj->params.NotifyObject);

        if(TRACE_ON(mmdevapi)){
            TRACE("ObjectFormat: {%s}\n", debugstr_fmtex(obj->params.ObjectFormat));
            TRACE("StaticObjectTypeMask: 0x%x\n", obj->params.StaticObjectTypeMask);
            TRACE("MinDynamicObjectCount: 0x%x\n", obj->params.MinDynamicObjectCount);
            TRACE("MaxDynamicObjectCount: 0x%x\n", obj->params.MaxDynamicObjectCount);
            TRACE("Category: 0x%x\n", obj->params.Category);
            TRACE("EventHandle: %p\n", obj->params.EventHandle);
            TRACE("NotifyObject: %p\n", obj->params.NotifyObject);
        }

        hr = activate_stream(obj);
        if(FAILED(hr)){
            if(obj->params.NotifyObject)
                ISpatialAudioObjectRenderStreamNotify_Release(obj->params.NotifyObject);
            DeleteCriticalSection(&obj->lock);
            free((void*)obj->params.ObjectFormat);
            CloseHandle(obj->params.EventHandle);
            ISpatialAudioClient_Release(&obj->sa_client->ISpatialAudioClient_iface);
            free(obj);
            *stream = NULL;
            return hr;
        }

        *stream = &obj->ISpatialAudioObjectRenderStream_iface;
    }else{
        FIXME("Unsupported audio stream IID: %s\n", debugstr_guid(riid));
        *stream = NULL;
        return E_NOTIMPL;
    }

    return S_OK;
}

static ISpatialAudioClientVtbl ISpatialAudioClient_vtbl = {
    SAC_QueryInterface,
    SAC_AddRef,
    SAC_Release,
    SAC_GetStaticObjectPosition,
    SAC_GetNativeStaticObjectTypeMask,
    SAC_GetMaxDynamicObjectCount,
    SAC_GetSupportedAudioObjectFormatEnumerator,
    SAC_GetMaxFrameCount,
    SAC_IsAudioObjectFormatSupported,
    SAC_IsSpatialAudioStreamAvailable,
    SAC_ActivateSpatialAudioStream,
};

static HRESULT WINAPI SAOFE_QueryInterface(IAudioFormatEnumerator *iface,
        REFIID riid, void **ppvObject)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_QueryInterface(&This->ISpatialAudioClient_iface, riid, ppvObject);
}

static ULONG WINAPI SAOFE_AddRef(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_AddRef(&This->ISpatialAudioClient_iface);
}

static ULONG WINAPI SAOFE_Release(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_Release(&This->ISpatialAudioClient_iface);
}

static HRESULT WINAPI SAOFE_GetCount(IAudioFormatEnumerator *iface, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%p)\n", This, count);

    *count = 1;

    return S_OK;
}

static HRESULT WINAPI SAOFE_GetFormat(IAudioFormatEnumerator *iface,
        UINT32 index, WAVEFORMATEX **format)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%u, %p)\n", This, index, format);

    if(index > 0)
        return E_INVALIDARG;

    *format = &This->object_fmtex.Format;

    return S_OK;
}

static IAudioFormatEnumeratorVtbl IAudioFormatEnumerator_vtbl = {
    SAOFE_QueryInterface,
    SAOFE_AddRef,
    SAOFE_Release,
    SAOFE_GetCount,
    SAOFE_GetFormat,
};

HRESULT SpatialAudioClient_Create(IMMDevice *mmdev, ISpatialAudioClient **out)
{
    SpatialAudioImpl *obj;
    IAudioClient *aclient;
    WAVEFORMATEX *closest, *mix;
    HRESULT hr;

    obj = calloc(1, sizeof(*obj));

    obj->ref = 1;
    obj->ISpatialAudioClient_iface.lpVtbl = &ISpatialAudioClient_vtbl;
    obj->IAudioFormatEnumerator_iface.lpVtbl = &IAudioFormatEnumerator_vtbl;

    obj->object_fmtex.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    obj->object_fmtex.Format.nChannels = 1;
    obj->object_fmtex.Format.nSamplesPerSec = 48000;
    obj->object_fmtex.Format.wBitsPerSample = sizeof(float) * 8;
    obj->object_fmtex.Format.nBlockAlign = (obj->object_fmtex.Format.nChannels * obj->object_fmtex.Format.wBitsPerSample) / 8;
    obj->object_fmtex.Format.nAvgBytesPerSec = obj->object_fmtex.Format.nSamplesPerSec * obj->object_fmtex.Format.nBlockAlign;
    obj->object_fmtex.Format.cbSize = 0;

    hr = IMMDevice_Activate(mmdev, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&aclient);
    if(FAILED(hr)){
        WARN("Activate failed: %08lx\n", hr);
        free(obj);
        return hr;
    }

    hr = IAudioClient_GetMixFormat(aclient, &mix);
    if(SUCCEEDED(hr)){
        if(mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            obj->native_channel_mask = ((WAVEFORMATEXTENSIBLE *)mix)->dwChannelMask;
        else if(mix->nChannels == 1)
            obj->native_channel_mask = SPEAKER_FRONT_CENTER;
        CoTaskMemFree(mix);
    }else{
        WARN("GetMixFormat failed: %08lx\n", hr);
    }
    if(!obj->native_channel_mask)
        obj->native_channel_mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    hr = IAudioClient_IsFormatSupported(aclient, AUDCLNT_SHAREMODE_SHARED, &obj->object_fmtex.Format, &closest);

    IAudioClient_Release(aclient);

    if(hr == S_FALSE){
        if(sizeof(WAVEFORMATEX) + closest->cbSize > sizeof(obj->object_fmtex)){
            ERR("Returned format too large: %s\n", debugstr_fmtex(closest));
            CoTaskMemFree(closest);
            free(obj);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }else if(!((closest->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (closest->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)closest)->SubFormat,
                         &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))) &&
                    closest->wBitsPerSample == 32)){
            ERR("Returned format not 32-bit float: %s\n", debugstr_fmtex(closest));
            CoTaskMemFree(closest);
            free(obj);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        WARN("The audio stack doesn't support 48kHz 32bit float. Using the closest match. Audio may be glitchy. %s\n", debugstr_fmtex(closest));
        memcpy(&obj->object_fmtex,
               closest,
               sizeof(WAVEFORMATEX) + closest->cbSize);
        CoTaskMemFree(closest);
    } else if(hr != S_OK){
        WARN("Checking supported formats failed: %08lx\n", hr);
        free(obj);
        return hr;
    }

    obj->mmdev = mmdev;
    IMMDevice_AddRef(mmdev);

    *out = &obj->ISpatialAudioClient_iface;

    return S_OK;
}
