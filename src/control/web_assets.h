/*
 * The control page, compiled in. One file, no external requests — the same
 * promise WebLinked's control page makes: everything the browser needs comes
 * from this binary.
 *
 * The raw-string delimiter is OXBOW_PAGE; the page must not contain it.
 */
#pragma once

namespace oxbow::assets {

inline constexpr const char* kControlPage = R"OXBOW_PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>oxbow</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 1.5rem; background: #10151a; color: #cfd8e3;
    font: 14px/1.5 -apple-system, "Segoe UI", system-ui, sans-serif;
  }
  header { display: flex; align-items: baseline; gap: 1rem; flex-wrap: wrap; }
  h1 { font-size: 1.3rem; margin: 0; color: #e8eef5; letter-spacing: .04em; }
  h1::after { content: " ~"; color: #4d9fb8; }
  #status { color: #7d8b9b; font-size: .85rem; }
  .card {
    background: #171e26; border: 1px solid #232d38; border-radius: 8px;
    padding: 1rem 1.25rem; margin-top: 1rem; max-width: 44rem;
  }
  .card h2 { margin: 0 0 .75rem; font-size: 1rem; color: #e8eef5; }
  .param { display: grid; grid-template-columns: 11rem 1fr 3.5rem; gap: .75rem;
           align-items: center; margin: .3rem 0; }
  .param label { color: #9fb0c0; overflow: hidden; text-overflow: ellipsis;
                 white-space: nowrap; }
  .param output { text-align: right; font-variant-numeric: tabular-nums;
                  color: #7d8b9b; }
  input[type=range] { width: 100%; accent-color: #4d9fb8; }
  input[type=checkbox] { accent-color: #4d9fb8; justify-self: start; }
  .empty { color: #7d8b9b; font-style: italic; }
</style>
</head>
<body>
<header><h1>oxbow</h1><div id="status">connecting…</div></header>
<div id="chain"></div>
<script>
"use strict";
// Types from FFGL: 0 boolean, 1 event, 10 standard, 11 option, 13 integer,
// 100 text, 14 file — sliders for the continuous ones, checkbox for boolean,
// nothing for text/file/event (not settable over this API yet).
const SLIDER_TYPES = new Set([2,3,4,5,6,10,11,13,200,201,202,203]);
let dragging = null; // "effect/index" while the pointer is down.

function setParam(effect, name, value) {
  fetch(`/api/param?effect=${effect}&name=${encodeURIComponent(name)}&value=${value}`,
        { method: "POST" });
}

function render(state) {
  const status = document.getElementById("status");
  const v = state.video;
  status.textContent = v.width
    ? `${v.source || "?"} — ${v.width}×${v.height} @ ${(+v.frameRate).toFixed(3)} — ${(+v.fps).toFixed(1)} fps — ${v.frames} frames`
    : `waiting for video from ${v.source || "source"}…`;

  const chain = document.getElementById("chain");
  state.chain.forEach((effect, e) => {
    let card = document.getElementById(`fx${e}`);
    if (!card) {
      card = document.createElement("div");
      card.className = "card"; card.id = `fx${e}`;
      card.innerHTML = `<h2></h2><div class="params"></div>`;
      chain.appendChild(card);
    }
    card.querySelector("h2").textContent = `${e + 1}. ${effect.name}`;
    const holder = card.querySelector(".params");
    effect.params.forEach(p => {
      const key = `${e}/${p.index}`;
      let row = holder.querySelector(`[data-key="${key}"]`);
      if (!row) {
        if (p.type === 0) {
          row = document.createElement("div");
          row.className = "param"; row.dataset.key = key;
          row.innerHTML = `<label></label><input type="checkbox"><output></output>`;
          const box = row.querySelector("input");
          box.addEventListener("change",
              () => setParam(e, p.name, box.checked ? 1 : 0));
        } else if (SLIDER_TYPES.has(p.type)) {
          row = document.createElement("div");
          row.className = "param"; row.dataset.key = key;
          row.innerHTML = `<label></label><input type="range" min="0" max="1" step="0.001"><output></output>`;
          const slider = row.querySelector("input");
          slider.addEventListener("pointerdown", () => dragging = key);
          slider.addEventListener("pointerup", () => dragging = null);
          slider.addEventListener("input", () => {
            setParam(e, p.name, slider.value);
            row.querySelector("output").textContent = (+slider.value).toFixed(2);
          });
        } else {
          return; // text/file/event: nothing useful to render yet
        }
        holder.appendChild(row);
      }
      row.querySelector("label").textContent = p.name;
      if (dragging !== key) {
        const input = row.querySelector("input");
        if (p.type === 0) input.checked = p.value >= 0.5;
        else input.value = p.value;
        row.querySelector("output").textContent = (+p.value).toFixed(2);
      }
    });
  });
  if (!state.chain.length && !chain.firstChild) {
    chain.innerHTML = `<div class="card empty">passthrough — no effects in the chain</div>`;
  }
}

async function poll() {
  try {
    const state = await (await fetch("/api/state")).json();
    render(state);
  } catch (e) {
    document.getElementById("status").textContent = "connection lost";
  }
  setTimeout(poll, 1000);
}
poll();
</script>
</body>
</html>
)OXBOW_PAGE";

}  // namespace oxbow::assets
