function escapeHtml(value) {
    return String(value)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/\n/g, "<br>");
}

function wikiCode(value) {
    var code = String(value || "").trim().toLowerCase();
    if (!code || code === "auto") {
        return "";
    }
    return code.split("-")[0].split("_")[0];
}

function execute(capability, params) {
    if (capability !== "dictionary") {
        return { error: "Wikipedia: поддерживается только словарный поиск." };
    }

    var word = String(params.word || params.text || "").trim();
    if (!word) {
        return { error: "Wikipedia: не передано слово для поиска." };
    }

    var language = wikiCode(params.from);
    if (!language) {
        language = wikiCode(params.to);
    }
    if (!language) {
        language = "en";
    }

    var url = "https://" + language + ".wikipedia.org/w/api.php"
        + "?action=query&prop=extracts&explaintext=1&redirects=1&format=json&titles="
        + encodeURIComponent(word);
    var response = http.get(url, { "User-Agent": "Q2Translate/1.0" });
    if (response.error) {
        return { error: "Wikipedia: " + response.error };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Wikipedia: некорректный ответ сервера." };
    }

    var pages = data && data.query && data.query.pages ? data.query.pages : null;
    if (!pages) {
        return { error: "Wikipedia: статья не найдена." };
    }

    var extract = "";
    for (var key in pages) {
        if (pages[key] && pages[key].extract) {
            extract = pages[key].extract;
            break;
        }
    }
    if (!extract) {
        return { error: "Wikipedia: статья «" + word + "» не найдена." };
    }

    return { html: "<p>" + escapeHtml(extract) + "</p>", text: extract };
}
