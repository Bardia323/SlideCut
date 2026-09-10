"""Render each shot that plays on a screen of its own, then run the real export.

SlideCut lifts those shots out of the export's filter graph and leaves this behind to
put them back. Each one is rendered on its own, through the same shader the preview
runs, BEFORE any track blending -- so whatever is layered over a shot stays off its
glass, exactly as the preview shows it.

Runs in the export work directory. Intermediates are lossless RGBA so layer coverage
survives the round trip. No shell is involved.
"""
from pathlib import Path
import ctypes
import subprocess
import sys
import tempfile


def windows_args(command):
    """Split a command line the way Windows itself does, so quoted paths survive."""
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    count = ctypes.c_int()
    ptr = shell.CommandLineToArgvW(command, ctypes.byref(count))
    if not ptr:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return [ptr[i] for i in range(count.value)]
    finally:
        free = ctypes.windll.kernel32.LocalFree
        free.argtypes = [ctypes.c_void_p]
        free(ptr)


def main():
    read = lambda name: Path(name).read_text(encoding='utf-8-sig')
    inputs = windows_args(read('looks.inputs'))
    # The per-shot passes must not write the progress file: the UI reads it to track
    # the real encode, and a short intermediate would make the bar jump to the end.
    if '-progress' in inputs:
        i = inputs.index('-progress')
        del inputs[i:i + 2]

    renderer_args = windows_args('renderer ' + read('looks.args'))[1:]
    renderer = Path(__file__).with_name('projector_render.py')
    outputs = []
    try:
        with tempfile.TemporaryDirectory(prefix='shader_', dir='.') as temporary:
            jobs = [j for j in read('looks.jobs').splitlines() if j.strip()]
            for n, job in enumerate(jobs, 1):
                if job == 'ERROR':
                    sys.exit('a clip shader job could not be staged')
                name, look, duration, pillar = job.split()
                uid = name.removeprefix('look_')
                raw = str(Path(temporary) / (name + '.mkv'))
                output = name + '.mkv'
                outputs.append(output)
                print(f'Clip shader {n}/{len(jobs)}: {name}', flush=True)

                # 1. this shot alone, straight out of the export's own graph
                graph = read(name + '.graph').rstrip(';')
                subprocess.run(inputs + ['-filter_complex', graph, '-map', f'[v{uid}]',
                                         '-an', '-t', duration,
                                         '-c:v', 'ffv1', '-pix_fmt', 'bgra', raw], check=True)

                # 2. the same shader the preview runs, on that shot by itself
                subprocess.run([sys.executable, str(renderer), raw, '-o', output]
                               + renderer_args
                               + ['--look', look, '--lossless', '--audio', 'none',
                                  '--duration', duration]
                               + (['--pillarbox'] if pillar == '1' else []), check=True)

            # 3. the export itself, with those files standing in for those shots
            subprocess.run(windows_args(read('looks.final')), check=True)
    finally:
        for output in outputs:
            Path(output).unlink(missing_ok=True)


if __name__ == '__main__':
    main()
