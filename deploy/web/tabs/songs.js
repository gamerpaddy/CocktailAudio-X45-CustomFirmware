/* Songs tab: the log of what the radio played (src/x45ui/ext/songlog.c, GET /api/songlog). Sends `songlog mark|unmark ID`,
 * `songlog log 0|1`, `songlog now` and `songlog clear`. The list can be saved as a CSV file from this page. */
(function(){
"use strict";
let LOG = null, seen = "", loading = false, filter = "", onlyMarked = false, armed = 0;
const pad = n => String(n).padStart(2, "0");

function when(t, zoneOffset){
  if(!t) return "-";
  const d = new Date((t + zoneOffset) * 1000);
  return pad(d.getUTCDate()) + "." + pad(d.getUTCMonth() + 1) + ". " + pad(d.getUTCHours()) + ":" + pad(d.getUTCMinutes());
}

async function fetchLog(){
  if(loading) return;
  loading = true;
  try{
    const r = await fetch("/api/songlog", { cache: "no-store" });
    if(r.ok){ LOG = await r.json(); sigs.body = null; if(window.render) window.render(); }
  }catch(e){}
  loading = false;
}

function offset(){ return (S && S.clock && S.clock.offset) || 0; }

function shown(){
  if(!LOG) return [];
  const f = filter.toLowerCase();
  return LOG.entries.filter(e => (!onlyMarked || e.mark) && (!f || (e.text + " " + e.station).toLowerCase().includes(f)));
}

function csv(){
  // a title that starts like a formula stays text in a spreadsheet
  const q = s => { s = String(s); if(/^[=+\-@\t\r]/.test(s)) s = "'" + s; return '"' + s.replace(/"/g, '""') + '"'; };
  const o = offset();
  const rows = ["time,station,source,title,marked"].concat(LOG.entries.slice().reverse().map(e => {
    const d = e.t ? new Date((e.t + o) * 1000).toISOString().slice(0, 16).replace("T", " ") : "";
    return [q(d), q(e.station), q(e.src), q(e.text), e.mark ? 1 : 0].join(",");
  }));
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob(["\ufeff" + rows.join("\r\n") + "\r\n"], { type: "text/csv" }));
  a.download = "x45-song-log.csv";
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 4000);
}

function draw(){
  const L = S && S.songlog;
  if(!L) return '<p class="dim">loading...</p>';
  const sig = L.count + ":" + L.last;
  if(sig !== seen){ seen = sig; fetchLog(); }
  const rows = shown();
  const o = offset();
  let h = '<div class="row" style="justify-content:space-between"><div><span class="big ' + (L.on ? "acc" : "dim") + '">' + (L.on ? "Logging what the radio plays" : "Song log is off") + '</span>' +
    '<div class="dim">' + L.count + ' songs kept, ' + L.marked + ' marked. Nothing leaves the unit.</div></div>' +
    '<button class="' + (L.on ? "on" : "") + '" data-c="songlog log ' + (L.on ? 0 : 1) + '">' + (L.on ? "on" : "off") + '</button></div>';
  h += '<div class="row"><input type="text" id="slf" placeholder="search" value="' + esc(filter) + '" style="flex:1;width:auto">' +
    '<button class="' + (onlyMarked ? "on" : "") + '" id="slm">marked only</button>' +
    '<button id="slnow" data-c="songlog now">&#9733; mark what plays</button></div>';
  if(!LOG) h += '<p class="dim">loading...</p>';
  else if(!rows.length) h += '<p class="dim">' + (LOG.entries.length ? "No song matches." : "Nothing yet. Play an internet radio station, a DAB+ or an FM station that sends song titles; each new title is kept here with the time and the station.") + '</p>';
  else h += list(rows.slice(0, 300).map(e =>
    '<span class="nm"><span class="dim" style="font-variant-numeric:tabular-nums">' + when(e.t, o) + '</span>&nbsp; ' + esc(e.text) +
    '<div class="dim" style="font-size:12px">' + esc(e.station) + (e.src ? " - " + esc(e.src) : "") + '</div></span>' +
    '<span class="acts"><button class="sm ' + (e.mark ? "on" : "") + '" data-c="songlog ' + (e.mark ? "unmark " : "mark ") + e.id + '">' + (e.mark ? "&#9733;" : "&#9734;") + '</button></span>'), -1);
  h += '<div class="row" style="margin-top:10px"><button id="slcsv">Save as CSV</button><button class="del" id="slclr">' + (armed ? "press again to clear" : "Clear the log") + '</button></div>';
  return h;
}

registerTab({
  id: "songs", label: "Songs", screen: -1,
  show: () => { seen = ""; },
  render: draw,
  busy: b => { const f = document.getElementById("slf"); return !!(f && document.activeElement === f && f.value !== filter); },
  wire: b => {
    b.querySelectorAll("[data-c]").forEach(x => x.onclick = e => { e.stopPropagation(); cmd(x.dataset.c); setTimeout(fetchLog, 500); });
    const f = document.getElementById("slf");
    if(f) f.oninput = () => { filter = f.value; sigs.body = null; if(window.render) window.render(); };
    const m = document.getElementById("slm");
    if(m) m.onclick = () => { onlyMarked = !onlyMarked; sigs.body = null; if(window.render) window.render(); };
    const c = document.getElementById("slcsv");
    if(c) c.onclick = () => { if(LOG) csv(); };
    const x = document.getElementById("slclr");
    if(x) x.onclick = () => {
      if(armed && Date.now() - armed < 4000){ armed = 0; cmd("songlog clear"); setTimeout(fetchLog, 500); }
      else { armed = Date.now(); sigs.body = null; if(window.render) window.render(); setTimeout(() => { armed = 0; sigs.body = null; if(window.render) window.render(); }, 4000); }
    };
  }
});
})();
