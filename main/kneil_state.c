#include <string.h>

#include "kneil_state.h"

/* Class names and colours as in the KNEIL PC app (pc_app/kneil/protocol.py):
   v1 = 9 classes, v2 = 8 classes with sitting + standing as one 'still'. */
static const char *const CLASS_V1[] = {
    "walking", "stairs up", "stairs down", "sitting", "standing",
    "lying", "sit-to-stand", "stand-to-sit", "bed transfer",
};

static const uint32_t COLOUR_V1[] = {
    0x2e9e44, 0xff9a1f, 0xc65102, 0x2166d6, 0x7fc4ef,
    0x8e44ad, 0xf4d03f, 0xc9a227, 0x8b5a2b,
};

static const char *const CLASS_V2[] = {
    "walking", "stairs up", "stairs down", "still", "lying",
    "sit-to-stand", "stand-to-sit", "bed transfer",
};

static const uint32_t COLOUR_V2[] = {
    0x2e9e44, 0xff9a1f, 0xc65102, 0x2166d6, 0x8e44ad,
    0xf4d03f, 0xc9a227, 0x8b5a2b,
};

static const char *const LINK_TEXT[] = {
    [KNEIL_LINK_OFF]        = "off",
    [KNEIL_LINK_SCANNING]   = "scanning",
    [KNEIL_LINK_CONNECTING] = "connecting",
    [KNEIL_LINK_CONNECTED]  = "connected",
    [KNEIL_LINK_LOST]       = "lost",
};

static const char *const POSTURE_TEXT[] = { "unknown", "sitting", "standing" };

int kneil_version_classes(uint8_t version)
{
    return version >= 2 ? 8 : 9;
}

const char *kneil_link_text(kneil_link_t link)
{
    return (link <= KNEIL_LINK_LOST) ? LINK_TEXT[link] : "?";
}

const char *kneil_class_text(int cls, uint8_t version)
{
    const char *const *names = version >= 2 ? CLASS_V2 : CLASS_V1;
    const int n = kneil_version_classes(version);

    return (cls >= 0 && cls < n) ? names[cls] : "-";
}

uint32_t kneil_class_colour(int cls, uint8_t version)
{
    const uint32_t *cols = version >= 2 ? COLOUR_V2 : COLOUR_V1;
    const int n = kneil_version_classes(version);

    return (cls >= 0 && cls < n) ? cols[cls] : 0x3a3d44;   /* UI_COL_FAINT */
}

const char *kneil_posture_text(int posture)
{
    return (posture >= 0 && posture <= 2) ? POSTURE_TEXT[posture] : "unknown";
}

void kneil_model_reset(kneil_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->link = KNEIL_LINK_OFF;
    m->cls = KNEIL_CLASS_NONE;
    m->cls_raw = KNEIL_CLASS_NONE;
    m->cls_smooth = KNEIL_CLASS_NONE;
    m->vote_pct = KNEIL_VOTE_NONE;
    m->posture = KNEIL_POSTURE_UNKNOWN;
}

void kneil_model_set_active(kneil_model_t *m, bool active)
{
    m->active = active;

    if(!active)
    {
        m->link = KNEIL_LINK_OFF;
    }
}

void kneil_model_set_link(kneil_model_t *m, kneil_link_t link, const char *name, int rssi, uint16_t mtu)
{
    m->link = link;
    m->rssi = rssi;
    m->mtu  = mtu;

    if(name != NULL)
    {
        size_t n = strlen(name);

        if(n >= sizeof(m->name))
        {
            n = sizeof(m->name) - 1;
        }

        memcpy(m->name, name, n);
        m->name[n] = '\0';
    }
}

void kneil_model_on_live(kneil_model_t *m, const kneil_live_t *p, int64_t now_us)
{
    /* Sequence gaps: a repeated packet, a restart or a jump > 1000 is not counted as loss. */
    if(m->have_seq)
    {
        const uint16_t gap = (uint16_t)(p->seq - m->last_seq);

        if(gap > 0 && gap <= 1000)
        {
            m->gaps += (uint32_t)(gap - 1);
        }
    }

    m->last_seq  = p->seq;
    m->have_seq  = true;
    m->packets++;
    m->version   = p->version;
    m->cls       = p->cls;
    m->cls_raw   = (p->version >= 2) ? p->raw_cls : p->cls;
    m->cls_smooth = p->cls_smooth;
    m->vote_pct  = p->vote_pct;
    m->posture   = (p->version >= 2) ? ((p->events & KNEIL_EV_POSTURE_MASK) >> KNEIL_EV_POSTURE_SHIFT)
                                     : KNEIL_POSTURE_UNKNOWN;
    m->last_live_us = now_us;

    if(p->version >= 2)
    {
        if(p->events & KNEIL_EV_SIT_TO_STAND)  m->n_sit_to_stand++;
        if(p->events & KNEIL_EV_STAND_TO_SIT)  m->n_stand_to_sit++;
        if(p->events & KNEIL_EV_BED_TRANSFER)  m->n_bed_transfer++;
    }

    m->history[m->hist_pos] = (uint8_t)p->cls;   /* KNEIL_CLASS_NONE when there is no result */
    m->hist_pos = (m->hist_pos + 1) % KNEIL_HISTORY;

    if(m->hist_count < KNEIL_HISTORY)
    {
        m->hist_count++;
    }
}

void kneil_model_on_summary(kneil_model_t *m, const kneil_summary_t *s)
{
    m->summary = *s;
    m->have_summary = true;
}

void kneil_model_on_info(kneil_model_t *m, const char *text)
{
    size_t n = strlen(text);

    if(n >= sizeof(m->info))
    {
        n = sizeof(m->info) - 1;
    }

    memcpy(m->info, text, n);
    m->info[n] = '\0';
    m->have_info = true;
}

bool kneil_page_needs_rebuild(bool built_kneil, int ble_mode, int kneil_mode)
{
    return built_kneil != (ble_mode == kneil_mode);
}

/*
 * UUID a8f5000X-3e2c-4c6e-9b1a-6b4e45494c00, little-endian (NimBLE order):
 * the text ends ...6b4e45494c00, so the bytes start 00 4c 49 45 4e 6b.
 */
const uint8_t KNEIL_UUID_BASE_LE[16] = {
    0x00, 0x4c, 0x49, 0x45, 0x4e, 0x6b, 0x1a, 0x9b,
    0x6e, 0x4c, 0x2c, 0x3e, 0x00, 0x00, 0xf5, 0xa8,
};

void kneil_uuid_le(uint8_t out[16], uint16_t id)
{
    memcpy(out, KNEIL_UUID_BASE_LE, 16);
    out[12] = (uint8_t)(id & 0xff);
    out[13] = (uint8_t)((id >> 8) & 0xff);
}

void kneil_uuid_text(char out[37], uint16_t id)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t b[16];
    char *p = out;

    kneil_uuid_le(b, id);

    /* the text is the byte array reversed, 8-4-4-4-12 hex digits */
    for(int i = 15; i >= 0; i--)
    {
        *p++ = hex[(b[i] >> 4) & 0xf];
        *p++ = hex[b[i] & 0xf];

        if(i == 12 || i == 10 || i == 8 || i == 6)
        {
            *p++ = '-';
        }
    }

    *p = '\0';
}

void kneil_chr_dsc_range(uint16_t value, uint16_t next_def, uint16_t svc_end,
                         uint16_t *first, uint16_t *last)
{
    uint16_t start_arg;

    kneil_dsc_disc_args(value, next_def, svc_end, &start_arg, last);

    /* the descriptors themselves start after the value handle; last < first means there are none */
    *first = (uint16_t)(value + 1);
}

void kneil_dsc_disc_args(uint16_t value, uint16_t next_def, uint16_t svc_end,
                         uint16_t *start_arg, uint16_t *end_arg)
{
    /* NimBLE's ble_gattc_disc_all_dscs() takes the characteristic value handle as its start handle and
       searches value + 1 .. end itself (ble_gattc.c: find_info(prev_handle + 1, end_handle)). */
    *start_arg = value;

    /* next_def == 0: last characteristic of the service. */
    *end_arg = (next_def != 0) ? (uint16_t)(next_def - 1) : svc_end;
}

uint16_t kneil_first_cccd(const uint16_t *handles, const uint8_t *is_cccd, int n,
                          uint16_t first, uint16_t last)
{
    uint16_t best = 0;

    for(int i = 0; i < n; i++)
    {
        if(!is_cccd[i] || handles[i] < first || handles[i] > last)
        {
            continue;
        }

        if(best == 0 || handles[i] < best)
        {
            best = handles[i];      /* the first CCCD of this characteristic, whatever order it arrives in */
        }
    }

    return best;
}

uint16_t kneil_cccd_for(uint16_t value, uint16_t next_def, uint16_t svc_end,
                        const uint16_t *handles, const uint8_t *is_cccd, int n)
{
    uint16_t first, last;

    kneil_chr_dsc_range(value, next_def, svc_end, &first, &last);

    if(last < first)
    {
        return 0;
    }

    return kneil_first_cccd(handles, is_cccd, n, first, last);
}

int kneil_model_history_at(const kneil_model_t *m, int i)
{
    if(i < 0 || i >= m->hist_count)
    {
        return KNEIL_CLASS_NONE;
    }

    return m->history[(m->hist_pos - 1 - i + KNEIL_HISTORY) % KNEIL_HISTORY];
}

int kneil_model_live_age_s(const kneil_model_t *m, int64_t now_us)
{
    if(m->last_live_us == 0 || now_us < m->last_live_us)
    {
        return -1;
    }

    return (int)((now_us - m->last_live_us) / 1000000);
}
