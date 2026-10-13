/* Files tab: the attached disks at a glance, and the way into the file manager (files.html, a page of its own because
 * it needs the room). Talks to /api/files/roots (src/x45web/api_files.c). */
(function(){
"use strict";

let roots = null, asked = 0;

function fmtSize(n){
  if(n < 1024) return n + " B";
  const u = ["KB","MB","GB","TB"]; let i = -1;
  do { n /= 1024; i++; } while(n >= 1024 && i < 3);
  return (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n.toFixed(2)) + " " + u[i];
}

function load(){
  asked = Date.now();
  const h = {}; if(token()) h["Authorization"] = "Bearer " + token();
  fetch("/api/files/roots", {headers: h}).then(r => r.ok ? r.json() : null).then(j => { roots = j || []; tabsSig = ""; sigs.body = ""; }).catch(() => { roots = []; });
}

function draw(){
  if(roots === null || Date.now() - asked > 15000) load();
  const link = p => 'files.html' + (p ? '#' + encodeURIComponent(p).replace(/%2F/g, "/") : "");
  return '<p class="dim" style="margin-top:0">Browse, upload, download, rename, move, delete and edit the files on the attached disks, like in a file explorer.</p>' +
    '<p><a href="files.html" target="_blank"><button class="on">Open the file manager</button></a></p>' +
    (roots && roots.length ? '<h3 style="margin-top:16px">Disks</h3>' + roots.map(r => {
      const used = r.total ? Math.round(100 * (r.total - r.free) / r.total) : 0;
      return '<p><a href="' + esc(link(r.path)) + '" target="_blank"><b>' + esc(r.name) + '</b></a> <span class="dim">' + esc(r.fs) + (r.rw ? "" : ", read-only") +
        '</span><span class="bar" style="display:block;cursor:default"><div style="width:' + used + '%"></div></span>' +
        '<span class="dim">' + fmtSize(r.free) + ' free of ' + fmtSize(r.total) + '</span></p>';
    }).join("") : roots ? '<p class="dim">No disk is attached.</p>' : '<p class="dim">loading...</p>');
}

registerTab({ id: "files", label: "Files", screen: -1, render: draw, wire: () => {} });
})();
