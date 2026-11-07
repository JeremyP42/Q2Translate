/*
 * Sogou web translator.
 *
 * The page embeds its state in window.__INITIAL_STATE__.  The result is
 * available both as plain text and as a small dictionary entry.
 */

var SOGOU_HOST = "https://fanyi.sogou.com";
var SOGOU_PAGE = SOGOU_HOST + "/text";
var SOGOU_CHUNK = 1200;

function sogouCode(value, fallback) {
    var code = String(value == null ? "" : value).trim().toLowerCase();
    if (!code || code === "auto")
        return fallback || "auto";
    if (code === "zh" || code === "zh-cn" || code === "zh-chs")
        return "zh-CHS";
    if (code === "zh-tw" || code === "zh-cht")
        return "zh-CHT";
    return code.split("-")[0];
}

function decodeEntities(value) {
    return String(value == null ? "" : value)
        .replace(/&nbsp;/gi, " ")
        .replace(/&amp;/gi, "&")
        .replace(/&lt;/gi, "<")
        .replace(/&gt;/gi, ">")
        .replace(/&quot;/gi, "\"")
        .replace(/&#39;/gi, "'")
        .replace(/&#x([0-9a-f]+);/gi, function (_, hex) {
            return String.fromCharCode(parseInt(hex, 16));
        })
        .replace(/&#(\d+);/g, function (_, dec) {
            return String.fromCharCode(parseInt(dec, 10));
        });
}

function plainText(value) {
    return decodeEntities(String(value == null ? "" : value).replace(/<[^>]*>/g, " "))
        .replace(/\s+/g, " ")
        .trim();
}

function splitText(text, limit) {
    var rest = String(text || "").replace(/\r\n/g, "\n");
    var chunks = [];
    while (rest.length > limit) {
        var cut = rest.lastIndexOf("\n", limit);
        if (cut < limit / 2)
            cut = Math.max(rest.lastIndexOf(". ", limit), rest.lastIndexOf("! ", limit), rest.lastIndexOf("? ", limit));
        if (cut < limit / 2)
            cut = rest.lastIndexOf(" ", limit);
        if (cut < limit / 2)
            cut = limit;
        chunks.push(rest.slice(0, cut + 1).trim());
        rest = rest.slice(cut + 1).trim();
    }
    if (rest)
        chunks.push(rest);
    return chunks;
}

function readState(html) {
    var marker = "window.__INITIAL_STATE__=";
    var start = String(html || "").indexOf(marker);
    if (start < 0)
        return null;
    start += marker.length;
    var end = html.indexOf(";(function()", start);
    if (end < 0)
        end = html.indexOf("</script>", start);
    if (end < 0)
        return null;
    try {
        return json.parse(html.slice(start, end).trim());
    } catch (error) {
        return null;
    }
}

function stateText(state) {
    if (!state || !state.textTranslate)
        return "";
    var data = state.textTranslate.translateData;
    var entry = data && data.translate;
    var value = entry && (entry.dit || entry.text);
    if (!value)
        value = state.textTranslate.result;
    return plainText(value || "");
}

function stateLanguage(state) {
    if (!state)
        return "";
    if (state.language && state.language.detect)
        return String(state.language.detect);
    var data = state.textTranslate && state.textTranslate.translateData;
    if (data && data.detect && data.detect.detect)
        return String(data.detect.detect);
    if (data && data.translate && data.translate.from)
        return String(data.translate.from);
    return "";
}

function requestChunk(chunk, from, to) {
    var url = SOGOU_PAGE
        + "?keyword=" + encodeURIComponent(chunk)
        + "&model=general&transfrom=" + encodeURIComponent(from)
        + "&transto=" + encodeURIComponent(to);
    var response = http.get(url, {
        "Accept": "text/html,application/xhtml+xml",
        "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
        "Referer": SOGOU_PAGE,
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    });
    if (response.error)
        return { error: "Sogou: " + response.error };
    if (response.status !== 200)
        return { error: "Sogou: HTTP " + response.status };
    var state = readState(response.body);
    if (!state)
        return { error: "Sogou: сервер вернул страницу без результата" };
    var text = stateText(state);
    if (!text)
        return { error: "Sogou: перевод не получен" };
    return { text: text, detected_lang: sogouCode(stateLanguage(state), from) };
}

function execute(capability, params) {
    if (capability !== "translate")
        return { error: "Sogou: поддерживается только перевод" };
    var text = String(params.text || params.word || "").trim();
    if (!text)
        return { error: "Sogou: пустой текст" };

    var from = sogouCode(params.from, "auto");
    var to = sogouCode(params.to, "ru");
    var chunks = splitText(text, SOGOU_CHUNK);
    var translated = [];
    var detected = "";

    for (var i = 0; i < chunks.length; ++i) {
        var result = requestChunk(chunks[i], from, to);
        if (result.error)
            return result;
        translated.push(result.text);
        if (!detected)
            detected = result.detected_lang;
    }

    return {
        text: translated.join("\n"),
        detected_lang: detected || from
    };
}
