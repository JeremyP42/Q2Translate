/*
 * PROMT.One web translator.
 *
 * The service needs a browser session cookie and XSRF token, both of which
 * are configured in the connector settings.  The request also carries a
 * small integrity hash derived from the text and the language pair.
 */

var PROMT_ENDPOINT = "https://www.online-translator.com/api/getTranslation";

function promtCode(value) {
    var code = String(value == null ? "" : value).trim().toLowerCase();
    if (!code || code === "auto")
        return "auto";
    if (code === "zh" || code === "zh-cn" || code === "zh-tw")
        return "zhcn";
    if (code === "no")
        return "nb";
    return code.split("-")[0];
}

function promtHash(value) {
    var hash = 0;
    var text = String(value == null ? "" : value);
    for (var i = 0; i < text.length; ++i) {
        hash = ((hash << 5) - hash + text.charCodeAt(i)) | 0;
    }
    return hash;
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
        .replace(/[ \t]+/g, " ")
        .replace(/\n{3,}/g, "\n\n")
        .trim();
}

function execute(capability, params) {
    if (capability !== "translate")
        return { error: "Promt: поддерживается только перевод" };
    var text = String(params.text || params.word || "").trim();
    if (!text)
        return { error: "Promt: пустой текст" };

    var from = promtCode(params.from);
    var to = promtCode(params.to) || "ru";
    var first = promtHash(text);
    var hash = promtHash("TranslateButton#" + from + "-" + to + "#General#" + first);
    var body = "eventName=TranslateButton"
        + "&text=" + encodeURIComponent(text.slice(0, 1000))
        + "&dirCode=" + encodeURIComponent(from + "-" + to)
        + "&useAutoDetect=true"
        + "&h=" + encodeURIComponent(hash)
        + "&aft=" + encodeURIComponent(String(settings.PromtPaft || ""))
        + "&pageIx=0&v=1";

    var headers = {
        "Accept": "application/json",
        "Content-Type": "application/x-www-form-urlencoded",
        "Origin": "https://www.online-translator.com",
        "Referer": "https://www.online-translator.com/",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    };
    if (settings.PromtCookie)
        headers.Cookie = String(settings.PromtCookie);
    if (settings.PromtXsrf)
        headers["XSRF-TOKEN"] = String(settings.PromtXsrf);

    var response = http.post(PROMT_ENDPOINT, body, headers);
    if (response.error)
        return { error: "Promt: " + response.error };
    if (response.status !== 200)
        return { error: "Promt: HTTP " + response.status + " (проверьте cookie и XSRF в настройках)" };

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Promt: некорректный ответ сервера" };
    }
    if (!data || data.error)
        return { error: "Promt: " + ((data && data.error) || "перевод не получен") };

    var result = String(data.text || "").trim();
    if (data.dictHtml)
        result += "\n\n" + plainText(data.dictHtml);
    if (data.phrasesHtml)
        result += "\n\n" + plainText(data.phrasesHtml);
    if (!result)
        return { error: "Promt: пустой перевод" };

    return {
        text: result,
        detected_lang: promtCode(data.from || from)
    };
}
