/* Bluetooth output tab: play what the unit plays on a Bluetooth headset or speaker. Reads S.btout (the panel's state,
 * see src/x45ui/ext/btout.c) and sends `btout on|off|scan|stopscan|connect ADDR|disconnect|forget ADDR`. */
(function(){
"use strict";
let armed = "";   /* the device whose "forget" was pressed once */
let armT = 0;

const bars = r => { const n = r >= -55 ? 4 : r >= -67 ? 3 : r >= -77 ? 2 : 1; let b = ""; for (let i = 0; i < 4; i++) b += '<rect x="' + (i * 4) + '" y="' + (12 - 3 * (i + 1)) + '" width="3" height="' + (3 * (i + 1)) + '" class="' + (i < n ? "acc" : "dim") + '"/>'; return '<svg viewBox="0 0 16 12" width="16" height="12" style="vertical-align:-1px">' + b + '</svg>'; };

function head(B){
  const nm = esc(B.name || B.target || "");
  let h, sub = "";
  if(!B.active){
    h = '<span class="big dim">Bluetooth output is off</span>';
    sub = esc(B.error || "switch it on, then search for a device");
  } else if(B.state === "starting" || !B.state){
    h = '<span class="big">Starting Bluetooth...</span>';
  } else if(B.state === "connecting"){
    h = '<span class="big">Connecting to ' + nm + '...</span>'; sub = "a new device has to be in pairing mode";
  } else if(B.connected){
    h = '<span class="big acc">Playing on ' + nm + '</span>'; sub = (B.streaming ? "sending sound" : "connected, waiting for sound") + (B.battery >= 0 ? " - headset battery " + B.battery + "%" : "");
  } else if(B.scanning){
    h = '<span class="big">Searching...</span>'; sub = "put the device in pairing mode now if it is new";
  } else {
    h = '<span class="big">Not connected</span>';
    sub = esc(B.error || (B.devices.length ? "choose a device" : "search for a device; a new one must be in pairing mode"));
  }
  return '<div class="row" style="justify-content:space-between"><div>' + h + '<div class="dim">' + sub + '</div></div>' +
    '<button data-c="btout ' + (B.active ? "off" : "on") + '" class="' + (B.active ? "on" : "") + '">' + (B.active ? "Bluetooth on" : "Bluetooth off") + '</button></div>';
}

function devices(B){
  if(!B.active) return "";
  const rows = B.devices.map(d => {
    const nm = esc(d.name || d.addr);
    let r = '<span class="nm">' + nm + (d.connected ? ' <span class="acc">&#9679; connected</span>' : d.known ? ' <span class="dim">paired</span>' : "") + '</span>' +
      '<span class="acts">' + (d.heard && d.rssi ? bars(d.rssi) + " " : "");
    if(d.connected) r += '<button class="sm" data-c="btout disconnect">disconnect</button>';
    else r += '<button class="sm" data-c="btout connect ' + esc(d.addr) + '">' + (d.known ? "connect" : "pair and connect") + '</button>';
    if(d.known) r += '<button class="sm del" data-f="' + esc(d.addr) + '">' + (armed === d.addr ? "press again" : "forget") + '</button>';
    return r + '</span>';
  });
  return '<h3 style="margin-top:14px">Devices <button class="sm" style="float:right" data-c="btout ' + (B.scanning ? "stopscan" : "scan") + '">' +
    (B.scanning ? "stop searching" : "search") + '</button></h3>' +
    (rows.length ? list(rows, -1) : '<p class="dim">' + (B.scanning ? "searching..." : "no device yet") + '</p>') +
    '<p class="dim" style="margin-top:10px">To add a new headset or speaker, put it in pairing mode first, then press "pair and connect". ' +
    'A device that was paired before only needs to be switched on; the unit calls it by itself.</p>';
}

function draw(){
  const B = S && S.btout;
  if(!B) return '<p class="dim">loading...</p>';
  return '<style>.nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis}.acts{display:flex;gap:6px;align-items:center}</style>' +
    head(B) + devices(B) +
    '<p class="dim" style="margin-top:14px">While a device is connected, what the unit plays from its library, the network and the radio list goes to it instead of the speakers; ' +
    'the volume knob sets the level. The tuner, CD and the line inputs keep their own outputs. The Bluetooth input and this output share one radio stick: choosing the input switches the output off until the input is left.</p>';
}

registerTab({
  id: "btout", label: "BT out", screen: -1,
  render: draw,
  wire: b => {
    b.querySelectorAll("[data-c]").forEach(x => x.onclick = () => cmd(x.dataset.c));
    b.querySelectorAll("[data-f]").forEach(x => x.onclick = () => {
      const a = x.dataset.f;
      if(armed === a && Date.now() - armT < 4000){ armed = ""; cmd("btout forget " + a); }
      else { armed = a; armT = Date.now(); sigs.body = null; if(window.render) window.render(); setTimeout(() => { if(armed === a){ armed = ""; sigs.body = null; if(window.render) window.render(); } }, 4000); }
    });
  }
});
})();
