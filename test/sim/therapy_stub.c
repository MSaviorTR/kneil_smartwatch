/* Simulator stand-in for the therapy glue: no BLE, audio or logging. */
#include <string.h>

#include "therapy.h"
#include "therapy_audio.h"

static therapy_snapshot_t fake;

void therapy_sim_set(const therapy_snapshot_t *s)
{
    fake = *s;
}

void therapy_sim_init(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.state = THERAPY_STATE_IDLE;
    fake.exercise = THERAPY_EX_EXTENSION;
    fake.target = THERAPY_EXT_TARGET_DEG;
    fake.set = 1;
    fake.rep = 1;
    fake.calibrated = true;
    fake.orientation_ok = true;
    fake.link_ok = true;
    fake.angle_valid = true;
    fake.audio_ready = true;
    fake.log_ready = true;
}

esp_err_t therapy_init(void) { therapy_sim_init(); return ESP_OK; }
bool therapy_start(therapy_exercise_t ex)
{
    fake.state = THERAPY_STATE_RUNNING;
    fake.phase = THERAPY_PHASE_REST;
    fake.exercise = ex;
    fake.target = (ex == THERAPY_EX_EXTENSION) ? THERAPY_EXT_TARGET_DEG : THERAPY_BEND_TARGET_DEG;
    return true;
}
void therapy_stop(void) { fake.state = THERAPY_STATE_IDLE; fake.phase = THERAPY_PHASE_IDLE; }
void therapy_pause(void) { fake.state = THERAPY_STATE_PAUSED; fake.phase = THERAPY_PHASE_PAUSED; }
bool therapy_resume(void) { fake.state = THERAPY_STATE_RUNNING; fake.phase = THERAPY_PHASE_REST; return true; }
bool therapy_calibrate_start(therapy_cal_mode_t mode)
{
    (void)mode;
    fake.state = THERAPY_STATE_CALIBRATING;
    fake.cal_progress_pct = 40;
    return true;
}
void therapy_tick(void) { }
void therapy_snapshot(therapy_snapshot_t *out) { *out = fake; }

/* Audio settings/playback: the simulator has no speaker. */
void therapy_audio_init(void) { }
bool therapy_audio_ready(void) { return false; }
int  therapy_audio_volume(void) { static int v = 70; return v; }
void therapy_audio_set_volume(int percent) { (void)percent; }
bool therapy_audio_muted(void) { return false; }
void therapy_audio_set_muted(bool mute) { (void)mute; }
void therapy_audio_save(void) { }
bool therapy_audio_play(therapy_clip_t clip) { (void)clip; return false; }
bool therapy_audio_play_rep(int rep) { (void)rep; return false; }
