(function (){
    // Runs in the main world: literals, not `new Array`/`new Object`,
    // so a page that reassigns those globals cannot shape the result.
    var links = [];
    var it = document.evaluate('/html/head/link', document, null, XPathResult.ANY_TYPE, null);
    var link = it.iterateNext();
    while (link) {
        var obj = {};
        obj.rel = link.rel;
        obj.type = link.type;
        obj.href = link.href;
        obj.title = link.title;
        links.push(obj);
        link = it.iterateNext();
    }
    return links;
})();