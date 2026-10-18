/* WiFi tab: join, forget and look after networks. Talks to /api/wifi (src/x45web/api_wifi.c).
 * Uses the helpers of index.html ($, esc, put, token) and registers itself with registerTab(). */
(function(){
"use strict";
let ST = null, APS = [], msg = "", pending = false, openRow = null, lastScan = 0, timer = 0;

async function api(path, body){
  const h = {}; if(token()) h["Authorization"] = "Bearer " + token();
  const o = body === undefined ? {cache:"no-store"} : {method:"POST", body:body, headers:h};
  try{
    const r = await fetch("/api/wifi" + path, o);
    let j = null; try{ j = await r.json(); }catch(e){}
    return {status:r.status, j:j};
  }catch(e){ return {status:0, j:null}; }
}
const form = o => Object.keys(o).map(k => k + "=" + o[k]).join("\n");
const band = f => f >= 4900 ? "5 GHz" : "2.4 GHz";
const bars = r => { const n = r >= -55 ? 4 : r >= -67 ? 3 : r >= -77 ? 2 : 1; let b = ""; for (let i = 0; i < 4; i++) b += '<rect x="' + (i * 4) + '" y="' + (12 - 3 * (i + 1)) + '" width="3" height="' + (3 * (i + 1)) + '" class="' + (i < n ? "acc" : "dim") + '"/>'; return '<svg viewBox="0 0 16 12" width="16" height="12" style="vertical-align:-1px">' + b + '</svg>'; };

var msgBad = false;
function showMsg(m, bad){ msg = m; msgBad = !!bad; }
/* repaint; while a field is being typed in the page leaves the markup alone, so the message line is set directly */
function redraw(){
  sigs.body = null;
  if(tab === "wifi") window.render();
  const m = $("wf_msg"); if(m){ m.textContent = msg + (pending ? " ..." : ""); m.className = msgBad ? "bad" : "dim"; }
}

async function load(){
  const r = await api("");
  if(r.j){ ST = r.j; redraw(); }
}
async function loadScan(){
  const r = await api("/scan");
  if(r.j){ APS = r.j.aps || []; redraw(); }
}
async function scan(){
  lastScan = Date.now();
  await api("/scan", "");
  setTimeout(loadScan, 4000);
}
function tick(){
  if(tab !== "wifi" || document.hidden){ return; }   /* no scans for a page nobody looks at */
  load();
  if(ST && ST.state !== "off" && ST.state !== "starting" && Date.now() - lastScan > 15000) scan();
}

const VIA = "This page is open over the unit's WiFi, so the WiFi cannot be changed from here: that would cut the page off. Use the cable address, or the panel's Network screen.";
async function act(path, o, done){
  if(ST && ST.via_wifi && path !== "/auto" && path !== "/scan"){ showMsg(VIA, true); redraw(); return; }
  pending = true; showMsg(""); redraw();
  const r = await api(path, form(o));
  pending = false;
  const ok = !!(r.j && r.j.ok);
  showMsg(ok ? (done || "") : "failed: " + ((r.j && r.j.error) || "no answer"), !ok);
  await load();
  redraw();
}

async function connect(ssid, psk, hidden){
  if(ST && ST.via_wifi){ showMsg(VIA, true); redraw(); return; }
  pending = true; showMsg("Connecting to " + ssid + "...");
  redraw();
  const r = await api("/connect", form({ssid:ssid, psk:psk, hidden:hidden ? 1 : 0}));
  pending = false;
  const res = r.j && r.j.result;
  if(res === "connected") showMsg("Connected to " + ssid);
  else if(res === "not_in_range") showMsg(ssid + " is saved, but not in range right now");
  else if(res === "pending") showMsg("Still connecting to " + ssid + " - the status above will update");
  else showMsg(ssid + ": " + ((r.j && r.j.error) || "no answer from the unit"), true);
  if(r.j && r.j.ok){          /* saved: the typed fields are done with (a refused key keeps them, to be corrected) */
    openRow = null;
    ["wf_psk", "wf_ssid", "wf_opsk"].forEach(id => { const e = $(id); if(e) e.value = ""; });
  }
  await load(); lastScan = 0; redraw();
}

function statusBlock(){
  if(!ST) return '<p class="dim">loading...</p>';
  const power = '<button data-w="power" class="' + (ST.enabled ? "on" : "") + '">' + (ST.enabled ? "WiFi on" : "WiFi off") + '</button>';
  let head, sub = "";
  switch(ST.state){
    case "connected": head = '<span class="big acc">Connected to ' + esc(ST.ssid) + '</span>';
      sub = esc(ST.ip) + " &middot; " + band(ST.freq) + " &middot; " + ST.rssi + " dBm"; break;
    case "connecting": head = '<span class="big">Connecting' + (ST.ssid ? " to " + esc(ST.ssid) : "") + '...</span>'; break;
    case "scanning": head = '<span class="big">Looking for networks...</span>'; break;
    case "starting": head = '<span class="big">Starting WiFi...</span>'; sub = esc(ST.step || ""); break;
    case "error": head = '<span class="big bad">WiFi stopped</span>'; sub = esc(ST.error || ""); break;
    case "off": head = '<span class="big dim">WiFi is off</span>'; break;
    default: head = '<span class="big">Not connected</span>'; sub = ST.networks && ST.networks.length ? "no saved network in range" : "no network saved yet";
  }
  return '<div class="row" style="justify-content:space-between"><div>' + head + '<div class="dim">' + sub + '</div></div>' + power + '</div>';
}

function savedBlock(){
  if(!ST || !ST.networks || !ST.networks.length) return "";
  return '<h3 style="margin-top:14px">Saved networks</h3>' + list(ST.networks.map((n, i) =>
    '<span class="nm">' + esc(n.ssid) + (n.current ? ' <span class="acc">&#9679; connected</span>' : "") + (n.auto ? "" : ' <span class="dim">(manual)</span>') + '</span>' +
    '<span class="acts">' +
    (n.current ? '<button class="sm" data-w="leave">disconnect</button>' : '<button class="sm" data-w="join" data-i="' + n.id + '">connect</button>') +
    '<button class="sm" data-w="auto" data-i="' + n.id + '" data-on="' + (n.auto ? 0 : 1) + '" title="join this network automatically when it is in range">' + (n.auto ? "auto on" : "auto off") + '</button>' +
    '<button class="sm del" data-w="forget" data-i="' + n.id + '" data-n="' + esc(n.ssid) + '">forget</button></span>'), -1);
}

function nearbyBlock(){
  if(!ST || ST.state === "off" || ST.state === "starting") return "";
  const rows = APS.map((a, i) => {
    const sel = openRow === a.ssid;
    let r = '<span class="nm">' + esc(a.ssid) + (a.saved >= 0 ? ' <span class="acc">saved</span>' : "") + '</span>' +
      '<span class="dim">' + esc(a.sec) + '</span> ' + bars(a.rssi);
    if(sel && a.saved < 0)
      r += '</li><li style="cursor:default;background:var(--btn)">' +
        (a.secure ? '<input type="text" id="wf_psk" placeholder="password" autocomplete="off" style="flex:1;width:auto"> ' : '<span class="dim" style="flex:1">open network</span>') +
        '<button data-w="go" data-s="' + esc(a.ssid) + '" data-h="0">connect</button>';
    return r;
  });
  return '<h3 style="margin-top:14px">Nearby <button class="sm" data-w="scan" style="float:right">scan</button></h3>' +
    (APS.length ? list(rows, -1) : '<p class="dim">' + (lastScan ? "scanning..." : "no networks heard yet") + '</p>') +
    '<h3 style="margin-top:14px">Other network</h3>' +
    '<div class="row"><input type="text" id="wf_ssid" placeholder="network name" style="flex:1;width:auto"><input type="text" id="wf_opsk" placeholder="password (empty: open)" autocomplete="off" style="flex:1;width:auto">' +
    '<label class="dim"><input type="checkbox" id="wf_hid"> hidden</label><button data-w="go-other">connect</button></div>';
}

function drawTab(){
  return (ST && ST.via_wifi ? '<p class="dim">This page is open over the unit\'s WiFi: the status is live, but joining, leaving or switching off would cut it off. To change the WiFi, open this page by the cable address or use the panel\'s Network screen.</p>' : "") +
    statusBlock() + savedBlock() + nearbyBlock() +
    '<p id="wf_msg" class="' + (msgBad ? "bad" : "dim") + '">' + esc(msg) + (pending ? " ..." : "") + '</p>' +
    '<p class="dim">WPA2 and WPA2/WPA3 networks work; WPA3-only, enterprise and WEP-only networks are not supported. The router must be in range of the unit.</p>';
}

function wire(b){
  b.querySelectorAll("[data-w]").forEach(x => x.onclick = e => {
    e.stopPropagation();
    const w = x.dataset.w;
    if(w === "power") act("/power", {on: ST && ST.enabled ? 0 : 1}, ST && ST.enabled ? "Switching WiFi off" : "Switching WiFi on - this takes up to half a minute");
    else if(w === "leave") act("/disconnect", {}, "Disconnected");
    else if(w === "join") act("/join", {id: x.dataset.i}, "Connecting...");
    else if(w === "auto") act("/auto", {id: x.dataset.i, on: x.dataset.on});
    else if(w === "forget"){ if(confirm('Forget "' + x.dataset.n + '"?')) act("/forget", {id: x.dataset.i}, "Forgotten"); }
    else if(w === "scan") scan();
    else if(w === "go") connect(x.dataset.s, ($("wf_psk") || {}).value || "", false);
    else if(w === "go-other"){
      const s = $("wf_ssid").value.trim();
      if(!s){ showMsg("Type the network name first", true); redraw(); return; }
      connect(s, $("wf_opsk").value, $("wf_hid").checked);
    }
  });
}

/* the list rows are plain <li>: the nearby list is the one that follows the "Nearby" heading */
function wireNearby(b){
  const hs = [...b.querySelectorAll("h3")].find(h => h.textContent.startsWith("Nearby"));
  const ul = hs && hs.nextElementSibling;
  if(!ul || ul.tagName !== "UL") return;
  [...ul.querySelectorAll("li[data-i]")].forEach(li => li.onclick = () => {
    const a = APS[+li.dataset.i];
    if(!a) return;
    if(a.unsupported){ showMsg(a.ssid + " uses WPA3-only or enterprise security, which this unit cannot join", true); redraw(); return; }
    if(a.saved >= 0){ act("/join", {id: a.saved}, "Connecting to " + a.ssid + "..."); return; }
    if(!a.secure){ connect(a.ssid, "", false); return; }
    openRow = openRow === a.ssid ? null : a.ssid; redraw();
    setTimeout(() => { const p = $("wf_psk"); if(p) p.focus(); }, 50);
  });
}

registerTab({
  id: "wifi", label: "WiFi", screen: -1,
  render: drawTab,
  wire: b => { wire(b); wireNearby(b); const p = $("wf_psk"); if(p) p.onkeydown = e => { if(e.key === "Enter") b.querySelector('[data-w="go"]').click(); }; },
  busy: b => !!b.querySelector("input:focus") || [...b.querySelectorAll("input[type=text]")].some(i => i.value),
  show: () => { load(); loadScan(); if(!timer) timer = setInterval(tick, 3000); }
});
})();
