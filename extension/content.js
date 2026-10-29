// SPDX-License-Identifier: GPL-3.0-or-later
// Runs in pages: reports Alt/Insert clicks (bypass / force Lanes), shows a "Download with Lanes"
// button over videos, and lists the page's links for "Download all links".
(() => {
  if (window.__lanesContent) return;
  window.__lanesContent = true;

  const send = (message) => chrome.runtime.sendMessage(message).catch(() => undefined);

  // --- modifier keys at click time ---
  let insertDown = false;
  addEventListener("keydown", (e) => { if (e.key === "Insert") insertDown = true; }, true);
  addEventListener("keyup", (e) => { if (e.key === "Insert") insertDown = false; }, true);
  addEventListener("blur", () => { insertDown = false; });
  addEventListener(
    "mousedown",
    (e) => {
      if (e.altKey) send({ type: "bypass" });
      else if (insertDown) send({ type: "force" });
    },
    true,
  );

  // --- links for "Download all links with Lanes" ---
  chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
    if (msg?.type !== "collect_links") return false;
    const seen = new Set();
    for (const a of document.querySelectorAll("a[href]")) {
      const href = a.href;
      if (/^(https?|ftps?):\/\//i.test(href) && !href.includes("#")) seen.add(href);
      if (seen.size >= 1000) break;
    }
    sendResponse([...seen]);
    return false;
  });

  // --- the video button ---
  let enabled = true;
  chrome.storage.sync.get({ videoButton: true }, (s) => { enabled = s.videoButton; });
  chrome.storage.onChanged.addListener((changes) => {
    if (changes.videoButton) enabled = changes.videoButton.newValue;
  });

  let host = null; // shadow host of the button
  let button = null;
  let current = null; // the video under the pointer
  let hideTimer = 0;

  function makeButton() {
    host = document.createElement("lanes-video-button");
    host.style.cssText = "all:initial;position:absolute;z-index:2147483647;display:none;";
    const root = host.attachShadow({ mode: "closed" });
    root.innerHTML = `<style>
      button{font:600 12px/1 "Segoe UI",system-ui,sans-serif;color:#fff;background:rgba(0,90,180,.92);
        border:1px solid rgba(255,255,255,.35);border-radius:6px;padding:7px 11px;cursor:pointer;
        box-shadow:0 2px 8px rgba(0,0,0,.35);display:flex;gap:6px;align-items:center}
      button:hover{background:rgba(0,103,192,1)}
      svg{width:14px;height:14px}
    </style>
    <button type="button" title="Send this video to Lanes">
      <svg viewBox="0 0 16 16" fill="currentColor"><path d="M8 1v8.6l3-3 .9.9L8 12 3.1 7.5l.9-.9 3 3V1z"/>
      <path d="M2 13h12v1.5H2z"/></svg><span>Download with Lanes</span></button>`;
    button = root.querySelector("button");
    const label = root.querySelector("span");
    button.addEventListener("click", async (e) => {
      e.preventDefault();
      e.stopPropagation();
      label.textContent = "Sending…";
      const reply = await send({ type: "video", srcUrl: current?.currentSrc || current?.src || "" });
      label.textContent = reply?.result === "accepted" ? "Sent to Lanes" : "Lanes could not take it";
      if (reply?.result !== "accepted" && reply?.message) button.title = reply.message;
      setTimeout(() => { label.textContent = "Download with Lanes"; button.title = "Send this video to Lanes"; }, 2500);
    });
    host.addEventListener("mouseenter", () => clearTimeout(hideTimer));
    document.documentElement.appendChild(host);
  }

  function place(video) {
    if (!host) makeButton();
    const r = video.getBoundingClientRect();
    host.style.left = `${Math.max(0, r.right + scrollX - 190)}px`;
    host.style.top = `${Math.max(0, r.top + scrollY + 10)}px`;
    host.style.display = "block";
  }

  function hideSoon() {
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => { if (host) host.style.display = "none"; current = null; }, 1500);
  }

  let last = 0;
  addEventListener(
    "mousemove",
    (e) => {
      if (!enabled) return;
      const now = Date.now();
      if (now - last < 150) return;
      last = now;
      // Players often cover the <video> with their own controls: look at everything under the pointer.
      const video = document.elementsFromPoint(e.clientX, e.clientY).find((el) => el.tagName === "VIDEO");
      if (video && video.offsetWidth >= 200 && video.offsetHeight >= 120) {
        clearTimeout(hideTimer);
        current = video;
        place(video);
      } else if (current && !(host && host.matches(":hover"))) {
        hideSoon();
      }
    },
    { passive: true },
  );
})();
