var ENDPOINT = "https://webapi.laratranslate.com/translate/segmented";

function providerCode(value) {
    var code = String(value || "").toLowerCase();
    if (code === "auto" || !code) return "";
    if (code === "zh-cn" || code === "zh-tw") return "zh";
    return code.split("-")[0];
}

function execute(capability, params) {
    if (capability !== "translate")
        return { error: "LaraTranslate: поддерживается только перевод." };
    var text = String(params.text || params.word || "");
    if (!text)
        return { error: "Пустой текст." };

    var from = providerCode(params.from);
    var to = providerCode(params.to) || "ru";
    var body = JSON.stringify({
        q: text,
        source: from,
        target: to,
        instructions: [],
        style: "faithful",
        source_hint: "",
        adapt_to: [],
        glossaries: [],
        content_type: "text/plain"
    });

    var response = http.post(ENDPOINT, body, {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "Origin": "https://app.laratranslate.com",
        "Referer": "https://app.laratranslate.com/",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    });
    if (response.error)
        return { error: "LaraTranslate: " + response.error };
    if (response.status !== 200)
        return { error: "LaraTranslate: HTTP " + response.status };

    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "LaraTranslate: не удалось разобрать JSON." }; }

    var content = data && data.content ? data.content : data;
    var translated = content && content.translation ? content.translation : "";
    if (!translated && content && content.translations) {
        for (var i = 0; i < content.translations.length; ++i) {
            if (content.translations[i] && content.translations[i].translation)
                translated += content.translations[i].translation;
        }
    }
    if (!translated)
        return { error: "LaraTranslate: пустой ответ." };

    var detected = content && content.source_language ? content.source_language : from;
    return { text: String(translated), detected_lang: detected };
}
