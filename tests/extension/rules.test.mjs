// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the extension's decision logic (run by ctest when Node is installed: node --test).
import assert from "node:assert/strict";
import { test } from "node:test";

import * as rules from "../../extension/lib/rules.js";

const item = (over) => ({ url: "https://example.com/a.zip", filename: "", mime: "", size: -1, referrer: "", ...over });

test("downloads of listed file types go to Lanes", () => {
  assert.equal(rules.shouldCapture(item(), {}), true);
  assert.equal(rules.shouldCapture(item({ url: "https://e.com/report", filename: "C:\\D\\Report.PDF" }), {}), true);
  assert.equal(rules.shouldCapture(item({ url: "https://e.com/get?id=1", mime: "application/x-msdownload" }), {}), true);
  assert.equal(rules.shouldCapture(item({ url: "https://e.com/clip", mime: "video/mp4; codecs=avc1" }), {}), true);
  assert.equal(rules.shouldCapture(item({ url: "https://e.com/page.html", mime: "text/html" }), {}), false);
});

test("blob, data and file downloads stay in the browser", () => {
  assert.equal(rules.shouldCapture(item({ url: "blob:https://e.com/1234" }), { captureAll: true }), false);
  assert.equal(rules.shouldCapture(item({ url: "data:text/plain,hi" }), { captureAll: true }), false);
  assert.equal(rules.shouldCapture(item({ url: "file:///C:/a.zip" }), {}), false);
});

test("settings: off switch, minimum size, excluded sites, everything", () => {
  assert.equal(rules.shouldCapture(item(), { enabled: false }), false);
  assert.equal(rules.shouldCapture(item({ size: 1000 }), { minSizeKiB: 2 }), false);
  assert.equal(rules.shouldCapture(item({ size: 4096 }), { minSizeKiB: 2 }), true);
  assert.equal(rules.shouldCapture(item({ size: -1 }), { minSizeKiB: 2 }), true); // unknown size
  assert.equal(rules.shouldCapture(item({ url: "https://dl.example.com/a.zip" }), { excludedSites: "example.com" }), false);
  assert.equal(rules.shouldCapture(item({ referrer: "https://intranet.corp.local/x" }), { excludedSites: "*.corp.local" }), false);
  assert.equal(rules.shouldCapture(item({ url: "https://e.com/page.html" }), { captureAll: true }), true);
  assert.equal(rules.shouldCapture(item({ url: "https://e.com/x.custom" }), { extensions: "custom, .other" }), true);
});

test("site patterns", () => {
  assert.equal(rules.hostMatches("example.com", "example.com"), true);
  assert.equal(rules.hostMatches("a.example.com", "example.com"), true);
  assert.equal(rules.hostMatches("badexample.com", "example.com"), false);
  assert.equal(rules.hostMatches("example.com", "*.example.com"), false);
  assert.equal(rules.hostMatches("", "example.com"), false);
});

test("media detection keeps playlists and whole files, not segments", () => {
  assert.equal(rules.mediaKind("https://cdn/x/master.m3u8?t=1", "", -1), "hls");
  assert.equal(rules.mediaKind("https://cdn/x/play", "application/vnd.apple.mpegurl", 500), "hls");
  assert.equal(rules.mediaKind("https://cdn/x/manifest.mpd", "", -1), "dash");
  assert.equal(rules.mediaKind("https://cdn/v.mp4", "video/mp4", 50 << 20), "video");
  assert.equal(rules.mediaKind("https://cdn/a.mp3", "audio/mpeg", -1), "audio");
  assert.equal(rules.mediaKind("https://cdn/seg-001.ts", "video/mp2t", 900000), null);
  assert.equal(rules.mediaKind("https://cdn/chunk-1.m4s", "video/mp4", 900000), null);
  assert.equal(rules.mediaKind("https://cdn/v.mp4?range=0-1000", "video/mp4", 1001), null);
  assert.equal(rules.mediaKind("https://cdn/tiny.mp4", "video/mp4", 1000), null);
  assert.equal(rules.mediaKind("https://cdn/page", "text/html", 5000), null);
});

test("cookies, form bodies, names and media lists", () => {
  assert.equal(rules.cookieHeader([{ name: "a", value: "1" }, { name: "b", value: "x y" }]), "a=1; b=x y");
  assert.equal(rules.formEncode({ q: ["a b"], n: ["1", "2"] }), "q=a%20b&n=1&n=2");
  assert.equal(rules.basename("C:\\Users\\me\\Downloads\\a.zip"), "a.zip");
  assert.equal(rules.filenameFromUrl("https://e.com/files/r%C3%A9sum%C3%A9.pdf?x=1"), "résumé.pdf");
  let list = rules.addMedia([], { url: "a" });
  list = rules.addMedia(list, { url: "b" });
  list = rules.addMedia(list, { url: "a" }, 2);
  assert.deepEqual(list.map((m) => m.url), ["a", "b"]);
});
