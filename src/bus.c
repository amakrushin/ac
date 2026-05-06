#include "bus.h"

#include "ring.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct EventBus {
    Ring           *ring;
    pthread_mutex_t mu;
};

EventBus *bus_create(size_t capacity) {
    EventBus *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->ring = ring_create(capacity, sizeof(LogEntry));
    if (!b->ring) { free(b); return NULL; }
    if (pthread_mutex_init(&b->mu, NULL) != 0) {
        ring_destroy(b->ring);
        free(b);
        return NULL;
    }
    return b;
}

void bus_free(EventBus *b) {
    if (!b) return;
    pthread_mutex_destroy(&b->mu);
    ring_destroy(b->ring);
    free(b);
}

uint64_t bus_push(EventBus *b, const LogEntry *e) {
    if (!b || !e) return 0;
    /* The serial field on the stored copy is left as the caller passed it
     * (typically 0). bus_get / bus_get_by_serial recompute and patch the
     * correct serial into the returned LogEntry on read. */
    LogEntry copy = *e;
    pthread_mutex_lock(&b->mu);
    uint64_t serial = ring_push(b->ring, &copy);
    pthread_mutex_unlock(&b->mu);
    return serial;
}

size_t bus_size(const EventBus *b) {
    if (!b) return 0;
    pthread_mutex_t *mu = (pthread_mutex_t *)&b->mu;
    pthread_mutex_lock(mu);
    size_t s = ring_size(b->ring);
    pthread_mutex_unlock(mu);
    return s;
}

size_t bus_capacity(const EventBus *b) {
    return b ? ring_capacity(b->ring) : 0;
}

bool bus_get(const EventBus *b, size_t offset, LogEntry *out) {
    if (!b || !out) return false;
    pthread_mutex_t *mu = (pthread_mutex_t *)&b->mu;
    pthread_mutex_lock(mu);
    /* Compute the serial of the entry at `offset` and patch it in. */
    bool ok = ring_get(b->ring, offset, out);
    if (ok) {
        uint64_t oldest = ring_oldest_serial(b->ring);
        out->serial = oldest + offset;
    }
    pthread_mutex_unlock(mu);
    return ok;
}

bool bus_get_by_serial(const EventBus *b, uint64_t serial, LogEntry *out) {
    if (!b || !out) return false;
    pthread_mutex_t *mu = (pthread_mutex_t *)&b->mu;
    pthread_mutex_lock(mu);
    bool ok = ring_get_by_serial(b->ring, serial, out);
    if (ok) out->serial = serial;
    pthread_mutex_unlock(mu);
    return ok;
}

uint64_t bus_oldest_serial(const EventBus *b) {
    if (!b) return 0;
    pthread_mutex_t *mu = (pthread_mutex_t *)&b->mu;
    pthread_mutex_lock(mu);
    uint64_t s = ring_oldest_serial(b->ring);
    pthread_mutex_unlock(mu);
    return s;
}

uint64_t bus_newest_serial(const EventBus *b) {
    if (!b) return 0;
    pthread_mutex_t *mu = (pthread_mutex_t *)&b->mu;
    pthread_mutex_lock(mu);
    uint64_t s = ring_newest_serial(b->ring);
    pthread_mutex_unlock(mu);
    return s;
}
