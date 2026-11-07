/*
 * Lara Translate connector using the plain JSON endpoint.
 *
 * This is intentionally separate from the segmented connector: the plain
 * endpoint accepts a single text block and returns one translation, which is
 * useful for short queries and as a fallback when segmentation fails.
 */

var LARA_PLAIN_ENDPOINT = "https://webapi.laratranslate.com/translate";

function laraPlainCode(value, fallback) {
    var code = String(value == null ? "" : value).trim().toLowerCase();
    if (!code || code === "auto")
        return fallback || "";
    if (code === "zh-cn" || code === "zh-tw")
        return "zh";
    return code.split("-")[0];
}

function execute(capability, params) {
    if (capability !== "translate")
        return { error: "Lara Translate: поддерживается только перевод" };
    var text = String(params.text || params.word || "");
    if (!text.trim())
        return { error: "Lara Translate: пустой текст" };

    var source = laraPlainCode(params.from, "");
    var target = laraPlainCode(params.to, "ru") || "ru";
    var body = JSON.stringify({
        q: text,
        source: source,
        target: target,
        instructions: []
    });

    var response = http.post(LARA_PLAIN_ENDPOINT, body, {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "Origin": "https://app.laratranslate.com",
        "Referer": "https://app.laratranslate.com/",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    });
    if (response.error)
        return { error: "Lara Translate: " + response.error };
    if (response.status !== 200)
        return { error: "Lara Translate: HTTP " + response.status };

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Lara Translate: некорректный ответ сервера" };
    }

    var content = data && data.content ? data.content : data;
    var translated = content && content.translation;
    if (!translated)
        return { error: "Lara Translate: перевод не получен" };

    return {
        text: String(translated),
        detected_lang: (content && content.source_language) || source || "auto"
    };
}
