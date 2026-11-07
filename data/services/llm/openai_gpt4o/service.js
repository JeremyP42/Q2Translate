function escapeHtml(value) {
    return String(value)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/\n/g, "<br>");
}

function promptFor(capability, params, prompts) {
    var source = params.source_language_name || params.from_name || params.from || "auto";
    var target = params.to_name || params.to || "ru";
    var text = params.text || params.word || "";

    if (capability === "translate") {
        return (prompts.translate || "Translate from {source} to {target}:\n{text}")
            .replace("{source}", source)
            .replace("{target}", target)
            .replace("{text}", text) + (params.context ? "\n\nScene context (do not translate; keep names and terminology consistent):\n" + String(params.context).trim() : "") + (params.instruction ? "\n\n" + String(params.instruction).trim() : "");
    }
    if (capability === "summarize") {
        return (prompts.summarize || "Summarize:\n{text}").replace("{text}", text);
    }
    if (capability === "paraphrase") {
        return (prompts.paraphrase || "Rewrite:\n{text}").replace("{text}", text);
    }
    if (capability === "dictionary") {
        return (prompts.dictionary || "Explain \"{text}\" in {target}.")
            .replace("{text}", text)
            .replace("{target}", target);
    }
    return (prompts.ocr_vision || "Extract all text from this image.");
}

function systemPromptFor(params) {
    var source = params.source_language_name || params.from_name || params.from || "auto";
    var target = params.to_name || params.to || "ru";
    var text = params.text || params.word || "";
    var prompt = String(settings.system_prompt || "").trim();
    if (!prompt) {
        prompt = "You are a precise assistant. Follow the user instruction exactly.";
    }
    return prompt
        .replace(/\{src\}/g, source)
        .replace(/\{dst\}/g, target)
        .replace(/\{text\}/g, text);
}

function normalizeEndpoint(value) {
    var endpoint = String(value || "https://api.openai.com/v1/chat/completions").trim().replace(/\/+$/, "");
    if (/\/chat\/completions$/.test(endpoint)) {
        return endpoint;
    }
    if (/\/v1$/.test(endpoint)) {
        return endpoint + "/chat/completions";
    }
    return endpoint + "/v1/chat/completions";
}

function execute(capability, params) {
    if (!settings.api_key) {
        return { error: "OpenAI: укажите API Key в настройках сервиса" };
    }

    var prompts = loadJSON("prompts.json");
    var endpoint = normalizeEndpoint(settings.endpoint);
    var model = settings.model || "gpt-4o-mini";
    var temperature = Number(settings.temperature);
    if (isNaN(temperature)) {
        temperature = 0.2;
    }

    var prompt = promptFor(capability, params, prompts);
    var userContent = prompt;

    if (capability === "ocr_vision") {
        if (!params.image_base64) {
            return { error: "OpenAI: не передано изображение" };
        }
        userContent = [
            { type: "text", text: prompt },
            {
                type: "image_url",
                image_url: { url: "data:image/png;base64," + params.image_base64 }
            }
        ];
    }

    var body = {
        model: model,
        temperature: temperature,
        messages: [
            { role: "system", content: systemPromptFor(params) },
            { role: "user", content: userContent }
        ]
    };

    var response = http.post(endpoint, body, {
        "Authorization": "Bearer " + settings.api_key,
        "Content-Type": "application/json"
    });
    if (response.error) {
        return { error: response.error };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "OpenAI: некорректный ответ сервера" };
    }

    if (response.status !== 200 || !data.choices || !data.choices.length) {
        var message = data && data.error && data.error.message;
        return { error: message || ("OpenAI: HTTP " + response.status) };
    }

    var text = data.choices[0].message.content || "";
    if (capability === "dictionary") {
        return { html: "<p>" + escapeHtml(text) + "</p>", text: text };
    }
    return { text: text };
}
