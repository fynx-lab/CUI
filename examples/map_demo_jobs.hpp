#ifndef CUI_MAP_DEMO_JOBS_HPP
#define CUI_MAP_DEMO_JOBS_HPP
#include <cui/cui.h>
#include <stdint.h>

/* UI-thread slots; workers see only job copies and completion tokens. */
struct map_demo_slot {
    int z, x, y, state;
    unsigned used;
    cui_mvt_tile *tile;
    uint64_t token;
    unsigned next_retry_tick, status, retry_count;
};
struct map_demo_job {
    int z, x, y;
    char path[240];
    uint64_t token;
};

/* Caller holds its queue lock. Only the UI thread touches slots. Jobs are
 * compacted in FIFO order; an already-running job can finish but its token
 * cannot match a recycled slot. Returns the number of queued jobs removed. */
int map_demo_cancel_stale(map_demo_slot *slots, int slot_count,
                          map_demo_job *queue, int capacity, int &head, int &tail,
                          int &count, unsigned frame);
bool map_demo_result_matches(const map_demo_slot &slot, const map_demo_job &job);
unsigned map_demo_retry_delay_ms(int attempt, unsigned retry_after_ms, unsigned jitter_ms);
/* After the worker's three attempts, retry a still-visible tile at most twice.
 * HTTP 200 with a failed decode is permanent; 507 is the demo's cache limit. */
bool map_demo_tile_retryable(unsigned status);
unsigned map_demo_visible_retry_delay_ms(unsigned retry_count, unsigned retry_after_ms);
void map_demo_note_failure(map_demo_slot &slot, unsigned status, unsigned retry_after_ms, unsigned now);
bool map_demo_retry_due(const map_demo_slot &slot, unsigned now);
#endif
