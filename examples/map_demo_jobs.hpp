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
    cui_texture raster;
    unsigned raster_used;
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
/* After the worker's three attempts, retry a still-visible transport failure at
 * most twice. Invalid PBF and an oversized decoded tile are permanent. A
 * temporary 507 cache-admission failure retries while the tile stays visible. */
bool map_demo_tile_retryable(unsigned status);
unsigned map_demo_visible_retry_delay_ms(unsigned retry_count, unsigned retry_after_ms);
void map_demo_note_failure(map_demo_slot &slot, unsigned status, unsigned retry_after_ms, unsigned now);
bool map_demo_retry_due(const map_demo_slot &slot, unsigned now);
/* Choose the oldest raster unused in the previous frame. Returns -1 if all
 * textures could still be referenced by that frame or none exist. */
int map_demo_raster_victim(const map_demo_slot *slots, int count, unsigned previous_frame);
/* Called after the previous frame has been presented, before constructing the
 * next one. Prefer tiles from other zoom levels, then the oldest target tile. */
int map_demo_decoded_victim(const map_demo_slot *slots, int count, int target_zoom);
#endif
