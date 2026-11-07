function providerCode(value) {
    var code = String(value || "").toLowerCase();
    if (code === "auto" || !code) return "auto-detect";
    if (code === "zh-cn") return "zh-Hans";
    if (code === "zh-tw") return "zh-Hant";
    if (code === "pt-br") return "pt";
    if (code === "sr") return "sr-Cyrl";
    if (code === "no") return "nb";
    return code;
}

function auth() {
    var page = http.get("https://www.bing.com/translator", {
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "en-US,en;q=0.9",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36"
    });
    if (page.error || page.status !== 200)
        return { error: "Bing: не удалось открыть страницу переводчика" };

    var ig = page.body.match(/IG:"([^"]+)"/);
    var iid = page.body.match(/data-iid="([^"]+)"/);
    var helper = page.body.match(/params_AbusePreventionHelper\s*=\s*(\[[\s\S]*?\]);/);
    if (!ig || !iid || !helper)
        return { error: "Bing: не найдены параметры авторизации" };

    var values;
    try { values = json.parse(helper[1]); }
    catch (error) { return { error: "Bing: некорректные параметры авторизации" }; }
    var muid = page.body.match(/muid":\s*"([^"]+)"/);
    return {
        ig: ig[1],
        iid: iid[1],
        key: values[0],
        token: values[1],
        muid: muid ? muid[1] : ""
    };
}

function cachedAuth() {
    var cached = qtranslate.cacheGet("bing_auth");
    if (cached && Number(cached.expires) > new Date().getTime()
        && cached.ig && cached.iid && cached.key && cached.token)
        return cached;

    var fresh = auth();
    if (!fresh.error) {
        fresh.expires = new Date().getTime() + 10 * 60 * 1000;
        qtranslate.cacheSet("bing_auth", fresh);
    }
    return fresh;
}

function execute(capability, params) {
    if (capability !== "translate" && capability !== "detect")
        return { error: "Bing: неподдерживаемая операция " + capability };
    var text = String(params.text || "");
    if (!text)
        return { error: "Пустой текст" };
    var from = capability === "detect" ? "auto-detect" : providerCode(params.from);
    var to = capability === "detect" ? "en" : providerCode(params.to);

    var credentials = cachedAuth();
    if (credentials.error)
        return credentials;

    var body = "text=" + encodeURIComponent(text)
        + "&fromLang=" + encodeURIComponent(from)
        + "&to=" + encodeURIComponent(to)
        + "&token=" + encodeURIComponent(credentials.token)
        + "&key=" + encodeURIComponent(credentials.key)
        + "&isAuthv2=true&tryFetchingGenderDebiasedTranslations=true";
    var headers = {
        "Accept": "application/json",
        "Content-Type": "application/x-www-form-urlencoded",
        "Origin": "https://www.bing.com",
        "Referer": "https://www.bing.com/translator",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36"
    };
    if (credentials.muid)
        headers.Cookie = "MUID=" + credentials.muid;

    var url = "https://www.bing.com/ttranslatev3?isVertical=1&IG="
        + encodeURIComponent(credentials.ig) + "&IID=" + encodeURIComponent(credentials.iid);
    var response = http.post(url, body, headers);
    if (response.error)
        return { error: "Bing: " + response.error };
    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "Bing: некорректный JSON" }; }
    if (response.status !== 200 || !data || !data.length)
        return { error: "Bing: HTTP " + response.status };
    var item = data[0];
    if (!item || !item.translations || !item.translations.length)
        return { error: "Bing: пустой перевод" };
    var translated = "";
    for (var i = 0; i < item.translations.length; ++i)
        translated += item.translations[i].text || "";
    return {
        text: translated,
        detected_lang: item.detectedLanguage ? item.detectedLanguage.language : undefined
    };
}
