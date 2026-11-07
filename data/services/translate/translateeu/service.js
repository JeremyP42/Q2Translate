/*
 * translate.eu web translator.
 *
 * The public page uses a LibreTranslate-compatible form endpoint and
 * returns a JSON object with the translated string in the "tr" field.
 */

var TRANSLATE_EU_ENDPOINT = "https://www.translate.eu/libre/";

function translateEuCode(value) {
    var code = String(value == null ? "" : value).trim().toLowerCase();
    if (!code || code === "auto")
        return "";
    if (code === "zh" || code === "zh-cn")
        return "zh";
    if (code === "zh-tw")
        return "zt";
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

function cleanText(value) {
    return decodeEntities(String(value == null ? "" : value).replace(/<[^>]*>/g, " "))
        .replace(/\s+/g, " ")
        .trim();
}

function execute(capability, params) {
    if (capability !== "translate")
        return { error: "TranslateEU: поддерживается только перевод" };
    var text = String(params.text || params.word || "").trim();
    if (!text)
        return { error: "TranslateEU: пустой текст" };

    var from = translateEuCode(params.from);
    var to = translateEuCode(params.to) || "en";
    var body = "txt=" + encodeURIComponent(text.slice(0, 2900))
        + "&from=" + encodeURIComponent(from)
        + "&to=" + encodeURIComponent(to);

    var response = http.post(TRANSLATE_EU_ENDPOINT, body, {
        "Accept": "message/x-jl-formresult,application/json",
        "Content-Type": "application/x-www-form-urlencoded",
        "Origin": "https://www.translate.eu",
        "Referer": "https://www.translate.eu/"
    });
    if (response.error)
        return { error: "TranslateEU: " + response.error };
    if (response.status !== 200)
        return { error: "TranslateEU: HTTP " + response.status };

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "TranslateEU: некорректный ответ сервера" };
    }
    if (!data || !data.tr)
        return { error: "TranslateEU: перевод не получен" };

    return {
        text: cleanText(data.tr),
        detected_lang: params.detected_from || from || "auto"
    };
}
