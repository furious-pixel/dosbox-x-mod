/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


/* 
    Remove the sdl code from here and have it handled in sdlmain.
    That should call the mixer start from there or something.
*/

#include <assert.h>
#include <string.h>
#include <sys/types.h>
#define _USE_MATH_DEFINES // needed for M_PI in Visual Studio as documented [https://msdn.microsoft.com/en-us/library/4hwaceh6.aspx]
#include <math.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>

#if defined(_MSC_VER)
# pragma warning(disable:4244) /* const fmath::local::uint64_t to double possible loss of data */
#endif

#if defined (WIN32)
//Midi listing
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#if !defined(M_PI)
# define M_PI (3.141592654)
#endif

#include "SDL.h"
#include "mem.h"
#include "pic.h"
#include "dosbox.h"
#include "logging.h"
#include "mixer.h"
#include "cpu.h"
#include "mod.h"
#include "timer.h"
#include "setup.h"
#include "cross.h"
#include "support.h"
#include "control.h"
#include "mapper.h"
#include "hardware.h"
#include "programs.h"
#include "midi.h"

#define MIXER_SSIZE 4
#define MIXER_VOLSHIFT 13

#ifdef C_SDL2
SDL_AudioDeviceID SDL2_AudioDevice = 0; /* valid IDs are 2 or higher, 1 for compat, 0 is never a valid ID */
#endif

static INLINE int16_t MIXER_CLIP(Bits SAMP) {
    if (SAMP < MAX_AUDIO) {
        if (SAMP > MIN_AUDIO)
            return SAMP;
        else
            return MIN_AUDIO;
    } else {
        return MAX_AUDIO;
    }
}

struct mixedFraction {
    unsigned int        w;
    unsigned int        fn,fd;
};

static struct {
    int32_t          work[MIXER_BUFSIZE][2];
    Bitu            work_in,work_out,work_wrap;
    Bitu            pos,done;
    float           mastervol[2];
    float           recordvol[2];
    MixerChannel*   channels;
    uint32_t          freq;
    uint32_t          blocksize;
    struct mixedFraction samples_per_ms;
    struct mixedFraction samples_this_ms;
    struct mixedFraction samples_rendered_ms;
    bool            nosound;
    bool            swapstereo;
    bool            sampleaccurate;
    bool            prebuffer_wait;
    Bitu            prebuffer_samples;
    bool            mute;
} mixer;

// Opt-in diagnostic only. Audio fields are protected by SDL's existing audio
// lock; the callback never formats, writes files, allocates, or adds a lock.
bool MIXER_TimingAuditEnabled = false;
namespace {
struct AudioAuditEvent {
    uint64_t time = 0, production_gap = 0;
    unsigned queued = 0, requested = 0, silence = 0, dropped = 0;
    bool prebuffer = false, muted = false, underrun = false;
};
struct AudioAuditWindow {
    uint64_t callbacks = 0, underruns = 0, silence = 0, dropped = 0;
    uint64_t productions = 0, max_gap = 0;
    unsigned min_queued = UINT32_MAX;
    std::array<AudioAuditEvent, 64> events = {};
};
struct SwapAuditEvent { uint64_t start = 0, end = 0; };
AudioAuditWindow audio_audit;
FILE *audit_log = nullptr;
uint64_t audit_last_production = 0;
uint64_t audit_started = 0, audit_reported = 0, audit_swaps = 0;
uint64_t audit_swap_total = 0, audit_swap_max = 0, audit_discarded = 0;
uint64_t audit_last_present = 0, audit_present_gap_max = 0;
double audit_emu = 0;
std::array<SwapAuditEvent, 128> audit_swap_events = {};
struct TickAuditEvent {
    uint64_t end = 0, gap = 0, update = 0, delivered = 0, swaps = 0;
    uint64_t swap_ns = 0, presents = 0, present_ns = 0, discarded = 0;
    uint32_t previous_budget = 0, pending = 0, debt = 0, next_budget = 0, flags = 0;
    int64_t cycles_before = 0, cycles_after = 0;
    uint64_t excluded_before = 0, excluded_after = 0;
    bool automatic = false;
};
TickAuditEvent audit_interval;
std::array<TickAuditEvent, 4> audit_longest_ticks = {};
uint64_t audit_tick_updates = 0, audit_cycle_changes = 0;
unsigned audit_transitions = 0, audit_last_view_state = UINT32_MAX;
uint32_t audit_last_pacing_flags = UINT32_MAX;
int32_t audit_native_result = INT32_MIN;
unsigned audit_native_flags = UINT32_MAX;

void audit_state_change(const char *kind, int64_t value, unsigned flags)
{
    if (audit_transitions++ < 8)
        MIXER_TimingAuditLog("AV_AUDIT state t_ms=%.3f kind=%s value=%lld flags=%u",
                (MIXER_TimingAuditNow()-audit_started)/1e6, kind, (long long)value, flags);
}
}

// Main thread only. Unlike LOG_MSG, this does not yield guest CPU cycles.
void MIXER_TimingAuditLog(const char *format, ...)
{
    if (!audit_log) {
        const char *enabled = std::getenv("MW2_AV_AUDIT");
        if (!enabled || strcmp(enabled, "1")) return;
        const char *path = std::getenv("MW2_AV_AUDIT_LOG");
        audit_log = fopen(path && *path ? path : "av_timing.log", "a");
        if (!audit_log) return;
    }
    va_list args;
    va_start(args, format);
    vfprintf(audit_log, format, args);
    va_end(args);
    fputc('\n', audit_log);
}

uint64_t MIXER_TimingAuditNow()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void MIXER_TimingAuditTicks(uint32_t discarded)
{
    audit_discarded += discarded;
    audit_interval.discarded += discarded;
}

void MIXER_TimingAuditSwap(uint64_t start)
{
    const uint64_t end = MIXER_TimingAuditNow();
    audit_swap_events[audit_swaps++ % audit_swap_events.size()] = {start, end};
    ++audit_interval.swaps;
    audit_interval.swap_ns += end - start;
    audit_swap_total += end - start;
    audit_swap_max = std::max(audit_swap_max, end - start);
    if (audit_last_present)
        audit_present_gap_max = std::max(audit_present_gap_max, end - audit_last_present);
    audit_last_present = end;
}

void MIXER_TimingAuditDeliveredTick()
{
    ++audit_interval.delivered;
}

void MIXER_TimingAuditTickUpdate(uint64_t start, uint32_t pending_before,
        uint32_t debt_before, uint32_t pending_after, int64_t cycles_before,
        uint64_t excluded_before, uint64_t excluded_after)
{
    const uint64_t end = MIXER_TimingAuditNow();
    TickAuditEvent event = audit_interval;
    event.end = end;
    event.gap = audit_interval.end ? end-audit_interval.end : 0;
    event.update = end-start;
    event.pending = pending_before;
    event.debt = debt_before;
    event.next_budget = pending_after;
    event.cycles_before = cycles_before;
    event.cycles_after = CPU_CycleMax;
    event.excluded_before = excluded_before;
    event.excluded_after = excluded_after;
    event.automatic = CPU_CycleAutoAdjust;
    event.flags = MOD_FramePacingAuditFlags();
    ++audit_tick_updates;
    if (cycles_before != CPU_CycleMax) ++audit_cycle_changes;
    if ((event.flags & ~4u) != audit_last_pacing_flags) {
        audit_state_change("pacer", 0, event.flags);
        audit_last_pacing_flags = event.flags & ~4u;
    }
    auto *shortest = &audit_longest_ticks[0];
    for (auto &record : audit_longest_ticks)
        if (record.gap < shortest->gap) shortest = &record;
    if (event.gap > shortest->gap) *shortest = event;
    audit_interval = {};
    audit_interval.end = end;
    audit_interval.previous_budget = pending_after;
}

void MIXER_TimingAuditPresentation(uint64_t start, unsigned view, bool active, bool invoked)
{
    ++audit_interval.presents;
    audit_interval.present_ns += MIXER_TimingAuditNow()-start;
    const unsigned state = view | (active ? 256u : 0u) | (invoked ? 512u : 0u);
    if (state != audit_last_view_state) {
        audit_state_change("presentation", view, state);
        audit_last_view_state = state;
    }
}

void MIXER_TimingAuditNativeState(int32_t result, unsigned flags)
{
    if (result != audit_native_result || flags != audit_native_flags) {
        audit_state_change("native_compositor", result, flags);
        audit_native_result = result;
        audit_native_flags = flags;
    }
}

void MIXER_TimingAuditService(bool final)
{
    if (!MIXER_TimingAuditEnabled)
        return;
    const uint64_t now = MIXER_TimingAuditNow();
    if (!final && now - audit_reported < 1000000000ull)
        return;
#if C_SDL2
    if (SDL2_AudioDevice) SDL_LockAudioDevice(SDL2_AudioDevice);
#else
    SDL_LockAudio();
#endif
    const AudioAuditWindow snapshot = audio_audit;
    audio_audit = {};
#if C_SDL2
    if (SDL2_AudioDevice) SDL_UnlockAudioDevice(SDL2_AudioDevice);
#else
    SDL_UnlockAudio();
#endif
    const double emu = PIC_FullIndex();
    const double wall_ms = (now - audit_reported) / 1e6;
    MIXER_TimingAuditLog("AV_AUDIT window t_ms=%.3f final=%u wall_ms=%.3f emu_ms=%.3f emu_ratio=%.4f swaps=%llu swap_avg_ms=%.3f swap_max_ms=%.3f present_gap_max_ms=%.3f discarded_ticks=%llu productions=%llu production_gap_max_ms=%.3f callbacks=%llu min_queued=%u underruns=%llu silence_samples=%llu dropped_samples=%llu",
            (now-audit_started)/1e6, (unsigned)final, wall_ms, emu-audit_emu,
            wall_ms > 0 ? (emu-audit_emu)/wall_ms : 0,
            (unsigned long long)audit_swaps, audit_swaps ? audit_swap_total/1e6/audit_swaps : 0,
            audit_swap_max/1e6, audit_present_gap_max/1e6,
            (unsigned long long)audit_discarded, (unsigned long long)snapshot.productions,
            snapshot.max_gap/1e6, (unsigned long long)snapshot.callbacks,
            snapshot.callbacks ? snapshot.min_queued : 0,
            (unsigned long long)snapshot.underruns, (unsigned long long)snapshot.silence,
            (unsigned long long)snapshot.dropped);
    // Limit detail output to four underruns per window to avoid console I/O
    // becoming the source of new stalls. Rings overwrite oldest events.
    unsigned details = 0;
    const uint64_t first = snapshot.callbacks > snapshot.events.size()
            ? snapshot.callbacks - snapshot.events.size() : 0;
    const uint64_t first_swap = audit_swaps > audit_swap_events.size()
            ? audit_swaps - audit_swap_events.size() : 0;
    for (uint64_t i = first; i < snapshot.callbacks && details < 4; ++i) {
        const auto &e = snapshot.events[i % snapshot.events.size()];
        if (!e.underrun) continue;
        ++details;
        MIXER_TimingAuditLog("AV_AUDIT underrun t_ms=%.3f production_age_ms=%.3f queued=%u requested=%u silence=%u dropped=%u prebuffer=%u muted=%u",
                (e.time-audit_started)/1e6, e.production_gap/1e6,
                e.queued, e.requested, e.silence, e.dropped,
                (unsigned)e.prebuffer, (unsigned)e.muted);
        // Most recent swap begun before this callback, including a swap
        // still in progress at callback time. Same clock on both threads.
        for (uint64_t j = audit_swaps; j > first_swap; --j) {
            const auto &swap = audit_swap_events[(j-1) % audit_swap_events.size()];
            if (swap.start > e.time) continue;
            MIXER_TimingAuditLog("AV_AUDIT preceding_swap start_ms=%.3f end_ms=%.3f overlaps_callback=%u",
                    (swap.start-audit_started)/1e6, (swap.end-audit_started)/1e6,
                    (unsigned)(swap.end >= e.time));
            break;
        }
    }
    if (snapshot.underruns > details)
        MIXER_TimingAuditLog("AV_AUDIT omitted_underrun_details=%llu",
                (unsigned long long)(snapshot.underruns-details));
    MIXER_TimingAuditLog("AV_AUDIT scheduler updates=%llu cycle_changes=%llu cycles_now=%lld auto=%u state=%u native_result=%d native_flags=%u presentation_state=%u omitted_states=%u open_tick_gap_ms=%.3f open_delivered=%llu open_swaps=%llu",
            (unsigned long long)audit_tick_updates, (unsigned long long)audit_cycle_changes,
            (long long)CPU_CycleMax, (unsigned)CPU_CycleAutoAdjust, MOD_FramePacingAuditFlags(),
            audit_native_result, audit_native_flags, audit_last_view_state,
            audit_transitions > 8 ? audit_transitions-8 : 0,
            audit_interval.end ? (now-audit_interval.end)/1e6 : 0,
            (unsigned long long)audit_interval.delivered,
            (unsigned long long)audit_interval.swaps);
    for (const auto &e : audit_longest_ticks) {
        if (!e.end) continue;
        MIXER_TimingAuditLog("AV_AUDIT tick_interval end_ms=%.3f gap_ms=%.3f update_ms=%.3f previous_budget=%u delivered=%llu pending_before=%u debt_before=%u next_budget=%u discarded=%llu swaps=%llu swap_ms=%.3f presents=%llu present_ms=%.3f cycles_before=%lld cycles_after=%lld excluded_before=%llu excluded_after=%llu auto=%u state=%u",
                (e.end-audit_started)/1e6, e.gap/1e6, e.update/1e6,
                e.previous_budget, (unsigned long long)e.delivered, e.pending, e.debt,
                e.next_budget, (unsigned long long)e.discarded,
                (unsigned long long)e.swaps, e.swap_ns/1e6,
                (unsigned long long)e.presents, e.present_ns/1e6,
                (long long)e.cycles_before, (long long)e.cycles_after,
                (unsigned long long)e.excluded_before, (unsigned long long)e.excluded_after,
                (unsigned)e.automatic, e.flags);
    }
    audit_longest_ticks = {};
    audit_tick_updates = audit_cycle_changes = 0;
    audit_transitions = 0;
    if (audit_log) fflush(audit_log);
    audit_reported = now;
    audit_emu = emu;
    audit_swaps = audit_swap_total = audit_swap_max = audit_discarded = 0;
    audit_present_gap_max = 0;
}

uint32_t Mixer_MIXQ(void) {
	return  ((uint32_t)mixer.freq) |
		((uint32_t)2u/*channels*/ << (uint32_t)20u) |
		(mixer.swapstereo ?      ((uint32_t)1u << (uint32_t)29u) : 0u) |
		(mixer.mute       ?      ((uint32_t)1u << (uint32_t)30u) : 0u) |
		(mixer.nosound    ? 0u : ((uint32_t)1u << (uint32_t)31u));
}

uint8_t MixTemp[MIXER_BUFSIZE];

inline void MixerChannel::updateSlew(void) {
    /* "slew" affects the linear interpolation ramp.
     * but, our implementation can only shorten the linear interpolation
     * period, it cannot extend it beyond one sample period */
    freq_nslew = freq_nslew_want;
    if (freq_nslew < freq_n) freq_nslew = freq_n;

    if (freq_nslew_want > 0 && freq_nslew_want < freq_n)
        max_change = ((uint64_t)freq_nslew_want * (uint64_t)0x8000) / (uint64_t)freq_n;
    else
        max_change = 0x7FFFFFFFUL;
}

void MIXER_SetMaster(float vol0, float vol1) {
	mixer.mastervol[0] = vol0;
	mixer.mastervol[1] = vol1;
}

MixerChannel * MIXER_AddChannel(MIXER_Handler handler,Bitu freq,const char * name) {
    MixerChannel * chan=new MixerChannel();
    chan->freq_fslew = 0;
    chan->freq_nslew_want = 0;
    chan->freq_nslew = 0;
    chan->last_sample_write = 0;
    chan->current_loaded = false;
    chan->handler=handler;
    chan->name=name;
    chan->msbuffer_i = 0;
    chan->msbuffer_o = 0;
    chan->freq_n = chan->freq_d = 1;
    chan->lowpass_freq = 0;
    chan->lowpass_alpha = 0;

    for (unsigned int i=0;i < LOWPASS_ORDER;i++) {
        for (unsigned int j=0;j < 2;j++)
            chan->lowpass[i][j] = 0;
    }

    chan->lowpass_on_load = false;
    chan->lowpass_on_out = false;
    chan->freq_d_orig = 1;
    chan->freq_f = 0;
    chan->SetFreq(freq);
    chan->next=mixer.channels;
    chan->SetScale(1.0);
    chan->SetVolume(1,1);
    chan->enabled=false;
    chan->last[0] = chan->last[1] = 0;
    chan->delta[0] = chan->delta[1] = 0;
    chan->current[0] = chan->current[1] = 0;

    mixer.channels=chan;
    return chan;
}

MixerChannel * MIXER_FirstChannel(void) {
    return mixer.channels;
}

MixerChannel * MIXER_FindChannel(const char * name) {
    MixerChannel * chan=mixer.channels;
    while (chan) {
        if (!strcasecmp(chan->name,name)) break;
        chan=chan->next;
    }
    return chan;
}

void MIXER_DelChannel(MixerChannel* delchan) {
    MixerChannel * chan=mixer.channels;
    MixerChannel * * where=&mixer.channels;
    while (chan) {
        if (chan==delchan) {
            *where=chan->next;
            delete delchan;
            return;
        }
        where=&chan->next;
        chan=chan->next;
    }
}

void MixerChannel::UpdateVolume(void) {
    volmul[0]=(Bits)((1 << MIXER_VOLSHIFT)*scale[0]*volmain[0]);
    volmul[1]=(Bits)((1 << MIXER_VOLSHIFT)*scale[1]*volmain[1]);
}

void MixerChannel::SetVolume(float _left,float _right) {
    volmain[0]=_left;
    volmain[1]=_right;
    UpdateVolume();
}

void MixerChannel::SetScale( float f ) {
    SetScale(f, f);
}

void MixerChannel::SetScale(float _left, float _right) {
	// Constrain application-defined volume between 0% and 400%
	const float min_volume(0.0);
	const float max_volume(4.0);
	_left  = clamp(_left,  min_volume, max_volume);
	_right = clamp(_right, min_volume, max_volume);
	if (scale[0] != _left || scale[1] != _right) {
		scale[0] = _left;
		scale[1] = _right;
		UpdateVolume();
	}
}

static void MIXER_FillUp(void);

void MixerChannel::Enable(bool _yesno) {
    if (_yesno==enabled) return;
    enabled=_yesno;
    if (!enabled) freq_f=0;
}

void MixerChannel::lowpassUpdate() {
    if (lowpass_freq != 0) {
        double timeInterval;
        double tau,talpha;

        if (freq_n > freq_d) { // source -> dest mixer rate ratio is > 100%
            timeInterval = (double)freq_d_orig / freq_n; // will filter on sample load at source rate
            lowpass_on_load = true;
            lowpass_on_out = false;
        }
        else {
            timeInterval = (double)1.0 / mixer.freq; // will filter mixer output at mixer rate
            lowpass_on_load = false;
            lowpass_on_out = true;
        }

        tau = 1.0 / (lowpass_freq * 2 * M_PI);
        talpha = timeInterval / (tau + timeInterval);
        lowpass_alpha = (int32_t)(talpha * 0x10000); // double -> 16.16 fixed point

//      LOG_MSG("Lowpass freq_n=%u freq_d=%u timeInterval=%.12f tau=%.12f alpha=%.6f onload=%u onout=%u",
//          freq_n,freq_d_orig,timeInterval,tau,talpha,lowpass_on_load,lowpass_on_out);
    }
    else {
        lowpass_on_load = false;
        lowpass_on_out = false;
    }
}

inline int32_t MixerChannel::lowpassStep(int32_t in,const unsigned int iteration,const unsigned int channel) {
    const int64_t m1 = (int64_t)in * (int64_t)lowpass_alpha;
    const int64_t m2 = ((int64_t)lowpass[iteration][channel] << ((int64_t)16)) - ((int64_t)lowpass[iteration][channel] * (int64_t)lowpass_alpha);
    const int32_t ns = (int32_t)((m1 + m2) >> (int64_t)16);
    lowpass[iteration][channel] = ns;
    return ns;
}

inline void MixerChannel::lowpassProc(int32_t ch[2]) {
    for (unsigned int i=0;i < lowpass_order;i++) {
        for (unsigned int c=0;c < 2;c++)
            ch[c] = lowpassStep(ch[c],i,c);
    }
}

void MixerChannel::SetLowpassFreq(Bitu _freq,unsigned int order) {
    if (order > LOWPASS_ORDER) order = LOWPASS_ORDER;
    if (_freq == lowpass_freq && lowpass_order == order) return;
    lowpass_order = order;
    lowpass_freq = _freq;
    lowpassUpdate();
}

void MixerChannel::SetSlewFreq(Bitu _freq) {
    freq_nslew_want = _freq;
    updateSlew();
}

void MixerChannel::SetFreq(Bitu _freq,Bitu _den) {
    if (freq_n == _freq && freq_d == freq_d_orig)
        return;

    if (freq_d_orig != _den) {
        uint64_t tmp = (uint64_t)freq_f * (uint64_t)_den * (uint64_t)mixer.freq;
        freq_f = freq_fslew = (unsigned int)(tmp / (uint64_t)freq_d_orig);
    }

    freq_n = _freq;
    freq_d = _den * mixer.freq;
    freq_d_orig = _den;
    updateSlew();
    lowpassUpdate();
}

void CAPTURE_MultiTrackAddWave(uint32_t freq, uint32_t len, int16_t * data,const char *name);

void MixerChannel::EndFrame(Bitu samples) {
    if (CaptureState & CAPTURE_MULTITRACK_WAVE) {// TODO: should be a separate call!
        int16_t convert[1024][2];
        Bitu cnv = msbuffer_o;
        Bitu padding = 0;

        if (cnv > samples)
            cnv = samples;
        else
            padding = samples - cnv;

        if (cnv > 0) {
            int32_t volscale1 = (int32_t)(mixer.recordvol[0] * (1 << MIXER_VOLSHIFT));
            int32_t volscale2 = (int32_t)(mixer.recordvol[1] * (1 << MIXER_VOLSHIFT));

            if (cnv > 1024) cnv = 1024;
            for (Bitu i=0;i<cnv;i++) {
                convert[i][0]=MIXER_CLIP(((int64_t)msbuffer[i][0] * (int64_t)volscale1) >> (MIXER_VOLSHIFT + MIXER_VOLSHIFT));
                convert[i][1]=MIXER_CLIP(((int64_t)msbuffer[i][1] * (int64_t)volscale2) >> (MIXER_VOLSHIFT + MIXER_VOLSHIFT));
            }
            CAPTURE_MultiTrackAddWave(mixer.freq,cnv,(int16_t*)convert,name);
        }

        if (padding > 0) {
            if (padding > 1024) padding = 1024;
            memset(&convert[0][0],0,padding*sizeof(int16_t)*2);
            CAPTURE_MultiTrackAddWave(mixer.freq,padding,(int16_t*)convert,name);
        }
    }

    rend_n = rend_d = 0;
    if (msbuffer_o <= samples) {
        msbuffer_o = 0;
        msbuffer_i = 0;
    }
    else {
        msbuffer_o -= samples;
        if (msbuffer_i >= samples) msbuffer_i -= samples;
        else msbuffer_i = 0;
        memmove(&msbuffer[0][0],&msbuffer[samples][0],msbuffer_o*sizeof(int32_t)*2/*stereo*/);
    }

    last_sample_write -= (int)samples;
}

void MixerChannel::Mix(Bitu whole,Bitu frac) {
    unsigned int patience = 2;
    Bitu upto;

    if (whole <= rend_n) return;
    assert(whole <= mixer.samples_this_ms.w);
    assert(rend_n < mixer.samples_this_ms.w);
    int32_t *outptr = &mixer.work[mixer.work_in+rend_n][0];

    if (!enabled) {
        rend_n = whole;
        rend_d = frac;
        return;
    }

    // HACK: We iterate twice only because of the Sound Blaster emulation. No other emulation seems to need this.
    rendering_to_n = whole;
    rendering_to_d = frac;
    while (msbuffer_o < whole) {
        uint64_t todo = (uint64_t)(whole - msbuffer_o) * (uint64_t)freq_n;
        todo += (uint64_t)freq_f;
        todo += (uint64_t)freq_d - (uint64_t)1;
        todo /= (uint64_t)freq_d;
        if (!current_loaded) todo++;
        handler(todo);

        if (--patience == 0) break;
    }

    if (msbuffer_o < whole)
        padFillSampleInterpolation(whole);

    upto = whole;
    if (upto > msbuffer_o) upto = msbuffer_o;

    if (lowpass_on_out) { /* before rendering out to mixer, process samples with lowpass filter */
        Bitu t_rend_n = rend_n;
        Bitu t_msbuffer_i = msbuffer_i;

        while (t_rend_n < whole && t_msbuffer_i < upto) {
            lowpassProc(msbuffer[t_msbuffer_i]);
            t_msbuffer_i++;
            t_rend_n++;
        }
    }

    if (mixer.swapstereo) {
        while (rend_n < whole && msbuffer_i < upto) {
            *outptr++ += msbuffer[msbuffer_i][1];
            *outptr++ += msbuffer[msbuffer_i][0];
            msbuffer_i++;
            rend_n++;
        }
    }
    else {
        while (rend_n < whole && msbuffer_i < upto) {
            *outptr++ += msbuffer[msbuffer_i][0];
            *outptr++ += msbuffer[msbuffer_i][1];
            msbuffer_i++;
            rend_n++;
        }
    }

    rend_n = whole;
    rend_d = frac;
}

void MixerChannel::AddSilence(void) {
}

template<class Type,bool stereo,bool signeddata,bool nativeorder,bool T_lowpass>
inline void MixerChannel::loadCurrentSample(Bitu &len, const Type* &data) {
    last[0] = current[0];
    last[1] = current[1];

    if (sizeof(Type) == 1) {
        const uint8_t xr = signeddata ? 0x00 : 0x80;

        len--;
        current[0] = ((int8_t)((*data++) ^ xr)) << 8;
        if (stereo)
            current[1] = ((int8_t)((*data++) ^ xr)) << 8;
        else
            current[1] = current[0];
    }
    else if (sizeof(Type) == 2) {
        const uint16_t xr = signeddata ? 0x0000 : 0x8000;
        uint16_t d;

        len--;
        if (nativeorder) d = ((uint16_t)((*data++) ^ xr));
        else d = host_readw((HostPt)(data++)) ^ xr;
        current[0] = (int16_t)d;
        if (stereo) {
            if (nativeorder) d = ((uint16_t)((*data++) ^ xr));
            else d = host_readw((HostPt)(data++)) ^ xr;
            current[1] = (int16_t)d;
        }
        else {
            current[1] = current[0];
        }
    }
    else if (sizeof(Type) == 4) {
        const uint32_t xr = signeddata ? 0x00000000UL : 0x80000000UL;
        uint32_t d;

        len--;
        if (nativeorder) d = ((uint32_t)((*data++) ^ xr));
        else d = host_readd((HostPt)(data++)) ^ xr;
        current[0] = (int32_t)d;
        if (stereo) {
            if (nativeorder) d = ((uint32_t)((*data++) ^ xr));
            else d = host_readd((HostPt)(data++)) ^ xr;
            current[1] = (int32_t)d;
        }
        else {
            current[1] = current[0];
        }
    }
    else {
        current[0] = current[1] = 0;
        len = 0;
    }

    if (T_lowpass && lowpass_on_load)
        lowpassProc(current);

    if (stereo) {
        delta[0] = current[0] - last[0];
        delta[1] = current[1] - last[1];
    }
    else {
        delta[1] = delta[0] = current[0] - last[0];
    }

    if (freq_nslew_want != 0) {
        if (delta[0] < -max_change) delta[0] = -max_change;
        else if (delta[0] > max_change) delta[0] = max_change;

        if (stereo) {
            if (delta[1] < -max_change) delta[1] = -max_change;
            else if (delta[1] > max_change) delta[1] = max_change;
        }
        else {
            delta[1] = delta[0];
        }
    }

    current_loaded = true;
}

inline void MixerChannel::padFillSampleInterpolation(const Bitu upto) {
    finishSampleInterpolation(upto);
    if (msbuffer_o < upto) {
        if (freq_f > freq_d) freq_f = freq_d; // this is an abrupt stop, so interpolation must not carry over, to help avoid popping artifacts

        while (msbuffer_o < upto) {
            msbuffer[msbuffer_o][0] = current[0];
            msbuffer[msbuffer_o][1] = current[1];
            msbuffer_o++;
        }
    }
}

void MixerChannel::finishSampleInterpolation(const Bitu upto) {
    if (!current_loaded) return;
    runSampleInterpolation(upto);
}

double MixerChannel::timeSinceLastSample(void) {
    Bits delta = (Bits)mixer.samples_rendered_ms.w - (Bits)last_sample_write;
    return ((double)delta) / mixer.freq;
}

inline bool MixerChannel::runSampleInterpolation(const Bitu upto) {
    if (msbuffer_o >= upto)
        return false;

    while (freq_fslew < freq_d) {
        int sample = last[0] + (int)(((int64_t)delta[0] * (int64_t)freq_fslew) / (int64_t)freq_d);
        msbuffer[msbuffer_o][0] = sample * volmul[0];
        sample = last[1] + (int)(((int64_t)delta[1] * (int64_t)freq_fslew) / (int64_t)freq_d);
        msbuffer[msbuffer_o][1] = sample * volmul[1];

        freq_f += freq_n;
        freq_fslew += freq_nslew;
        if ((++msbuffer_o) >= upto)
            return false;
    }

    current[0] = last[0] + delta[0];
    current[1] = last[1] + delta[1];
    while (freq_f < freq_d) {
        msbuffer[msbuffer_o][0] = current[0] * volmul[0];
        msbuffer[msbuffer_o][1] = current[1] * volmul[1];

        freq_f += freq_n;
        if ((++msbuffer_o) >= upto)
            return false;
    }

    return true;
}

template<class Type,bool stereo,bool signeddata,bool nativeorder>
inline void MixerChannel::AddSamples(Bitu len, const Type* data) {
    last_sample_write = (Bits)mixer.samples_rendered_ms.w;

    if (msbuffer_o >= 2048) {
        fprintf(stderr,"WARNING: addSample overrun (immediate)\n");
        return;
    }

    if (!current_loaded) {
        if (len == 0) return;

        loadCurrentSample<Type,stereo,signeddata,nativeorder,false>(len,data);
        if (len == 0) {
            freq_f = freq_fslew = freq_d; /* encourage loading next round */
            return;
        }

        loadCurrentSample<Type,stereo,signeddata,nativeorder,false>(len,data);
        freq_f = freq_fslew = 0; /* interpolate now from what we just loaded */
    }

    if (lowpass_on_load) {
        for (;;) {
            if (freq_f >= freq_d) {
                if (len == 0) break;
                loadCurrentSample<Type,stereo,signeddata,nativeorder,true>(len,data);
                freq_f -= freq_d;
                freq_fslew = freq_f;
            }
            if (!runSampleInterpolation(2048))
                break;
        }
    }
    else {
        for (;;) {
            if (freq_f >= freq_d) {
                if (len == 0) break;
                loadCurrentSample<Type,stereo,signeddata,nativeorder,false>(len,data);
                freq_f -= freq_d;
                freq_fslew = freq_f;
            }
            if (!runSampleInterpolation(2048))
                break;
        }
    }
}

void MixerChannel::AddSamples_m8(Bitu len, const uint8_t * data) {
    AddSamples<uint8_t,false,false,true>(len,data);
}
void MixerChannel::AddSamples_s8(Bitu len,const uint8_t * data) {
    AddSamples<uint8_t,true,false,true>(len,data);
}
void MixerChannel::AddSamples_m8s(Bitu len,const int8_t * data) {
    AddSamples<int8_t,false,true,true>(len,data);
}
void MixerChannel::AddSamples_s8s(Bitu len,const int8_t * data) {
    AddSamples<int8_t,true,true,true>(len,data);
}
void MixerChannel::AddSamples_m16(Bitu len,const int16_t * data) {
    AddSamples<int16_t,false,true,true>(len,data);
}
void MixerChannel::AddSamples_s16(Bitu len,const int16_t * data) {
    AddSamples<int16_t,true,true,true>(len,data);
}
void MixerChannel::AddSamples_m16u(Bitu len,const uint16_t * data) {
    AddSamples<uint16_t,false,false,true>(len,data);
}
void MixerChannel::AddSamples_s16u(Bitu len,const uint16_t * data) {
    AddSamples<uint16_t,true,false,true>(len,data);
}
void MixerChannel::AddSamples_m32(Bitu len,const int32_t * data) {
    AddSamples<int32_t,false,true,true>(len,data);
}
void MixerChannel::AddSamples_s32(Bitu len,const int32_t * data) {
    AddSamples<int32_t,true,true,true>(len,data);
}
void MixerChannel::AddSamples_m16_nonnative(Bitu len,const int16_t * data) {
    AddSamples<int16_t,false,true,false>(len,data);
}
void MixerChannel::AddSamples_s16_nonnative(Bitu len,const int16_t * data) {
    AddSamples<int16_t,true,true,false>(len,data);
}
void MixerChannel::AddSamples_m16u_nonnative(Bitu len,const uint16_t * data) {
    AddSamples<uint16_t,false,false,false>(len,data);
}
void MixerChannel::AddSamples_s16u_nonnative(Bitu len,const uint16_t * data) {
    AddSamples<uint16_t,true,false,false>(len,data);
}
void MixerChannel::AddSamples_m32_nonnative(Bitu len,const int32_t * data) {
    AddSamples<int32_t,false,true,false>(len,data);
}
void MixerChannel::AddSamples_s32_nonnative(Bitu len,const int32_t * data) {
    AddSamples<int32_t,true,true,false>(len,data);
}

extern bool ticksLocked;

#if 0//unused
static inline bool Mixer_irq_important(void) {
    /* In some states correct timing of the irqs is more important then 
     * non stuttering audo */
    return (ticksLocked || (CaptureState & (CAPTURE_WAVE|CAPTURE_VIDEO|CAPTURE_MULTITRACK_WAVE)));
}
#endif

unsigned long long mixer_sample_counter = 0;
double mixer_start_pic_time = 0;

/* once a millisecond, render 1ms of audio, up to whole samples */
static void MIXER_MixData(Bitu fracs/*render up to*/) {
    unsigned int prev_rendered = mixer.samples_rendered_ms.w;
    MixerChannel *chan = mixer.channels;
    unsigned int whole,frac;
    bool endframe = false;

    if (fracs >= ((Bitu)mixer.samples_this_ms.w * mixer.samples_this_ms.fd)) {
        fracs = ((Bitu)mixer.samples_this_ms.w * mixer.samples_this_ms.fd);
        endframe = true;
    }

    whole = (unsigned int)(fracs / mixer.samples_this_ms.fd);
    frac = (unsigned int)(fracs % mixer.samples_this_ms.fd);
    if (whole <= mixer.samples_rendered_ms.w) return;

    while (chan) {
        chan->Mix(whole,fracs);
        if (endframe) chan->EndFrame(mixer.samples_this_ms.w);
        chan=chan->next;
    }

    if (CaptureState & (CAPTURE_WAVE|CAPTURE_VIDEO)) {
        int32_t volscale1 = (int32_t)(mixer.recordvol[0] * (1 << MIXER_VOLSHIFT));
        int32_t volscale2 = (int32_t)(mixer.recordvol[1] * (1 << MIXER_VOLSHIFT));
        int16_t convert[1024][2];
        Bitu added = whole - prev_rendered;
        if (added>1024) added=1024;
        Bitu readpos = mixer.work_in + prev_rendered;
        for (Bitu i=0;i<added;i++) {
            convert[i][0]=MIXER_CLIP(((int64_t)mixer.work[readpos][0] * (int64_t)volscale1) >> (MIXER_VOLSHIFT + MIXER_VOLSHIFT));
            convert[i][1]=MIXER_CLIP(((int64_t)mixer.work[readpos][1] * (int64_t)volscale2) >> (MIXER_VOLSHIFT + MIXER_VOLSHIFT));
            readpos++;
        }
        assert(readpos <= MIXER_BUFSIZE);
        CAPTURE_AddWave( mixer.freq, added, (int16_t*)convert );
    }

    mixer.samples_rendered_ms.w = whole;
    mixer.samples_rendered_ms.fd = frac;
    mixer_sample_counter += mixer.samples_rendered_ms.w - prev_rendered;
}

static void MIXER_FillUp(void) {
#ifdef C_SDL2
    SDL_LockAudioDevice(SDL2_AudioDevice);
#else
    SDL_LockAudio();
#endif
    float index = PIC_TickIndex();
    if (index < 0) index = 0;
    MIXER_MixData((Bitu)((double)index * ((Bitu)mixer.samples_this_ms.w * mixer.samples_this_ms.fd)));
#ifdef C_SDL2
    SDL_UnlockAudioDevice(SDL2_AudioDevice);
#else
    SDL_UnlockAudio();
#endif
}

void MixerChannel::FillUp(void) {
    MIXER_FillUp();
}

void MIXER_MixSingle(Bitu /*val*/) {
    MIXER_FillUp();
    PIC_AddEvent(MIXER_MixSingle,1000.0 / mixer.freq);
}

static void MIXER_Mix(void) {
    Bitu thr;

#ifdef C_SDL2
    SDL_LockAudioDevice(SDL2_AudioDevice);
#else
    SDL_LockAudio();
#endif

    if (MIXER_TimingAuditEnabled) {
        const uint64_t now = MIXER_TimingAuditNow();
        if (audit_last_production)
            audio_audit.max_gap = std::max(audio_audit.max_gap, now-audit_last_production);
        audit_last_production = now;
        ++audio_audit.productions;
    }
    /* render */
    assert((mixer.work_in+mixer.samples_per_ms.w) <= MIXER_BUFSIZE);
    MIXER_MixData((Bitu)mixer.samples_this_ms.w * (Bitu)mixer.samples_this_ms.fd);
    mixer.work_in += mixer.samples_this_ms.w;

    /* how many samples for the next ms? */
    mixer.samples_this_ms.w = mixer.samples_per_ms.w;
    mixer.samples_this_ms.fn += mixer.samples_per_ms.fn;
    if (mixer.samples_this_ms.fn >= mixer.samples_this_ms.fd) {
        mixer.samples_this_ms.fn -= mixer.samples_this_ms.fd;
        mixer.samples_this_ms.w++;
    }

    /* advance. we use in/out & wrap pointers to make sure the rendering code
     * doesn't have to worry about circular buffer wraparound. */
    thr = mixer.blocksize;
    if (thr < mixer.samples_this_ms.w) thr = mixer.samples_this_ms.w;
    if ((mixer.work_in+thr) > MIXER_BUFSIZE) {
        mixer.work_wrap = mixer.work_in;
        mixer.work_in = 0;
    }
    assert((mixer.work_in+thr) <= MIXER_BUFSIZE);
    assert((mixer.work_in+mixer.samples_this_ms.w) <= MIXER_BUFSIZE);
    memset(&mixer.work[mixer.work_in][0],0,sizeof(int32_t)*2*mixer.samples_this_ms.w);
    mixer.samples_rendered_ms.fn = 0;
    mixer.samples_rendered_ms.w = 0;
#ifdef C_SDL2
    SDL_UnlockAudioDevice(SDL2_AudioDevice);
#else
    SDL_UnlockAudio();
#endif
    MIXER_FillUp();
}

static void SDLCALL MIXER_CallBack(void * userdata, Uint8 *stream, int len) {
    (void)userdata;//UNUSED
    int32_t volscale1 = (int32_t)(mixer.mastervol[0] * (1 << MIXER_VOLSHIFT));
    int32_t volscale2 = (int32_t)(mixer.mastervol[1] * (1 << MIXER_VOLSHIFT));
    Bitu need = (Bitu)len/MIXER_SSIZE;
    int16_t *output = (int16_t*)stream;
    int remains;
    AudioAuditEvent *audit_event = nullptr;
    if (MIXER_TimingAuditEnabled) {
        audit_event = &audio_audit.events[audio_audit.callbacks++ % audio_audit.events.size()];
        *audit_event = {};
        audit_event->time = MIXER_TimingAuditNow();
        audit_event->production_gap = audit_last_production
                ? audit_event->time-audit_last_production : 0;
        int queued = (int)mixer.work_in - (int)mixer.work_out;
        if (queued < 0) queued += (int)mixer.work_wrap;
        audit_event->queued = (unsigned)std::max(queued, 0);
        audit_event->requested = (unsigned)need;
        audit_event->prebuffer = mixer.prebuffer_wait;
        audit_event->muted = mixer.mute;
        audio_audit.min_queued = std::min(audio_audit.min_queued, audit_event->queued);
    }

    if (mixer.mute) {
        if ((CaptureState & (CAPTURE_WAVE|CAPTURE_VIDEO|CAPTURE_MULTITRACK_WAVE)) != 0)
            mixer.work_out = mixer.work_in;
        else
            mixer.work_out = mixer.work_in = 0;
    }

    if (mixer.prebuffer_wait) {
        remains = (int)mixer.work_in - (int)mixer.work_out;
        if (remains < 0) remains += (int)mixer.work_wrap;
        if (remains < 0) remains = 0;

        if ((unsigned int)remains >= mixer.prebuffer_samples)
            mixer.prebuffer_wait = false;
    }

    if (!mixer.prebuffer_wait && !mixer.mute) {
        int32_t *in = &mixer.work[mixer.work_out][0];
        while (need > 0) {
            if (mixer.work_out == mixer.work_in) break;
            *output++ = MIXER_CLIP((((int64_t)(*in++)) * (int64_t)volscale1) >> (MIXER_VOLSHIFT + MIXER_VOLSHIFT));
            *output++ = MIXER_CLIP((((int64_t)(*in++)) * (int64_t)volscale2) >> (MIXER_VOLSHIFT + MIXER_VOLSHIFT));
            mixer.work_out++;
            if (mixer.work_out >= mixer.work_wrap) {
                mixer.work_out = 0;
                in = &mixer.work[mixer.work_out][0];
            }
            need--;
        }
    }

    if (audit_event && !mixer.mute) {
        audit_event->silence = (unsigned)need;
        audio_audit.silence += need;
        if (need && !mixer.prebuffer_wait) {
            audit_event->underrun = true;
            ++audio_audit.underruns;
        }
    }
    if (need > 0)
        mixer.prebuffer_wait = true;

    while (need > 0) {
        *output++ = 0;
        *output++ = 0;
        need--;
    }

    remains = (int)mixer.work_in - (int)mixer.work_out;
    if (remains < 0) remains += (int)mixer.work_wrap;

    if ((unsigned long)remains >= (mixer.blocksize*2UL)) {
        /* drop some samples to keep time */
        unsigned int drop;

        if ((unsigned long)remains >= (mixer.blocksize*3UL)) // hard drop
            drop = ((unsigned int)remains - (unsigned int)(mixer.blocksize));
        else // subtle drop
            drop = (((unsigned int)remains - (unsigned int)(mixer.blocksize*2)) / 50U) + 1;

        if (audit_event) {
            audit_event->dropped = drop;
            audio_audit.dropped += drop;
        }
        while (drop > 0) {
            mixer.work_out++;
            if (mixer.work_out >= mixer.work_wrap) mixer.work_out = 0;
            drop--;
        }
    }
}

std::string mixerinfo() {
    std::string info="Channel  Main    Main(dB)\n";
    char str[100];
    sprintf(str, "%-8s %3.0f:%-3.0f  %+3.2f:%-+3.2f\n","MASTER",
        (double)mixer.mastervol[0]*100,(double)mixer.mastervol[1]*100,
        20*log(mixer.mastervol[0])/log(10.0f),20*log(mixer.mastervol[1])/log(10.0f)
    );
    info+=std::string(str);
    sprintf(str, "%-8s %3.0f:%-3.0f  %+3.2f:%-+3.2f\n","RECORD",
        (double)mixer.recordvol[0]*100,(double)mixer.recordvol[1]*100,
        20*log(mixer.recordvol[0])/log(10.0f),20*log(mixer.recordvol[1])/log(10.0f)
    );
    info+=std::string(str);
    MixerChannel * chan=mixer.channels;
    for (chan=mixer.channels;chan;chan=chan->next) {
        sprintf(str, "%-8s %3.0f:%-3.0f  %+3.2f:%-+3.2f\n",chan->name,
            (double)chan->volmain[0]*100,(double)chan->volmain[1]*100,
            20*log(chan->volmain[0])/log(10.0f),20*log(chan->volmain[1])/log(10.0f)
        );
        info+=std::string(str);
    }
    return info;
}

static void MIXER_Stop(Section* sec) {
    MIXER_TimingAuditService(true);
    if (audit_log) { fclose(audit_log); audit_log = nullptr; }
    (void)sec;//UNUSED
}

class MIXER : public Program {
public:
    void MakeVolume(char * scan,float & vol0,float & vol1) {
        Bitu w=0;
        bool db=(toupper(*scan)=='D');
        if (db) scan++;
        while (*scan) {
            if (*scan==':') {
                ++scan;w=1;
            }
            char * before=scan;
            float val=(float)strtod(scan,&scan);
            if (before==scan) {
                ++scan;continue;
            }
            if (!db) val/=100;
            else val=powf(10.0f,(float)val/20.0f);
            if (val<0) val=1.0f;
            if (!w) {
                vol0=val;
            } else {
                vol1=val;
            }
        }
        if (!w) vol1=vol0;
    }

    void Run(void) override {
        if (cmd->FindExist("-?", false) || cmd->FindExist("/?", false)) {
			WriteOut("Displays or changes the current sound mixer volumes.\n\n"
                    "MIXER [/GUI|/NOSHOW] [/LISTMIDI [handler]] [channel volume]\n\n"
                    "  /GUI      Displays a dialog box showing the sound volumes.\n"
                    "  /NOSHOW   Does not show volumes when making changes to channel volumes.\n"
                    "  /LISTMIDI Lists and shows options for the current MIDI device handler.\n"
                    "            You can also add a handler name to show the specified handler.\n"
                    "  channel   A sound channel name (such as MASTER, RECORD, and SPKR).\n"
                    "  volume    An integer between 0 and 100 representing the sound volume.\n");
            return;
		}
        if(cmd->FindString("/LISTMIDI",temp_line,true)) {
            void MIDI_ListHandler(Program *caller, const char *name);
            MIDI_ListHandler(this, temp_line.c_str());
            return;
        }
        if(cmd->FindExist("/LISTMIDI")) {
            ListMidi();
            return;
        }
        if (cmd->FindString("MASTER",temp_line,false)) {
            MakeVolume((char *)temp_line.c_str(),mixer.mastervol[0],mixer.mastervol[1]);
        }
        if (cmd->FindString("RECORD",temp_line,false)) {
            MakeVolume((char *)temp_line.c_str(),mixer.recordvol[0],mixer.recordvol[1]);
        }
        MixerChannel * chan=mixer.channels;
        while (chan) {
            if (cmd->FindString(chan->name,temp_line,false)) {
                MakeVolume((char *)temp_line.c_str(),chan->volmain[0],chan->volmain[1]);
            }
            chan->UpdateVolume();
            chan=chan->next;
        }
        if (cmd->FindExist("/NOSHOW"))
            return;
        else if (cmd->FindExist("/GUI"))
            GUI_Shortcut(40);
        else
            WriteOut(mixerinfo().c_str());
    }
private:
    void ListMidi(){
        if(midi.handler) {
            WriteOut("MIDI handler: %s\n", midi.handler->GetName());
            midi.handler->ListAll(this);
        }
    };
};

void MIXER_ProgramStart(Program * * make) {
    *make=new MIXER;
}

MixerChannel* MixerObject::Install(MIXER_Handler handler,Bitu freq,const char * name){
    if(!installed) {
        if(strlen(name) > 31) E_Exit("Too long mixer channel name");
        safe_strncpy(m_name,name,32);
        installed = true;
        return MIXER_AddChannel(handler,freq,name);
    } else {
        E_Exit("already added mixer channel.");
        return nullptr; //Compiler happy
    }
}

MixerObject::~MixerObject(){
    if(!installed) return;
    MIXER_DelChannel(MIXER_FindChannel(m_name));
}

void MENU_mute(bool enabled) {
    mixer.mute=enabled;
    mainMenu.get_item("mixer_mute").check(mixer.mute).refresh_item(mainMenu);
}

bool MENU_get_mute(void) {
    return mixer.mute;
}

void MENU_swapstereo(bool enabled) {
    mixer.swapstereo=enabled;
    mainMenu.get_item("mixer_swapstereo").check(mixer.swapstereo).refresh_item(mainMenu);
}

bool MENU_get_swapstereo(void) {
    return mixer.swapstereo;
}

void MAPPER_VolumeUp(bool pressed) {
    if (!pressed) return;

    double newvol = (((double)mixer.mastervol[0] + mixer.mastervol[1]) / 0.7) * 0.5;

    if (newvol > 1) newvol = 1;

    mixer.mastervol[0] = mixer.mastervol[1] = newvol;

    LOG(LOG_MISC,LOG_NORMAL)("Master volume UP to %.3f%%",newvol * 100);
}

void MAPPER_VolumeDown(bool pressed) {
    if (!pressed) return;

    double newvol = ((double)mixer.mastervol[0] + mixer.mastervol[1]) * 0.7 * 0.5;

    if (fabs(newvol - 1.0) < 0.25)
        newvol = 1;

    mixer.mastervol[0] = mixer.mastervol[1] = newvol;

    LOG(LOG_MISC,LOG_NORMAL)("Master volume DOWN to %.3f%%",newvol * 100);
}

void MAPPER_RecVolumeUp(bool pressed) {
    if (!pressed) return;

    double newvol = (((double)mixer.recordvol[0] + mixer.recordvol[1]) / 0.7) * 0.5;

    if (newvol > 1) newvol = 1;

    mixer.recordvol[0] = mixer.recordvol[1] = newvol;

    LOG(LOG_MISC,LOG_NORMAL)("Recording volume UP to %.3f%%",newvol * 100);
}

void MAPPER_RecVolumeDown(bool pressed) {
    if (!pressed) return;

    double newvol = ((double)mixer.recordvol[0] + mixer.recordvol[1]) * 0.7 * 0.5;

    if (fabs(newvol - 1.0) < 0.25)
        newvol = 1;

    mixer.recordvol[0] = mixer.recordvol[1] = newvol;

    LOG(LOG_MISC,LOG_NORMAL)("Recording volume DOWN to %.3f%%",newvol * 100);
}

void MIXER_Controls_Init() {
    DOSBoxMenu::item *item;

    MAPPER_AddHandler(MAPPER_VolumeUp,MK_kpplus, MMODHOST,"volup","Increase volume",&item);
    item->set_text("Increase volume");
    
    MAPPER_AddHandler(MAPPER_VolumeDown,MK_kpminus,MMODHOST,"voldown","Decrease volume",&item);
    item->set_text("Decrease volume");

    MAPPER_AddHandler(MAPPER_RecVolumeUp,MK_nothing, 0,"recvolup","Increase recording volume",&item);
    item->set_text("Increase recording volume");

    MAPPER_AddHandler(MAPPER_RecVolumeDown,MK_nothing, 0,"recvoldown","Decrease recording volume",&item);
    item->set_text("Decrease recording volume");
}

void MIXER_DOS_Boot(Section *) {
}

void MIXER_Init() {
    const char *audit = std::getenv("MW2_AV_AUDIT");
    MIXER_TimingAuditEnabled = audit && !strcmp(audit, "1");
    if (MIXER_TimingAuditEnabled) {
        audit_started = audit_reported = MIXER_TimingAuditNow();
        audit_emu = PIC_FullIndex();
    }
    AddExitFunction(AddExitFunctionFuncPair(MIXER_Stop));

    LOG(LOG_MISC,LOG_DEBUG)("Initializing DOSBox audio mixer");

    Section_prop * section=static_cast<Section_prop *>(control->GetSection("mixer"));
    /* Read out config section */
    mixer.freq=(unsigned int)section->Get_int("rate");
    mixer.nosound=section->Get_bool("nosound");
    mixer.blocksize=(unsigned int)section->Get_int("blocksize");
    mixer.swapstereo=section->Get_bool("swapstereo");
    mixer.sampleaccurate=section->Get_bool("sample accurate");
    mixer.mute=false;
    if (control->opt_silent) mixer.nosound = true;

    /* Initialize the internal stuff */
    mixer.prebuffer_samples=0;
    mixer.prebuffer_wait=true;
    mixer.channels = nullptr;
    mixer.pos=0;
    mixer.done=0;
    memset(mixer.work,0,sizeof(mixer.work));
    mixer.mastervol[0]=1.0f;
    mixer.mastervol[1]=1.0f;
    mixer.recordvol[0]=1.0f;
    mixer.recordvol[1]=1.0f;

    /* Start the Mixer using SDL Sound at 22 khz */
    SDL_AudioSpec spec;
    SDL_AudioSpec obtained;

    spec.freq=(int)mixer.freq;
    spec.format=AUDIO_S16SYS;
    spec.channels=2;
    spec.callback=MIXER_CallBack;
    spec.userdata=NULL;
    spec.samples=(Uint16)mixer.blocksize;

#ifdef C_SDL2
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        LOG(LOG_MISC,LOG_DEBUG)("MIXER:Can't initialize SDL audio: %s , running in nosound mode.",SDL_GetError());
        mixer.nosound = true;
    }
#endif

    if (mixer.nosound) {
        LOG(LOG_MISC,LOG_DEBUG)("MIXER:No Sound Mode Selected.");
        TIMER_AddTickHandler(MIXER_Mix);
#ifdef C_SDL2
    } else if ((SDL2_AudioDevice=SDL_OpenAudioDevice(NULL, 0, &spec, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE)) == 0) {
#else
    } else if (SDL_OpenAudio(&spec, &obtained) <0 ) {
#endif
        mixer.nosound = true;
        LOG(LOG_MISC,LOG_DEBUG)("MIXER:Can't open audio: %s , running in nosound mode.",SDL_GetError());
        TIMER_AddTickHandler(MIXER_Mix);
    } else if (obtained.format != AUDIO_S16SYS) {
        mixer.nosound = true;
        LOG(LOG_MISC,LOG_DEBUG)("MIXER:Failed to get the sample format I wanted.");
        TIMER_AddTickHandler(MIXER_Mix);
#ifdef C_SDL2
        SDL_CloseAudioDevice(SDL2_AudioDevice);
        SDL2_AudioDevice = 0;
#else
        SDL_CloseAudio();
#endif
    } else {
        if(((Bitu)mixer.freq != (Bitu)obtained.freq) || ((Bitu)mixer.blocksize != (Bitu)obtained.samples))
            LOG(LOG_MISC,LOG_DEBUG)("MIXER:Got different values from SDL: freq %d, blocksize %d",(int)obtained.freq,(int)obtained.samples);

        mixer.freq=(unsigned int)obtained.freq;
        mixer.blocksize=obtained.samples;
        TIMER_AddTickHandler(MIXER_Mix);
        if (mixer.sampleaccurate) PIC_AddEvent(MIXER_MixSingle,1000.0 / mixer.freq);
#ifdef C_SDL2
        SDL_PauseAudioDevice(SDL2_AudioDevice, 0);
#else
        SDL_PauseAudio(0);
#endif
    }
    mixer_start_pic_time = PIC_FullIndex();
    mixer_sample_counter = 0;
    mixer.work_in = mixer.work_out = 0;
    mixer.work_wrap = MIXER_BUFSIZE;
    if (mixer.work_wrap <= mixer.blocksize) E_Exit("blocksize too large");

    {
        int ms = section->Get_int("prebuffer");

        if (ms < 0) ms = 20;

        mixer.prebuffer_samples = ((unsigned int)ms * (unsigned int)mixer.freq) / 1000u;
        if (mixer.prebuffer_samples > (mixer.work_wrap / 2))
            mixer.prebuffer_samples = (mixer.work_wrap / 2);
    }

    // how many samples per millisecond? compute as improper fraction (sample rate / 1000)
    mixer.samples_per_ms.w = mixer.freq / 1000U;
    mixer.samples_per_ms.fn = mixer.freq % 1000U;
    mixer.samples_per_ms.fd = 1000U;
    mixer.samples_this_ms.w = mixer.samples_per_ms.w;
    mixer.samples_this_ms.fn = 0;
    mixer.samples_this_ms.fd = mixer.samples_per_ms.fd;
    mixer.samples_rendered_ms.w = 0;
    mixer.samples_rendered_ms.fn = 0;
    mixer.samples_rendered_ms.fd = mixer.samples_per_ms.fd;

    LOG(LOG_MISC,LOG_DEBUG)("Mixer: sample_accurate=%u blocksize=%u sdl_rate=%uHz mixer_rate=%uHz channels=%u samples=%u min/max/need=%u/%u/%u per_ms=%u %u/%u samples prebuffer=%u",
        (unsigned int)mixer.sampleaccurate,
        (unsigned int)mixer.blocksize,
        (unsigned int)obtained.freq,
        (unsigned int)mixer.freq,
        (unsigned int)obtained.channels,
        (unsigned int)obtained.samples,
        (unsigned int)0,
        (unsigned int)0,
        (unsigned int)0,
        (unsigned int)mixer.samples_per_ms.w,
        (unsigned int)mixer.samples_per_ms.fn,
        (unsigned int)mixer.samples_per_ms.fd,
        (unsigned int)mixer.prebuffer_samples);

    AddVMEventFunction(VM_EVENT_DOS_INIT_KERNEL_READY,AddVMEventFunctionFuncPair(MIXER_DOS_Boot));

    if (MIXER_TimingAuditEnabled)
        MIXER_TimingAuditLog("AV_AUDIT schema=2 tick_gap=between_update_returns debt=host_ms_since_ticksLast budgets=guest_ms present_ms_includes_swap=1 state_bits=active:1,suspended:2,waiting:4,continuous:8,barrier:16,pending_safe:32,guest_call:64,view_eligible:128,pacer_running:256,safe_active:512 native_bits=available:1,mission:2,context:4,current_gl:8,presented:16,suspended:32,continuous:64 presentation_bits=view_low8,active:256,invoked:512");
    if (MIXER_TimingAuditEnabled)
        MIXER_TimingAuditLog("AV_AUDIT begin rate=%u blocksize=%u prebuffer_samples=%u sampleaccurate=%u nosound=%u",
                mixer.freq, mixer.blocksize, (unsigned)mixer.prebuffer_samples,
                (unsigned)mixer.sampleaccurate, (unsigned)mixer.nosound);
    MIXER_Controls_Init();
}

// save state support
//void *MIXER_Mix_NoSound_PIC_Timer = (void*)MIXER_Mix_NoSound;
void *MIXER_Mix_PIC_Timer = (void*)((uintptr_t)MIXER_Mix);


void MixerChannel::SaveState( std::ostream& stream )
{
	// - pure data
	WRITE_POD( &volmain, volmain );
	WRITE_POD( &scale, scale );
	WRITE_POD( &volmul, volmul );
	//WRITE_POD( &freq_add, freq_add );
	WRITE_POD( &enabled, enabled );
}


void MixerChannel::LoadState( std::istream& stream )
{
	// - pure data
	READ_POD( &volmain, volmain );
	READ_POD( &scale, scale );
	READ_POD( &volmul, volmul );
	//READ_POD( &freq_add, freq_add );
	READ_POD( &enabled, enabled );

	//********************************************
	//********************************************
	//********************************************

	// reset mixer channel (system data)
	mixer.pos = 0;
	mixer.done = 0;
}

extern void POD_Save_Adlib(std::ostream& stream);
extern void POD_Save_Disney(std::ostream& stream);
extern void POD_Save_Gameblaster(std::ostream& stream);
extern void POD_Save_GUS(std::ostream& stream);
extern void POD_Save_MPU401(std::ostream& stream);
extern void POD_Save_PCSpeaker(std::ostream& stream);
extern void POD_Save_Sblaster(std::ostream& stream);
extern void POD_Save_Tandy_Sound(std::ostream& stream);
extern void POD_Load_Adlib(std::istream& stream);
extern void POD_Load_Disney(std::istream& stream);
extern void POD_Load_Gameblaster(std::istream& stream);
extern void POD_Load_GUS(std::istream& stream);
extern void POD_Load_MPU401(std::istream& stream);
extern void POD_Load_PCSpeaker(std::istream& stream);
extern void POD_Load_Sblaster(std::istream& stream);
extern void POD_Load_Tandy_Sound(std::istream& stream);

namespace
{
class SerializeMixer : public SerializeGlobalPOD
{
public:
	SerializeMixer() : SerializeGlobalPOD("Mixer")
	{}

private:
	void getBytes(std::ostream& stream) override
	{

		//*************************************************
		//*************************************************

		SerializeGlobalPOD::getBytes(stream);


		POD_Save_Adlib(stream);
		POD_Save_Disney(stream);
		POD_Save_Gameblaster(stream);
		POD_Save_GUS(stream);
		POD_Save_MPU401(stream);
		POD_Save_PCSpeaker(stream);
		POD_Save_Sblaster(stream);
		POD_Save_Tandy_Sound(stream);
	}

	void setBytes(std::istream& stream) override
	{

		//*************************************************
		//*************************************************

		SerializeGlobalPOD::setBytes(stream);


		POD_Load_Adlib(stream);
		POD_Load_Disney(stream);
		POD_Load_Gameblaster(stream);
		POD_Load_GUS(stream);
		POD_Load_MPU401(stream);
		POD_Load_PCSpeaker(stream);
		POD_Load_Sblaster(stream);
		POD_Load_Tandy_Sound(stream);

		// reset mixer channel (system data)
		mixer.pos = 0;
		mixer.done = 0;
	}

} dummy;
}
