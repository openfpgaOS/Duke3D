#!/usr/bin/env python3
"""Check production Duke GPU queues against SDK commands and exact wall math.

Only hardware/ring services are mocked. Extract hardware implementations
explicitly: compiling the SDK with OF_PC would test its empty GPU stubs.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    # Same extraction approach as Doom/tools/gpu_test_sources.py. Preserve
    # offsets while hiding comments/strings so their braces cannot end a body.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: ' ' * len(m[0]), source, flags=re.S)
    pattern = (r'(?m)^(?:static[^;{}]*?|(?:int32_t|void|int|uint32_t)\s+)\b'
               + re.escape(name) + r'\s*\([^;{}]*\)\s*\{')
    match = re.search(pattern, masked)
    if not match:
        raise ValueError(f'Missing production function: {name}')
    end = masked.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def prepare(output, gpu_path=None):
    gpu = (gpu_path or ROOT / 'src/duke3d/d3d_gpu.c').read_text()
    sdk = (ROOT / 'src/sdk/include/of_gpu.h').read_text()
    types = sdk[sdk.index('#include <stdint.h>'):
                sdk.index('/* ================================================================\n * MMIO Registers')]
    types = types.replace('#include "of_caps.h"', '').replace('#include "of_cache.h"', '')
    constants = sdk[sdk.index('#define OF_GPU_COLUMN_LIST_LANE_WORDS'):
                    sdk.index('/* ================================================================\n * Palookup')]
    (output / 'gpu_types.h').write_text(types + constants)
    sdk_names = ['_gpu_count12', '_gpu_affine_group_lane_count',
                 'of_gpu_draw_affine_span_group', '_gpu_column_group_lane_count',
                 'of_gpu_draw_column_list']
    (output / 'sdk_emit.h').write_text(''.join(function(sdk, n) for n in sdk_names))
    # Include the actual packed storage, assertions, flushes and append paths.
    packed = 'typedef struct {\n    uint8_t flags;\n    uint8_t live_chunks;'
    start = gpu.index(packed if packed in gpu else
                      'static of_gpu_affine_span_group_t affine_batch;')
    end = gpu.index('static inline void d3d_gpu_barrier_before_fb_read', start)
    body = function(gpu, 'd3d_gpu_batch_bytes') + gpu[start:end]
    for name in ['d3d_gpu_column_batch_compatible', 'd3d_gpu_queue_column',
                 'd3d_gpu_queue_affine_span']:
        body += function(gpu, name)
    body += '\n#define TEST_PACKED_BATCHES ' + str(int(packed in gpu)) + '\n'
    (output / 'duke_batch.h').write_text(body)
    math = (ROOT / 'src/duke3d/Engine/src/fixedPoint_math.c').read_text()
    (output / 'duke_math.h').write_text(function(math, 'build_divscale12'))
    caps = (ROOT / 'src/sdk/include/of_caps.h').read_text()
    features = ''
    for name in ['SPAN', 'FRAGPIPE', 'PARAM_SPAN_LIST', 'SPAN_GROUP', 'COLUMN_LIST']:
        line = re.search(r'^#define OF_HW_GPU_' + name + r'\s+[^\n]+', caps, re.M)[0]
        features += line.split('/*')[0] + '\n'
    (output / 'gpu_features.h').write_text(features)
    mask = gpu[gpu.index('#define D3D_GPU_REQUIRED_FEATURES'):
               gpu.index('#define D3D_GPU_TRANSLUC_WAIT_TIMEOUT_US')]
    (output / 'duke_init.h').write_text(mask + function(gpu, 'd3d_gpu_init'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/optimization-check/host')
    parser.add_argument('--reference-gpu', type=Path, help='Check a saved baseline d3d_gpu.c')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    prepare(output, args.reference_gpu)
    exe = output / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-no-pie', '-I' + str(output),
                    str(ROOT / 'tools/tests/test_duke_optimizations.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True,
                   env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1',
                            ASAN_OPTIONS='detect_leaks=0'))
    if not args.reference_gpu:
        init_exe = output / 'init-test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-I' + str(output), str(ROOT / 'tools/tests/test_duke_gpu_init.c'),
                        '-o', str(init_exe)], check=True)
        subprocess.run([str(init_exe)], check=True)


if __name__ == '__main__':
    main()
