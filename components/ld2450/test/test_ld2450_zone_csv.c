// SPDX-License-Identifier: MIT
// Host-side Unity tests for zone CSV serialization/deserialization.
//
// Build (from components/ld2450/test/):
//   make -f Makefile.csv
// Run:
//   ./test_ld2450_zone_csv

#include <stdio.h>
#include "unity.h"
#include "ld2450_zone.h"
#include "ld2450_zone_csv.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// zone_to_csv tests
// ---------------------------------------------------------------------------

void test_zone_to_csv_quad(void)
{
    ld2450_zone_t z = {
        .vertex_count = 4,
        .v = { {100,-200}, {300,400}, {-500,600}, {700,-800} }
    };
    char buf[160];
    zone_to_csv(&z, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("100,-200,300,400,-500,600,700,-800", buf);
}

void test_zone_to_csv_triangle(void)
{
    ld2450_zone_t z = {
        .vertex_count = 3,
        .v = { {0,0}, {1000,0}, {500,1000} }
    };
    char buf[160];
    zone_to_csv(&z, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0,0,1000,0,500,1000", buf);
}

void test_zone_to_csv_disabled_empty(void)
{
    ld2450_zone_t z = { .vertex_count = 0 };
    char buf[160];
    zone_to_csv(&z, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

// ---------------------------------------------------------------------------
// csv_to_zone tests
// ---------------------------------------------------------------------------

void test_csv_to_zone_quad(void)
{
    ld2450_zone_t z = { .vertex_count = 4 };
    bool ok = csv_to_zone("100,-200,300,400,-500,600,700,-800", &z);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT16(100,  z.v[0].x_mm);
    TEST_ASSERT_EQUAL_INT16(-200, z.v[0].y_mm);
    TEST_ASSERT_EQUAL_INT16(700,  z.v[3].x_mm);
    TEST_ASSERT_EQUAL_INT16(-800, z.v[3].y_mm);
}

void test_csv_to_zone_triangle(void)
{
    ld2450_zone_t z = { .vertex_count = 3 };
    bool ok = csv_to_zone("0,0,1000,0,500,1000", &z);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT16(500,  z.v[2].x_mm);
    TEST_ASSERT_EQUAL_INT16(1000, z.v[2].y_mm);
}

void test_csv_to_zone_invalid_vertex_count(void)
{
    ld2450_zone_t z = { .vertex_count = 2 };  /* below minimum */
    bool ok = csv_to_zone("0,0,1000,1000", &z);
    TEST_ASSERT_FALSE(ok);
}

// ---------------------------------------------------------------------------
// csv_count_pairs tests
// ---------------------------------------------------------------------------

void test_csv_count_pairs_quad(void)
{
    TEST_ASSERT_EQUAL_INT(4, csv_count_pairs("100,-200,300,400,-500,600,700,-800"));
}

void test_csv_count_pairs_triangle(void)
{
    TEST_ASSERT_EQUAL_INT(3, csv_count_pairs("0,0,1000,0,500,1000"));
}

void test_csv_count_pairs_empty(void)
{
    TEST_ASSERT_EQUAL_INT(0, csv_count_pairs(""));
}

void test_csv_count_pairs_null(void)
{
    TEST_ASSERT_EQUAL_INT(0, csv_count_pairs(NULL));
}

// ---------------------------------------------------------------------------
// Round-trip test
// ---------------------------------------------------------------------------

void test_csv_roundtrip(void)
{
    ld2450_zone_t orig = {
        .vertex_count = 5,
        .v = { {1,2},{3,4},{5,6},{7,8},{9,10} }
    };
    char buf[160];
    zone_to_csv(&orig, buf, sizeof(buf));

    ld2450_zone_t result = { .vertex_count = 5 };
    bool ok = csv_to_zone(buf, &result);
    TEST_ASSERT_TRUE(ok);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT16(orig.v[i].x_mm, result.v[i].x_mm);
        TEST_ASSERT_EQUAL_INT16(orig.v[i].y_mm, result.v[i].y_mm);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_zone_to_csv_quad);
    RUN_TEST(test_zone_to_csv_triangle);
    RUN_TEST(test_zone_to_csv_disabled_empty);
    RUN_TEST(test_csv_to_zone_quad);
    RUN_TEST(test_csv_to_zone_triangle);
    RUN_TEST(test_csv_to_zone_invalid_vertex_count);
    RUN_TEST(test_csv_count_pairs_quad);
    RUN_TEST(test_csv_count_pairs_triangle);
    RUN_TEST(test_csv_count_pairs_empty);
    RUN_TEST(test_csv_count_pairs_null);
    RUN_TEST(test_csv_roundtrip);

    return UNITY_END();
}
