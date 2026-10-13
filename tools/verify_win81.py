"""Run current release binaries with isolated logs; never reuse old PASS rows."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time
import uuid

NAMES = ['luling_index_migration_test', 'luling_preview_test', 'luling_ops_test', 'luling_localization_test', 'luling_crash_test',
         'luling_index_test', 'luling_index_engine_test', 'luling_content_search_test',
         'luling_duplicate_scan_test', 'luling_saved_search_test', 'luling_search_query_test',
         'luling_update_test', 'luling_shell_icons_test',
         'luling_material_test', 'luling_app_controllers_test', 'luling_index_host_stress', 'luling']


def fingerprint(path):
    digest = hashlib.sha256()
    with path.open('rb') as binary:
        for block in iter(lambda: binary.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def run_tests(root, selected=(), runner=subprocess.run, names=NAMES):
    unknown = set(selected) - set(names)
    if unknown:
        raise ValueError('Unknown test: ' + ', '.join(sorted(unknown)))
    out = root / 'bench_data' / 'win81-tests'
    run_dir = out / ('run-' + uuid.uuid4().hex)
    profile = run_dir / 'profile'
    profile.mkdir(parents=True)
    env = dict(os.environ, LULING_TEST_DATA_DIR=str(profile), LULING_TEST_HIDDEN_SHOT='1')
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    results = []

    def save():
        contents = json.dumps(results, indent=2) + '\n'
        (run_dir / 'results.json').write_text(contents, encoding='utf-8')
        pending = out / ('results-' + run_dir.name + '.tmp')
        pending.write_text(contents, encoding='utf-8')
        pending.replace(out / 'results.json')

    for name in names:
        if selected and name not in selected:
            continue
        executable = root / 'build-win81' / (name + '.exe')
        row = dict(test=name, exit='running', executable=str(executable), run=run_dir.name)
        results.append(row)
        save()
        start = time.monotonic()
        args = [str(executable)] + (['--selftest'] if name == 'luling' else [])
        with (run_dir / (name + '.log')).open('wb') as log:
            try:
                row['sha256'] = fingerprint(executable)
                process = runner(args, stdout=log, stderr=subprocess.STDOUT, cwd=root,
                                 env=env, timeout=180, startupinfo=startup)
                row['exit'] = process.returncode
                if row['sha256'] != fingerprint(executable):
                    row['exit'] = 'binary-changed'
            except subprocess.TimeoutExpired:
                row['exit'] = 'timeout'
            except OSError as error:
                row['exit'] = 'launch-error'
                row['error'] = str(error)
                log.write((str(error) + '\n').encode('utf-8'))
        row['seconds'] = round(time.monotonic() - start, 2)
        print(json.dumps(row), flush=True)
        save()
    return int(any(row['exit'] != 0 for row in results))


def main(argv=None):
    try:
        return run_tests(Path(__file__).resolve().parents[1], sys.argv[1:] if argv is None else argv)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
