import os
import subprocess
import sys
probe, loader = sys.argv[1:]
def run(*args, env=None):
    return subprocess.run([probe, *args], text=True, capture_output=True, env=env)
r = run('--loader', loader)
assert r.returncode == 0, (r.stdout, r.stderr)
assert 'DVE fake runtime' in r.stdout and 'DVE fake headset' in r.stdout
assert 'Vulkan extension advertised: yes' in r.stdout and 'Discovery only' in r.stdout
assert 'fake destroy called' in r.stderr
no_headset = dict(os.environ, DVE_TEST_NO_HEADSET='1')
r = run('--loader', loader, env=no_headset)
assert r.returncode != 0 and 'headset system unavailable' in r.stderr and 'fake destroy called' in r.stderr
r = run('--loader', loader + '.missing')
assert r.returncode != 0 and 'loader unavailable' in r.stderr
r = run('--bad-argument')
assert r.returncode == 2 and 'Usage:' in r.stderr
print('OpenXR discovery probe: fake loader and failure paths passed')
