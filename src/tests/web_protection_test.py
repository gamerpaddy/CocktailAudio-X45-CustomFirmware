import socket, sys, time, subprocess, os

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080


def req(raw, wait=True):
    s = socket.create_connection(('127.0.0.1', PORT), timeout=8)
    s.sendall(raw)
    data = b''
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            data += d
            if not wait:
                break
    except Exception as e:
        data += ('[' + str(e) + ']').encode()
    s.close()
    return data


def first(d):
    return d.split(b'\r\n', 1)[0].decode('latin1')


def hdrs(d):
    return d.split(b'\r\n\r\n', 1)[0].decode('latin1')


fails = 0


def check(name, cond, extra=''):
    global fails
    print(('ok   ' if cond else 'FAIL ') + name + (' ' + extra if (extra and not cond) else ''))
    if not cond:
        fails += 1


def post(path, body, host='127.0.0.1:%d' % PORT, origin=None, extra=''):
    h = 'POST %s HTTP/1.1\r\nHost: %s\r\n' % (path, host)
    if origin:
        h += 'Origin: %s\r\n' % origin
    h += extra
    h += 'Content-Length: %d\r\n\r\n' % len(body)
    return req(h.encode() + body)


d = req(b'GET /index.html HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n' % PORT)
check('plain GET index', first(d).startswith('HTTP/1.1 200'), first(d))
check('no ACAO by default', 'Access-Control-Allow-Origin' not in hdrs(d), hdrs(d))
d = req(b'GET /index.html HTTP/1.1\r\nHost: evil.example.com\r\n\r\n')
check('foreign host name refused (DNS rebinding)', first(d).startswith('HTTP/1.1 403'), first(d))
for h in (b'localhost:%d' % PORT, b'x45', b'x45.local', b'cocktail.fritz.box', b'[::1]:80'):
    d = req(b'GET /index.html HTTP/1.1\r\nHost: ' + h + b'\r\n\r\n')
    check('host ' + h.decode() + ' allowed', first(d).startswith('HTTP/1.1 200'), first(d))
d = post('/api/cmd', b'noop\n', origin='http://evil.example.com')
check('cross-site POST refused', first(d).startswith('HTTP/1.1 403'), first(d))
d = post('/api/cmd', b'noop\n', origin='null')
check('Origin null POST refused', first(d).startswith('HTTP/1.1 403'), first(d))
d = post('/api/cmd', b'noop\n', origin='http://127.0.0.1:%d' % PORT)
check('same-origin POST accepted', first(d).startswith('HTTP/1.1 200'), first(d))
d = post('/api/cmd', b'noop\n')
check('POST without Origin (curl, apps) accepted', first(d).startswith('HTTP/1.1 200'), first(d))
d = req(b'OPTIONS /api/cmd HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nOrigin: http://evil.example.com\r\nAccess-Control-Request-Method: PUT\r\n\r\n' % PORT)
check('preflight gets no CORS grant', 'Access-Control-Allow' not in hdrs(d), hdrs(d))
d = req(b'PUT /api/files/put?p=/tmp/x HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nOrigin: http://evil.example.com\r\nContent-Length: 1\r\n\r\nx' % PORT)
check('cross-site PUT refused', first(d).startswith('HTTP/1.1 403'), first(d))
d = post('/api/files/op?cmd=delete&p=/mnt/HDD4/x', b'', origin='http://evil.example.com')
check('cross-site file delete refused', first(d).startswith('HTTP/1.1 403'), first(d))
d = req(b'GET /' + b'a' * 3000 + b' HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n')
check('over-long address refused', first(d).startswith('HTTP/1.1 414'), first(d))
d = post('/api/cmd', b'x' * 5000)
check('over-long command refused', first(d).startswith('HTTP/1.1 413'), first(d))
# trickling sender is dropped by the 20 s deadline
s = socket.create_connection(('127.0.0.1', PORT), timeout=40)
t0 = time.time()
try:
    for i in range(30):
        s.sendall(b'G')
        time.sleep(1)
except Exception:
    pass
dt = time.time() - t0
s.close()
check('slow sender dropped within ~25 s (took %.1f)' % dt, dt < 26)
d = req(b'GET /api/access HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n')
check('no token endpoint any more', first(d).startswith('HTTP/1.1 404'), first(d))
d = req(b'GET /api/files/roots HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n')
check('file manager needs no token', not first(d).startswith('HTTP/1.1 401'), first(d))
print('FAILS', fails)
sys.exit(1 if fails else 0)
