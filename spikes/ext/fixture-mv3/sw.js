// EXT13 fixture service worker — the SAME file runs in the real
// extension SW and inside the hosted hidden page.  A SW global has no
// document; a hosted extension page does — the reply's `from` field
// attributes which copy answered a sendMessage.
const SPIKE_HERE = (typeof document === 'undefined') ? 'real-sw' : 'hosted-page';
self.__swRan = SPIKE_HERE;

chrome.runtime.onInstalled.addListener(() => {
  chrome.storage.local.set({ spikeSwInstalled: SPIKE_HERE });
});

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg || !msg.ping)
    return false;
  const reply = { pong: true, from: SPIKE_HERE, via: msg.via || '?' };
  if (SPIKE_HERE === 'real-sw') {
    // Answer late so the hosted copy (which replies synchronously) wins
    // the first-sendResponse race — deterministic attribution.
    setTimeout(() => sendResponse(reply), 250);
  } else {
    sendResponse(reply);
  }
  return true;
});
