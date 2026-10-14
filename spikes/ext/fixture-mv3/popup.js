// Fixture popup: one sendMessage round-trip on load; the reply (or
// error) lands in the title and on window for the harness to read.
chrome.runtime.sendMessage({ ping: true, via: 'popup' }, (r) => {
  const err = chrome.runtime.lastError;
  if (err) {
    window.__popupReply = { error: String(err.message || err) };
    document.title = 'popup:error';
    return;
  }
  window.__popupReply = r || null;
  document.title = 'popup:' + (r && r.from);
});
