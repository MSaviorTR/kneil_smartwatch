#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * CONTROL write queue of the KNEIL central, as pure C so the host tests can cover it.
 *
 * One byte is in flight at a time: `busy` is set when a byte leaves the queue, before the write is
 * started, and cleared by the write's completion callback. A byte whose write could not be started goes
 * back to the front with kneil_ctrl_queue_requeue(), so it is not lost.
 *
 * Not thread-safe on its own: kneil_central.c calls these inside its lock.
 */

#define KNEIL_CTRL_QUEUE_SIZE 4

typedef struct
{
    uint8_t q[KNEIL_CTRL_QUEUE_SIZE];
    int     head;     /* index of the oldest byte */
    int     count;    /* bytes waiting, not counting the one in flight */
    bool    busy;     /* a write for the last taken byte is in flight */
} kneil_ctrl_queue_t;

/* Empty and idle. The bytes themselves need no clearing. */
void kneil_ctrl_queue_reset(kneil_ctrl_queue_t *q);

/* Appends one byte; false when the queue is full (the byte is not queued). */
bool kneil_ctrl_queue_push(kneil_ctrl_queue_t *q, uint8_t ctrl);

/* Takes the oldest byte and marks the queue busy; false when empty or a write is already in flight. */
bool kneil_ctrl_queue_take(kneil_ctrl_queue_t *q, uint8_t *ctrl);

/* The write in flight completed (successfully or not): the next byte may be taken. */
void kneil_ctrl_queue_done(kneil_ctrl_queue_t *q);

/* Puts `ctrl` back at the front, after a write could not be started. */
void kneil_ctrl_queue_requeue(kneil_ctrl_queue_t *q, uint8_t ctrl);

int  kneil_ctrl_queue_count(const kneil_ctrl_queue_t *q);
bool kneil_ctrl_queue_busy(const kneil_ctrl_queue_t *q);
