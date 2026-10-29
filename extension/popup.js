// SPDX-License-Identifier: GPL-3.0-or-later
import { DEFAULT_SETTINGS } from "./lib/rules.js";

const $ = (id) => document.getElementById(id);
const ask = (message) => chrome.runtime.sendMessage(message);

function formatSize(bytes) {
  if (!(bytes >= 0)) return "";
  const units = ["B", "KiB", "MiB", "GiB"];
  let v = bytes;
  let u = 0;
  while (v >= 1024 && u < units.length - 1) {
    v /= 1024;
    u++;
  }
  return `${v.toFixed(u === 0 ? 0 : 1)} ${units[u]}`;
}

function say(text) {
  $("message").textContent = text || "";
}

function result(reply) {
  say(reply?.result === "accepted" ? "Sent to Lanes." : reply?.message || "Lanes could not take it.");
}

async function main() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });

  // Connection to the desktop app.
  ask({ type: "ping" }).then((reply) => {
    const ok = reply?.result === "accepted";
    $("status").textContent = ok ? `Connected${reply.message ? " · " + reply.message : ""}` : "Lanes not found";
    $("status").classList.toggle("bad", !ok);
    if (!ok) $("status").title = reply?.message || "";
  });

  // Settings: saved on change.
  const s = { ...DEFAULT_SETTINGS, ...(await chrome.storage.sync.get(DEFAULT_SETTINGS)) };
  for (const key of Object.keys(DEFAULT_SETTINGS)) {
    const el = $(key);
    if (!el) continue;
    if (el.type === "checkbox") el.checked = s[key];
    else el.value = s[key];
    el.addEventListener("change", () => {
      const value = el.type === "checkbox" ? el.checked : el.type === "number" ? Math.max(0, Number(el.value) || 0) : el.value;
      chrome.storage.sync.set({ [key]: value });
    });
  }

  // Media the page loaded.
  const media = tab ? (await ask({ type: "popup_media", tabId: tab.id })) || [] : [];
  $("no-media").hidden = media.length > 0;
  $("send-all").hidden = media.length < 2;
  for (const m of media) {
    const li = document.createElement("li");
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = decodeURIComponent(new URL(m.url).pathname.split("/").pop() || m.url);
    name.title = m.url;
    const meta = document.createElement("span");
    meta.className = "meta";
    meta.textContent = [m.kind.toUpperCase(), formatSize(m.size)].filter(Boolean).join(" · ");
    const send = document.createElement("button");
    send.textContent = "Download";
    send.addEventListener("click", async () =>
      result(await ask({ type: "popup_send_media", tabId: tab.id, items: [{ url: m.url, mime: m.mime, size: m.size, referrer: m.referrer }] })),
    );
    li.append(name, meta, send);
    $("media").append(li);
  }
  $("send-all").addEventListener("click", async () =>
    result(await ask({
      type: "popup_send_media",
      tabId: tab.id,
      items: media.map((m) => ({ url: m.url, mime: m.mime, size: m.size, referrer: m.referrer })),
    })),
  );
  $("video").addEventListener("click", async () => {
    if (tab) result(await ask({ type: "popup_video", tabId: tab.id }));
  });
  $("open").addEventListener("click", async () => {
    const reply = await ask({ type: "open" });
    if (reply?.result !== "accepted") result(reply);
    else window.close();
  });
}

main();
