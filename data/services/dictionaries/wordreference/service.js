/*
 * WordReference dictionary connector.
 *
 * WordReference uses language-pair paths such as /enru/word.  The article
 * is returned from the articleWRD block and is trimmed to the dictionary
 * section so forum links and page chrome are not shown.
 */

var WORDREFERENCE_HOST = "https://www.wordreference.com";

function wordReferenceCode(value) {
    var code = String(value == null ? "" : value).toLowerCase();
    if (!code || code === "auto")
        return "";
    if (code === "el" || code === "gre")
        return "gr";
    if (code === "cs")
        return "cz";
    if (code === "zh-cn" || code === "zh-tw")
        return "zh";
    return code.split("-")[0];
}

function absoluteLinks(html, base) {
    return String(html || "").replace(/(href|src)\s*=\s*"([^"]*)"/gi, function (all, attr, url) {
        if (/^(https?:|mailto:|#|\/\/)/i.test(url))
            return attr + '="' + url + '"';
        if (url.charAt(0) === "/")
            return attr + '="' + base + url + '"';
        return attr + '="' + base + "/" + url + '"';
    });
}

function cleanArticle(html, base) {
    var out = String(html || "");
    out = out.replace(/<script[\s\S]*?<\/script>/gi, "")
        .replace(/<style[\s\S]*?<\/style>/gi, "")
        .replace(/<svg[\s\S]*?<\/svg>/gi, "")
        .replace(/<button[\s\S]*?<\/button>/gi, "")
        .replace(/\s(?:on\w+|data-\w+)\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)/gi, "")
        .replace(/\s(?:id|name)\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)/gi, "");
    return absoluteLinks(out, base);
}

function execute(capability, params) {
    if (capability !== "dictionary")
        return { error: "WordReference: поддерживаются только словарные статьи" };
    var word = String(params.word || params.text || "").trim();
    if (!word)
        return { error: "WordReference: пустое слово" };

    var from = wordReferenceCode(params.from) || "en";
    var to = wordReferenceCode(params.to) || "ru";
    var url = WORDREFERENCE_HOST + "/" + from + to + "/" + encodeURIComponent(word);

    var response = http.get(url, {
        "Accept": "text/html,application/xhtml+xml",
        "Accept-Language": "en-US,en;q=0.9",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    });
    if (response.error)
        return { error: "WordReference: " + response.error };
    if (response.status !== 200)
        return { error: "WordReference: HTTP " + response.status };

    var body = String(response.body || "");
    var match = /<div id="(?:articleWRD|article)"[^>]*>/i.exec(body);
    if (!match)
        return { error: "WordReference: статья не найдена" };

    var start = match.index;
    var end = body.indexOf('<div id="postArticle">', start);
    var fragment = end > start ? body.slice(start, end) : body.slice(start);
    var html = cleanArticle(fragment, WORDREFERENCE_HOST);
    if (!html.replace(/<[^>]*>/g, "").trim())
        return { error: "WordReference: пустой ответ" };

    return {
        html: "<div class=\"dict-entry\">" + html + "</div>",
        text: word,
        title: "WordReference — " + word,
        url: url
    };
}
