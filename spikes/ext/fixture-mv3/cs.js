// Fixture content script: fires sendMessage at document_idle and
// records who answered on <html data-spike-cs=...> for the harness.
chrome.runtime.sendMessage({ ping: true, via: 'cs' }, (r) => {
  const err = chrome.runtime.lastError;
  const v = err ? ('err:' + (err.message || err))
                : ('reply:' + (r && r.from));
  document.documentElement.setAttribute('data-spike-cs', v);
});
