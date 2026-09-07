#include "test.h"
#include "ring.h"
TEST(push_pop_order) {
    uint8_t buf[8]; ring_t r; ring_init(&r, buf, 8);
    ASSERT_EQ(ring_count(&r), 0); ASSERT_EQ(ring_free(&r), 7);
    for (int i = 0; i < 7; i++) ASSERT(ring_push(&r, (uint8_t)i));
    ASSERT(!ring_push(&r, 99));              /* full: 7 usable in a size-8 ring */
    uint8_t b; ASSERT(ring_peek(&r, &b)); ASSERT_EQ(b, 0);
    for (int i = 0; i < 7; i++) { ASSERT(ring_pop(&r, &b)); ASSERT_EQ(b, i); }
    ASSERT(!ring_pop(&r, &b));
}
TEST(wraps) {
    uint8_t buf[4]; ring_t r; ring_init(&r, buf, 4); uint8_t b;
    for (int i = 0; i < 100; i++) { ASSERT(ring_push(&r, (uint8_t)i)); ASSERT(ring_pop(&r, &b)); ASSERT_EQ(b, (uint8_t)i); }
}
int main(void) { RUN(push_pop_order); RUN(wraps); TEST_MAIN_END }
