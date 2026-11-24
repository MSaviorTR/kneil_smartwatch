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
                         float best_extension_deg, float best_flexion_deg, const char *reason);

#ifdef __cplusplus
}
#endif
