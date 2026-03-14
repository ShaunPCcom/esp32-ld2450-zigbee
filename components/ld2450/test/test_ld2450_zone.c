// SPDX-License-Identifier: MIT
// Host-side Unity tests for ld2450_zone variable-vertex ray casting.
//
// Build:
//   cd components/ld2450/test
//   make
// Run:
//   ./test_ld2450_zone

#include <stdio.h>
#include "unity.h"
#include "ld2450_zone.h"

// ---------------------------------------------------------------------------
// Unity stubs
// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// New variable-vertex tests (will fail until Task 1 implementation)
// ---------------------------------------------------------------------------

// Test triangle (3 vertices) — minimum valid zone
void test_zone_triangle_contains_point(void)
{
    ld2450_zone_t z = {
        .vertex_count = 3,
        .v = { {0,0}, {1000,0}, {500,1000} }
    };
    TEST_ASSERT_TRUE(ld2450_zone_contains_point(&z, (ld2450_point_t){500, 400}));
    TEST_ASSERT_FALSE(ld2450_zone_contains_point(&z, (ld2450_point_t){0, 1000}));
}

// Test pentagon (5 vertices)
void test_zone_pentagon_contains_point(void)
{
    ld2450_zone_t z = {
        .vertex_count = 5,
        .v = { {0,500},{500,0},{1000,200},{900,900},{100,900} }
    };
    TEST_ASSERT_TRUE(ld2450_zone_contains_point(&z, (ld2450_point_t){500, 500}));
    TEST_ASSERT_FALSE(ld2450_zone_contains_point(&z, (ld2450_point_t){1500, 500}));
}

// Test vertex_count=0 (disabled) always returns false
void test_zone_disabled_zero_vertices(void)
{
    ld2450_zone_t z = { .vertex_count = 0 };
    TEST_ASSERT_FALSE(ld2450_zone_contains_point(&z, (ld2450_point_t){0, 0}));
}

// Test vertex_count=1 and 2 (below minimum) also return false
void test_zone_below_minimum_vertices(void)
{
    ld2450_zone_t z1 = { .vertex_count = 1, .v = { {500, 500} } };
    ld2450_zone_t z2 = { .vertex_count = 2, .v = { {0,0}, {1000,1000} } };
    TEST_ASSERT_FALSE(ld2450_zone_contains_point(&z1, (ld2450_point_t){500, 500}));
    TEST_ASSERT_FALSE(ld2450_zone_contains_point(&z2, (ld2450_point_t){500, 500}));
}

// ---------------------------------------------------------------------------
// Existing quad tests — must keep passing after refactor
// ---------------------------------------------------------------------------

// Simple axis-aligned rectangle: (0,0)-(1000,0)-(1000,1000)-(0,1000)
void test_zone_quad_contains_point(void)
{
    ld2450_zone_t z = {
        .vertex_count = 4,
        .v = { {0,0}, {1000,0}, {1000,1000}, {0,1000} }
    };
    TEST_ASSERT_TRUE(ld2450_zone_contains_point(&z, (ld2450_point_t){500, 500}));
    TEST_ASSERT_FALSE(ld2450_zone_contains_point(&z, (ld2450_point_t){1500, 500}));
    TEST_ASSERT_FALSE(ld2450_zone_contains_point(&z, (ld2450_point_t){500, -100}));
}

// Disabled quad (vertex_count=0) ignores coords
void test_zone_quad_disabled(void)
{
    ld2450_zone_t z = {
        .vertex_count = 0,
        .v = { {0,0}, {1000,0}, {1000,1000}, {0,1000} }
    };
    TEST_ASSERT_FALSE(ld2450_zone_contains_point(&z, (ld2450_point_t){500, 500}));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_zone_triangle_contains_point);
    RUN_TEST(test_zone_pentagon_contains_point);
    RUN_TEST(test_zone_disabled_zero_vertices);
    RUN_TEST(test_zone_below_minimum_vertices);
    RUN_TEST(test_zone_quad_contains_point);
    RUN_TEST(test_zone_quad_disabled);

    return UNITY_END();
}
