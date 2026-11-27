// TorBox for PS5 web UI. Talks only to the console; the console talks to TorBox, Real-Debrid and AllDebrid.
"use strict";

const $ = (s) => document.querySelector(s);
const PROVIDERS = {
  torbox: {
    title: "TorBox", keyName: "API key",
    help: "Paste your TorBox API key (torbox.app, Settings, API Key).",
    tabs: [["torrents", "Torrents"], ["webdl", "Web"], ["usenet", "Usenet"]],
  },
  realdebrid: {
    title: "Real-Debrid", keyName: "API token",
    help: "Paste your Real-Debrid private API token (real-debrid.com/apitoken, while logged in there).",
    tabs: [["rd-torrents", "Torrents"], ["rd-history", "History"]],
  },
  alldebrid: {
    title: "AllDebrid", keyName: "API key",
    help: "Paste an AllDebrid API key (alldebrid.com/apikeys, while logged in there).",
    tabs: [["ad-magnets", "Magnets"], ["ad-links", "Saved links"], ["ad-history", "History"]],
  },
};
function loadPref(k, d) { try { return localStorage.getItem(k) || d; } catch (e) { return d; } }
function savePref(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* private mode */ } }

const hashProvider = location.hash.slice(1); // #torbox, #realdebrid or #alldebrid picks the service (bookmarks)
const state = {
  provider: PROVIDERS[hashProvider] ? hashProvider : PROVIDERS[loadPref("provider", "")] ? loadPref("provider", "") : "torbox",
  tab: { torbox: "torrents", realdebrid: "rd-torrents", alldebrid: "ad-magnets" },
  lists: {}, open: new Set(), rdInfo: {}, adFiles: {}, status: null, jobs: [], installs: [], served: {}, pollTimer: 0,
  updateChecked: false,
};

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
const curTab = () => state.tab[state.provider];
const prov = () => state.status && state.status.providers[state.provider];

// ---------- providers, login ----------
document.querySelectorAll("#providers button").forEach((b) => b.addEventListener("click", () => {
  state.provider = b.dataset.provider; savePref("provider", state.provider);
  history.replaceState(null, "", "#" + state.provider);
  render();
}));

async function loadStatus() {
  try {
    state.status = await api("status");
  } catch (e) {
    $("#conn").textContent = "console offline";
    setTimeout(loadStatus, 3000);
    return;
  }
  setConn(true);
  if (!$("#folder").value) $("#folder").value = loadPref("folder", state.status.default_folder);
  render();
  pollJobs();
  if (!state.updateChecked) { state.updateChecked = true; checkUpdate(false); }
}

// Update check: the console asks GitHub for the newest release (cached there for 6 hours).
async function checkUpdate(manual) {
  let r;
  try { r = await api("update" + (manual ? "?fresh=1" : "")); } catch (e) { if (manual) toast("Update check failed: " + e.message); return; }
  const box = $("#update");
  if (r.newer) {
    box.replaceChildren(el("span", {}, "Update available: " + r.latest + " (you have " + r.current + ")."),
      r.url ? el("a", { href: r.url, target: "_blank", rel: "noopener" }, "Release notes and download") : null,
      el("span", { class: "muted small" }, "Replace the files on the console, then press Stop server and open the tile again; downloads resume."));
    box.classList.remove("hidden");
  } else box.classList.add("hidden");
  if (manual) toast(r.newer ? "Update available: " + r.latest : r.error ? "Update check: " + r.error : "Up to date (" + r.current + ")");
}
$("#update-btn").addEventListener("click", () => checkUpdate(true));

function accountText(id) {
  const p = state.status.providers[id], u = p.user;
  if (!u) return "Logged in" + (p.user_error ? " (" + p.user_error + ")" : "");
  if (id === "realdebrid") {
    const prem = u.type === "premium";
    return "Logged in as " + u.name + " · " + (prem ? "premium" + (u.expires ? " until " + u.expires.slice(0, 10) : "") : u.type + " account (downloads need premium)");
  }
  if (id === "alldebrid") {
    const until = u.premium_until > 0 ? " until " + new Date(u.premium_until * 1000).toISOString().slice(0, 10) : "";
    return "Logged in as " + u.name + " · " + (u.premium ? "premium" + until : u.trial ? "trial account" : "free account (downloads need premium)");
  }
  return "Logged in as " + u.name + (u.expires ? " · plan until " + u.expires.slice(0, 10) : "");
}

function render() {
  const s = state.status, P = PROVIDERS[state.provider], p = prov();
  document.querySelectorAll("#providers button").forEach((b) => {
    b.classList.toggle("active", b.dataset.provider === state.provider);
    b.replaceChildren(PROVIDERS[b.dataset.provider].title,
      s.providers[b.dataset.provider].logged_in ? el("span", { class: "dot", title: "logged in" }) : "");
  });
  if (!p.logged_in) {
    $("#login").classList.remove("hidden"); $("#app").classList.add("hidden");
    $("#login-title").textContent = "Log in to " + P.title;
    $("#login-help").textContent = P.help;
    $("#key").placeholder = P.keyName;
    $("#key-file").textContent = p.key_file;
    $("#login-error").textContent = "";
    return;
  }
  $("#login").classList.add("hidden"); $("#app").classList.remove("hidden");
  $("#account").textContent = P.title + ": " + accountText(state.provider) + " · " + s.mode + " " + s.version;
  $("#logout").textContent = "Log out of " + P.title;
  const viaRd = s.providers.realdebrid.logged_in, viaAd = s.providers.alldebrid.logged_in;
  $("#url-via-wrap").classList.toggle("hidden", !viaRd && !viaAd);
  $("#url-via").querySelector("[value=rd]").hidden = !viaRd;
  $("#url-via").querySelector("[value=ad]").hidden = !viaAd;
  $("#url-via").value = state.provider === "realdebrid" && viaRd ? "rd" : state.provider === "alldebrid" && viaAd ? "ad" :
    viaRd ? "rd" : viaAd ? "ad" : "url";
  $("#tabs").replaceChildren(...P.tabs.map(([id, label]) => el("button", { "data-tab": id, onclick: () => showTab(id) }, label)),
    el("button", { "data-tab": "downloads", onclick: () => showTab("downloads") }, "Downloads ", el("span", { id: "badge", class: "badge hidden" }, "0")));
  showTab(curTab());
  updateBadge();
}

$("#login-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const input = $("#key"), btn = ev.target.querySelector("button");
  $("#login-error").textContent = "";
  btn.disabled = true; btn.textContent = "Checking...";
  try {
    await api("login", { provider: state.provider, key: input.value.trim() });
    input.value = "";
    state.lists = {}; state.rdInfo = {}; state.adFiles = {};
    await loadStatus();
  } catch (e) {
    $("#login-error").textContent = e.message;
  } finally {
    btn.disabled = false; btn.textContent = "Log in";
  }
});

$("#logout").addEventListener("click", async () => {
  const P = PROVIDERS[state.provider];
  if (!confirm("Log out of " + P.title + " and delete its stored " + P.keyName + " from the console?")) return;
  await api("logout", { provider: state.provider }).catch((e) => toast(e.message));
  for (const [tab] of P.tabs) delete state.lists[tab];
  state.rdInfo = {}; state.adFiles = {};
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
function showTab(tab) {
  state.tab[state.provider] = tab;
  document.querySelectorAll("#tabs button").forEach((b) => b.classList.toggle("active", b.dataset.tab === tab));
  const dl = tab === "downloads";
  $("#list-view").classList.toggle("hidden", dl);
  $("#downloads-view").classList.toggle("hidden", !dl);
  if (dl) renderJobs();
  else if (state.lists[tab]) renderList();
  else loadList(false);
}

// Real-Debrid lists come in pages; a list keeps {items, total, page}.
async function loadList(fresh, more) {
  const tab = curTab();
  const rd = tab.startsWith("rd-"), ad = tab.startsWith("ad-");
  const old = state.lists[tab];
  if (!more) { $("#list-status").textContent = "Loading..."; $("#items").replaceChildren(); $("#more").classList.add("hidden"); }
  try {
    if (ad) {
      const r = await api("ad/" + tab.slice(3));
      let items = Array.isArray(r.data) ? r.data : Object.values(r.data || {});
      if (tab === "ad-magnets") items.sort((a, b) => (b.uploadDate || 0) - (a.uploadDate || 0));
      else items.sort((a, b) => (b.date || 0) - (a.date || 0));
      state.lists[tab] = { items, total: items.length, page: 1 };
    } else if (rd) {
      const page = more && old ? old.page + 1 : 1;
      const r = await api("rd/" + (tab === "rd-torrents" ? "torrents" : "downloads") + "?limit=100&page=" + page);
      const items = (more && old ? old.items : []).concat(Array.isArray(r.data) ? r.data : []);
      state.lists[tab] = { items, total: r.total >= 0 ? r.total : items.length, page };
    } else {
      const r = await api("list?kind=" + tab + (fresh ? "&fresh=1" : ""));
      let data = r.data || [];
      if (!Array.isArray(data)) data = [data];
      data.sort((a, b) => String(b.created_at || "").localeCompare(String(a.created_at || "")));
      state.lists[tab] = { items: data, total: data.length, page: 1 };
    }
  } catch (e) {
    if (e.status === 401) return loadStatus();
    if (!more) state.lists[tab] = null;
    $("#list-status").textContent = "Could not load the list: " + e.message;
    return;
  }
  if (tab === curTab()) renderList();
}

$("#refresh").addEventListener("click", () => {
  if (curTab().startsWith("rd-")) state.rdInfo = {};
  if (curTab().startsWith("ad-")) state.adFiles = {};
  loadList(true);
});
$("#more").addEventListener("click", () => loadList(false, true));
$("#filter").addEventListener("input", () => renderList());

function renderList() {
  const tab = curTab(), L = state.lists[tab] || { items: [], total: 0 };
  const items = L.items;
  const name = (it) => String(it.name || it.filename || "");
  const q = $("#filter").value.trim().toLowerCase();
  const shown = q ? items.filter((it) => name(it).toLowerCase().includes(q)) : items;
  const more = L.total > items.length;
  $("#list-status").textContent = items.length ?
    (shown.length === items.length ? items.length + (more ? " of " + L.total : "") + " item(s)" : shown.length + " of " + items.length) :
    tab.startsWith("rd-") ? "Nothing here yet. Add torrents on real-debrid.com, then press Refresh." :
    tab === "ad-history" ? "Nothing here yet (AllDebrid keeps history only when it is enabled in your account settings)." :
    tab.startsWith("ad-") ? "Nothing here yet. Add magnets or links on alldebrid.com, then press Refresh." :
      "Nothing here yet. Add downloads on torbox.app, then press Refresh.";
  const render = tab === "rd-torrents" ? renderRdTorrent : tab === "rd-history" ? renderRdHistory :
    tab === "ad-magnets" ? renderAdMagnet : tab.startsWith("ad-") ? renderAdLink : renderItem;
  $("#items").replaceChildren(...shown.slice(0, 500).map(render));
  $("#more").classList.toggle("hidden", !more);
  $("#more").textContent = "Load more (" + (L.total - items.length) + " left)";
}

function folder() {
  const f = $("#folder").value.trim().replace(/\/+$/, "");
  savePref("folder", f);
  return f;
}

function dirOf(path) {
  const n = String(path || "").replace(/^\/+/, "");
  const i = n.lastIndexOf("/");
  return i > 0 ? n.slice(0, i) : "";
}
function baseOf(path) { return String(path || "").split("/").pop(); }

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

// ---------- installing packages while they download ----------
const isPkg = (name) => /\.pkg$/i.test(String(name || ""));
async function installPkg(spec, name) {
  if (!confirm("Install " + name + " on the PS5?\n\nThe PS5 installs it while it downloads: no .pkg file is saved, " +
      "only the installed game takes space. Progress shows in Downloads and on the PS5.")) return;
  try {
    await api("install", { ...spec, name });
    toast("Installing " + name + " on the PS5");
  } catch (e) { toast("Not started: " + e.message); }
  pollJobs();
}
const installButton = (spec, name, enabled = true) => el("button", { class: "small", type: "button", disabled: !enabled,
  title: "Install on the PS5 while it downloads (no .pkg file kept)", onclick: () => installPkg(spec, name) }, "Stream Install");

function itemCard(title, metaParts, actions, extra) {
  return el("div", { class: "item" },
    el("div", { class: "item-name" }, title || "(no name)"),
    el("div", { class: "meta" }, metaParts),
    el("div", { class: "actions" }, actions), extra);
}

function itemState(it) {
  const s = String(it.download_state || "");
  if (it.download_present || it.download_finished) return { text: s && !/^(completed|cached|uploading|seeding)/i.test(s) ? s : "ready", cls: "ready", ready: true };
  if (/error|fail|stalled|dead/i.test(s)) return { text: s, cls: "bad", ready: false };
  const p = typeof it.progress === "number" ? " " + Math.round(it.progress * 100) + "%" : "";
  return { text: (s || "processing") + p, cls: "busy", ready: false };
}

// TorBox item (torrent, web download, usenet).
function renderItem(it) {
  const tab = curTab(), st = itemState(it);
  const files = Array.isArray(it.files) ? it.files : [];
  const key = tab + ":" + it.id, open = state.open.has(key);
  const fileJob = (f, withDir) => ({ kind: tab, id: it.id, file_id: f.id, name: f.short_name || baseOf(f.name),
    subdir: withDir ? (dirOf(f.name) || it.name) : "" });
  return itemCard(it.name, [
    el("span", { class: "state " + st.cls }, st.text), el("span", {}, size(it.size)),
    el("span", {}, files.length + " file" + (files.length === 1 ? "" : "s")),
    it.created_at ? el("span", {}, String(it.created_at).slice(0, 10)) : null,
  ], [
    files.length ? el("button", { class: "small", type: "button", onclick: () => { open ? state.open.delete(key) : state.open.add(key); renderList(); } },
      open ? "Hide files" : "Show files") : null,
    el("button", { class: "small primary", type: "button", disabled: !st.ready || !files.length,
      onclick: () => queue(files.map((f) => fileJob(f, true))) }, files.length > 1 ? "Download all files" : "Download"),
    files.length > 1 ? el("button", { class: "small", type: "button", disabled: !st.ready,
      onclick: () => queue([{ kind: tab, id: it.id, file_id: "", name: (it.name || "download") + ".zip" }]) }, "As one ZIP") : null,
    files.length === 1 && isPkg(files[0].name) ? installButton({ kind: tab, id: it.id, file_id: files[0].id },
      files[0].short_name || baseOf(files[0].name), st.ready) : null,
  ], open ? el("ul", { class: "files" }, files.map((f) => el("li", {},
    el("span", { class: "fname" }, f.short_name || f.name), el("span", { class: "fsize" }, size(f.size)),
    isPkg(f.name) ? installButton({ kind: tab, id: it.id, file_id: f.id }, f.short_name || baseOf(f.name), st.ready) : null,
    el("button", { class: "small", type: "button", disabled: !st.ready, onclick: () => queue([fileJob(f, false)]) }, "Download")))) : null);
}

// ---------- Real-Debrid ----------
async function rdInfo(id) {
  if (!state.rdInfo[id]) state.rdInfo[id] = await api("rd/info?id=" + encodeURIComponent(id));
  return state.rdInfo[id];
}

// Jobs for a torrent: each selected file with its link (folder structure from the file paths), or, when
// Real-Debrid packed the files, its links (named by Real-Debrid when they start).
function rdJobs(info, files, withDir) {
  const top = info.filename || "download";
  if (!info.paired) return info.links.map((link) => ({ kind: "rd", url: link, name: "", subdir: withDir ? top : "" }));
  // Paths may or may not start with the torrent's own folder: add it only when it is missing.
  const inTop = info.files.every((f) => String(f.path).replace(/^\/+/, "").startsWith(top + "/"));
  const dirFor = (f) => {
    const d = dirOf(f.path);
    if (inTop || info.files.length === 1) return d;
    return top + (d ? "/" + d : "");
  };
  return files.filter((f) => f.link).map((f) => ({ kind: "rd", url: f.link, name: baseOf(f.path),
    subdir: withDir ? dirFor(f) : "" }));
}

function renderRdTorrent(t) {
  const key = "rd:" + t.id, open = state.open.has(key), info = state.rdInfo[t.id];
  const ready = t.status === "downloaded";
  const bad = /error|virus|dead/.test(t.status);
  const stText = ready ? "ready" : t.status + (t.status === "downloading" ? " " + t.progress + "%" : "");
  const toggle = async () => {
    if (open) { state.open.delete(key); return renderList(); }
    try { await rdInfo(t.id); state.open.add(key); renderList(); } catch (e) { toast(e.message); }
  };
  const all = async () => {
    try { const i = await rdInfo(t.id); queue(rdJobs(i, i.files, true)); } catch (e) { toast(e.message); }
  };
  let extra = null;
  if (open && info) {
    const rows = info.files.map((f) => el("li", {},
      el("span", { class: "fname" }, String(f.path).replace(/^\//, "")), el("span", { class: "fsize" }, size(f.bytes)),
      info.paired && isPkg(f.path) ? installButton({ kind: "rd", url: f.link }, baseOf(f.path), ready && !!f.link) : null,
      info.paired ? el("button", { class: "small", type: "button", disabled: !ready || !f.link,
        onclick: () => queue(rdJobs(info, [f], false)) }, "Download") : null));
    extra = el("div", {}, el("ul", { class: "files" }, rows),
      !info.paired && info.links.length ? el("div", {}, el("p", { class: "links-note" },
        "Real-Debrid packed these files into " + info.links.length + " link(s); each is named by Real-Debrid when it starts:"),
      el("ul", { class: "files" }, info.links.map((link, i) => el("li", {}, el("span", { class: "fname" }, "Link " + (i + 1)),
        el("button", { class: "small", type: "button", disabled: !ready, onclick: () => queue([{ kind: "rd", url: link, name: "", subdir: "" }]) }, "Download"))))) : null);
  }
  return itemCard(t.filename, [
    el("span", { class: "state " + (ready ? "ready" : bad ? "bad" : "busy") }, stText), el("span", {}, size(t.bytes)),
    el("span", {}, (t.links || []).length + " link" + ((t.links || []).length === 1 ? "" : "s")),
    t.added ? el("span", {}, String(t.added).slice(0, 10)) : null,
  ], [
    el("button", { class: "small", type: "button", onclick: toggle }, open ? "Hide files" : "Show files"),
    el("button", { class: "small primary", type: "button", disabled: !ready || !(t.links || []).length, onclick: all },
      (t.links || []).length > 1 ? "Download all files" : "Download"),
    isPkg(t.filename) && (t.links || []).length === 1 ? installButton({ kind: "rd", url: t.links[0] }, t.filename, ready) : null,
  ], extra);
}

function renderRdHistory(d) {
  return itemCard(d.filename, [
    el("span", {}, size(d.filesize)), d.host ? el("span", {}, d.host) : null,
    d.generated ? el("span", {}, String(d.generated).slice(0, 10)) : null,
  ], [
    el("button", { class: "small primary", type: "button", disabled: !d.link,
      onclick: () => queue([{ kind: "rd", url: d.link, name: d.filename || "" }]) }, "Download"),
    isPkg(d.filename) && d.link ? installButton({ kind: "rd", url: d.link }, d.filename) : null,
  ]);
}

// ---------- AllDebrid ----------
// A magnet's files come as a tree: folders {n, e: [...]}, files {n, s, l}. Flattened to {path, size, link}.
function adFlatten(nodes, dir, out) {
  for (const n of nodes || []) {
    const path = dir ? dir + "/" + n.n : String(n.n || "");
    if (Array.isArray(n.e)) adFlatten(n.e, path, out);
    else out.push({ path, size: n.s, link: n.l });
  }
  return out;
}
async function adFiles(id) {
  if (!state.adFiles[id]) state.adFiles[id] = adFlatten((await api("ad/files?id=" + encodeURIComponent(id))).files, "", []);
  return state.adFiles[id];
}
// Jobs for magnet files: the links are unlocked when each download starts. With withDir the magnet's folder
// structure is kept, adding the magnet's own name on top when the paths don't start with it.
function adJobs(m, all, files, withDir) {
  const top = m.filename || "download";
  const inTop = all.every((f) => f.path.startsWith(top + "/"));
  const dirFor = (f) => {
    const d = dirOf(f.path);
    return inTop || all.length === 1 ? d : top + (d ? "/" + d : "");
  };
  return files.filter((f) => f.link).map((f) => ({ kind: "ad", url: f.link, name: baseOf(f.path), subdir: withDir ? dirFor(f) : "" }));
}

function renderAdMagnet(m) {
  const key = "ad:" + m.id, open = state.open.has(key), files = state.adFiles[m.id];
  const ready = m.statusCode === 4, bad = m.statusCode >= 5;
  const pct = m.size > 0 && m.downloaded >= 0 ? " " + Math.floor((m.downloaded / m.size) * 100) + "%" : "";
  const stText = ready ? "ready" : (m.status || "processing") + (m.statusCode === 1 ? pct : "");
  const toggle = async () => {
    if (open) { state.open.delete(key); return renderList(); }
    try { await adFiles(m.id); state.open.add(key); renderList(); } catch (e) { toast(e.message); }
  };
  const all = async () => {
    try { const f = await adFiles(m.id); queue(adJobs(m, f, f, true)); } catch (e) { toast(e.message); }
  };
  const extra = open && files ? el("ul", { class: "files" }, files.map((f) => el("li", {},
    el("span", { class: "fname" }, f.path), el("span", { class: "fsize" }, size(f.size)),
    isPkg(f.path) ? installButton({ kind: "ad", url: f.link }, baseOf(f.path), ready && !!f.link) : null,
    el("button", { class: "small", type: "button", disabled: !ready || !f.link, onclick: () => queue(adJobs(m, files, [f], false)) }, "Download")))) : null;
  return itemCard(m.filename, [
    el("span", { class: "state " + (ready ? "ready" : bad ? "bad" : "busy") }, stText), el("span", {}, size(m.size)),
    m.uploadDate ? el("span", {}, new Date(m.uploadDate * 1000).toISOString().slice(0, 10)) : null,
  ], [
    el("button", { class: "small", type: "button", disabled: !ready, onclick: toggle }, open ? "Hide files" : "Show files"),
    el("button", { class: "small primary", type: "button", disabled: !ready, onclick: all }, "Download all files"),
  ], extra);
}

function renderAdLink(d) {
  return itemCard(d.filename || d.link, [
    el("span", {}, size(d.size)), d.host ? el("span", {}, d.host) : null,
    d.date ? el("span", {}, new Date(d.date * 1000).toISOString().slice(0, 10)) : null,
  ], [
    el("button", { class: "small primary", type: "button", disabled: !d.link,
      onclick: () => queue([{ kind: "ad", url: d.link, name: d.filename || "" }]) }, "Download"),
    isPkg(d.filename) && d.link ? installButton({ kind: "ad", url: d.link }, d.filename) : null,
  ]);
}

function updateBadge() {
  const active = state.jobs.filter((j) => ["queued", "starting", "downloading"].includes(j.state)).length +
    state.installs.filter((s) => ["starting", "installing"].includes(s.install)).length;
  const badge = $("#badge");
  if (badge) { badge.textContent = active; badge.classList.toggle("hidden", !active); }
  return active;
}

// ---------- downloads ----------
async function pollJobs() {
  clearTimeout(state.pollTimer);
  try {
    const [r, st] = await Promise.all([api("jobs"), api("streams").catch(() => ({ streams: [] }))]);
    state.jobs = r.jobs;
    const now = Date.now();
    state.installs = (st.streams || []).filter((s) => s.install).map((s) => {
      const prev = state.served[s.id]; // speed from the bytes the installer pulled since the last poll
      s.speed = prev && now > prev.t && s.served >= prev.b ? (s.served - prev.b) * 1000 / (now - prev.t) : 0;
      if (prev && s.speed === 0 && s.active && prev.speed) s.speed = prev.speed * 0.5;
      state.served[s.id] = { t: now, b: s.served, speed: s.speed };
      return s;
    });
    $("#connections").value = String(r.connections);
    $("#parallel").value = String(r.parallel);
    if (!$("#connections").value) $("#connections").append(el("option", {}, String(r.connections))), $("#connections").value = String(r.connections);
  } catch (e) { /* offline: retry below */ }
  const active = updateBadge();
  if (curTab() === "downloads") renderJobs();
  state.pollTimer = setTimeout(pollJobs, curTab() === "downloads" || active ? 1000 : 5000);
}

function jobAction(action, id) {
  return api("jobs/" + action, { id }).then(pollJobs).catch((e) => toast(e.message));
}

function renderInstall(s) {
  const running = s.install === "starting" || s.install === "installing";
  const pct = s.size > 0 ? Math.min(100, (s.served / s.size) * 100) : 0;
  const parts = [];
  if (s.size > 0) parts.push(size(s.served) + " of " + size(s.size) + " (" + pct.toFixed(1) + "%)");
  if (running && s.speed > 0) parts.push(size(s.speed) + "/s");
  if (running && s.speed > 0 && s.size > s.served) parts.push(duration((s.size - s.served) / s.speed) + " left");
  if (s.install === "installed" && s.ins_time_s > 0) parts.push("in " + duration(s.ins_time_s));
  const label = { starting: "starting the installer", installing: "installing", installed: "installed", failed: "failed" }[s.install];
  const quiet = running && s.report_age_s > 60;
  return el("div", { class: "item job install " + s.install },
    el("div", { class: "item-name" }, s.name),
    el("div", { class: "meta" },
      el("span", { class: "state " + (s.install === "installed" ? "ready" : s.install === "failed" ? "bad" : "busy") }, label),
      el("span", {}, "Stream Install"), parts.length ? el("span", {}, parts.join(" · ")) : null),
    el("div", { class: "bar" }, el("div", { style: "width:" + (s.install === "installed" ? 100 : pct) + "%" })),
    s.content_id ? el("div", { class: "meta" }, el("code", {}, s.content_id)) : null,
    s.error ? el("div", { class: "error small" }, s.error) : null,
    quiet ? el("div", { class: "muted small" }, "The PS5 has not pulled any data for " + duration(s.report_age_s) +
      ": check Downloads on the PS5.") : null,
    running ? el("div", { class: "muted small" }, "To cancel, use the download list on the PS5.") : null,
    el("div", { class: "actions" }, !running ? el("button", { class: "small", type: "button",
      onclick: () => api("stream/remove", { id: s.id }).then(pollJobs).catch((e) => toast(e.message)) }, "Remove from list") : null));
}

function renderJobs() {
  const jobs = state.jobs.slice().reverse();
  const installs = state.installs.slice().sort((a, b) => a.ins_age_s - b.ins_age_s);
  $("#jobs-empty").classList.toggle("hidden", jobs.length + installs.length > 0);
  $("#jobs").replaceChildren(...installs.map(renderInstall), ...jobs.map((j) => {
    const pct = j.total > 0 ? Math.min(100, (j.done / j.total) * 100) : 0;
    const running = j.state === "downloading";
    const parts = [];
    if (j.total > 0) parts.push(size(j.done) + " of " + size(j.total) + " (" + pct.toFixed(1) + "%)");
    else if (j.done > 0) parts.push(size(j.done));
    if (running && j.speed > 0) parts.push(size(j.speed) + "/s");
    if (running && j.speed > 0 && j.total > 0) parts.push(duration((j.total - j.done) / j.speed) + " left");
    if (running && (j.conns || j.win)) {
      const target = j.win || j.conns;
      parts.push((target > 1 ? j.active + " of " + target + " connections active" : "1 connection") +
        (j.waiting ? " (" + j.waiting + " waiting)" : "") +
        (j.paused_conns ? " (disk backpressure)" : ""));
    }
    if (running && j.disk_speed > 0 && j.paused_conns) parts.push("disk " + size(j.disk_speed) + "/s");
    if (running && j.cdn) parts.push(j.cdn + (j.http_ver ? " HTTP/" + j.http_ver : ""));
    if (running && j.rcvbuf > 0) parts.push("rcvbuf " + Math.round(j.rcvbuf / 1024) + " KiB");
    if (running && j.usb_mode) parts.push("USB sequential");
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
        !canPause ? el("button", {
          class: "small", type: "button",
          title: (j.state === "failed" || j.state === "paused") ?
            "Remove from list (keeps any partial file on disk so re-downloading resumes)" : "Remove from list",
          onclick: () => jobAction("remove", j.id),
        }, "Remove from list") : null));
  }));
}

$("#clear").addEventListener("click", () => jobAction("clear", 0));
async function loadLogs() {
  $("#logs-body").textContent = "Loading...";
  try {
    const r = await fetch("/api/log");
    $("#logs-body").textContent = r.ok ? await r.text() : "HTTP " + r.status;
    $("#logs-body").scrollTop = $("#logs-body").scrollHeight;
  } catch (e) {
    $("#logs-body").textContent = "Could not load log";
  }
}
$("#logs-btn").addEventListener("click", () => { $("#logs-modal").classList.remove("hidden"); loadLogs(); });
$("#logs-refresh").addEventListener("click", loadLogs);
$("#logs-close").addEventListener("click", () => $("#logs-modal").classList.add("hidden"));
$("#logs-modal").addEventListener("click", (ev) => { if (ev.target.id === "logs-modal") $("#logs-modal").classList.add("hidden"); });
for (const id of ["#connections", "#parallel"]) {
  $(id).addEventListener("change", () => api("settings", { connections: $("#connections").value, parallel: $("#parallel").value })
    .then(() => toast("Saved (applies to downloads that start from now)")).catch((e) => toast(e.message)));
}
$("#url-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const u = $("#url").value.trim();
  if (!u) return;
  const via = $("#url-via-wrap").classList.contains("hidden") ? "url" : $("#url-via").value;
  queue([via === "url" ? { kind: "url", url: u } : { kind: via, url: u, name: "" }]).then(() => { $("#url").value = ""; });
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
