#include "map_demo_jobs.hpp"

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
