function unescapeHtml(value) {
    return String(value || "")
        .replace(/&lt;/g, "<")
        .replace(/&gt;/g, ">")
        .replace(/&quot;/g, '"')
        .replace(/&#39;/g, "'")
        .replace(/&amp;/g, "&");
}

function execute(capability, params) {
    if (capability !== "dictionary")
        return { error: "ImTranslator: поддерживаются только словарные статьи" };
    var word = String(params.word || params.text || "").trim();
    if (!word)
        return { error: "Пустое слово" };
    var from = params.from || "en";
    var to = params.to || "ru";
    if (from === "auto")
        from = "en";
    var pair = encodeURIComponent(from + "/" + to);
    var url = "http://imtranslator.net/translation/dictionary/DicService.asmx/lookup?text="
        + encodeURIComponent(word) + "&dicID=&lang=" + pair + "&langs=" + pair + "&flags=DEFAULT";
    var response = http.get(url, {
        "Accept": "text/xml,application/xml,text/html",
        "Referer": "http://dictionary.imtranslator.net/"
    });
    if (response.error)
        return { error: "ImTranslator: " + response.error };
    if (response.status !== 200)
        return { error: "ImTranslator: HTTP " + response.status };
    var match = response.body.match(/<translation[^>]*status="ok"[^>]*>([\s\S]*?)<\/translation>/i);
    if (!match)
        return { error: "ImTranslator: статья не найдена" };
    var html = unescapeHtml(match[1]);
    if (!html)
        return { error: "ImTranslator: пустая статья" };
    return { html: html, text: word, title: "ImTranslator — " + word, url: "http://dictionary.imtranslator.net/" };
}
