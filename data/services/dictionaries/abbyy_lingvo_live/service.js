/*
 * ABBYY Lingvo Live dictionary connector.
 *
 * Fetches the public dictionary page and returns the article fragment as
 * HTML.  The page is a React application, so the connector extracts the
 * server-rendered article block and removes interactive chrome.
 */

var LINGVO_HOST = "https://www.lingvolive.com";

function lingvoUiLanguage(value) {
    var code = String(value == null ? "" : value).toLowerCase();
    if (code.indexOf("ru") === 0)
        return "ru-ru";
    if (code.indexOf("es") === 0)
        return "es-mx";
    if (code.indexOf("pt") === 0)
        return "pt-br";
    return "en-us";
}

function lingvoSource(params) {
    var from = lingvoCode(params.from);
    if (!from)
        from = lingvoCode(params.detected_from);
    return from || "en";
}

function lingvoCode(value) {
    var code = String(value == null ? "" : value).toLowerCase();
    if (!code || code === "auto")
        return "";
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
        .replace(/\s(?:on\w+|data-reactid|data-react-checksum)\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)/gi, "")
        .replace(/\s(?:id|name)\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)/gi, "");
    return absoluteLinks(out, base);
}

function execute(capability, params) {
    if (capability !== "dictionary")
        return { error: "ABBYY Lingvo Live: поддерживаются только словарные статьи" };
    var word = String(params.word || params.text || "").trim();
    if (!word)
        return { error: "ABBYY Lingvo Live: пустое слово" };

    var from = lingvoSource(params);
    var to = lingvoCode(params.to) || "ru";
    var ui = lingvoUiLanguage(to);
    var url = LINGVO_HOST + "/" + ui + "/translate/" + from + "-" + to + "/" + encodeURIComponent(word);

    var response = http.get(url, {
        "Accept": "text/html,application/xhtml+xml",
        "Accept-Language": "en-US,en;q=0.9,ru;q=0.8",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    });
    if (response.error)
        return { error: "ABBYY Lingvo Live: " + response.error };
    if (response.status !== 200)
        return { error: "ABBYY Lingvo Live: HTTP " + response.status };

    var body = String(response.body || "");
    var start = body.search(/<div name="#(?:dictionary|user|quote|phrase|wordform)"/);
    if (start < 0)
        return { error: "ABBYY Lingvo Live: статья не найдена" };

    var fragment = body.slice(start);
    var end = fragment.search(/<footer|<div class="HLjpm"/);
    if (end > 0)
        fragment = fragment.slice(0, end);

    var html = cleanArticle(fragment, LINGVO_HOST);
    if (!html.replace(/<[^>]*>/g, "").trim())
        return { error: "ABBYY Lingvo Live: пустой ответ" };

    return {
        html: "<div class=\"dict-entry\">" + html + "</div>",
        text: word,
        title: "ABBYY Lingvo Live — " + word,
        url: url
    };
}
