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
    std::fprintf(stderr, "map demo jobs tests: %s\n", errors ? "FAILED" : "OK");
    return errors ? 1 : 0;
}
