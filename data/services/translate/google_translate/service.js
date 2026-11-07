function execute(capability, params) {
    if (capability !== "translate" && capability !== "detect") {
        return { error: "Google Translate: неподдерживаемая возможность " + capability };
    }

    var text = params.text || "";
    if (!text) {
        return { error: "Пустой текст" };
    }

    var from = params.from || "auto";
    var to = params.to || "ru";
    if (capability === "detect") {
        from = "auto";
        to = "en";
    }

    var endpoint = settings.endpoint || "https://translate.googleapis.com/translate_a/single";
    var url = endpoint +
        "?client=gtx&dt=t&sl=" + encodeURIComponent(from) +
        "&tl=" + encodeURIComponent(to) +
        "&q=" + encodeURIComponent(text);

    var response = http.get(url, {
        "Accept": "application/json,text/plain,*/*",
        "Accept-Language": "en-US,en;q=0.9,ru;q=0.8"
    });

    if (response.error) {
        return { error: response.error };
    }
    if (response.status !== 200) {
        return { error: "Google Translate: HTTP " + response.status };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Google Translate: некорректный ответ сервера" };
    }

    if (!data || !data[0]) {
        return { error: "Google Translate: пустой ответ сервера" };
    }

    var translated = "";
    for (var index = 0; index < data[0].length; ++index) {
        if (data[0][index] && data[0][index][0]) {
            translated += data[0][index][0];
        }
    }

    if (!translated) {
        return { error: "Google Translate: перевод не получен" };
    }

    return {
        text: translated,
        detected_lang: data[2] || params.detected_from || from
    };
}
