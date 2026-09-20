#!/usr/bin/env python3
"""Measure isolated RV32 queue and division work, not device cycles or FPS."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
from check_duke_optimizations import ROOT, prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qsim', type=Path, required=True)
    parser.add_argument('--reference-gpu', type=Path, required=True,
                        help='Saved pre-optimization d3d_gpu.c')
    parser.add_argument('--cross', default='riscv64-elf-')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/optimization-check/bench')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sdk = ROOT / 'src/sdk'
    musl = sdk / 'musl'
    cc = args.cross + 'gcc'
    flags = ['-march=rv32imafc', '-mabi=ilp32f', '-O3', '-ffunction-sections',
             '-falign-functions=64', '-falign-loops=64', '-fno-tree-loop-distribute-patterns',
             '-nostdinc', '-isystem', str(musl / 'include'), '-nostdlib', '-static',
             '-T', str(sdk / 'app.ld'), '-Wl,--gc-sections', '-Wl,--no-warn-rwx-segments']

    def run(label, sources, extra):
        elf = out / (label + '.elf')
        subprocess.run([cc, *flags, *extra, *map(str, sources), '-lgcc', '-o', str(elf)], check=True)
        result = subprocess.run([str(args.qsim.resolve()), str(elf), '--out', str(out / label),
                                 '--end-on', 'BENCH_COMPLETE', '--echo', '--quiet'],
                                text=True, capture_output=True, check=True)
        log = result.stdout + result.stderr
        (out / (label + '.log')).write_text(log)
        if 'BENCH_COMPLETE' not in log or 'FAIL' in log:
            raise RuntimeError(f'{label} did not complete: {log}')
        print(log, end='', flush=True)
        return log

    gpu = {}
    for label, source in [('before', args.reference_gpu), ('after', None)]:
        job = out / ('sources-' + label)
        job.mkdir(exist_ok=True)
        prepare(job, source)
        # libc is used only for assertions/reporting, no SDK runtime or GPU
        # services. The same ring model and input sequence run on both builds.
        wrapper = job / 'gpu.c'
        wrapper.write_text('#include "' + str(ROOT / 'tools/tests/test_duke_optimizations.c') + '"\n')
        crt = [musl / 'lib' / name for name in ['crt1.o', 'crti.o', 'crtn.o']]
        log = run('gpu-' + label, [*crt, wrapper, musl / 'lib/libc.a'],
                  ['-I' + str(job), '-DD3D_BENCH'])
        gpu[label] = int(re.search(r'GPU queue/flush instructions: (\d+)', log)[1])
    math_source = out / 'math.c'
    math_source.write_text('#include <stdint.h>\n' + (out / 'sources-after/duke_math.h').read_text())
    math_log = run('math', [ROOT / 'tools/tests/bench_duke_math.c', math_source], [])
    math = []
    for match in re.finditer(r'MATH mode/calls/before/after/hash:\s+(\d+) (\d+) (\d+) (\d+) (\d+)', math_log):
        mode, calls, before, after, checksum = map(int, match.groups())
        math.append(dict(mode=mode, calls=calls, before=before, after=after, checksum=checksum))
    assert len(math) == 2
    report = dict(scope='Executed RV32 fixture instructions; excludes hardware GPU/DMA/cache timing and gameplay',
                  compiler=subprocess.check_output([cc, '--version'], text=True).splitlines()[0],
                  gpu=gpu, math=math)
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
