import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1])
linker = sys.argv[2:]
# Distinct names bypass readFile's cache; hard links keep the fixture small.
inputs = []
for i in range(8192):
    path = root / (str(i) + '.o')
    os.link(root / 'input.o', path)
    inputs.append('"' + str(path) + '"')
response = root / 'inputs.rsp'
response.write_text('\n'.join(inputs))
base = linker + ['@' + str(response), '-dylib', '-no_uuid', '-no_adhoc_codesign']


def run(threads, output, failure=False, returned_error=False, trace=None):
    command = base + ['--threads=' + str(threads), '-o', output]
    if trace:
        command += ['--time-trace=' + str(trace), '--time-trace-granularity=0']
    # A fatal returned error stops the link-twice driver on its first, silent
    # iteration. Use one iteration for its diagnostic; successful calls below
    # inherit LLD_IN_TEST and exercise actual library cleanup/re-entry.
    env = dict(os.environ, LLD_IN_TEST="1") if returned_error else None
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=120, start_new_session=True, env=env)
    stderr = result.stderr.decode(errors='replace')
    assert 'AddressSanitizer' not in stderr, stderr
    assert 'ThreadSanitizer' not in stderr, stderr
    if returned_error:
        assert result.returncode == 1, (result.returncode, stderr)
        assert "failed to write output '/dev/tty':" in stderr, stderr
    elif failure:
        assert result.returncode == 1, (result.returncode, stderr)
        assert 'IO failure on output stream: No space left on device' in stderr, stderr
    else:
        assert result.returncode == 0, stderr
        assert not stderr, stderr
    return result.stdout


# /dev/tty is a special file, but opening it in a new session without a
# controlling terminal fails. This exercises commit() returning an Error.
run(1, '/dev/tty', returned_error=True)
run(8, '/dev/tty', returned_error=True)
run(1, '/dev/full', failure=True)
# Exercise the scheduling window repeatedly: the failure must stay a clean
# diagnostic on every attempt, even if the first release happens to finish.
for _ in range(5):
    run(8, '/dev/full', failure=True)

# Both successful in-memory commit paths still release inputs and produce the
# same output as the one-thread control (UUID and signing are disabled).
base = linker + [str(root / 'input.o'), '-dylib', '-no_uuid', '-no_adhoc_codesign']
assert run(1, '-') == run(8, '-')
trace = root / 'null.json'
run(8, '/dev/null', trace=trace)
assert any(e['name'] == 'Release input buffers'
           for e in json.loads(trace.read_text())['traceEvents'])
