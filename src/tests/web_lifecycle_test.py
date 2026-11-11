"""Run against a native x45web binary, with no device or writable device paths."""
import concurrent.futures
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

with tempfile.TemporaryDirectory() as d:
    pathlib.Path(d, 'index.html').write_text('test page')
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    p = subprocess.Popen([sys.argv[1], '-p', str(port), '-d', d])
    try:
        def get(_):
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/', timeout=3) as r:
                assert r.read() == b'test page'
        for _ in range(50):
            try:
                get(0)
                break
            except OSError:
                time.sleep(.05)
        else:
            raise AssertionError('server did not start')
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(get, range(120)))
        # No new request to wake accept: all workers must still be reaped.
        time.sleep(.7)
        children = pathlib.Path(f'/proc/{p.pid}/task/{p.pid}/children').read_text().strip()
        assert not children, f'unreaped children: {children}'
        print('web: 120 concurrent requests passed; no idle zombies')
    finally:
        p.terminate()
        p.wait(timeout=5)
