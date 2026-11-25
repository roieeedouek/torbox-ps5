// TorBox for PS5 web UI. Talks only to the console; the console talks to TorBox.
"use strict";

const $ = (s) => document.querySelector(s);
const state = { tab: "torrents", lists: {}, open: new Set(), status: null, jobs: [], pollTimer: 0 };

function el(tag, props, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat()) if (c !== null && c !== undefined && c !== false) e.append(c.nodeType ? c : String(c));
  return e;
}

async function api(path, form) {
  const opts = form ? { method: "POST", headers: { "X-TorBox-PS5": "1" }, body: new URLSearchParams(form) } : {};
  let res;
  try { res = await fetch("/api/" + path, opts); } catch (e) { setConn(false); throw new Error("console not reachable"); }
  setConn(true);
  let data = null;
  try { data = await res.json(); } catch (e) { /* not JSON */ }
  if (!res.ok || !data || data.ok === false || data.success === false) {
    const msg = (data && (data.error && data.ok === false ? data.error : data.detail || data.error)) || ("HTTP " + res.status);
    const err = new Error(msg); err.status = res.status; err.data = data; throw err;
  }
  return data;
}

function setConn(ok) {
  const c = $("#conn");
  c.textContent = ok ? (state.status ? "console " + state.status.ip : "connected") : "console offline";
  c.className = "pill " + (ok ? "ok" : "bad");
}

let toastTimer = 0;
function toast(msg) {
  const t = $("#toast");
  t.textContent = msg; t.classList.remove("hidden");
  clearTimeout(toastTimer); toastTimer = setTimeout(() => t.classList.add("hidden"), 3500);
}

function size(n) {
  if (n === undefined || n === null || n < 0) return "?";
  const u = ["B", "KB", "MB", "GB", "TB"]; let i = 0; let v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return (i ? v.toFixed(v < 10 ? 2 : 1) : v) + " " + u[i];
}
function duration(s) {
  if (!isFinite(s) || s < 0) return "";
  s = Math.round(s);
  if (s < 60) return s + "s";
  if (s < 3600) return Math.floor(s / 60) + "m " + (s % 60) + "s";
  return Math.floor(s / 3600) + "h " + Math.floor((s % 3600) / 60) + "m";
}

// ---------- login ----------
async function loadStatus() {
  try {
    state.status = await api("status");
  } catch (e) {
    $("#conn").textContent = "console offline";
    setTimeout(loadStatus, 3000);
    return;
  }
  setConn(true);
  const s = state.status;
  $("#key-file").textContent = s.key_file;
  if (!$("#folder").value) $("#folder").value = localStorage.getItem("folder") || s.default_folder;
  if (!s.logged_in) {
    $("#login").classList.remove("hidden"); $("#app").classList.add("hidden");
    $("#key").focus();
    return;
  }
  $("#login").classList.add("hidden"); $("#app").classList.remove("hidden");
  const u = s.user;
  $("#account").textContent = (u ? "Logged in as " + u.email + (u.premium_expires_at ? " · plan until " + u.premium_expires_at.slice(0, 10) : "") :
    "Logged in" + (s.user_error ? " (" + s.user_error + ")" : "")) + " · " + s.mode + " " + s.version;
  showTab(state.tab);
  pollJobs();
}

$("#login-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const input = $("#key"), btn = ev.target.querySelector("button");
  $("#login-error").textContent = "";
  btn.disabled = true; btn.textContent = "Checking...";
  try {
    await api("login", { key: input.value.trim() });
    input.value = "";
    await loadStatus();
  } catch (e) {
    $("#login-error").textContent = e.message;
  } finally {
    btn.disabled = false; btn.textContent = "Log in";
  }
});

$("#logout").addEventListener("click", async () => {
  if (!confirm("Log out and delete the stored API key from the console?")) return;
  await api("logout", {}).catch((e) => toast(e.message));
  state.lists = {};
  loadStatus();
});

$("#quit").addEventListener("click", async () => {
  const active = state.jobs.filter((j) => ["queued", "starting", "downloading"].includes(j.state)).length;
  if (!confirm("Stop the server on the console?" + (active ? " " + active + " download(s) stop too and resume next time." : "") +
      "\nYou need to start the app again to use this page.")) return;
  await api("quit", {}).catch(() => {});
  toast("Server stopped");
  setConn(false);
});

// ---------- tabs and lists ----------
document.querySelectorAll(".tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));

function showTab(tab) {
  state.tab = tab;
  document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("active", b.dataset.tab === tab));
  const dl = tab === "downloads";
  $("#list-view").classList.toggle("hidden", dl);
  $("#downloads-view").classList.toggle("hidden", !dl);
  if (dl) renderJobs();
  else if (state.lists[tab]) renderList();
  else loadList(false);
}

async function loadList(fresh) {
  const tab = state.tab;
  $("#list-status").textContent = "Loading...";
  $("#items").replaceChildren();
  try {
    const r = await api("list?kind=" + tab + (fresh ? "&fresh=1" : ""));
    let data = r.data || [];
    if (!Array.isArray(data)) data = [data];
    data.sort((a, b) => String(b.created_at || "").localeCompare(String(a.created_at || "")));
    state.lists[tab] = data;
  } catch (e) {
    if (e.status === 401 && e.data && e.data.error === "not logged in") return loadStatus();
    state.lists[tab] = null;
    $("#list-status").textContent = "Could not load the list: " + e.message;
    return;
  }
  if (tab === state.tab) renderList();
}

$("#refresh").addEventListener("click", () => loadList(true));
$("#filter").addEventListener("input", () => renderList());

function itemState(it) {
  const s = String(it.download_state || "");
  if (it.download_present || it.download_finished) return { text: s && !/^(completed|cached|uploading|seeding)/i.test(s) ? s : "ready", cls: "ready", ready: true };
  if (/error|fail|stalled|dead/i.test(s)) return { text: s, cls: "bad", ready: false };
  const p = typeof it.progress === "number" ? " " + Math.round(it.progress * 100) + "%" : "";
  return { text: (s || "processing") + p, cls: "busy", ready: false };
}

function renderList() {
  const items = state.lists[state.tab] || [];
  const q = $("#filter").value.trim().toLowerCase();
  const shown = q ? items.filter((it) => String(it.name || "").toLowerCase().includes(q)) : items;
  $("#list-status").textContent = items.length ? (shown.length === items.length ? items.length + " item(s)" : shown.length + " of " + items.length) :
    "Nothing here yet. Add downloads on torbox.app, then press Refresh.";
  $("#items").replaceChildren(...shown.slice(0, 300).map(renderItem));
}

function folder() {
  const f = $("#folder").value.trim().replace(/\/+$/, "");
  localStorage.setItem("folder", f);
  return f;
}

function relDir(file) {
  // TorBox file names carry the item's folder structure ("Name/sub/file.ext"); keep it for whole-item downloads.
  const n = String(file.name || "");
  const i = n.lastIndexOf("/");
  return i > 0 ? n.slice(0, i) : "";
}

async function queue(jobs) {
  const f = folder();
  if (!f) { toast("Choose a folder first"); return; }
  let ok = 0, last = "";
  for (const j of jobs) {
    try { await api("download", { folder: f, ...j }); ok++; } catch (e) { last = e.message; }
  }
  toast(ok ? "Queued " + ok + " download(s) to " + f + (last ? " (some failed: " + last + ")" : "") : "Not queued: " + last);
  pollJobs();
}

function renderItem(it) {
  const st = itemState(it);
  const files = Array.isArray(it.files) ? it.files : [];
  const key = state.tab + ":" + it.id;
  const open = state.open.has(key);
  const fileJob = (f, withDir) => ({ kind: state.tab, id: it.id, file_id: f.id, name: f.short_name || String(f.name || "").split("/").pop(),
    subdir: withDir ? (relDir(f) || it.name) : "" });
  const box = el("div", { class: "item" },
    el("div", { class: "item-head" },
      el("div", { class: "grow" },
        el("div", { class: "item-name" }, it.name || "(no name)"),
        el("div", { class: "meta" },
          el("span", { class: "state " + st.cls }, st.text),
          el("span", {}, size(it.size)),
          el("span", {}, files.length + " file" + (files.length === 1 ? "" : "s")),
          it.created_at ? el("span", {}, String(it.created_at).slice(0, 10)) : null))),
    el("div", { class: "actions" },
      files.length ? el("button", { class: "small", type: "button", onclick: () => { open ? state.open.delete(key) : state.open.add(key); renderList(); } },
        open ? "Hide files" : "Show files") : null,
      el("button", { class: "small primary", type: "button", disabled: !st.ready || !files.length,
        onclick: () => queue(files.map((f) => fileJob(f, true))) }, files.length > 1 ? "Download all files" : "Download"),
      files.length > 1 ? el("button", { class: "small", type: "button", disabled: !st.ready,
        onclick: () => queue([{ kind: state.tab, id: it.id, file_id: "", name: (it.name || "download") + ".zip" }]) }, "As one ZIP") : null));
  if (open) {
    box.append(el("ul", { class: "files" }, files.map((f) => el("li", {},
      el("span", { class: "fname" }, f.short_name || f.name),
      el("span", { class: "fsize" }, size(f.size)),
      el("button", { class: "small", type: "button", disabled: !st.ready, onclick: () => queue([fileJob(f, false)]) }, "Download")))));
  }
  return box;
}

// ---------- downloads ----------
async function pollJobs() {
  clearTimeout(state.pollTimer);
  try {
    const r = await api("jobs");
    state.jobs = r.jobs;
    $("#connections").value = String(r.connections);
    $("#parallel").value = String(r.parallel);
    if (!$("#connections").value) $("#connections").append(el("option", {}, String(r.connections))), $("#connections").value = String(r.connections);
  } catch (e) { /* offline: retry below */ }
  const active = state.jobs.filter((j) => ["queued", "starting", "downloading"].includes(j.state)).length;
  const badge = $("#badge");
  badge.textContent = active; badge.classList.toggle("hidden", !active);
  if (state.tab === "downloads") renderJobs();
  state.pollTimer = setTimeout(pollJobs, state.tab === "downloads" || active ? 1000 : 5000);
}

function jobAction(action, id) {
  return api("jobs/" + action, { id }).then(pollJobs).catch((e) => toast(e.message));
}

function renderJobs() {
  const jobs = state.jobs.slice().reverse();
  $("#jobs-empty").classList.toggle("hidden", jobs.length > 0);
  $("#jobs").replaceChildren(...jobs.map((j) => {
    const pct = j.total > 0 ? Math.min(100, (j.done / j.total) * 100) : 0;
    const running = j.state === "downloading";
    const parts = [];
    if (j.total > 0) parts.push(size(j.done) + " of " + size(j.total) + " (" + pct.toFixed(1) + "%)");
    else if (j.done > 0) parts.push(size(j.done));
    if (running && j.speed > 0) parts.push(size(j.speed) + "/s");
    if (running && j.speed > 0 && j.total > 0) parts.push(duration((j.total - j.done) / j.speed) + " left");
    if (running && j.conns) parts.push(j.conns + " connection" + (j.conns > 1 ? "s" : ""));
    const canPause = ["queued", "starting", "downloading"].includes(j.state);
    const canResume = ["paused", "failed", "cancelled"].includes(j.state);
    const canCancel = ["queued", "starting", "downloading", "paused", "failed"].includes(j.state);
    return el("div", { class: "item job " + j.state },
      el("div", { class: "item-name" }, j.name),
      el("div", { class: "meta" },
        el("span", { class: "state " + (j.state === "done" ? "ready" : j.state === "failed" ? "bad" : "busy") }, j.state),
        parts.length ? el("span", {}, parts.join(" · ")) : null),
      el("div", { class: "bar" + (running && j.total <= 0 ? " indet" : "") }, el("div", { style: "width:" + (j.state === "done" ? 100 : pct) + "%" })),
      el("div", { class: "meta" }, el("code", {}, j.path)),
      j.err ? el("div", { class: "error small" }, j.err) : null,
      j.note ? el("div", { class: "muted small" }, j.note) : null,
      el("div", { class: "actions" },
        canPause ? el("button", { class: "small", type: "button", onclick: () => jobAction("pause", j.id) }, "Pause") : null,
        canResume ? el("button", { class: "small primary", type: "button", onclick: () => jobAction("resume", j.id) }, j.state === "failed" ? "Retry (resume)" : "Resume") : null,
        canCancel ? el("button", { class: "small danger", type: "button", onclick: () => {
          if (confirm("Cancel and delete the partial file?")) jobAction("cancel", j.id);
        } }, "Cancel") : null,
        !canPause ? el("button", { class: "small", type: "button", onclick: () => jobAction("remove", j.id) }, "Remove from list") : null));
  }));
}

$("#clear").addEventListener("click", () => jobAction("clear", 0));
for (const id of ["#connections", "#parallel"]) {
  $(id).addEventListener("change", () => api("settings", { connections: $("#connections").value, parallel: $("#parallel").value })
    .then(() => toast("Saved (applies to downloads that start from now)")).catch((e) => toast(e.message)));
}
$("#url-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const u = $("#url").value.trim();
  if (!u) return;
  queue([{ kind: "url", url: u }]).then(() => { $("#url").value = ""; });
});

// ---------- folder picker ----------
const picker = { path: "" };
$("#browse").addEventListener("click", () => openPicker($("#folder").value.trim() || state.status.default_folder));
$("#picker-close").addEventListener("click", () => $("#picker").classList.add("hidden"));
$("#picker").addEventListener("click", (ev) => { if (ev.target.id === "picker") $("#picker").classList.add("hidden"); });
$("#picker-up").addEventListener("click", () => picker.parent && browse(picker.parent));
$("#picker-use").addEventListener("click", () => {
  if (!picker.path) return;
  $("#folder").value = picker.path; folder();
  $("#picker").classList.add("hidden");
});
$("#picker-new").addEventListener("click", async () => {
  if (!picker.path) return;
  const name = prompt("New folder name in " + picker.path);
  if (!name) return;
  const path = picker.path.replace(/\/$/, "") + "/" + name.replace(/[\/\\]/g, "_");
  try { await api("mkdir", { path }); browse(path); } catch (e) { $("#picker-error").textContent = e.message; }
});

async function openPicker(start) {
  $("#picker").classList.remove("hidden");
  // Walk up from the typed folder to the first one that exists.
  let p = start.replace(/\/+$/, "") || "/data";
  for (let i = 0; i < 12 && p; i++) {
    if (await browse(p, true)) return;
    p = p.slice(0, p.lastIndexOf("/"));
  }
  browse("/data");
}

async function browse(path, quiet) {
  $("#picker-error").textContent = "";
  let r;
  try { r = await api("dirs?path=" + encodeURIComponent(path)); } catch (e) {
    if (!quiet) $("#picker-error").textContent = e.message;
    if (e.data && e.data.ok === false && !quiet) return false;
    return false;
  }
  picker.path = r.path; picker.parent = r.parent;
  $("#picker-path").textContent = r.path;
  $("#picker-up").disabled = !r.parent;
  $("#picker-free").textContent = r.free !== undefined ? size(r.free) + " free" : "";
  $("#picker-roots").replaceChildren(...r.roots.map((root) => el("button", { type: "button", onclick: () => browse(root) },
    root.startsWith("/mnt/usb") ? "USB " + root.slice(8) : root.startsWith("/mnt/ext") ? "Ext " + root.slice(8) : root)));
  const items = r.dirs.map((d) => el("li", { onclick: () => browse(r.path.replace(/\/$/, "") + "/" + d) }, "\u{1F4C1} " + d));
  const files = (r.files || []).map((f) => el("li", { class: "file" }, el("span", { class: "fname" }, f.name), el("span", { class: "fsize" }, size(f.size))));
  $("#picker-dirs").replaceChildren(...(items.length || files.length ? items.concat(files) : [el("li", { class: "empty" }, "Empty folder")]));
  return true;
}

loadStatus();
