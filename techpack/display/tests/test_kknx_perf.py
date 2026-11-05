#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise KKNX load classification and five-sample smoothing."""

import unittest

from test_hotpath import DISPLAY, definition, run_c


SOURCE = DISPLAY.parents[1] / "drivers/misc/kknx_performance/kknx_perf.c"
HEADER = DISPLAY.parents[1] / "include/linux/kknx_perf.h"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define U32_MAX UINT32_MAX
#define READ_ONCE(x) (x)
#define min(a, b) ((a) < (b) ? (a) : (b))
typedef uint32_t u32;
typedef uint64_t u64;
static u64 div64_u64(u64 a, u64 b) { assert(b); return a / b; }
static u64 div_u64(u64 a, u32 b) { assert(b); return a / b; }
"""


def program(body):
    source = SOURCE.read_text()
    header = HEADER.read_text()
    text = PREFIX + definition(header, "enum kknx_perf_load_level {") + ";\n"
    text += source[source.index("#define KKNX_SAMPLE_PERIOD"):
                   source.index("struct kknx_window {")]
    text += definition(source, "struct kknx_window {") + ";\n"
    text += definition(source, "struct kknx_gpu_window {") + ";\n"
    text += "struct kknx_perf_state { enum kknx_perf_load_level level; };\n"
    text += "static struct kknx_perf_state kknx_perf;\n"
    for signature in (
        "static void kknx_window_add(",
        "static u32 kknx_window_cpu_percent_x100(",
        "static void kknx_gpu_window_add(",
        "static u32 kknx_window_gpu_percent_x100(",
        "static bool kknx_load_is_minimal(",
        "static bool kknx_load_is_medium(",
        "static bool kknx_load_is_high(",
        "static enum kknx_perf_load_level\nkknx_classify_load(",
        "static bool kknx_should_probe_60hz(",
        "enum kknx_perf_load_level kknx_perf_get_load_level(",
        "bool kknx_perf_is_minimal_load(",
        "bool kknx_perf_is_medium_load(",
        "bool kknx_perf_is_high_load(",
    ):
        text += definition(source, signature) + "\n"
    text += "int main(void)\n{\n" + body
    text += "\nreturn 0;\n}\n"
    return text


class KknxPerfTests(unittest.TestCase):
    def test_load_thresholds_and_unknown_probe(self):
        run_c(
            '#pragma GCC diagnostic ignored "-Wunused-function"\n' +
            program(r"""
            assert(kknx_classify_load(2000, 60, 10000, true) ==
                   KKNX_PERF_LEVEL_MINIMAL);
            assert(kknx_classify_load(2001, 60, 0, true) ==
                   KKNX_PERF_LEVEL_MEDIUM);
            assert(kknx_classify_load(2001, 120, 0, true) ==
                   KKNX_PERF_LEVEL_MEDIUM);
            assert(kknx_classify_load(5000, 120, 3100, true) ==
                   KKNX_PERF_LEVEL_MEDIUM);
            assert(kknx_classify_load(5001, 120, 3000, true) ==
                   KKNX_PERF_LEVEL_MEDIUM);
            assert(kknx_classify_load(5001, 120, 3001, true) ==
                   KKNX_PERF_LEVEL_HIGH);
            assert(kknx_classify_load(5001, 60, 9000, true) ==
                   KKNX_PERF_LEVEL_MEDIUM);
            assert(kknx_classify_load(2000, 120, 0, true) ==
                   KKNX_PERF_LEVEL_UNKNOWN);
            assert(kknx_should_probe_60hz(KKNX_PERF_LEVEL_UNKNOWN, 2000, 120));
            assert(!kknx_should_probe_60hz(KKNX_PERF_LEVEL_MEDIUM, 2000, 120));
            assert(!kknx_should_probe_60hz(KKNX_PERF_LEVEL_UNKNOWN, 2001, 120));
            assert(!kknx_should_probe_60hz(KKNX_PERF_LEVEL_UNKNOWN, 1000, 60));

            kknx_perf.level = KKNX_PERF_LEVEL_MINIMAL;
            assert(kknx_perf_is_minimal_load());
            assert(!kknx_perf_is_medium_load() && !kknx_perf_is_high_load());
            kknx_perf.level = KKNX_PERF_LEVEL_MEDIUM;
            assert(kknx_perf_is_medium_load());
            kknx_perf.level = KKNX_PERF_LEVEL_HIGH;
            assert(kknx_perf_is_high_load());
            puts("KKNX levels: strict boundaries, high precedence, 120-to-60 probe");
            """)
        )

    def test_five_second_windows_reject_short_spikes(self):
        run_c(
            '#pragma GCC diagnostic ignored "-Wunused-function"\n' +
            program(r"""
            struct kknx_window cpu = {0};
            struct kknx_gpu_window gpu = {0};
            unsigned int i;

            assert(kknx_window_cpu_percent_x100(&cpu) == U32_MAX);
            kknx_window_add(&cpu, 100, 100);
            for (i = 0; i < 4; i++)
                kknx_window_add(&cpu, 0, 100);
            assert(kknx_window_cpu_percent_x100(&cpu) == 2000);
            assert(kknx_classify_load(kknx_window_cpu_percent_x100(&cpu),
                                      60, 0, false) ==
                   KKNX_PERF_LEVEL_MINIMAL);
            kknx_window_add(&cpu, 0, 100);
            assert(kknx_window_cpu_percent_x100(&cpu) == 0);
            kknx_window_add(&cpu, 100, 100);
            assert(kknx_window_cpu_percent_x100(&cpu) == 2000);
            kknx_window_add(&cpu, 100, 100);
            assert(kknx_window_cpu_percent_x100(&cpu) == 4000);
            assert(kknx_classify_load(kknx_window_cpu_percent_x100(&cpu),
                                      60, 0, false) ==
                   KKNX_PERF_LEVEL_MEDIUM);

            kknx_gpu_window_add(&gpu, 100, true);
            for (i = 0; i < 4; i++)
                kknx_gpu_window_add(&gpu, 0, true);
            assert(kknx_window_gpu_percent_x100(&gpu) == 2000);
            kknx_gpu_window_add(&gpu, 100, false);
            assert(kknx_window_gpu_percent_x100(&gpu) == 0);
            assert(gpu.valid_count == 4);
            puts("KKNX windows: one-second spikes are diluted over five samples");
            """)
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
