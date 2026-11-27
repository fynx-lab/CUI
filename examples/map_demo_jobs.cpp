#include "map_demo_jobs.hpp"

bool map_demo_tile_retryable(unsigned status) {
    return status == 0 || status == 429 || status == 503 ||
           (status >= 500 && status < 600 && status != 507 && status != 508);
}

unsigned map_demo_visible_retry_delay_ms(unsigned retry_count, unsigned retry_after_ms) {
    if (retry_count >= 2) return 0;
    unsigned delay = retry_count == 0 ? 15000 : 45000;
    return retry_after_ms > delay ? retry_after_ms : delay;
}

void map_demo_note_failure(map_demo_slot &slot, unsigned status, unsigned retry_after_ms, unsigned now) {
    slot.state = 3;
    slot.status = status;
    unsigned delay = status == 507 ? 1000 : map_demo_tile_retryable(status) ?
        map_demo_visible_retry_delay_ms(slot.retry_count, retry_after_ms) : 0;
    slot.next_retry_tick = delay ? now + delay : 0;
}

bool map_demo_retry_due(const map_demo_slot &slot, unsigned now) {
    return slot.state == 3 && slot.next_retry_tick &&
           (slot.status == 507 || slot.retry_count < 2) &&
           (int32_t)(now - slot.next_retry_tick) >= 0;
}

int map_demo_raster_victim(const map_demo_raster_slot *slots, int count, unsigned previous_frame) {
    int oldest = -1;
    for (int i = 0; i < count; i++) {
        if (!slots[i].raster || slots[i].raster_used == previous_frame) continue;
        if (oldest < 0 || slots[i].raster_used < slots[oldest].raster_used) oldest = i;
    }
    return oldest;
}

int map_demo_decoded_victim(const map_demo_slot *slots, int count, int target_zoom) {
    int oldest = -1;
    for (int i = 0; i < count; i++) {
        if (!slots[i].tile) continue;
        if (oldest < 0 || (slots[i].z != target_zoom && slots[oldest].z == target_zoom) ||
            ((slots[i].z != target_zoom) == (slots[oldest].z != target_zoom) &&
             slots[i].used < slots[oldest].used)) oldest = i;
    }
    return oldest;
}

unsigned map_demo_retry_delay_ms(int attempt, unsigned retry_after_ms, unsigned jitter_ms) {
    if (attempt < 1 || attempt >= 3) return 0;
    if (retry_after_ms) return retry_after_ms > 30000 ? 30000 : retry_after_ms;
    unsigned backoff = 500u << (attempt - 1);
    return backoff + (jitter_ms % 251);
}

bool map_demo_result_matches(const map_demo_slot &slot, const map_demo_job &job) {
    return slot.state == 1 && slot.token == job.token &&
           slot.z == job.z && slot.x == job.x && slot.y == job.y;
}

int map_demo_cancel_stale(map_demo_slot *slots, int slot_count,
                          map_demo_job *queue, int capacity, int &head, int &tail,
                          int &count, unsigned frame) {
    for (int i = 0; i < slot_count; i++) {
        if (slots[i].state == 1 && slots[i].used != frame) {
            slots[i].state = 0;
            slots[i].token = 0;
        }
    }
    int old_count = count, kept = 0;
    for (int i = 0; i < old_count; i++) {
        map_demo_job item = queue[(head + i) % capacity];
        bool live = false;
        for (int k = 0; k < slot_count; k++) {
            if (map_demo_result_matches(slots[k], item)) { live = true; break; }
        }
        if (live) queue[(head + kept++) % capacity] = item;
    }
    count = kept;
    tail = (head + kept) % capacity;
    return old_count - kept;
}
