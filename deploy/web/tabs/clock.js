/* Clock tab: the unit's time, the sleep timer and the clock settings. Reads S.clock (src/x45ui/ext/clock.c) and sends
 * `clock sleep MIN|off`, `clock action standby|off`, `clock bar|h24|secs 0|1`, `clock idle SECONDS`, `clock open|dark`,
 * `clock set EPOCH` (only while the unit has no time at all). */
(function(){
"use strict";
const pad = n => String(n).padStart(2, "0");
const DAYS = ["Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"];
const MONTHS = ["January","February","March","April","May","June","July","August","September","October","November","December"];

function clockText(C){
  /* the unit's local time = its epoch plus the zone offset, read as UTC */
  const d = new Date((C.epoch + C.offset) * 1000);
  const h = d.getUTCHours(), m = d.getUTCMinutes(), s = d.getUTCSeconds();
  let t;
  if(C.h24) t = pad(h) + ":" + pad(m);
  else t = ((h % 12) || 12) + ":" + pad(m);
  if(C.secs) t += ":" + pad(s);
  if(!C.h24) t += " " + (h < 12 ? "AM" : "PM");
  return { t: t, date: DAYS[d.getUTCDay()] + ", " + d.getUTCDate() + " " + MONTHS[d.getUTCMonth()] + " " + d.getUTCFullYear() };
}

const minText = m => m === 0 ? "Off" : m >= 60 && m % 60 === 0 ? (m / 60) + " h" : m > 60 ? Math.floor(m / 60) + " h " + (m % 60) + " min" : m + " min";
const idleText = s => s === 0 ? "Never" : s < 60 ? s + " s" : (s / 60) + " min";

function left(sec){
  if(sec >= 90) return Math.round(sec / 60) + " min";
  return sec + " s";
}

function draw(){
  const C = S && S.clock;
  if(!C) return '<p class="dim">loading...</p>';
  let h = "";
  if(C.valid){
    const c = clockText(C);
    h += '<div style="text-align:center;margin:6px 0 14px"><div style="font-size:56px;font-variant-numeric:tabular-nums;line-height:1.1">' + esc(c.t) + '</div>' +
      '<div class="dim">' + esc(c.date) + (C.zone ? " - " + esc(C.zone) : "") + '</div></div>';
  } else {
    h += '<div style="text-align:center;margin:6px 0 14px"><div class="big bad">The unit has no time</div>' +
      '<div class="dim">It sets its clock from the network when it is online. Until then you can give it this device\'s time.</div>' +
      '<div class="row" style="justify-content:center"><button data-c="clock set ' + Math.floor(Date.now() / 1000) + '">Set the time from this device</button></div></div>';
  }
  const Z = C.sleep;
  h += '<h3>Sleep timer</h3>';
  if(Z.running) h += '<div class="big ' + (Z.fading ? "bad" : "acc") + '">' + (Z.fading ? "Fading out - " : "Ends in ") + left(Z.left) + '</div>';
  else if(Z.asleep) h += '<div class="big acc">Asleep - any key on the unit wakes it up</div>';
  else h += '<div class="dim" style="margin-bottom:6px">No timer is running.</div>';
  h += '<div class="row">' + C.presets.map(m => '<button class="' + ((Z.running ? Z.minutes === m : m === 0) ? "on" : "") + '" data-c="clock sleep ' + (m || "off") + '">' + minText(m) + '</button>').join("") + '</div>';
  h += '<div class="row"><label>When it ends</label>' +
    '<button class="' + (C.action === "standby" ? "on" : "") + '" data-c="clock action standby">Stop and sleep</button>' +
    '<button class="' + (C.action === "off" ? "on" : "") + '" data-c="clock action off">Switch the unit off</button></div>' +
    '<p class="dim">The sound fades out over the last 30 seconds (not with a fixed volume). A key press on the unit during the fade ends the timer. ' +
    '"Stop and sleep" stops what plays, mutes the output and puts the screen out; the first key press wakes the unit. A recording that is running is never cut off by the timer.</p>';
  h += '<h3 style="margin-top:14px">Clock</h3>';
  h += '<div class="row"><label>Time in the top bar</label><button class="' + (C.bar ? "on" : "") + '" data-c="clock bar ' + (C.bar ? 0 : 1) + '">' + (C.bar ? "on" : "off") + '</button></div>';
  h += '<div class="row"><label>Format</label><button class="' + (C.h24 ? "on" : "") + '" data-c="clock h24 1">24 hour</button><button class="' + (C.h24 ? "" : "on") + '" data-c="clock h24 0">12 hour</button></div>';
  h += '<div class="row"><label>Seconds</label><button class="' + (C.secs ? "on" : "") + '" data-c="clock secs ' + (C.secs ? 0 : 1) + '">' + (C.secs ? "on" : "off") + '</button></div>';
  h += '<div class="row"><label>Idle clock</label>' + C.idles.map(s => '<button class="' + (C.idle === s ? "on" : "") + '" data-c="clock idle ' + s + '">' + idleText(s) + '</button>').join("") + '</div>';
  h += '<p class="dim">The idle clock comes up on the unit after that long without a key. The CLOCK key on the remote shows the clock; pressed there, it puts the screen out.</p>';
  h += '<div class="row"><button data-c="clock open">Show the clock on the unit</button><button data-c="clock dark">Screen off</button></div>';
  return h;
}

registerTab({
  id: "clock", label: "Clock", screen: -1,
  render: draw,
  wire: b => b.querySelectorAll("[data-c]").forEach(x => x.onclick = () => cmd(x.dataset.c))
});
})();
