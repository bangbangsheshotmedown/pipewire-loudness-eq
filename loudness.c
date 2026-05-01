#include <stdio.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

// Long-term loudness target (~-12 dBFS)
#define DEFAULT_TARGET   0.25f

// Gate: below this RMS level, hold current gain instead of boosting further.
#define DEFAULT_GATE     0.001f   // ~-60 dBFS

// Max gain cap (+30 dB)
#define GAIN_MAX         32.0f

// Limiter ceiling
#define LIMITER_CEIL     0.95f

// RMS detector time constants.
#define RMS_ATTACK_MS       3.0f    // very fast: catches loud peaks immediately
#define RMS_RELEASE_MS   1500.0f    // faster recovery for quiet sounds

#define GAIN_DOWN_MS       10.0f    // very fast: pull gain down quickly on loud content
#define GAIN_UP_MS        400.0f    // faster: raise gain promptly on quiet content

// Gain smoothing to prevent step artifacts
#define GAIN_SMOOTH_MS   100.0f

// Look-ahead latency (ms)
#define LOOKAHEAD_MS     10.0f
#define DELAY_BUF_SIZE   16384  // ~340ms at 48kHz, plenty for look-ahead

// Limiter release time constant
#define LIMITER_RELEASE_MS 500.0f

// Fletcher-Munson shelf EQ settings (Subtler to prevent "heaviness")
#define FM_BASS_FREQ     80.0f    // Focus on sub-bass
#define FM_TREBLE_FREQ   7500.0f  // Focus on clarity
#define FM_BASS_MAX_DB   3.0f     // Reduced from 6dB to prevent boominess
#define FM_TREBLE_MAX_DB 5.0f
#define FM_SHELF_SLOPE   0.6f     // Gentler slope

// -----------------------------------------------------------------------------
// Biquad — Direct Form II Transposed, stereo state
// -----------------------------------------------------------------------------
struct biquad {
    float b0, b1, b2, a1, a2;
    float z1[2], z2[2]; // [0]=L [1]=R
};

static inline float biquad_tick(struct biquad *f, float x, int ch)
{
    float y    = f->b0 * x   + f->z1[ch];
    f->z1[ch]  = f->b1 * x   - f->a1 * y + f->z2[ch];
    f->z2[ch]  = f->b2 * x   - f->a2 * y;
    return y;
}

// K-Weighting filters: Stage 1 (Pre-filter / High-shelf) and Stage 2 (High-pass)
static void k_weight_stage1_set(struct biquad *f, float rate) {
    // Stage 1: +2dB High-shelf at 1.5kHz (Reduced from +4dB to avoid ignoring bass too much)
    float gain_db = 2.0f;
    float freq = 1500.0f;
    float Q = 0.707f;
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq / rate;
    float alpha = sinf(w0) / (2.0f * Q);
    float cw0 = cosf(w0);
    float sqA = 2.0f * sqrtf(A) * alpha;
    float a0 = (A + 1.0f) + (A - 1.0f) * cw0 + sqA;

    f->b0 = A * ((A + 1.0f) - (A - 1.0f) * cw0 + sqA) / a0;
    f->b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw0) / a0;
    f->b2 = A * ((A + 1.0f) - (A - 1.0f) * cw0 - sqA) / a0;
    f->a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw0) / a0;
    f->a2 = ((A + 1.0f) + (A - 1.0f) * cw0 - sqA) / a0;
}

static void k_weight_stage2_set(struct biquad *f, float rate) {
    // Stage 2: High-pass at 40Hz (Lowered from 50Hz to be more sensitive to sub-bass)
    float freq = 40.0f;
    float Q = 0.6f; // Slightly more damped
    float w0 = 2.0f * (float)M_PI * freq / rate;
    float alpha = sinf(w0) / (2.0f * Q);
    float cw0 = cosf(w0);
    float a0 = 1.0f + alpha;

    f->b0 = (1.0f + cw0) / 2.0f / a0;
    f->b1 = -(1.0f + cw0) / a0;
    f->b2 = (1.0f + cw0) / 2.0f / a0;
    f->a1 = -2.0f * cw0 / a0;
    f->a2 = (1.0f - alpha) / a0;
}

static void low_shelf_set(struct biquad *f, float rate, float freq, float gain_db)
{
    float A    = powf(10.0f, gain_db / 40.0f);
    float w0   = 2.0f * (float)M_PI * freq / rate;
    float sw0  = sinf(w0);
    float cw0  = cosf(w0);
    float alph = sw0 / 2.0f * sqrtf((A + 1.0f/A) * (1.0f/FM_SHELF_SLOPE - 1.0f) + 2.0f);
    float sqA2 = 2.0f * sqrtf(A) * alph;
    float a0   = (A+1) + (A-1)*cw0 + sqA2;

    f->b0 =  A * ((A+1) - (A-1)*cw0 + sqA2) / a0;
    f->b1 =  2.0f*A * ((A-1) - (A+1)*cw0)   / a0;
    f->b2 =  A * ((A+1) - (A-1)*cw0 - sqA2) / a0;
    f->a1 = -2.0f   * ((A-1) + (A+1)*cw0)   / a0;
    f->a2 =           ((A+1) + (A-1)*cw0 - sqA2) / a0;
}

static void high_shelf_set(struct biquad *f, float rate, float freq, float gain_db)
{
    float A    = powf(10.0f, gain_db / 40.0f);
    float w0   = 2.0f * (float)M_PI * freq / rate;
    float sw0  = sinf(w0);
    float cw0  = cosf(w0);
    float alph = sw0 / 2.0f * sqrtf((A + 1.0f/A) * (1.0f/FM_SHELF_SLOPE - 1.0f) + 2.0f);
    float sqA2 = 2.0f * sqrtf(A) * alph;
    float a0   = (A+1) - (A-1)*cw0 + sqA2;

    f->b0 =  A * ((A+1) + (A-1)*cw0 + sqA2) / a0;
    f->b1 = -2.0f*A * ((A-1) + (A+1)*cw0)   / a0;
    f->b2 =  A * ((A+1) + (A-1)*cw0 - sqA2) / a0;
    f->a1 =  2.0f   * ((A-1) - (A+1)*cw0)   / a0;
    f->a2 =           ((A+1) - (A-1)*cw0 - sqA2) / a0;
}

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
struct data {
    struct pw_main_loop *loop;
    struct pw_filter    *filter;

    void *in_l, *in_r, *out_l, *out_r;

    float target;
    float gate;

    // Long-term RMS detector (the actual loudness measurement)
    float rms_env;
    // Short-term peak detector for crest factor calculation
    float peak_env;

    // Smoothed applied gain (avoids abrupt jumps)
    float smooth_gain;

    // Limiter: instant attack, slow release
    float limiter_gain;

    // FM EQ
    struct biquad bass_shelf;
    struct biquad treble_shelf;

    // K-Weighting Detection Filters
    struct biquad k_stage1;
    struct biquad k_stage2;

    // Look-ahead delay buffer
    float *delay_l;
    float *delay_r;
    uint32_t delay_ptr;
    uint32_t delay_samples;

    bool latency_dirty;

    uint32_t sample_count;
};

// -----------------------------------------------------------------------------
// Latency Reporting
// -----------------------------------------------------------------------------
static void report_latency(struct data *d, float rate) {
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];

    struct spa_process_latency_info latency = SPA_PROCESS_LATENCY_INFO_INIT(
        .ns = (int64_t)(LOOKAHEAD_MS * 1000000.0f)
    );

    params[0] = spa_process_latency_build(&b, SPA_PARAM_Latency, &latency);

    pw_filter_update_params(d->filter, d->in_l, params, 1);
    pw_filter_update_params(d->filter, d->in_r, params, 1);

    params[0] = spa_process_latency_build(&b, SPA_PARAM_Latency, &latency);
    pw_filter_update_params(d->filter, d->out_l, params, 1);
    pw_filter_update_params(d->filter, d->out_r, params, 1);

    printf("\n[Main Thread] Latency reported: %.1fms\n", LOOKAHEAD_MS);
}

// -----------------------------------------------------------------------------
// Process
// Signal chain: RMS detect (long-term) → compute gain → smooth gain →
//               apply gain → FM EQ (proportional to gain) → limiter
// -----------------------------------------------------------------------------
static void on_process(void *userdata, struct spa_io_position *position)
{
    struct data *d = userdata;
    uint32_t     n = position->clock.duration;

    float *il  = pw_filter_get_dsp_buffer(d->in_l,  n);
    float *ir  = pw_filter_get_dsp_buffer(d->in_r,  n);
    float *ol  = pw_filter_get_dsp_buffer(d->out_l, n);
    float *or_ = pw_filter_get_dsp_buffer(d->out_r, n);

    if (!il || !ir || !ol || !or_)
        return;

    // rate.num == 1 for standard rates; denom is the sample rate (e.g. 48000)
    float rate = (float)position->clock.rate.denom;

    float atk_c       = 1.0f - expf(-1.0f / (rate * RMS_ATTACK_MS  / 1000.0f));
    float rel_c       = 1.0f - expf(-1.0f / (rate * RMS_RELEASE_MS / 1000.0f));
    float gain_down_c = 1.0f - expf(-1.0f / (rate * GAIN_DOWN_MS   / 1000.0f));
    float gain_up_c   = 1.0f - expf(-1.0f / (rate * GAIN_UP_MS     / 1000.0f));
    float lim_rel_c   = 1.0f - expf(-1.0f / (rate * LIMITER_RELEASE_MS / 1000.0f));

    // Initialize detection filters if needed (once per rate change)
    static float last_rate = 0;
    if (rate != last_rate) {
        k_weight_stage1_set(&d->k_stage1, rate);
        k_weight_stage2_set(&d->k_stage2, rate);
        d->delay_samples = (uint32_t)(rate * LOOKAHEAD_MS / 1000.0f);
        if (d->delay_samples >= DELAY_BUF_SIZE) d->delay_samples = DELAY_BUF_SIZE - 1;

        if (!d->delay_l) d->delay_l = calloc(DELAY_BUF_SIZE, sizeof(float));
        if (!d->delay_r) d->delay_r = calloc(DELAY_BUF_SIZE, sizeof(float));

        d->latency_dirty = true;
        last_rate = rate;
    }

    // Update FM EQ coefficients once per block.
    // Amount is proportional to how much gain we're currently applying —
    // more gain means quieter content, which needs more bass+treble.
    {
        float gain_db   = 20.0f * log10f(d->smooth_gain + 1e-6f);
        float gain_norm = fmaxf(0.0f, fminf(1.0f,
                            gain_db / (20.0f * log10f(GAIN_MAX))));

        low_shelf_set (&d->bass_shelf,   rate, FM_BASS_FREQ,
                       FM_BASS_MAX_DB   * gain_norm);
        high_shelf_set(&d->treble_shelf, rate, FM_TREBLE_FREQ,
                       FM_TREBLE_MAX_DB * gain_norm);
    }

    float last_rms  = d->rms_env;
    float last_gain = d->smooth_gain;

    for (uint32_t i = 0; i < n; i++) {
        float in_l = il[i];
        float in_r = ir[i];

        // 1. K-Weighted detection on FUTURE signal
        float wl = biquad_tick(&d->k_stage1, in_l, 0);
        wl = biquad_tick(&d->k_stage2, wl, 0);
        float wr = biquad_tick(&d->k_stage1, in_r, 1);
        wr = biquad_tick(&d->k_stage2, wr, 1);

        // Amplitude for RMS and Peak
        float abs_samp = fmaxf(fabsf(wl), fabsf(wr));

        // Long-term RMS
        float rms_coeff = (abs_samp > d->rms_env) ? atk_c : rel_c;
        d->rms_env += rms_coeff * (abs_samp - d->rms_env);

        // Short-term Peak (fast attack, medium release ~50ms)
        float peak_atk = 1.0f - expf(-1.0f / (rate * 0.001f)); // 1ms
        float peak_rel = 1.0f - expf(-1.0f / (rate * 0.050f)); // 50ms
        float p_coeff = (abs_samp > d->peak_env) ? peak_atk : peak_rel;
        d->peak_env += p_coeff * (abs_samp - d->peak_env);

        // Crest Factor: ratio of peak to RMS (approximate density)
        // 1.0 = pure square wave (heavily compressed), >3.0 = dynamic
        float crest = (d->rms_env > 1e-6f) ? (d->peak_env / d->rms_env) : 1.0f;

        // Adaptation: if crest is low (compressed), slow down the gain changes
        // Scale factor: 1.0 at crest=3.0, down to 0.1 at crest=1.2
        float adapt = fmaxf(0.1f, fminf(1.0f, (crest - 1.2f) / 1.8f));

        // Target gain based on weighted mids
        float target_gain;
        if (d->rms_env > d->gate)
            target_gain = fminf(d->target / d->rms_env, GAIN_MAX);
        else
            target_gain = d->smooth_gain; // hold

        // Smooth the gain with adaptation
        float gain_c = (target_gain < d->smooth_gain) ? gain_down_c : gain_up_c;
        d->smooth_gain += (gain_c * adapt) * (target_gain - d->smooth_gain);

        // 2. Delay Buffer (Store future, retrieve present)
        d->delay_l[d->delay_ptr] = in_l;
        d->delay_r[d->delay_ptr] = in_r;

        uint32_t read_idx = (d->delay_ptr + DELAY_BUF_SIZE - d->delay_samples) % DELAY_BUF_SIZE;
        float sl = d->delay_l[read_idx];
        float sr = d->delay_r[read_idx];

        d->delay_ptr = (d->delay_ptr + 1) % DELAY_BUF_SIZE;

        // 3. Apply leveling gain to PRESENT signal
        float el = sl * d->smooth_gain;
        float er = sr * d->smooth_gain;

        // Fletcher-Munson psychoacoustic shelf EQ
        el = biquad_tick(&d->bass_shelf,   el, 0);
        er = biquad_tick(&d->bass_shelf,   er, 1);
        el = biquad_tick(&d->treble_shelf, el, 0);
        er = biquad_tick(&d->treble_shelf, er, 1);

        // Smoothed peak limiter: instant attack, ~500ms release
        float peak      = fmaxf(fabsf(el), fabsf(er));
        float target_lg = (peak > LIMITER_CEIL) ? (LIMITER_CEIL / peak) : 1.0f;
        if (target_lg < d->limiter_gain)
            d->limiter_gain = target_lg;
        else
            d->limiter_gain += lim_rel_c * (1.0f - d->limiter_gain);

        ol[i]  = el * d->limiter_gain;
        or_[i] = er * d->limiter_gain;

        last_rms  = d->rms_env;
        last_gain = d->smooth_gain;
    }

    d->sample_count += n;
    if (d->sample_count >= (uint32_t)rate) {
        float gain_db = 20.0f * log10f(last_gain + 1e-6f);
        float crest = (d->rms_env > 1e-6f) ? (d->peak_env / d->rms_env) : 1.0f;
        float adapt = fmaxf(0.1f, fminf(1.0f, (crest - 1.2f) / 1.8f));

        printf("RMS: %.4f | Gain: %+.1f dB | Crest: %.1f | Adapt: %d%%   \r",
               last_rms,
               gain_db,
               crest,
               (int)(adapt * 100));
        fflush(stdout);
        d->sample_count = 0;
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
    (void)signal_number;
    struct data *d = userdata;
    pw_main_loop_quit(d->loop);
}

static void on_timeout(void *userdata, uint64_t expirations) {
    struct data *d = userdata;
    if (d->latency_dirty) {
        d->latency_dirty = false;
        // rate isn't easily available here without more state,
        // but report_latency only needs it for the printf and info struct.
        // We'll pass 0 and update report_latency to be more robust.
        report_latency(d, 0);
    }
}

int main(int argc, char *argv[]) {

    struct data data = {0};

    pw_init(&argc, &argv);

    data.loop = pw_main_loop_new(NULL);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT,  do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

    // Add a timer to check for latency updates (100ms interval)
    struct spa_source *timer;
    timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop), on_timeout, &data);
    struct timespec value, interval;
    value.tv_sec = 0; value.tv_nsec = 100000000;
    interval.tv_sec = 0; interval.tv_nsec = 100000000;
    pw_loop_update_timer(pw_main_loop_get_loop(data.loop), timer, &value, &interval, false);

    data.target       = getenv("TARGET") ? atof(getenv("TARGET")) : DEFAULT_TARGET;
    data.gate         = getenv("GATE")   ? atof(getenv("GATE"))   : DEFAULT_GATE;
    data.smooth_gain  = 1.0f;
    data.limiter_gain = 1.0f;

    data.filter = pw_filter_new_simple(
        pw_main_loop_get_loop(data.loop),
        "Loudness Equalizer",
        pw_properties_new(
            PW_KEY_NODE_NAME,        "loudness-eq",
            PW_KEY_NODE_DESCRIPTION, "Windows-like Loudness Equalization",
            PW_KEY_MEDIA_TYPE,       "Audio",
            PW_KEY_MEDIA_CATEGORY,   "Filter",
            PW_KEY_MEDIA_ROLE,       "DSP",
            NULL),
        &filter_events,
        &data);

    data.in_l = pw_filter_add_port(data.filter,
        PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono",
                          PW_KEY_PORT_NAME, "input_FL",
                          PW_KEY_AUDIO_CHANNEL, "FL", NULL), NULL, 0);

    data.in_r = pw_filter_add_port(data.filter,
        PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono",
                          PW_KEY_PORT_NAME, "input_FR",
                          PW_KEY_AUDIO_CHANNEL, "FR", NULL), NULL, 0);

    data.out_l = pw_filter_add_port(data.filter,
        PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono",
                          PW_KEY_PORT_NAME, "output_FL",
                          PW_KEY_AUDIO_CHANNEL, "FL", NULL), NULL, 0);

    data.out_r = pw_filter_add_port(data.filter,
        PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono",
                          PW_KEY_PORT_NAME, "output_FR",
                          PW_KEY_AUDIO_CHANNEL, "FR", NULL), NULL, 0);

    if (pw_filter_connect(data.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
        fprintf(stderr, "can't connect filter\n");
        return -1;
    }

    printf("Loudness Equalizer running\n");
    printf("Target: %.2f (%.1f dBFS) | Gate: %.4f | RMS window: %.0fms/%.0fms | Gain smooth: %.0fms\n",
           data.target,
           20.0f * log10f(data.target),
           data.gate,
           (double)RMS_ATTACK_MS, (double)RMS_RELEASE_MS,
           (double)GAIN_SMOOTH_MS);

    pw_main_loop_run(data.loop);

    pw_filter_destroy(data.filter);
    pw_main_loop_destroy(data.loop);
    pw_deinit();

    return 0;
}
