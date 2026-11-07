/*
 * Yandex Translate connector.
 *
 * The web API accepts a short-lived client UUID in the query string and
 * returns JSON with the translated text and the detected language pair.
 */

var YANDEX_API = "https://translate.yandex.net/api/v1/tr.json";
var YANDEX_REFERER = "https://translate.yandex.com/";

function yandexUuid() {
    var out = "";
    for (var i = 0; i < 32; ++i)
        out += (Math.random() * 16 | 0).toString(16);
    return out;
}

function yandexCode(value, fallback) {
    var code = String(value == null ? "" : value).trim().toLowerCase();
    if (!code || code === "auto")
        return fallback || "auto";
    if (code === "zh-cn" || code === "zh-tw")
        return "zh";
    return code.split("-")[0];
}

function yandexHeaders() {
    return {
        "Accept": "application/json",
        "Content-Type": "application/x-www-form-urlencoded",
        "Origin": "https://translate.yandex.com",
        "Referer": YANDEX_REFERER,
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    };
}

function yandexDetect(text) {
    var url = YANDEX_API + "/detect?uuid=" + encodeURIComponent(yandexUuid())
        + "&srv=android&text=" + encodeURIComponent(String(text || "").slice(0, 256));
    var response = http.get(url, yandexHeaders());
    if (response.error)
        return { error: "Yandex: " + response.error };
    if (response.status !== 200)
        return { error: "Yandex: HTTP " + response.status };
    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Yandex: некорректный ответ сервера" };
    }
    if (!data || !data.lang)
        return { error: "Yandex: язык не определён" };
    return { detected_lang: yandexCode(data.lang) };
}

function yandexTranslate(text, from, to) {
    var source = yandexCode(from, "auto");
    var target = yandexCode(to, "ru");
    var url = YANDEX_API + "/translate?uuid=" + encodeURIComponent(yandexUuid())
        + "&srv=android&lang=" + encodeURIComponent(source + "-" + target)
        + "&reason=auto&format=text&yu=2210680511641235828";
    var body = "text=" + encodeURIComponent(text);
    var response = http.post(url, body, yandexHeaders());
    if (response.error)
        return { error: "Yandex: " + response.error };
    if (response.status !== 200)
        return { error: "Yandex: HTTP " + response.status };

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Yandex: некорректный ответ сервера" };
    }
    if (!data || Number(data.code) !== 200)
        return { error: "Yandex: " + ((data && data.message) || "перевод не получен") };

    var translated = "";
    if (data.text && data.text.join) {
        translated = data.text.join("\n");
    } else if (typeof data.text === "string") {
        translated = data.text;
    }
    if (!translated)
        return { error: "Yandex: пустой перевод" };

    var pair = String(data.lang || "").split("-");
    return {
        text: translated,
        detected_lang: pair[0] || source
    };
}

function execute(capability, params) {
    var text = String(params.text || params.word || "");
    if (!text.trim())
        return { error: "Yandex: пустой текст" };

    if (capability === "detect")
        return yandexDetect(text);
    if (capability === "translate")
        return yandexTranslate(text, params.from, params.to);
    return { error: "Yandex: неподдерживаемая возможность " + capability };
}
