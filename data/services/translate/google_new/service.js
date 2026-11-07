/*
 * Google web translation connector.
 *
 * Uses the public translate_a/single endpoint.  The endpoint returns a
 * compact JSON array: data[0] contains translated segments, data[2] the
 * detected source language and data[1] optional dictionary entries.
 */

function googleDomain() {
    var value = String(settings.PreferredDomain || settings.GoogleDomain || "com").trim();
    value = value.replace(/^https?:\/\//i, "").replace(/^translate\.google\./i, "");
    value = value.replace(/\/.*$/, "").replace(/^\.+|\.+$/g, "");
    return value || "com";
}

function googleCode(value, fallback) {
    var code = String(value == null ? "" : value).trim();
    if (!code || code === "auto")
        return fallback || "auto";
    if (code === "zh" || code === "zh-cn")
        return "zh-CN";
    if (code === "zh-tw")
        return "zh-TW";
    return code;
}

function googleHost() {
    // translate.googleapis.com is the stable endpoint used by the desktop
    // connector.  A country-specific translate.google.<domain> host is only
    // used when the user explicitly selects a non-default domain.
    var domain = googleDomain();
    if (!domain || domain === "com")
        return "https://translate.googleapis.com";
    return "https://translate.google." + domain;
}

function joinSegments(rows) {
    var out = "";
    if (!rows)
        return out;
    for (var i = 0; i < rows.length; ++i) {
        if (rows[i] && rows[i][0])
            out += rows[i][0];
    }
    return out;
}

function dictionaryHtml(entries) {
    if (!entries || !entries.length)
        return "";
    var html = "";
    for (var i = 0; i < entries.length; ++i) {
        var entry = entries[i];
        if (!entry || !entry[0])
            continue;
        html += "<div class=\"dict-entry\"><div class=\"dict-pos\">" + entry[0] + "</div><ol class=\"dict-defs\">";
        var defs = entry[2] || [];
        for (var j = 0; j < defs.length; ++j) {
            if (defs[j] && defs[j][0])
                html += "<li>" + defs[j][0] + "</li>";
        }
        html += "</ol></div>";
    }
    return html;
}

function execute(capability, params) {
    if (capability !== "translate" && capability !== "detect")
        return { error: "Google New: неподдерживаемая возможность " + capability };

    var text = String(params.text || params.word || "");
    if (!text.trim())
        return { error: "Google New: пустой текст" };

    var from = capability === "detect" ? "auto" : googleCode(params.from, "auto");
    var to = capability === "detect" ? "en" : googleCode(params.to, "ru");
    var query = "/translate_a/single?client=gtx&dt=t&dt=bd&ie=UTF-8&oe=UTF-8"
        + "&sl=" + encodeURIComponent(from)
        + "&tl=" + encodeURIComponent(to)
        + "&q=" + encodeURIComponent(text);
    var urls = [googleHost() + query];
    var fallback = "https://translate.googleapis.com" + query;
    if (urls[0] !== fallback)
        urls.push(fallback);

    var response = null;
    for (var attempt = 0; attempt < urls.length; ++attempt) {
        response = http.get(urls[attempt], {
            "Accept": "application/json,text/plain,*/*",
            "Accept-Language": "en-US,en;q=0.9"
        });
        if (!response.error && response.status === 200)
            break;
    }
    if (!response || response.error)
        return { error: "Google New: " + ((response && response.error) || "нет ответа") };
    if (response.status !== 200)
        return { error: "Google New: HTTP " + response.status };

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Google New: некорректный ответ сервера" };
    }
    if (!data || !data[0])
        return { error: "Google New: пустой ответ сервера" };

    var translated = joinSegments(data[0]);
    if (!translated)
        return { error: "Google New: перевод не получен" };

    var result = {
        text: translated,
        detected_lang: data[2] || params.detected_from || from
    };
    var html = dictionaryHtml(data[1]);
    if (html)
        result.html = html;
    return result;
}
