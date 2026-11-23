#include "kneil_ctrl_queue.h"

void kneil_ctrl_queue_reset(kneil_ctrl_queue_t *q)
{
    q->head  = 0;
    q->count = 0;
    q->busy  = false;
}

bool kneil_ctrl_queue_push(kneil_ctrl_queue_t *q, uint8_t ctrl)
{
    if(q->count >= KNEIL_CTRL_QUEUE_SIZE)
    {
        return false;
    }

    q->q[(q->head + q->count) % KNEIL_CTRL_QUEUE_SIZE] = ctrl;
    q->count++;
    return true;
}

bool kneil_ctrl_queue_take(kneil_ctrl_queue_t *q, uint8_t *ctrl)
{
    if(q->busy || q->count == 0)
    {
        return false;
    }

    *ctrl = q->q[q->head];
    q->head = (q->head + 1) % KNEIL_CTRL_QUEUE_SIZE;
    q->count--;
    q->busy = true;
    return true;
}

void kneil_ctrl_queue_done(kneil_ctrl_queue_t *q)
{
    q->busy = false;
}

void kneil_ctrl_queue_requeue(kneil_ctrl_queue_t *q, uint8_t ctrl)
{
    if(q->count >= KNEIL_CTRL_QUEUE_SIZE)
    {
        return;         /* cannot happen after a take(), but never overwrite a queued byte */
    }

    q->head = (q->head + KNEIL_CTRL_QUEUE_SIZE - 1) % KNEIL_CTRL_QUEUE_SIZE;
    q->q[q->head] = ctrl;
    q->count++;
}

int kneil_ctrl_queue_count(const kneil_ctrl_queue_t *q)
{
    return q->count;
}

bool kneil_ctrl_queue_busy(const kneil_ctrl_queue_t *q)
{
    return q->busy;
}
