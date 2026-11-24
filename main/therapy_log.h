#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Therapy log on the watch storage (the same root as the data logger: SD, else internal flash).
   One line per counted repetition and one summary line per session, in gw_therapy.csv. */

void therapy_log_init(void);

void therapy_log_rep(const char *exercise, int set, int rep, float peak_deg, float hold_s, bool full);
void therapy_log_session(const char *exercise, int sets_done, int full, int partial,
                         float best_extension_deg, float best_flexion_deg, bool standing, const char *reason);
void therapy_log_straight(float extension_deg, bool orientation_ok);

/* True while the data logger's root (SD or internal flash) is mounted. */
bool therapy_log_available(void);

#ifdef __cplusplus
}
#endif
