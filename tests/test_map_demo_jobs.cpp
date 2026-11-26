#include "../examples/map_demo_jobs.hpp"
#include <cstdio>

static int errors;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); errors++; } } while (0)
int main() {
    map_demo_slot slots[3] = {};
    slots[0] = map_demo_slot{8, 1, 2, 1, 42, nullptr, 100};
    slots[1] = map_demo_slot{8, 2, 2, 1, 41, nullptr, 101};
    slots[2] = map_demo_slot{8, 3, 2, 1, 42, nullptr, 102};
    map_demo_job queue[4] = {};
    queue[3] = map_demo_job{8,1,2,"/a",100};
    queue[0] = map_demo_job{8,2,2,"/b",101};
    queue[1] = map_demo_job{8,3,2,"/c",102};
    int head = 3, tail = 2, count = 3;
    CHECK(map_demo_cancel_stale(slots, 3, queue, 4, head, tail, count, 42) == 1);
    CHECK(slots[1].state == 0 && slots[1].token == 0);
    CHECK(count == 2 && head == 3 && tail == 1);
    CHECK(queue[3].token == 100 && queue[0].token == 102);
    CHECK(map_demo_result_matches(slots[0], queue[3]));
    map_demo_job stale = map_demo_job{8,2,2,"/old",101};
    slots[1] = map_demo_slot{8,2,2,1,42,nullptr,104};
    CHECK(!map_demo_result_matches(slots[1], stale));
    map_demo_job replacement = map_demo_job{8,2,2,"/new",104};
    CHECK(map_demo_result_matches(slots[1], replacement));
    CHECK(map_demo_cancel_stale(slots, 3, queue, 4, head, tail, count, 43) == 2);
    CHECK(count == 0 && head == tail);
    CHECK(map_demo_retry_delay_ms(1, 0, 0) == 500);
    CHECK(map_demo_retry_delay_ms(2, 0, 250) == 1250);
    CHECK(map_demo_retry_delay_ms(1, 12000, 0) == 12000);
    CHECK(map_demo_retry_delay_ms(1, 120000, 0) == 30000);
    CHECK(map_demo_retry_delay_ms(3, 0, 0) == 0);
    CHECK(map_demo_tile_retryable(0));
    CHECK(map_demo_tile_retryable(429));
    CHECK(map_demo_tile_retryable(503));
    CHECK(map_demo_tile_retryable(500));
    CHECK(!map_demo_tile_retryable(404));
    CHECK(!map_demo_tile_retryable(200)); /* corrupt MVT, not HTTP success */
    CHECK(!map_demo_tile_retryable(507)); /* cache budget, not transport */
    CHECK(map_demo_visible_retry_delay_ms(0, 30000) == 30000);
    CHECK(map_demo_visible_retry_delay_ms(0, 0) == 15000);
    CHECK(map_demo_visible_retry_delay_ms(1, 0) == 45000);
    CHECK(map_demo_visible_retry_delay_ms(2, 0) == 0);
    map_demo_slot failed = {};
    failed.state = 3;
    map_demo_note_failure(failed, 503, 30000, 1000);
    CHECK(failed.next_retry_tick == 31000 && !map_demo_retry_due(failed, 30999));
    CHECK(map_demo_retry_due(failed, 31000));
    failed.retry_count = 2;
    map_demo_note_failure(failed, 503, 0, 35000);
    CHECK(!map_demo_retry_due(failed, 999999));
    failed.retry_count = 0;
    map_demo_note_failure(failed, 404, 0, 35000);
    CHECK(!map_demo_retry_due(failed, 50000));
    map_demo_note_failure(failed, 0, 0, 0xfffffff0u);
    CHECK(map_demo_retry_due(failed, 0x3a88u)); /* tick wrap at 15000ms */
    failed.retry_count = 2;
    map_demo_note_failure(failed, 507, 0, 5000);
    CHECK(failed.next_retry_tick == 6000 && !map_demo_retry_due(failed, 5999));
    CHECK(map_demo_retry_due(failed, 6000)); /* capacity is temporary */
    map_demo_note_failure(failed, 508, 0, 6000);
    CHECK(!map_demo_retry_due(failed, 99999)); /* tile larger than cache */
    map_demo_slot images[4] = {};
    images[0].raster = 1; images[0].raster_used = 4;
    images[1].raster = 2; images[1].raster_used = 2;
    images[2].raster = 3; images[2].raster_used = 6;
    CHECK(map_demo_raster_victim(images, 4, 6) == 1);
    images[1].raster = 0;
    CHECK(map_demo_raster_victim(images, 4, 6) == 0);
    images[0].raster_used = 6;
    CHECK(map_demo_raster_victim(images, 4, 6) == -1);
    CHECK(map_demo_raster_victim(images, 4, 7) == 0);
    /* pump runs after the previous frame has been presented: a full cache of
     * previously visible tiles must still be able to admit the next tile. */
    map_demo_slot decoded[3] = {};
    decoded[0].z = 10; decoded[0].tile = reinterpret_cast<cui_mvt_tile *>(1);
    decoded[0].used = 42;
    decoded[1].z = 9; decoded[1].tile = reinterpret_cast<cui_mvt_tile *>(1);
    decoded[1].used = 42;
    decoded[2].z = 9; decoded[2].tile = reinterpret_cast<cui_mvt_tile *>(1);
    decoded[2].used = 41;
    CHECK(map_demo_decoded_victim(decoded, 3, 9) == 0); /* old zoom first */
    decoded[0].tile = nullptr;
    CHECK(map_demo_decoded_victim(decoded, 3, 9) == 2); /* then LRU */
    decoded[2].tile = nullptr;
    CHECK(map_demo_decoded_victim(decoded, 3, 9) == 1); /* even last frame */
    decoded[1].tile = nullptr;
    CHECK(map_demo_decoded_victim(decoded, 3, 9) == -1);
    std::fprintf(stderr, "map demo jobs tests: %s\n", errors ? "FAILED" : "OK");
    return errors ? 1 : 0;
}
