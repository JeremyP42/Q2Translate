/*
 * Oxford Learner's Dictionaries connector.
 *
 * The search page redirects to the entry page and contains the article in
 * the ox-wrapper block.  Only the article column is returned.
 */

var OXFORD_HOST = "https://www.oxfordlearnersdictionaries.com";

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
        .replace(/<img[^>]*>/gi, "")
        .replace(/<button[\s\S]*?<\/button>/gi, "")
        .replace(/<div class="sound[\s\S]*?<\/div>/gi, "")
        .replace(/\s(?:on\w+|data-\w+)\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)/gi, "")
        .replace(/\s(?:id|name)\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)/gi, "");
    return absoluteLinks(out, base);
}

function execute(capability, params) {
    if (capability !== "dictionary")
        return { error: "Oxford: поддерживаются только словарные статьи" };
    var word = String(params.word || params.text || "").trim();
    if (!word)
        return { error: "Oxford: пустое слово" };

    var url = OXFORD_HOST + "/search/english/?q=" + encodeURIComponent(word);
    var response = http.get(url, {
        "Accept": "text/html,application/xhtml+xml",
        "Accept-Language": "en-US,en;q=0.9",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    });
    if (response.error)
        return { error: "Oxford: " + response.error };
    if (response.status !== 200)
        return { error: "Oxford: HTTP " + response.status };

    var body = String(response.body || "");
    var start = body.indexOf('<div id="ox-wrapper"');
    if (start < 0)
        return { error: "Oxford: статья не найдена" };

    var end = body.indexOf('<div id="rightcolumn"', start);
    var fragment = end > start ? body.slice(start, end) : body.slice(start);
    var html = cleanArticle(fragment, OXFORD_HOST);
    if (!html.replace(/<[^>]*>/g, "").trim())
        return { error: "Oxford: пустой ответ" };

    return {
        html: "<div class=\"dict-entry\">" + html + "</div>",
        text: word,
        title: "Oxford — " + word,
        url: url
    };
}
