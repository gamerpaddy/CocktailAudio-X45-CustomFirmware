/* Signal path tab: read only. Shows S.path (src/x45ui/ext/path.c): the stages between the source and the output
 * as they are right now, and whether the path is bit-perfect. */
(function(){
"use strict";

function draw(){
  const P = S && S.path;
  if(!P) return '<p class="dim">loading...</p>';
  let h = '<div style="position:relative;padding-left:26px;margin-top:4px">' +
    '<div style="position:absolute;left:7px;top:10px;bottom:22px;width:2px;background:var(--line)"></div>';
  P.stages.forEach(s => {
    h += '<div style="position:relative;margin:0 0 14px">' +
      '<span style="position:absolute;left:-24px;top:5px;width:12px;height:12px;border-radius:3px;background:var(--accent)"></span>' +
      '<div class="dim" style="font-size:12px;letter-spacing:.1em">' + esc(s.name) + '</div>' +
      '<div class="big">' + esc(s.a || "-") + '</div>' +
      (s.b ? '<div class="dim">' + esc(s.b) + '</div>' : '') + '</div>';
  });
  h += '</div><div style="border-top:1px solid var(--line);padding-top:10px">' +
    '<div class="big ' + (P.perfect === 1 ? "acc" : P.perfect === 0 ? "bad" : "") + '">' + esc(P.verdict) + '</div>' +
    P.notes.map(n => '<div class="dim">' + esc(n) + '</div>').join("") + '</div>' +
    '<p class="dim" style="margin-top:12px">This page only shows; it changes nothing. The same picture is on the unit\'s screen (Options, Signal path; the blue key on the remote).</p>';
  return h;
}

registerTab({ id: "path", label: "Path", screen: -1, render: draw });
})();
