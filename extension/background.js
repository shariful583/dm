// SPDX-License-Identifier: GPL-3.0-or-later
// Service worker: hands browser downloads to Lanes, collects media per tab, context menus.
// Talks to the desktop app through the native-messaging host com.lanes.bridge (lanes-bridge.exe),
// which starts Lanes when needed. When Lanes does not answer, downloads stay in the browser.

import * as rules from "./lib/rules.js";

const HOST = "com.lanes.bridge";

async function settings() {
  return { ...rules.DEFAULT_SETTINGS, ...(await chrome.storage.sync.get(rules.DEFAULT_SETTINGS)) };
}

// One message to Lanes; a missing host or app counts as "rejected".
function send(message) {
  return new Promise((resolve) => {
    chrome.runtime.sendNativeMessage(HOST, { v: 1, ...message }, (reply) => {
      if (chrome.runtime.lastError || !reply) {
        resolve({ result: "rejected", message: chrome.runtime.lastError?.message || "no reply" });
      } else {
        resolve(reply);
      }
    });
  });
}

async function cookiesFor(url) {
  try {
    return rules.cookieHeader(await chrome.cookies.getAll({ url }));
  } catch {
    return "";
  }
}

// --- Alt-click bypass and Insert-click force (reported by content.js) ---

const modifier = { bypassUntil: 0, forceUntil: 0 };

// --- POST bodies of form submissions, by target URL (downloads do not say how they were made) ---

const recentPosts = new Map(); // url → {body, raw, time}

chrome.webRequest.onBeforeRequest.addListener(
  (d) => {
    if (d.method !== "POST" || !d.requestBody) return;
    const now = Date.now();
    for (const [url, p] of recentPosts) {
      if (now - p.time > 60_000) recentPosts.delete(url);
    }
    recentPosts.set(d.url, {
      body: rules.formEncode(d.requestBody.formData),
      raw: !d.requestBody.formData, // file uploads / binary bodies cannot be repeated
      time: now,
    });
  },
  { urls: ["<all_urls>"], types: ["main_frame", "sub_frame"] },
  ["requestBody"],
);

// --- download interception ---

chrome.downloads.onCreated.addListener(async (item) => {
  const now = Date.now();
  if (now < modifier.bypassUntil) return;
  const url = item.finalUrl || item.url;
  const size = item.fileSize > 0 ? item.fileSize : item.totalBytes > 0 ? item.totalBytes : -1;
  const forced = now < modifier.forceUntil && rules.isDownloadableUrl(url);
  const wanted = rules.shouldCapture(
    { url, filename: item.filename, mime: item.mime, size, referrer: item.referrer },
    await settings(),
  );
  if (!forced && !wanted) return;
  const post = recentPosts.get(item.url);
  if (post?.raw) return; // the body cannot be sent again: leave it to the browser

  // Hold the browser's copy while Lanes decides; resume it if Lanes says no.
  await chrome.downloads.pause(item.id).catch(() => {});
  const reply = await send({
    type: "capture_download",
    url: item.url,
    finalUrl: item.finalUrl,
    filename: rules.basename(item.filename) || rules.filenameFromUrl(url),
    fileSize: size >= 0 ? size : undefined,
    mime: item.mime,
    referrer: item.referrer,
    userAgent: navigator.userAgent,
    cookies: await cookiesFor(url),
    method: post ? "POST" : "GET",
    body: post?.body,
  });
  if (reply.result === "accepted") {
    await chrome.downloads.cancel(item.id).catch(() => {});
    await chrome.downloads.erase({ id: item.id }).catch(() => {});
  } else {
    await chrome.downloads.resume(item.id).catch(() => {});
  }
});

// --- media detection (badge shows the count per tab) ---

const mediaKey = (tabId) => `media:${tabId}`;

async function mediaOf(tabId) {
  return (await chrome.storage.session.get(mediaKey(tabId)))[mediaKey(tabId)] || [];
}

async function setMedia(tabId, list) {
  await chrome.storage.session.set({ [mediaKey(tabId)]: list });
  await chrome.action
    .setBadgeText({ tabId, text: list.length > 0 ? String(list.length) : "" })
    .catch(() => {});
}

chrome.webRequest.onHeadersReceived.addListener(
  (d) => {
    if (d.tabId < 0) return;
    const header = (name) =>
      (d.responseHeaders || []).find((h) => h.name.toLowerCase() === name)?.value || "";
    const length = Number(header("content-length") || -1);
    const kind = rules.mediaKind(d.url, header("content-type"), Number.isFinite(length) ? length : -1);
    if (!kind) return;
    mediaOf(d.tabId).then((list) =>
      setMedia(d.tabId, rules.addMedia(list, {
        url: d.url, kind, mime: header("content-type"), size: length,
        // The frame that asked for it (players are often embedded from another site): stream
        // servers check this, so Lanes sends it as the referrer.
        referrer: d.initiator && d.initiator !== "null" ? d.initiator + "/" : undefined,
      })),
    );
  },
  { urls: ["<all_urls>"], types: ["media", "xmlhttprequest", "other"] },
  ["responseHeaders"],
);

chrome.tabs.onUpdated.addListener((tabId, change) => {
  if (change.url) setMedia(tabId, []); // a new page
});
chrome.tabs.onRemoved.addListener((tabId) => chrome.storage.session.remove(mediaKey(tabId)));

// --- sending pages and media ---

// The video on a page: Lanes tries the page itself (video sites, through yt-dlp) and gets the
// media the page loaded along with it, which its video window offers when the page cannot be
// read. If Lanes takes no pages at all, the media are sent directly.
async function downloadVideo(tab, srcUrl) {
  const common = {
    pageUrl: tab.url,
    referrer: tab.url,
    title: tab.title,
    userAgent: navigator.userAgent,
    cookies: await cookiesFor(tab.url),
  };
  let items = (await mediaOf(tab.id)).map((m) => ({
    url: m.url, mime: m.mime, size: m.size, referrer: m.referrer,
  }));
  if (srcUrl && rules.isDownloadableUrl(srcUrl) && !items.some((m) => m.url === srcUrl)) {
    items = [{ url: srcUrl }, ...items];
  }
  const page = await send({ type: "video_page", ...common, items });
  if (page.result === "accepted") return page;
  if (items.length === 0) return page; // nothing to send: show why the page did not work
  return send({ type: "capture_media", items, ...common });
}

async function downloadLink(url, tab) {
  return send({
    type: "capture_download",
    url,
    referrer: tab?.url,
    userAgent: navigator.userAgent,
    cookies: await cookiesFor(url),
  });
}

async function downloadAllLinks(tab) {
  const links = await chrome.tabs.sendMessage(tab.id, { type: "collect_links" }).catch(() => []);
  if (!links || links.length === 0) return { result: "rejected", message: "No links on this page." };
  return send({
    type: "capture_media",
    items: links.map((url) => ({ url })),
    pageUrl: tab.url,
    referrer: tab.url,
    userAgent: navigator.userAgent,
    cookies: await cookiesFor(tab.url),
  });
}

// --- context menus ---

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({ id: "link", title: "Download with Lanes", contexts: ["link"] });
    chrome.contextMenus.create({ id: "all-links", title: "Download all links with Lanes", contexts: ["page"] });
    chrome.contextMenus.create({
      id: "video",
      title: "Download video from this page",
      contexts: ["page", "video", "audio"],
    });
  });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId === "link" && info.linkUrl) downloadLink(info.linkUrl, tab);
  if (info.menuItemId === "all-links") downloadAllLinks(tab);
  if (info.menuItemId === "video") downloadVideo(tab, info.srcUrl);
});

// --- messages from content.js and the popup ---

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  const tab = sender.tab;
  switch (msg?.type) {
    case "bypass": // Alt held on click: the browser downloads normally
      modifier.bypassUntil = Date.now() + 3000;
      return false;
    case "force": // Insert held on click: Lanes takes it whatever the type
      modifier.forceUntil = Date.now() + 3000;
      return false;
    case "video":
      downloadVideo(tab, msg.srcUrl).then(sendResponse);
      return true;
    case "popup_media":
      mediaOf(msg.tabId).then(sendResponse);
      return true;
    case "popup_send_media":
      chrome.tabs.get(msg.tabId).then(async (t) =>
        sendResponse(await send({
          type: "capture_media",
          items: msg.items,
          pageUrl: t.url,
          referrer: t.url,
          userAgent: navigator.userAgent,
          cookies: await cookiesFor(t.url),
        })),
      );
      return true;
    case "popup_video":
      chrome.tabs.get(msg.tabId).then((t) => downloadVideo(t)).then(sendResponse);
      return true;
    case "ping":
    case "open":
      send({ type: msg.type }).then(sendResponse);
      return true;
  }
  return false;
});
