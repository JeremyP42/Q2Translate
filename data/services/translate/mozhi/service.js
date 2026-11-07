/*
 * Mozhi connector.
 *
 * Mozhi is a proxy in front of several translation engines.  Only the engine
 * selected by the user is used; there is no automatic fallback to another
 * engine or another instance.
 */

function baseUrl() {
    var instance = String(settings.instance || "https://mozhi.adminforge.de").trim();
    if (instance === "custom")
        instance = String(settings.custom_instance || "").trim();
    return instance.replace(/\/+$/, "");
}

function code(value) {
    if (value === "zh-CN") return "zh-CN";
    if (value === "zh-TW") return "zh-TW";
    return value || "auto";
}

function requestEngine(engine, from, to, text) {
    var base = baseUrl();
    if (!base)
        return { error: "Mozhi: укажите адрес self-hosted сервера" };
    var url = base + "/api/translate?engine=" + encodeURIComponent(engine)
        + "&from=" + encodeURIComponent(code(from))
        + "&to=" + encodeURIComponent(code(to))
        + "&text=" + encodeURIComponent(text);
    var response = http.get(url, { "Accept": "application/json" });
    if (response.error)
        return { error: "Mozhi: " + response.error };
    if (response.status !== 200)
        return { error: "Mozhi: " + engine + ": HTTP " + response.status };

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Mozhi: " + engine + ": некорректный JSON" };
    }

    var translated = data && (data["translated-text"] || data.translatedText);
    if (!translated)
        return { error: "Mozhi: " + engine + ": пустой перевод" };

    return {
        text: translated,
        detected_lang: data.detected || data.source_language || from
    };
}

function execute(capability, params) {
    if (capability !== "translate" && capability !== "detect")
        return { error: "Mozhi: неподдерживаемая операция " + capability };
    var text = String(params.text || "");
    if (!text)
        return { error: "Mozhi: пустой текст" };

    var from = capability === "detect" ? "auto" : (params.from || "auto");
    var to = capability === "detect" ? "en" : (params.to || "ru");
    var engine = String(settings.engine || "Yandex").trim().toLowerCase() || "yandex";
    return requestEngine(engine, from, to, text);
}
