/* Disc tab: the tags of the audio CD in the drive (GET/POST /api/cd/meta, GET /api/cd/riplog). What is typed here is kept
 * on the unit under the disc's number and used for the panel's track list and for every rip of that disc; nothing is
 * looked up on the internet. The text the disc itself carries (CD-Text) shows as the grey hint in each field. */
(function(){
"use strict";
let META = null, seen = "", loading = false, note = "", html = "", logText = null, retryAt = 0;

const val = (set, k, n) => (set && set[k] && set[k][n]) || "";

function fields(m){
  const t = m.tracks.filter(x => x.audio);
  const f = (k, n, label, w) => '<input type="text" data-k="' + k + '" data-n="' + n + '" placeholder="' + esc(val(m.cdtext, k, n) || label) +
    '" value="' + esc(val(m.user, k, n)) + '" style="flex:' + w + ';min-width:120px;width:auto" aria-label="' + esc(label) + '">';
  let h = '<div class="row">' + f("T", 0, "album", 2) + f("P", 0, "album artist", 2) + f("G", 0, "genre", 1) + '</div>';
  h += t.map(x => '<div class="row" style="gap:6px"><span class="dim" style="width:26px;text-align:right">' + x.n + '</span>' +
    f("T", x.n, "title", 3) + f("P", x.n, "artist (if different)", 2) + f("C", x.n, "composer", 2) + '</div>').join("");
  return h;
}

function build(){
  if(!META) return '<p class="dim">loading...</p>';
  if(!META.id) return '<p class="dim">No audio disc in the drive.</p>';
  const nA = META.tracks.filter(x => x.audio).length;
  if(!nA) return '<p class="dim">This disc has no audio tracks.</p>';
  const has = Object.keys(META.user || {}).length, hasText = Object.keys(META.cdtext || {}).length;
  return '<div class="dim">Disc ' + esc(META.id) + ', ' + nA + ' tracks. ' +
    (hasText ? "The disc carries its own text (grey)." : "The disc carries no text.") +
    ' What you type is kept on the unit and used for the track list and for every rip of this disc. Nothing is looked up on the internet.</div>' +
    fields(META) +
    '<div class="row"><button id="dtsave" class="on" data-id="' + esc(META.id) + '">Save</button><button id="dtfill">Copy the disc text into the fields</button>' +
    (has ? '<button id="dtclr" class="del" data-id="' + esc(META.id) + '">Remove my tags</button>' : "") + '<span class="dim" id="dtnote">' + esc(note) + '</span></div>';
}

async function fetchMeta(){
  if(loading) return;
  loading = true;
  try{
    const r = await fetch("/api/cd/meta", { cache: "no-store" });
    if(r.status === 200 || r.status === 404){ META = await r.json(); html = build(); sigs.body = null; if(window.render) window.render(); }
  }catch(e){
    retryAt = Date.now() + 5000;   // draw() asks again after a pause
    seen = "";
    if(!META){ html = '<p class="dim">Cannot read the disc, trying again...</p>'; sigs.body = null; }
  }
  loading = false;
}

// the fields on the screen belong to the disc whose Save button is shown, not to whatever META says by now
function shownId(){ const s = document.getElementById("dtsave"); return (s && s.dataset.id) || (META && META.id) || ""; }

function lines(){
  const out = ["id " + shownId()];
  document.querySelectorAll("#body input[data-k]").forEach(i => { const v = i.value.trim(); if(v) out.push(i.dataset.k + " " + i.dataset.n + " " + v); });
  return out.join("\n") + "\n";
}

async function post(text){
  const h = {}; if(token()) h["Authorization"] = "Bearer " + token();
  try{
    const r = await fetch("/api/cd/meta", { method: "POST", body: text, headers: h });
    if(r.status === 401){ const t = prompt("This unit needs an access token:"); if(t){ try{ localStorage.setItem(TOKEN_KEY, t); }catch(e){} return post(text); } return; }
    const j = await r.json().catch(() => ({}));
    note = r.ok ? "Saved. The panel shows the names the next time the disc is read." : (j.error || "Not saved");
  }catch(e){ note = "Not saved: " + e; }
  fetchMeta();
}

function ripBox(){
  const R = S && S.rec;
  let h = "";
  if(R && R.cd && R.active)
    h += '<p>Ripping track ' + R.track + ' of ' + R.tracks + (R.bad ? ' - <span class="bad">' + R.bad + ' unreadable sectors</span>' : "") + '</p>';
  else if(R && R.msg)
    h += '<p class="bad">' + esc(R.msg) + '</p>';
  if(R && R.dir) h += '<div class="dim">Last rip: ' + esc(R.dir) + '</div>';
  h += '<div class="row"><button id="dtlog">Show the rip log</button></div>';
  if(logText != null) h += '<pre style="white-space:pre-wrap;overflow-x:auto;font-size:12px">' + esc(logText) + '</pre>';
  return h;
}

function draw(){
  const sig = (S && S.input_name) + ":" + (S && S.cd ? S.cd.tracks.length + ":" + S.cd.album : "");
  if(sig !== seen && Date.now() >= retryAt){ seen = sig; fetchMeta(); }
  return (html || build()) + ripBox();
}

registerTab({
  id: "disctags", label: "Disc tags", screen: -1,
  shown: () => !!(S && S.input_name === "cd"),
  show: () => { seen = ""; note = ""; logText = null; },
  render: draw,
  busy: b => { const a = document.activeElement; return !!(a && a.tagName === "INPUT" && b.contains(a)); },
  wire: b => {
    const s = document.getElementById("dtsave");
    if(s) s.onclick = () => { note = "saving..."; post(lines()); };
    const c = document.getElementById("dtclr");
    if(c) c.onclick = () => { if(confirm("Remove the tags you entered for this disc?")) post("id " + (c.dataset.id || META.id) + "\n"); };
    const f = document.getElementById("dtfill");
    if(f) f.onclick = () => b.querySelectorAll("input[data-k]").forEach(i => { if(!i.value && i.placeholder && val(META.cdtext, i.dataset.k, +i.dataset.n)) i.value = i.placeholder; });
    const l = document.getElementById("dtlog");
    if(l) l.onclick = async () => {
      try{ const r = await fetch("/api/cd/riplog", { cache: "no-store" }); logText = r.ok ? await r.text() : "No rip log yet."; }catch(e){ logText = "Cannot read the log."; }
      sigs.body = null; if(window.render) window.render();
    };
  }
});
})();
