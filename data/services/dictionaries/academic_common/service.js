/*
 * Shared Academic.ru dictionary connector.
 *
 * The two manifests differ only by host and URL layout.  The manifest is
 * loaded with loadJSON() so both connectors can use this implementation.
 */

var manifest = loadJSON("manifest.json");
var host = String(manifest.host || "https://dic.academic.ru");
var label = String(manifest.name || "Academic");
var mode = String(manifest.mode || "search");

function languageCode(value) {
    var code = String(value == null ? "" : value).trim().toLowerCase();
    if (!code || code === "auto")
        return "en";
    return code.split("-")[0].split("_")[0];
}

function qualifyLinks(html) {
    return String(html).replace(/(href|src)="\/([^"]*)"/gi, function (match, attribute, path) {
        return attribute + '="' + host + '/' + path + '"';
    });
}

function decorate(html) {
    var page = qualifyLinks(html);
    var style = "<style>"
        + "#header,#footer,#ad_top{display:none!important}"
        + "#content{margin:0!important}"
        + "#page{padding:0!important;width:100%!important}"
        + "</style>";
    if (/<\/head>/i.test(page))
        return page.replace(/<\/head>/i, style + "</head>");
    return style + page;
}

function plainText(html) {
    return String(html)
        .replace(/<script[\s\S]*?<\/script>/gi, " ")
        .replace(/<style[\s\S]*?<\/style>/gi, " ")
        .replace(/<[^>]+>/g, " ")
        .replace(/&nbsp;/g, " ")
        .replace(/&amp;/g, "&")
        .replace(/\s+/g, " ")
        .trim();
}

function execute(capability, params) {
    if (capability !== "dictionary")
        return { error: label + ": поддерживается только поиск по словарям." };

    var word = String(params.word || params.text || "").trim();
    if (!word)
        return { error: label + ": не задано слово для поиска." };

    var from = languageCode(params.from || params.to);
    var url = mode === "path"
        ? host + "/" + encodeURIComponent(word) + "/" + encodeURIComponent(from) + "/ru/"
        : host + "/searchall.php?from=" + encodeURIComponent(from)
            + "&to=xx&did=&stype=&SWord=" + encodeURIComponent(word);

    var response = http.get(url, {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                      + "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    });
    if (response.error)
        return { error: label + ": " + response.error };

    var body = String(response.body == null ? "" : response.body);
    if (!body)
        return { error: label + ": пустой ответ сервера." };

    var text = plainText(body);
    if (!text)
        return { error: label + ": ничего не найдено." };

    return { html: decorate(body), text: text };
}
