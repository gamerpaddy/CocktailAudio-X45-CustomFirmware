/* Health tab: how the unit is doing (GET /api/health), its logs (GET /api/logs), the access token (GET/POST /api/access)
 * and the child lock of the panel (`childlock 1|0`, `childlock volume 0|1`). Everything is read from the unit itself. */
(function(){
"use strict";
let H = null, ACC = null, loading = false, last = 0;
let LOGS = null, logName = "ui", logOld = 0, logLines = 300, logText = null, logNote = "";
let accNote = "";

const kb = v => v >= 1048576 ? (v / 1048576).toFixed(1) + " GB" : v >= 1024 ? Math.round(v / 1024) + " MB" : v + " kB";
const mb = v => v >= 1024 ? (v / 1024).toFixed(1) + " GB" : v + " MB";
const dur = s => { s = Math.floor(s || 0); const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return d ? d + " d " + h + " h" : h ? h + " h " + m + " min" : m + " min " + (s % 60) + " s"; };
const bytes = n => n >= 1073741824 ? (n / 1073741824).toFixed(1) + " GB" : n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : Math.round(n / 1024) + " kB";
const hdrs = () => { const h = {}; if(token()) h["Authorization"] = "Bearer " + token(); return h; };

async function fetchHealth(){
  if(loading) return;
  loading = true; last = Date.now();
  try{
    const r = await fetch("/api/health", { cache: "no-store" });
    if(r.ok){ H = await r.json(); sigs.body = null; if(window.render) window.render(); }
  }catch(e){}
  try{
    const r = await fetch("/api/access", { cache: "no-store" });
    if(r.ok) ACC = await r.json();
  }catch(e){}
  loading = false;
}

async function fetchLogs(){
  try{
    const r = await fetch("/api/logs", { cache: "no-store", headers: hdrs() });
    if(r.ok){ LOGS = await r.json(); } else if(r.status === 401){ logNote = "The logs need the access token."; }
  }catch(e){}
  sigs.body = null; if(window.render) window.render();
}

async function loadLog(){
  const name = logName === "kernel" ? "kernel" : logName + (logOld ? "." + logOld : "") + "/" + logLines;
  logText = "loading..."; sigs.body = null; if(window.render) window.render();
  try{
    const r = await fetch("/api/logs/" + name, { cache: "no-store", headers: hdrs() });
    if(r.status === 401){
      const t = prompt("This unit needs an access token:");
      if(t){ try{ localStorage.setItem(TOKEN_KEY, t); }catch(e){} return loadLog(); }
      logText = "The logs need the access token.";
    }else{
      logText = r.ok ? (await r.text()) || "(empty)" : "No such log yet.";
    }
  }catch(e){ logText = "Cannot read the log."; }
  sigs.body = null; if(window.render) window.render();
  const pre = document.getElementById("hlpre"); if(pre) pre.scrollTop = pre.scrollHeight;
}

async function setToken(tok){
  try{
    const r = await fetch("/api/access", { method: "POST", body: tok, headers: hdrs() });
    if(r.status === 401){
      const t = prompt("This unit needs the current access token:");
      if(t){ try{ localStorage.setItem(TOKEN_KEY, t); }catch(e){} return setToken(tok); }
      accNote = "Not changed."; return;
    }
    const j = await r.json().catch(() => ({}));
    if(r.ok){
      try{ if(tok) localStorage.setItem(TOKEN_KEY, tok); else localStorage.removeItem(TOKEN_KEY); }catch(e){}
      accNote = tok ? "Saved. Changes now need this token; looking is still free." : "Removed. Everybody can change things again.";
    }else{
      accNote = j.error || "Not saved";
    }
  }catch(e){ accNote = "Not saved: " + e; }
  last = 0; fetchHealth();
}

function bar(frac, warnAt){
  const p = Math.max(0, Math.min(1, frac));
  return '<div class="bar" style="cursor:default;margin:4px 0"><div style="width:' + Math.round(p * 100) + '%;' + (p > (warnAt || 0.9) ? "background:var(--warn)" : "") + '"></div></div>';
}

function verdict(){
  const w = [];
  const t = (H.temps[0] || {}).c;
  if(t != null && t > 75) w.push("the processor is hot (" + t + " C)");
  if(H.mem.total && H.mem.avail / H.mem.total < 0.1) w.push("little memory is left");
  H.disks.forEach(d => { if(d.total_mb && d.free_mb / d.total_mb < 0.05) w.push(d.mount + " is nearly full"); });
  H.net.forEach(n => { if(n.rx_err > 100) w.push(n.if + " has receive errors"); });
  if(H.wd && !H.wd.armed) w.push("the hardware watchdog is not armed");
  if(!H.wd) w.push("the watchdog service is not running");
  if(H.wd && H.wd.gave_up) w.push("the supervisor was given up on");
  if(!H.procs.mpd.n) w.push("the music server is not running");
  if(!H.procs.x45ui.n) w.push("the panel UI is not running");
  return w;
}

function cards(){
  const w = verdict();
  const memUsed = H.mem.total - H.mem.avail;
  let h = '<div class="row" style="justify-content:space-between"><div><span class="big ' + (w.length ? "bad" : "acc") + '">' +
    (w.length ? "Needs a look" : "All well") + '</span><div class="dim">' + (w.length ? esc(w.join("; ")) : "Up for " + dur(H.up) + ".") + '</div></div>' +
    '<button id="hrefresh">Refresh</button></div>';
  h += '<div class="dim" style="margin-top:6px">Up ' + dur(H.up) + ' &middot; load ' + H.load.join(" ") +
    (H.temps.length ? " &middot; " + H.temps.map(t => esc(t.zone) + " " + t.c + " &deg;C").join(", ") : "") + '</div>';
  h += '<h3>Memory</h3>' + bar(memUsed / (H.mem.total || 1), 0.9) +
    '<div class="dim">' + kb(memUsed) + ' used of ' + kb(H.mem.total) + ', ' + kb(H.mem.avail) + ' available' +
    (H.mem.swap_total ? ', swap ' + kb(H.mem.swap_total - H.mem.swap_free) + ' of ' + kb(H.mem.swap_total) : "") + '</div>';
  h += '<h3>Storage</h3>' + H.disks.map(d => '<div>' + esc(d.mount) + ' <span class="dim">' + esc(d.fs) + ', ' + mb(d.free_mb) + ' free of ' + mb(d.total_mb) + '</span>' +
    bar(1 - d.free_mb / (d.total_mb || 1), 0.95) + '</div>').join("");
  h += '<h3>Network</h3>' + (H.net.length ? H.net.map(n => '<div>' + esc(n.if) + ' <span class="' + (n.state === "up" ? "ok" : "dim") + '">' + esc(n.state || "?") + '</span>' +
    (n.speed ? ' <span class="dim">' + n.speed + ' Mbit</span>' : "") +
    '<div class="dim">received ' + bytes(n.rx) + ', sent ' + bytes(n.tx) + ', errors ' + (n.rx_err + n.tx_err) + ', dropped ' + n.rx_drop + '</div></div>').join("") : '<div class="dim">none</div>');
  h += '<h3>Services</h3><div class="row" style="gap:6px;flex-wrap:wrap">' + Object.keys(H.procs).filter(k => H.procs[k].n || /^(x45ui|mpd|x45web|x45wd|x45stack)$/.test(k)).map(k =>
    '<span class="pill ' + (H.procs[k].n ? "" : "warn") + '">' + esc(k) + (H.procs[k].n ? " " + kb(H.procs[k].rss) : " stopped") + '</span>').join("") + '</div>';
  return h;
}

function guard(){
  const wd = H.wd;
  let h = '<h3>Protection</h3>';
  if(wd){
    h += '<div>Hardware watchdog: <span class="' + (wd.armed ? "ok" : "bad") + '">' + (wd.armed ? "armed, " + wd.timeout + " s" : "not armed") + '</span>' +
      ' <span class="dim">(fed ' + (wd.fed_ago >= 0 ? wd.fed_ago + ' s ago' : "never") + (wd.stalled ? "; STOPPED" : "") + ')</span></div>' +
      '<div class="dim">The panel UI answers' + (wd.ui_pid ? ' (beat ' + wd.ui_beat_age + ' s ago)' : " - not running") + '; hung UIs killed: ' + wd.ui_kills +
      '; supervisor restarts: ' + wd.stack_restarts + (wd.gave_up ? "; gave up" : "") + '</div>';
  }else{
    h += '<div class="bad">The watchdog service is not running.</div>';
  }
  h += '<div class="dim">Boots in a row that never got healthy: ' + H.stack.bootcount + ' (three switch the stack off, the vendor UI comes back).</div>';
  const L = S && S.childlock;
  if(L){
    h += '<div class="row" style="margin-top:8px;justify-content:space-between"><div><b>Child lock</b><div class="dim">' +
      (L.locked ? "The buttons, the knob and the remote are off. Hold OK for 3 seconds on the unit to unlock." : "Locks the buttons, the knob and the remote; this page keeps working.") + '</div></div>' +
      '<button class="' + (L.locked ? "on" : "") + '" data-c="childlock ' + (L.locked ? 0 : 1) + '">' + (L.locked ? "locked" : "lock") + '</button></div>' +
      '<div class="row"><label class="dim"><input type="checkbox" data-c="childlock volume ' + (L.volume ? 0 : 1) + '"' + (L.volume ? " checked" : "") + '> the volume still works while locked</label></div>';
  }
  return h;
}

function history(){
  let h = '<h3>Crashes and kills</h3>';
  if(!H.crashes.length) h += '<div class="dim">None recorded.</div>';
  else h += list(H.crashes.map(c => '<span class="nm" style="white-space:normal"><span class="dim" style="font-variant-numeric:tabular-nums">' + esc(c[0]) + '</span>&nbsp; ' + esc(c[2]) + '</span>'), -1);
  h += '<h3>Starts</h3>';
  if(!H.boots.length) h += '<div class="dim">None recorded.</div>';
  else h += list(H.boots.map(c => '<span class="nm" style="white-space:normal"><span class="dim" style="font-variant-numeric:tabular-nums">' + esc(c[0]) + '</span>&nbsp; ' + esc(c[2]) + '</span>'), -1);
  return h;
}

function logs(){
  let h = '<h3>Logs</h3>';
  if(!LOGS) return h + '<div class="row"><button id="hlopen">Show the logs</button><span class="dim">' + esc(logNote) + '</span></div>';
  const names = LOGS.logs.map(l => l.name).concat(LOGS.kernel ? ["kernel"] : []);
  const cur = LOGS.logs.find(l => l.name === logName);
  h += '<div class="row"><select id="hlname">' + names.map(n => '<option' + (n === logName ? " selected" : "") + '>' + n + '</option>').join("") + '</select>' +
    '<select id="hlold"><option value="0">this run</option>' + (cur ? cur.old : []).map(k => '<option value="' + k + '"' + (logOld === k ? " selected" : "") + '>' + k + (k === 1 ? " run" : " runs") + ' ago</option>').join("") + '</select>' +
    '<select id="hllines">' + [100, 300, 1000, 2000].map(n => '<option' + (n === logLines ? " selected" : "") + '>' + n + '</option>').join("") + '</select>' +
    '<button id="hlshow">Show</button></div>';
  if(logText != null) h += '<pre id="hlpre" style="white-space:pre-wrap;overflow:auto;max-height:420px;font-size:12px;background:var(--panel);border:1px solid var(--line);padding:8px;border-radius:6px">' + esc(logText) + '</pre>';
  return h;
}

function access(){
  let h = '<h3>Access</h3>';
  if(!ACC) return h + '<div class="dim">loading...</div>';
  h += '<div>' + (ACC.protected ? '<span class="ok">Protected</span>: anybody can look, changes need the token.' : '<span class="bad">Open</span>: anybody on the network can change things.') + '</div>' +
    '<div class="row"><input type="text" id="hacc" placeholder="new token, 4 to 64 letters or digits" style="flex:1;width:auto;min-width:200px" autocomplete="off">' +
    '<button id="hsetacc">' + (ACC.protected ? "Change" : "Set") + '</button>' + (ACC.protected ? '<button class="del" id="hclracc">Remove</button>' : "") + '</div>' +
    '<div class="dim">' + esc(accNote) + '</div>';
  return h;
}

function draw(){
  if(Date.now() - last > 5000) fetchHealth();
  if(!H) return '<p class="dim">loading...</p>';
  return cards() + guard() + history() + logs() + access();
}

registerTab({
  id: "health", label: "Health", screen: -1,
  show: () => { last = 0; accNote = ""; },
  render: draw,
  busy: b => { const a = document.activeElement; return !!(a && (a.tagName === "INPUT" || a.tagName === "SELECT") && b.contains(a)); },
  wire: b => {
    b.querySelectorAll("[data-c]").forEach(x => x.onclick = e => { e.stopPropagation(); cmd(x.dataset.c); });
    const q = id => document.getElementById(id);
    if(q("hrefresh")) q("hrefresh").onclick = () => { last = 0; fetchHealth(); };
    if(q("hlopen")) q("hlopen").onclick = () => { fetchLogs(); };
    if(q("hlname")) q("hlname").onchange = e => { logName = e.target.value; logOld = 0; };
    if(q("hlold")) q("hlold").onchange = e => { logOld = +e.target.value; };
    if(q("hllines")) q("hllines").onchange = e => { logLines = +e.target.value; };
    if(q("hlshow")) q("hlshow").onclick = () => { loadLog(); };
    if(q("hsetacc")) q("hsetacc").onclick = () => { const v = q("hacc").value.trim(); if(v) setToken(v); else accNote = "Type the new token first."; sigs.body = null; if(window.render) window.render(); };
    if(q("hclracc")) q("hclracc").onclick = () => { if(confirm("Remove the token? Everybody on the network can then change things.")) setToken(""); };
  }
});
})();
