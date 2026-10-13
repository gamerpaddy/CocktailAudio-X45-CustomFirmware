/* Look tab: the skin (layout), the colours (accent, background, text) and the visualizers of the panel. Reads S.look
 * (the panel's state, see src/x45ui/main.c) and sends `skin NAME`, `theme N`, `ground N`, `ink N`, `vis NAME`, `specstyle N`, `bars N`.
 * A new skin, colour or visual in the panel's source shows up here by itself. */
(function(){
"use strict";

function skinBlock(L){
  return '<h3>Skin</h3>' + '<div class="skins">' + L.skins.map(s =>
    '<button class="skin ' + (s.name === L.skin ? "on" : "") + '" data-c="skin ' + esc(s.name) + '">' +
    '<b>' + esc(s.label) + '</b><span class="dim">' + esc(s.about || "") + '</span></button>').join("") + '</div>';
}

/* a row of colour chips: `list` is S.look.themes/grounds/inks, `cur` the chosen index, `verb` the command */
function swatches(title, list, cur, verb){
  return '<h3 style="margin-top:16px">' + title + '</h3><div class="row">' + list.map((t, i) =>
    '<button class="sw ' + (i === cur ? "on" : "") + '" data-c="' + verb + ' ' + i + '" title="' + esc(t.name) + '">' +
    (t.color ? '<i style="background:' + esc(t.color) + '"></i>' : '<i class="none"></i>') + esc(t.name) + '</button>').join("") + '</div>';
}

/* S.look.looks says which look options the visualizers that are on make use of (VIS_LOOK_* in src/x45ui/vis/vis.h):
 * the options of one that is off are left out, they would change nothing. An older panel sends no `looks`: all show. */
const LOOK_COLOURS = 2, LOOK_FIELD = 4, LOOK_STYLE = 8;
function uses(L, bit){ return L.looks === undefined || (L.looks & bit) !== 0; }

function visBlock(L){
  if(!L.vis) return "";
  const colours = uses(L, LOOK_COLOURS), field = uses(L, LOOK_FIELD), style = uses(L, LOOK_STYLE) && L.styles;
  return '<h3 style="margin-top:16px">Visualizers <span class="dim">(' + L.max_vis + ' at a time; a third turns the oldest off)</span></h3>' +
    '<div class="row">' + L.vis.map(v =>
      '<button class="sw ' + (v.on ? "on" : "") + '" data-c="vis ' + esc(v.name) + '">' + esc(v.label) + '</button>').join("") + '</div>' +
    (style ? '<h3 style="margin-top:16px">Spectrum style</h3><div class="row">' + L.styles.map((n, i) =>
      '<button class="sw ' + (i === L.style ? "on" : "") + '" data-c="specstyle ' + i + '">' + esc(n) + '</button>').join("") + '</div>' : "") +
    (colours ? '<h3 style="margin-top:16px">Bar colours</h3><div class="row">' + L.bar_names.map((n, i) =>
      '<button class="sw ' + (i === L.bars ? "on" : "") + '" data-c="bars ' + i + '">' + esc(n) + '</button>').join("") + '</div>' : "") +
    (field ? gonBlock(L) : "") +
    (colours || field || style ? "" : '<p class="dim" style="margin-top:8px">The spectrum style, the colours and the stereo field\'s options show here while a visualizer that uses them is on.</p>');
}

/* the stereo field: its look, the colour of its trace, and its bars (correlation, balance) */
function gonBlock(L){
  const G = L.gon;
  if(!G) return "";
  return '<h3 style="margin-top:16px">Stereo field</h3><div class="row">' + G.looks.map((n, i) =>
      '<button class="sw ' + (i === G.look ? "on" : "") + '" data-c="gonlook ' + i + '">' + esc(n) + '</button>').join("") + '</div>' +
    '<div class="row" style="margin-top:8px">' + G.colours.map((n, i) =>
      '<button class="sw ' + (i === G.colour ? "on" : "") + '" data-c="goncol ' + i + '">' + esc(n) + '</button>').join("") + '</div>' +
    '<div class="row" style="margin-top:8px"><button class="sw ' + (G.bars ? "on" : "") + '" data-c="gonbars ' + (G.bars ? 0 : 1) +
    '">bars: correlation and balance</button>' +
    '<button class="sw ' + (G.big ? "on" : "") + '" data-c="gonbig ' + (G.big ? 0 : 1) +
    '" title="the scope almost fullscreen, the text a small column beside it (leaves the bars out)">almost fullscreen</button></div>';
}

function draw(){
  const L = S && S.look;   /* S is the page's state (a top-level let of index.html) */
  if(!L) return '<p class="dim">loading...</p>';
  return '<style>.skins{display:grid;gap:8px}.skin{display:flex;flex-direction:column;align-items:flex-start;gap:2px;text-align:left}' +
    '.sw{display:flex;align-items:center;gap:6px}.sw i{width:14px;height:14px;border-radius:50%;display:inline-block;border:1px solid #8886}' +
    '.sw i.none{background:linear-gradient(135deg,transparent 45%,#8888 45%,#8888 55%,transparent 55%)}</style>' +
    skinBlock(L) +
    swatches("Accent colour", L.themes, L.theme, "theme") +
    swatches("Background", L.grounds, L.ground, "ground") +
    swatches("Text colour", L.inks, L.ink, "ink") +
    visBlock(L) +
    '<p class="dim" style="margin-top:14px">The skin sets how things are laid out; the colours are independent of it ("skin" keeps the skin\'s own). ' +
    'A light background gets dark text by itself. Everything here is kept across restarts.</p>';
}

registerTab({
  id: "look", label: "Look", screen: -1,
  render: draw,
  wire: b => b.querySelectorAll("[data-c]").forEach(x => x.onclick = () => cmd(x.dataset.c))
});
})();
