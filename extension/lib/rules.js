// SPDX-License-Identifier: GPL-3.0-or-later
// Decisions of the extension that need no browser APIs, so they can be tested with Node
// (extension/test). Everything here is a pure function.

export const DEFAULT_SETTINGS = {
  enabled: true,
  // File types handed to Lanes (space or comma separated, without dots).
  extensions:
    "zip rar 7z gz bz2 xz zst tar tgz iso img exe msi msix appx dmg pkg apk deb rpm bin " +
    "pdf epub doc docx xls xlsx ppt pptx odt " +
    "mp3 m4a aac flac wav ogg opus wma " +
    "mp4 m4v mkv webm avi mov wmv flv mpg mpeg 3gp",
  minSizeKiB: 0,
  // Sites whose downloads stay in the browser ("example.com, *.intranet.local").
  excludedSites: "",
  captureAll: false, // every download, whatever its type
  videoButton: true, // "Download with Lanes" over videos
};

const MIME_PREFIXES = ["video/", "audio/"];
const MIME_TYPES = new Set([
  "application/zip", "application/x-zip-compressed", "application/x-rar-compressed",
  "application/vnd.rar", "application/x-7z-compressed", "application/gzip", "application/x-tar",
  "application/x-msdownload", "application/x-msi", "application/x-iso9660-image",
  "application/vnd.android.package-archive", "application/x-apple-diskimage", "application/pdf",
]);

export function lower(s) {
  return (s || "").toLowerCase();
}

export function isDownloadableUrl(url) {
  return /^(https?|ftps?):\/\//i.test(url || "");
}

export function hostOf(url) {
  try {
    return new URL(url).hostname.toLowerCase();
  } catch {
    return "";
  }
}

// Last path segment of a URL, percent-decoded ("" if none).
export function filenameFromUrl(url) {
  try {
    const parts = new URL(url).pathname.split("/");
    return decodeURIComponent(parts[parts.length - 1] || "");
  } catch {
    return "";
  }
}

// File name from a path the browser reports ("C:\\Users\\me\\Downloads\\a.zip" → "a.zip").
export function basename(path) {
  const p = path || "";
  return p.substring(Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/")) + 1);
}

export function extensionOf(name) {
  const n = basename(name);
  const dot = n.lastIndexOf(".");
  return dot > 0 ? n.substring(dot + 1).toLowerCase() : "";
}

export function splitList(text) {
  return lower(text).split(/[\s,;]+/).map((s) => s.replace(/^\./, "")).filter((s) => s.length > 0);
}

// "example.com" matches example.com and its subdomains; "*.corp.local" only subdomains.
export function hostMatches(host, patterns) {
  const h = lower(host);
  if (!h) return false;
  return splitList(patterns).some((p) => {
    if (p.startsWith("*.")) return h.endsWith(p.substring(1));
    return h === p || h.endsWith("." + p);
  });
}

// Should this browser download go to Lanes? `item`: {url, filename, mime, size (-1 unknown),
// referrer}. Holding Alt while clicking bypasses this in the background script.
export function shouldCapture(item, settings) {
  const s = { ...DEFAULT_SETTINGS, ...settings };
  if (!s.enabled || !isDownloadableUrl(item.url)) return false; // blob:, data:, file: stay
  if (hostMatches(hostOf(item.url), s.excludedSites) ||
      hostMatches(hostOf(item.referrer), s.excludedSites)) {
    return false;
  }
  if (item.size >= 0 && item.size < s.minSizeKiB * 1024) return false;
  if (s.captureAll) return true;
  const ext = extensionOf(item.filename) || extensionOf(filenameFromUrl(item.url));
  if (ext && splitList(s.extensions).includes(ext)) return true;
  const mime = lower(item.mime).split(";")[0].trim();
  return MIME_TYPES.has(mime) || MIME_PREFIXES.some((p) => mime.startsWith(p));
}

// What a network response is, for the media list: "hls", "dash", "video", "audio" or null.
// Stream fragments (TS / fMP4 segments, byte ranges) and small files are left out.
export function mediaKind(url, contentType, contentLength) {
  const type = lower(contentType).split(";")[0].trim();
  const path = lower(url).split(/[?#]/)[0];
  if (type.includes("mpegurl") || path.endsWith(".m3u8")) return "hls";
  if (type === "application/dash+xml" || path.endsWith(".mpd")) return "dash";
  if (type === "video/mp2t" || type === "video/iso.segment" || /\.(ts|m4s|m4f|aac)$/.test(path)) {
    return null;
  }
  if (/[?&](range|bytes)=/.test(lower(url))) return null; // a piece of a larger file
  const known = contentLength >= 0;
  if (known && contentLength < 256 * 1024) return null;
  if (type.startsWith("video/")) return "video";
  if (type.startsWith("audio/")) return "audio";
  return null;
}

// chrome.cookies.getAll() result → "name=value; name2=value2".
export function cookieHeader(cookies) {
  return (cookies || []).map((c) => `${c.name}=${c.value}`).join("; ");
}

// webRequest requestBody.formData ({name: [values]}) → application/x-www-form-urlencoded.
export function formEncode(formData) {
  const parts = [];
  for (const [name, values] of Object.entries(formData || {})) {
    for (const v of values) {
      parts.push(`${encodeURIComponent(name)}=${encodeURIComponent(v)}`);
    }
  }
  return parts.join("&");
}

// Adds a detected media entry to a tab's list (newest first, no duplicates, at most `cap`).
export function addMedia(list, entry, cap = 100) {
  const rest = (list || []).filter((m) => m.url !== entry.url);
  return [entry, ...rest].slice(0, cap);
}
