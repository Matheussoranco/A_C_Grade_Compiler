"""The real Makefile must fail for a failing compiler, even after dump success."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
for mode in ('always-fail', 'compile-only-fail'):
    with tempfile.TemporaryDirectory(prefix='acc-gate-') as temp:
        target = Path(temp)
        shutil.copy2(ROOT / 'Makefile', target / 'Makefile')
        compiler = target / 'acc'
        compiler.write_text('#!/bin/sh\n' + (
            'echo controlled-failure; exit 7\n' if mode == 'always-fail' else
            'case "$*" in *-dump-*) echo dump-ok; exit 0;; esac\necho controlled-failure; exit 7\n'))
        compiler.chmod(0o700)
        # This copy has no source/object prerequisites: -o marks only acc current.
        result = subprocess.run(['make', '-o', 'acc', 'test'], cwd=target,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode != 0, result.stdout
        assert 'Tests complete' not in result.stdout, result.stdout
print('Both compiler failure paths stop make test.')
