function execute(capability, params) {
    if (capability !== "translate") {
        return { error: "MyMemory: неподдерживаемая возможность " + capability };
    }

    var text = params.text || "";
    if (!text) {
        return { error: "Пустой текст" };
    }

    var from = params.detected_from || params.from || "en";
    if (!from || from === "auto") {
        from = "en";
    }
    var to = params.to || "ru";
    if (!to || to === "auto") {
        to = "ru";
    }

    var endpoint = settings.endpoint || "https://api.mymemory.translated.net/get";
    var url = endpoint +
        "?q=" + encodeURIComponent(text) +
        "&langpair=" + encodeURIComponent(from + "|" + to);
    if (settings.email) {
        url += "&de=" + encodeURIComponent(settings.email);
    }

    var response = http.get(url, { "Accept": "application/json" });
    if (response.error) {
        return { error: response.error };
    }
    if (response.status !== 200) {
        return { error: "MyMemory: HTTP " + response.status };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "MyMemory: некорректный ответ сервера" };
    }

    if (!data || !data.responseData || !data.responseData.translatedText) {
        return { error: (data && data.responseDetails) || "MyMemory: перевод не получен" };
    }
    if (data.responseStatus && Number(data.responseStatus) !== 200) {
        return { error: data.responseDetails || ("MyMemory: " + data.responseStatus) };
    }

    return {
        text: data.responseData.translatedText,
        detected_lang: from
    };
}
