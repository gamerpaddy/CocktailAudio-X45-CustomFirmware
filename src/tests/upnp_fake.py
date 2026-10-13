#!/usr/bin/env python3
"""A tiny fake UPnP ContentDirectory server for src/tests/upnp_test.sh.

POST /cd with an ObjectID in the SOAP body:
  "root"            -> one folder whose id contains an ampersand, one track, TotalMatches 300
  "Simon & Garfunkel" (sent as &amp;) -> the folder was asked for with its id escaped: one track
  "chunked-short"   -> a chunked reply whose last chunk claims more bytes than it carries
  "chunked-nul"     -> a chunked reply whose only chunk is made of NUL bytes
  anything else     -> 500
It records every ObjectID it received, as text, in the file given as argv[2].
"""
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

LOG = sys.argv[2]


def didl(items):
    return ('<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
            'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
            + "".join(items) + "</DIDL-Lite>")


def soap(result, total):
    esc = result.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
            '<u:BrowseResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1">'
            "<Result>%s</Result><NumberReturned>2</NumberReturned><TotalMatches>%d</TotalMatches>"
            "<UpdateID>1</UpdateID></u:BrowseResponse></s:Body></s:Envelope>" % (esc, total))


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
        m = re.search(r"<ObjectID>(.*?)</ObjectID>", body)
        oid_raw = m.group(1) if m else ""
        with open(LOG, "a") as f:
            f.write(oid_raw + "\n")
        oid = oid_raw.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
        if oid == "root":
            r = didl(['<container id="Simon &amp; Garfunkel" parentID="root"><dc:title>Simon &amp; Garfunkel</dc:title></container>',
                      '<item id="t1" parentID="root"><dc:title>Track one</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class>'
                      '<res protocolInfo="http-get:*:audio/flac:*">http://127.0.0.1:1/a.flac</res></item>'])
            data = soap(r, 300).encode()
        elif oid == "Simon & Garfunkel":
            r = didl(['<item id="t2" parentID="x"><dc:title>The Boxer</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class>'
                      '<res protocolInfo="http-get:*:audio/mpeg:*">http://127.0.0.1:1/boxer.mp3?' + "q" * 300 + '</res></item>'])
            data = soap(r, 1).encode()
        elif oid == "chunked-short":
            data = soap(didl([]), 0).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/xml")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"%x\r\n" % 20 + data[:20] + b"\r\n")
            self.wfile.write(b"ffff\r\n" + data[20:60])   # announces 65535 bytes, sends 40, then hangs up
            self.wfile.flush()
            self.close_connection = True
            return
        elif oid == "chunked-nul":
            self.send_response(200)
            self.send_header("Content-Type", "text/xml")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"28\r\n" + b"\0" * 40 + b"\r\n0\r\n\r\n")   # a chunk made of NUL bytes
            self.wfile.flush()
            self.close_connection = True
            return
        else:
            self.send_response(500)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/xml")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True


HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
